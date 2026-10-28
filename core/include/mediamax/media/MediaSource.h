#pragma once

// Abstraction over a decoded media stream.
//
// An IMediaSource owns its decoder (FFmpeg context, procedural generator,
// ...) and is consumed by the playback engine's media pump:
//
//   - seek(t) positions the stream at media time t
//   - nextVideoFrame(out, timeout) returns the next decoded video frame,
//     blocking up to `timeoutMs`; false when the stream is exhausted
//   - nextAudioFrame(out, timeout) ditto for audio
//
// A source instance is single-consumer: exactly one thread calls its
// methods at a time. The playback engine guarantees this: the media pump's
// decode workers each open and consume their own instance per active layer
// (see MediaPump); the library's own instance is kept for probing/metadata
// and is not decoded by anyone.

#include "mediamax/math/Rect.h"
#include "mediamax/media/MediaTypes.h"

namespace mediamax
{

class IMediaSource
{
public:
    virtual ~IMediaSource() = default;

    // Metadata (duration, size, rates). Valid after construction.
    virtual MediaMetadata metadata() const = 0;

    // Positions the stream at media time t (seek to the keyframe at or
    // before t for video).
    virtual bool seek(const Time& t) = 0;

    // Advances and returns the next video frame. Blocks up to timeoutMs
    // waiting for decoder output. Returns false on EOF or error.
    virtual bool nextVideoFrame(VideoFrame& out, std::uint32_t timeoutMs) = 0;

    // Advances and returns the next audio block.
    virtual bool nextAudioFrame(AudioFrame& out, std::uint32_t timeoutMs) = 0;

    // Hint: the video is displayed at about this size in px (an invalid
    // size = no hint). Sources that scale at decode time (FFmpegSource)
    // decode at min(native, hint) so the uploaded texture matches what the
    // display shows - decoding 4K for a 1080p layer wastes CPU and upload
    // bandwidth. Sources without a scalable path (or without a video
    // stream) ignore it. Idempotent: a source no-ops while the hint
    // matches its current output.
    virtual void setVideoOutputSizeHint(Size2i) {}

    // Sources without a video stream always return false.
    virtual bool hasVideo() const { return false; }
    virtual bool hasAudio() const { return false; }
};

} // namespace mediamax
