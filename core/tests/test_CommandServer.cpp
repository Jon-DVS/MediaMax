// The command server: the full control surface mapped onto a real engine,
// media library and display set. This is the integration test for the
// protocol layer - it exercises every one of the 25 commands.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <variant>

#include <doctest/doctest.h>

#include "mediamax/math/Transform2D.h"
#include "mediamax/media/MediaThumbnail.h"
#include "mediamax/protocol/CommandServer.h"
#include "mediamax/protocol/Protocol.h"
#include "mediamax/util/Base64.h"
#include "mediamax/util/File.h"

#include "TestFixtures.h"

using namespace mediamax;
using namespace mediamax::protocol;
using mediamax::test::kMediaA;
using mediamax::test::kMediaB;

namespace
{

// One complete system: media library (patterns loaded as ids 1 and 2),
// two displays, a playback engine running the fixture show.
struct Fixture
{
    MediaLibrary media = mediamax::test::makeMediaLibrary();
    DisplayManager displays;
    PlaybackEngine engine;
    CommandServer server;
    Id displayA = kInvalidId;
    Id displayB = kInvalidId;
    std::string lastLoadedPath; // the (resolved) path the last media load used

    Fixture()
        : engine(media)
        , server(engine, media, displays, "test-system")
    {
        displayA = displays.addDisplay("main", 0, 0, 1920, 1080).id;
        displayB = displays.addDisplay("side", 1920, 0, 1920, 1080).id;
        engine.loadShow(mediamax::test::makeTestShow());
        mediamax::test::loadShowMedia(media, mediamax::test::makeTestShow());
        server.setMediaLoadFunction([this](const std::string& path, std::string& error) {
            lastLoadedPath = path;
            // The synthetic media factory keys on the bare relative paths
            // the fixtures load with. show.load resolves media against the
            // show file's directory, so canonicalize base-qualified variants
            // of the same file onto that string (the library dedups by path).
            const std::string leaf = std::filesystem::path(path).filename().string();
            const std::string canonical =
                leaf == "video-a.mp4" ? "media/video-a.mp4"
                : leaf == "video-b.mp4" ? "media/video-b.mp4"
                                        : path;
            return media.load(canonical, error);
        });
    }

