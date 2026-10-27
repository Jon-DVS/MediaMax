#pragma once

// Media value types shared by all media sources.

#include <cstdint>
#include <string>
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

    // Codec-level video details, filled in from the container/codec probe.
    // Empty strings and zero mean "not reported" (the FFmpeg source fills
    // these; synthetic sources leave them default).
    std::string videoCodec; // e.g. "h264", "png" (avcodec_get_name)
    std::string videoColorspace; // e.g. "bt709" (av_color_space_name)
    std::uint32_t videoBitDepth = 0; // e.g. 8, 10; 0 = unknown
    std::string videoScanType; // "progressive" / "interlaced"; empty = unknown
    std::int64_t videoBitRate = 0; // bits/s; 0 = unknown

    // Audio properties (zero when no audio).
    bool hasAudio = false;
    std::uint32_t audioChannels = 0;
    double audioSampleRate = 0.0;
    std::string audioCodec; // e.g. "aac" (avcodec_get_name)

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
