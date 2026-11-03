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

// The pixel format of a decoded video frame (what a source produces).
enum class VideoPixelFormat : std::uint8_t
{
    // One plane, RGBA, 4 bytes per pixel, top-left origin. Images, test
    // patterns, and any source format without a plane fast path.
    RGBA = 0,
    // Three planes, YUV 4:2:0, 1 byte per sample, top-left origin:
    //   plane 0 = Y:  width * height
    //   plane 1 = U:  (width/2) * (height/2)
    //   plane 2 = V:  (width/2) * (height/2)
    YUV420P,
    // Three planes, YUV 4:2:0, 2 bytes per sample (10-bit, little-endian,
    // scaled to 16-bit UNORM storage: sample * 65535 / 1023, so 1023 maps
    // to 65535 exactly). Plane layout as YUV420P with 2x the bytes.
    YUV420P10LE,
};

// The YUV -> RGB matrix of the content (see VideoConversion.h).
enum class YuvColorSpace : std::uint8_t
{
    BT601,   // SMPTE 170M (SD)
    BT709,   // HD
    BT2020,  // UHDTV
    Unknown, // not reported: the conversion defaults by frame height
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

// One decoded video frame, top-left origin, tightly packed.
//
// `planes` holds the frame's pixel data for its `format` (see
// VideoPixelFormat): RGBA frames have one plane (width * height * 4 bytes),
// YUV420P(10LE) frames have three (see the format's layout). YUV frames
// carry the color metadata the YUV -> RGB conversion needs (the GPU layer
// pass converts them on the GPU; see VideoConversion.h for the CPU
// reference of the same math).
struct VideoFrame
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VideoPixelFormat format = VideoPixelFormat::RGBA;
    std::vector<std::vector<std::uint8_t>> planes;
    // YUV frames: the content's matrix and range (RGBA frames ignore them).
    YuvColorSpace colorSpace = YuvColorSpace::Unknown;
    bool fullRange = false;
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
