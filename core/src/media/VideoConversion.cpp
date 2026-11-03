// The YUV -> RGB conversion shared by the CPU and the GPU (see the header
// for the contract with render/shaders/layer.frag).

#include "mediamax/media/VideoConversion.h"

#include <algorithm>
#include <cmath>

namespace mediamax
{

namespace
{

// The matrix coefficients (normalized form: R = Y' + a*Cr', G = Y' - b*Cb' -
// c*Cr', B = Y' + d*Cb'). The GLSL layer pass uses the same literals.
struct MatrixCoeffs
{
    float a, b, c, d;
};

const MatrixCoeffs& matrixFor(YuvColorSpace space)
{
    static const MatrixCoeffs kTables[] = {
        {1.402f, 0.34414f, 0.71414f, 1.772f},  // BT601
        {1.5748f, 0.1873f, 0.4681f, 1.8556f},  // BT709
        {1.8814f, 0.1197f, 0.6812f, 2.1145f},  // BT2020
    };
    switch (space)
    {
        case YuvColorSpace::BT601:
            return kTables[0];
        case YuvColorSpace::BT2020:
            return kTables[2];
        case YuvColorSpace::BT709:
        case YuvColorSpace::Unknown:
        default:
            return kTables[1];
    }
}

// The stored (UNORM) value of one plane sample, as the texture sampler would
// return it (8-bit: byte / 255; 10-bit: the scaled 16-bit LE sample / 65535).
float sampleValue(const std::vector<std::uint8_t>& plane, std::size_t index, bool is10Bit)
{
    if (is10Bit)
    {
        // Explicit little-endian load (p10le semantics), no pointer casts.
        const std::uint16_t v =
            static_cast<std::uint16_t>(plane[2 * index] | (plane[2 * index + 1] << 8));
        return static_cast<float>(v) / 65535.0f;
    }
    return static_cast<float>(plane[index]) / 255.0f;
}

} // namespace

std::array<float, 3> yuvToRgb(float y, float cb, float cr, YuvColorSpace space, bool fullRange)
{
    // Range normalization: stored UNORM value -> linear Y' in [0, 1] and
    // centered chroma Cb'/Cr' (limited: luma 16-235, chroma 16-240; full:
    // 0-255, chroma centered at 128). The same expressions in the shader.
    float yn, cbn, crn;
    if (fullRange)
    {
        yn = y;
        cbn = cb - 0.5f;
        crn = cr - 0.5f;
    }
    else
    {
        yn = (y - 16.0f / 255.0f) * (255.0f / 219.0f);
        cbn = (cb - 128.0f / 255.0f) * (255.0f / 224.0f);
        crn = (cr - 128.0f / 255.0f) * (255.0f / 224.0f);
    }

    const MatrixCoeffs m = matrixFor(space);
    const float r = yn + m.a * crn;
    const float g = yn - m.b * cbn - m.c * crn;
    const float b = yn + m.d * cbn;
    return {std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f)};
}

std::array<float, 2> yuvChromaCoord(int x, int y, std::uint32_t width, std::uint32_t height)
{
    // The co-sited 4:2:0 mapping (see the header): the U/V texel (k, l) is
    // centered on the 2x2 luma block starting at (2k, 2l), so the luma texel
    // (x, y) reads the U/V plane at ((x + 1) / w, (y + 1) / h) - exactly on
    // the U/V texel center for even luma positions, and exactly on the
    // boundary between two U/V texels (a 50/50 blend) for odd ones.
    return {static_cast<float>(x + 1) / static_cast<float>(width),
            static_cast<float>(y + 1) / static_cast<float>(height)};
}

void scalePlane10To16(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out)
{
    // 1023 -> 65535 exactly; every other sample is rounded to the nearest
    // 16-bit value (max error 1/65535 in UNORM, below the float noise).
    const std::size_t count = in.size() / 2;
    out.resize(count * 2);
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::uint16_t v =
            static_cast<std::uint16_t>(in[2 * i] | (in[2 * i + 1] << 8));
        const std::uint32_t scaled = static_cast<std::uint32_t>(v) * 65535u / 1023u;
        out[2 * i] = static_cast<std::uint8_t>(scaled & 0xFFu);
        out[2 * i + 1] = static_cast<std::uint8_t>((scaled >> 8) & 0xFFu);
    }
}

