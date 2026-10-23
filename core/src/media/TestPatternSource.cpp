#include "mediamax/media/TestPatternSource.h"

#include <algorithm>
#include <cmath>

namespace mediamax
{

namespace
{

struct BarColor
{
    float r, g, b;
};

// The seven classic bars (white, yellow, cyan, green, magenta, red, blue).
const BarColor kBars[] = {
    {1.f, 1.f, 1.f},
    {1.f, 1.f, 0.f},
    {0.f, 1.f, 1.f},
    {0.f, 1.f, 0.f},
    {1.f, 0.f, 1.f},
    {1.f, 0.f, 0.f},
    {0.f, 0.f, 1.f},
};

} // namespace

TestPatternSource::TestPatternSource(std::uint32_t width, std::uint32_t height,
                                     const FrameRate& fps, Time duration, TestPatternKind kind,
                                     Color4 solidColor)
    : m_Kind(kind)
    , m_SolidColor(solidColor)
{
    m_Metadata.type = MediaType::Video;
    m_Metadata.hasVideo = true;
    m_Metadata.width = width;
    m_Metadata.height = height;
    m_Metadata.duration = duration;
    m_Metadata.frameRate = fps;
    m_Current = Time::zero();
}

MediaMetadata TestPatternSource::metadata() const
{
    return m_Metadata;
}

bool TestPatternSource::seek(const Time& t)
{
    m_Current = t.clamp(Time::zero(), m_Metadata.duration);
    return true;
}

bool TestPatternSource::nextVideoFrame(VideoFrame& out, std::uint32_t /*timeoutMs*/)
{
    if (m_Current >= m_Metadata.duration)
    {
        return false; // EOF
    }

    out.width = m_Metadata.width;
    out.height = m_Metadata.height;
    out.time = m_Current;
    renderFrameTo(m_Current, out.rgba);

    m_Current = m_Current + Time::fromFrames(1, m_Metadata.frameRate);
    return true;
}

bool TestPatternSource::nextAudioFrame(AudioFrame& /*out*/, std::uint32_t /*timeoutMs*/)
{
    return false; // no audio
}

void TestPatternSource::renderFrameTo(const Time& t, std::vector<std::uint8_t>& outRgba) const
{
    const std::uint32_t w = m_Metadata.width;
    const std::uint32_t h = m_Metadata.height;
    outRgba.resize(static_cast<size_t>(w) * h * 4);

    // Frame index within the pattern's motion cycle. The pattern repeats
    // every 8 seconds so motion is visible but slow enough to inspect.
    const std::int64_t frameIndex = t.toFrames(m_Metadata.frameRate);
    const double seconds = t.toSeconds();

    for (std::uint32_t y = 0; y < h; ++y)
    {
        for (std::uint32_t x = 0; x < w; ++x)
        {
            float r = 0.f, g = 0.f, b = 0.f;

            switch (m_Kind)
            {
                case TestPatternKind::ColorBars:
                {
                    const BarColor& c = kBars[(x * 7) / w];
                    r = c.r;
                    g = c.g;
                    b = c.b;
                    break;
                }
                case TestPatternKind::Gradient:
                {
                    // Diagonal gradient scrolling with time.
                    constexpr double kTwoPi = 6.283185307179586;
                    const float phase = static_cast<float>((seconds * 0.25) * kTwoPi);
                    const float u = static_cast<float>(x) / static_cast<float>(w);
                    const float v = static_cast<float>(y) / static_cast<float>(h);
                    const float s = (u + v) * 0.5f + static_cast<float>(0.5 * (1.0 - std::cos(phase)));
                    r = 0.5f + 0.5f * std::sin(s * 6.28318f);
                    g = 0.5f + 0.5f * std::sin(s * 6.28318f + 2.0944f);
                    b = 0.5f + 0.5f * std::sin(s * 6.28318f + 4.18879f);
                    break;
                }
                case TestPatternKind::Solid:
                {
                    r = m_SolidColor.r;
                    g = m_SolidColor.g;
                    b = m_SolidColor.b;
                    break;
                }
                case TestPatternKind::Checkerboard:
                {
                    const int cell = 32;
                    const int cx = static_cast<int>(x) / cell + static_cast<int>(frameIndex / 10);
                    const int cy = static_cast<int>(y) / cell;
                    const bool on = ((cx + cy) & 1) == 0;
                    r = g = b = on ? 1.f : 0.f;
                    break;
                }
            }

            const std::size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            outRgba[idx + 0] = static_cast<std::uint8_t>(std::clamp(r, 0.f, 1.f) * 255.f + 0.5f);
            outRgba[idx + 1] = static_cast<std::uint8_t>(std::clamp(g, 0.f, 1.f) * 255.f + 0.5f);
            outRgba[idx + 2] = static_cast<std::uint8_t>(std::clamp(b, 0.f, 1.f) * 255.f + 0.5f);
            outRgba[idx + 3] = 255;
        }
    }
}

} // namespace mediamax
