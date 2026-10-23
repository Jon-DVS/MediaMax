#pragma once

// Shared fixtures for the MediaMax test suite.

#include <doctest/doctest.h>

#include "mediamax/media/MediaLibrary.h"
#include "mediamax/media/TestPatternSource.h"
#include "mediamax/playback/PlaybackEngine.h"
#include "mediamax/show/Show.h"
#include "mediamax/time/Time.h"

namespace doctest
{
// Print Time as "value/scale s" so failed checks are diagnosable.
template <>
struct StringMaker<mediamax::Time>
{
    static String convert(const mediamax::Time& t)
    {
        const std::string s = std::to_string(t.value()) + "/" + std::to_string(t.scale()) + " s";
        return String(s.c_str(), s.size());
    }
};
} // namespace doctest

namespace mediamax::test
{

// Fixture media ids. The test media library loads its assets in a fixed
// order, so these ids are deterministic:
//   video-a.mp4 -> id 1 (color bars, 4 s, 640x360 @ 30 fps)
//   video-b.mp4 -> id 2 (checkerboard, 4 s, 640x360 @ 30 fps)
inline constexpr Id kMediaA = 1;
inline constexpr Id kMediaB = 2;

// Builds a deterministic 2-scene show:
//   scene 1 "A": layer 10 (media A, video, in-point 1s), layer 11 (solid color)
//   scene 2 "B": layer 20 (media B, video, loops)
// Timeline:
//   t=0    playScene(1)
//   t=2.0  playScene(2)
//   t=4.0  setLayerOpacity(20, 0.5)
//   t=5.0  playScene(1)
//   t=6.0  stop
// Frame rate: 30 fps. Duration: 7 seconds.
inline Show makeTestShow()
{
    Show show;
    show.name = "Test Show";
    show.frameRate = FrameRate::fps30();
    show.duration = Time::fromSeconds(7.0);

    show.media.push_back(MediaReference{kMediaA, "media/video-a.mp4"});
    show.media.push_back(MediaReference{kMediaB, "media/video-b.mp4"});

    Scene sceneA;
    sceneA.id = 1;
    sceneA.name = "A";

    Layer layerA;
    layerA.id = 10;
    layerA.name = "videoA";
    layerA.mediaId = kMediaA;
    layerA.inPoint = Time::fromSeconds(1.0);
    layerA.zIndex = 1;

    Layer colorLayer;
    colorLayer.id = 11;
    colorLayer.name = "bg";
    colorLayer.color = {0.1f, 0.2f, 0.3f, 1.f};
    colorLayer.zIndex = 0;

    sceneA.layers.push_back(colorLayer);
    sceneA.layers.push_back(layerA);

    Scene sceneB;
    sceneB.id = 2;
    sceneB.name = "B";

    Layer layerB;
    layerB.id = 20;
    layerB.name = "videoB";
    layerB.mediaId = kMediaB;
    layerB.inPoint = Time::fromSeconds(0.0);
    layerB.loop = true;
    layerB.zIndex = 1;

    sceneB.layers.push_back(layerB);

    show.scenes.push_back(sceneA);
    show.scenes.push_back(sceneB);

    show.addCue(Cue{Time::zero(), "start", PlayScene{1}});
    show.addCue(Cue{Time::fromSeconds(2.0), "toB", PlayScene{2}});
    show.addCue(Cue{Time::fromSeconds(4.0), "fadeB", SetLayerOpacity{20, 0.5f}});
    show.addCue(Cue{Time::fromSeconds(5.0), "backA", PlayScene{1}});
    show.addCue(Cue{Time::fromSeconds(6.0), "end", StopPlayback{}});

    return show;
}

// A media library with the two test patterns loaded (ids kMediaA/kMediaB).
inline MediaLibrary makeLoadedMediaLibrary()
{
    MediaLibrary library;
    library.setSourceFactory(
        [](const std::string& path, std::string& error) -> std::shared_ptr<IMediaSource> {
            if (path == "media/video-a.mp4")
            {
                return std::make_shared<TestPatternSource>(
                    640, 360, FrameRate::fps30(), Time::fromSeconds(4.0),
                    TestPatternKind::ColorBars);
            }
            if (path == "media/video-b.mp4")
            {
                return std::make_shared<TestPatternSource>(
                    640, 360, FrameRate::fps30(), Time::fromSeconds(4.0),
                    TestPatternKind::Checkerboard);
            }
            error = "unknown test media: " + path;
            return nullptr;
        });

    std::string error;
    const Id a = library.load("media/video-a.mp4", error);
    const Id b = library.load("media/video-b.mp4", error);
    if (a != kMediaA || b != kMediaB)
    {
        // Fixture invariant: deterministic load order.
        throw std::runtime_error("test media library ids changed - update fixtures");
    }
    return library;
}

// A media library whose factory can create patterns on demand (unloaded).
inline MediaLibrary makeMediaLibrary()
{
    MediaLibrary library;
    library.setSourceFactory(
        [](const std::string& path, std::string& error) -> std::shared_ptr<IMediaSource> {
            if (path == "media/video-a.mp4" || path == "media/video-b.mp4")
            {
                const auto kind = path == "media/video-a.mp4" ? TestPatternKind::ColorBars
                                                              : TestPatternKind::Checkerboard;
                return std::make_shared<TestPatternSource>(
                    640, 360, FrameRate::fps30(), Time::fromSeconds(4.0), kind);
            }
            error = "unknown test media: " + path;
            return nullptr;
        });
    return library;
}

inline PlaybackEngine makeTestEngine(MediaLibrary& media, const Show& show = makeTestShow())
{
    PlaybackEngine engine(media);
    engine.loadShow(show);
    return engine;
}

// Loads the show's referenced media into the library through the factory.
inline void loadShowMedia(MediaLibrary& media, const Show& show)
{
    std::string error;
    for (const auto& ref : show.media)
    {
        media.load(ref.path, error);
    }
}

} // namespace mediamax::test
