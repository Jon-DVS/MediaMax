// Show model: serialization round-trips and timeline queries.

#include <doctest/doctest.h>

#include "mediamax/show/Show.h"

#include "TestFixtures.h"

using namespace mediamax;

TEST_SUITE("Show")
{

TEST_CASE("Round trip: full show serializes and parses back")
{
    Show original = test::makeTestShow();

    // Add a layer with tracks to exercise the track codec.
    auto& layer = original.scenes[0].layers[1];
    layer.opacityTrack.setKey(Time::fromSeconds(0.0), 1.f);
    layer.opacityTrack.setKey(Time::fromSeconds(2.0), 0.f);
    // Interpolation is stored on the from-key and applies forward.
    layer.xTrack.setKey(Time::fromSeconds(0.0), 0.f, Interpolation::Step);
    layer.xTrack.setKey(Time::fromSeconds(3.0), 120.f);
    layer.transform.rotation = 0.5f;
    layer.color = {0.5f, 0.25f, 0.125f, 0.75f};

    // Target systems (issue 18): the show file carries the system list.
    original.systems.push_back({"Main Wall", "192.168.1.50", 47600});
    original.systems.push_back({"Lobby", "192.168.1.51", 47601});

    const nlohmann::json json = original.toJson();
    std::string error;
    auto parsed = Show::fromJson(json, error);
    REQUIRE(parsed.has_value());
    CHECK(error.empty());

    const Show& s = *parsed;
    CHECK(s.name == original.name);
    CHECK(s.frameRate == original.frameRate);
    CHECK(s.duration == original.duration);
    CHECK(s.media.size() == 2);
    CHECK(s.media[0].id == test::kMediaA);
    CHECK(s.media[0].path == "media/video-a.mp4");
    CHECK(s.scenes.size() == 2);
    CHECK(s.scenes[0].name == "A");
    CHECK(s.scenes[0].layers.size() == 2);

    const Layer& l = s.scenes[0].layers[1];
    CHECK(l.id == 10);
    CHECK(l.mediaId == test::kMediaA);
    CHECK(l.color.r == doctest::Approx(0.5f));
    CHECK(l.color.a == doctest::Approx(0.75f));
    CHECK(l.transform.rotation == doctest::Approx(0.5f));
    CHECK(l.inPoint == Time::fromSeconds(1.0));
    CHECK(l.opacityTrack.keyCount() == 2);
    CHECK(l.opacityTrack.valueAt(Time::fromSeconds(1.0)) == doctest::Approx(0.5f));
    CHECK(l.xTrack.keyCount() == 2);
    CHECK(l.xTrack.keyframes()[0].interpolation == Interpolation::Step);
    CHECK(l.xTrack.valueAt(Time::fromSeconds(2.9f)) == doctest::Approx(0.f));

    CHECK(s.cues.size() == 5);
    CHECK(s.cues[0].time == Time::zero());
    const auto* playScene = std::get_if<PlayScene>(&s.cues[0].action);
    REQUIRE(playScene != nullptr);
    CHECK(playScene->scene == 1);

    CHECK(s.systems.size() == 2);
    CHECK(s.systems[0].name == "Main Wall");
    CHECK(s.systems[0].host == "192.168.1.50");
    CHECK(s.systems[0].port == 47600);
    CHECK(s.systems[1].name == "Lobby");
    CHECK(s.systems[1].host == "192.168.1.51");
    CHECK(s.systems[1].port == 47601);
}

TEST_CASE("Cue round trip: every action type")
{
    Show show;
    show.addCue(Cue{Time::zero(), "a", PlayScene{1}});
    show.addCue(Cue{Time::fromSeconds(1.0), "b", StopPlayback{}});
    show.addCue(Cue{Time::fromSeconds(2.0), "c", PausePlayback{}});
    show.addCue(Cue{Time::fromSeconds(3.0), "d", SeekTo{Time::fromSeconds(5.0)}});
    show.addCue(Cue{Time::fromSeconds(4.0), "e", SetLayerOpacity{7, 0.25f}});
    show.addCue(Cue{Time::fromSeconds(5.0), "f", SetLayerVisible{7, false}});
    show.addCue(Cue{Time::fromSeconds(6.0), "g", SetLayerBlend{7, BlendMode::Screen}});
    show.addCue(Cue{Time::fromSeconds(7.0), "h", SetLayerPosition{7, 3.f, 4.f}});

    const nlohmann::json json = show.toJson();
    std::string error;
    auto parsed = Show::fromJson(json, error);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->cues.size() == 8);

    CHECK(std::holds_alternative<PlayScene>(parsed->cues[0].action));
    CHECK(std::holds_alternative<StopPlayback>(parsed->cues[1].action));
    CHECK(std::holds_alternative<PausePlayback>(parsed->cues[2].action));
    const auto* seek = std::get_if<SeekTo>(&parsed->cues[3].action);
    REQUIRE(seek != nullptr);
    CHECK(seek->time == Time::fromSeconds(5.0));
    const auto* op = std::get_if<SetLayerOpacity>(&parsed->cues[4].action);
    REQUIRE(op != nullptr);
    CHECK(op->layer == 7);
    CHECK(op->value == doctest::Approx(0.25f));
    const auto* vis = std::get_if<SetLayerVisible>(&parsed->cues[5].action);
    REQUIRE(vis != nullptr);
    CHECK(vis->value == false);
    const auto* blend = std::get_if<SetLayerBlend>(&parsed->cues[6].action);
    REQUIRE(blend != nullptr);
    CHECK(blend->mode == BlendMode::Screen);
    const auto* pos = std::get_if<SetLayerPosition>(&parsed->cues[7].action);
    REQUIRE(pos != nullptr);
    CHECK(pos->x == doctest::Approx(3.f));
    CHECK(pos->y == doctest::Approx(4.f));
}

