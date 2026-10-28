#include "MediaPump.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "mediamax/util/Logger.h"

namespace mediamax
{

namespace
{

// The audio sink rate (AudioSink runs a fixed 48 kHz stereo device).
constexpr double kSinkSampleRate = 48000.0;

// How far behind the target we keep decoded audio (trim horizon).
constexpr double kAudioTrimHistorySeconds = 0.25;

// Safety caps per decode pass (a stalled decoder must not spin the thread).
constexpr int kMaxVideoDecodesPerPass = 32;
constexpr int kMaxAudioDecodesPerPass = 128;

// A channel re-seeks when its target media time jumps backwards by more than
// this (loop wrap or a seek the generation counter did not catch).
constexpr double kBackwardJumpSeconds = 0.25;

// Idle sleep slice for the coordinator and worker loops (keeps stop()
// responsive).
constexpr std::chrono::milliseconds kSleepSlice{2};

std::int64_t nanosSince(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - from)
        .count();
}

} // namespace

MediaPump::MediaPump(MediaLibrary& media, std::mutex& libraryMutex, render::SceneRenderer& renderer)
    : m_Media(media)
    , m_LibraryMutex(libraryMutex)
    , m_Renderer(renderer)
{
}

void MediaPump::start()
{
    if (!m_Running.exchange(true))
    {
        m_Thread = std::thread(&MediaPump::pumpLoop, this);
    }
}

void MediaPump::stop()
{
    if (m_Running.exchange(false))
    {
        // The coordinator joins the decode workers on its way out (see the
        // tail of pumpLoop).
        if (m_Thread.joinable())
        {
            m_Thread.join();
        }
    }
}

void MediaPump::publishTargets(const Time& showTime, const std::vector<LayerTarget>& targets,
                               std::uint64_t generation)
{
    (void)showTime; // documented for the contract; the workers read targets only
    std::lock_guard lock(m_TargetsMutex);
    m_Targets = targets;
    m_Generation = generation;
}

std::vector<LayerTarget> MediaPump::snapshotTargets() const
{
    std::lock_guard lock(m_TargetsMutex);
    return m_Targets;
}

void MediaPump::pumpLoop()
{
    while (m_Running.load())
    {
        // Sleep in small slices so stop() stays responsive.
        for (int i = 0; i < 5 && m_Running.load(); ++i)
        {
            std::this_thread::sleep_for(kSleepSlice);
        }
        if (!m_Running.load())
        {
            break;
        }

        std::vector<LayerTarget> targets;
        {
            std::lock_guard lock(m_TargetsMutex);
            targets = m_Targets;
        }

        // --- Worker lifecycle ------------------------------------------------
        // Spawn a worker for every media layer, retire the workers whose
        // layer left the published state (or whose media changed), and join
        // the retired ones.
        std::vector<std::shared_ptr<Worker>> toRetire;
        {
            std::lock_guard lock(m_ChannelsMutex);
            for (const auto& target : targets)
            {
                if (target.mediaId == kInvalidId)
                {
                    continue; // color layer
                }
                channelForLocked(target);
                auto wit = m_Workers.find(target.layerId);
                if (wit == m_Workers.end())
                {
                    auto worker = std::make_shared<Worker>();
                    worker->layerId = target.layerId;
                    worker->mediaId = target.mediaId;
                    worker->thread = std::thread(&MediaPump::workerLoop, this, std::ref(*worker));
                    m_Workers.emplace(target.layerId, worker);
                }
                else if (wit->second->mediaId != target.mediaId)
                {
                    // The layer's media changed: the channel is rebound by
                    // channelFor(); replace the worker so it opens the new
                    // source.
                    wit->second->stopping.store(true);
                    toRetire.push_back(wit->second);
                }
            }
            for (auto& [layerId, worker] : m_Workers)
            {
                bool active = false;
                for (const auto& target : targets)
                {
                    if (target.layerId == layerId && target.mediaId != kInvalidId)
                    {
                        active = true;
                        break;
                    }
                }
                if (!active)
                {
                    worker->stopping.store(true);
                    toRetire.push_back(worker);
                }
            }
        }

        // Join outside the channels lock (a worker still running takes it).
        for (auto& worker : toRetire)
        {
            if (worker->thread.joinable())
            {
                worker->thread.join();
            }
        }
        {
            std::lock_guard lock(m_ChannelsMutex);
            for (auto& worker : toRetire)
            {
                auto cit = m_Channels.find(worker->layerId);
                if (cit != m_Channels.end())
                {
                    std::lock_guard chLock(cit->second->mutex);
                    m_DroppedRetired += cit->second->dropCounter.total();
                    m_Channels.erase(cit);
                }
                m_Workers.erase(worker->layerId);
            }
        }
    }

    // Shutdown: retire every remaining worker and channel. (The channel
    // objects stay alive for any worker still holding a shared_ptr; the
    // pump's own references drop here - drop accounting is session-scope,
    // so no banking is needed on the way out.)
    std::vector<std::shared_ptr<Worker>> remaining;
    {
        std::lock_guard lock(m_ChannelsMutex);
        for (auto& [layerId, worker] : m_Workers)
        {
            worker->stopping.store(true);
            remaining.push_back(worker);
        }
        m_Workers.clear();
        m_Channels.clear();
    }
    // Join outside the channels lock (a worker still running takes it).
    for (auto& worker : remaining)
    {
        if (worker->thread.joinable())
        {
            worker->thread.join();
        }
    }
}

