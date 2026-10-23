#include "mediamax/show/Cue.h"

#include <cstdint>

#include "mediamax/blend/Blend.h"

namespace mediamax
{

namespace
{

// Compares two cue actions of the same type without using
// std::variant::operator== (which MSVC fails to instantiate for empty
// alternative types like StopPlayback).
bool cueActionEqual(const CueAction& a, const CueAction& b)
{
    if (a.index() != b.index())
    {
        return false;
    }
    return std::visit(
        [](const auto& x, const auto& y) {
            using TX = std::decay_t<decltype(x)>;
            using TY = std::decay_t<decltype(y)>;
            if constexpr (!std::is_same_v<TX, TY>)
            {
                return false;
            }
            else if constexpr (std::is_empty_v<TX>)
            {
                return true; // stateless actions (stop, pause)
            }
            else
            {
                return x == y;
            }
        },
        a, b);
}

} // namespace

bool Cue::operator==(const Cue& o) const
{
    return time == o.time && name == o.name && cueActionEqual(action, o.action);
}

void toActionJson(nlohmann::json& out, const CueAction& action)
{
    std::visit(
        [&](const auto& a) {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, PlayScene>)
            {
                out = {{"type", "playScene"}, {"scene", a.scene}};
            }
            else if constexpr (std::is_same_v<T, StopPlayback>)
            {
                out = {{"type", "stop"}};
            }
            else if constexpr (std::is_same_v<T, PausePlayback>)
            {
                out = {{"type", "pause"}};
            }
            else if constexpr (std::is_same_v<T, SeekTo>)
            {
                out = {{"type", "seek"}, {"time", a.time.toSeconds()}};
            }
            else if constexpr (std::is_same_v<T, SetLayerOpacity>)
            {
                out = {{"type", "setLayerOpacity"}, {"layer", a.layer}, {"value", a.value}};
            }
            else if constexpr (std::is_same_v<T, SetLayerVisible>)
            {
                out = {{"type", "setLayerVisible"}, {"layer", a.layer}, {"value", a.value}};
            }
            else if constexpr (std::is_same_v<T, SetLayerBlend>)
            {
                out = {{"type", "setLayerBlend"},
                       {"layer", a.layer},
                       {"mode", blend::toString(a.mode)}};
            }
            else if constexpr (std::is_same_v<T, SetLayerPosition>)
            {
                out = {{"type", "setLayerPosition"},
                       {"layer", a.layer},
                       {"x", a.x},
                       {"y", a.y}};
            }
        },
        action);
}

std::optional<CueAction> fromActionJson(const nlohmann::json& in)
{
    if (!in.is_object() || !in.contains("type") || !in["type"].is_string())
    {
        return std::nullopt;
    }
    const std::string type = in["type"].get<std::string>();

    const auto num = [&](const char* key) -> std::optional<double> {
        if (!in.contains(key) || !in[key].is_number())
        {
            return std::nullopt;
        }
        return in[key].get<double>();
    };
    const auto id = [&](const char* key) -> std::optional<Id> {
        if (!in.contains(key) || !in[key].is_number_unsigned())
        {
            return std::nullopt;
        }
        return static_cast<Id>(in[key].get<std::uint64_t>());
    };

    if (type == "playScene")
    {
        if (auto scene = id("scene"))
        {
            return PlayScene{*scene};
        }
        return std::nullopt;
    }
    if (type == "stop")
    {
        return StopPlayback{};
    }
    if (type == "pause")
    {
        return PausePlayback{};
    }
    if (type == "seek")
    {
        if (auto t = num("time"))
        {
            return SeekTo{Time::fromSeconds(*t)};
        }
        return std::nullopt;
    }
    if (type == "setLayerOpacity")
    {
        if (auto layer = id("layer"))
        {
            if (auto v = num("value"))
            {
                return SetLayerOpacity{layer.value(), static_cast<float>(*v)};
            }
        }
        return std::nullopt;
    }
    if (type == "setLayerVisible")
    {
        if (auto layer = id("layer"); layer)
        {
            if (!in.contains("value") || !in["value"].is_boolean())
            {
                return std::nullopt;
            }
            return SetLayerVisible{*layer, in["value"].get<bool>()};
        }
        return std::nullopt;
    }
    if (type == "setLayerBlend")
    {
        if (auto layer = id("layer"))
        {
            std::string modeStr = "normal";
            if (in.contains("mode") && in["mode"].is_string())
            {
                modeStr = in["mode"].get<std::string>();
            }
            return SetLayerBlend{layer.value(), blend::fromString(modeStr)};
        }
        return std::nullopt;
    }
    if (type == "setLayerPosition")
    {
        if (auto layer = id("layer"))
        {
            const auto x = num("x");
            const auto y = num("y");
            if (x && y)
            {
                return SetLayerPosition{layer.value(), static_cast<float>(*x),
                                        static_cast<float>(*y)};
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace mediamax
