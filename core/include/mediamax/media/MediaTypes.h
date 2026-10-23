#pragma once

// Media value types shared by all media sources.

#include <cstdint>
#include <vector>

#include "mediamax/time/FrameRate.h"
#include "mediamax/time/Time.h"

namespace mediamax
{

enum class MediaType : std::uint8_t
{
    Unknown = 0,
    Video,   // video only (or video+audio; see hasVideo/hasAudio)
    Image,   // single still frame
    Audio,   // audio only
};

struct MediaMetadata
{
    MediaType type = MediaType::Unknown;

    // Video properties (zero when no video).
    bool hasVideo = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Time duration;
    FrameRate frameRate = FrameRate::fps30();

    // Audio properties (zero when no audio).
    bool hasAudio = false;
    std::uint32_t audioChannels = 0;
    double audioSampleRate = 0.0;

    bool valid() const { return hasVideo || hasAudio; }
};

// One decoded video frame, RGBA8, top-left origin, tightly packed.
struct VideoFrame
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4 bytes
    Time time; // media time of the first sample in this frame
};

// One decoded audio block: interleaved float32 samples in [-1, 1].
struct AudioFrame
{
    std::vector<float> samples; // channels * (samples / channels)
    std::uint32_t channels = 0;
    double sampleRate = 0.0;
    Time time; // media time of the first sample
};

// Frame / audio data as produced by a source, plus transport status.
struct FrameRequestResult
{
    bool eof = false;
    bool error = false;
};

} // namespace mediamax