std::shared_ptr<MediaPump::Channel> MediaPump::channelForLocked(const LayerTarget& target)
{
    // The caller holds m_ChannelsMutex.
    std::shared_ptr<Channel> channel;
    auto it = m_Channels.find(target.layerId);
    if (it != m_Channels.end())
    {
        channel = it->second;
        if (channel->mediaId != target.mediaId)
        {
            // The layer's media changed: bank the old drop count, rebind
            // and reset all state (the new worker re-primes everything).
            std::lock_guard chLock(channel->mutex);
            m_DroppedRetired += channel->dropCounter.total();
            channel->mediaId = target.mediaId;
            channel->dropCounter.reset();
            channel->generation = 0;
            channel->primed = false;
            channel->mediaFrameRate = FrameRate();
            channel->video.clear();
            channel->preLoopVideo.clear();
            channel->audio.clear();
            channel->audioBase = Time::zero();
            channel->audioDecodedUpTo = Time::zero();
            channel->lastDeliveredTime = Time::zero();
            channel->hasDelivered = false;
            channel->lastTarget.reset();
        }
    }
    else
    {
        channel = std::make_shared<Channel>();
        channel->mediaId = target.mediaId;
        m_Channels.emplace(target.layerId, channel);
    }
    return channel;
}

std::shared_ptr<MediaPump::Channel> MediaPump::findChannel(Id layerId, Id mediaId) const
{
    std::lock_guard lock(m_ChannelsMutex);
    auto it = m_Channels.find(layerId);
    if (it != m_Channels.end() && it->second->mediaId == mediaId)
    {
        return it->second;
    }
    return nullptr;
}