    Response run(const std::string& method, nlohmann::json params = nlohmann::json::object())
    {
        Command cmd;
        cmd.id = 1;
        cmd.method = method;
        cmd.params = std::move(params);
        return server.handle(cmd);
    }
};

// Writes `contents` to a unique temp file and returns its path.
std::filesystem::path tempFile(const std::string& stem, const std::string& contents)
{
    static std::atomic<std::uint64_t> counter{0};
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const auto dir = std::filesystem::temp_directory_path() / "mediamax-tests";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / (stem + "-" + std::to_string(ns) + "-" +
                             std::to_string(counter.fetch_add(1)) + ".json");
    writeFile(path, contents);
    return path;
}

std::string readTempFile(const std::filesystem::path& path)
{
    std::ifstream in(path);
    std::string contents((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    return contents;
}

// The corner array frame.state reports for a transform + media size (the
// same projection the command uses; the tests check the wiring, not the
// math, which Transform2D's own tests cover).
nlohmann::json cornerJson(const Transform2D& t, const Size2i& mediaSize)
{
    nlohmann::json corners = nlohmann::json::array();
    const float uvs[4][2] = {{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}};
    for (const auto& uv : uvs)
    {
        const auto [px, py] = t.toOutputSpace(uv[0], uv[1], mediaSize);
        corners.push_back({px, py});
    }
    return corners;
}

} // namespace

TEST_SUITE("CommandServer")
{

TEST_CASE("ping reports the system")
{
    Fixture f;
    const Response resp = f.run("ping");
    REQUIRE(resp.ok);
    CHECK(resp.result.at("server").get<std::string>() == "mediamax");
    CHECK(resp.result.at("system").get<std::string>() == "test-system");
    CHECK(resp.result.at("protocolVersion").get<int>() == kProtocolVersion);
}

TEST_CASE("status reflects the full system state")
{
    Fixture f;
    const Response resp = f.run("status");
    REQUIRE(resp.ok);

    CHECK(resp.result.at("system").get<std::string>() == "test-system");
    CHECK(resp.result.at("version").get<std::string>() == std::string(MEDIAMAX_VERSION));
    CHECK(resp.result.at("state").get<std::string>() == "stopped");
    // The machine identity is reported too; the core fixture has no
    // platform access, so the app-set values default to empty.
    CHECK(resp.result.at("hostname").get<std::string>().empty());
    CHECK(resp.result.at("ip").get<std::string>().empty());
    CHECK(resp.result.at("time").get<double>() == doctest::Approx(0.0));
    CHECK(resp.result.at("timecode").get<std::string>() == "00:00:00:00");
    CHECK(resp.result.at("duration").get<double>() == doctest::Approx(7.0));
    CHECK(resp.result.at("frameRate").get<std::string>() == "30");
    CHECK(resp.result.at("scene").get<std::uint64_t>() == 1);

    CHECK(resp.result.at("scenes").size() == 2);
    CHECK(resp.result.at("media").size() == 2);
    CHECK(resp.result.at("displays").size() == 2);
    // Scene 1 has two visible layers at t=0 (bg + videoA).
    CHECK(resp.result.at("layers").get<std::uint64_t>() == 2);

    const auto& d0 = resp.result.at("displays")[0];
    CHECK(d0.at("name").get<std::string>() == "main");
    CHECK(d0.at("width").get<std::uint32_t>() == 1920);
}

TEST_CASE("status reports the machine identity set by the app")
{
    Fixture f;
    f.server.setSystemHost("wall-pc-01");
    f.server.setSystemIp("10.0.0.42");

    const Response resp = f.run("status");
    REQUIRE(resp.ok);
    CHECK(resp.result.at("hostname").get<std::string>() == "wall-pc-01");
    CHECK(resp.result.at("ip").get<std::string>() == "10.0.0.42");
}

TEST_CASE("status reports the dropped video frame count set by the app")
{
    Fixture f;
    // Default (the playback app sets this every frame; 0 until then).
    const Response def = f.run("status");
    REQUIRE(def.ok);
    CHECK(def.result.at("droppedVideoFrames").get<std::uint64_t>() == 0);

    f.server.setDroppedVideoFrames(1234);
    const Response resp = f.run("status");
    REQUIRE(resp.ok);
    CHECK(resp.result.at("droppedVideoFrames").get<std::uint64_t>() == 1234);
}

TEST_CASE("unknown method fails with a descriptive error")
{
    Fixture f;
    const Response resp = f.run("bogus.command");
    CHECK_FALSE(resp.ok);
    CHECK(resp.error.find("unknown method") != std::string::npos);
    CHECK(resp.id == 1);
}

TEST_CASE("missing parameters fail with the parameter name")
{
    Fixture f;
    CHECK_FALSE(f.run("playback.seek").ok);
    CHECK(f.run("playback.seek").error.find("time") != std::string::npos);
    CHECK_FALSE(f.run("media.load").ok);
    CHECK(f.run("media.load").error.find("path") != std::string::npos);
    CHECK_FALSE(f.run("scene.set").ok);
    CHECK(f.run("scene.set").error.find("index") != std::string::npos);
}

TEST_CASE("system.set renames the system")
{
    Fixture f;
    const Response resp = f.run("system.set", {{"name", "lobby"}});
    REQUIRE(resp.ok);
    CHECK(resp.result.at("system").get<std::string>() == "lobby");

    CHECK(f.run("status").result.at("system").get<std::string>() == "lobby");
    CHECK(f.run("ping").result.at("system").get<std::string>() == "lobby");
}

TEST_CASE("playback transport commands")
{
    Fixture f;
    CHECK(f.run("playback.play").result.at("state").get<std::string>() == "playing");
    CHECK(f.run("playback.pause").result.at("state").get<std::string>() == "paused");

    // Resume + seek from a string time.
    const Response play = f.run("playback.play", {{"at", "3.5"}});
    REQUIRE(play.ok);
    CHECK(play.result.at("state").get<std::string>() == "playing");
    CHECK(f.run("status").result.at("time").get<double>() == doctest::Approx(3.5));

    CHECK_FALSE(f.run("playback.play", {{"at", "not-a-time"}}).ok);

    CHECK(f.run("playback.stop").result.at("state").get<std::string>() == "stopped");
    CHECK(f.run("status").result.at("time").get<double>() == doctest::Approx(0.0));
}

TEST_CASE("playback.seek clamps to the show duration")
{
    Fixture f;
    CHECK(f.run("playback.seek", {{"time", 100.0}})
              .result.at("time").get<double>() == doctest::Approx(7.0));
    CHECK(f.run("playback.seek", {{"time", -5.0}})
              .result.at("time").get<double>() == doctest::Approx(0.0));
    CHECK(f.run("playback.seek", {{"time", 2.5}})
              .result.at("time").get<double>() == doctest::Approx(2.5));
}

TEST_CASE("scene.set switches the live scene")
{
    Fixture f;
    const Response resp = f.run("scene.set", {{"index", 1}});
    REQUIRE(resp.ok);
    CHECK(resp.result.at("scene").get<std::uint64_t>() == 2);
    CHECK(resp.result.at("name").get<std::string>() == "B");

    // The override wins over the timeline: scene 2 even at t=0.
    CHECK(f.run("status").result.at("scene").get<std::uint64_t>() == 2);
    CHECK(f.run("status").result.at("layers").get<std::uint64_t>() == 1);

    // Out of range.
    CHECK_FALSE(f.run("scene.set", {{"index", 5}}).ok);
    CHECK(f.run("scene.set", {{"index", 5}}).error.find("out of range") != std::string::npos);
    CHECK_FALSE(f.run("scene.set", {{"index", -1}}).ok);
}

TEST_CASE("scene.list marks the active scene")
{
    Fixture f;
    const Response resp = f.run("scene.list");
    REQUIRE(resp.ok);
    const auto& scenes = resp.result.at("scenes");
    CHECK(scenes.size() == 2);
    CHECK(scenes[0].at("active").get<bool>());   // cue at t=0 plays scene 1
    CHECK_FALSE(scenes[1].at("active").get<bool>());

    f.run("playback.seek", {{"time", 2.5}});
    const Response resp2 = f.run("scene.list");
    REQUIRE(resp2.ok);
    const auto& scenes2 = resp2.result.at("scenes");
    CHECK_FALSE(scenes2[0].at("active").get<bool>());
    CHECK(scenes2[1].at("active").get<bool>());
}

TEST_CASE("scene.list reports the control position, not the animation")
{
    Fixture f;
    // A show whose only layer has an x track (0 -> 1000 over 4 s); the
    // authored (static) position is x=123.
    const nlohmann::json doc = R"({
        "app": "mediamax-show",
        "version": 1,
        "name": "Tracked",
        "frameRate": {"num": 30, "den": 1},
        "scenes": [
            {"id": 1, "name": "S", "layers": [
                {"id": 10, "name": "vid",
                 "transform": {"x": 123.0, "y": 45.0},
                 "tracks": {"x": [
                     {"time": 0.0, "value": 0.0, "interpolation": "linear"},
                     {"time": 4.0, "value": 1000.0, "interpolation": "linear"}
                 ]}}
            ]}
        ]
    })"_json;
    const Response load = f.run("show.loadJson", {{"show", doc}});
    REQUIRE(load.ok);
    f.engine.play();
    f.engine.seek(Time::fromSeconds(2.0)); // track value: 500

    // The reported position is the authored value - the animation does not
    // drive it (stable while the show plays).
    auto layerX = [&f]() {
        return f.run("scene.list").result.at("scenes")[0].at("layers")[0].at("x").get<float>();
    };
    CHECK(layerX() == doctest::Approx(123.0f));
    f.engine.tick(Time::fromSeconds(1.0)); // t=3: track value would be 750
    CHECK(layerX() == doctest::Approx(123.0f));

    // A manual override drives it instead (layer.set takes x and y as a
    // pair).
    const Response set = f.run("layer.set", {{"layer", 10}, {"x", 777.0}, {"y", 1.0}});
    REQUIRE(set.ok);
    CHECK(layerX() == doctest::Approx(777.0f));
}

TEST_CASE("scene.toggle composites multiple scenes in list order")
{
    Fixture f;

    // Toggling scene 2 (index 1) on from the timeline (scene 1 at t=0)
    // keeps the timeline scene active as well: both scenes are active, in
    // show-list order.
    const Response on = f.run("scene.toggle", {{"index", 1}});
    REQUIRE(on.ok);
    CHECK(on.result.at("scene").get<std::uint64_t>() == 2);
    CHECK(on.result.at("active").get<bool>());
    CHECK(on.result.at("activeScenes").get<std::vector<std::uint64_t>>() ==
          std::vector<std::uint64_t>{1, 2});

    // Both scenes' layers are composited in list order (scene 1: 11, 10;
    // scene 2: 20).
    std::vector<Id> order;
    for (const auto& l : f.engine.frameState().layers)
    {
        order.push_back(l.layerId);
    }
    CHECK(order == std::vector<Id>{11, 10, 20});

    // scene.list marks every active scene; status reports the active set
    // (the primary scene stays the single "scene" field).
    const Response list = f.run("scene.list");
    REQUIRE(list.ok);
    CHECK(list.result.at("scenes")[0].at("active").get<bool>());
    CHECK(list.result.at("scenes")[1].at("active").get<bool>());
    const nlohmann::json status = f.run("status").result;
    CHECK(status.at("scene").get<std::uint64_t>() == 1);
    CHECK(status.at("activeScenes").get<std::vector<std::uint64_t>>() ==
          std::vector<std::uint64_t>{1, 2});

    // Toggling scene 1 off leaves scene 2 (manual) active.
    const Response off = f.run("scene.toggle", {{"index", 0}});
    REQUIRE(off.ok);
    CHECK_FALSE(off.result.at("active").get<bool>());
    CHECK(off.result.at("activeScenes").get<std::vector<std::uint64_t>>() ==
          std::vector<std::uint64_t>{2});

    // Toggling the last manual scene off leaves the manual set in effect
    // but empty: no scene is active (status reports scene 0) and the
    // timeline does not take back until scene.clear.
    const Response off2 = f.run("scene.toggle", {{"index", 1}});
    REQUIRE(off2.ok);
    CHECK_FALSE(off2.result.at("active").get<bool>());
    CHECK(off2.result.at("activeScenes").get<std::vector<std::uint64_t>>().empty());
    CHECK(f.engine.hasSceneOverride());
    const nlohmann::json status2 = f.run("status").result;
    CHECK(status2.at("scene").get<std::uint64_t>() == 0);
    CHECK(status2.at("activeScenes").get<std::vector<std::uint64_t>>().empty());

    // scene.clear releases the empty set back to the timeline (0 scenes).
    const Response released = f.run("scene.clear");
    REQUIRE(released.ok);
    CHECK(released.result.at("cleared").get<int>() == 0);
    CHECK(released.result.at("scene").get<std::uint64_t>() == 1);

    // scene.set is still a solo: exactly the given scene, nothing else.
    REQUIRE(f.run("scene.set", {{"index", 1}}).ok);
    CHECK(f.run("status").result.at("activeScenes").get<std::vector<std::uint64_t>>() ==
          std::vector<std::uint64_t>{2});

    // Out of range.
    CHECK_FALSE(f.run("scene.toggle", {{"index", 5}}).ok);
    CHECK(f.run("scene.toggle", {{"index", 5}}).error.find("out of range") != std::string::npos);
}

TEST_CASE("layer.set applies manual overrides")
{
    Fixture f;
    REQUIRE(f.run("layer.set", {{"layer", 11}, {"opacity", 0.25}}).ok);
    FrameState state = f.engine.frameState();
    REQUIRE(state.layers.size() == 2);
    const LayerFrame* layer = nullptr;
    for (const auto& lf : state.layers)
    {
        if (lf.layerId == 11)
        {
            layer = &lf;
        }
    }
    REQUIRE(layer);
    CHECK(layer->opacity == doctest::Approx(0.25f));

    REQUIRE(f.run("layer.set", {{"layer", 11}, {"blend", "add"}}).ok);
    CHECK(f.engine.frameState().layers[0].blend == BlendMode::Add);

    REQUIRE(f.run("layer.set", {{"layer", 11}, {"x", 0.25}, {"y", -0.5}}).ok);
    for (const auto& lf : f.engine.frameState().layers)
    {
        if (lf.layerId == 11)
        {
            CHECK(lf.x == doctest::Approx(0.25f));
            CHECK(lf.y == doctest::Approx(-0.5f));
        }
    }

    // Hiding a layer removes it from the resolved frame.
    REQUIRE(f.run("layer.set", {{"layer", 11}, {"visible", false}}).ok);
    CHECK(f.engine.frameState().layers.size() == 1);
}

TEST_CASE("layer.clear removes manual overrides")
{
    Fixture f;
    // Set two overrides, on a visible layer and on layer 10.
    REQUIRE(f.run("layer.set", {{"layer", 11}, {"opacity", 0.25}}).ok);
    REQUIRE(f.run("layer.set", {{"layer", 10}, {"visible", false}}).ok);
    CHECK(f.engine.frameState().layers.size() == 1); // layer 10 hidden

    // Clearing one layer restores just that layer.
    const Response one = f.run("layer.clear", {{"layer", 10}});
    REQUIRE(one.ok);
    CHECK(one.result.at("layer").get<std::uint64_t>() == 10);
    CHECK(f.engine.frameState().layers.size() == 2); // layer 10 back
    // The other override is still in place.
    CHECK(f.engine.frameState().layers[0].opacity == doctest::Approx(0.25f));

    // Clearing a layer with no override is a clean error.
    CHECK_FALSE(f.run("layer.clear", {{"layer", 20}}).ok);
    CHECK(f.run("layer.clear", {{"layer", 20}}).error.find("no override") != std::string::npos);

    // Clearing with no params wipes everything.
    const Response all = f.run("layer.clear");
    REQUIRE(all.ok);
    CHECK(all.result.at("cleared").get<int>() == 1); // only layer 11's remains
    CHECK(f.engine.frameState().layers[0].opacity == doctest::Approx(1.0f));
}

TEST_CASE("layer.set rejects layer ids that are not in the show")
{
    Fixture f;
    // Layer 99 is not in any scene of the fixture show.
    const Response bad = f.run("layer.set", {{"layer", 99}, {"opacity", 0.5}});
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("no layer with id 99") != std::string::npos);
    // And nothing was stored: clearing it finds no override.
    CHECK_FALSE(f.run("layer.clear", {{"layer", 99}}).ok);