bool convertYuvFrameToRgba(const VideoFrame& frame, std::vector<std::uint8_t>& outRgba)
{
    const bool is10Bit = frame.format == VideoPixelFormat::YUV420P10LE;
    if ((frame.format != VideoPixelFormat::YUV420P && !is10Bit) || frame.width < 2 ||
        frame.height < 2 || (frame.width & 1u) != 0 || (frame.height & 1u) != 0 ||
        frame.planes.size() < 3)
    {
        return false;
    }

    const std::uint32_t w = frame.width;
    const std::uint32_t h = frame.height;
    const std::uint32_t uw = w / 2u;
    const std::uint32_t uh = h / 2u;
    const std::size_t sampleBytes = is10Bit ? 2u : 1u;
    if (frame.planes[0].size() < static_cast<std::size_t>(w) * h * sampleBytes ||
        frame.planes[1].size() < static_cast<std::size_t>(uw) * uh * sampleBytes ||
        frame.planes[2].size() < static_cast<std::size_t>(uw) * uh * sampleBytes)
    {
        return false;
    }

    outRgba.resize(static_cast<std::size_t>(w) * h * 4);
    const int uwLast = static_cast<int>(uw) - 1;
    const int uhLast = static_cast<int>(uh) - 1;

    for (std::uint32_t y = 0; y < h; ++y)
    {
        // The luma row y reads the U/V row at the co-sited mapping's float
        // index y / 2 (bilinear, clamped like the sampler's edge replicate).
        const float cv = 0.5f * static_cast<float>(y);
        const int v0 = std::min(static_cast<int>(cv), uhLast);
        const float fv = cv - static_cast<float>(v0);
        const int v1 = std::min(v0 + 1, uhLast);

        for (std::uint32_t x = 0; x < w; ++x)
        {
            const std::size_t xy = static_cast<std::size_t>(y) * w + x;
            const float yv = sampleValue(frame.planes[0], xy, is10Bit);

            const float cu = 0.5f * static_cast<float>(x);
            const int u0 = std::min(static_cast<int>(cu), uwLast);
            const float fu = cu - static_cast<float>(u0);
            const int u1 = std::min(u0 + 1, uwLast);

            const float cb00 = sampleValue(frame.planes[1], static_cast<std::size_t>(v0) * uw + u0, is10Bit);
            const float cb10 = sampleValue(frame.planes[1], static_cast<std::size_t>(v0) * uw + u1, is10Bit);
            const float cb01 = sampleValue(frame.planes[1], static_cast<std::size_t>(v1) * uw + u0, is10Bit);
            const float cb11 = sampleValue(frame.planes[1], static_cast<std::size_t>(v1) * uw + u1, is10Bit);
            const float cr00 = sampleValue(frame.planes[2], static_cast<std::size_t>(v0) * uw + u0, is10Bit);
            const float cr10 = sampleValue(frame.planes[2], static_cast<std::size_t>(v0) * uw + u1, is10Bit);
            const float cr01 = sampleValue(frame.planes[2], static_cast<std::size_t>(v1) * uw + u0, is10Bit);
            const float cr11 = sampleValue(frame.planes[2], static_cast<std::size_t>(v1) * uw + u1, is10Bit);

            // Bilinear at the same coordinate the GPU's linear sampler uses.
            const float cbTop = cb00 + (cb10 - cb00) * fu;
            const float cbBot = cb01 + (cb11 - cb01) * fu;
            const float cb = cbTop + (cbBot - cbTop) * fv;
            const float crTop = cr00 + (cr10 - cr00) * fu;
            const float crBot = cr01 + (cr11 - cr01) * fu;
            const float cr = crTop + (crBot - crTop) * fv;

            const std::array<float, 3> rgb = yuvToRgb(yv, cb, cr, frame.colorSpace, frame.fullRange);
            std::uint8_t* dst = outRgba.data() + xy * 4;
            dst[0] = static_cast<std::uint8_t>(std::lround(rgb[0] * 255.0f));
            dst[1] = static_cast<std::uint8_t>(std::lround(rgb[1] * 255.0f));
            dst[2] = static_cast<std::uint8_t>(std::lround(rgb[2] * 255.0f));
            dst[3] = 255;
        }
    }
    return true;
}

} // namespace mediamax
