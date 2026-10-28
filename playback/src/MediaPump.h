#pragma once

// The media pump: decodes video and audio ahead of the playhead.
//
// One decode worker thread per active media layer. A worker owns a source
// instance of its layer's media (opened through the library's source
// factory) and is the only consumer of it (sources are single-consumer).
// The render thread publishes, every frame, the target media time of each
// active layer (a snapshot built under the app's state lock); each worker
// decodes toward the targets of the layers it serves:
//
//   - video: up to kVideoLookaheadFrames frames per layer, queued in the
//     channel; the render thread picks the newest frame at or before the
//     target media time and uploads it (via the SceneRenderer's thread-safe
//     upload queue).
//   - audio: up to kAudioLookaheadSeconds of decoded samples per layer.
//
// Seeking: the render thread bumps a generation counter whenever the
// playhead jumps (seek command or SeekTo cue); a worker re-seeks when its
// target's generation is stale. A backward target jump (loop wrap or a
// seek the generation counter did not catch) re-seeks as well - except on
// a loop wrap that the worker pre-decoded (see below).
//
// Seamless looping: a worker for a looping layer pre-decodes the start of
// the loop before the playhead reaches the end (it seeks the source to
// the loop start and decodes kPreLoopFrames frames into the tail of the
// queue while the end-of-media frames are still being shown). At the wrap
// the queue already holds the first frames of the next pass, so no seek
// stalls the display.
//
// A lightweight coordinator (the original pump thread) manages the
// worker lifecycle: it spawns a worker when a media layer appears and
// retires it when the layer leaves the published state.
//
// Audio mixing (the miniaudio device callback) consumes from the channels'
// sample buffers under the channel mutex, resampling to the sink rate with
// linear interpolation.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "mediamax/media/MediaLibrary.h"
#include "mediamax/playback/FrameDropCounter.h"
#include "mediamax/render/SceneRenderer.h"
#include "mediamax/time/FrameRate.h"
#include "mediamax/time/Time.h"

namespace mediamax
{

// Per-layer, per-frame decode target. Built by the render thread from the
// FrameState.
struct LayerTarget
{
    Id layerId = kInvalidId;
    Id mediaId = kInvalidId; // kInvalidId = color layer (no media)
    Time mediaTime; // target position in the media's timeline
    bool loop = false;
    Time mediaDuration; // media duration (wrap boundary when looping)
    // Loop bounds (valid when loop): the playhead wraps within
    // [loopStart, loopEnd). The pump uses them for the seamless-loop
    // pre-decode.
    Time loopStart;
    Time loopEnd;
    Size2i mediaSize{0, 0}; // media display size in px (texture size)
    // The layer's on-screen size in px (its transform scale applied to the
    // media size); {0,0} when unknown. The decode worker hints its source
    // with it, so frames are decoded at min(native, on-screen) - the
    // display never shows more pixels than the on-screen size.
    Size2i onScreenSize{0, 0};
    float gain = 1.f; // layer opacity (audio gain)

    // Audio properties (zero when the media has no audio).
    bool hasAudio = false;
    std::uint32_t audioChannels = 0;
    double audioSampleRate = 0.0;
};

class MediaPump
{
public:
    // `libraryMutex` must be the same mutex the app uses around MediaLibrary
    // mutations (media load/unload); the pump takes it briefly for lookups.
    MediaPump(MediaLibrary& media, std::mutex& libraryMutex, render::SceneRenderer& renderer);

    MediaPump(const MediaPump&) = delete;
    MediaPump& operator=(const MediaPump&) = delete;

    // Starts the coordinator (which spawns decode workers on demand).
    // Idempotent.
    void start();
    // Stops the coordinator and all workers (joins). Safe to call twice.
    void stop();

    // Called every frame by the render thread. `generation` must change
    // when the playhead jumped (workers re-seek stale channels).
    void publishTargets(const Time& showTime, const std::vector<LayerTarget>& targets,
                        std::uint64_t generation);

    // The latest published targets (for the audio callback).
    std::vector<LayerTarget> snapshotTargets() const;