void MediaPump::workerLoop(Worker& worker)
{
    // Open our own source instance. A source is single-consumer and this
    // worker is the only thread that will ever touch it; the library's own
    // instance stays with the library (probing/metadata).
    std::string path;
    MediaSourceFactory factory;
    {
        std::lock_guard lock(m_LibraryMutex);
        if (const auto asset = m_Media.find(worker.mediaId))
        {
            path = (*asset)->path;
        }
        factory = m_Media.sourceFactory();
    }
    if (path.empty())
    {
        Logger::instance().debug("media pump: worker for layer {}: media {} not in the library",
                                 worker.layerId, worker.mediaId);
        return;
    }
    std::string error;
    std::shared_ptr<IMediaSource> source = factory ? factory(path, error) : nullptr;
    if (!source)
    {
        Logger::instance().warning("media pump: worker for layer {}: cannot open {}: {}",
                                   worker.layerId, path, error);
        return;
    }
    worker.source = source;

    while (!worker.stopping.load() && m_Running.load())
    {
        std::this_thread::sleep_for(kSleepSlice);

        LayerTarget target;
        std::uint64_t generation = 0;
        bool haveTarget = false;
        {
            std::lock_guard lock(m_TargetsMutex);
            generation = m_Generation;
            for (const auto& t : m_Targets)
            {
                if (t.layerId == worker.layerId)
                {
                    target = t;
                    haveTarget = true;
                    break;
                }
            }
        }
        if (!haveTarget || target.mediaId != worker.mediaId)
        {
            continue; // no target for this layer yet (or the media changed)
        }

        std::shared_ptr<Channel> channel = findChannel(worker.layerId, worker.mediaId);
        if (channel == nullptr)
        {
            continue; // channel not (re)created yet, or rebound to new media
        }
        decodeChannel(worker, *channel, target, generation);
    }
}

