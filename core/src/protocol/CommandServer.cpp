#include "mediamax/protocol/CommandServer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "mediamax/blend/Blend.h"
#include "mediamax/math/Transform2D.h"
#include "mediamax/time/FrameRate.h"
#include "mediamax/util/Base64.h"
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
        // Codec-level probe details (additive; empty string / 0 when the
        // container does not report them - see MediaMetadata).
        {"codec", asset.metadata.videoCodec},
        {"colorspace", asset.metadata.videoColorspace},
        {"bitDepth", asset.metadata.videoBitDepth},
        {"scanType", asset.metadata.videoScanType},
        {"bitRate", asset.metadata.videoBitRate},
        {"audioCodec", asset.metadata.audioCodec},
    };
}

// The wire string for a warp interpolation (PROTOCOL.md display.warp.set).
const char* warpModeToString(WarpInterpolation mode)
{
    return mode == WarpInterpolation::Spline ? "spline" : "grid";
}

nlohmann::json displayToJson(const Display& d)
{
    nlohmann::json points = nlohmann::json::array();
    for (const auto& p : d.warp.pointPositions())
    {
        points.push_back({p[0], p[1]});
    }
    return {
        {"id", d.id},
        {"name", d.name},
        {"x", d.x},
        {"y", d.y},
        {"width", d.width},
        {"height", d.height},
        {"brightness", d.brightness},
        {"enabled", d.enabled},
        {"alwaysOnTop", d.alwaysOnTop},
        {"statusOverlay", d.statusOverlay},
        {"vsync", d.vsync},
        {"monitor", d.monitor},
        // The current projection warp (display.warp.set's `points` layout:
        // absolute positions in display-local UV, row-major). Clients use
        // it to restore the warp editor's state.
        {"warp", {{"mode", warpModeToString(d.warp.interpolation())}, {"rows", d.warp.rows()},
                  {"cols", d.warp.cols()}, {"points", points}}},
    };
}