    // Render thread: picks the frame for the layer and queues the texture
    // upload if it changed. Returns the layer's texture (may be null before
    // the first frame is decoded). Also advances the layer's
    // dropped-frames metric.
    nvrhi::TextureHandle deliverVideo(const LayerTarget& target);

    // Audio callback thread: mixes `frameCount` stereo frames into
    // outStereo (interleaved L,R; zeroed first) from the given targets.
    // The i-th output sample corresponds to target mediaTime + i / 48000
    // seconds (the sink rate; audio advances in real time).
    void mixAudio(const std::vector<LayerTarget>& targets, float* outStereo, int frameCount);

    // Total video frames dropped so far, across all layers (high water).
    std::uint64_t droppedVideoFrames() const;

private:
    static constexpr int kVideoLookaheadFrames = 8;
    static constexpr double kAudioLookaheadSeconds = 0.5;
    static constexpr std::uint32_t kDecodeTimeoutMs = 25;
    // Staging calls (pre-decoding the loop start) wait longer: the first
    // keyframe decode after a seek regularly takes 25-35 ms.
    static constexpr std::uint32_t kStagingTimeoutMs = 50;
    // Seamless looping: how many frames of the loop start a worker
    // pre-decodes before the playhead reaches the end, and how early
    // (in frames plus a margin) the pre-decode starts.
    static constexpr int kPreLoopFrames = 8;
    static constexpr int kPreLoopMarginFrames = 4;
    // Health summary period, and the threshold that makes a video decode
    // call "slow" in the health summary.
    static constexpr std::chrono::seconds kHealthWindow{5};
    static constexpr std::int64_t kSlowDecodeNanos = 10'000'000;

    struct Channel
    {
        Id mediaId = kInvalidId;
        std::uint64_t generation = 0; // last generation this channel decoded for
        bool primed = false; // has seeked for the current generation

        // Last target published for this layer (backward-jump detection for
        // loop wraps and seeks that the generation counter does not catch).
        std::optional<LayerTarget> lastTarget;

        // Media frame rate (set by the worker once its source is open);
        // used by the dropped-frames metric.
        FrameRate mediaFrameRate;

        std::mutex mutex; // guards the decoded data below
        std::deque<VideoFrame> video;
        // Loop pre-decode staging: the loop-start frames decoded before the
        // playhead reaches the end live here (not in `video`) so a delivery
        // before the wrap can never select them (they sit far behind the
        // target). Merged into `video` at the wrap (see decodeChannel).
        std::deque<VideoFrame> preLoopVideo;
        Time lastDeliveredTime; // media time of the last frame handed to the GPU
        bool hasDelivered = false;

        // Dropped-frames metric (render thread advances it in deliverVideo).
        FrameDropCounter dropCounter;

        // Decoded audio (source rate, interleaved).
        std::vector<float> audio;
        Time audioBase; // media time of audio[0]
        Time audioDecodedUpTo;

        // --- Health telemetry (diagnoses decode/delivery throughput) ----
        // Windowed counters, touched only under `mutex`; the worker prints
        // and resets them every kHealthWindow (debug level). The worker-side
        // fields are written by the decode passes, the render-side ones by
        // deliverVideo().
        std::int64_t passCount = 0; // decode passes completed
        std::int64_t passNanos = 0; // total wall time of passes
        std::int64_t passMaxNanos = 0;
        std::int64_t lockWaitNanos = 0; // time waiting to acquire the mutex
        std::int64_t decodeCalls = 0; // nextVideoFrame calls
        std::int64_t decodeNanos = 0;
        std::int64_t slowDecodes = 0; // calls taking more than kSlowDecodeNanos
        std::int64_t minQueueDepth = 0; // queue depth at pass start (window)
        std::int64_t maxQueueDepth = 0;
        bool haveQueueSample = false;
        std::int64_t deliverCount = 0; // render-thread deliveries (window)
        std::int64_t deliverLockNanos = 0; // render-thread mutex wait
        std::int64_t uploadNanos = 0; // time for ensure+update of new frames
        std::int64_t uploadMaxNanos = 0;
        std::int64_t ensureNanos = 0; // part of uploadNanos spent in ensureMediaTexture
        double maxTailLag = 0.0; // worst target-minus-queue-tail, in frames
        std::int64_t currentGaps = 0; // frame gaps currently in the video queue
        double maxBacklog = 0.0; // worst display lag behind target (frames, window)
        std::chrono::steady_clock::time_point lastSummary{};
    };