void MediaPump::decodeChannel(Worker& worker, Channel& ch, const LayerTarget& target,
                              std::uint64_t generation)
{
    std::shared_ptr<IMediaSource> source = worker.source;
    if (!source)
    {
        return;
    }

    const auto passStart = std::chrono::steady_clock::now();
    std::unique_lock lock(ch.mutex);
    ch.lockWaitNanos += nanosSince(passStart);
    // Queue depth at pass start (how much lookahead the render thread had).
    const std::int64_t depth = static_cast<std::int64_t>(ch.video.size());
    if (!ch.haveQueueSample)
    {
        ch.minQueueDepth = ch.maxQueueDepth = depth;
        ch.haveQueueSample = true;
    }
    else
    {
        ch.minQueueDepth = std::min(ch.minQueueDepth, depth);
        ch.maxQueueDepth = std::max(ch.maxQueueDepth, depth);
    }

    // One windowed health update per pass (and a summary line every
    // kHealthWindowSeconds); see printHealth.
    const auto finishPass = [this, &ch, &passStart, layerId = target.layerId]() {
        ch.passCount++;
        const std::int64_t pn = nanosSince(passStart);
        ch.passNanos += pn;
        ch.passMaxNanos = std::max(ch.passMaxNanos, pn);
        ch.maxBacklog = std::max(ch.maxBacklog, static_cast<double>(ch.dropCounter.backlog()));
        if (ch.lastSummary != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() - ch.lastSummary >= kHealthWindow)
        {
            printHealth(ch, layerId);
            ch.lastSummary = std::chrono::steady_clock::now();
        }
    };

    const MediaMetadata md = source->metadata();
    ch.mediaFrameRate = md.frameRate;
    // Decode at the on-screen size (see IMediaSource::setVideoOutputSizeHint).
    // Idempotent per pass: the source no-ops while the hint matches its
    // current output size.
    if (md.hasVideo && target.onScreenSize.valid())
    {
        source->setVideoOutputSizeHint(target.onScreenSize);
    }

    // Re-seek when the generation changed, when the target jumped backwards
    // (loop wrap or a seek the generation counter did not catch), or on
    // first use.
    const bool generationStale = ch.generation != generation;
    const bool backwardJump = ch.lastTarget.has_value() &&
                              target.mediaTime <
                                  ch.lastTarget->mediaTime - Time::fromSeconds(kBackwardJumpSeconds);

    if (generationStale || backwardJump || !ch.primed)
    {
        const bool wrapWithCache =
            backwardJump && !generationStale && target.loop && target.loopStart.isValid() &&
            loopStartInQueue(ch, target);
        if (wrapWithCache)
        {
            // The start of the loop was pre-decoded: continue without a
            // seek - the video queue already holds the first frames of the
            // pass. Reset the audio state so it follows the source: the
            // packets near the loop start are parked by the source and
            // drained by the first audio pull (see the FFmpeg parking-lot
            // note in AGENTS.md), so the wrap stays seamless there too.
            //
            // The queue also still holds the tail of the previous pass (the
            // pre-decode only appended the loop-start frames). Those tail
            // frames are stale from now on: the target is back at the loop
            // start, so they can never be selected again. Remove them, or
            // they block the front trim (their time is "after" the wrapped
            // target) and keep the queue "full" so the decode loop below
            // never runs and the display freezes.
            while (!ch.video.empty() &&
                   ch.video.front().time > target.loopStart + Time::fromSeconds(0.1))
            {
                ch.video.pop_front();
            }
            // Merge the staged loop-start frames into the (now cleared)
            // tail; the source is positioned just after them, so the decode
            // loop continues seamlessly from here.
            ch.video.insert(ch.video.end(), std::make_move_iterator(ch.preLoopVideo.begin()),
                            std::make_move_iterator(ch.preLoopVideo.end()));
            ch.preLoopVideo.clear();
            ch.audio.clear();
            ch.audioBase = Time::zero();
            ch.audioDecodedUpTo = Time::zero();
        }
        else if (!source->seek(target.mediaTime))
        {
            ch.primed = false; // retry on the next pass
            finishPass();
            return;
        }
        else
        {
            ch.video.clear();
            ch.preLoopVideo.clear(); // staged frames of a pass that did not wrap
            ch.audio.clear();
            ch.audioBase = Time::zero();
            ch.audioDecodedUpTo = Time::zero();
        }
        ch.lastDeliveredTime = Time::zero();
        ch.hasDelivered = false;
        ch.generation = generation;
        ch.primed = true;
        worker.preLoopDone = false;
        worker.pumpSuspended = false;
        ch.lastSummary = std::chrono::steady_clock::now(); // health window starts at prime
    }

    // --- Seamless looping: pre-decode the loop start -------------------------
    // While the playhead approaches the loop end, the worker drains the
    // remaining end-of-pass frames from the source, seeks it to the loop
    // start and pre-decodes kPreLoopFrames start frames. At the wrap the
    // staged frames are merged into the queue, so no seek stalls the
    // display.
    //
    // The decode work runs WITHOUT the channel lock (the source is
    // worker-private): a ~150 ms decode burst under the lock would stall
    // every deliverVideo()/mixAudio() of the frame loop. While the lock is
    // released the render thread keeps delivering the tail frames and the
    // audio callback keeps consuming the tail audio - both stay consistent
    // because the pre-decode only appends ahead of them.
    if (target.loop && target.loopStart.isValid() && target.loopEnd.isValid() && md.hasVideo &&
        !worker.preLoopDone && !worker.pumpSuspended)
    {
        const Time remaining = target.loopEnd - target.mediaTime;
        const Time window =
            Time::fromFrames(kPreLoopFrames + kPreLoopMarginFrames, md.frameRate) +
            Time::fromSeconds(0.1);
        if (remaining.isValid() && remaining >= Time::zero() && remaining <= window)
        {
            const Time audioUpToBefore = ch.audioDecodedUpTo;
            lock.unlock();
            const PredecodeResult pre = preDecodeUnlocked(*source, target, md, audioUpToBefore);
            lock.lock();
            for (auto& frame : pre.tailVideo)
            {
                ch.video.push_back(std::move(frame));
            }
            if (pre.haveAudioBase && !ch.audioBase.isValid())
            {
                ch.audioBase = pre.audioBase;
            }
            for (auto& block : pre.tailAudio)
            {
                ch.audio.insert(ch.audio.end(), block.samples.begin(), block.samples.end());
            }
            if (pre.haveAudioUpTo)
            {
                ch.audioDecodedUpTo = pre.audioUpTo;
            }
            for (auto& frame : pre.startVideo)
            {
                ch.preLoopVideo.push_back(std::move(frame));
            }
            if (pre.seeked)
            {
                worker.preLoopDone = true;
                // The source is parked at the loop start: suspend BOTH
                // decode loops until the wrap. Decoding more would push
                // next-pass frames into the live queue (they sit far behind
                // the pre-wrap target and hijack the frame selection).
                worker.pumpSuspended = true;
            }
        }
    }

    // --- Video: keep the lookahead queue full -----------------------------
    if (md.hasVideo)
    {
        // (Suspended between the loop pre-decode and the wrap: the source is
        // parked at the loop start, so decoding would push next-pass frames
        // into the live queue.)
        if (!worker.pumpSuspended)
        {
            int decodes = 0;
            while (ch.video.size() < static_cast<size_t>(kVideoLookaheadFrames + 2) &&
                   ++decodes <= kMaxVideoDecodesPerPass)
            {
                VideoFrame frame;
                const auto d0 = std::chrono::steady_clock::now();
                const bool got = source->nextVideoFrame(frame, kDecodeTimeoutMs);
                const std::int64_t dn = nanosSince(d0);
                ch.decodeCalls++;
                ch.decodeNanos += dn;
                if (dn > kSlowDecodeNanos)
                {
                    ++ch.slowDecodes;
                }
                if (!got)
                {
                    break; // EOF or decoder not ready yet
                }
                ch.video.push_back(std::move(frame));
            }
        }
        // Frames already handed to the GPU are only needed for the next
        // deliverVideo() call; drop them to bound memory. (After a loop wrap
        // hasDelivered is false until the first delivery of the new pass, so
        // the pre-decoded start frames are protected.)
        if (ch.hasDelivered)
        {
            while (!ch.video.empty() && ch.video.front().time <= ch.lastDeliveredTime)
            {
                ch.video.pop_front();
            }
        }
        // Queue integrity: count gaps (missing frames between consecutive
        // entries). A gap appears when the trim pops a frame the display
        // never showed - the presentation jumps two frames after a render
        // stall, and the skipped frame is trimmed away.
        if (ch.video.size() >= 2)
        {
            const double stepSeconds = ch.mediaFrameRate.secondsPerFrame();
            if (stepSeconds > 0.0)
            {
                std::int64_t gaps = 0;
                for (std::size_t i = 1; i < ch.video.size(); ++i)
                {
                    if ((ch.video[i].time - ch.video[i - 1].time).toSeconds() > 1.5 * stepSeconds)
                    {
                        ++gaps;
                    }
                }
                ch.currentGaps = gaps;
            }
        }
    }

    // --- Audio: keep kAudioLookaheadSeconds decoded ahead ------------------
    // (Suspended between the loop pre-decode and the wrap - the tail audio
    // was already filled up to the loop end by the pre-decode.)
    if (md.hasAudio && md.audioSampleRate > 0.0 && !worker.pumpSuspended)
    {
        const Time horizon = target.mediaTime + Time::fromSeconds(kAudioLookaheadSeconds);
        int decodes = 0;
        while (ch.audioDecodedUpTo < horizon && ++decodes <= kMaxAudioDecodesPerPass)
        {
            AudioFrame block;
            if (!source->nextAudioFrame(block, kDecodeTimeoutMs))
            {
                break; // EOF or decoder not ready yet
            }
            if (block.channels == 0 || block.sampleRate <= 0.0 || block.samples.empty())
            {
                break;
            }
            ch.audio.insert(ch.audio.end(), block.samples.begin(), block.samples.end());
            if (!ch.audioBase.isValid())
            {
                ch.audioBase = block.time;
            }
            ch.audioDecodedUpTo =
                block.time + Time::fromSeconds(static_cast<double>(block.samples.size() / block.channels) /
                                               block.sampleRate);
        }

        // Trim history: nothing older than kAudioTrimHistorySeconds behind the
        // target will ever be mixed.
        const double baseSeconds = ch.audioBase.toSeconds();
        const double trimSeconds = target.mediaTime.toSeconds() - kAudioTrimHistorySeconds;
        if (trimSeconds > baseSeconds)
        {
            const std::size_t drop = static_cast<std::size_t>((trimSeconds - baseSeconds) * md.audioSampleRate);
            if (drop > 0 && drop < ch.audio.size())
            {
                ch.audio.erase(ch.audio.begin(), ch.audio.begin() + drop);
                ch.audioBase = ch.audioBase + Time::fromSeconds(static_cast<double>(drop) / md.audioSampleRate);
            }
        }
    }

    ch.lastTarget = target;
    finishPass();
}

