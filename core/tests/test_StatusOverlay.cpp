// Status overlay text rasterizer: font table integrity + layout.

#include <doctest/doctest.h>

#include "mediamax/overlay/StatusOverlay.h"

using namespace mediamax;

namespace
{

struct Raster
{
    std::vector<std::uint8_t> rgba;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

Raster render(const std::string& text, int scale)
{
    Raster r;
    std::string error;
    const bool ok = overlay::renderText(text, scale, r.rgba, r.width, r.height, error);
    REQUIRE(ok);
    REQUIRE(r.rgba.size() == static_cast<size_t>(r.width) * r.height * 4u);
    return r;
}

bool isOn(const Raster& r, std::uint32_t x, std::uint32_t y)
{
    if (x >= r.width || y >= r.height)
    {
        return false;
    }
    return r.rgba[(static_cast<size_t>(y) * r.width + x) * 4u + 3u] == 255;
}

size_t countOn(const Raster& r)
{
    size_t n = 0;
    for (std::uint32_t y = 0; y < r.height; ++y)
    {
        for (std::uint32_t x = 0; x < r.width; ++x)
        {
            if (isOn(r, x, y))
            {
                ++n;
            }
        }
    }
    return n;
}

// The status overlay shows IP addresses, names, "60.0 fps 1920x1080" and
// "frame 4.2 ms" style lines - everything in this string must render.
const std::string kStatusChars =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789"
    " .,/:()-[]{}=+?@%$#&*!\"'<>\\|~^_`";

} // namespace

TEST_SUITE("StatusOverlay")
{

TEST_CASE("Every printable ASCII glyph has ink")
{
    for (char c : kStatusChars)
    {
        if (c == ' ')
        {
            continue;
        }
        const Raster r = render(std::string(1, c), 1);
        INFO("glyph: ", c, " (", static_cast<int>(c), ")");
        CHECK(countOn(r) > 0);
    }
}

TEST_CASE("Space and out-of-range characters render blank")
{
    CHECK_EQ(countOn(render(" ", 1)), 0u);
    const Raster latin1 = render(std::string(1, '\xE9'), 1);
    CHECK_EQ(countOn(latin1), 0u);
    // Same canvas as a space (blank cell), so mixed text keeps its width.
    CHECK_EQ(latin1.width, render(" ", 1).width);
    CHECK_EQ(latin1.height, render(" ", 1).height);
}

TEST_CASE("Known glyph shapes (font table corruption check)")
{
    // '-': only the middle row, all five columns.
    {
        const Raster r = render("-", 1);
        CHECK_EQ(r.width, 6u);
        CHECK_EQ(r.height, 8u);
        for (std::uint32_t x = 0; x < 5u; ++x)
        {
            CHECK(isOn(r, x, 3));
        }
        CHECK_EQ(countOn(r), 5u);
    }
    // '=': rows 2 and 4 fully on, nothing else.
    {
        const Raster r = render("=", 1);
        CHECK_EQ(countOn(r), 10u);
        CHECK(isOn(r, 0, 2));
        CHECK(isOn(r, 4, 2));
        CHECK(isOn(r, 0, 4));
        CHECK(isOn(r, 4, 4));
        CHECK(!isOn(r, 2, 3));
    }
    // '1': the center column is on for all seven rows.
    {
        const Raster r = render("1", 1);
        for (std::uint32_t y = 0; y < 7u; ++y)
        {
            CHECK(isOn(r, 2, y));
        }
    }
    // '0': closed ring - the four corners of the 5x7 box are off, the
    // top/bottom middle and side middles are on.
    {
        const Raster r = render("0", 1);
        CHECK(!isOn(r, 0, 0));
        CHECK(!isOn(r, 4, 0));
        CHECK(!isOn(r, 0, 6));
        CHECK(!isOn(r, 4, 6));
        CHECK(isOn(r, 2, 0));
        CHECK(isOn(r, 2, 6));
        CHECK(isOn(r, 0, 3));
        CHECK(isOn(r, 4, 3));
    }
}

TEST_CASE("Canvas size follows the layout rules")
{
    // 2 chars * 6 px advance, 1 line * 8 px.
    CHECK_EQ(render("Hi", 1).width, 12u);
    CHECK_EQ(render("Hi", 1).height, 8u);
    // Two lines, longest 1 char.
    const Raster two = render("a\nb", 1);
    CHECK_EQ(two.width, 6u);
    CHECK_EQ(two.height, 16u);
    // Scale multiplies everything.
    const Raster s2 = render("Hi", 2);
    CHECK_EQ(s2.width, 24u);
    CHECK_EQ(s2.height, 16u);
    CHECK_EQ(countOn(s2), 4 * countOn(render("Hi", 1)));
}

TEST_CASE("Second line starts below the first")
{
    const Raster r = render("A\nB", 1);
    // 'A' occupies rows 0..6 of the first line.
    CHECK(isOn(r, 2, 3)); // A's middle bar
    // 'B' occupies rows 8..14 of the second line.
    CHECK(isOn(r, 0, 12)); // B's left edge, middle of line 2
    CHECK(!isOn(r, 0, 7)); // the 1-px line gap
}

TEST_CASE("Empty and all-blank input")
{
    Raster r;
    std::string error;
    std::uint32_t w = 99, h = 99;
    CHECK(overlay::renderText("", 1, r.rgba, w, h, error));
    CHECK_EQ(w, 0u);
    CHECK_EQ(h, 0u);
    CHECK(r.rgba.empty());
    CHECK(overlay::renderText("\n\n", 1, r.rgba, w, h, error));
    CHECK_EQ(w, 0u);
}

TEST_CASE("Invalid scale is an error")
{
    Raster r;
    std::string error;
    CHECK_FALSE(overlay::renderText("x", 0, r.rgba, r.width, r.height, error));
    CHECK(!error.empty());
}

TEST_CASE("Rendering is deterministic")
{
    const Raster a = render("System 192.168.1.5\nDisplay Main (1)\n60.0 fps  1280x720\nframe 3.2 ms  gpu 1.1 ms", 2);
    const Raster b = render("System 192.168.1.5\nDisplay Main (1)\n60.0 fps  1280x720\nframe 3.2 ms  gpu 1.1 ms", 2);
    CHECK(a.rgba == b.rgba);
    CHECK_EQ(a.width, b.width);
    CHECK_EQ(a.height, b.height);
}

TEST_CASE("Typical status text fits the expected canvas")
{
    // The playback app renders at scale 3 onto a 512x128 canvas; the
    // longest expected line ("frame 8.32 ms  gpu 2.41 ms" = 28 chars) must
    // fit: 28 * 6 * 3 = 504 <= 512, 4 lines * 8 * 3 = 96 <= 128.
    const Raster r = render("System 192.168.100.254\nDisplay: Extra (2)\n123.4 fps  3840x2160\nframe 8.32 ms  gpu 2.41 ms", 3);
    CHECK(r.width <= 512);
    CHECK(r.height <= 128);
}

} // TEST_SUITE
