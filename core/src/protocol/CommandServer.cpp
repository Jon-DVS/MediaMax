#include "mediamax/protocol/CommandServer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "mediamax/blend/Blend.h"
#include "mediamax/math/Transform2D.h"
#include "mediamax/media/MediaCache.h"
#include "mediamax/time/FrameRate.h"
#include "mediamax/util/Base64.h"
#include "mediamax/util/File.h"
#include "mediamax/util/Logger.h"
#include "mediamax/util/Sha256.h"

namespace mediamax::protocol
{

namespace
{

// The maximum payload of one media.putChunk: 1 MiB of file data keeps the
// base64 + JSON envelope well under the protocol's line limit (8 MiB)
// while the per-chunk round trip keeps the transfer's progress fine-grained.
constexpr std::size_t kMaxPutChunkBytes = 1u << 20;

// Validates a media cache path: non-empty, relative to the cache root, and
// without `..` components (the cache tree must stay inside the cache
// directory). On Windows drive-relative paths ("C:foo") are rejected as
// well - they are not absolute, but they are not cache-relative either.
void validateCachePath(const std::string& path)
{
    if (path.empty())
    {
        throw ParameterError("missing or invalid parameter: path");
    }
    const std::filesystem::path p(path);
    if (p.is_absolute())
    {
        throw ParameterError("path must be relative to the media cache: " + path);
    }
#if defined(_WIN32)
    if (p.has_root_name())
    {
        throw ParameterError("path must be relative to the media cache: " + path);
    }
#endif
    for (const std::filesystem::path& part : p)
    {
        if (part == "..")
        {
            throw ParameterError("path must not contain '..': " + path);
        }
    }
}

// Validates a sha256 hex checksum and normalizes it to the protocol's
// lowercase form (64 characters; compared case-insensitively, so an
// uppercase spelling is accepted).
std::string normalizeChecksum(const std::string& in)
{
    if (in.size() != 64)
    {
        throw ParameterError("checksum must be the 64-character sha256 hex of the file");
    }
    std::string out;
    out.reserve(64);
    for (const char c : in)
    {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
        {
            throw ParameterError("checksum must be the 64-character sha256 hex of the file");
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// A non-negative 64-bit integer parameter (requireInt is 32-bit; media file
// sizes exceed it).
std::uint64_t requireUint64(const nlohmann::json& params, const char* key)
{
    if (!params.contains(key) || !params[key].is_number_integer())
    {
        throw ParameterError("missing or invalid parameter: " + std::string(key) + " (integer)");
    }
    const std::int64_t v = params[key].get<std::int64_t>();
    if (v < 0)
    {
        throw ParameterError("parameter " + std::string(key) + " must be non-negative");
    }
    return static_cast<std::uint64_t>(v);
}

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
        {"warpEnabled", d.warpEnabled},
        // Transient operator state (the warp editor's live preview overlay),
        // reported so clients can see it and reconcile after reconnects.
        {"warpPreview", d.warpPreview},
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

// The protocol-level name for a log level (log.tail entries).
std::string levelString(LogLevel level)
{
    switch (level)
    {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warning: return "warning";
        case LogLevel::Error: return "error";
    }
    return "info";
}

// Case-insensitive name comparison (device names are vendor-cased; the
// UI passes them back verbatim, and operators may retype them).
bool sameNameInsensitive(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

// The engine display that carries this name (the show file's display
// entries key on the name; see applyShowDisplays).
std::optional<Id> findDisplayIdByName(DisplayManager& displays, const std::string& name)
{
    for (const auto& d : displays.displays())
    {
        if (d.name == name)
        {
            return d.id;
        }
    }
    return std::nullopt;
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
            "system.setRenderDevice",
            "system.disks",
            "system.setCacheDisk",
            "log.tail",
            "show.load",
            "show.loadJson",
            "show.save",
            "show.info",
            "media.load",
            "media.list",
            "media.unload",
            "media.thumbnail",
            "media.cacheList",
            "media.cacheCheck",
            "media.put",
            "media.putChunk",
            "media.putCancel",
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
        else if (cmd.method == "system.setRenderDevice")
        {
            result = handleSystemSetRenderDevice(cmd.params);
        }
        else if (cmd.method == "system.disks")
        {
            result = handleSystemDisks();
        }
        else if (cmd.method == "system.setCacheDisk")
        {
            result = handleSystemSetCacheDisk(cmd.params);
        }
        else if (cmd.method == "log.tail")
        {
            result = handleLogTail(cmd.params);
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
        else if (cmd.method == "media.cacheList")
        {
            result = handleMediaCacheList();
        }
        else if (cmd.method == "media.cacheCheck")
        {
            result = handleMediaCacheCheck(cmd.params);
        }
        else if (cmd.method == "media.put")
        {
            result = handleMediaPut(cmd.params);
        }
        else if (cmd.method == "media.putChunk")
        {
            result = handleMediaPutChunk(cmd.params);
        }
        else if (cmd.method == "media.putCancel")
        {
            result = handleMediaPutCancel(cmd.params);
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

    // The render device the engine runs on + what it could run on (the
    // control's system settings dialog selects from it). Omitted when the
    // app never reported device info (the core tests run without a GPU).
    nlohmann::json renderDevice;
    {
        std::lock_guard lock(m_RenderDeviceMutex);
        if (!m_RenderDeviceCurrent.empty() || !m_RenderDeviceAvailable.empty())
        {
            nlohmann::json available = nlohmann::json::array();
            for (const auto& d : m_RenderDeviceAvailable)
            {
                available.push_back({{"index", d.index}, {"name", d.name}, {"type", d.type}});
            }
            renderDevice = {
                {"current", m_RenderDeviceCurrent},
                {"type", m_RenderDeviceType},
                {"switching", m_RenderDeviceSwitching},
                {"available", available},
            };
        }
    }

    // The media-cache copy in flight (system.setCacheDisk across disks):
    // the app's copy worker reports it; omitted when no copy runs.
    nlohmann::json cacheCopy;
    {
        std::lock_guard lock(m_CacheCopyMutex);
        if (m_CacheCopyActive)
        {
            cacheCopy = {{"bytesCopied", m_CacheCopyBytes}, {"bytesTotal", m_CacheCopyTotal}};
        }
    }

    nlohmann::json result = {
        {"system", m_SystemName},
        {"version", MEDIAMAX_VERSION},
        {"hostname", m_HostName},
        {"ip", m_Ip},
        {"cacheDisk", m_CacheDisk},
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
    if (!renderDevice.is_null())
    {
        result["renderDevice"] = std::move(renderDevice);
    }
    if (!cacheCopy.is_null())
    {
        result["cacheCopy"] = std::move(cacheCopy);
    }
    return result;
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
            {"x", layer.x},
            {"y", layer.y},
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

nlohmann::json CommandServer::handleSystemSetRenderDevice(const nlohmann::json& params) const
{
    // "device" is optional: absent or empty means the engine's default
    // selection (the first eligible device, as at startup).
    const std::string device = optionalString(params, "device").value_or("");

    std::lock_guard lock(m_RenderDeviceMutex);
    if (!m_RenderDeviceSwitchFn)
    {
        throw ParameterError("this engine does not support render device selection");
    }
    if (m_RenderDeviceSwitching)
    {
        throw ParameterError("a render device switch is already in progress");
    }
    // Selecting the device the engine already runs on is a no-op.
    if (!device.empty() && sameNameInsensitive(device, m_RenderDeviceCurrent))
    {
        return {{"device", m_RenderDeviceCurrent}, {"switching", false}};
    }
    std::string error;
    if (!m_RenderDeviceSwitchFn(device, error))
    {
        throw ParameterError(error.empty() ? "the engine rejected the render device switch" : error);
    }
    // The switch runs on the app's frame loop (a GPU rebuild); the status
    // result's "renderDevice.switching" tracks it until the app clears it.
    m_RenderDeviceSwitching = true;
    return {{"device", device.empty() ? "(default)" : device}, {"switching", true}};
}

nlohmann::json CommandServer::handleSystemDisks() const
{
    if (!m_DisksFn)
    {
        throw ParameterError("disk enumeration is not available on this system");
    }
    std::vector<std::string> disks;
    std::string error;
    if (!m_DisksFn(disks, error))
    {
        throw ParameterError(error.empty() ? "disk enumeration failed" : error);
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& disk : disks)
    {
        arr.push_back(disk);
    }
    return {{"disks", arr}};
}

nlohmann::json CommandServer::handleSystemSetCacheDisk(const nlohmann::json& params) const
{
    // "disk" is required: a disk root ("C:\\"), or the empty string to
    // clear the cache (the cache folder is then left in place; media
    // resolution simply stops consulting it).
    const std::string disk = requireString(params, "disk");

    std::string root;
    if (!disk.empty())
    {
        const auto normalized = mediamax::media::normalizeDiskRoot(disk);
        if (!normalized)
        {
            throw ParameterError("not a disk root: " + disk + " (expected e.g. \"C:\\\")");
        }
        // The root must be a disk the machine actually has (system.disks):
        // a typo'd letter or a disconnected drive is rejected here, not
        // discovered mid-copy.
        if (!m_DisksFn)
        {
            throw ParameterError("disk enumeration is not available on this system");
        }
        std::vector<std::string> disks;
        std::string enumError;
        if (!m_DisksFn(disks, enumError))
        {
            throw ParameterError(enumError.empty() ? "disk enumeration failed" : enumError);
        }
        const bool known = std::any_of(disks.begin(), disks.end(), [&](const auto& candidate) {
            return candidate == *normalized;
        });
        if (!known)
        {
            throw ParameterError("disk not present on this system: " + normalized->string());
        }
        root = normalized->string();
    }

    // Unchanged: a no-op success (re-applying the show's systems entry on
    // every show load must not re-trigger the copy).
    if (root == m_CacheDisk)
    {
        nlohmann::json result = {{"cacheDisk", root}};
        if (!root.empty())
        {
            result["cacheDir"] =
                (std::filesystem::path(root) / mediamax::media::kCacheFolder).string();
        }
        result["copying"] = false;
        return result;
    }

    if (!m_CacheDiskRequestFn)
    {
        throw ParameterError("a media cache is not available on this system");
    }
    std::string error;
    if (!m_CacheDiskRequestFn(root, m_CacheDisk, error))
    {
        throw ParameterError(error.empty() ? "the engine rejected the cache disk change" : error);
    }
    m_CacheDisk = root;

    bool copying = false;
    {
        std::lock_guard lock(m_CacheCopyMutex);
        copying = m_CacheCopyActive;
    }
    nlohmann::json result = {{"cacheDisk", root}, {"copying", copying}};
    if (!root.empty())
    {
        result["cacheDir"] = (std::filesystem::path(root) / mediamax::media::kCacheFolder).string();
    }
    return result;
}

void CommandServer::pushLog(LogLevel level, const std::string& message)
{
    // Only the serious messages (warning and above) reach the control
    // surface - render faults, media failures, protocol errors. Debug/info
    // traffic is console-only.
    if (level < LogLevel::Warning)
    {
        return;
    }
    const double now = std::chrono::duration<double>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    std::lock_guard lock(m_LogMutex);
    m_Log.push_back(LogEntry{m_LogNextSeq++, level, now, message});
    while (m_Log.size() > kLogCapacity)
    {
        m_Log.pop_front();
    }
}

nlohmann::json CommandServer::handleLogTail(const nlohmann::json& params) const
{
    // "since" (optional, default 0): the last sequence number the client
    // has seen; the result carries every newer entry in the ring. Clients
    // pass "next" back as "since" on the next call (the control's poll
    // loop does, on every 500 ms cycle).
    std::uint64_t since = 0;
    if (params.contains("since"))
    {
        if (!params["since"].is_number())
        {
            throw ParameterError("log.tail: 'since' must be a number");
        }
        since = params["since"].get<std::uint64_t>();
    }
    nlohmann::json entries = nlohmann::json::array();
    std::uint64_t oldest = 0;
    std::uint64_t newest = 0;
    {
        std::lock_guard lock(m_LogMutex);
        if (!m_Log.empty())
        {
            oldest = m_Log.front().seq;
            newest = m_Log.back().seq;
        }
        for (const auto& entry : m_Log)
        {
            if (entry.seq <= since)
            {
                continue;
            }
            entries.push_back({{"seq", entry.seq},
                               {"level", levelString(entry.level)},
                               {"time", entry.time},
                               {"message", entry.message}});
        }
    }
    // "next" is the ring's newest seq - the client passes it back as
    // "since" (entries are delivered for seq > since, so a cursor at the
    // newest picks up exactly the newer ones). "truncated" tells the
    // client the ring dropped entries newer than "since" that it never saw
    // (it fell behind or polled after a restart): the GUI marks the gap
    // instead of pretending the list is complete. "started" identifies this
    // engine run (a change means the cursor must be reset - a fresh run's
    // ring starts at seq 1).
    return {{"started", m_LogStarted},
            {"next", newest},
            {"truncated", newest > 0 && oldest > since + 1},
            {"entries", entries}};
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
    // the file, so each loaded asset is remapped to its library id. A file
    // the original path cannot read falls back to this system's media cache
    // (the control interface's transfer target, see media.put). What is
    // missing from both gets a pending id and a pending entry: the show
    // runs with a placeholder for those layers, and the playback app
    // activates the media in place as the file appears. Relative paths
    // resolve against mediaBase (the show file's directory for show.load;
    // see resolveMediaPath).
    if (m_LoadMedia)
    {
        // A new show load resets the pending list: its entries refer to the
        // previous show's refs (a fresh load re-resolves every reference,
        // and a stale pending id can match neither a file-local nor a
        // library id, so a stale activation is a no-op).
        {
            std::lock_guard lock(m_PendingMutex);
            m_PendingMedia.clear();
        }

        std::map<Id, Id> remap;
        for (const auto& ref : show->media)
        {
            std::string loadError;
            const std::string mediaPath = resolveMediaPath(ref.path, mediaBase);
            const Id libraryId = loadMediaWithCacheFallback(mediaPath, ref.path, loadError);
            if (libraryId != kInvalidId)
            {
                remap[ref.id] = libraryId;
                continue;
            }
            // Missing on this system (original and cache): the ref gets a
            // pending id (a file-local id could collide with a live library
            // id, and the layer would composite the wrong asset) and a
            // pending entry for the playback app's late-arrival poll.
            const std::uint32_t pendingId = m_NextPendingMediaId.fetch_add(1);
            PendingMedia pending;
            pending.id = pendingId;
            pending.originalPath = mediaPath;
            pending.cachedPath = cachePathForMedia(ref.path);
            pending.fileName = std::filesystem::path(ref.path).filename().string();
            if (pending.fileName.empty())
            {
                pending.fileName = "media";
            }
            {
                std::lock_guard lock(m_PendingMutex);
                m_PendingMedia.push_back(std::move(pending));
            }
            Logger::instance().warning("show references missing media '{}' ({}): the layer draws "
                                       "a placeholder until the file appears (a transfer into "
                                       "the media cache or a copy to the original path activates "
                                       "it)",
                                       mediaPath, loadError);
            remap[ref.id] = pendingId;
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
    // The show's display wall follows the document (a no-op for shows
    // without a `displays` section).
    applyShowDisplays(*show);
    std::uint32_t pendingCount = 0;
    {
        std::lock_guard lock(m_PendingMutex);
        pendingCount = static_cast<std::uint32_t>(m_PendingMedia.size());
    }
    return {
        {"name", show->name},
        {"scenes", show->scenes.size()},
        {"media", show->media.size()},
        {"displays", show->displays.size()},
        {"pendingMedia", pendingCount},
    };
}

void CommandServer::applyShowDisplays(const Show& show) const
{
    if (show.displays.empty())
    {
        return;
    }
    // 1. Add every document display the engine does not have yet. The
    //    DisplayAddedFunction callback lets the app create the window and
    //    swapchain for the new record (it only queues the surface work;
    //    see DisplayAddedFunction's header comment).
    for (const ShowDisplay& sd : show.displays)
    {
        if (findDisplayIdByName(m_Displays, sd.name))
        {
            continue;
        }
        const std::int32_t w = sd.width > 0 ? sd.width : 1920;
        const std::int32_t h = sd.height > 0 ? sd.height : 1080;
        const Display added = m_Displays.addDisplay(sd.name, sd.x, sd.y, w, h);
        m_Displays.setDisplayBrightness(added.id, sd.brightness);
        m_Displays.setDisplayEnabled(added.id, sd.enabled);
        m_Displays.setDisplayAlwaysOnTop(added.id, sd.alwaysOnTop);
        m_Displays.setDisplayStatusOverlay(added.id, sd.statusOverlay);
        m_Displays.setDisplayVsync(added.id, sd.vsync);
        m_Displays.setDisplayWarpEnabled(added.id, sd.warpEnabled);
        if (const auto record = m_Displays.find(added.id))
        {
            (*record)->warp = sd.warp;
            if (m_DisplayAdded)
            {
                m_DisplayAdded(*(*record));
            }
        }
    }
    // 2. Update every document display that exists on both sides.
    for (const ShowDisplay& sd : show.displays)
    {
        const auto id = findDisplayIdByName(m_Displays, sd.name);
        if (!id)
        {
            continue;
        }
        if (sd.width > 0 && sd.height > 0)
        {
            m_Displays.setDisplayGeometry(*id, sd.x, sd.y, sd.width, sd.height);
        }
        m_Displays.setDisplayBrightness(*id, sd.brightness);
        m_Displays.setDisplayEnabled(*id, sd.enabled);
        m_Displays.setDisplayAlwaysOnTop(*id, sd.alwaysOnTop);
        m_Displays.setDisplayStatusOverlay(*id, sd.statusOverlay);
        m_Displays.setDisplayVsync(*id, sd.vsync);
        m_Displays.setDisplayWarpEnabled(*id, sd.warpEnabled);
        if (const auto record = m_Displays.find(*id))
        {
            (*record)->warp = sd.warp;
        }
    }
    // 3. Remove engine displays the document no longer defines.
    const std::vector<Display> current = m_Displays.displayList();
    for (const auto& d : current)
    {
        const bool inShow = std::any_of(show.displays.begin(), show.displays.end(),
                                        [&](const ShowDisplay& sd) { return sd.name == d.name; });
        if (!inShow)
        {
            if (m_DisplayRemoved)
            {
                m_DisplayRemoved(d.id);
            }
            m_Displays.removeDisplay(d.id);
        }
    }
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
    // Cache fallback: a path that only exists on the control machine loads
    // from this system's media cache (the transferred copy, see
    // media.put). The key is the path as the client wrote it (relative:
    // as written, absolute: the leaf - the same key the control side
    // transfers under).
    const Id id = loadMediaWithCacheFallback(path, path, error);
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

std::string CommandServer::requireCacheRoot() const
{
    const std::string noCache = "no media cache is configured on this system (system.setCacheDisk)";
    if (!m_CacheRootProvider)
    {
        throw ParameterError(noCache);
    }
    const std::string root = m_CacheRootProvider();
    if (root.empty())
    {
        throw ParameterError(noCache);
    }
    return root;
}

std::string CommandServer::cachePathForMedia(const std::string& cacheKeyPath) const
{
    if (!m_CacheRootProvider)
    {
        return std::string();
    }
    const std::string root = m_CacheRootProvider();
    if (root.empty())
    {
        return std::string();
    }
    return (media::cacheDirFor(std::filesystem::path(root)) / media::cacheKeyFor(cacheKeyPath)).string();
}

Id CommandServer::loadMediaWithCacheFallback(const std::string& path, const std::string& cacheKeyPath,
                                             std::string& error) const
{
    if (!m_LoadMedia)
    {
        error = "media loading is not available on this system";
        return kInvalidId;
    }
    const Id id = m_LoadMedia(path, error);
    if (id != kInvalidId)
    {
        return id;
    }
    // The original path is not readable on this system (it may only exist
    // on the control machine): try this system's media cache, where the
    // control interface's transfers (media.put) land the show's media. The
    // original's error is kept when there is nothing in the cache to try.
    std::string root;
    if (m_CacheRootProvider)
    {
        root = m_CacheRootProvider();
    }
    const std::optional<std::filesystem::path> cached =
        root.empty() ? std::nullopt : media::cachedPathFor(std::filesystem::path(root), cacheKeyPath);
    if (cached)
    {
        const std::string cachedPath = cached->string();
        std::string cacheError;
        const Id cachedId = m_LoadMedia(cachedPath, cacheError);
        if (cachedId != kInvalidId)
        {
            Logger::instance().info("media '{}' loaded from the media cache ({})", path, cachedPath);
            return cachedId;
        }
        error = "failed to load media from the media cache: " + cacheError;
    }
    return kInvalidId;
}

std::vector<PendingMedia> CommandServer::pendingMedia() const
{
    std::lock_guard lock(m_PendingMutex);
    return m_PendingMedia;
}

void CommandServer::clearPendingMedia(std::uint32_t id)
{
    std::lock_guard lock(m_PendingMutex);
    m_PendingMedia.erase(
        std::remove_if(m_PendingMedia.begin(), m_PendingMedia.end(),
                       [id](const PendingMedia& p) { return p.id == id; }),
        m_PendingMedia.end());
}

nlohmann::json CommandServer::handleMediaCacheList() const
{
    const std::string root = requireCacheRoot();
    const std::filesystem::path cacheDir = media::cacheDirFor(root);
    std::error_code ec;
    // No hashing here (media.cacheCheck is the per-file check): a cache
    // list of a multi-GB tree must stay fast.
    std::vector<std::pair<std::string, std::uint64_t>> list;
    if (std::filesystem::is_directory(cacheDir, ec) && !ec)
    {
        for (std::filesystem::recursive_directory_iterator it(cacheDir, ec), end;
             !ec && it != end; it.increment(ec))
        {
            if (ec)
            {
                break;
            }
            std::error_code fec;
            if (!it->is_regular_file(fec) || fec)
            {
                continue;
            }
            // Transfer temp files are in-flight, not cache content.
            const std::string name = it->path().filename().string();
            if (name.find(".put-") != std::string::npos && name.ends_with(".tmp"))
            {
                continue;
            }
            const std::filesystem::path rel = std::filesystem::relative(it->path(), cacheDir, fec);
            if (fec)
            {
                break;
            }
            const std::uint64_t bytes = std::filesystem::file_size(it->path(), fec);
            if (fec)
            {
                break;
            }
            // Forward slashes on the wire (the form the control side
            // writes); the platform separator is only in the tree itself.
            list.emplace_back(rel.generic_string(), bytes);
        }
    }
    if (ec)
    {
        throw ParameterError("cannot read the media cache: " + ec.message());
    }
    std::sort(list.begin(), list.end());
    nlohmann::json files = nlohmann::json::array();
    for (const auto& [rel, bytes] : list)
    {
        files.push_back({{"path", rel}, {"bytes", bytes}});
    }
    return {
        {"cacheDisk", root},
        {"files", files},
    };
}

nlohmann::json CommandServer::handleMediaCacheCheck(const nlohmann::json& params) const
{
    const std::string path = requireString(params, "path");
    validateCachePath(path);
    const std::string checksum = normalizeChecksum(requireString(params, "checksum"));
    const std::string root = requireCacheRoot();
    const std::filesystem::path file = media::cacheDirFor(root) / path;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec)
    {
        return {
            {"path", path},
            {"present", false},
        };
    }
    // A full-file hash takes seconds for a multi-GB file and the command
    // runs under the server's command mutex, so this briefly blocks other
    // commands - accepted: it is the transfer's pre-check, run once per
    // file, and hashing is far cheaper than transferring.
    std::string hash, error;
    if (!util::sha256File(file, hash, error))
    {
        throw ParameterError("cannot hash the cached file: " + error);
    }
    return {
        {"path", path},
        {"present", hash == checksum},
        {"bytes", std::filesystem::file_size(file, ec)},
    };
}

nlohmann::json CommandServer::handleMediaPut(const nlohmann::json& params) const
{
    const std::string path = requireString(params, "path");
    validateCachePath(path);
    const std::uint64_t bytes = requireUint64(params, "bytes");
    const std::string checksum = normalizeChecksum(requireString(params, "checksum"));
    const bool overwrite = optionalBool(params, "overwrite");
    const std::string root = requireCacheRoot();
    const std::filesystem::path finalFile = media::cacheDirFor(root) / path;

    // A cached file that already is the expected file means nothing to
    // transfer; a different file is a conflict unless the sender asked to
    // overwrite it.
    std::error_code ec;
    if (std::filesystem::is_regular_file(finalFile, ec) && !ec)
    {
        std::string hash, error;
        if (util::sha256File(finalFile, hash, error))
        {
            if (hash == checksum)
            {
                return {
                    {"path", path},
                    {"state", "current"},
                };
            }
            if (!overwrite)
            {
                return {
                    {"path", path},
                    {"state", "conflict"},
                    {"error", "the cache already holds a different file at this path; retry with overwrite"},
                };
            }
        }
    }

    std::lock_guard lock(m_TransferMutex);
    const std::uint32_t token = m_NextTransferToken++;
    const std::filesystem::path tempFile =
        finalFile.parent_path() / (finalFile.filename().string() + ".put-" + std::to_string(token) + ".tmp");
    std::error_code tec;
    std::filesystem::create_directories(finalFile.parent_path(), tec);
    auto stream = std::make_unique<std::ofstream>(tempFile, std::ios::binary | std::ios::trunc);
    if (tec || !stream->is_open())
    {
        throw ParameterError("cannot create the transfer temp file: " + tempFile.string() +
                             (tec ? " (" + tec.message() + ")" : std::string()));
    }
    TransferSession session;
    session.path = path;
    session.finalFile = finalFile;
    session.tempFile = tempFile;
    session.expectedBytes = bytes;
    session.expectedChecksum = checksum;
    session.tempStream = std::move(stream);
    m_Transfers.emplace(token, std::move(session));
    return {
        {"token", token},
        {"path", path},
    };
}

nlohmann::json CommandServer::handleMediaPutChunk(const nlohmann::json& params) const
{
    const std::uint32_t token = static_cast<std::uint32_t>(requireUint64(params, "token"));
    const std::uint64_t offset = requireUint64(params, "offset");
    const bool done = optionalBool(params, "done");
    std::vector<std::uint8_t> data;
    if (!util::base64Decode(requireString(params, "data"), data))
    {
        throw ParameterError("data is not valid base64");
    }
    if (data.size() > kMaxPutChunkBytes)
    {
        throw ParameterError("chunk data exceeds the 1 MiB limit (" + std::to_string(kMaxPutChunkBytes) +
                             " bytes)");
    }

    std::lock_guard lock(m_TransferMutex);
    const auto it = m_Transfers.find(token);
    if (it == m_Transfers.end())
    {
        throw ParameterError("no transfer with token " + std::to_string(token));
    }
    TransferSession& session = it->second;
    // Once a chunk of a found stream is rejected, the stream is over:
    // tear it down (temp file + session) so no half-open transfer lingers.
    // The client starts a fresh media.put.
    const auto reject = [&](const std::string& message) {
        session.tempStream->close();
        std::error_code ec;
        std::filesystem::remove(session.tempFile, ec);
        m_Transfers.erase(it);
        throw ParameterError(message);
    };
    if (offset != session.received)
    {
        reject("chunks must arrive in order: expected offset " +
               std::to_string(session.received) + ", got " + std::to_string(offset));
    }
    if (data.empty() && !(done && session.received == 0))
    {
        reject("chunk data is empty");
    }
    if (session.received + data.size() > session.expectedBytes)
    {
        reject("the transfer would exceed the declared size (" +
               std::to_string(session.expectedBytes) + " bytes)");
    }

    if (!data.empty())
    {
        session.tempStream->write(reinterpret_cast<const char*>(data.data()),
                                  static_cast<std::streamsize>(data.size()));
        session.hash.update(data.data(), data.size());
    }
    session.received += data.size();

    if (!done)
    {
        return {
            {"token", token},
            {"state", "progress"},
            {"received", session.received},
        };
    }

    // Final chunk: verify the stream, then move the temp file into place
    // (an atomic rename on the same volume).
    if (session.received != session.expectedBytes)
    {
        reject("declared " + std::to_string(session.expectedBytes) + " bytes, received " +
               std::to_string(session.received));
    }
    const std::string actual = session.hash.hex();
    if (actual != session.expectedChecksum)
    {
        reject("checksum mismatch: got " + actual + ", expected " + session.expectedChecksum);
    }
    if (!session.tempStream->good())
    {
        reject("cannot write the transfer temp file: " + session.tempFile.string());
    }
    session.tempStream->close();
    std::error_code ec;
    std::filesystem::rename(session.tempFile, session.finalFile, ec);
    const std::string landedPath = session.path;
    m_Transfers.erase(it);
    if (ec)
    {
        std::error_code rec;
        std::filesystem::remove(session.tempFile, rec);
        throw ParameterError("cannot move the transfer into place: " + ec.message());
    }
    return {
        {"token", token},
        {"state", "done"},
        {"path", landedPath},
    };
}

nlohmann::json CommandServer::handleMediaPutCancel(const nlohmann::json& params) const
{
    const std::uint32_t token = static_cast<std::uint32_t>(requireUint64(params, "token"));
    std::lock_guard lock(m_TransferMutex);
    const auto it = m_Transfers.find(token);
    if (it == m_Transfers.end())
    {
        throw ParameterError("no transfer with token " + std::to_string(token));
    }
    it->second.tempStream->close();
    std::error_code ec;
    std::filesystem::remove(it->second.tempFile, ec);
    m_Transfers.erase(it);
    return {{"token", token}};
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
    // Deactivates all scenes: whatever is active now (a manual set, or the
    // timeline's scene) is replaced by the deliberate empty manual set - no
    // scene is active (black output). It stays in effect until a scene is
    // set/toggled or a playScene cue is crossed. The response reports how
    // many scenes were deactivated and the now-empty active set, so the
    // caller does not need a follow-up status call.
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
    // Projection warp on/off: when off the renderer skips the warp texture
    // and the per-pixel lookup entirely (the grid itself is kept).
    if (params.contains("warpEnabled"))
    {
        m_Displays.setDisplayWarpEnabled(id, params["warpEnabled"].get<bool>());
    }
    // Transient warp-preview overlay (grid + nodes on the output while the
    // warp editor is open); off by default, cleared by the editor's close.
    if (params.contains("warpPreview"))
    {
        m_Displays.setDisplayWarpPreview(id, params["warpPreview"].get<bool>());
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