void MediaPump::printHealth(Channel& ch, Id layerId)
{
    // The caller holds ch.mutex. Windowed averages since the last summary.
    const auto avgMs = [](std::int64_t nanos, std::int64_t count) {
        return count > 0 ? static_cast<double>(nanos) / count / 1e6 : 0.0;
    };
    Logger::instance().debug(
        "media pump: layer {} health: passes {} (avg {:.1f} ms, max {:.1f} ms), "
        "lockwait {:.1f} ms, decodes {} (avg {:.1f} ms, {} slow >10 ms), "
        "queue {}..{} gaps {}, tailLag {:.1f} frames, backlog max {:.1f}, "
        "delivers {} (lockwait {:.1f} ms, upload avg {:.1f} ms, ensure {:.1f} ms, "
        "update {:.1f} ms, max {:.1f} ms)",
        layerId, ch.passCount, avgMs(ch.passNanos, ch.passCount), ch.passMaxNanos / 1e6,
        avgMs(ch.lockWaitNanos, ch.passCount), ch.decodeCalls, avgMs(ch.decodeNanos, ch.decodeCalls),
        ch.slowDecodes, ch.minQueueDepth, ch.maxQueueDepth, ch.currentGaps, ch.maxTailLag,
        ch.maxBacklog, ch.deliverCount, avgMs(ch.deliverLockNanos, ch.deliverCount),
        avgMs(ch.uploadNanos, ch.deliverCount), avgMs(ch.ensureNanos, ch.deliverCount),
        avgMs(ch.uploadNanos, ch.deliverCount) - avgMs(ch.ensureNanos, ch.deliverCount),
        ch.uploadMaxNanos / 1e6);
    ch.passCount = 0;
    ch.passNanos = 0;
    ch.passMaxNanos = 0;
    ch.lockWaitNanos = 0;
    ch.decodeCalls = 0;
    ch.decodeNanos = 0;
    ch.slowDecodes = 0;
    ch.minQueueDepth = 0;
    ch.maxQueueDepth = 0;
    ch.haveQueueSample = false;
    ch.deliverCount = 0;
    ch.deliverLockNanos = 0;
    ch.uploadNanos = 0;
    ch.uploadMaxNanos = 0;
    ch.ensureNanos = 0;
    ch.maxTailLag = 0.0;
    ch.maxBacklog = 0.0;
}