nlohmann::json idArray(const std::vector<Id>& ids)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const Id id : ids)
    {
        arr.push_back(id);
    }
    return arr;
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
            "frame.state",
            "system.set",
            "show.load",
            "show.loadJson",
            "show.save",
            "show.info",
            "media.load",
            "media.list",
            "media.unload",
            "media.thumbnail",
            "playback.play",
            "playback.pause",
            "playback.stop",
            "playback.seek",
            "scene.set",
            "scene.toggle",
            "scene.list",
            "scene.clear",
            "layer.set",
            "layer.clear",
            "display.add",
            "display.list",
            "display.remove",
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
        else if (cmd.method == "frame.state")
        {
            result = handleFrameState();
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
        else if (cmd.method == "media.thumbnail")
        {
            result = handleMediaThumbnail(cmd.params);
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
        else if (cmd.method == "scene.toggle")
        {
            result = handleSceneToggle(cmd.params);
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
        else if (cmd.method == "display.add")
        {
            result = handleDisplayAdd(cmd.params);
        }
        else if (cmd.method == "display.list")
        {
            result = handleDisplayList();
        }
        else if (cmd.method == "display.remove")
        {
            result = handleDisplayRemove(cmd.params);
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
        {"version", MEDIAMAX_VERSION},
        {"hostname", m_HostName},
        {"ip", m_Ip},
        {"droppedVideoFrames", m_DroppedVideoFrames.load()},
        {"state", PlaybackEngine::stateToString(m_Engine.state())},
        {"time", state.showTime.toSeconds()},
        {"timecode", state.showTime.toTimecode(show.frameRate)},
        {"duration", show.duration.isValid() ? show.duration.toSeconds() : -1.0},
        {"frameRate", show.frameRate.toString()},
        {"scene", state.activeSceneId},
        {"activeScenes", idArray(state.activeSceneIds)},
        {"previewScene", m_Engine.previewScene()},
        {"layers", state.layers.size()},
        {"media", mediaArr},
        {"displays", displayArr},
        {"scenes", sceneArr},
    };
}

nlohmann::json CommandServer::handleFrameState() const
{
    // The resolved, per-layer state of the frame the engine is currently
    // (or would) render: the display wall plus every visible layer with its
    // content footprint in output space. This is the data the control GUI's
    // 2D "show view" draws from (PROTOCOL.md: frame.state).
    const FrameState state = m_Engine.frameState();
    const Rect2i bounds = m_Displays.outputBounds();

    // Layer names are not carried by LayerFrame; map them from the show.
    std::unordered_map<Id, const Layer*> byId;
    for (const auto& scene : m_Engine.show().scenes)
    {
        for (const auto& layer : scene.layers)
        {
            byId[layer.id] = &layer;
        }
    }

    nlohmann::json displayArr = nlohmann::json::array();
    for (const auto& d : m_Displays.displays())
    {
        displayArr.push_back({
            {"id", d.id},
            {"name", d.name},
            {"x", d.x},
            {"y", d.y},
            {"width", d.width},
            {"height", d.height},
            {"enabled", d.enabled},
        });
    }

    nlohmann::json layerArr = nlohmann::json::array();
    for (const auto& layer : state.layers)
    {
        const bool isColor = layer.mediaId == kInvalidId;
        const MediaAsset* asset = nullptr;
        if (!isColor)
        {
            if (const auto found = m_Media.find(layer.mediaId))
            {
                asset = *found;
            }
        }
        // Layers without video content (audio-only media, or media that
        // disappeared from the library) have nothing to preview.
        if (!isColor && (asset == nullptr || !asset->metadata.hasVideo))
        {
            continue;
        }

        // The content footprint: a color layer fills the output space, a
        // media layer is placed at its native media size - the same
        // convention the playback renderer uses.
        Size2i mediaSize = bounds.size();
        std::string content = "color";
        if (!isColor)
        {
            if (asset->metadata.width > 0 && asset->metadata.height > 0)
            {
                mediaSize = {static_cast<std::int32_t>(asset->metadata.width),
                             static_cast<std::int32_t>(asset->metadata.height)};
            }
            content = asset->metadata.type == MediaType::Image ? "image" : "video";
        }

        Transform2D t;
        t.x = layer.x;
        t.y = layer.y;
        t.rotation = layer.rotation;
        t.scaleX = layer.scaleX;
        t.scaleY = layer.scaleY;
        t.anchorX = layer.anchorX;
        t.anchorY = layer.anchorY;

        // The transformed corners, clockwise from the top-left in output
        // space (y down): the client draws a single quad from these.
        nlohmann::json corners = nlohmann::json::array();
        const float uvs[4][2] = {{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}};
        for (const auto& uv : uvs)
        {
            const auto [px, py] = t.toOutputSpace(uv[0], uv[1], mediaSize);
            corners.push_back({px, py});
        }

        nlohmann::json layerJson = {
            {"id", layer.layerId},
            {"name", byId.count(layer.layerId) ? byId.at(layer.layerId)->name : std::string()},
            {"content", content},
            {"corners", corners},
            {"opacity", layer.opacity},
            {"zIndex", layer.zIndex},
        };
        if (isColor)
        {
            layerJson["color"] = {layer.color.r, layer.color.g, layer.color.b, layer.color.a};
        }
        else
        {
            layerJson["mediaId"] = layer.mediaId;
        }
        layerArr.push_back(std::move(layerJson));
    }

    return {
        {"sceneSize", {{"width", bounds.width}, {"height", bounds.height}}},
        {"displays", displayArr},
        {"layers", layerArr},
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
    // A show is a self-contained folder: resolve its media against the show
    // file's directory, not the engine process's working directory.
    return applyShowDocument(parsed, "show file", optionalBool(params, "preserveState"),
                             std::filesystem::path(path).parent_path());
}

// Parses a show document (a parsed JSON object), loads and remaps the media it
// references, and makes the show active. Shared by show.load and show.loadJson.
nlohmann::json CommandServer::applyShowDocument(const nlohmann::json& parsed,
                                                const char* context,
                                                bool preserveState,
                                                const std::filesystem::path& mediaBase) const
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
    // the show. Relative paths resolve against mediaBase (the show file's
    // directory for show.load; see resolveMediaPath).
    if (m_LoadMedia)
    {
        std::map<Id, Id> remap;
        for (const auto& ref : show->media)
        {
            std::string loadError;
            const std::string mediaPath = resolveMediaPath(ref.path, mediaBase);
            const Id libraryId = m_LoadMedia(mediaPath, loadError);
            if (libraryId == kInvalidId)
            {
                Logger::instance().warning("Show references missing media '{}': {}", mediaPath,
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

    m_Engine.loadShow(*show, preserveState);
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
    // mediaDir: the directory the document's relative media paths resolve
    // against - an authoring tool sends the directory of the show file it
    // has open, so a push behaves like a show.load of that file. Omitted:
    // relative paths are used as written (process working directory).
    std::filesystem::path mediaBase;
    if (const auto mediaDir = optionalString(params, "mediaDir"); mediaDir && !mediaDir->empty())
    {
        mediaBase = std::filesystem::path(*mediaDir);
    }
    return applyShowDocument(params.at("show"), "show JSON", optionalBool(params, "preserveState"),
                             mediaBase);
}

std::string CommandServer::resolveMediaPath(const std::string& path,
                                            const std::filesystem::path& mediaBase)
{
    std::filesystem::path p(path);
    if (!p.is_absolute() && !mediaBase.empty())
    {
        p = mediaBase / p;
    }
    return p.string();
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

nlohmann::json CommandServer::handleMediaThumbnail(const nlohmann::json& params) const
{
    const Id id = static_cast<Id>(requireInt(params, "id"));
    // Request box for the aspect-fit result (PROTOCOL.md: media.thumbnail).
    // Defaults are the control GUI's preview size; the caps keep the base64
    // payload well under the line limit even for large requests.
    const int width = std::clamp(optionalInt(params, "width").value_or(320), 16, 640);
    const int height = std::clamp(optionalInt(params, "height").value_or(180), 16, 360);
    if (!m_Thumbnail)
    {
        throw ParameterError("media thumbnails are not available on this system");
    }

    std::vector<std::uint8_t> rgba;
    int outWidth = 0;
    int outHeight = 0;
    std::string error;
    if (!m_Thumbnail(id, width, height, rgba, outWidth, outHeight, error))
    {
        throw ParameterError(error.empty() ? "thumbnail failed" : error);
    }
    return {
        {"id", id},
        {"width", outWidth},
        {"height", outHeight},
        {"data", mediamax::util::base64Encode(rgba)},
    };
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
    return {{"scene", sceneId},
            {"name", show.scenes[static_cast<size_t>(index)].name},
            {"activeScenes", idArray(m_Engine.activeScenesAt(m_Engine.time()))}};
}

nlohmann::json CommandServer::handleSceneToggle(const nlohmann::json& params) const
{
    const int index = requireInt(params, "index");
    const Show& show = m_Engine.show();
    if (index < 0 || index >= static_cast<int>(show.scenes.size()))
    {
        throw ParameterError("scene index out of range: " + std::to_string(index));
    }
    const Id sceneId = show.scenes[static_cast<size_t>(index)].id;
    if (!m_Engine.toggleSceneNow(sceneId))
    {
        throw ParameterError("cannot toggle scene " + std::to_string(sceneId));
    }
    const std::vector<Id> active = m_Engine.activeScenesAt(m_Engine.time());
    return {
        {"scene", sceneId},
        {"name", show.scenes[static_cast<size_t>(index)].name},
        {"active", std::find(active.begin(), active.end(), sceneId) != active.end()},
        {"activeScenes", idArray(active)},
    };
}

nlohmann::json CommandServer::handleSceneList() const
{
    nlohmann::json arr = nlohmann::json::array();
    const Show& show = m_Engine.show();
    const std::vector<Id> activeIds = m_Engine.activeScenesAt(m_Engine.time());
    // Layers visible in the current frame report their position; a hidden
    // layer (visibility/opacity) reports none.
    std::unordered_set<Id> visibleLayers;
    for (const auto& layerFrame : m_Engine.frameState().layers)
    {
        visibleLayers.insert(layerFrame.layerId);
    }
    for (const auto& scene : show.scenes)
    {
        const bool sceneActive =
            std::find(activeIds.begin(), activeIds.end(), scene.id) != activeIds.end();
        nlohmann::json layerArr = nlohmann::json::array();
        for (const auto& layer : scene.layers)
        {
            nlohmann::json layerJson = {{"id", layer.id}, {"name", layer.name}};
            if (sceneActive && visibleLayers.count(layer.id) > 0)
            {
                // The control position: the authored value with timeline
                // cues and manual overrides applied, without keyframe-track
                // animation - stable while a track plays, so the control
                // surface's position fields do not chase the animation.
                float x = 0.f, y = 0.f;
                if (m_Engine.layerControlPosition(layer.id, x, y))
                {
                    layerJson["x"] = x;
                    layerJson["y"] = y;
                }
            }
            layerJson["override"] = m_Engine.hasLayerOverride(layer.id);
            layerArr.push_back(std::move(layerJson));
        }
        arr.push_back({
            {"id", scene.id},
            {"name", scene.name},
            {"active", sceneActive},
            {"layers", layerArr},
        });
    }
    return {{"scenes", arr}};
}

nlohmann::json CommandServer::handleSceneClear() const
{
    // Releases the operator's manual scene set (scene.set/scene.toggle);
    // scene selection returns to the timeline. The response reports the
    // now-active scenes so the caller does not need a follow-up status call.
    const int cleared = m_Engine.clearSceneNow();
    const std::vector<Id> active = m_Engine.activeScenesAt(m_Engine.time());
    return {
        {"cleared", cleared},
        {"scene", active.empty() ? kInvalidId : active.front()},
        {"activeScenes", idArray(active)},
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

nlohmann::json CommandServer::handleDisplayAdd(const nlohmann::json& params) const
{
    // Defaults: tile a full-HD panel to the right of the current wall (an
    // empty wall starts at the origin).
    const auto wall = m_Displays.outputBounds();
    std::int32_t x =
        params.contains("x") ? static_cast<std::int32_t>(params["x"].get<double>()) : wall.maxX();
    std::int32_t y = params.contains("y") ? static_cast<std::int32_t>(params["y"].get<double>()) : 0;
    std::int32_t width =
        params.contains("width") ? static_cast<std::int32_t>(params["width"].get<double>()) : 1920;
    std::int32_t height =
        params.contains("height") ? static_cast<std::int32_t>(params["height"].get<double>()) : 1080;
    if (width <= 0 || height <= 0)
    {
        throw ParameterError("invalid display geometry");
    }

    std::string name;
    if (params.contains("name") && params["name"].is_string())
    {
        name = params["name"].get<std::string>();
    }
    Display display = m_Displays.addDisplay(name, x, y, width, height);
    if (name.empty())
    {
        // Default name derived from the id the display got.
        if (const auto updated = m_Displays.find(display.id))
        {
            (*updated)->name = "Display " + std::to_string(display.id);
            display = **updated;
        }
    }

    if (m_DisplayAdded)
    {
        m_DisplayAdded(display);
    }
    return displayToJson(display);
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

nlohmann::json CommandServer::handleDisplayRemove(const nlohmann::json& params) const
{
    const Id id = static_cast<Id>(requireInt(params, "display"));
    if (!m_Displays.find(id))
    {
        throw ParameterError("no display with id " + std::to_string(id));
    }
    if (m_DisplayRemoved)
    {
        m_DisplayRemoved(id);
    }
    m_Displays.removeDisplay(id);
    return {{"display", id}};
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
    if (params.contains("alwaysOnTop"))
    {
        m_Displays.setDisplayAlwaysOnTop(id, params["alwaysOnTop"].get<bool>());
    }
    if (params.contains("statusOverlay"))
    {
        m_Displays.setDisplayStatusOverlay(id, params["statusOverlay"].get<bool>());
    }
    if (params.contains("vsync"))
    {
        m_Displays.setDisplayVsync(id, params["vsync"].get<bool>());
    }
    if (params.contains("name") && params["name"].is_string())
    {
        m_Displays.setDisplayName(id, params["name"].get<std::string>());
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

    // The interpolation between the nodes: "grid" (bilinear, the default,
    // so existing clients keep their behavior) or "spline" (a cubic
    // B-spline through the nodes).
    WarpInterpolation mode = WarpInterpolation::Bilinear;
    if (params.contains("mode"))
    {
        if (!params["mode"].is_string())
        {
            throw ParameterError("warp mode must be \"grid\" or \"spline\"");
        }
        const std::string m = params["mode"].get<std::string>();
        if (m == "grid")
        {
            mode = WarpInterpolation::Bilinear;
        }
        else if (m == "spline")
        {
            mode = WarpInterpolation::Spline;
        }
        else
        {
            throw ParameterError("warp mode must be \"grid\" or \"spline\"");
        }
    }

    WarpMap warp(rows, cols, mode);
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
    return {{"display", id}, {"rows", rows}, {"cols", cols}, {"mode", warpModeToString(mode)}};
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

std::optional<int> CommandServer::optionalInt(const nlohmann::json& params, const char* key)
{
    if (!params.contains(key) || params[key].is_null())
    {
        return std::nullopt;
    }
    if (!params[key].is_number())
    {
        throw ParameterError(std::string("invalid parameter: ") + key + " must be a number");
    }
    return static_cast<int>(params[key].get<double>());
}

bool CommandServer::optionalBool(const nlohmann::json& params, const char* key)
{
    if (params.contains(key) && params[key].is_boolean())
    {
        return params[key].get<bool>();
    }
    return false;
}

} // namespace mediamax::protocol
