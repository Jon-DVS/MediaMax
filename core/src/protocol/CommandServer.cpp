#include "mediamax/protocol/CommandServer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include "mediamax/blend/Blend.h"
#include "mediamax/time/FrameRate.h"
#include "mediamax/util/File.h"
#include "mediamax/util/Logger.h"

namespace mediamax::protocol
{

namespace
{

std::string mediaTypeToString(MediaType t)
{
    switch (t)
    {
        case MediaType::Video: return "video";
        case MediaType::Image: return "image";
        case MediaType::Audio: return "audio";
        case MediaType::Unknown: return "unknown";
    }
    return "unknown";
}

nlohmann::json mediaToJson(const MediaAsset& asset)
{
    return {
        {"id", asset.id},
        {"path", asset.path},
        {"type", mediaTypeToString(asset.metadata.type)},
        {"width", asset.metadata.width},
        {"height", asset.metadata.height},
        {"duration", asset.metadata.duration.toSeconds()},
        {"frameRate", asset.metadata.frameRate.toString()},
    };
}

nlohmann::json displayToJson(const Display& d)
{
    return {
        {"id", d.id},
        {"name", d.name},
        {"x", d.x},
        {"y", d.y},
        {"width", d.width},
        {"height", d.height},
        {"brightness", d.brightness},
        {"enabled", d.enabled},
    };
}

// Layer ids are validated against the loaded show (the engine's override
// storage is generic and would silently accept unknown ids).
bool layerExistsInShow(const Show& show, Id layerId)
{
    for (const auto& scene : show.scenes)
    {
        for (const auto& layer : scene.layers)
        {
            if (layer.id == layerId)
            {
                return true;
            }
        }
    }
    return false;
}

} // namespace

std::vector<std::string> CommandServer::supportedMethods()
{
    return {"ping",
            "status",
            "system.set",
            "show.load",
            "show.loadJson",
            "show.save",
            "show.info",
            "media.load",
            "media.list",
            "media.unload",
            "playback.play",
            "playback.pause",
            "playback.stop",
            "playback.seek",
            "scene.set",
            "scene.list",
            "scene.clear",
            "layer.set",
            "layer.clear",
            "display.list",
            "display.set",
            "display.warp.set"};
}

Response CommandServer::handle(const Command& cmd) const
{
    try
    {
        nlohmann::json result;
        if (cmd.method == "ping")
        {
            result = handlePing();
        }
        else if (cmd.method == "status")
        {
            result = handleStatus();
        }
        else if (cmd.method == "system.set")
        {
            result = handleSystemSet(cmd.params);
        }
        else if (cmd.method == "show.load")
        {
            result = handleShowLoad(cmd.params);
        }
        else if (cmd.method == "show.loadJson")
        {
            result = handleShowLoadJson(cmd.params);
        }
        else if (cmd.method == "show.save")
        {
            result = handleShowSave(cmd.params);
        }
        else if (cmd.method == "show.info")
        {
            result = handleShowInfo();
        }
        else if (cmd.method == "media.load")
        {
            result = handleMediaLoad(cmd.params);
        }
        else if (cmd.method == "media.list")
        {
            result = handleMediaList();
        }
        else if (cmd.method == "media.unload")
        {
            result = handleMediaUnload(cmd.params);
        }
        else if (cmd.method == "playback.play")
        {
            result = handlePlaybackPlay(cmd.params);
        }
        else if (cmd.method == "playback.pause")
        {
            result = handlePlaybackPause();
        }
        else if (cmd.method == "playback.stop")
        {
            result = handlePlaybackStop();
        }
        else if (cmd.method == "playback.seek")
        {
            result = handlePlaybackSeek(cmd.params);
        }
        else if (cmd.method == "scene.set")
        {
            result = handleSceneSet(cmd.params);
        }
        else if (cmd.method == "scene.list")
        {
            result = handleSceneList();
        }
        else if (cmd.method == "scene.clear")
        {
            result = handleSceneClear();
        }
        else if (cmd.method == "layer.set")
        {
            result = handleLayerSet(cmd.params);
        }
        else if (cmd.method == "layer.clear")
        {
            result = handleLayerClear(cmd.params);
        }
        else if (cmd.method == "display.list")
        {
            result = handleDisplayList();
        }
        else if (cmd.method == "display.set")
        {
            result = handleDisplaySet(cmd.params);
        }
        else if (cmd.method == "display.warp.set")
        {
            result = handleDisplayWarpSet(cmd.params);
        }
        else
        {
            throw ParameterError("unknown method: " + cmd.method);
        }
        return Response::success(cmd.id, std::move(result));
    }
    catch (const ParameterError& e)
    {
        return Response::failure(cmd.id, e.what());
    }
    catch (const std::exception& e)
    {
        Logger::instance().error("Command '{}' failed: {}", cmd.method, e.what());
        return Response::failure(cmd.id, e.what());
    }
}

// --- Commands --------------------------------------------------------------

nlohmann::json CommandServer::handlePing() const
{
    return {
        {"server", "mediamax"},
        {"system", m_SystemName},
        {"protocolVersion", kProtocolVersion},
    };
}

nlohmann::json CommandServer::handleStatus() const
{
    const FrameState state = m_Engine.frameState();
    const Show& show = m_Engine.show();

    nlohmann::json mediaArr = nlohmann::json::array();
    for (Id id : m_Media.assetIds())
    {
        if (const auto asset = m_Media.find(id))
        {
            mediaArr.push_back(mediaToJson(**asset));
        }
    }
    nlohmann::json displayArr = nlohmann::json::array();
    for (const auto& d : m_Displays.displays())
    {
        displayArr.push_back(displayToJson(d));
    }

    nlohmann::json sceneArr = nlohmann::json::array();
    for (const auto& scene : show.scenes)
    {
        sceneArr.push_back({{"id", scene.id}, {"name", scene.name},
                            {"layers", scene.layers.size()}});
    }

    return {
        {"system", m_SystemName},
        {"state", PlaybackEngine::stateToString(m_Engine.state())},
        {"time", state.showTime.toSeconds()},
        {"timecode", state.showTime.toTimecode(show.frameRate)},
        {"duration", show.duration.isValid() ? show.duration.toSeconds() : -1.0},
        {"frameRate", show.frameRate.toString()},
        {"scene", state.activeSceneId},
        {"previewScene", m_Engine.previewScene()},
        {"layers", state.layers.size()},
        {"media", mediaArr},
        {"displays", displayArr},
        {"scenes", sceneArr},
    };
}

nlohmann::json CommandServer::handleSystemSet(const nlohmann::json& params) const
{
    const std::string name = requireString(params, "name");
    // Note: the system name is owned by the app (set at startup or via this
    // command); the engine mirrors it for status reporting.
    m_SystemName = name;
    return {{"system", m_SystemName}};
}

nlohmann::json CommandServer::handleShowLoad(const nlohmann::json& params) const
{
    const std::string path = requireString(params, "path");
    auto file = readFile(path);
    if (!file)
    {
        throw ParameterError("cannot read show file: " + path);
    }
    nlohmann::json parsed;
    try
    {
        parsed = nlohmann::json::parse(*file);
    }
    catch (const nlohmann::json::exception& e)
    {
        throw ParameterError("invalid JSON in show file: " + std::string(e.what()));
    }
    return applyShowDocument(parsed, "show file");
}

// Parses a show document (a parsed JSON object), loads and remaps the media it
// references, and makes the show active. Shared by show.load and show.loadJson.
nlohmann::json CommandServer::applyShowDocument(const nlohmann::json& parsed,
                                                const char* context) const
{
    std::string error;
    auto show = Show::fromJson(parsed, error);
    if (!show)
    {
        throw ParameterError(std::string("invalid ") + context + ": " + error);
    }

    // Load the media the show references. Show-file media ids are local to
    // the file, so each loaded asset is remapped to its library id. Missing
    // files are logged and skipped so partially missing media does not block
    // the show.
    if (m_LoadMedia)
    {
        std::map<Id, Id> remap;
        for (const auto& ref : show->media)
        {
            std::string loadError;
            const Id libraryId = m_LoadMedia(ref.path, loadError);
            if (libraryId == kInvalidId)
            {
                Logger::instance().warning("Show references missing media '{}': {}", ref.path,
                                           loadError);
                continue;
            }
            remap[ref.id] = libraryId;
        }
        for (auto& ref : show->media)
        {
            if (const auto it = remap.find(ref.id); it != remap.end())
            {
                ref.id = it->second;
            }
        }
        for (auto& scene : show->scenes)
        {
            for (auto& layer : scene.layers)
            {
                if (layer.mediaId != kInvalidId)
                {
                    if (const auto it = remap.find(layer.mediaId); it != remap.end())
                    {
                        layer.mediaId = it->second;
                    }
                }
            }
        }
    }

    m_Engine.loadShow(*show);
    return {
        {"name", show->name},
        {"scenes", show->scenes.size()},
        {"media", show->media.size()},
    };
}

nlohmann::json CommandServer::handleShowLoadJson(const nlohmann::json& params) const
{
    if (!params.contains("show") || !params.at("show").is_object())
    {
        throw ParameterError("show.loadJson requires a 'show' object parameter");
    }
    return applyShowDocument(params.at("show"), "show JSON");
}

nlohmann::json CommandServer::handleShowSave(const nlohmann::json& params) const
{
    const std::string path = requireString(params, "path");
    if (!writeFile(path, m_Engine.show().toJson().dump(2)))
    {
        throw ParameterError("cannot write show file: " + path);
    }
    return {{"path", path}};
}

nlohmann::json CommandServer::handleShowInfo() const
{
    const Show& show = m_Engine.show();
    nlohmann::json sceneArr = nlohmann::json::array();
    for (const auto& scene : show.scenes)
    {
        sceneArr.push_back({{"id", scene.id}, {"name", scene.name},
                            {"layers", scene.layers.size()}});
    }
    return {
        {"name", show.name},
        {"frameRate", show.frameRate.toString()},
        {"duration", show.duration.isValid() ? show.duration.toSeconds() : -1.0},
        {"scenes", sceneArr},
        {"cues", show.cues.size()},
    };
}

nlohmann::json CommandServer::handleMediaLoad(const nlohmann::json& params) const
{
    const std::string path = requireString(params, "path");
    if (!m_LoadMedia)
    {
        throw ParameterError("media loading is not available on this system");
    }
    std::string error;
    const Id id = m_LoadMedia(path, error);
    if (id == kInvalidId)
    {
        throw ParameterError("failed to load media: " + error);
    }
    const auto asset = m_Media.find(id);
    if (!asset)
    {
        throw ParameterError("media not found in library after load: " + std::to_string(id));
    }
    return mediaToJson(**asset);
}

nlohmann::json CommandServer::handleMediaList() const
{
    nlohmann::json arr = nlohmann::json::array();
    for (Id id : m_Media.assetIds())
    {
        if (const auto asset = m_Media.find(id))
        {
            arr.push_back(mediaToJson(**asset));
        }
    }
    return {{"media", arr}};
}

nlohmann::json CommandServer::handleMediaUnload(const nlohmann::json& params) const
{
    const Id id = static_cast<Id>(requireInt(params, "id"));
    if (!m_Media.unload(id))
    {
        throw ParameterError("no media with id " + std::to_string(id));
    }
    return {{"id", id}};
}

nlohmann::json CommandServer::handlePlaybackPlay(const nlohmann::json& params) const
{
    if (const auto at = optionalString(params, "at"))
    {
        std::string error;
        try
        {
            m_Engine.seek(Time::fromSeconds(std::stod(*at)));
        }
        catch (const std::exception&)
        {
            throw ParameterError("invalid 'at' time: " + *at);
        }
    }
    m_Engine.play();
    return {{"state", PlaybackEngine::stateToString(m_Engine.state())}};
}

nlohmann::json CommandServer::handlePlaybackPause() const
{
    m_Engine.pause();
    return {{"state", PlaybackEngine::stateToString(m_Engine.state())}};
}

nlohmann::json CommandServer::handlePlaybackStop() const
{
    m_Engine.stop();
    return {{"state", PlaybackEngine::stateToString(m_Engine.state())}};
}

nlohmann::json CommandServer::handlePlaybackSeek(const nlohmann::json& params) const
{
    const double seconds = requireNumber(params, "time");
    m_Engine.seek(Time::fromSeconds(seconds));
    return {{"time", m_Engine.time().toSeconds()}};
}

nlohmann::json CommandServer::handleSceneSet(const nlohmann::json& params) const
{
    const int index = requireInt(params, "index");
    const Show& show = m_Engine.show();
    if (index < 0 || index >= static_cast<int>(show.scenes.size()))
    {
        throw ParameterError("scene index out of range: " + std::to_string(index));
    }
    const Id sceneId = show.scenes[static_cast<size_t>(index)].id;
    if (!m_Engine.setSceneNow(sceneId))
    {
        throw ParameterError("cannot switch to scene " + std::to_string(sceneId));
    }
    return {{"scene", sceneId}, {"name", show.scenes[static_cast<size_t>(index)].name}};
}

nlohmann::json CommandServer::handleSceneList() const
{
    nlohmann::json arr = nlohmann::json::array();
    const Show& show = m_Engine.show();
    for (const auto& scene : show.scenes)
    {
        nlohmann::json layerArr = nlohmann::json::array();
        for (const auto& layer : scene.layers)
        {
            layerArr.push_back({{"id", layer.id}, {"name", layer.name}});
        }
        arr.push_back({
            {"id", scene.id},
            {"name", scene.name},
            {"active", m_Engine.activeSceneAt(m_Engine.time()) == scene.id},
            {"layers", layerArr},
        });
    }
    return {{"scenes", arr}};
}

nlohmann::json CommandServer::handleSceneClear() const
{
    // Releases the operator's scene override (scene.set); scene selection
    // returns to the timeline. The response reports the now-active scene so
    // the caller does not need a follow-up status call.
    bool cleared = false;
    if (m_Engine.hasSceneOverride())
    {
        m_Engine.clearSceneNow();
        cleared = true;
    }
    return {
        {"cleared", cleared ? 1 : 0},
        {"scene", m_Engine.activeSceneAt(m_Engine.time())},
    };
}

nlohmann::json CommandServer::handleLayerSet(const nlohmann::json& params) const
{
    const Id layerId = static_cast<Id>(requireInt(params, "layer"));
    if (!layerExistsInShow(m_Engine.show(), layerId))
    {
        throw ParameterError("no layer with id " + std::to_string(layerId) + " in the show");
    }

    if (params.contains("opacity"))
    {
        m_Engine.setLayerOpacity(layerId, static_cast<float>(params["opacity"].get<double>()));
    }
    if (params.contains("visible"))
    {
        m_Engine.setLayerVisible(layerId, params["visible"].get<bool>());
    }
    if (params.contains("blend"))
    {
        m_Engine.setLayerBlend(layerId, blend::fromString(params["blend"].get<std::string>()));
    }
    if (params.contains("x") && params.contains("y"))
    {
        m_Engine.setLayerPosition(layerId, static_cast<float>(params["x"].get<double>()),
                                  static_cast<float>(params["y"].get<double>()));
    }
    return {{"layer", layerId}};
}

nlohmann::json CommandServer::handleLayerClear(const nlohmann::json& params) const
{
    // With 'layer': clear that layer's overrides. Without it: clear all.
    if (params.contains("layer"))
    {
        const Id layerId = static_cast<Id>(requireInt(params, "layer"));
        if (!m_Engine.clearLayerOverride(layerId))
        {
            throw ParameterError("no override set for layer " + std::to_string(layerId));
        }
        return {{"layer", layerId}};
    }
    const int cleared = m_Engine.clearLayerOverrides();
    return {{"cleared", cleared}};
}

nlohmann::json CommandServer::handleDisplayList() const
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& d : m_Displays.displays())
    {
        arr.push_back(displayToJson(d));
    }
    return {{"displays", arr}};
}