MediaPump::PredecodeResult
MediaPump::preDecodeUnlocked(IMediaSource& source, const LayerTarget& target,
                             const MediaMetadata& md, const Time& audioUpTo)
{
    // Called WITHOUT ch.mutex: the source is worker-private, and the caller
    // applies the result to the channel under the lock.
    PredecodeResult result;

    // 0. Drain the remaining end-of-pass video frames (past the normal
    //    lookahead cap, up to EOF): the source is about to be seeked away,
    //    so anything not in the queue now would never be decoded and the
    //    last tenths of a second before the wrap would freeze on screen.
    {
        int drain = 0;
        while (++drain <= 16)
        {
            VideoFrame frame;
            if (!source.nextVideoFrame(frame, kDecodeTimeoutMs))
            {
                break; // EOF
            }
            result.tailVideo.push_back(std::move(frame));
        }
    }

    // 1. Fill the tail audio up to the loop end while the source is still
    //    at the tail (the last blocks of the pass would otherwise go
    //    silent).
    if (md.hasAudio && md.audioSampleRate > 0.0)
    {
        Time upTo = audioUpTo.isValid() ? audioUpTo : Time::zero();
        int decodes = 0;
        while (upTo < target.loopEnd && ++decodes <= kMaxAudioDecodesPerPass)
        {
            AudioFrame block;
            if (!source.nextAudioFrame(block, kDecodeTimeoutMs))
            {
                break; // EOF
            }
            if (block.channels == 0 || block.sampleRate <= 0.0 || block.samples.empty())
            {
                break;
            }
            const Time blockUpTo =
                block.time + Time::fromSeconds(static_cast<double>(block.samples.size() / block.channels) /
                                               block.sampleRate);
            if (!result.haveAudioBase)
            {
                result.haveAudioBase = true;
                result.audioBase = block.time;
            }
            result.tailAudio.push_back(std::move(block));
            result.haveAudioUpTo = true;
            result.audioUpTo = blockUpTo;
            upTo = blockUpTo;
        }
    }

    // 2. Seek to the loop start and pre-decode the first frames. The audio
    //    packets read while pulling video are parked by the source and
    //    drained by the first audio pull after the wrap.
    const auto seekStart = std::chrono::steady_clock::now();
    result.seeked = source.seek(target.loopStart);
    Logger::instance().debug("predecode: seek to loop start took {} ms (ok={})",
                             std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - seekStart)
                                 .count(),
                             result.seeked ? 1 : 0);
    int frames = 0;
    while (frames < kPreLoopFrames)
    {
        VideoFrame frame;
        const bool got = source.nextVideoFrame(frame, kStagingTimeoutMs);
        if (!got)
        {
            break;
        }
        // Staged, not yet merged: a delivery before the wrap must not be
        // able to select these (they are far behind the pre-wrap target).
        result.startVideo.push_back(std::move(frame));
        ++frames;
    }
    Logger::instance().debug(
        "predecode: staged {} start frames, drained {} tail video / {} tail audio blocks",
        frames, result.tailVideo.size(), result.tailAudio.size());
    return result;
}