    // Layers of inactive scenes are valid ids.
    CHECK(f.run("layer.set", {{"layer", 20}, {"opacity", 0.5}}).ok);
    f.run("layer.clear"); // leave the fixture clean
}

TEST_CASE("scene.list reports layer positions and override state")
{
    Fixture f;
    auto layerEntry = [](const nlohmann::json& scenes, Id sceneId, Id layerId)
                       -> std::optional<nlohmann::json>
    {
        for (const auto& s : scenes)
        {
            if (s.at("id").get<std::uint64_t>() == sceneId)
            {
                for (const auto& l : s.at("layers"))
                {
                    if (l.at("id").get<std::uint64_t>() == layerId)
                    {
                        return l;
                    }
                }
            }
        }
        return std::nullopt;
    };

    // Before any override: the active scene's layers report their effective
    // (here: static) position and no override.
    const Response base = f.run("scene.list");
    REQUIRE(base.ok);
    const auto l11 = layerEntry(base.result.at("scenes"), 1, 11);
    REQUIRE(l11.has_value());
    CHECK(l11->value("x", -1.0f) == 0.0f);
    CHECK(l11->value("y", -1.0f) == 0.0f);
    CHECK(l11->value("override", true) == false);

    // A position override is reported back (this is what the control GUI
    // resyncs its x/y fields from).
    REQUIRE(f.run("layer.set", {{"layer", 11}, {"x", 0.25}, {"y", -0.5}}).ok);
    const Response moved = f.run("scene.list");
    REQUIRE(moved.ok);
    const auto l11m = layerEntry(moved.result.at("scenes"), 1, 11);
    REQUIRE(l11m.has_value());
    CHECK(l11m->at("x").get<float>() == doctest::Approx(0.25f));
    CHECK(l11m->at("y").get<float>() == doctest::Approx(-0.5f));
    CHECK(l11m->value("override", false) == true);

    // Inactive scenes' layers report no position (undefined while not
    // active), but the override flag is global.
    const auto l20 = layerEntry(moved.result.at("scenes"), 2, 20);
    REQUIRE(l20.has_value());
    CHECK(!l20->contains("x"));
    CHECK(!l20->contains("y"));
    CHECK(l20->value("override", true) == false);

    // A hidden layer is not resolved, so it reports no position either.
    REQUIRE(f.run("layer.set", {{"layer", 10}, {"visible", false}}).ok);
    const Response hidden = f.run("scene.list");
    REQUIRE(hidden.ok);
    const auto l10 = layerEntry(hidden.result.at("scenes"), 1, 10);
    REQUIRE(l10.has_value());
    CHECK(!l10->contains("x"));
    CHECK(l10->value("override", false) == true);
    REQUIRE(f.run("layer.clear", {{"layer", 10}}).ok);

    // Clearing drops the override; the reported position returns to the
    // timeline value.
    REQUIRE(f.run("layer.clear", {{"layer", 11}}).ok);
    const Response cleared = f.run("scene.list");
    REQUIRE(cleared.ok);
    const auto l11c = layerEntry(cleared.result.at("scenes"), 1, 11);
    REQUIRE(l11c.has_value());
    CHECK(l11c->at("x").get<float>() == doctest::Approx(0.0f));
    CHECK(l11c->at("y").get<float>() == doctest::Approx(0.0f));
    CHECK(l11c->value("override", true) == false);
}

TEST_CASE("media.load registers an asset with its metadata")
{
    Fixture f;
    // The fixture already loaded video-a as kMediaA: a deduplicated re-load
    // returns that existing asset (same id, no re-decode).
    const Response dup = f.run("media.load", {{"path", "media/video-a.mp4"}});
    REQUIRE(dup.ok);
    CHECK(dup.result.at("id").get<std::uint64_t>() == kMediaA);
    CHECK(dup.result.at("width").get<std::uint32_t>() == 640);

    // A file that is not loaded registers a fresh asset. (Unload video-b so
    // the factory has to build it again.)
    REQUIRE(f.run("media.unload", {{"id", kMediaB}}).ok);
    const Response resp = f.run("media.load", {{"path", "media/video-b.mp4"}});
    REQUIRE(resp.ok);
    const auto id = resp.result.at("id").get<std::uint64_t>();
    CHECK(id > 2); // a new asset, not one of the fixture loads
    CHECK(resp.result.at("type").get<std::string>() == "video");
    CHECK(resp.result.at("width").get<std::uint32_t>() == 640);
    CHECK(resp.result.at("height").get<std::uint32_t>() == 360);
    CHECK(resp.result.at("duration").get<double>() == doctest::Approx(4.0));
    CHECK(resp.result.at("frameRate").get<std::string>() == "30");
    // The codec-detail fields are always present (additive protocol shape);
    // the synthetic test-pattern source reports none of them.
    CHECK(resp.result.contains("codec"));
    CHECK(resp.result.contains("colorspace"));
    CHECK(resp.result.contains("bitDepth"));
    CHECK(resp.result.contains("scanType"));
    CHECK(resp.result.contains("bitRate"));
    CHECK(resp.result.contains("audioCodec"));
    CHECK(resp.result.at("codec").get<std::string>().empty());

    // Unknown file fails cleanly.
    const Response bad = f.run("media.load", {{"path", "nope.mp4"}});
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("failed to load media") != std::string::npos);
}

TEST_CASE("media.list and media.unload")
{
    Fixture f;
    CHECK(f.run("media.list").result.at("media").size() == 2);

    const Response unload = f.run("media.unload", {{"id", kMediaB}});
    REQUIRE(unload.ok);
    CHECK(f.run("media.list").result.at("media").size() == 1);

    const Response again = f.run("media.unload", {{"id", kMediaB}});
    CHECK_FALSE(again.ok);
    CHECK(again.error.find("no media") != std::string::npos);
}