nlohmann::json CommandServer::handleDisplaySet(const nlohmann::json& params) const
{
    const Id id = static_cast<Id>(requireInt(params, "display"));
    const auto display = m_Displays.find(id);
    if (!display)
    {
        throw ParameterError("no display with id " + std::to_string(id));
    }

    std::int32_t x = (*display)->x, y = (*display)->y, w = (*display)->width, h = (*display)->height;
    if (params.contains("x")) x = static_cast<std::int32_t>(params["x"].get<double>());
    if (params.contains("y")) y = static_cast<std::int32_t>(params["y"].get<double>());
    if (params.contains("width")) w = static_cast<std::int32_t>(params["width"].get<double>());
    if (params.contains("height")) h = static_cast<std::int32_t>(params["height"].get<double>());
    if (!m_Displays.setDisplayGeometry(id, x, y, w, h))
    {
        throw ParameterError("invalid display geometry");
    }
    if (params.contains("brightness"))
    {
        m_Displays.setDisplayBrightness(id, static_cast<float>(params["brightness"].get<double>()));
    }
    if (params.contains("enabled"))
    {
        m_Displays.setDisplayEnabled(id, params["enabled"].get<bool>());
    }

    const auto updated = m_Displays.find(id);
    return displayToJson(**updated);
}

