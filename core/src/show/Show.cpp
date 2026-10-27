#include "mediamax/show/Show.h"

#include <algorithm>
#include <cstdint>
#include <functional>

#include "mediamax/blend/Blend.h"

namespace mediamax
{

namespace
{

constexpr const char* kShowMagic = "mediamax-show";
constexpr int kShowVersion = 1;

nlohmann::json timeToJson(const Time& t)
{
    return t.toSeconds();
}

std::optional<Time> timeFromJson(const nlohmann::json& j)
{
    if (!j.is_number())
    {
        return std::nullopt;
    }
    return Time::fromSeconds(j.get<double>());
}

nlohmann::json frameRateToJson(const FrameRate& fps)
{
    return {{"num", fps.numerator()}, {"den", fps.denominator()}};
}

std::optional<FrameRate> frameRateFromJson(const nlohmann::json& j)
{
    if (!j.is_object() || !j.contains("num") || !j.contains("den") ||
        !j["num"].is_number() || !j["den"].is_number())
    {
        return std::nullopt;
    }
    const std::int64_t num = j["num"].get<std::int64_t>();
    const std::int64_t den = j["den"].get<std::int64_t>();
    if (num <= 0 || den <= 0)
    {
        return std::nullopt;
    }
    return FrameRate::of(num, den);
}

nlohmann::json transformToJson(const Transform2D& t)
{
    return {{"x", t.x},
            {"y", t.y},
            {"rotation", t.rotation},
            {"scaleX", t.scaleX},
            {"scaleY", t.scaleY},
            {"anchorX", t.anchorX},
            {"anchorY", t.anchorY}};
}

Transform2D transformFromJson(const nlohmann::json& j)
{
    Transform2D t;
    if (!j.is_object())
    {
        return t;
    }
    if (j.contains("x")) t.x = j["x"].get<float>();
    if (j.contains("y")) t.y = j["y"].get<float>();
    if (j.contains("rotation")) t.rotation = j["rotation"].get<float>();
    if (j.contains("scaleX")) t.scaleX = j["scaleX"].get<float>();
    if (j.contains("scaleY")) t.scaleY = j["scaleY"].get<float>();
    if (j.contains("anchorX")) t.anchorX = j["anchorX"].get<float>();
    if (j.contains("anchorY")) t.anchorY = j["anchorY"].get<float>();
    return t;
}

nlohmann::json colorToJson(const Color4& c)
{
    return {{"r", c.r}, {"g", c.g}, {"b", c.b}, {"a", c.a}};
}

Color4 colorFromJson(const nlohmann::json& j)
{
    Color4 c;
    if (!j.is_object())
    {
        return c;
    }
    if (j.contains("r")) c.r = j["r"].get<float>();
    if (j.contains("g")) c.g = j["g"].get<float>();
    if (j.contains("b")) c.b = j["b"].get<float>();
    if (j.contains("a")) c.a = j["a"].get<float>();
    return c;
}

nlohmann::json trackToJson(const KeyframeTrack& track,
                           const std::function<void(nlohmann::json&, const Keyframe&)>& kfToJson)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& kf : track.keyframes())
    {
        kfToJson(arr, kf);
    }
    return arr;
}

void trackFromJson(const nlohmann::json& j, KeyframeTrack& track,
                   const std::function<void(const nlohmann::json&, Keyframe&)>& kfFromJson)
{
    track.clear();
    if (!j.is_array())
    {
        return;
    }
    for (const auto& item : j)
    {
        Keyframe kf;
        kfFromJson(item, kf);
        track.setKey(kf.time, kf.value, kf.interpolation);
    }
}

void keyframeToJsonItem(nlohmann::json& arr, const Keyframe& kf)
{
    arr.push_back(nlohmann::json{
        {"time", kf.time.toSeconds()},
        {"value", kf.value},
        {"interpolation", kf.interpolation == Interpolation::Step ? "step" : "linear"},
    });
}

void keyframeFromJsonItem(const nlohmann::json& item, Keyframe& kf)
{
    if (!item.is_object())
    {
        return;
    }
    auto t = timeFromJson(item.value("time", nlohmann::json(0)));
    kf.time = t.value_or(Time::zero());
    kf.value = item.value("value", 0.0f);
    const std::string interp = item.value("interpolation", std::string("linear"));
    kf.interpolation = interp == "step" ? Interpolation::Step : Interpolation::Linear;
}

} // namespace

const Scene* Show::findScene(Id sceneId) const
{
    for (const auto& scene : scenes)
    {
        if (scene.id == sceneId)
        {
            return &scene;
        }
    }
    return nullptr;
}

Scene* Show::findScene(Id sceneId)
{
    for (auto& scene : scenes)
    {
        if (scene.id == sceneId)
        {
            return &scene;
        }
    }
    return nullptr;
}