TEST_CASE("display.add appends a display and grows the wall")
{
    Fixture f;
    std::vector<Display> added;
    f.server.setDisplayAddedFunction([&added](const Display& d) { added.push_back(d); });

    const Response resp = f.run("display.add",
                                {{"x", 3840}, {"y", 0}, {"width", 1280}, {"height", 720},
                                 {"name", "right"}});
    REQUIRE(resp.ok);
    const Id id = resp.result.at("id").get<Id>();
    CHECK(id == 3); // the fixture's displays are 1 and 2
    CHECK(resp.result.at("name").get<std::string>() == "right");
    CHECK(resp.result.at("x").get<std::int32_t>() == 3840);
    CHECK(resp.result.at("width").get<std::int32_t>() == 1280);
    CHECK(resp.result.at("height").get<std::int32_t>() == 720);

    CHECK(f.displays.count() == 3);
    CHECK(f.displays.outputBounds().size() == Size2i{5120, 1080});

    // The host callback observed the new display (the app builds its
    // window and swapchain from it).
    REQUIRE(added.size() == 1);
    CHECK(added[0].id == id);
    CHECK(added[0].name == "right");
    CHECK(added[0].width == 1280);

    const Response list = f.run("display.list");
    REQUIRE(list.ok);
    CHECK(list.result.at("displays").size() == 3);
}

TEST_CASE("display.add without params tiles a 1920x1080 panel right of the wall")
{
    Fixture f;
    const Response resp = f.run("display.add");
    REQUIRE(resp.ok);
    const Id id = resp.result.at("id").get<Id>();
    CHECK(resp.result.at("x").get<std::int32_t>() == 3840); // the wall's right edge
    CHECK(resp.result.at("y").get<std::int32_t>() == 0);
    CHECK(resp.result.at("width").get<std::int32_t>() == 1920);
    CHECK(resp.result.at("height").get<std::int32_t>() == 1080);
    // The default name is derived from the assigned id.
    CHECK(resp.result.at("name").get<std::string>() == "Display " + std::to_string(id));
}

TEST_CASE("display.add rejects non-positive sizes")
{
    Fixture f;
    CHECK_FALSE(f.run("display.add", {{"width", 0}}).ok);
    CHECK_FALSE(f.run("display.add", {{"height", -5}}).ok);
    CHECK(f.displays.count() == 2); // nothing was added
}

TEST_CASE("display.remove drops the display and shrinks the wall")
{
    Fixture f;
    std::vector<Id> removed;
    f.server.setDisplayRemovedFunction([&removed](Id id) { removed.push_back(id); });

    const Response resp = f.run("display.remove", {{"display", f.displayB}});
    REQUIRE(resp.ok);
    CHECK(resp.result.at("display").get<Id>() == f.displayB);
    CHECK(removed == std::vector<Id>{f.displayB});

    CHECK(f.displays.count() == 1);
    CHECK_FALSE(f.displays.find(f.displayB).has_value());
    CHECK(f.displays.outputBounds().size() == Size2i{1920, 1080});

    // Removing an unknown display fails cleanly and changes nothing.
    const Response bad = f.run("display.remove", {{"display", 99}});
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("no display with id 99") != std::string::npos);
    CHECK(f.displays.count() == 1);
}

TEST_CASE("display.add works after all displays were removed")
{
    Fixture f;
    REQUIRE(f.run("display.remove", {{"display", f.displayA}}).ok);
    REQUIRE(f.run("display.remove", {{"display", f.displayB}}).ok);
    CHECK(f.displays.empty());

    // Empty wall: the default x is the (zero) right edge.
    const Response resp = f.run("display.add");
    REQUIRE(resp.ok);
    CHECK(resp.result.at("x").get<std::int32_t>() == 0);
    CHECK(resp.result.at("width").get<std::int32_t>() == 1920);
    CHECK(f.displays.outputBounds().size() == Size2i{1920, 1080});
}

TEST_CASE("display.list reports all displays")
{
    Fixture f;
    const Response resp = f.run("display.list");
    INFO("response: " << resp.result.dump());
    const auto& displays = resp.result.at("displays");
    REQUIRE(displays.size() == 2);
    CHECK(displays[0].at("name").get<std::string>() == "main");
    CHECK(displays[0].at("x").get<std::int32_t>() == 0);
    CHECK(displays[1].at("name").get<std::string>() == "side");
    CHECK(displays[1].at("x").get<std::int32_t>() == 1920);
    // The physical output is reported per display (additive field); the
    // core fixture has no window manager, so it is empty by default.
    CHECK(displays[0].at("monitor").get<std::string>().empty());
}

TEST_CASE("display.set renames the display")
{
    Fixture f;
    const Response resp = f.run("display.set", {{"display", f.displayA}, {"name", "Wall A"}});
    REQUIRE(resp.ok);
    CHECK(resp.result.at("name").get<std::string>() == "Wall A");
    const auto updated = f.displays.find(f.displayA);
    REQUIRE(updated.has_value());
    CHECK((*updated)->name == "Wall A");

    // A non-string name is ignored (the record keeps its name).
    const Response bad = f.run("display.set", {{"display", f.displayA}, {"name", 42}});
    REQUIRE(bad.ok);
    CHECK(bad.result.at("name").get<std::string>() == "Wall A");

    CHECK_FALSE(f.run("display.set", {{"display", 999}, {"name", "nope"}}).ok);
}

TEST_CASE("display.set updates geometry, brightness and enabled state")
{
    Fixture f;
    const Response resp = f.run("display.set", {{"display", f.displayA},
                                                {"x", 10},
                                                {"width", 2048},
                                                {"brightness", 0.75},
                                                {"enabled", false}});
    REQUIRE(resp.ok);
    CHECK(resp.result.at("x").get<std::int32_t>() == 10);
    CHECK(resp.result.at("width").get<std::int32_t>() == 2048);
    CHECK(resp.result.at("brightness").get<double>() == doctest::Approx(0.75));
    CHECK(resp.result.at("enabled").get<bool>() == false);

    const auto updated = f.displays.find(f.displayA);
    REQUIRE(updated.has_value());
    CHECK((*updated)->x == 10);
    CHECK((*updated)->enabled == false);

    CHECK_FALSE(f.run("display.set", {{"display", 999}, {"brightness", 1.0}}).ok);
}

TEST_CASE("display.set toggles the status overlay")
{
    Fixture f;
    // Off by default; the field is present in display records (additive).
    const Response list = f.run("display.list", {});
    REQUIRE(list.ok);
    CHECK(list.result.at("displays").at(0).at("statusOverlay").get<bool>() == false);

    const Response on = f.run("display.set", {{"display", f.displayA}, {"statusOverlay", true}});
    REQUIRE(on.ok);
    CHECK(on.result.at("statusOverlay").get<bool>() == true);
    const auto updated = f.displays.find(f.displayA);
    REQUIRE(updated.has_value());
    CHECK((*updated)->statusOverlay == true);

    const Response off = f.run("display.set", {{"display", f.displayA}, {"statusOverlay", false}});
    REQUIRE(off.ok);
    CHECK(off.result.at("statusOverlay").get<bool>() == false);
}

