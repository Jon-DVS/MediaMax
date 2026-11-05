// The media deploy tracker (core net/MediaDeploy): the per-system transfer
// progress registry the control GUI's Systems tab renders. The transfer
// itself (net/MediaTransfer) is covered by the CommandServer transfer
// tests; here the bookkeeping is.

#include <doctest/doctest.h>

#include <thread>

#include "mediamax/net/MediaDeploy.h"

using namespace mediamax;

TEST_SUITE("MediaDeploy")
{

TEST_CASE("deploy entries track start order and unique handles")
{
    net::MediaDeployTracker tracker;
    const std::uint32_t a = tracker.begin("alpha", "a.mp4", 1000);
    const std::uint32_t b = tracker.begin("beta", "b.mp4", 2000);
    const std::uint32_t c = tracker.begin("alpha", "c.mp4", 3000);

    REQUIRE(a != b);
    REQUIRE(b != c);
    REQUIRE(a != c);

    const auto entries = tracker.entries();
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].handle == a);
    CHECK(entries[0].systemName == "alpha");
    CHECK(entries[0].fileName == "a.mp4");
    CHECK(entries[0].totalBytes == 1000);
    CHECK(entries[0].bytesSent == 0);
    CHECK(entries[0].state == "sending");
    CHECK(entries[1].handle == b);
    CHECK(entries[2].handle == c);
}

TEST_CASE("deploy progress is monotone and clamped to the total")
{
    net::MediaDeployTracker tracker;
    const std::uint32_t h = tracker.begin("alpha", "a.mp4", 1000);

    tracker.progress(h, 400);
    tracker.progress(h, 300); // a late, smaller report must not move back
    {
        const auto e = tracker.entries();
        CHECK(e[0].bytesSent == 400);
    }
    tracker.progress(h, 5000); // overshoot clamps to the total
    {
        const auto e = tracker.entries();
        CHECK(e[0].bytesSent == 1000);
    }

    // An unknown handle and a post-finish update are no-ops.
    tracker.finish(h);
    tracker.progress(h, 0);
    tracker.progress(9999, 500);
    const auto done = tracker.entries();
    CHECK(done[0].bytesSent == 1000);
    CHECK(done[0].state == "done");
}

TEST_CASE("deploy finish records success and failure")
{
    net::MediaDeployTracker tracker;
    const std::uint32_t ok = tracker.begin("alpha", "a.mp4", 1000);
    const std::uint32_t bad = tracker.begin("beta", "b.mp4", 2000);

    tracker.progress(ok, 700);
    tracker.finish(ok);
    tracker.progress(bad, 900);
    tracker.finish(bad, "connection lost");

    const auto entries = tracker.entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].state == "done");
    CHECK(entries[0].error.empty());
    CHECK(entries[0].bytesSent == 1000); // a success counts as fully sent
    CHECK(entries[1].state == "failed");
    CHECK(entries[1].error == "connection lost");
    CHECK(entries[1].bytesSent == 900); // the failure keeps the last report

    // The first outcome wins: a second finish is ignored.
    tracker.finish(bad, "another error");
    const auto after = tracker.entries();
    CHECK(after[1].error == "connection lost");
}

TEST_CASE("deploy clearFinished keeps the in-flight entries")
{
    net::MediaDeployTracker tracker;
    const std::uint32_t done = tracker.begin("alpha", "a.mp4", 10);
    const std::uint32_t live = tracker.begin("alpha", "b.mp4", 10);
    tracker.finish(done);

    tracker.clearFinished();
    const auto remaining = tracker.entries();
    REQUIRE(remaining.size() == 1);
    CHECK(remaining[0].handle == live);

    // reset drops everything, in flight included.
    tracker.reset();
    CHECK(tracker.entries().empty());
}

TEST_CASE("deploy clearSystem drops only the named system")
{
    net::MediaDeployTracker tracker;
    tracker.begin("alpha", "a.mp4", 10);
    tracker.begin("beta", "b.mp4", 10);
    tracker.begin("alpha", "c.mp4", 10);

    tracker.clearSystem("alpha");
    const auto remaining = tracker.entries();
    REQUIRE(remaining.size() == 1);
    CHECK(remaining[0].systemName == "beta");
}

TEST_CASE("deploy tracker survives concurrent progress reports")
{
    // The real usage: one worker thread per transfer reporting progress
    // while the UI thread takes snapshots every frame.
    net::MediaDeployTracker tracker;
    constexpr int kTransfers = 4;
    constexpr int kSteps = 25000;
    const std::uint64_t total = 1000000;
    std::vector<std::uint32_t> handles;
    for (int i = 0; i < kTransfers; ++i)
    {
        handles.push_back(tracker.begin("sys" + std::to_string(i), "f.mp4", total));
    }

    std::vector<std::thread> workers;
    for (int i = 0; i < kTransfers; ++i)
    {
        workers.emplace_back([&, i] {
            for (int step = 1; step <= kSteps; ++step)
            {
                tracker.progress(handles[static_cast<size_t>(i)],
                                 static_cast<std::uint64_t>(step) * (total / kSteps));
            }
            tracker.finish(handles[static_cast<size_t>(i)]);
        });
    }
    // Snapshot while the workers run (the UI's every-frame read); the
    // invariants must hold under the interleaving.
    for (int spin = 0; spin < 200; ++spin)
    {
        const auto snapshot = tracker.entries();
        for (const auto& e : snapshot)
        {
            const bool sending = e.state == "sending";
            const bool done = e.state == "done";
            const bool failed = e.state == "failed";
            const bool knownState = sending || done || failed;
            CHECK(knownState);
            CHECK(e.bytesSent <= total);
            if (done)
            {
                CHECK(e.bytesSent == total);
            }
        }
    }
    for (auto& w : workers)
    {
        w.join();
    }

    const auto finalEntries = tracker.entries();
    REQUIRE(finalEntries.size() == static_cast<size_t>(kTransfers));
    for (const auto& e : finalEntries)
    {
        CHECK(e.state == "done");
        CHECK(e.bytesSent == total);
    }
}

} // TEST_SUITE("MediaDeploy")
