#include "mediamax/media/MediaThumbnail.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "mediamax/math/Rect.h"

namespace mediamax::media
{

namespace
{

// Seek cap for the representative frame: a 30 s clip lands at 3 s, a 4 h
// clip lands here (5 s is deep enough past any leader/fade-in to still be
// fast for a keyframe seek).
constexpr double kMaxSeekSeconds = 5.0;
// Fraction of the duration used for the seek (10%).
constexpr double kSeekFraction = 0.1;
// How long to wait for decoder output after the seek (ms).
constexpr std::uint32_t kDecodeTimeoutMs = 10000;

// Aspect-fit: the largest w x h with w <= nativeW, h <= nativeH (no
// upscaling) that fits within maxW x maxH. Sides are at least 1.
Size2i fitSize(const MediaMetadata& md, int maxW, int maxH)
{
    std::int32_t w = static_cast<std::int32_t>(md.width);
    std::int32_t h = static_cast<std::int32_t>(md.height);
    if (w > maxW)
    {
        h = (h * maxW) / w;
        w = maxW;
    }
    if (h > maxH)
    {
        w = (w * maxH) / h;
        h = maxH;
    }
    w = std::max(w, 1);
    h = std::max(h, 1);
    return Size2i{w, h};
}

// Bilinear resample of tightly packed RGBA8 (top-left origin). The scale
// factors are exact center-to-center maps, so a constant input produces a
// constant output (modulo rounding to 8 bits).
void bilinearScaleRgba(const std::uint8_t* src, int srcW, int srcH, std::uint8_t* dst, int dstW,
                       int dstH)
{
    const float sxScale = static_cast<float>(srcW) / static_cast<float>(dstW);
    const float syScale = static_cast<float>(srcH) / static_cast<float>(dstH);
    for (int y = 0; y < dstH; ++y)
    {
        const float sy = (static_cast<float>(y) + 0.5f) * syScale;
        const int y0 = std::min(static_cast<int>(sy), srcH - 1);
        const int y1 = std::min(y0 + 1, srcH - 1);
        const float fy = sy - static_cast<float>(y0);
        const std::uint8_t* top = src + static_cast<std::size_t>(y0) * srcW * 4;
        const std::uint8_t* bottom = src + static_cast<std::size_t>(y1) * srcW * 4;
        std::uint8_t* row = dst + static_cast<std::size_t>(y) * dstW * 4;
        for (int x = 0; x < dstW; ++x)
        {
            const float sx = (static_cast<float>(x) + 0.5f) * sxScale;
            const int x0 = std::min(static_cast<int>(sx), srcW - 1);
            const int x1 = std::min(x0 + 1, srcW - 1);
            const float fx = sx - static_cast<float>(x0);
            for (int ch = 0; ch < 4; ++ch)
            {
                const float a = static_cast<float>(top[static_cast<std::size_t>(x0) * 4 + ch]);
                const float b = static_cast<float>(top[static_cast<std::size_t>(x1) * 4 + ch]);
                const float c = static_cast<float>(bottom[static_cast<std::size_t>(x0) * 4 + ch]);
                const float d = static_cast<float>(bottom[static_cast<std::size_t>(x1) * 4 + ch]);
                const float topV = a + (b - a) * fx;
                const float v = topV + (c + (d - c) * fx - topV) * fy;
                row[static_cast<std::size_t>(x) * 4 + ch] = static_cast<std::uint8_t>(
                    std::lround(std::min(255.0f, std::max(0.0f, v))));
            }
        }
    }
}

} // namespace

bool makeThumbnail(const MediaSourceFactory& factory, const std::string& path,
                   const MediaMetadata& metadata, int maxWidth, int maxHeight,
                   std::vector<std::uint8_t>& outRgba, int& outWidth, int& outHeight,
                   std::string& error)
{
    if (!factory)
    {
        error = "no source factory installed (media cannot be decoded)";
        return false;
    }
    if (!metadata.hasVideo || metadata.width == 0 || metadata.height == 0)
    {
        error = "media has no video content";
        return false;
    }
    if (maxWidth < 1 || maxHeight < 1)
    {
        error = "invalid thumbnail size";
        return false;
    }

    const Size2i target = fitSize(metadata, maxWidth, maxHeight);

    auto source = factory(path, error);
    if (!source)
    {
        if (error.empty())
        {
            error = "could not open media: " + path;
        }
        return false;
    }
    // Scalable sources (FFmpegSource) decode directly at min(native, hint)
    // per axis; the aspect-fit target is <= native on both axes, so the hint
    // is exact and the decode runs at thumbnail size. Non-scalable sources
    // ignore the hint; the scale below then handles their native-size frame.
    source->setVideoOutputSizeHint(target);

    // Video: ~10% into the clip (capped at kMaxSeekSeconds) so the first
    // frame is representative (past any fade-in/leader). Images have no
    // duration; their single frame sits at t=0, which is the source's
    // initial position, so no seek is needed.
    if (metadata.duration > Time::zero())
    {
        Time t = metadata.duration * kSeekFraction;
        if (t.toSeconds() > kMaxSeekSeconds)
        {
            t = Time::fromSeconds(kMaxSeekSeconds);
        }
        if (!source->seek(t))
        {
            error = "seek to thumbnail time failed";
            return false;
        }
    }

    VideoFrame frame;
    if (!source->nextVideoFrame(frame, kDecodeTimeoutMs))
    {
        error = "could not decode a frame";
        return false;
    }
    const std::size_t expectedBytes =
        static_cast<std::size_t>(frame.width) * frame.height * 4;
    if (frame.width == 0 || frame.height == 0 || frame.rgba.size() < expectedBytes)
    {
        error = "decoded frame is empty";
        return false;
    }

    if (static_cast<int>(frame.width) == target.width &&
        static_cast<int>(frame.height) == target.height)
    {
        outRgba = std::move(frame.rgba);
    }
    else
    {
        outRgba.assign(static_cast<std::size_t>(target.width) * target.height * 4, 0);
        bilinearScaleRgba(frame.rgba.data(), static_cast<int>(frame.width),
                          static_cast<int>(frame.height), outRgba.data(), target.width,
                          target.height);
    }
    outWidth = target.width;
    outHeight = target.height;
    return true;
}

} // namespace mediamax::media