TEST_CASE("display.warp.set installs a warp grid")
{
    Fixture f;
    // 2x2 grid: all corners identity.
    nlohmann::json points = nlohmann::json::array();
    for (const auto& uv : {std::array<float, 2>{0, 0},
                           std::array<float, 2>{1, 0},
                           std::array<float, 2>{0, 1},
                           std::array<float, 2>{1, 1}})
    {
        points.push_back(nlohmann::json::array({uv[0], uv[1]}));
    }
    const Response resp = f.run("display.warp.set", {{"display", f.displayA}, {"points", points}});
    REQUIRE(resp.ok);
    CHECK(resp.result.at("rows").get<int>() == 2);
    CHECK(resp.result.at("cols").get<int>() == 2);
    CHECK(resp.result.at("mode").get<std::string>() == "grid"); // default
    const auto display = f.displays.find(f.displayA);
    REQUIRE(display.has_value());
    CHECK((*display)->warp.rows() == 2);
    CHECK((*display)->warp.interpolation() == WarpInterpolation::Bilinear);
    CHECK((*display)->warp.isIdentity());

    // Shear the top-right corner and verify the warp samples accordingly.
    nlohmann::json shear = nlohmann::json::array();
    for (const auto& uv : {std::array<float, 2>{0, 0},
                           std::array<float, 2>{0.85f, 0.1f},
                           std::array<float, 2>{0, 1},
                           std::array<float, 2>{1, 1}})
    {
        shear.push_back(nlohmann::json::array({uv[0], uv[1]}));
    }
    REQUIRE(f.run("display.warp.set", {{"display", f.displayA}, {"points", shear}}).ok);
    CHECK_FALSE((*display)->warp.isIdentity());

    // The interpolation mode: "spline" switches the map, "grid" switches
    // back, anything else is rejected.
    const Response spline =
        f.run("display.warp.set", {{"display", f.displayA}, {"points", shear}, {"mode", "spline"}});
    REQUIRE(spline.ok);
    CHECK(spline.result.at("mode").get<std::string>() == "spline");
    CHECK((*display)->warp.interpolation() == WarpInterpolation::Spline);
    const Response grid =
        f.run("display.warp.set", {{"display", f.displayA}, {"points", shear}, {"mode", "grid"}});
    REQUIRE(grid.ok);
    CHECK((*display)->warp.interpolation() == WarpInterpolation::Bilinear);
    CHECK_FALSE(
        f.run("display.warp.set", {{"display", f.displayA}, {"points", shear}, {"mode", "bezier"}})
            .ok);
    CHECK_FALSE(
        f.run("display.warp.set", {{"display", f.displayA}, {"points", shear}, {"mode", 3}}).ok);

    // The display object reports the current warp (mode, grid, absolute
    // node positions) so clients can restore their editor state.
    const Response list = f.run("display.list");
    REQUIRE(list.ok);
    const auto& reported =
        list.result.at("displays")
            .at(0)
            .at("warp");
    CHECK(reported.at("mode").get<std::string>() == "grid");
    CHECK(reported.at("rows").get<int>() == 2);
    CHECK(reported.at("cols").get<int>() == 2);
    REQUIRE(reported.at("points").size() == 4);
    CHECK(reported.at("points")
              .at(1)
              .at(0)
              .get<double>() == doctest::Approx(0.85).epsilon(1e-6));

    // Invalid grids.
    nlohmann::json bad = nlohmann::json::array();
    for (int i = 0; i < 6; ++i)
    {
        bad.push_back(nlohmann::json::array({0.0, 0.0}));
    }
    CHECK_FALSE(f.run("display.warp.set", {{"display", f.displayA}, {"points", bad}}).ok);
    CHECK_FALSE(f.run("display.warp.set", {{"display", f.displayA}}).ok);
    CHECK_FALSE(f.run("display.warp.set", {{"display", 999}, {"points", points}}).ok);
}

TEST_CASE("display.set toggles always-on-top (default on)")
{
    Fixture f;
    // Default: on.
    const Response list = f.run("display.list");
    REQUIRE(list.ok);
    CHECK(list.result.at("displays").at(0).at("alwaysOnTop").get<bool>() == true);

    // Off.
    const Response off = f.run("display.set", {{"display", f.displayA}, {"alwaysOnTop", false}});
    REQUIRE(off.ok);
    CHECK(off.result.at("alwaysOnTop").get<bool>() == false);
    const auto display = f.displays.find(f.displayA);
    REQUIRE(display.has_value());
    CHECK((*display)->alwaysOnTop == false);

    // Back on + invalid ids rejected.
    const Response on = f.run("display.set", {{"display", f.displayA}, {"alwaysOnTop", true}});
    REQUIRE(on.ok);
    CHECK(on.result.at("alwaysOnTop").get<bool>() == true);
    CHECK((*display)->alwaysOnTop == true);
    CHECK_FALSE(f.run("display.set", {{"display", 999}, {"alwaysOnTop", true}}).ok);
}

TEST_CASE("display.set toggles vsync (default on)")
{
    Fixture f;
    // Default: on.
    const Response list = f.run("display.list");
    REQUIRE(list.ok);
    CHECK(list.result.at("displays").at(0).at("vsync").get<bool>() == true);

    // Off (the playback engine reacts by recreating the display's swapchain
    // in the FIFO->MAILBOX present mode).
    const Response off = f.run("display.set", {{"display", f.displayA}, {"vsync", false}});
    REQUIRE(off.ok);
    CHECK(off.result.at("vsync").get<bool>() == false);
    const auto display = f.displays.find(f.displayA);
    REQUIRE(display.has_value());
    CHECK((*display)->vsync == false);

    // Back on + invalid ids rejected.
    const Response on = f.run("display.set", {{"display", f.displayA}, {"vsync", true}});
    REQUIRE(on.ok);
    CHECK(on.result.at("vsync").get<bool>() == true);
    CHECK((*display)->vsync == true);
    CHECK_FALSE(f.run("display.set", {{"display", 999}, {"vsync", true}}).ok);
}

TEST_CASE("show.save then show.load round trips the show")
{
    Fixture f;
    const auto path = tempFile("roundtrip", "placeholder");

    const Response save = f.run("show.save", {{"path", path.string()}});
    REQUIRE(save.ok);
    REQUIRE(std::filesystem::exists(path));

    const auto parsed = nlohmann::json::parse(readTempFile(path));
    CHECK(parsed.at("app").get<std::string>() == "mediamax-show");
    CHECK(parsed.at("version").get<int>() == 1);
    CHECK(parsed.at("name").get<std::string>() == "Test Show");

    const Response load = f.run("show.load", {{"path", path.string()}});
    REQUIRE(load.ok);
    CHECK(load.result.at("name").get<std::string>() == "Test Show");
    CHECK(load.result.at("scenes").get<std::uint64_t>() == 2);
    CHECK(load.result.at("media").get<std::uint64_t>() == 2);

    // The show file's media ids are file-local and were remapped to library
    // assets. Both files were already loaded by the fixture, so the deduped
    // remap lands on the existing assets and the library does not grow.
    const Show& show = f.engine.show();
    CHECK(show.media.size() == 2);
    CHECK(f.media.count() == 2);
    for (const auto& ref : show.media)
    {
        CHECK(ref.id != kInvalidId);
        CHECK(f.media.find(ref.id).has_value());
    }
    // The video layer now points at the remapped (existing) asset.
    const Scene* sceneA = show.findScene(1);
    REQUIRE(sceneA);
    const Layer* videoLayer = nullptr;
    for (const auto& layer : sceneA->layers)
    {
        if (layer.mediaId != kInvalidId)
        {
            videoLayer = &layer;
        }
    }
    REQUIRE(videoLayer);
    CHECK(videoLayer->mediaId == kMediaA);
    CHECK(f.media.find(videoLayer->mediaId).has_value());

    // The engine was reset by the load.
    CHECK(f.engine.state() == PlayState::Stopped);
    CHECK(f.engine.time().isZero());

    std::filesystem::remove(path);
}

TEST_CASE("show.load rejects missing and invalid files")
{
    Fixture f;
    const Response missing = f.run("show.load", {{"path", "definitely/missing/show.json"}});
    CHECK_FALSE(missing.ok);
    CHECK(missing.error.find("cannot read show file") != std::string::npos);

    const auto garbage = tempFile("garbage", "this is not json");
    const Response bad = f.run("show.load", {{"path", garbage.string()}});
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("invalid JSON") != std::string::npos);
    std::filesystem::remove(garbage);

    const auto wrongApp = tempFile("wrongapp", R"({"app":"other-app","version":1})");
    const Response wrong = f.run("show.load", {{"path", wrongApp.string()}});
    CHECK_FALSE(wrong.ok);
    CHECK(wrong.error.find("invalid show file") != std::string::npos);
    std::filesystem::remove(wrongApp);
}

TEST_CASE("show.load tolerates missing media files")
{
    Fixture f;
    nlohmann::json show = R"({
        "app": "mediamax-show",
        "version": 1,
        "name": "Partial",
        "frameRate": {"num": 30, "den": 1},
        "media": [{"id": 1, "path": "missing-file.mp4"}],
        "scenes": [
            {"id": 1, "name": "S", "layers": [
                {"id": 10, "name": "vid", "mediaId": 1}
            ]}
        ]
    })"_json;
    const auto path = tempFile("partial", show.dump());

    const Response resp = f.run("show.load", {{"path", path.string()}});
    REQUIRE(resp.ok); // missing media is a warning, not a failure
    CHECK(resp.result.at("name").get<std::string>() == "Partial");
    // The layer keeps its (unresolved) show-local media id.
    const Scene* scene = f.engine.show().findScene(1);
    REQUIRE(scene);
    CHECK(scene->layers[0].mediaId == 1);

    std::filesystem::remove(path);
}

