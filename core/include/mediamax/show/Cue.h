#pragma once

// Timeline cues.
//
// The show timeline is evaluated as a pure function of time: the engine finds
// the last cue of each kind at or before the current time. This makes seeking
// (forward or backward) trivially correct - the state at time t never depends
// on how t was reached.

#include <optional>
#include <variant>

#include <nlohmann/json.hpp>

#include "mediamax/blend/Blend.h"
#include "mediamax/time/Time.h"
#include "mediamax/util/Id.h"

namespace mediamax
{

// --- Cue actions -----------------------------------------------------------

struct PlayScene
{
    Id scene = kInvalidId;
    bool operator==(const PlayScene&) const = default;
};

struct StopPlayback
{
    bool operator==(const StopPlayback&) const = default;
};

struct PausePlayback
{
    bool operator==(const PausePlayback&) const = default;
};

struct SeekTo
{
    Time time;
    bool operator==(const SeekTo&) const = default;
};

struct SetLayerOpacity
{
    Id layer = kInvalidId;
    float value = 1.f;
    bool operator==(const SetLayerOpacity&) const = default;
};

struct SetLayerVisible
{
    Id layer = kInvalidId;
    bool value = true;
    bool operator==(const SetLayerVisible&) const = default;
};

struct SetLayerBlend
{
    Id layer = kInvalidId;
    BlendMode mode = BlendMode::Normal;
    bool operator==(const SetLayerBlend&) const = default;
};

struct SetLayerPosition
{
    Id layer = kInvalidId;
    float x = 0.f;
    float y = 0.f;
    bool operator==(const SetLayerPosition&) const = default;
};

using CueAction = std::variant<PlayScene, StopPlayback, PausePlayback, SeekTo, SetLayerOpacity,
                               SetLayerVisible, SetLayerBlend, SetLayerPosition>;

struct Cue
{
    Time time;
    std::string name;
    CueAction action;

    bool operator==(const Cue& o) const;
    bool operator!=(const Cue& o) const { return !(*this == o); }
};

// JSON codec for actions.
void toActionJson(nlohmann::json& out, const CueAction& action);
std::optional<CueAction> fromActionJson(const nlohmann::json& in);

} // namespace mediamax