    // One decode thread per active media layer. The worker owns its source
    // instance (single-consumer per source).
    struct Worker
    {
        Id layerId = kInvalidId;
        Id mediaId = kInvalidId;
        std::shared_ptr<IMediaSource> source;
        std::thread thread;
        std::atomic<bool> stopping{false};
        bool preLoopDone = false; // pre-decode fired for the current pass
        // Decoding (video AND audio) is suspended between the loop
        // pre-decode and the wrap: the source is parked at the loop start,
        // the tail is fully drained, and the start frames are staged in
        // preLoopVideo. Decoding more would push next-pass frames into the
        // live queue (they sit far behind the pre-wrap target and hijack the
        // frame selection).
        bool pumpSuspended = false;
    };

    void pumpLoop();
    // Finds or creates the channel for the target (rebinding and resetting
    // when the layer's media changed, banking the old drop count). The
    // caller must hold m_ChannelsMutex.
    std::shared_ptr<Channel> channelForLocked(const LayerTarget& target);
    // The channel for a target without creating one (nullptr when absent or
    // bound to a different media).
    std::shared_ptr<Channel> findChannel(Id layerId, Id mediaId) const;
    // Worker body: opens the source, then decodes toward its layer's target.
    void workerLoop(Worker& worker);
    // One decode pass for the worker's channel.
    void decodeChannel(Worker& worker, Channel& ch, const LayerTarget& target,
                       std::uint64_t generation);
    // Seamless looping: decode the tail of the pass (remaining video frames
    // up to EOF + remaining audio up to the loop end), seek the source to
    // the loop start, and pre-decode kPreLoopFrames start frames - all
    // WITHOUT holding ch.mutex (the source is worker-private; the render
    // thread keeps delivering the tail frames meanwhile). The caller
    // applies the result to the channel under the lock.
    struct PredecodeResult
    {
        bool seeked = false;
        std::vector<VideoFrame> tailVideo; // remaining end-of-pass frames
        std::vector<AudioFrame> tailAudio; // remaining audio up to the loop end
        std::vector<VideoFrame> startVideo; // staged loop-start frames
        bool haveAudioBase = false;
        Time audioBase;
        bool haveAudioUpTo = false;
        Time audioUpTo;
    };
    PredecodeResult preDecodeUnlocked(IMediaSource& source, const LayerTarget& target,
                                      const MediaMetadata& md, const Time& audioUpTo);
    // True when the video queue already holds frames at the loop start
    // (pre-decoded), so a loop wrap needs no re-seek.
    static bool loopStartInQueue(const Channel& ch, const LayerTarget& target);
    // Prints the channel's windowed health summary and resets the counters.
    // The caller must hold ch.mutex.
    void printHealth(Channel& ch, Id layerId);

    MediaLibrary& m_Media;
    std::mutex& m_LibraryMutex;
    render::SceneRenderer& m_Renderer;

    std::thread m_Thread;
    std::atomic<bool> m_Running{false};

    // Published targets.
    mutable std::mutex m_TargetsMutex;
    std::vector<LayerTarget> m_Targets;
    std::uint64_t m_Generation = 0;

    mutable std::mutex m_ChannelsMutex; // guards m_Channels, m_Workers, m_DroppedRetired
    std::map<Id, std::shared_ptr<Channel>> m_Channels; // keyed by layer id
    std::map<Id, std::shared_ptr<Worker>> m_Workers; // keyed by layer id
    std::uint64_t m_DroppedRetired = 0; // drop counts of retired channels
};

} // namespace mediamax
