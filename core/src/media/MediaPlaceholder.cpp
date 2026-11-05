#include "mediamax/media/MediaPlaceholder.h"

#include <algorithm>

#include "mediamax/overlay/StatusOverlay.h"

namespace mediamax::media
{

namespace
{

// The placeholder colors: a dark slate fill; the file name in white, the
// hint line in a mid grey (so the name reads as primary).
constexpr int kFillR = 22, kFillG = 24, kFillB = 30;
constexpr int kNameGray = 255;
constexpr int kHintGray = 150;

// The fixed hint under the file name (ASCII only: the overlay font covers
// 32..126).
constexpr const char* kHintText = "media not available on this system";

// The file name, middle-truncated to `maxChars` (the font has no ellipsis
// glyph): "abcdef...uvwxyz".
std::string truncateMiddle(const std::string& in, std::size_t maxChars)
{
    if (in.size() <= maxChars)
    {
        return in;
    }
    const std::size_t keep = maxChars - 3;
    const std::size_t head = keep / 2;
    return in.substr(0, head) + "..." + in.substr(in.size() - (keep - head));
}

// Blits a tightly packed RGBA8 glyph canvas (white glyphs on transparent,
// straight alpha) into `dst` at (x, y), tinting the glyph RGB to `tint`
// while keeping the glyph alpha. The destination fill is opaque, so the
// composite is a straight over onto (tint*a, tint*a, tint*a, 255). Pixels
// outside the destination are clipped.
void blitText(std::vector<std::uint8_t>& dst, int dstW, int dstH,
              const std::vector<std::uint8_t>& src, int srcW, int srcH, int x, int y, int tint)
{
    for (int sy = 0; sy < srcH; ++sy)
    {
        const int dy = y + sy;
        if (dy < 0 || dy >= dstH)
        {
            continue;
        }
        for (int sx = 0; sx < srcW; ++sx)
        {
            const int dx = x + sx;
            if (dx < 0 || dx >= dstW)
            {
                continue;
            }
            const std::size_t si = (static_cast<std::size_t>(sy) * srcW + sx) * 4;
            const std::uint8_t a = src[si + 3];
            if (a == 0)
            {
                continue;
            }
            const std::size_t di = (static_cast<std::size_t>(dy) * dstW + dx) * 4;
            const int level = tint * a / 255;
            dst[di + 0] = static_cast<std::uint8_t>(level);
            dst[di + 1] = static_cast<std::uint8_t>(level);
            dst[di + 2] = static_cast<std::uint8_t>(level);
            dst[di + 3] = 255;
        }
    }
}

// The integer glyph scale that fits `text` (a single line) into `maxWidth`
// px (renderText's advance is 9 px per character at scale 1), clamped to
// [1, maxScale].
int fitScale(const std::string& text, int maxWidth, int maxScale)
{
    const int widthAt1 = static_cast<int>(text.size()) * 9;
    int scale = widthAt1 > 0 ? maxWidth / widthAt1 : 1;
    scale = std::max(scale, 1);
    scale = std::min(scale, maxScale);
    return scale;
}

} // namespace

void renderMediaPlaceholder(int width, int height, const std::string& fileName,
                            std::vector<std::uint8_t>& outRgba)
{
    outRgba.clear();
    if (width <= 0 || height <= 0)
    {
        return;
    }
    const std::string name =
        truncateMiddle(fileName.empty() ? std::string("media") : fileName, 80);

    // The dark fill (opaque).
    outRgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (std::size_t i = 0; i < outRgba.size() / 4; ++i)
    {
        outRgba[i * 4 + 0] = static_cast<std::uint8_t>(kFillR);
        outRgba[i * 4 + 1] = static_cast<std::uint8_t>(kFillG);
        outRgba[i * 4 + 2] = static_cast<std::uint8_t>(kFillB);
        outRgba[i * 4 + 3] = 255;
    }

    // The text block: the file name and the hint line, centered as a block.
    // The scale fits both lines across the canvas and grows with the canvas
    // (never finer than the scene needs: capped at height / 80).
    const int maxScale = std::max(1, height / 80);
    const int scale = std::min(fitScale(name, width, maxScale), fitScale(kHintText, width, maxScale));

    std::string error;
    std::vector<std::uint8_t> nameRgba;
    std::uint32_t nameW = 0, nameH = 0;
    std::vector<std::uint8_t> hintRgba;
    std::uint32_t hintW = 0, hintH = 0;
    if (!overlay::renderText(name, scale, nameRgba, nameW, nameH, error) ||
        !overlay::renderText(kHintText, scale, hintRgba, hintW, hintH, error))
    {
        return; // an unrasterizable name still leaves the dark fill
    }

    const int gap = 4 * scale; // between the name and the hint
    const int blockH = static_cast<int>(nameH) + gap + static_cast<int>(hintH);
    const int top = (height - blockH) / 2;
    blitText(outRgba, width, height, nameRgba, static_cast<int>(nameW), static_cast<int>(nameH),
             (width - static_cast<int>(nameW)) / 2, top, kNameGray);
    blitText(outRgba, width, height, hintRgba, static_cast<int>(hintW), static_cast<int>(hintH),
             (width - static_cast<int>(hintW)) / 2, top + static_cast<int>(nameH) + gap, kHintGray);
}

} // namespace mediamax::media