TEST_CASE("show.load resolves relative media against the show file's directory")
{
    Fixture f;
    // The show file lives in the temp directory, not in the process CWD:
    // its relative media path must resolve against the file's parent.
    const nlohmann::json show = R"({
        "app": "mediamax-show",
        "version": 1,
        "name": "Basedir",
        "frameRate": {"num": 30, "den": 1},
        "media": [{"id": 1, "path": "media/video-a.mp4"}],
        "scenes": [
            {"id": 1, "name": "S", "layers": [
                {"id": 10, "name": "vid", "mediaId": 1}
            ]}
        ]
    })"_json;
    const auto path = tempFile("basedir", show.dump());
    const Response resp = f.run("show.load", {{"path", path.string()}});
    REQUIRE(resp.ok);

    // The load was attempted at the show file's directory, not the CWD.
    const auto expected = std::filesystem::path(path).parent_path() / "media/video-a.mp4";
    CHECK(f.lastLoadedPath == expected.string());

    // And it succeeded: the layer was remapped onto the library asset.
    const Scene* scene = f.engine.show().findScene(1);
    REQUIRE(scene);
    CHECK(scene->layers[0].mediaId == kMediaA);

    std::filesystem::remove(path);
}

TEST_CASE("show.load leaves absolute media paths untouched")
{
    Fixture f;
    const auto absolute =
        (std::filesystem::temp_directory_path() / "mediamax-abs" / "video-a.mp4").string();
    nlohmann::json show = R"({
        "app": "mediamax-show",
        "version": 1,
        "name": "AbsMedia",
        "frameRate": {"num": 30, "den": 1},
        "media": [{"id": 1, "path": "media/video-a.mp4"}],
        "scenes": [
            {"id": 1, "name": "S", "layers": [
                {"id": 10, "name": "vid", "mediaId": 1}
            ]}
        ]
    })"_json;
    show["media"][0]["path"] = absolute;
    const auto path = tempFile("absmedia", show.dump());
    const Response resp = f.run("show.load", {{"path", path.string()}});
    REQUIRE(resp.ok);

    CHECK(f.lastLoadedPath == absolute);
    const Scene* scene = f.engine.show().findScene(1);
    REQUIRE(scene);
    CHECK(scene->layers[0].mediaId == kMediaA);

    std::filesystem::remove(path);
}

TEST_CASE("show.loadJson resolves relative media against the mediaDir parameter")
{
    Fixture f;
    const nlohmann::json doc = R"({
        "app": "mediamax-show",
        "version": 1,
        "name": "DirJson",
        "frameRate": {"num": 30, "den": 1},
        "media": [{"id": 1, "path": "media/video-b.mp4"}],
        "scenes": [
            {"id": 1, "name": "S", "layers": [
                {"id": 10, "name": "vid", "mediaId": 1}
            ]}
        ]
    })"_json;
    const std::string dir = (std::filesystem::temp_directory_path() / "mediamax-dir").string();
    const Response resp = f.run("show.loadJson", {{"show", doc}, {"mediaDir", dir}});
    REQUIRE(resp.ok);

    CHECK(f.lastLoadedPath == (std::filesystem::path(dir) / "media/video-b.mp4").string());
    const Scene* scene = f.engine.show().findScene(1);
    REQUIRE(scene);
    CHECK(scene->layers[0].mediaId == kMediaB);
}

TEST_CASE("show.loadJson loads an inline show document")
{
    Fixture f;
    // Round-trip the currently loaded show through the JSON path: it is the
    // same contract as show.load (engine reset, media ids remapped).
    const nlohmann::json doc = f.engine.show().toJson();
    const Response resp = f.run("show.loadJson", {{"show", doc}});
    REQUIRE(resp.ok);
    CHECK(resp.result.at("name").get<std::string>() == "Test Show");
    CHECK(resp.result.at("scenes").get<std::uint64_t>() == 2);
    CHECK(resp.result.at("media").get<std::uint64_t>() == 2);

    CHECK(f.engine.state() == PlayState::Stopped);
    CHECK(f.engine.time().isZero());
    // The show's media ids remap onto library assets; the fixture already
    // had both files, so the deduped remap lands on the existing assets.
    CHECK(f.media.count() == 2);
    // No mediaDir given: the document's relative media paths are used as
    // written (the last one loaded, in show order).
    CHECK(f.lastLoadedPath == "media/video-b.mp4");
    for (const auto& ref : f.engine.show().media)
    {
        CHECK(ref.id != kInvalidId);
        CHECK(f.media.find(ref.id).has_value());
    }
    // The video layer points at the existing library asset for its file.
    const Scene* sceneA = f.engine.show().findScene(1);
    REQUIRE(sceneA);
    const Layer* videoLayer = nullptr;
    for (const auto& layer : sceneA->layers)
    {
        if (layer.mediaId != kInvalidId)
        {
            videoLayer = &layer;
        }
    }
    REQUIRE(videoLayer);
    CHECK(videoLayer->mediaId == kMediaA);
    CHECK(f.media.find(videoLayer->mediaId).has_value());
}

TEST_CASE("show.loadJson with a displays section reconciles the engine's wall")
{
    Fixture f;
    // The fixture's wall is "main" (0,0,1920,1080) + "side" (1920,0,1920,1080).
    // The document updates "main" (geometry, flags, warp), adds "extra" and
    // no longer defines "side".
    const nlohmann::json doc = R"({
        "app": "mediamax-show",
        "version": 1,
        "name": "Wall show",
        "frameRate": {"num": 60, "den": 1},
        "displays": [
            {"id": 1, "name": "main", "x": 0, "y": 0, "width": 3840, "height": 2160,
             "brightness": 0.7, "enabled": true, "alwaysOnTop": false,
             "statusOverlay": true, "vsync": false,
             "warp": {"mode": "spline", "rows": 2, "cols": 2,
                      "points": [[0,0],[1,0],[0,1],[1,1]]}},
            {"id": 2, "name": "extra", "x": 3840, "y": 0, "width": 1920, "height": 1080}
        ]
    })"_json;
    const Response resp = f.run("show.loadJson", {{"show", doc}});
    REQUIRE(resp.ok);
    CHECK(resp.result.at("displays").get<std::uint64_t>() == 2);

    const Response list = f.run("display.list");
    REQUIRE(list.ok);
    const auto& displays = list.result.at("displays");
    REQUIRE(displays.size() == 2);

    nlohmann::json mainJson;
    nlohmann::json extraJson;
    for (const auto& d : displays)
    {
        if (d.at("name") == "main")
        {
            mainJson = d;
        }
        else if (d.at("name") == "extra")
        {
            extraJson = d;
        }
    }
    REQUIRE(mainJson.is_object());
    REQUIRE(extraJson.is_object());

    // "main" was updated in place: it keeps its engine id.
    CHECK(mainJson.at("id").get<std::uint64_t>() == f.displayA);
    CHECK(mainJson.at("x").get<int>() == 0);
    CHECK(mainJson.at("y").get<int>() == 0);
    CHECK(mainJson.at("width").get<int>() == 3840);
    CHECK(mainJson.at("height").get<int>() == 2160);
    CHECK(mainJson.at("brightness").get<double>() == doctest::Approx(0.7));
    CHECK(mainJson.at("alwaysOnTop").get<bool>() == false);
    CHECK(mainJson.at("statusOverlay").get<bool>() == true);
    CHECK(mainJson.at("vsync").get<bool>() == false);
    CHECK(mainJson.at("warp").at("mode") == "spline");
    CHECK(mainJson.at("warp").at("rows") == 2);

    // "extra" was added with the document's geometry.
    CHECK(extraJson.at("x").get<int>() == 3840);
    CHECK(extraJson.at("y").get<int>() == 0);
    CHECK(extraJson.at("width").get<int>() == 1920);
    CHECK(extraJson.at("height").get<int>() == 1080);
    CHECK(extraJson.at("enabled").get<bool>() == true);
    CHECK(extraJson.at("vsync").get<bool>() == true); // document default

    // "side" is not in the document anymore: it is gone.
    for (const auto& d : displays)
    {
        CHECK(d.at("name") != "side");
    }
}

TEST_CASE("show.loadJson without a displays section leaves the wall untouched")
{
    Fixture f;
    const nlohmann::json doc = f.engine.show().toJson(); // no displays
    const Response resp = f.run("show.loadJson", {{"show", doc}});
    REQUIRE(resp.ok);
    CHECK(resp.result.at("displays").get<std::uint64_t>() == 0);

    const Response list = f.run("display.list");
    REQUIRE(list.ok);
    CHECK(list.result.at("displays").size() == 2); // main + side, as before
}