bool MediaPump::loopStartInQueue(const Channel& ch, const LayerTarget& target)
{
    if (!target.loopStart.isValid())
    {
        return false;
    }
    // Tolerance: the pre-decoded frames sit at (and just after) the loop
    // start; a few frames of slack cover frame-step rounding.
    const Time upper = target.loopStart + Time::fromSeconds(0.1);
    for (const auto& frame : ch.preLoopVideo)
    {
        if (frame.time >= target.loopStart && frame.time <= upper)
        {
            return true;
        }
    }
    return false;
}

nvrhi::TextureHandle MediaPump::deliverVideo(const LayerTarget& target)
{
    if (target.mediaId == kInvalidId)
    {
        return nullptr;
    }
    std::shared_ptr<Channel> channel = findChannel(target.layerId, target.mediaId);
    if (channel == nullptr)
    {
        return nullptr;
    }

    nvrhi::TextureHandle texture;
    const auto dl0 = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(channel->mutex);
        channel->deliverCount++;
        channel->deliverLockNanos += nanosSince(dl0);
        // The newest decoded frame at or before the target media time.
        // (Non-const: the chosen frame's pixels are MOVED out into the
        // upload queue below.)
        VideoFrame* chosen = nullptr;
        for (auto it = channel->video.rbegin(); it != channel->video.rend(); ++it)
        {
            if (it->time <= target.mediaTime)
            {
                chosen = &*it;
                break;
            }
        }
        // How far the queue tail sits behind the target (starvation margin).
        if (!channel->video.empty())
        {
            const double stepSeconds = channel->mediaFrameRate.secondsPerFrame();
            if (stepSeconds > 0.0)
            {
                const double lag = (target.mediaTime - channel->video.back().time).toSeconds() /
                                   stepSeconds;
                channel->maxTailLag = std::max(channel->maxTailLag, lag);
            }
        }
        if (chosen != nullptr)
        {
            if (!channel->hasDelivered || chosen->time != channel->lastDeliveredTime)
            {
                const auto u0 = std::chrono::steady_clock::now();
                m_Renderer.ensureMediaTexture(target.layerId, chosen->width, chosen->height);
                const auto u1 = std::chrono::steady_clock::now();
                // The pixels move into the upload queue (no 33 MB copy): the
                // frame stays in the channel queue for time bookkeeping only,
                // and the same time is never uploaded twice (the guard above).
                m_Renderer.updateMediaTexture(target.layerId, std::move(chosen->rgba));
                const std::int64_t un = nanosSince(u0);
                const std::int64_t ue =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(u1 - u0).count();
                channel->uploadNanos += un;
                channel->uploadMaxNanos = std::max(channel->uploadMaxNanos, un);
                channel->ensureNanos += ue;
                channel->lastDeliveredTime = chosen->time;
                channel->hasDelivered = true;
            }
        }

        // Dropped-frames metric: one presentation pass per call, whether or
        // not a new frame reached the screen (a repeated or missing frame
        // while the playhead runs on counts as drops).
        const Time frameStep = Time::fromFrames(1, channel->mediaFrameRate);
        channel->dropCounter.advance(
            target.mediaTime, chosen != nullptr ? std::optional<Time>(chosen->time) : std::nullopt,
            frameStep);

        texture = m_Renderer.mediaTexture(target.layerId);
    }
    return texture;
}

