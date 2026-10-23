#pragma once

// The media pump: decodes video and audio ahead of the playhead.
//
// One pump thread owns all IMediaSource consumption (sources are
// single-consumer). The render thread publishes, every frame, the target
// media time of each active layer (a snapshot built under the app's state
// lock); the pump decodes toward those targets:
//
//   - video: up to kVideoLookaheadFrames frames per layer, queued in the
//     channel; the render thread picks the newest frame at or before the
//     target media time and uploads it (via the SceneRenderer's thread-safe
//     upload queue).
//   - audio: up to kAudioLookahead seconds of decoded samples per layer.
//
// Seeking: the render thread bumps a generation counter whenever the
// playhead jumps (seek command or SeekTo cue); the pump re-seeks every
// channel whose generation is stale.
//
// Audio mixing (the miniaudio device callback) consumes from the channels'
// sample buffers under the channel mutex, resampling to the sink rate with
// linear interpolation.

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "mediamax/media/MediaLibrary.h"
#include "mediamax/render/SceneRenderer.h"
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
    Size2i mediaSize{0, 0}; // media display size in px (texture size)
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

    // Starts the pump thread. Idempotent.
    void start();
    // Stops the thread (joins). Safe to call twice.
    void stop();

    // Called every frame by the render thread. `generation` must change
    // when the playhead jumped (the pump re-seeks stale channels).
    void publishTargets(const Time& showTime, const std::vector<LayerTarget>& targets,
                        std::uint64_t generation);

    // The latest published targets (for the audio callback).
    std::vector<LayerTarget> snapshotTargets() const;

    // Render thread: picks the frame for the layer and queues the texture
    // upload if it changed. Returns the layer's texture (may be null before
    // the first frame is decoded).
    nvrhi::TextureHandle deliverVideo(const LayerTarget& target);

    // Audio callback thread: mixes `frameCount` stereo frames into
    // outStereo (interleaved L,R; zeroed first) from the given targets.
    // The i-th output sample corresponds to target mediaTime + i / 48000
    // seconds (the sink rate; audio advances in real time).
    void mixAudio(const std::vector<LayerTarget>& targets, float* outStereo, int frameCount);

private:
    static constexpr int kVideoLookaheadFrames = 8;
    static constexpr double kAudioLookaheadSeconds = 0.5;
    static constexpr std::uint32_t kDecodeTimeoutMs = 25;

    struct Channel
    {
        Id mediaId = kInvalidId;
        std::shared_ptr<IMediaSource> source;
        std::uint64_t generation = 0; // last generation this channel decoded for
        bool primed = false; // has seeked for the current generation

        // Last target published for this layer (backward-jump detection for
        // loop wraps and seeks that the generation counter does not catch).
        std::optional<LayerTarget> lastTarget;

        std::mutex mutex; // guards the decoded data below
        std::deque<VideoFrame> video;
        Time lastDeliveredTime; // media time of the last frame handed to the GPU
        bool hasDelivered = false;

        // Decoded audio (source rate, interleaved).
        std::vector<float> audio;
        Time audioBase; // media time of audio[0]
        Time audioDecodedUpTo;
    };

    void pumpLoop();
    // Finds or creates the channel for the target (rebinding when the layer's
    // media changed). nullptr when the media is not in the library.
    std::shared_ptr<Channel> channelFor(const LayerTarget& target);
    void decodeChannel(Channel& ch, const LayerTarget& target, std::uint64_t generation);

    MediaLibrary& m_Media;
    std::mutex& m_LibraryMutex;
    render::SceneRenderer& m_Renderer;

    std::thread m_Thread;
    std::atomic<bool> m_Running{false};

    // Published targets.
    mutable std::mutex m_TargetsMutex;
    Time m_ShowTime;
    std::vector<LayerTarget> m_Targets;
    std::uint64_t m_Generation = 0;

    std::mutex m_ChannelsMutex;
    std::map<Id, std::shared_ptr<Channel>> m_Channels; // keyed by layer id
};

} // namespace mediamax
