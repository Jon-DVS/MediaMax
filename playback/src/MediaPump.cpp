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

// Safety caps per pump pass (a stalled decoder must not spin the thread).
constexpr int kMaxVideoDecodesPerPass = 32;
constexpr int kMaxAudioDecodesPerPass = 128;

// A channel re-seeks when its target media time jumps backwards by more than
// this (loop wrap or a seek the generation counter did not catch).
constexpr double kBackwardJumpSeconds = 0.25;

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
        if (m_Thread.joinable())
        {
            m_Thread.join();
        }
    }
}

void MediaPump::publishTargets(const Time& showTime, const std::vector<LayerTarget>& targets,
                               std::uint64_t generation)
{
    std::lock_guard lock(m_TargetsMutex);
    m_ShowTime = showTime;
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
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!m_Running.load())
        {
            break;
        }

        Time showTime;
        std::vector<LayerTarget> targets;
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(m_TargetsMutex);
            showTime = m_ShowTime;
            targets = m_Targets;
            generation = m_Generation;
        }
        if (targets.empty())
        {
            continue;
        }

        std::vector<Id> activeLayers;
        activeLayers.reserve(targets.size());
        for (const auto& target : targets)
        {
            if (target.mediaId == kInvalidId)
            {
                continue; // color layer
            }
            activeLayers.push_back(target.layerId);
            auto channel = channelFor(target);
            if (channel == nullptr)
            {
                continue; // media missing from the library
            }
            decodeChannel(*channel, target, generation);
        }

        // Drop channels whose layer is no longer part of the published state.
        {
            std::lock_guard lock(m_ChannelsMutex);
            for (auto it = m_Channels.begin(); it != m_Channels.end();)
            {
                const bool active =
                    std::find(activeLayers.begin(), activeLayers.end(), it->first) != activeLayers.end();
                if (!active)
                {
                    it = m_Channels.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }
}

std::shared_ptr<MediaPump::Channel> MediaPump::channelFor(const LayerTarget& target)
{
    std::shared_ptr<Channel> channel;
    {
        std::lock_guard lock(m_ChannelsMutex);
        auto it = m_Channels.find(target.layerId);
        if (it != m_Channels.end())
        {
            channel = it->second;
            if (channel->mediaId != target.mediaId)
            {
                // The layer's media changed: rebind and reset all state.
                std::lock_guard chLock(channel->mutex);
                channel->mediaId = target.mediaId;
                channel->source = nullptr;
                channel->generation = 0;
                channel->primed = false;
                channel->video.clear();
                channel->audio.clear();
                channel->audioBase = Time::zero();
                channel->audioDecodedUpTo = Time::zero();
                channel->lastDeliveredTime = Time::zero();
                channel->hasDelivered = false;
            }
        }
        else
        {
            channel = std::make_shared<Channel>();
            channel->mediaId = target.mediaId;
            m_Channels.emplace(target.layerId, channel);
        }
    }

    // Library lookup under the app's library mutex (the pump is the only
    // thread reading the library outside that lock).
    std::shared_ptr<IMediaSource> source;
    {
        std::lock_guard lock(m_LibraryMutex);
        if (const auto asset = m_Media.find(target.mediaId))
        {
            source = (*asset)->source;
        }
    }
    if (source == nullptr)
    {
        Logger::instance().debug("media pump: media {} not in the library", target.mediaId);
        return nullptr;
    }
    {
        std::lock_guard lock(channel->mutex);
        channel->source = source;
    }
    return channel;
}

void MediaPump::decodeChannel(Channel& ch, const LayerTarget& target, std::uint64_t generation)
{
    std::lock_guard lock(ch.mutex);
    if (!ch.source)
    {
        return;
    }

    // Re-seek when the generation changed, when the target jumped backwards
    // (loop wrap) or on first use.
    const bool generationStale = ch.generation != generation;
    const bool backwardJump = ch.lastTarget.has_value() &&
                              target.mediaTime < ch.lastTarget->mediaTime - Time::fromSeconds(kBackwardJumpSeconds);
    if (generationStale || backwardJump || !ch.primed)
    {
        if (!ch.source->seek(target.mediaTime))
        {
            ch.primed = false; // retry on the next pass
            return;
        }
        ch.video.clear();
        ch.audio.clear();
        ch.audioBase = Time::zero();
        ch.audioDecodedUpTo = Time::zero();
        ch.lastDeliveredTime = Time::zero();
        ch.hasDelivered = false;
        ch.generation = generation;
        ch.primed = true;
    }

    const MediaMetadata md = ch.source->metadata();

    // --- Video: keep the lookahead queue full -----------------------------
    if (md.hasVideo)
    {
        int decodes = 0;
        while (ch.video.size() < static_cast<size_t>(kVideoLookaheadFrames + 2) &&
               ++decodes <= kMaxVideoDecodesPerPass)
        {
            VideoFrame frame;
            if (!ch.source->nextVideoFrame(frame, kDecodeTimeoutMs))
            {
                break; // EOF or decoder not ready yet
            }
            ch.video.push_back(std::move(frame));
        }
        // Frames already handed to the GPU are only needed for the next
        // deliverVideo() call; drop them to bound memory.
        if (ch.hasDelivered)
        {
            while (!ch.video.empty() && ch.video.front().time <= ch.lastDeliveredTime)
            {
                ch.video.pop_front();
            }
        }
    }

    // --- Audio: keep kAudioLookaheadSeconds decoded ahead ------------------
    if (md.hasAudio && md.audioSampleRate > 0.0)
    {
        const Time horizon = target.mediaTime + Time::fromSeconds(kAudioLookaheadSeconds);
        int decodes = 0;
        while (ch.audioDecodedUpTo < horizon && ++decodes <= kMaxAudioDecodesPerPass)
        {
            AudioFrame block;
            if (!ch.source->nextAudioFrame(block, kDecodeTimeoutMs))
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
}

nvrhi::TextureHandle MediaPump::deliverVideo(const LayerTarget& target)
{
    if (target.mediaId == kInvalidId)
    {
        return nullptr;
    }
    std::shared_ptr<Channel> channel;
    {
        std::lock_guard lock(m_ChannelsMutex);
        auto it = m_Channels.find(target.layerId);
        if (it != m_Channels.end() && it->second->mediaId == target.mediaId)
        {
            channel = it->second;
        }
    }
    if (channel == nullptr)
    {
        return nullptr;
    }

    nvrhi::TextureHandle texture;
    {
        std::lock_guard lock(channel->mutex);
        // The newest decoded frame at or before the target media time.
        const VideoFrame* chosen = nullptr;
        for (auto it = channel->video.rbegin(); it != channel->video.rend(); ++it)
        {
            if (it->time <= target.mediaTime)
            {
                chosen = &*it;
                break;
            }
        }
        if (chosen != nullptr)
        {
            if (!channel->hasDelivered || chosen->time != channel->lastDeliveredTime)
            {
                m_Renderer.ensureMediaTexture(target.layerId, chosen->width, chosen->height);
                m_Renderer.updateMediaTexture(target.layerId, chosen->rgba);
                channel->lastDeliveredTime = chosen->time;
                channel->hasDelivered = true;
            }
        }
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
        std::shared_ptr<Channel> channel;
        {
            std::lock_guard lock(m_ChannelsMutex);
            auto it = m_Channels.find(target.layerId);
            if (it != m_Channels.end() && it->second->mediaId == target.mediaId)
            {
                channel = it->second;
            }
        }
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

} // namespace mediamax