TEST_CASE("show.loadJson validates its parameter")
{
    Fixture f;
    const Response missing = f.run("show.loadJson");
    CHECK_FALSE(missing.ok);
    CHECK(missing.error.find("'show' object") != std::string::npos);

    const Response wrongType = f.run("show.loadJson", {{"show", "not an object"}});
    CHECK_FALSE(wrongType.ok);
    CHECK(wrongType.error.find("'show' object") != std::string::npos);

    const Response invalid = f.run("show.loadJson",
                                   {{"show", {{"app", "other-app"}, {"version", 1}}}});
    CHECK_FALSE(invalid.ok);
    CHECK(invalid.error.find("invalid show JSON") != std::string::npos);
}

TEST_CASE("show.loadJson tolerates missing media like show.load")
{
    Fixture f;
    const nlohmann::json doc = R"({
        "app": "mediamax-show",
        "version": 1,
        "name": "PartialJson",
        "frameRate": {"num": 30, "den": 1},
        "media": [{"id": 1, "path": "missing-file.mp4"}],
        "scenes": [
            {"id": 1, "name": "S", "layers": [
                {"id": 10, "name": "vid", "mediaId": 1}
            ]}
        ]
    })"_json;
    const Response resp = f.run("show.loadJson", {{"show", doc}});
    REQUIRE(resp.ok); // missing media is a warning, not a failure
    CHECK(resp.result.at("name").get<std::string>() == "PartialJson");
    const Scene* scene = f.engine.show().findScene(1);
    REQUIRE(scene);
    CHECK(scene->layers[0].mediaId == 1);
}

TEST_CASE("show.loadJson resets live playback by default")
{
    Fixture f;
    f.engine.play();
    f.engine.seek(Time::fromSeconds(3.0));
    CHECK(f.engine.setSceneNow(1)); // scene 2 is on the timeline at 3 s
    CHECK(f.engine.setLayerOpacity(11, 0.25f));

    const Response resp = f.run("show.loadJson", {{"show", f.engine.show().toJson()}});
    REQUIRE(resp.ok);
    CHECK(f.engine.state() == PlayState::Stopped);
    CHECK(f.engine.time().isZero());
    CHECK_FALSE(f.engine.hasSceneOverride());
    // The layer override is gone: at 1 s scene 1 is on the timeline, and the
    // background layer renders at its default opacity again.
    const FrameState fs = f.engine.frameStateAt(Time::fromSeconds(1.0));
    const LayerFrame* bg = nullptr;
    for (const auto& layer : fs.layers)
    {
        if (layer.layerId == 11)
        {
            bg = &layer;
        }
    }
    REQUIRE(bg);
    CHECK(bg->opacity == doctest::Approx(1.0f));
}

TEST_CASE("show.loadJson with preserveState keeps live playback going")
{
    Fixture f;
    f.engine.play();
    f.engine.seek(Time::fromSeconds(3.0)); // timeline scene at 3 s is 2 (cue at 2 s)
    CHECK(f.engine.setSceneNow(1));        // operator override back to scene 1
    CHECK(f.engine.setLayerOpacity(11, 0.25f));

    const Response resp =
        f.run("show.loadJson", {{"show", f.engine.show().toJson()}, {"preserveState", true}});
    REQUIRE(resp.ok);

    CHECK(f.engine.state() == PlayState::Playing);
    CHECK(f.engine.time() == Time::fromSeconds(3.0));
    CHECK(f.engine.hasSceneOverride());
    CHECK(f.engine.activeSceneAt(f.engine.time()) == 1); // override still wins
    const FrameState fs = f.engine.frameState();
    const LayerFrame* bg = nullptr;
    for (const auto& layer : fs.layers)
    {
        if (layer.layerId == 11)
        {
            bg = &layer;
        }
    }
    REQUIRE(bg);
    CHECK(bg->opacity == doctest::Approx(0.25f)); // layer override survived
}

TEST_CASE("show.loadJson with preserveState drops what the new show lacks")
{
    Fixture f;
    f.engine.play();
    f.engine.seek(Time::fromSeconds(6.5));
    CHECK(f.engine.setSceneNow(2)); // scene 2 (layers 20)
    CHECK(f.engine.setLayerOpacity(20, 0.25f));

    // The edited show: scene 2 (and its cues) are gone, the duration is
    // shorter than the current playhead.
    Show next = mediamax::test::makeTestShow();
    next.name = "Trimmed";
    next.duration = Time::fromSeconds(3.0);
    next.scenes.erase(std::remove_if(next.scenes.begin(), next.scenes.end(),
                                     [](const Scene& s) { return s.id == 2; }),
                      next.scenes.end());
    next.cues.erase(std::remove_if(next.cues.begin(), next.cues.end(),
                                   [](const Cue& c) {
                                       return std::holds_alternative<PlayScene>(c.action) &&
                                              std::get<PlayScene>(c.action).scene == 2;
                                   }),
                    next.cues.end());

    const Response resp =
        f.run("show.loadJson", {{"show", next.toJson()}, {"preserveState", true}});
    REQUIRE(resp.ok);

    CHECK(f.engine.state() == PlayState::Playing); // still playing the trimmed show
    CHECK(f.engine.time() == Time::fromSeconds(3.0)); // clamped to the new duration
    CHECK_FALSE(f.engine.hasSceneOverride());         // scene 2 no longer exists
    // Layer 20 is gone, so its override cannot survive.
    const FrameState fs = f.engine.frameState();
    for (const auto& layer : fs.layers)
    {
        CHECK(layer.layerId != 20);
    }
}

TEST_CASE("scene.clear releases the operator scene override")
{
    Fixture f;
    // No override yet: cleared=0, the timeline's scene (1 at t=0) is active.
    const Response none = f.run("scene.clear");
    REQUIRE(none.ok);
    CHECK(none.result.at("cleared").get<int>() == 0);
    CHECK(none.result.at("scene").get<std::uint64_t>() == 1);

    const Response set = f.run("scene.set", {{"index", 1}});
    REQUIRE(set.ok);
    CHECK(set.result.at("scene").get<std::uint64_t>() == 2);

    const Response cleared = f.run("scene.clear");
    REQUIRE(cleared.ok);
    CHECK(cleared.result.at("cleared").get<int>() == 1);
    // Back on the timeline: at t=0 the playScene cue selects scene 1.
    CHECK(cleared.result.at("scene").get<std::uint64_t>() == 1);
}

TEST_CASE("show.info summarizes the loaded show")
{
    Fixture f;
    const Response resp = f.run("show.info");
    REQUIRE(resp.ok);
    CHECK(resp.result.at("name").get<std::string>() == "Test Show");
    CHECK(resp.result.at("frameRate").get<std::string>() == "30");
    CHECK(resp.result.at("duration").get<double>() == doctest::Approx(7.0));
    CHECK(resp.result.at("scenes").size() == 2);
    CHECK(resp.result.at("cues").get<std::uint64_t>() == 5);
}

TEST_CASE("frame.state reports the wall and the current frame's layers")
{
    Fixture f;
    const Response resp = f.run("frame.state");
    REQUIRE(resp.ok);
    const nlohmann::json& r = resp.result;

    // The wall: two 1920x1080 displays tiled side by side.
    CHECK(r.at("sceneSize") == nlohmann::json({{"width", 3840}, {"height", 1080}}));
    const nlohmann::json& displays = r.at("displays");
    REQUIRE(displays.size() == 2);
    CHECK(displays[0].at("id") == f.displayA);
    CHECK(displays[0].at("name").get<std::string>() == "main");
    CHECK(displays[0].at("x") == 0);
    CHECK(displays[0].at("width") == 1920);
    CHECK(displays[0].at("enabled").get<bool>());
    CHECK(displays[1].at("x") == 1920);

    // At t=0 the show plays scene 1: the color background (z 0) under the
    // video layer (z 1).
    const nlohmann::json& layers = r.at("layers");
    REQUIRE(layers.size() == 2);

    const nlohmann::json& bg = layers[0];
    CHECK(bg.at("id") == 11);
    CHECK(bg.at("name").get<std::string>() == "bg");
    CHECK(bg.at("content").get<std::string>() == "color");
    const nlohmann::json& color = bg.at("color");
    REQUIRE(color.size() == 4);
    CHECK(color[0].get<double>() == doctest::Approx(0.1f));
    CHECK(color[1].get<double>() == doctest::Approx(0.2f));
    CHECK(color[2].get<double>() == doctest::Approx(0.3f));
    CHECK(color[3].get<double>() == doctest::Approx(1.0f));
    CHECK(bg.at("opacity").get<double>() == doctest::Approx(1.0f));
    CHECK(bg.at("zIndex") == 0);
    CHECK(!bg.contains("mediaId"));
    // A color layer fills the output space (identity transform, centered
    // anchor at the origin of a 3840x1080 wall would be off-center, but the
    // fixture layer has the default transform: anchor 0.5 at x=y=0).
    CHECK(bg.at("corners") == cornerJson(Transform2D::identity(), Size2i{3840, 1080}));

    const nlohmann::json& video = layers[1];
    CHECK(video.at("id") == 10);
    CHECK(video.at("name").get<std::string>() == "videoA");
    CHECK(video.at("content").get<std::string>() == "video");
    CHECK(video.at("mediaId") == kMediaA);
    CHECK(!video.contains("color"));
    // The media sits at its native 640x360 size under the default transform.
    CHECK(video.at("corners") == cornerJson(Transform2D::identity(), Size2i{640, 360}));
}

