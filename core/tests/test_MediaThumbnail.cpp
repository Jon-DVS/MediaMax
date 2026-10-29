// Media thumbnails: representative-frame decode + aspect-fit scaling of a
// procedural source (deterministic pixels), and the clean error paths.

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mediamax/media/MediaThumbnail.h"
#include "mediamax/media/TestPatternSource.h"

using namespace mediamax;

namespace
{

// A source factory fixed on one pattern, so each case gets an identical
// fresh instance (the thumbnail function opens its own instance).
MediaSourceFactory patternFactory(std::uint32_t w, std::uint32_t h, Time duration,
                                  TestPatternKind kind, Color4 solid = {})
{
    return [w, h, duration, kind, solid](const std::string&, std::string& error) {
        (void)error;
        return std::make_shared<TestPatternSource>(w, h, FrameRate::fps30(), duration, kind,
                                                   solid);
    };
}

MediaMetadata patternMetadata(std::uint32_t w, std::uint32_t h, Time duration)
{
    MediaMetadata md;
    md.type = MediaType::Video;
    md.hasVideo = true;
    md.width = w;
    md.height = h;
    md.duration = duration;
    md.frameRate = FrameRate::fps30();
    return md;
}

} // namespace

TEST_SUITE("MediaThumbnail")
{

TEST_CASE("solid pattern scales aspect-fit without upscaling")
{
    // 320x160 (2:1) source into a 160x160 box -> 160x80.
    const Color4 solid{0.25f, 0.5f, 0.75f, 1.f};
    const auto factory = patternFactory(320, 160, Time::fromSeconds(4.0),
                                        TestPatternKind::Solid, solid);
    const auto md = patternMetadata(320, 160, Time::fromSeconds(4.0));

    std::vector<std::uint8_t> rgba;
    int w = 0;
    int h = 0;
    std::string error;
    REQUIRE(media::makeThumbnail(factory, "pattern", md, 160, 160, rgba, w, h, error));
    CHECK(w == 160);
    CHECK(h == 80);
    CHECK(rgba.size() == static_cast<std::size_t>(w) * h * 4);

    // Bilinear on a constant is the constant: every pixel equals the source
    // color (0.25 -> 64, 0.5 -> 128, 0.75 -> 191, alpha 255).
    const auto* p = rgba.data();
    CHECK(p[0] == 64);
    CHECK(p[1] == 128);
    CHECK(p[2] == 191);
    CHECK(p[3] == 255);
    const std::size_t mid = (static_cast<std::size_t>(w) * (h / 2) + w / 2) * 4;
    CHECK(rgba[mid] == 64);
    CHECK(rgba[mid + 1] == 128);
    CHECK(rgba[mid + 2] == 191);
    CHECK(rgba[mid + 3] == 255);
}

TEST_CASE("small assets are not upscaled")
{
    const auto factory = patternFactory(64, 32, Time::fromSeconds(4.0),
                                        TestPatternKind::Solid, {0.f, 0.f, 0.f, 1.f});
    const auto md = patternMetadata(64, 32, Time::fromSeconds(4.0));

    std::vector<std::uint8_t> rgba;
    int w = 0;
    int h = 0;
    std::string error;
    REQUIRE(media::makeThumbnail(factory, "pattern", md, 160, 160, rgba, w, h, error));
    CHECK(w == 64);
    CHECK(h == 32);
    CHECK(rgba.size() == static_cast<std::size_t>(64) * 32 * 4);
}

TEST_CASE("video seeks to 10% of the duration (capped at 5 s)")
{
    // 32x32 checkerboard: the whole frame is one 32px cell, so the frame
    // color flips with the pattern's 10-frame motion offset. Duration 4 s
    // -> seek lands on frame 12 (offset 1 -> black); without the seek the
    // first frame (offset 0 -> white) would come back instead.
    const auto factory = patternFactory(32, 32, Time::fromSeconds(4.0),
                                        TestPatternKind::Checkerboard);
    const auto md = patternMetadata(32, 32, Time::fromSeconds(4.0));

    std::vector<std::uint8_t> rgba;
    int w = 0;
    int h = 0;
    std::string error;
    REQUIRE(media::makeThumbnail(factory, "pattern", md, 160, 160, rgba, w, h, error));
    CHECK(w == 32);
    CHECK(h == 32);
    const std::size_t mid = (static_cast<std::size_t>(w) * 16 + 16) * 4;
    CHECK(rgba[mid] == 0);      // black cell: the seek happened
    CHECK(rgba[mid + 1] == 0);
    CHECK(rgba[mid + 2] == 0);
    CHECK(rgba[mid + 3] == 255); // alpha is always opaque

    // A long clip (10 min) seeks to the 5 s cap, not to 60 s: frame 150
    // (offset 15 -> odd -> black as well, but the point is it decodes fast
    // and lands past the first cell cycle, which frame 0 would not).
    const auto longFactory = patternFactory(32, 32, Time::fromSeconds(600.0),
                                            TestPatternKind::Checkerboard);
    const auto longMd = patternMetadata(32, 32, Time::fromSeconds(600.0));
    std::vector<std::uint8_t> rgba2;
    int w2 = 0;
    int h2 = 0;
    std::string error2;
    REQUIRE(media::makeThumbnail(longFactory, "pattern", longMd, 160, 160, rgba2, w2, h2,
                                 error2));
    const std::size_t mid2 = (static_cast<std::size_t>(w2) * 16 + 16) * 4;
    CHECK(rgba2[mid2] == 0); // frame 150: offset 15, still black
}

TEST_CASE("colorbars keep their structure after scaling")
{
    // 280x160 color bars into 160x160 -> 160x91 (aspect fit). The seven
    // bars are vertical, so a center-row scan still crosses white first and
    // blue last.
    const auto factory = patternFactory(280, 160, Time::fromSeconds(4.0),
                                        TestPatternKind::ColorBars);
    const auto md = patternMetadata(280, 160, Time::fromSeconds(4.0));

    std::vector<std::uint8_t> rgba;
    int w = 0;
    int h = 0;
    std::string error;
    REQUIRE(media::makeThumbnail(factory, "pattern", md, 160, 160, rgba, w, h, error));
    CHECK(w == 160);
    CHECK(h == 91);

    const int row = h / 2;
    const std::uint8_t* left = rgba.data() + (static_cast<std::size_t>(row) * w) * 4;
    const std::uint8_t* right =
        rgba.data() + (static_cast<std::size_t>(row) * w + static_cast<std::size_t>(w - 1)) * 4;
    CHECK(left[0] == 255); // first bar is white
    CHECK(left[1] == 255);
    CHECK(left[2] == 255);
    CHECK(right[0] == 0);  // last bar is blue
    CHECK(right[1] == 0);
    CHECK(right[2] == 255);
}

TEST_CASE("audio-only media is rejected with a clean error")
{
    const auto factory = patternFactory(64, 32, Time::fromSeconds(4.0),
                                        TestPatternKind::Solid);
    MediaMetadata md;
    md.type = MediaType::Audio;
    md.hasAudio = true;

    std::vector<std::uint8_t> rgba;
    int w = 0;
    int h = 0;
    std::string error;
    CHECK_FALSE(media::makeThumbnail(factory, "audio", md, 160, 160, rgba, w, h, error));
    CHECK(error.find("no video content") != std::string::npos);
    CHECK(rgba.empty());
    CHECK(w == 0);
}

TEST_CASE("missing factory or failing source are rejected with a clean error")
{
    const auto md = patternMetadata(64, 32, Time::fromSeconds(4.0));
    std::vector<std::uint8_t> rgba;
    int w = 0;
    int h = 0;
    std::string error;

    // No factory installed at all.
    MediaSourceFactory none;
    CHECK_FALSE(media::makeThumbnail(none, "x", md, 160, 160, rgba, w, h, error));
    CHECK(error.find("no source factory") != std::string::npos);

    // A factory that cannot open the path.
    const MediaSourceFactory failing = [](const std::string&, std::string& error) {
        error = "disk on fire";
        return std::shared_ptr<IMediaSource>{};
    };
    error.clear();
    CHECK_FALSE(media::makeThumbnail(failing, "x", md, 160, 160, rgba, w, h, error));
    CHECK(error == "disk on fire");
}

TEST_CASE("invalid request sizes are rejected")
{
    const auto factory = patternFactory(64, 32, Time::fromSeconds(4.0),
                                        TestPatternKind::Solid);
    const auto md = patternMetadata(64, 32, Time::fromSeconds(4.0));
    std::vector<std::uint8_t> rgba;
    int w = 0;
    int h = 0;
    std::string error;
    CHECK_FALSE(media::makeThumbnail(factory, "x", md, 0, 160, rgba, w, h, error));
    CHECK_FALSE(media::makeThumbnail(factory, "x", md, 160, -5, rgba, w, h, error));
}

} // TEST_SUITE("MediaThumbnail")
