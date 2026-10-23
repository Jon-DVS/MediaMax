// KeyframeTrack: interpolation and edge cases.

#include <doctest/doctest.h>

#include "mediamax/show/KeyframeTrack.h"

using namespace mediamax;

TEST_SUITE("KeyframeTrack")
{

TEST_CASE("Empty track returns zero")
{
    KeyframeTrack track;
    CHECK(track.empty());
    CHECK(track.valueAt(Time::fromSeconds(1.0)) == doctest::Approx(0.f));
}

TEST_CASE("Single key holds its value")
{
    KeyframeTrack track;
    track.setKey(Time::fromSeconds(1.0), 0.5f);
    CHECK(track.valueAt(Time::fromSeconds(0.0)) == doctest::Approx(0.5f)); // before: hold
    CHECK(track.valueAt(Time::fromSeconds(1.0)) == doctest::Approx(0.5f));
    CHECK(track.valueAt(Time::fromSeconds(9.0)) == doctest::Approx(0.5f)); // after: hold
}

TEST_CASE("Linear interpolation between keys")
{
    KeyframeTrack track;
    track.setKey(Time::fromSeconds(0.0), 0.f);
    track.setKey(Time::fromSeconds(1.0), 1.f);

    CHECK(track.valueAt(Time::fromSeconds(0.0)) == doctest::Approx(0.f));
    CHECK(track.valueAt(Time::fromSeconds(0.25)) == doctest::Approx(0.25f));
    CHECK(track.valueAt(Time::fromSeconds(0.5)) == doctest::Approx(0.5f));
    CHECK(track.valueAt(Time::fromSeconds(0.75)) == doctest::Approx(0.75f));
    CHECK(track.valueAt(Time::fromSeconds(1.0)) == doctest::Approx(1.f));
}

TEST_CASE("Step interpolation holds until the next key")
{
    KeyframeTrack track;
    track.setKey(Time::fromSeconds(0.0), 1.f, Interpolation::Step);
    track.setKey(Time::fromSeconds(1.0), 0.f, Interpolation::Step);

    CHECK(track.valueAt(Time::fromSeconds(0.99)) == doctest::Approx(1.f));
    CHECK(track.valueAt(Time::fromSeconds(1.0)) == doctest::Approx(0.f));
}

TEST_CASE("Mixed interpolation: per-key interpolation applies forward")
{
    KeyframeTrack track;
    track.setKey(Time::fromSeconds(0.0), 0.f, Interpolation::Step);
    track.setKey(Time::fromSeconds(1.0), 1.f, Interpolation::Linear);
    track.setKey(Time::fromSeconds(2.0), 2.f, Interpolation::Step);

    // 0..1: step from the first key -> stays 0
    CHECK(track.valueAt(Time::fromSeconds(0.5)) == doctest::Approx(0.f));
    // 1..2: linear from the second key -> ramps 1..2
    CHECK(track.valueAt(Time::fromSeconds(1.5)) == doctest::Approx(1.5f));
}

TEST_CASE("Keys stay sorted on insert and update")
{
    KeyframeTrack track;
    track.setKey(Time::fromSeconds(2.0), 2.f);
    track.setKey(Time::fromSeconds(0.0), 0.f);
    track.setKey(Time::fromSeconds(1.0), 1.f);
    track.setKey(Time::fromSeconds(1.0), 1.5f); // update in place

    CHECK(track.keyCount() == 3);
    CHECK(track.keyframes()[0].time == Time::fromSeconds(0.0));
    CHECK(track.keyframes()[1].time == Time::fromSeconds(1.0));
    CHECK(track.keyframes()[1].value == doctest::Approx(1.5f));
    CHECK(track.keyframes()[2].time == Time::fromSeconds(2.0));
}

TEST_CASE("Remove key")
{
    KeyframeTrack track;
    track.setKey(Time::fromSeconds(0.0), 0.f);
    track.setKey(Time::fromSeconds(1.0), 1.f);
    CHECK(track.removeKey(Time::fromSeconds(1.0)));
    CHECK_FALSE(track.removeKey(Time::fromSeconds(1.0)));
    CHECK(track.keyCount() == 1);
}

} // TEST_SUITE("KeyframeTrack")