TEST_CASE("addCue keeps cues sorted and replaces duplicates")
{
    Show show;
    show.addCue(Cue{Time::fromSeconds(5.0), "late", StopPlayback{}});
    show.addCue(Cue{Time::zero(), "early", PlayScene{1}});
    show.addCue(Cue{Time::fromSeconds(2.5), "mid", PausePlayback{}});

    CHECK(show.cues.size() == 3);
    CHECK(show.cues[0].time == Time::zero());
    CHECK(show.cues[1].time == Time::fromSeconds(2.5));
    CHECK(show.cues[2].time == Time::fromSeconds(5.0));

    // Replace the cue at t=2.5.
    show.addCue(Cue{Time::fromSeconds(2.5), "replaced", StopPlayback{}});
    CHECK(show.cues.size() == 3);
    CHECK(show.cues[1].name == "replaced");
    CHECK(std::holds_alternative<StopPlayback>(show.cues[1].action));
}

TEST_CASE("fromJson rejects malformed input")
{
    std::string error;

    CHECK_FALSE(Show::fromJson(nlohmann::json(42), error).has_value());
    CHECK(error.find("object") != std::string::npos);

    CHECK_FALSE(Show::fromJson(nlohmann::json::object(), error).has_value());
    CHECK(error.find("app") != std::string::npos);

    nlohmann::json bad = {{"app", "mediamax-show"}, {"version", 99}};
    CHECK_FALSE(Show::fromJson(bad, error).has_value());
    CHECK(error.find("version") != std::string::npos);

    nlohmann::json badRate = {{"app", "mediamax-show"},
                              {"version", 1},
                              {"frameRate", {{"num", -1}, {"den", 1}}}};
    CHECK_FALSE(Show::fromJson(badRate, error).has_value());

    nlohmann::json badCue = {{"app", "mediamax-show"},
                             {"version", 1},
                             {"cues", nlohmann::json::array({{
                                 {"time", 1.0},
                                 {"action", nlohmann::json::object()},
                             }})}};
    CHECK_FALSE(Show::fromJson(badCue, error).has_value());

    nlohmann::json badSys = {{"app", "mediamax-show"},
                             {"version", 1},
                             {"systems", nlohmann::json::array({{{"name", "no host"}}})}};
    CHECK_FALSE(Show::fromJson(badSys, error).has_value());
    CHECK(error.find("systems") != std::string::npos);

    nlohmann::json badSysPort = {{"app", "mediamax-show"},
                                 {"version", 1},
                                 {"systems",
                                  nlohmann::json::array(
                                      {{{"name", "a"}, {"host", "b"}, {"port", 99999}}})}};
    CHECK_FALSE(Show::fromJson(badSysPort, error).has_value());

    // A display entry must carry a non-empty name (the name is the wall's
    // identity).
    nlohmann::json badDisplay = {{"app", "mediamax-show"},
                                 {"version", 1},
                                 {"displays", nlohmann::json::array({{{"x", 0}}})}};
    CHECK_FALSE(Show::fromJson(badDisplay, error).has_value());
    CHECK(error.find("display") != std::string::npos);

    // Warp points, when present, must be exactly rows*cols [u, v] pairs.
    const nlohmann::json warpWithBadPoints = {
        {"name", "a"},
        {"warp",
         {
             {"rows", 2},
             {"cols", 2},
             {"points", nlohmann::json::array({{0.0, 0.0}})}, // one point, need 4
         }},
    };
    const nlohmann::json badWarpPoints = {
        {"app", "mediamax-show"},
        {"version", 1},
        {"displays", nlohmann::json::array({warpWithBadPoints})},
    };
    CHECK_FALSE(Show::fromJson(badWarpPoints, error).has_value());
}

