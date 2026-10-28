// FrameDropCounter: the dropped-video-frames metric used by the media
// pump and shown in the display overlay / status protocol.

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>

#include "mediamax/playback/FrameDropCounter.h"
#include "mediamax/time/FrameRate.h"
#include "mediamax/time/Time.h"

using mediamax::FrameDropCounter;
using mediamax::FrameRate;
using mediamax::Time;

namespace
{

FrameRate fps(int num, int den = 1)
{
    return FrameRate::of(num, den);
}

// One frame step at the given rate.
Time step(const FrameRate& r)
{
    return Time::fromFrames(1, r);
}

Time at(const FrameRate& r, std::int64_t frame)
{
    return Time::fromFrames(frame, r);
}

} // namespace

TEST_SUITE("FrameDropCounter")
{
    TEST_CASE("steady state shows one frame per step: no drops")
    {
        const FrameRate r = fps(60);
        FrameDropCounter c;
        for (std::int64_t f = 0; f < 1000; ++f)
        {
            CHECK(c.advance(at(r, f), at(r, f), step(r)) == 0);
        }
        CHECK(c.total() == 0);
        CHECK(c.backlog() == 0);
    }

    TEST_CASE("the normal one-frame decode latency is not a drop")
    {
        // The display consistently shows the frame one step behind the
        // target (decode latency): the screen advances with the playhead,
        // so nothing is dropped.
        const FrameRate r = fps(30);
        FrameDropCounter c;
        for (std::int64_t f = 0; f < 100; ++f)
        {
            const std::optional<Time> disp = f > 0 ? std::optional<Time>(at(r, f - 1)) : std::nullopt;
            CHECK(c.advance(at(r, f), disp, step(r)) == 0);
        }
        CHECK(c.total() == 0);
        CHECK(c.backlog() == 1);
    }

    TEST_CASE("a stall (same frame on screen) counts one drop per step")
    {
        const FrameRate r = fps(60);
        FrameDropCounter c;
        c.advance(at(r, 0), at(r, 0), step(r));
        for (std::int64_t f = 1; f <= 10; ++f)
        {
            // The playhead runs on; the screen is stuck on frame 0.
            CHECK(c.advance(at(r, f), at(r, 0), step(r)) == 1);
        }
        CHECK(c.total() == 10);
        CHECK(c.backlog() == 10);
    }

    TEST_CASE("a decode slip counts the missed frames, catch-up re-earns nothing")
    {
        const FrameRate r = fps(30);
        FrameDropCounter c;
        c.advance(at(r, 0), at(r, 0), step(r));
        // Target runs three steps ahead while the screen shows nothing new
        // (the queue ran dry): frames 1-3 never make it on time.
        CHECK(c.advance(at(r, 1), std::nullopt, step(r)) == 1);
        CHECK(c.advance(at(r, 2), std::nullopt, step(r)) == 1);
        CHECK(c.advance(at(r, 3), std::nullopt, step(r)) == 1);
        // The decoder burst-catches up: the screen jumps to frame 3 (one
        // step late). No new drops.
        CHECK(c.advance(at(r, 4), at(r, 3), step(r)) == 0);
        CHECK(c.total() == 3);
        // Steady state afterwards stays clean.
        for (std::int64_t f = 5; f < 20; ++f)
        {
            CHECK(c.advance(at(r, f), at(r, f - 1), step(r)) == 0);
        }
        CHECK(c.total() == 3);
    }

    TEST_CASE("startup before the first frame is decoded is warm-up, not drops")
    {
        const FrameRate r = fps(60);
        FrameDropCounter c;
        // The playhead runs for a while with no frame displayed yet.
        for (std::int64_t f = 0; f <= 30; ++f)
        {
            CHECK(c.advance(at(r, f), std::nullopt, step(r)) == 0);
        }
        CHECK(c.total() == 0);
        // The first frame arrives (already behind: frames 0-29 were never
        // shown). The arrival itself is not counted...
        CHECK(c.advance(at(r, 30), at(r, 5), step(r)) == 0);
        // ...but the playhead running on while the screen is 25 frames
        // stale is.
        CHECK(c.advance(at(r, 31), at(r, 5), step(r)) == 1);
        CHECK(c.total() == 1);
    }

    TEST_CASE("a loop wrap (backward target jump) restarts the accounting")
    {
        const FrameRate r = fps(30);
        FrameDropCounter c;
        for (std::int64_t f = 0; f < 30; ++f)
        {
            c.advance(at(r, f), at(r, f), step(r));
        }
        CHECK(c.total() == 0);
        // Wrap: the target goes back to frame 0 (loop start).
        CHECK(c.advance(at(r, 0), at(r, 0), step(r)) == 0);
        // And runs on normally.
        for (std::int64_t f = 1; f < 10; ++f)
        {
            CHECK(c.advance(at(r, f), at(r, f), step(r)) == 0);
        }
        CHECK(c.total() == 0);
    }

    TEST_CASE("a user seek backwards is not counted as drops")
    {
        const FrameRate r = fps(30);
        FrameDropCounter c;
        for (std::int64_t f = 0; f < 300; ++f)
        {
            c.advance(at(r, f), at(r, f), step(r));
        }
        // Seek back 200 frames.
        CHECK(c.advance(at(r, 100), at(r, 100), step(r)) == 0);
        CHECK(c.total() == 0);
    }

    TEST_CASE("paused playback counts nothing")
    {
        const FrameRate r = fps(60);
        FrameDropCounter c;
        c.advance(at(r, 10), at(r, 10), step(r));
        for (int i = 0; i < 50; ++i)
        {
            CHECK(c.advance(at(r, 10), at(r, 10), step(r)) == 0);
        }
        CHECK(c.total() == 0);
        CHECK(c.backlog() == 0);
    }

    TEST_CASE("reset clears everything")
    {
        const FrameRate r = fps(60);
        FrameDropCounter c;
        c.advance(at(r, 0), at(r, 0), step(r));
        c.advance(at(r, 5), at(r, 0), step(r));
        CHECK(c.total() == 5);
        c.reset();
        CHECK(c.total() == 0);
        CHECK(c.backlog() == 0);
        // Clean after the reset.
        for (std::int64_t f = 0; f < 50; ++f)
        {
            CHECK(c.advance(at(r, f), at(r, f), step(r)) == 0);
        }
        CHECK(c.total() == 0);
    }

    TEST_CASE("non-integer frame rates (29.97) stay exact")
    {
        const FrameRate r = fps(30000, 1001);
        FrameDropCounter c;
        c.advance(at(r, 0), at(r, 0), step(r));
        // 1001 frames = exactly 30 seconds.
        for (std::int64_t f = 1; f <= 1001; ++f)
        {
            CHECK(c.advance(at(r, f), at(r, f), step(r)) == 0);
        }
        CHECK(c.total() == 0);
        // Stall for 3 steps.
        for (std::int64_t f = 1002; f <= 1004; ++f)
        {
            CHECK(c.advance(at(r, f), at(r, 1001), step(r)) == 1);
        }
        CHECK(c.total() == 3);
    }

} // TEST_SUITE("FrameDropCounter")
