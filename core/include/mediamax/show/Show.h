#pragma once

// The show: the complete description of a media presentation.
//
// A show contains:
//   - a frame rate (the master rate of the show timeline)
//   - media references (assets the show uses, by id + file path)
//   - scenes (each a set of layers)
//   - a timeline of cues (a pure function of show time, see Cue.h)
//
// Shows serialize to a human-readable JSON document (see docs/SHOW_FORMAT.md)
// which doubles as the interchange format.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mediamax/show/Cue.h"
#include "mediamax/show/Scene.h"
#include "mediamax/time/FrameRate.h"
#include "mediamax/time/Time.h"

namespace mediamax
{

struct MediaReference
{
    Id id = kInvalidId;
    std::string path;
};

// A playback system the show runs on (issue 18: the show file is where the
// control GUI stores its system list). Client-side targeting info: the
// engine parses and stores the field but does not use it for rendering.
struct SystemReference
{
    std::string name;
    std::string host;
    std::uint16_t port = 47600;
};

class Show
{
public:
    std::string name = "Untitled Show";
    FrameRate frameRate = FrameRate::fps30();
    // Show duration. Zero = unlimited (playback stops at the last cue only
    // when a Stop cue exists; otherwise it runs forever).
    Time duration;

    std::vector<MediaReference> media;
    std::vector<Scene> scenes;

    // Timeline, kept sorted by time.
    std::vector<Cue> cues;

    // The playback systems the show targets (the control GUI's system list;
    // issue 18 - stored in the show file rather than a separate config).
    std::vector<SystemReference> systems;

    // --- Queries -----------------------------------------------------------

    const Scene* findScene(Id sceneId) const;
    Scene* findScene(Id sceneId);

    const MediaReference* findMedia(Id mediaId) const;

    // Sorted insert; replaces an existing cue at the same time.
    void addCue(Cue cue);

    bool empty() const { return scenes.empty(); }

    // --- Serialization -----------------------------------------------------

    nlohmann::json toJson() const;
    static std::optional<Show> fromJson(const nlohmann::json& in, std::string& error);
};

} // namespace mediamax
