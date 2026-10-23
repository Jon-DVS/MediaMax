#pragma once

// Procedural media sources used for demos, tests and headless operation.
//
// TestPatternSource generates deterministic frames: a specific frame index
// always produces the exact same pixels, which makes it ideal for regression
// tests of the render pipeline and for running the playback engine without
// any media files.

#include <string>

#include "mediamax/blend/Blend.h"
#include "mediamax/media/MediaSource.h"

namespace mediamax
{

enum class TestPatternKind : std::uint8_t
{
    ColorBars, // seven vertical color bars
    Gradient,  // diagonal moving gradient
    Solid,     // solid color (configurable)
    Checkerboard, // moving checkerboard (easy to see motion)
};

class TestPatternSource : public IMediaSource
{
public:
    TestPatternSource(std::uint32_t width, std::uint32_t height, const FrameRate& fps,
                      Time duration, TestPatternKind kind, Color4 solidColor = {});

    MediaMetadata metadata() const override;

    bool seek(const Time& t) override;
    bool nextVideoFrame(VideoFrame& out, std::uint32_t timeoutMs) override;
    bool nextAudioFrame(AudioFrame& out, std::uint32_t timeoutMs) override;

    bool hasVideo() const override { return true; }

    // Renders the frame for media time t into a pre-allocated RGBA buffer.
    // Exposed for unit tests (pure, deterministic).
    void renderFrameTo(const Time& t, std::vector<std::uint8_t>& outRgba) const;

    Time currentTime() const { return m_Current; }

private:
    MediaMetadata m_Metadata;
    TestPatternKind m_Kind;
    Color4 m_SolidColor;
    Time m_Current; // position after the last seek/frame (exclusive next)
};

} // namespace mediamax