const MediaReference* Show::findMedia(Id mediaId) const
{
    for (const auto& ref : media)
    {
        if (ref.id == mediaId)
        {
            return &ref;
        }
    }
    return nullptr;
}

void Show::addCue(Cue cue)
{
    // Keep cues sorted by time; replace any cue at the same time.
    auto it = std::lower_bound(cues.begin(), cues.end(), cue.time,
                               [](const Cue& c, const Time& t) { return c.time < t; });
    if (it != cues.end() && it->time == cue.time)
    {
        *it = std::move(cue);
        return;
    }
    cues.insert(it, std::move(cue));
}

nlohmann::json Show::toJson() const
{
    nlohmann::json j;
    j["app"] = kShowMagic;
    j["version"] = kShowVersion;
    j["name"] = name;
    j["frameRate"] = frameRateToJson(frameRate);
    j["duration"] = duration.isValid() ? duration.toSeconds() : 0.0;

    nlohmann::json mediaArr = nlohmann::json::array();
    for (const auto& m : media)
    {
        mediaArr.push_back({{"id", m.id}, {"path", m.path}});
    }
    j["media"] = mediaArr;

    nlohmann::json sceneArr = nlohmann::json::array();
    for (const auto& scene : scenes)
    {
        nlohmann::json layerArr = nlohmann::json::array();
        for (const auto& layer : scene.layers)
        {
            nlohmann::json lj = {
                {"id", layer.id},
                {"name", layer.name},
                {"mediaId", layer.mediaId},
                {"color", colorToJson(layer.color)},
                {"inPoint", timeToJson(layer.inPoint)},
                {"outPoint",
                 layer.outPoint.isValid() ? timeToJson(layer.outPoint)
                                          : nlohmann::json(0.0)},
                {"loop", layer.loop},
                {"rate", layer.rate},
                {"transform", transformToJson(layer.transform)},
                {"opacity", layer.opacity},
                {"blend", blend::toString(layer.blend)},
                {"visible", layer.visible},
                {"zIndex", layer.zIndex},
            };

            if (!layer.opacityTrack.empty() || !layer.xTrack.empty() || !layer.yTrack.empty() ||
                !layer.rotationTrack.empty() || !layer.scaleTrack.empty())
            {
                lj["tracks"] = {
                    {"opacity", trackToJson(layer.opacityTrack, keyframeToJsonItem)},
                    {"x", trackToJson(layer.xTrack, keyframeToJsonItem)},
                    {"y", trackToJson(layer.yTrack, keyframeToJsonItem)},
                    {"rotation", trackToJson(layer.rotationTrack, keyframeToJsonItem)},
                    {"scale", trackToJson(layer.scaleTrack, keyframeToJsonItem)},
                };
            }
            layerArr.push_back(std::move(lj));
        }
        sceneArr.push_back({{"id", scene.id}, {"name", scene.name}, {"layers", layerArr}});
    }
    j["scenes"] = sceneArr;

    nlohmann::json cueArr = nlohmann::json::array();
    for (const auto& cue : cues)
    {
        nlohmann::json action;
        toActionJson(action, cue.action);
        cueArr.push_back({{"time", timeToJson(cue.time)}, {"name", cue.name}, {"action", action}});
    }
    j["cues"] = cueArr;

    nlohmann::json systemsArr = nlohmann::json::array();
    for (const auto& sys : systems)
    {
        systemsArr.push_back({{"name", sys.name}, {"host", sys.host}, {"port", sys.port}});
    }
    j["systems"] = systemsArr;

    return j;
}

