// The media placeholder rasterizer (core media/MediaPlaceholder): the
// stand-in frame a media layer draws while its file is not on this system
// yet (pending media). The GPU side (upload into the layer's media texture
// and compositing) is covered by the render test "Media placeholder: ...".

#include <algorithm>

#include <doctest/doctest.h>

#include "mediamax/media/MediaPlaceholder.h"

using namespace mediamax;

TEST_SUITE("MediaPlaceholder")
{

namespace
{

int channelAt(const std::vector<std::uint8_t>& rgba, int w, int x, int y, int channel)
{
    return rgba[(static_cast<std::size_t>(y) * w + x) * 4 + channel];
}

// Counts the pixels that differ from a uniform (22, 24, 30) fill, i.e. the
// text coverage.
int nonFillPixels(const std::vector<std::uint8_t>& rgba, int w, int h)
{
    int count = 0;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            if (rgba[i + 0] != 22 || rgba[i + 1] != 24 || rgba[i + 2] != 30)
            {
                ++count;
            }
        }
    }
    return count;
}

} // namespace

TEST_CASE("placeholder renders a dark fill with centered text")
{
    const int w = 320, h = 180;
    std::vector<std::uint8_t> rgba;
    media::renderMediaPlaceholder(w, h, "clip.mp4", rgba);
    const std::size_t expectedSize = static_cast<std::size_t>(w) * h * 4;
    CHECK(rgba.size() == expectedSize);

    // The corners are the opaque dark fill.
    for (const auto [x, y] : {std::pair<int, int>{0, 0},
                              {w - 1, 0},
                              {0, h - 1},
                              {w - 1, h - 1}})
    {
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
        CHECK(rgba[i + 3] == 255);
        CHECK(rgba[i + 0] < 64); // dark
    }

    // The canvas carries text (a real amount of non-fill pixels: the file
    // name plus the hint line).
    CHECK(nonFillPixels(rgba, w, h) > 100);

    // The text block is vertically centered: the top and bottom margins of
    // the text region are equal.
    int firstTextRow = h, lastTextRow = -1;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            if (rgba[i + 0] != 22 || rgba[i + 1] != 24 || rgba[i + 2] != 30)
            {
                firstTextRow = std::min(firstTextRow, y);
                lastTextRow = std::max(lastTextRow, y);
                break;
            }
        }
    }
    REQUIRE(lastTextRow >= firstTextRow);
    const int textHeight = lastTextRow - firstTextRow + 1;
    const int expectedTop = (h - textHeight) / 2;
    CHECK(firstTextRow == expectedTop);
}

TEST_CASE("placeholder is deterministic")
{
    std::vector<std::uint8_t> a, b;
    media::renderMediaPlaceholder(320, 180, "clip.mp4", a);
    media::renderMediaPlaceholder(320, 180, "clip.mp4", b);
    CHECK(a == b);
    // A different file name renders different pixels.
    std::vector<std::uint8_t> c;
    media::renderMediaPlaceholder(320, 180, "another-clip.mp4", c);
    CHECK(a != c);
}

TEST_CASE("placeholder adapts the text scale to the canvas")
{
    // Same file name, two very different canvases: both render, and the
    // smaller canvas never carries more text pixels (the scale drops).
    std::vector<std::uint8_t> big, small;
    media::renderMediaPlaceholder(1920, 1080, "a-reasonably-named-clip.mp4", big);
    media::renderMediaPlaceholder(320, 180, "a-reasonably-named-clip.mp4", small);
    const std::size_t bigSize = 1920u * 1080u * 4u;
    const std::size_t smallSize = 320u * 180u * 4u;
    CHECK(big.size() == bigSize);
    CHECK(small.size() == smallSize);
    CHECK(nonFillPixels(small, 320, 180) > 0);
    CHECK(nonFillPixels(small, 320, 180) < nonFillPixels(big, 1920, 1080));
}

TEST_CASE("placeholder handles degenerate input")
{
    std::vector<std::uint8_t> rgba;

    // Non-positive sizes: empty output, no crash.
    media::renderMediaPlaceholder(0, 100, "x.mp4", rgba);
    CHECK(rgba.empty());
    media::renderMediaPlaceholder(100, 0, "x.mp4", rgba);
    CHECK(rgba.empty());

    // A very long file name is truncated, not an error: the output is the
    // requested size and still carries text.
    media::renderMediaPlaceholder(160, 90, std::string(500, 'f'), rgba);
    const std::size_t wideSize = 160u * 90u * 4u;
    CHECK(rgba.size() == wideSize);
    CHECK(nonFillPixels(rgba, 160, 90) > 0);

    // An empty file name falls back to the "media" label.
    media::renderMediaPlaceholder(160, 90, "", rgba);
    CHECK(rgba.size() == wideSize);
    CHECK(nonFillPixels(rgba, 160, 90) > 0);

    // A tiny canvas still fills (the scale clamps to 1, the text clips).
    media::renderMediaPlaceholder(8, 8, "clip.mp4", rgba);
    const std::size_t tinySize = 8u * 8u * 4u;
    CHECK(rgba.size() == tinySize);
}

} // TEST_SUITE("MediaPlaceholder")