void MediaPump::mixAudio(const std::vector<LayerTarget>& targets, float* outStereo, int frameCount)
{
    for (int i = 0; i < frameCount; ++i)
    {
        outStereo[i * 2 + 0] = 0.f;
        outStereo[i * 2 + 1] = 0.f;
    }
    if (frameCount <= 0 || targets.empty())
    {
        return;
    }

    for (const auto& target : targets)
    {
        if (!target.hasAudio || target.audioSampleRate <= 0.0 || target.gain <= 0.0005f ||
            target.mediaId == kInvalidId)
        {
            continue;
        }
        std::shared_ptr<Channel> channel = findChannel(target.layerId, target.mediaId);
        if (channel == nullptr)
        {
            continue;
        }

        std::lock_guard lock(channel->mutex);
        if (channel->audio.empty() || !channel->audioBase.isValid())
        {
            continue;
        }

        const double baseSeconds = channel->audioBase.toSeconds();
        const double rate = target.audioSampleRate;
        const double t0 = target.mediaTime.toSeconds();
        const int sourceChannels = target.audioChannels >= 1 ? target.audioChannels : 1;
        const std::size_t sampleCount = channel->audio.size();
        const float gain = target.gain;
        const auto& samples = channel->audio;

        for (int i = 0; i < frameCount; ++i)
        {
            // The i-th output sample plays at media time t0 + i / sinkRate.
            const double t = t0 + static_cast<double>(i) / kSinkSampleRate;
            if (t < baseSeconds)
            {
                continue; // gap (just after a seek): silence
            }
            const double position = (t - baseSeconds) * rate;
            if (position >= static_cast<double>(sampleCount))
            {
                break; // not decoded yet (starvation): silence for the rest
            }
            const std::size_t i0 = static_cast<std::size_t>(position);
            const std::size_t i1 = std::min(i0 + 1, sampleCount - 1);
            const float frac = static_cast<float>(position - static_cast<double>(i0));

            auto pick = [&](std::size_t idx, int chan) -> float {
                const std::size_t offset = idx * static_cast<std::size_t>(sourceChannels) +
                                            static_cast<std::size_t>(chan % sourceChannels);
                return offset < sampleCount ? samples[offset] : 0.f;
            };

            outStereo[i * 2 + 0] += (pick(i0, 0) * (1.f - frac) + pick(i1, 0) * frac) * gain;
            outStereo[i * 2 + 1] += (pick(i0, 1) * (1.f - frac) + pick(i1, 1) * frac) * gain;
        }
    }
}

std::uint64_t MediaPump::droppedVideoFrames() const
{
    std::lock_guard lock(m_ChannelsMutex);
    std::uint64_t total = m_DroppedRetired;
    for (const auto& [layerId, channel] : m_Channels)
    {
        std::lock_guard channelLock(channel->mutex);
        total += channel->dropCounter.total();
    }
    return total;
}

} // namespace mediamax
