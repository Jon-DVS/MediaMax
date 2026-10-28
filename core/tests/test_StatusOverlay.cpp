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
    // '-': rows 4 and 5 (the middle of the 11-row glyph), seven columns.
    {
        const Raster r = render("-", 1);
        CHECK_EQ(r.width, 9u);
        CHECK_EQ(r.height, 12u);
        for (std::uint32_t x = 0; x < 7u; ++x)
        {
            CHECK(isOn(r, x, 4));
            CHECK(isOn(r, x, 5));
        }
        CHECK_EQ(countOn(r), 14u);
    }
    // '=': rows 3-4 and 6-7 fully on, nothing between.
    {
        const Raster r = render("=", 1);
        CHECK_EQ(countOn(r), 28u);
        CHECK(isOn(r, 0, 3));
        CHECK(isOn(r, 6, 3));
        CHECK(isOn(r, 0, 7));
        CHECK(isOn(r, 6, 7));
        CHECK(!isOn(r, 3, 5));
    }
    // '1': column 3 is on for all ten rows of the cap height.
    {
        const Raster r = render("1", 1);
        for (std::uint32_t y = 0; y < 10u; ++y)
        {
            CHECK(isOn(r, 3, y));
        }
    }
    // '0': closed ring with a center dot (the dot tells it apart from
    // 'O') - the corners of the box are off, the top/bottom middle and
    // side middles are on, and the dot pixel is on.
    {
        const Raster r = render("0", 1);
        CHECK(!isOn(r, 0, 0));
        CHECK(!isOn(r, 7, 0));
        CHECK(!isOn(r, 0, 10));
        CHECK(!isOn(r, 7, 10));
        CHECK(isOn(r, 3, 0));
        CHECK(isOn(r, 4, 0));
        CHECK(isOn(r, 3, 9));
        CHECK(isOn(r, 4, 9));
        CHECK(isOn(r, 1, 5));
        CHECK(isOn(r, 6, 5));
        CHECK(isOn(r, 3, 4));
        // 'O' has no dot.
        const Raster o = render("O", 1);
        CHECK(!isOn(o, 3, 4));
    }
}

TEST_CASE("Canvas size follows the layout rules")
{
    // 2 chars * 9 px advance, 1 line * 12 px.
    CHECK_EQ(render("Hi", 1).width, 18u);
    CHECK_EQ(render("Hi", 1).height, 12u);
    // Two lines, longest 1 char.
    const Raster two = render("a\nb", 1);
    CHECK_EQ(two.width, 9u);
    CHECK_EQ(two.height, 24u);
    // Scale multiplies everything.
    const Raster s2 = render("Hi", 2);
    CHECK_EQ(s2.width, 36u);
    CHECK_EQ(s2.height, 24u);
    CHECK_EQ(countOn(s2), 4 * countOn(render("Hi", 1)));
}

TEST_CASE("Second line starts below the first")
{
    const Raster r = render("A\nB", 1);
    // 'A' occupies rows 0..10 of the first line; its crossbar is rows 4-5.
    CHECK(isOn(r, 2, 4));
    // 'B' occupies rows 12..22 of the second line.
    CHECK(isOn(r, 1, 17)); // B's left edge, middle of line 2
    CHECK(!isOn(r, 0, 11)); // the 1-px line gap
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
    // The playback app renders the "MediaMax" header at scale 2 plus a
    // 9-line body at scale 1 onto a canvas that is at least 256x160 (half
    // the display pixel size, clamped). The longest expected line (28
    // chars: "System 255.255.255.254:47600", the full 32-bit IP + port)
    // must fit: 28 * 9 = 252 <= 256; header + 4-px gap + 9 body lines:
    // 24 + 4 + 108 = 136 <= 160.
    const Raster header = render("MediaMax", 2);
    CHECK_EQ(header.width, 144u);
    CHECK_EQ(header.height, 24u);
    const Raster body = render("System 255.255.255.254:47600\nDisplay VeryLongName (255)\noff-screen\n3840x2160\n      now   min   max   avg\n"
                               "fps 360.0  60.0 60.2 59.8\ncpu 999.9  4.0 24.1 17.0\ngpu  99.9  1.8  8.3   2.4\nvid drop 0",
                               1);
    CHECK_EQ(body.width, 252u);
    CHECK_EQ(body.height, 108u);
    CHECK(body.width <= 256u);
    CHECK(header.height + 4u + body.height <= 160u);
}

} // TEST_SUITE