std::optional<Show> Show::fromJson(const nlohmann::json& in, std::string& error)
{
    Show show;

    if (!in.is_object())
    {
        error = "show file: root must be a JSON object";
        return std::nullopt;
    }
    if (in.value("app", std::string()) != kShowMagic)
    {
        error = "show file: unknown 'app' field (expected \"" + std::string(kShowMagic) + "\")";
        return std::nullopt;
    }
    const int version = in.value("version", 0);
    if (version != kShowVersion)
    {
        error = "show file: unsupported version " + std::to_string(version);
        return std::nullopt;
    }

    show.name = in.value("name", std::string("Untitled Show"));

    if (in.contains("frameRate"))
    {
        auto fps = frameRateFromJson(in["frameRate"]);
        if (!fps)
        {
            error = "show file: invalid frameRate";
            return std::nullopt;
        }
        show.frameRate = *fps;
    }

    if (in.contains("duration"))
    {
        auto dur = timeFromJson(in["duration"]);
        if (!dur)
        {
            error = "show file: invalid duration";
            return std::nullopt;
        }
        show.duration = *dur;
    }

    // Media references.
    if (in.contains("media") && in["media"].is_array())
    {
        for (const auto& m : in["media"])
        {
            if (!m.is_object() || !m.contains("id") || !m.contains("path") ||
                !m["path"].is_string())
            {
                error = "show file: invalid media entry";
                return std::nullopt;
            }
            show.media.push_back({static_cast<Id>(m["id"].get<std::uint64_t>()),
                                  m["path"].get<std::string>()});
        }
    }

    // Scenes and layers.
    if (in.contains("scenes") && in["scenes"].is_array())
    {
        for (const auto& sj : in["scenes"])
        {
            if (!sj.is_object() || !sj.contains("id") || !sj.contains("name"))
            {
                error = "show file: invalid scene entry";
                return std::nullopt;
            }
            Scene scene;
            scene.id = static_cast<Id>(sj["id"].get<std::uint64_t>());
            scene.name = sj["name"].get<std::string>();

            if (sj.contains("layers") && sj["layers"].is_array())
            {
                for (const auto& lj : sj["layers"])
                {
                    if (!lj.is_object() || !lj.contains("id"))
                    {
                        error = "show file: invalid layer entry";
                        return std::nullopt;
                    }
                    Layer layer;
                    layer.id = static_cast<Id>(lj["id"].get<std::uint64_t>());
                    layer.name = lj.value("name", std::string());
                    layer.mediaId =
                        static_cast<Id>(lj.value("mediaId", static_cast<std::uint64_t>(kInvalidId)));
                    if (lj.contains("color"))
                    {
                        layer.color = colorFromJson(lj["color"]);
                    }
                    if (auto t = timeFromJson(lj.value("inPoint", nlohmann::json(0.0))))
                    {
                        layer.inPoint = *t;
                    }
                    if (auto t = timeFromJson(lj.value("outPoint", nlohmann::json(0.0))))
                    {
                        layer.outPoint = *t;
                    }
                    layer.loop = lj.value("loop", false);
                    layer.rate = lj.value("rate", 1.0f);
                    if (lj.contains("transform"))
                    {
                        layer.transform = transformFromJson(lj["transform"]);
                    }
                    layer.opacity = lj.value("opacity", 1.0f);
                    layer.blend = blend::fromString(lj.value("blend", std::string("normal")));
                    layer.visible = lj.value("visible", true);
                    layer.zIndex = lj.value("zIndex", 0);

                    if (lj.contains("tracks") && lj["tracks"].is_object())
                    {
                        const auto& tracks = lj["tracks"];
                        if (tracks.contains("opacity"))
                        {
                            trackFromJson(tracks["opacity"], layer.opacityTrack, keyframeFromJsonItem);
                        }
                        if (tracks.contains("x"))
                        {
                            trackFromJson(tracks["x"], layer.xTrack, keyframeFromJsonItem);
                        }
                        if (tracks.contains("y"))
                        {
                            trackFromJson(tracks["y"], layer.yTrack, keyframeFromJsonItem);
                        }
                        if (tracks.contains("rotation"))
                        {
                            trackFromJson(tracks["rotation"], layer.rotationTrack,
                                          keyframeFromJsonItem);
                        }
                        if (tracks.contains("scale"))
                        {
                            trackFromJson(tracks["scale"], layer.scaleTrack, keyframeFromJsonItem);
                        }
                    }
                    scene.layers.push_back(std::move(layer));
                }
            }
            show.scenes.push_back(std::move(scene));
        }
    }

    // Cues.
    if (in.contains("cues") && in["cues"].is_array())
    {
        for (const auto& cj : in["cues"])
        {
            if (!cj.is_object() || !cj.contains("time") || !cj.contains("action"))
            {
                error = "show file: invalid cue entry";
                return std::nullopt;
            }
            auto t = timeFromJson(cj["time"]);
            if (!t)
            {
                error = "show file: invalid cue time";
                return std::nullopt;
            }
            auto action = fromActionJson(cj["action"]);
            if (!action)
            {
                error = "show file: invalid cue action";
                return std::nullopt;
            }
            show.cues.push_back(Cue{*t, cj.value("name", std::string()), *action});
        }
        std::sort(show.cues.begin(), show.cues.end(),
                  [](const Cue& a, const Cue& b) { return a.time < b.time; });
    }

    // Target systems (client-side info; issue 18).
    if (in.contains("systems") && in["systems"].is_array())
    {
        for (const auto& sj : in["systems"])
        {
            if (!sj.is_object() || !sj.contains("name") || !sj.contains("host") ||
                !sj["name"].is_string() || !sj["host"].is_string())
            {
                error = "show file: invalid systems entry";
                return std::nullopt;
            }
            SystemReference sys;
            sys.name = sj["name"].get<std::string>();
            sys.host = sj["host"].get<std::string>();
            if (sj.contains("port"))
            {
                if (!sj["port"].is_number_integer() || sj["port"].get<int>() < 1 ||
                    sj["port"].get<int>() > 65535)
                {
                    error = "show file: invalid systems entry";
                    return std::nullopt;
                }
                sys.port = static_cast<std::uint16_t>(sj["port"].get<int>());
            }
            show.systems.push_back(std::move(sys));
        }
    }

    return show;
}

} // namespace mediamax
