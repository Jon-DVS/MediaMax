// PlaybackEngine: transport state machine, cue evaluation, media scheduling,
// overrides. The engine's visual frame state is a pure function of time -
// these tests pin down that contract.

#include <algorithm>

#include <doctest/doctest.h>

#include "mediamax/playback/PlaybackEngine.h"

#include "TestFixtures.h"

using namespace mediamax;
using test::makeLoadedMediaLibrary;
using test::makeTestEngine;
using test::makeTestShow;

TEST_SUITE("PlaybackEngine")
{

TEST_CASE("Initial state: stopped at zero, first cue scene active")
{
    auto media = makeLoadedMediaLibrary();
    auto engine = makeTestEngine(media);

    CHECK(engine.state() == PlayState::Stopped);
    CHECK(engine.time() == Time::zero());

    const FrameState state = engine.frameState();
    CHECK(state.showTime == Time::zero());
    // The PlayScene(1) cue fires at t=0 (cues apply at or before t).
    CHECK(state.activeSceneId == 1);
    CHECK(state.layers.size() == 2); // color layer + video layer
}

TEST_CASE("Play / pause / stop")
{
    auto media = makeLoadedMediaLibrary();
    auto engine = makeTestEngine(media);

    engine.play();
    CHECK(engine.state() == PlayState::Playing);

    engine.tick(Time::fromSeconds(1.0));
    CHECK(engine.time() == Time::fromSeconds(1.0));

    engine.pause();
    CHECK(engine.state() == PlayState::Paused);
    engine.tick(Time::fromSeconds(1.0));
    CHECK(engine.time() == Time::fromSeconds(1.0)); // frozen

    engine.play();
    CHECK(engine.state() == PlayState::Playing);

    engine.stop();
    CHECK(engine.state() == PlayState::Stopped);
    CHECK(engine.time() == Time::zero());
}

TEST_CASE("Playing to the end of a finite show stops playback")
{
    auto media = makeLoadedMediaLibrary();
    Show show = makeTestShow(); // duration 7s
    // Drop the stop cue: this test exercises the duration bound, not cues.
    show.cues.erase(
        std::remove_if(show.cues.begin(), show.cues.end(),
                       [](const Cue& c) { return std::holds_alternative<StopPlayback>(c.action); }),
        show.cues.end());
    auto engine = makeTestEngine(media, show);

    engine.play();
    engine.tick(Time::fromSeconds(6.5));
    CHECK(engine.state() == PlayState::Playing);
    engine.tick(Time::fromSeconds(1.0)); // crosses 7s
    CHECK(engine.state() == PlayState::Stopped);
    CHECK(engine.time() == Time::fromSeconds(7.0));

    // Playing from the end restarts at the beginning.
    engine.play();
    CHECK(engine.time() == Time::zero());
    CHECK(engine.state() == PlayState::Playing);
}

TEST_CASE("Transport cues fire as the playhead crosses them")
{
    auto media = makeLoadedMediaLibrary();
    auto engine = makeTestEngine(media); // fixture: StopPlayback at t=6

    engine.play();
    engine.tick(Time::fromSeconds(5.5));
    CHECK(engine.state() == PlayState::Playing);
    CHECK(engine.time() == Time::fromSeconds(5.5));

    engine.tick(Time::fromSeconds(1.0)); // crosses the stop cue at t=6
    CHECK(engine.state() == PlayState::Stopped);
    CHECK(engine.time() == Time::fromSeconds(6.0));

    // A pause cue freezes the clock at the cue.
    Show show = makeTestShow();
    show.cues = {};
    show.addCue(Cue{Time::zero(), "start", PlayScene{1}});
    show.addCue(Cue{Time::fromSeconds(3.0), "hold", PausePlayback{}});
    engine.loadShow(show);
    engine.play();
    engine.tick(Time::fromSeconds(2.9));
    CHECK(engine.state() == PlayState::Playing);
    engine.tick(Time::fromSeconds(0.5)); // crosses the pause cue at t=3
    CHECK(engine.state() == PlayState::Paused);
    CHECK(engine.time() == Time::fromSeconds(3.0));
    engine.tick(Time::fromSeconds(5.0)); // frozen
    CHECK(engine.time() == Time::fromSeconds(3.0));

    // Resuming after a pause cue continues from the cue.
    engine.play();
    engine.tick(Time::fromSeconds(1.0));
    CHECK(engine.time() == Time::fromSeconds(4.0));
    CHECK(engine.state() == PlayState::Playing);

    // A seek cue jumps the playhead within the same tick.
    Show seekShow;
    seekShow.frameRate = FrameRate::fps30();
    seekShow.duration = Time::fromSeconds(10.0);
    seekShow.addCue(Cue{Time::fromSeconds(2.0), "jump", SeekTo{Time::fromSeconds(8.0)}});
    engine.loadShow(seekShow);
    engine.play();
    engine.tick(Time::fromSeconds(3.0)); // crosses the seek cue at t=2
    CHECK(engine.state() == PlayState::Playing);
    CHECK(engine.time() == Time::fromSeconds(8.0));
}

TEST_CASE("Scene follows cues as time advances")
{
    auto media = makeLoadedMediaLibrary();
    auto engine = makeTestEngine(media);

    // Cue layout: t=0 scene1, t=2 scene2, t=4 fade, t=5 scene1, t=6 stop.
    CHECK(engine.activeSceneAt(Time::fromSeconds(0.0)) == 1);
    CHECK(engine.activeSceneAt(Time::fromSeconds(1.99)) == 1);
    CHECK(engine.activeSceneAt(Time::fromSeconds(2.0)) == 2);
    CHECK(engine.activeSceneAt(Time::fromSeconds(4.99)) == 2);
    CHECK(engine.activeSceneAt(Time::fromSeconds(5.0)) == 1);
    // After the last PlayScene cue, the scene stays 1.
    CHECK(engine.activeSceneAt(Time::fromSeconds(6.9)) == 1);
}

TEST_CASE("Seeking changes the resolved state immediately (pure function)")
{
    auto media = makeLoadedMediaLibrary();
    auto engine = makeTestEngine(media);

    engine.play();
    engine.seek(Time::fromSeconds(3.0));
    CHECK(engine.activeSceneAt(engine.time()) == 2);

    // Seek backwards: state must be consistent with t, not with history.
    engine.seek(Time::fromSeconds(0.5));
    CHECK(engine.activeSceneAt(engine.time()) == 1);
}

TEST_CASE("Layer state resolves tracks and overrides")
{
    auto media = makeLoadedMediaLibrary();
    auto engine = makeTestEngine(media);

    // Give layer 10 an opacity track: 1.0 at t=0 to 0.2 at t=2.
    auto& show = engine.show();
    Layer* layer = show.findScene(1)->findLayer(10);
    REQUIRE(layer != nullptr);
    layer->opacityTrack.setKey(Time::zero(), 1.f);
    layer->opacityTrack.setKey(Time::fromSeconds(2.0), 0.2f);

    const FrameState at1 = engine.frameStateAt(Time::fromSeconds(1.0));
    const LayerFrame* lf = nullptr;
    for (const auto& l : at1.layers)
    {
        if (l.layerId == 10)
        {
            lf = &l;
        }
    }
    REQUIRE(lf != nullptr);
    CHECK(lf->opacity == doctest::Approx(0.6f)); // linear between 1.0 and 0.2

    // Manual override wins over the track.
    engine.setLayerOpacity(10, 0.1f);
    const FrameState overridden = engine.frameStateAt(Time::fromSeconds(1.0));
    for (const auto& l : overridden.layers)
    {
        if (l.layerId == 10)
        {
            CHECK(l.opacity == doctest::Approx(0.1f));
        }
    }
    engine.clearLayerOverrides();
    const FrameState cleared = engine.frameStateAt(Time::fromSeconds(1.0));
    for (const auto& l : cleared.layers)
    {
        if (l.layerId == 10)
        {
            CHECK(l.opacity == doctest::Approx(0.6f));
        }
    }
}

TEST_CASE("Cue-driven layer opacity applies at the right time")
{
    auto media = makeLoadedMediaLibrary();
    auto engine = makeTestEngine(media); // t=4 cue: setLayerOpacity(20, 0.5)

    const auto before = engine.frameStateAt(Time::fromSeconds(3.9));
    const auto after = engine.frameStateAt(Time::fromSeconds(4.0));

    float opBefore = -1.f, opAfter = -1.f;
    for (const auto& l : before.layers)
    {
        if (l.layerId == 20) opBefore = l.opacity;
    }
    for (const auto& l : after.layers)
    {
        if (l.layerId == 20) opAfter = l.opacity;
    }
    CHECK(opBefore == doctest::Approx(1.f));
    CHECK(opAfter == doctest::Approx(0.5f));
}

TEST_CASE("Media scheduling: media time follows show time with in-point offset")
{
    auto media = makeLoadedMediaLibrary();
    Show show = makeTestShow();
    // Keep scene 1 active for the whole observation window (the default
    // fixture switches to scene 2 at t=2, which would hide layer 10).
    show.cues = {};
    show.addCue(Cue{Time::zero(), "start", PlayScene{1}});
    auto engine = makeTestEngine(media, show);

    // Scene 1 starts at t=0; layer 10 plays media A (4 s) from in-point 1 s.
    // At show time 3 s the media is at 1 + 3 = 4 s -> end, held (no loop).
    const FrameState at3 = engine.frameStateAt(Time::fromSeconds(3.0));
    const LayerFrame* lf = nullptr;
    for (const auto& l : at3.layers)
    {
        if (l.layerId == 10)
        {
            lf = &l;
        }
    }
    REQUIRE(lf != nullptr);
    CHECK(lf->mediaTime == Time::fromSeconds(4.0));
    CHECK(lf->mediaAtEnd);

    // Before the end: media time = in-point + elapsed.
    const FrameState at1 = engine.frameStateAt(Time::fromSeconds(1.0));
    for (const auto& l : at1.layers)
    {
        if (l.layerId == 10)
        {
            CHECK(l.mediaTime == Time::fromSeconds(2.0));
            CHECK_FALSE(l.mediaAtEnd);
        }
    }
}

TEST_CASE("Looping media wraps within [inPoint, duration)")
{
    auto media = makeLoadedMediaLibrary();
    Show show = makeTestShow();
    // Scene 2 stays active from t=2 (the default fixture switches back to
    // scene 1 at t=5 and stops at t=6, which would hide layer 20 at t=7).
    show.cues = {};
    show.addCue(Cue{Time::zero(), "start", PlayScene{1}});
    show.addCue(Cue{Time::fromSeconds(2.0), "toB", PlayScene{2}});
    auto engine = makeTestEngine(media, show);

    // Scene 2 starts at t=2; layer 20 (media B, 4 s, loops) plays from 0.
    // At show time 7 s the media elapsed 5 s -> wraps to 1 s.
    const FrameState at7 = engine.frameStateAt(Time::fromSeconds(7.0));
    const LayerFrame* lf = nullptr;
    for (const auto& l : at7.layers)
    {
        if (l.layerId == 20)
        {
            lf = &l;
        }
    }
    REQUIRE(lf != nullptr);
    CHECK(lf->mediaTime == Time::fromSeconds(1.0));
    CHECK_FALSE(lf->mediaAtEnd);
}

TEST_CASE("Layer rate multiplies media speed")
{
    auto media = makeLoadedMediaLibrary();
    Show show = makeTestShow();
    auto& layer = show.scenes[0].layers[1];
    layer.rate = 2.f; // double speed
    layer.inPoint = Time::zero();
    auto engine = makeTestEngine(media, show);

    const FrameState at2 = engine.frameStateAt(Time::fromSeconds(2.0));
    for (const auto& l : at2.layers)
    {
        if (l.layerId == 10)
        {
            CHECK(l.mediaTime == Time::fromSeconds(4.0)); // 2 s * 2
        }
    }
}

TEST_CASE("Scene override (live scene switch)")
{
    auto media = makeLoadedMediaLibrary();
    auto engine = makeTestEngine(media);

    engine.play();
    engine.seek(Time::fromSeconds(1.0)); // scene 1 by cues
    CHECK(engine.activeSceneAt(engine.time()) == 1);

    CHECK(engine.setSceneNow(2));
    CHECK(engine.hasSceneOverride());
    CHECK(engine.activeSceneAt(engine.time()) == 2);

    // Media in the newly switched scene start fresh from the in-point.
    const FrameState state = engine.frameState();
    CHECK(state.activeSceneId == 2);
    for (const auto& l : state.layers)
    {
        if (l.layerId == 20)
        {
            CHECK(l.mediaTime == Time::zero());
        }
    }

    engine.clearSceneNow();
    CHECK(engine.activeSceneAt(engine.time()) == 1); // cues win again
    CHECK_FALSE(engine.setSceneNow(99)); // invalid scene rejected
}

TEST_CASE("Scene override releases at the next PlayScene cue")
{
    auto media = makeLoadedMediaLibrary();
    auto engine = makeTestEngine(media);
    // Fixture timeline: playScene(1) @0, playScene(2) @2, playScene(1) @5.

    engine.play();
    engine.seek(Time::fromSeconds(1.0)); // scene 1 by cues
    CHECK(engine.setSceneNow(2));
    CHECK(engine.hasSceneOverride());
    CHECK(engine.activeSceneAt(engine.time()) == 2);

    // Crossing the PlayScene(2) cue at t=2 releases the override: the
    // timeline takes scene control back. Scene 2 is still active, but now
    // by cue, with the cue time as the scene start.
    engine.tick(Time::fromSeconds(1.1)); // t=1.0 -> t=2.1
    CHECK_FALSE(engine.hasSceneOverride());
    CHECK(engine.activeSceneAt(engine.time()) == 2);
    CHECK(engine.sceneStartAt(engine.time()) == Time::fromSeconds(2.0));

    // Later PlayScene cues keep applying: t=5 plays scene 1.
    engine.tick(Time::fromSeconds(3.0)); // t=2.1 -> t=5.1
    CHECK(engine.activeSceneAt(engine.time()) == 1);
}

TEST_CASE("Multiple scenes can be active at once (composited in list order)")
{
    auto media = makeLoadedMediaLibrary();
    auto engine = makeTestEngine(media);

    engine.play();
    engine.seek(Time::fromSeconds(1.0)); // timeline scene 1

    // Toggling scene 2 on seeds the manual set with the timeline scene, so
    // both are active, in show-list order.
    CHECK(engine.toggleSceneNow(2));
    CHECK(engine.hasSceneOverride());
    CHECK(engine.activeSceneAt(engine.time()) == 1); // primary = bottom-most
    CHECK(engine.activeScenesAt(engine.time()) == std::vector<Id>{1, 2});

    // Both scenes' layers are composited: scene 1's (z order: 11, 10), then
    // scene 2's (20).
    const FrameState state = engine.frameState();
    CHECK(state.activeSceneId == 1);
    CHECK(state.activeSceneIds == std::vector<Id>{1, 2});
    std::vector<Id> order;
    for (const auto& l : state.layers)
    {
        order.push_back(l.layerId);
    }
    CHECK(order == std::vector<Id>{11, 10, 20});

    // Media in the toggled scene start from its activation time (the seek
    // point), like setSceneNow.
    for (const auto& l : state.layers)
    {
        if (l.layerId == 20)
        {
            CHECK(l.mediaTime == Time::zero());
        }
    }

    // Toggling scene 2 off leaves scene 1 (manual) active.
    CHECK(engine.toggleSceneNow(2));
    CHECK(engine.activeScenesAt(engine.time()) == std::vector<Id>{1});

    // Toggling the last manual scene off returns to the timeline.
    CHECK(engine.toggleSceneNow(1));
    CHECK_FALSE(engine.hasSceneOverride());
    CHECK(engine.activeScenesAt(engine.time()) == std::vector<Id>{1}); // by cue

    // Toggling a scene that is not in the show is rejected.
    CHECK_FALSE(engine.toggleSceneNow(99));
}

TEST_CASE("Scene list order determines compositing order")
{
    auto media = makeLoadedMediaLibrary();
    Show show = makeTestShow();
    // Reorder the scenes: scene 2 first, scene 1 second. Cues reference
    // scenes by id, so the timeline is unaffected.
    show.scenes = {show.scenes[1], show.scenes[0]};
    auto engine = makeTestEngine(media, show);

    engine.play();
    engine.seek(Time::fromSeconds(1.0)); // timeline scene 1 (by cue id)
    CHECK(engine.activeSceneAt(engine.time()) == 1);

    engine.toggleSceneNow(2);
    CHECK(engine.activeScenesAt(engine.time()) == std::vector<Id>{2, 1});
    const FrameState state = engine.frameState();
    CHECK(state.activeSceneId == 2); // bottom-most = first in the list
    std::vector<Id> order;
    for (const auto& l : state.layers)
    {
        order.push_back(l.layerId);
    }
    CHECK(order == std::vector<Id>{20, 11, 10});
}

TEST_CASE("Layers render in z order (stable)")
{
    auto media = makeLoadedMediaLibrary();
    auto engine = makeTestEngine(media);

    const FrameState state = engine.frameState();
    REQUIRE(state.layers.size() == 2);
    CHECK(state.layers[0].layerId == 11); // z=0 first
    CHECK(state.layers[1].layerId == 10); // z=1 second
}

TEST_CASE("Hidden and zero-opacity layers are excluded from frame state")
{
    auto media = makeLoadedMediaLibrary();
    auto engine = makeTestEngine(media);

    engine.setLayerVisible(11, false);
    const FrameState state = engine.frameState();
    for (const auto& l : state.layers)
    {
        CHECK(l.layerId != 11);
    }

    engine.clearLayerOverrides();
    engine.setLayerOpacity(11, 0.f);
    const FrameState state2 = engine.frameState();
    for (const auto& l : state2.layers)
    {
        CHECK(l.layerId != 11);
    }
}

TEST_CASE("Seek clamps to show duration")
{
    auto media = makeLoadedMediaLibrary();
    auto engine = makeTestEngine(media); // 7s

    engine.seek(Time::fromSeconds(100.0));
    CHECK(engine.time() == Time::fromSeconds(7.0));

    engine.seek(Time::fromSeconds(-5.0));
    CHECK(engine.time() == Time::zero());
}

TEST_CASE("Unlimited shows accept any non-negative time")
{
    auto media = makeLoadedMediaLibrary();
    Show show = makeTestShow();
    show.duration = Time{}; // zero = unlimited
    auto engine = makeTestEngine(media, show);

    engine.seek(Time::fromSeconds(100000.0));
    CHECK(engine.time() == Time::fromSeconds(100000.0));
}

TEST_CASE("State strings")
{
    CHECK(PlaybackEngine::stateToString(PlayState::Stopped) == "stopped");
    CHECK(PlaybackEngine::stateToString(PlayState::Playing) == "playing");
    CHECK(PlaybackEngine::stateToString(PlayState::Paused) == "paused");
}

} // TEST_SUITE("PlaybackEngine")