nlohmann::json CommandServer::handleDisplayWarpSet(const nlohmann::json& params) const
{
    const Id id = static_cast<Id>(requireInt(params, "display"));
    const auto display = m_Displays.find(id);
    if (!display)
    {
        throw ParameterError("no display with id " + std::to_string(id));
    }
    if (!params.contains("points") || !params["points"].is_array())
    {
        throw ParameterError("warp.set requires 'points': [[u, v], ...] (rows*cols values)");
    }

    const auto& points = params["points"];
    const int rows = static_cast<int>(std::sqrt(static_cast<double>(points.size())));
    const int cols = rows;
    if (rows < 2 || rows * cols != static_cast<int>(points.size()))
    {
        throw ParameterError("warp grid must be a square of >= 2x2 points");
    }

    WarpMap warp(rows, cols);
    for (int r = 0; r < rows; ++r)
    {
        for (int c = 0; c < cols; ++c)
        {
            const auto& p = points[static_cast<size_t>(r) * cols + c];
            if (!p.is_array() || p.size() != 2)
            {
                throw ParameterError("warp point must be [u, v]");
            }
            warp.setPointPosition(r, c, static_cast<float>(p[0].get<double>()),
                                  static_cast<float>(p[1].get<double>()));
        }
    }
    (*display)->warp = std::move(warp);
    return {{"display", id}, {"rows", rows}, {"cols", cols}};
}

// --- Parameter helpers -----------------------------------------------------

std::string CommandServer::requireString(const nlohmann::json& params, const char* key)
{
    if (!params.contains(key) || !params[key].is_string())
    {
        throw ParameterError(std::string("missing or invalid parameter: ") + key);
    }
    return params[key].get<std::string>();
}

double CommandServer::requireNumber(const nlohmann::json& params, const char* key)
{
    if (!params.contains(key) || !params[key].is_number())
    {
        throw ParameterError(std::string("missing or invalid parameter: ") + key);
    }
    return params[key].get<double>();
}

int CommandServer::requireInt(const nlohmann::json& params, const char* key)
{
    if (!params.contains(key) || !params[key].is_number())
    {
        throw ParameterError(std::string("missing or invalid parameter: ") + key);
    }
    return static_cast<int>(params[key].get<double>());
}

std::optional<std::string> CommandServer::optionalString(const nlohmann::json& params,
                                                         const char* key)
{
    if (params.contains(key) && params[key].is_string())
    {
        return params[key].get<std::string>();
    }
    return std::nullopt;
}

} // namespace mediamax::protocol