TEST_CASE("fromJson tolerates optional fields")
{
    nlohmann::json j = {
        {"app", "mediamax-show"},
        {"version", 1},
        {"scenes",
         nlohmann::json::array({{
             {"id", 1},
             {"name", "s"},
             {"layers", nlohmann::json::array({{{"id", 1}}})},
         }})},
    };
    std::string error;
    auto show = Show::fromJson(j, error);
    REQUIRE(show.has_value());
    CHECK(show->name == "Untitled Show");
    CHECK(show->frameRate == FrameRate::fps30()); // default
    CHECK(show->systems.empty()); // absent field: no target systems
    const auto& layer = show->scenes[0].layers[0];
    CHECK(layer.opacity == doctest::Approx(1.f));
    CHECK(layer.blend == BlendMode::Normal);
    CHECK(layer.visible);
}

TEST_CASE("findScene and findMedia")
{
    const Show show = test::makeTestShow();
    CHECK(show.findScene(1) != nullptr);
    CHECK(show.findScene(2)->name == "B");
    CHECK(show.findScene(99) == nullptr);
    CHECK(show.findMedia(test::kMediaA) != nullptr);
    CHECK(show.findMedia(test::kMediaA)->path == "media/video-a.mp4");
    CHECK(show.findMedia(999) == nullptr);
}

TEST_CASE("Round trip: display wall (geometry, flags, warp)")
{
    Show show = test::makeTestShow();

    ShowDisplay main;
    main.id = 1;
    main.name = "Wall A";
    main.x = 0;
    main.y = 0;
    main.width = 3840;
    main.height = 2160;
    main.brightness = 0.85f;
    main.alwaysOnTop = false;
    main.statusOverlay = true;
    main.vsync = false;
    main.warp = WarpMap(3, 3, WarpInterpolation::Spline);
    main.warp.setPointPosition(1, 1, 0.55f, 0.475f); // nudged center node

    ShowDisplay side;
    side.id = 2;
    side.name = "Wall B";
    side.x = 3840;
    // y / brightness / enabled / vsync / warp left at defaults
    show.displays.push_back(main);
    show.displays.push_back(side);

    const nlohmann::json json = show.toJson();
    REQUIRE(json.contains("displays"));
    CHECK(json["displays"].size() == 2);

    std::string error;
    auto parsed = Show::fromJson(json, error);
    REQUIRE(parsed.has_value());
    CHECK(error.empty());

    const Show& s = *parsed;
    CHECK(s.displays.size() == 2);
    const ShowDisplay& a = s.displays[0];
    CHECK(a.id == 1);
    CHECK(a.name == "Wall A");
    CHECK(a.x == 0);
    CHECK(a.y == 0);
    CHECK(a.width == 3840);
    CHECK(a.height == 2160);
    CHECK(a.brightness == doctest::Approx(0.85f));
    CHECK(a.enabled);
    CHECK(!a.alwaysOnTop);
    CHECK(a.statusOverlay);
    CHECK(!a.vsync);
    CHECK(a.warp.interpolation() == WarpInterpolation::Spline);
    CHECK(a.warp.rows() == 3);
    CHECK(a.warp.cols() == 3);
    const auto center = a.warp.pointPositions()[4]; // row 1, col 1 of a 3x3
    CHECK(center[0] == doctest::Approx(0.55f));
    CHECK(center[1] == doctest::Approx(0.475f));

    // The second display round-trips its defaults (2x2 identity warp).
    const ShowDisplay& b = s.displays[1];
    CHECK(b.id == 2);
    CHECK(b.name == "Wall B");
    CHECK(b.x == 3840);
    CHECK(b.y == 0);
    CHECK(b.width == 1920);
    CHECK(b.height == 1080);
    CHECK(b.vsync);
    CHECK(b.warp.rows() == 2);
    CHECK(b.warp.cols() == 2);

    // A show with no displays omits the section entirely, so saving such a
    // show leaves existing documents (and a system's live display set)
    // untouched.
    Show bare = test::makeTestShow();
    CHECK_FALSE(bare.toJson().contains("displays"));
}

} // TEST_SUITE("Show")