TEST_CASE("frame.state projects rotated and scaled layers")
{
    MediaLibrary media;
    DisplayManager displays;
    displays.addDisplay("wall", 0, 0, 100, 100);
    PlaybackEngine engine(media);

    Show show;
    show.name = "frame-state test";
    show.frameRate = FrameRate::fps30();
    show.duration = Time::fromSeconds(5.0);
    Scene scene;
    scene.id = 1;
    scene.name = "S";
    Layer layer;
    layer.id = 7;
    layer.name = "rot";
    layer.color = {1.f, 0.f, 0.f, 1.f};
    layer.transform.x = 50.f;
    layer.transform.y = 50.f;
    layer.transform.rotation = 0.7853981634f; // ~45 degrees
    layer.transform.scaleX = 2.0f;
    layer.transform.scaleY = 2.0f;
    layer.transform.anchorX = 0.25f;
    layer.transform.anchorY = 0.75f;
    scene.layers.push_back(layer);
    show.scenes.push_back(scene);
    show.addCue(Cue{Time::zero(), "start", PlayScene{1}});
    engine.loadShow(show);

    CommandServer server(engine, media, displays, "t");
    const Response resp =
        server.handle(Command{1, "frame.state", nlohmann::json::object()});
    REQUIRE(resp.ok);
    const nlohmann::json& r = resp.result;
    CHECK(r.at("sceneSize") == nlohmann::json({{"width", 100}, {"height", 100}}));
    const nlohmann::json& layers = r.at("layers");
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].at("content").get<std::string>() == "color");

    // A color layer is projected at the output-space size.
    Transform2D expected;
    expected.x = 50.f;
    expected.y = 50.f;
    expected.rotation = 0.7853981634f;
    expected.scaleX = 2.0f;
    expected.scaleY = 2.0f;
    expected.anchorX = 0.25f;
    expected.anchorY = 0.75f;
    const nlohmann::json want = cornerJson(expected, Size2i{100, 100});
    const nlohmann::json& got = layers[0].at("corners");
    REQUIRE(got.size() == 4);
    for (int i = 0; i < 4; ++i)
    {
        CHECK(got[static_cast<std::size_t>(i)][0].get<double>() ==
              doctest::Approx(want[static_cast<std::size_t>(i)][0].get<double>()));
        CHECK(got[static_cast<std::size_t>(i)][1].get<double>() ==
              doctest::Approx(want[static_cast<std::size_t>(i)][1].get<double>()));
    }

    // The rotation actually turned the quad (not the flat box) and preserved
    // its area (200x200 = 40000).
    Transform2D flat = expected;
    flat.rotation = 0.f;
    const nlohmann::json flatCorners = cornerJson(flat, Size2i{100, 100});
    bool differs = false;
    for (int i = 0; i < 4; ++i)
    {
        differs = differs || (got[static_cast<std::size_t>(i)] !=
                              flatCorners[static_cast<std::size_t>(i)]);
    }
    CHECK(differs);
    double area = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        const int j = (i + 1) % 4;
        area += got[static_cast<std::size_t>(i)][0].get<double>() *
                    got[static_cast<std::size_t>(j)][1].get<double>() -
                got[static_cast<std::size_t>(j)][0].get<double>() *
                    got[static_cast<std::size_t>(i)][1].get<double>();
    }
    CHECK(std::abs(area * 0.5) == doctest::Approx(40000.0).epsilon(1e-3));
}

TEST_CASE("frame.state omits hidden layers")
{
    Fixture f;
    REQUIRE(f.run("layer.set", {{"layer", 11}, {"visible", false}}).ok);
    const Response resp = f.run("frame.state");
    REQUIRE(resp.ok);
    const nlohmann::json& layers = resp.result.at("layers");
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].at("id") == 10);
}

TEST_CASE("media.thumbnail decodes through the app callback")
{
    Fixture f;
    // The app-side implementation: library lookup + makeThumbnail (the
    // playback app re-locks around the lookup; nothing to lock here).
    f.server.setThumbnailFunction(
        [&f](Id id, int maxW, int maxH, std::vector<std::uint8_t>& rgba, int& w, int& h,
             std::string& error) {
            const auto asset = f.media.find(id);
            if (!asset)
            {
                error = "no media with id " + std::to_string(id);
                return false;
            }
            return media::makeThumbnail(f.media.sourceFactory(), (*asset)->path,
                                        (*asset)->metadata, maxW, maxH, rgba, w, h, error);
        });

    // Explicit size: the 640x360 source fits 160x90 exactly.
    const Response r1 = f.run("media.thumbnail",
                              {{"id", kMediaA}, {"width", 160}, {"height", 90}});
    REQUIRE(r1.ok);
    CHECK(r1.result.at("id") == kMediaA);
    CHECK(r1.result.at("width") == 160);
    CHECK(r1.result.at("height") == 90);
    std::vector<std::uint8_t> pixels;
    REQUIRE(util::base64Decode(r1.result.at("data").get<std::string>(), pixels));
    CHECK(pixels.size() == static_cast<std::size_t>(160) * 90 * 4);

    // Defaults: 320x180.
    const Response r2 = f.run("media.thumbnail", {{"id", kMediaA}});
    REQUIRE(r2.ok);
    CHECK(r2.result.at("width") == 320);
    CHECK(r2.result.at("height") == 180);

    // Out-of-range sizes are clamped, not errors.
    const Response r3 =
        f.run("media.thumbnail", {{"id", kMediaA}, {"width", 4000}, {"height", 9000}});
    REQUIRE(r3.ok);
    CHECK(r3.result.at("width") == 640);
    CHECK(r3.result.at("height") == 360);

    // Unknown media id: clean failure from the callback.
    const Response r4 = f.run("media.thumbnail", {{"id", 99}});
    CHECK_FALSE(r4.ok);
    CHECK(r4.error.find("no media with id 99") != std::string::npos);
}

TEST_CASE("media.thumbnail fails cleanly without a callback or with bad params")
{
    Fixture f; // no thumbnail callback installed

    const Response r1 = f.run("media.thumbnail", {{"id", kMediaA}});
    CHECK_FALSE(r1.ok);
    CHECK(r1.error.find("not available") != std::string::npos);

    const Response r2 = f.run("media.thumbnail"); // missing id
    CHECK_FALSE(r2.ok);
    CHECK(r2.error.find("id") != std::string::npos);

    const Response r3 = f.run("media.thumbnail", {{"id", kMediaA}, {"width", "big"}});
    CHECK_FALSE(r3.ok);
    CHECK(r3.error.find("width") != std::string::npos);
}

TEST_CASE("every supported method is dispatched")
{
    // Guard against a handler falling through to "unknown method" for a
    // method that is documented as supported.
    Fixture f;
    for (const auto& method : CommandServer::supportedMethods())
    {
        if (method == "ping" || method == "status" || method == "frame.state" ||
            method == "scene.list" || method == "scene.clear" || method == "media.list" ||
            method == "display.list" || method == "show.info" || method == "layer.clear")
        {
            // These succeed with no parameters at all.
            CHECK(f.run(method).ok);
        }
        else
        {
            // With parameters absent these may fail validation, but never
            // with "unknown method".
            const Response resp = f.run(method);
            CHECK(resp.error.find("unknown method") == std::string::npos);
        }
    }
}

} // TEST_SUITE("CommandServer")
