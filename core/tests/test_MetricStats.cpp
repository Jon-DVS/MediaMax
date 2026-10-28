// MetricStats: rolling current/min/max/avg over a fixed sample window.

#include <doctest/doctest.h>

#include "mediamax/playback/MetricStats.h"

using namespace mediamax;

TEST_SUITE("MetricStats")
{

TEST_CASE("Empty stats report zero")
{
    const MetricStats stats;
    CHECK_EQ(stats.count(), 0u);
    CHECK(stats.current() == 0.0);
    CHECK(stats.min() == 0.0);
    CHECK(stats.max() == 0.0);
    CHECK(stats.avg() == 0.0);
}

TEST_CASE("Filling window tracks all samples")
{
    MetricStats stats(10);
    stats.record(2.0);
    stats.record(4.0);
    stats.record(6.0);
    CHECK_EQ(stats.count(), 3u);
    CHECK(stats.current() == 6.0);
    CHECK(stats.min() == 2.0);
    CHECK(stats.max() == 6.0);
    CHECK(stats.avg() == 4.0);
}

TEST_CASE("A spike ages out of the window")
{
    // window 3: the 100x spike must stop being the max once three more
    // samples push it out of the window.
    MetricStats stats(3);
    stats.record(100.0);
    CHECK(stats.max() == 100.0);
    stats.record(1.0);
    stats.record(1.0);
    CHECK(stats.max() == 100.0); // [100, 1, 1]
    stats.record(1.0);
    CHECK(stats.max() == 1.0); // [1, 1, 1]
    CHECK(stats.min() == 1.0);
    CHECK(stats.avg() == 1.0);
    CHECK_EQ(stats.count(), 4u); // count is lifetime, not the window
}

TEST_CASE("Average follows the sliding window")
{
    MetricStats stats(4);
    for (double v = 1.0; v <= 4.0; ++v)
    {
        stats.record(v);
    }
    CHECK(stats.avg() == 2.5); // [1, 2, 3, 4]
    stats.record(5.0);
    CHECK(stats.avg() == 3.5); // [2, 3, 4, 5]
    stats.record(6.0);
    CHECK(stats.avg() == 4.5); // [3, 4, 5, 6]
}

TEST_CASE("Current tracks the newest sample")
{
    MetricStats stats(2);
    stats.record(0.0);
    CHECK(stats.current() == 0.0);
    stats.record(1.0);
    CHECK(stats.current() == 1.0);
    stats.record(0.0); // wraps the ring
    CHECK(stats.current() == 0.0);
    stats.record(7.0);
    CHECK(stats.current() == 7.0);
}

TEST_CASE("Window of one holds only the latest sample")
{
    MetricStats stats(1);
    stats.record(5.0);
    stats.record(7.0);
    CHECK_EQ(stats.count(), 2u);
    CHECK(stats.current() == 7.0);
    CHECK(stats.min() == 7.0);
    CHECK(stats.max() == 7.0);
    CHECK(stats.avg() == 7.0);
}

TEST_CASE("Fractional values are kept exactly")
{
    MetricStats stats(3);
    stats.record(16.25);
    stats.record(15.75);
    stats.record(17.125);
    CHECK(stats.min() == 15.75);
    CHECK(stats.max() == 17.125);
    CHECK(stats.avg() == (16.25 + 15.75 + 17.125) / 3.0);
}

TEST_CASE("A zero window is clamped to one")
{
    MetricStats stats(0);
    stats.record(3.0);
    stats.record(9.0);
    CHECK(stats.max() == 9.0);
    CHECK(stats.min() == 9.0);
}

} // TEST_SUITE
