// Time and FrameRate: exact rational time math.

#include <cmath>

#include <doctest/doctest.h>

#include "TestFixtures.h" // Time StringMaker
#include "mediamax/time/Timecode.h"

using namespace mediamax;

TEST_SUITE("Time")
{

TEST_CASE("FrameRate: presets reduce to lowest terms")
{
    const FrameRate fps2997 = FrameRate::fps2997();
    CHECK(fps2997.numerator() == 30000);
    CHECK(fps2997.denominator() == 1001);
    CHECK(fps2997.toString() == "29.97");

    const FrameRate fps30 = FrameRate::fps30();
    CHECK(fps30.toString() == "30");
    CHECK(fps30.isInteger());

    const FrameRate fps23976 = FrameRate::fps23976();
    CHECK(fps23976.numerator() == 24000);
    CHECK(fps23976.denominator() == 1001);
    CHECK(fps23976.toString() == "23.98");
}

TEST_CASE("FrameRate: of() reduces fractions")
{
    const FrameRate fps = FrameRate::of(60, 2);
    CHECK(fps.numerator() == 30);
    CHECK(fps.denominator() == 1);
}

TEST_CASE("FrameRate: nominal frames per second")
{
    CHECK(FrameRate::fps2997().nominalFramesPerSecond() == 30);
    CHECK(FrameRate::fps30().nominalFramesPerSecond() == 30);
    CHECK(FrameRate::fps25().nominalFramesPerSecond() == 25);
    CHECK(FrameRate::fps5994().nominalFramesPerSecond() == 60);
}

TEST_CASE("Time: fromMicroseconds is exact")
{
    const Time t = Time::fromMicroseconds(1500);
    CHECK(t.toMicroseconds() == 1500);
    CHECK(t.toSeconds() == doctest::Approx(0.0015));
}

TEST_CASE("Time: fromFrames is exact round trip")
{
    const FrameRate fps = FrameRate::fps2997();
    for (std::int64_t f = 0; f <= 10000; f += 997)
    {
        const Time t = Time::fromFrames(f, fps);
        CHECK(t.toFrames(fps) == f);
    }
}

TEST_CASE("Time: no accumulated drift over 100000 frames at 29.97")
{
    const FrameRate fps = FrameRate::fps2997();
    // 100000 frames is exactly 100000*1001/30000 seconds.
    const Time t = Time::fromFrames(100000, fps);
    const double expected = 100000.0 * 1001.0 / 30000.0;
    CHECK(t.toSeconds() == doctest::Approx(expected).epsilon(1e-12));
    CHECK(t.toFrames(fps) == 100000);
}

TEST_CASE("Time: arithmetic is exact")
{
    const FrameRate fps = FrameRate::fps30();
    const Time a = Time::fromFrames(10, fps);
    const Time b = Time::fromFrames(3, fps);

    const Time sum = a + b;
    CHECK(sum.toFrames(fps) == 13);

    const Time diff = a - b;
    CHECK(diff.toFrames(fps) == 7);

    CHECK(a + b == Time::fromFrames(13, fps));
    CHECK(a - b == Time::fromFrames(7, fps));
}

TEST_CASE("Time: subtraction stays exact (no double fallback)")
{
    // Regression: the overflow guard in operator+ misdetected overflow for
    // negative operands, silently falling back to double/microsecond
    // precision on every subtraction. Frame-exact values must survive.
    const FrameRate fps = FrameRate::fps30();
    const Time a = Time::fromFrames(10, fps);
    const Time b = Time::fromFrames(3, fps);
    CHECK(a - b == Time(7, 30));

    // A reduced numerator of 1 must not trip the guard either (INT64_MIN / -1
    // raises a hardware divide error).
    const Time c = Time(1, 10);
    const Time d = Time(1, 3);
    CHECK(d - c == Time(7, 30));
    CHECK(c - d == Time(-7, 30));
}

TEST_CASE("Time: absurd magnitudes fall back to double instead of overflowing")
{
    // 3037000501^2 overflows int64, so the addition must fall back to the
    // double/microsecond path (the documented precision boundary) instead of
    // doing undefined integer math. The value stays well above the 0.5 us
    // rounding boundary so the fallback result is still meaningful.
    const Time huge = Time(1000000, 3037000501LL); // ~0.33 ms
    const Time sum = huge + huge;
    CHECK(sum.isValid());
    CHECK(sum.toSeconds() == doctest::Approx(2000000.0 / 3037000501.0).epsilon(1e-3));
}

TEST_CASE("Time: addition across frame rates stays exact")
{
    const Time a = Time::fromFrames(10, FrameRate::fps25()); // 10/25 = 2/5 s
    const Time b = Time::fromFrames(10, FrameRate::fps30()); // 10/30 = 1/3 s
    const Time sum = a + b;
    // 2/5 + 1/3 = 11/15 s
    CHECK(sum == Time(11, 15));
    CHECK(sum.toFrames(FrameRate::fps30()) == 22);
}

TEST_CASE("Time: comparisons")
{
    const Time a = Time::fromFrames(10, FrameRate::fps25());
    const Time b = Time::fromFrames(10, FrameRate::fps30());
    // 10/25 = 0.4 s > 10/30 = 0.333 s
    CHECK(a > b);
    CHECK(b < a);
    CHECK(a != b);
    CHECK(a == Time::fromSeconds(0.4));
}

TEST_CASE("Time: clamp and min/max")
{
    const Time a = Time::fromSeconds(5.0);
    CHECK(a.clamp(Time::fromSeconds(1.0), Time::fromSeconds(3.0)) ==
          Time::fromSeconds(3.0));
    CHECK(a.clamp(Time::fromSeconds(1.0), Time::fromSeconds(10.0)) ==
          Time::fromSeconds(5.0));
    CHECK(Time::min(a, Time::fromSeconds(2.0)) == Time::fromSeconds(2.0));
    CHECK(Time::max(a, Time::fromSeconds(2.0)) == a);
}

TEST_CASE("Time: modulo (loop wrap)")
{
    const Time duration = Time::fromSeconds(4.0);
    const Time t = Time::fromSeconds(9.0);
    CHECK((t % duration) == Time::fromSeconds(1.0));
    CHECK((Time::fromSeconds(4.0) % duration) == Time::zero());
    CHECK((Time::fromSeconds(3.9) % duration) == Time::fromSeconds(3.9));
}

TEST_CASE("Time: multiplication by rate")
{
    const Time t = Time::fromSeconds(1.0);
    const Time doubled = t * 2.0;
    CHECK(doubled == Time::fromSeconds(2.0));
    CHECK((t * 0.5) == Time::fromSeconds(0.5));
}

TEST_CASE("Time: timecode formatting (non-drop)")
{
    const FrameRate fps30 = FrameRate::fps30();
    CHECK(Time::zero().toTimecode(fps30) == "00:00:00:00");
    CHECK(Time::fromFrames(1, fps30).toTimecode(fps30) == "00:00:00:01");
    CHECK(Time::fromFrames(29, fps30).toTimecode(fps30) == "00:00:00:29");
    CHECK(Time::fromFrames(30, fps30).toTimecode(fps30) == "00:00:01:00");
    CHECK(Time::fromFrames(30 * 60 + 15, fps30).toTimecode(fps30) == "00:01:00:15");
    CHECK(Time::fromFrames(30 * 3600 + 30 * 60 + 2, fps30).toTimecode(fps30) == "01:01:00:02");
}

TEST_CASE("Time: timecode at 25 fps uses 25 frames per second")
{
    const FrameRate fps25 = FrameRate::fps25();
    CHECK(Time::fromFrames(24, fps25).toTimecode(fps25) == "00:00:00:24");
    CHECK(Time::fromFrames(25, fps25).toTimecode(fps25) == "00:00:01:00");
}

TEST_CASE("Time: negative times")
{
    const Time t = Time::fromSeconds(-1.5);
    CHECK(t.isNegative());
    CHECK(t.toSeconds() == doctest::Approx(-1.5));
    CHECK(t.toTimecode(FrameRate::fps30()) == "-00:00:00:00");
}

TEST_CASE("Time: fromSeconds microsecond precision boundary")
{
    // 0.1 s is not exactly representable in binary; fromSeconds must land on
    // the nearest microsecond, not drift.
    const Time t = Time::fromSeconds(0.1);
    CHECK(t.toMicroseconds() == 100000);
    CHECK(t.toSeconds() == doctest::Approx(0.1).epsilon(1e-9));
}

TEST_CASE("Time: wire timecode round trip matches the engine for every standard rate")
{
    const FrameRate rates[] = {FrameRate::fps24(),  FrameRate::fps25(),  FrameRate::fps2997(),
                               FrameRate::fps30(),  FrameRate::fps50(),  FrameRate::fps5994(),
                               FrameRate::fps60()};
    for (const auto& fps : rates)
    {
        const std::int64_t nominal = fps.nominalFramesPerSecond();
        const std::int64_t probes[] = {0, 1, nominal - 1, nominal, nominal + 1,
                                       nominal * 60 + 15,
                                       nominal * 3600 + nominal * 60 + 2,
                                       nominal * (3600 * 99 + 59 * 60 + 59) + (nominal - 1)};
        for (const std::int64_t frames : probes)
        {
            const Time t = Time::fromFrames(frames, fps);
            const std::string tc = t.toTimecode(fps);
            CHECK_EQ(timecodeToFrames(tc, nominal), frames);
            CHECK_EQ(framesToTimecode(frames, nominal), tc);
        }
    }
}

TEST_CASE("Time: wire timecode parsing rejects malformed strings")
{
    CHECK_EQ(timecodeToFrames("?", 30), -1);
    CHECK_EQ(timecodeToFrames("", 30), -1);
    CHECK_EQ(timecodeToFrames("00:00:00", 30), -1);
    CHECK_EQ(timecodeToFrames("00:00:00:00:00", 30), -1);
    CHECK_EQ(timecodeToFrames("00:00:00x00", 30), -1);
    CHECK_EQ(timecodeToFrames("-00:00:00:00", 30), -1);
    CHECK_EQ(timecodeToFrames("00:60:00:00", 30), -1);
    CHECK_EQ(timecodeToFrames("00:00:60:00", 30), -1);
    CHECK_EQ(timecodeToFrames("00:00:00:30", 30), -1); // ff must be < nominal
    CHECK_EQ(timecodeToFrames("00:00:00:00", 0), -1);
    CHECK_EQ(timecodeToFrames("00:00:00:00", -1), -1);
    CHECK_EQ(framesToTimecode(-1, 30), std::string("-00:00:00:00"));
    CHECK_EQ(framesToTimecode(0, 30), std::string("00:00:00:00"));
    CHECK_EQ(framesToTimecode(30 * 3661, 30), std::string("01:01:01:00"));
}

TEST_CASE("Time: frame rate string parsing recovers the standard rates")
{
    const FrameRate rates[] = {FrameRate::fps24(),  FrameRate::fps25(),  FrameRate::fps2997(),
                               FrameRate::fps30(),  FrameRate::fps50(),  FrameRate::fps5994(),
                               FrameRate::fps60()};
    for (const auto& fps : rates)
    {
        double parsed = 0.0;
        std::int64_t nominal = 0;
        CHECK(parseFrameRateString(fps.toString(), parsed, nominal));
        CHECK_EQ(nominal, fps.nominalFramesPerSecond());
        // The parsed double matches the exact rate within the string's
        // 2-decimal resolution.
        CHECK(parsed > fps.framesPerSecond() - 0.005);
        CHECK(parsed < fps.framesPerSecond() + 0.005);
    }
}

TEST_CASE("Time: frame rate string parsing rejects malformed rates")
{
    double parsed = 0.0;
    std::int64_t nominal = 0;
    CHECK_FALSE(parseFrameRateString("?", parsed, nominal));
    CHECK_FALSE(parseFrameRateString("", parsed, nominal));
    CHECK_FALSE(parseFrameRateString("0", parsed, nominal));
    CHECK_FALSE(parseFrameRateString("-5", parsed, nominal));
    CHECK_FALSE(parseFrameRateString("inf", parsed, nominal));
}

TEST_CASE("Time: the parsed frame rate is accurate enough for sub-poll extrapolation")
{
    // The control GUI re-anchors on the engine's timecode at most one poll
    // period (500 ms) apart: the frame error from the parsed double rate
    // over such a delta must stay a tiny fraction of a frame.
    const FrameRate rates[] = {FrameRate::fps24(),  FrameRate::fps25(),  FrameRate::fps2997(),
                               FrameRate::fps30(),  FrameRate::fps50(),  FrameRate::fps5994(),
                               FrameRate::fps60()};
    for (const auto& fps : rates)
    {
        double parsed = 0.0;
        std::int64_t nominal = 0;
        CHECK(parseFrameRateString(fps.toString(), parsed, nominal));
        const double delta = 0.5; // one poll period
        CHECK(std::fabs(delta * parsed - delta * fps.framesPerSecond()) < 1e-3);
    }
}

} // TEST_SUITE("Time")
