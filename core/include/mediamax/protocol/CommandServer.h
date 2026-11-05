#pragma once

// The command server: maps control-protocol commands onto the playback
// engine, media library and display manager.
//
// It is engine-agnostic in the same sense as PlaybackEngine: no GPU, no
// network - given a command it produces a response. This makes the entire
// control surface unit-testable (see core/tests/test_CommandServer.cpp), and
// lets the control interface validate commands before sending.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mediamax/display/DisplayManager.h"
#include "mediamax/media/MediaLibrary.h"
#include "mediamax/playback/PlaybackEngine.h"
#include "mediamax/protocol/Protocol.h"
#include "mediamax/util/Logger.h"
#include "mediamax/util/Sha256.h"

namespace mediamax::protocol
{

// Host callback for commands that need to load files into the media library
// (the app owns the source factory). Implemented by the playback engine.
using MediaLoadFunction = std::function<Id(const std::string& path, std::string& error)>;

// Host callbacks for display.add / display.remove: the protocol layer adds
// or removes the display record, the app owns the window and swapchain
// surfaces for it. The playback app queues the surface work and executes it
// on its frame loop (window/surface creation is main-thread work). Tests
// may record the calls instead. The callbacks run while handle() executes
// (under the caller's lock) and must not block.
using DisplayAddedFunction = std::function<void(const Display& display)>;
using DisplayRemovedFunction = std::function<void(Id displayId)>;

// Host callback for media.thumbnail: the app decodes the frame (it owns the
// source factory and the asset lookup, and a decode must not run under the
// caller's state lock - the playback app dispatches this command without it
// and re-locks only around the short library lookup). Unlike the display
// callbacks this one may block for the duration of the decode. Tests
// implement it with media::makeThumbnail directly.
using ThumbnailFunction = std::function<bool(Id mediaId, int maxWidth, int maxHeight,
                                             std::vector<std::uint8_t>& outRgba, int& outWidth,
                                             int& outHeight, std::string& error)>;

// One render (Vulkan) device the engine can run on - a "renderDevice.
// available" entry in the status result.
struct RenderDeviceInfo
{
    std::uint32_t index = 0; // position in the instance enumeration
    std::string name;
    std::string type; // "Discrete GPU" / "Integrated GPU" / "Virtual GPU" / ...
};

// Host callback for system.setRenderDevice: the app owns the Vulkan
// context, so validating the request and queueing the GPU rebuild is
// app work. It runs while handle() executes (under the caller's lock) and
// must not block; true means the switch was accepted - the app executes it
// on its frame loop and clears the "switching" flag through
// setRenderDeviceSwitchState() when it finishes (the status result reports
// the progress). `device` is the requested device name; empty means the
// engine's default selection (first eligible device).
using RenderDeviceSwitchFunction = std::function<bool(const std::string& device, std::string& error)>;

// Host callback for system.disks: the app enumerates the disks present on
// the machine (the core has no platform access). Returns false with
// `error` when the enumeration itself fails.
using DisksFunction = std::function<bool(std::vector<std::string>& disks, std::string& error)>;

// Host callback for system.setCacheDisk: the app owns the cache folder
// (its creation, the copy of an existing cache to the new disk, and the
// app's own cache-disk state), so applying the request is app work.
// `diskRoot` is the validated canonical disk root (the empty string clears
// the cache); `previousRoot` is the current one (the app starts the
// cache copy when the two differ and the old cache has content). It runs
// while handle() executes (under the caller's lock) and must not block for
// long (a cache copy is spawned, not performed, here).
using CacheDiskRequestFunction =
    std::function<bool(const std::string& diskRoot, const std::string& previousRoot,
                       std::string& error)>;

// Host callback for the media-cache commands (media.cacheList,
// media.cacheCheck, media.put / media.putChunk / media.putCancel): returns
// the canonical disk root of this system's media cache (e.g. "C:\\") or
// the empty string when no cache disk is configured. The app provides it
// (the command server has no platform state); the commands fail with a
// clean error until one is installed and it reports a root.
using CacheRootProviderFunction = std::function<std::string()>;

// The id range reserved for pending media (show references whose file is
// not on this system yet): allocated upward from here, so a pending id can
// never collide with a library id (the IdAllocator starts at 1) or a
// small file-local show id.
inline constexpr std::uint32_t kPendingMediaIdBase = 1u << 28;

// A media reference that could not be loaded when the show was loaded
// (neither the original path nor this system's media cache was readable):
// the show runs with a placeholder for those layers, and the playback app
// polls pendingMedia() until a file appears (a transfer landing in the
// cache, or a copy to the original path), then loads it into the library
// and remaps the show in place (pending id -> library id).
struct PendingMedia
{
    std::uint32_t id = 0; // the id the show's refs/layers carry
    std::string originalPath; // the resolved original path (media base applied)
    std::string cachedPath; // this system's cache path for the file ("" = no cache disk)
    std::string fileName; // display label (the path as written, leaf)
};

class CommandServer
{
public:
    CommandServer(PlaybackEngine& engine, MediaLibrary& media, DisplayManager& displays,
                  std::string systemName)
        : m_Engine(engine)
        , m_Media(media)
        , m_Displays(displays)
        , m_SystemName(std::move(systemName))
        , m_LogStarted(
              std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
                  .count())
    {
    }

    // Sets the media-loading callback (see MediaLoadFunction).
    void setMediaLoadFunction(MediaLoadFunction fn) { m_LoadMedia = std::move(fn); }

    // Sets the display add/remove callbacks (see above).
    void setDisplayAddedFunction(DisplayAddedFunction fn) { m_DisplayAdded = std::move(fn); }
    void setDisplayRemovedFunction(DisplayRemovedFunction fn) { m_DisplayRemoved = std::move(fn); }

    // Sets the media-thumbnail callback (see ThumbnailFunction). media.
    // thumbnail fails with a clean error until one is installed.
    void setThumbnailFunction(ThumbnailFunction fn) { m_Thumbnail = std::move(fn); }

    // Sets the render-device callback (see RenderDeviceSwitchFunction).
    // system.setRenderDevice fails with a clean error until one is
    // installed (the core tests run without a GPU).
    void setRenderDeviceSwitchFunction(RenderDeviceSwitchFunction fn)
    {
        m_RenderDeviceSwitchFn = std::move(fn);
    }

    // Sets the disk-enumeration callback (see DisksFunction). system.disks
    // and the disk validation of system.setCacheDisk fail with a clean
    // error until one is installed.
    void setDiskFunction(DisksFunction fn) { m_DisksFn = std::move(fn); }

    // Sets the cache-disk apply callback (see CacheDiskRequestFunction).
    // system.setCacheDisk fails with a clean error until one is installed.
    void setCacheDiskRequestFunction(CacheDiskRequestFunction fn)
    {
        m_CacheDiskRequestFn = std::move(fn);
    }

    // Sets the cache-root provider (see CacheRootProviderFunction). The
    // media-cache commands (media.cacheList / media.cacheCheck / media.put
    // and its chunk/cancel pair) fail with a clean error until one is
    // installed and it reports a root. Media resolution (show.load,
    // media.load) uses the same provider for its cache fallback.
    void setCacheRootProvider(CacheRootProviderFunction fn)
    {
        m_CacheRootProvider = std::move(fn);
    }

    // A snapshot of the media pending from the last show load (see
    // PendingMedia): the playback app polls this from its frame loop and
    // activates an entry - loads the file into the library and remaps the
    // show in place - as soon as the file appears, then drops it with
    // clearPendingMedia().
    std::vector<PendingMedia> pendingMedia() const;

    // Marks a pending media as activated (or the entry stale after a show
    // change). No-op when the id is not pending.
    void clearPendingMedia(std::uint32_t id);

    // The cache disk (a canonical disk root, or empty) reported in
    // "status". Set by system.setCacheDisk (after the app accepted the
    // request) and by the app itself when it applies the setting outside
    // the command path (startup flag, the show's own systems entry).
    void setCacheDisk(const std::string& diskRoot) { m_CacheDisk = diskRoot; }
    std::string cacheDisk() const { return m_CacheDisk; }

    // The cache-copy progress reported in "status" as `cacheCopy`
    // ({"bytesCopied","bytesTotal"}) while a copy across disks runs: the
    // app's copy worker updates it, the final call passes active = false.
    void setCacheCopyProgress(bool active, std::uint64_t bytesCopied, std::uint64_t bytesTotal)
    {
        std::lock_guard lock(m_CacheCopyMutex);
        m_CacheCopyActive = active;
        m_CacheCopyBytes = bytesCopied;
        m_CacheCopyTotal = bytesTotal;
    }

    // The render devices reported in "status" (the "renderDevice" block):
    // the current device + type and the full available list. The playback
    // app fills this in after Vulkan initialization (the core has no GPU
    // access); the status result omits the "renderDevice" field until
    // setRenderDeviceInfo has been called with a non-empty name or list.
    void setRenderDeviceInfo(const std::string& current, const std::string& type,
                             const std::vector<RenderDeviceInfo>& available)
    {
        std::lock_guard lock(m_RenderDeviceMutex);
        m_RenderDeviceCurrent = current;
        m_RenderDeviceType = type;
        m_RenderDeviceAvailable = available;
    }

    // True while a requested device switch is executing on the app's frame
    // loop (set true by system.setRenderDevice when accepted; the app
    // clears it when the switch finishes - success or no-op).
    void setRenderDeviceSwitchState(bool switching)
    {
        std::lock_guard lock(m_RenderDeviceMutex);
        m_RenderDeviceSwitching = switching;
    }

    // Feeds the log.tail ring buffer from the process logger: the app
    // installs a Logger::Listener that forwards every emitted message here.
    // Only Warning and above is stored (the "serious" messages the control
    // surface reports - render faults, media failures, protocol errors);
    // the buffer keeps the last kLogCapacity entries.
    static constexpr std::size_t kLogCapacity = 500;
    void pushLog(LogLevel level, const std::string& message);

    // The machine identity reported in "status": the computer name
    // (`hostname`) and primary IPv4 address (`ip`) of the machine running
    // the engine. The app fills these in (the core has no platform access).
    void setSystemHost(const std::string& hostName) { m_HostName = hostName; }
    void setSystemIp(const std::string& ip) { m_Ip = ip; }

    // Video frames dropped so far (high water, all layers), reported in
    // "status" as `droppedVideoFrames`. The playback app updates this from
    // its media pump every frame (the core has no access to the pump).
    // Atomic: written by the render thread, read by the command threads.
    void setDroppedVideoFrames(std::uint64_t count) { m_DroppedVideoFrames.store(count); }

    // Handles a decoded command. Never throws; all failures become responses
    // with ok=false.
    Response handle(const Command& cmd) const;

    // Static helpers used by both the command implementations and tests.
    static std::vector<std::string> supportedMethods();

    // Resolves a media path from a show document against `mediaBase`:
    // absolute paths pass through unchanged, relative paths are qualified
    // with the base directory (an empty base leaves the path as written).
    static std::string resolveMediaPath(const std::string& path,
                                        const std::filesystem::path& mediaBase);

private:
    // --- Individual command handlers (all return a result JSON) -------------

    nlohmann::json handlePing() const;
    nlohmann::json handleStatus() const;
    nlohmann::json handleFrameState() const;
    nlohmann::json handleSystemSet(const nlohmann::json& params) const;
    nlohmann::json handleSystemSetRenderDevice(const nlohmann::json& params) const;
    nlohmann::json handleSystemDisks() const;
    nlohmann::json handleSystemSetCacheDisk(const nlohmann::json& params) const;
    nlohmann::json handleLogTail(const nlohmann::json& params) const;
    nlohmann::json handleShowLoad(const nlohmann::json& params) const;
    nlohmann::json handleShowLoadJson(const nlohmann::json& params) const;
    nlohmann::json handleShowSave(const nlohmann::json& params) const;
    nlohmann::json handleShowInfo() const;
    nlohmann::json handleMediaLoad(const nlohmann::json& params) const;
    nlohmann::json handleMediaList() const;
    nlohmann::json handleMediaUnload(const nlohmann::json& params) const;
    nlohmann::json handleMediaThumbnail(const nlohmann::json& params) const;
    nlohmann::json handleMediaCacheList() const;
    nlohmann::json handleMediaCacheCheck(const nlohmann::json& params) const;
    nlohmann::json handleMediaPut(const nlohmann::json& params) const;
    nlohmann::json handleMediaPutChunk(const nlohmann::json& params) const;
    nlohmann::json handleMediaPutCancel(const nlohmann::json& params) const;
    nlohmann::json handlePlaybackPlay(const nlohmann::json& params) const;
    nlohmann::json handlePlaybackPause() const;
    nlohmann::json handlePlaybackStop() const;
    nlohmann::json handlePlaybackSeek(const nlohmann::json& params) const;
    nlohmann::json handleSceneSet(const nlohmann::json& params) const;
    nlohmann::json handleSceneToggle(const nlohmann::json& params) const;
    nlohmann::json handleSceneList() const;
    nlohmann::json handleSceneClear() const;
    nlohmann::json handleLayerSet(const nlohmann::json& params) const;
    nlohmann::json handleLayerClear(const nlohmann::json& params) const;
    nlohmann::json handleDisplayAdd(const nlohmann::json& params) const;
    nlohmann::json handleDisplayList() const;
    nlohmann::json handleDisplayRemove(const nlohmann::json& params) const;
    nlohmann::json handleDisplaySet(const nlohmann::json& params) const;
    nlohmann::json handleDisplayWarpSet(const nlohmann::json& params) const;

    // Parses a show document, loads and remaps its media, makes it active
    // and applies its display wall (see applyShowDisplays). Shared by
    // show.load (after the file read) and show.loadJson.
    // `context` is only used in error messages ("show file" / "show JSON").
    // `preserveState` keeps the live playback state across the load (see
    // PlaybackEngine::loadShow). `mediaBase` is the directory that relative
    // media paths in the document resolve against - the show file's
    // directory for show.load, the `mediaDir` parameter for show.loadJson;
    // empty leaves the paths as written.
    nlohmann::json applyShowDocument(const nlohmann::json& parsed, const char* context,
                                     bool preserveState,
                                     const std::filesystem::path& mediaBase) const;

    // Applies the show's display definitions (when the document has a
    // `displays` section) to the display manager: the show file is where
    // display settings persist, so loading a show makes the system's wall
    // follow the document. Identity is the display NAME - document entries
    // the engine does not have are added (through the DisplayAddedFunction
    // callback, so the app creates the window/swapchain surface for them),
    // entries present on both sides are updated to the document's values,
    // and engine displays the document no longer defines are removed
    // (through the DisplayRemovedFunction callback). A document without a
    // `displays` section leaves the display set untouched (the operator's
    // live wall - including moves made after the push - stands).
    void applyShowDisplays(const Show& show) const;

    // Parameter extraction helpers (throw protocol::ParameterError).
    static std::string requireString(const nlohmann::json& params, const char* key);
    static double requireNumber(const nlohmann::json& params, const char* key);
    static int requireInt(const nlohmann::json& params, const char* key);
    static std::optional<std::string> optionalString(const nlohmann::json& params, const char* key);
    static std::optional<int> optionalInt(const nlohmann::json& params, const char* key);
    static bool optionalBool(const nlohmann::json& params, const char* key);

    // The media-cache root for the media.* cache commands (see
    // CacheRootProviderFunction); throws a clean error when the system has
    // no cache configured.
    std::string requireCacheRoot() const;

    // Loads `path` into the media library, falling back to this system's
    // media cache when the original path does not load (the file may only
    // exist on the control machine; the cache holds the copy the control
    // interface transferred, see media.put). `cacheKeyPath` is the media
    // path as written (the cache key is computed from it; see
    // media::cacheKeyFor). Returns the library id, or kInvalidId with
    // `error` (the original path's failure) when neither location holds a
    // readable file.
    Id loadMediaWithCacheFallback(const std::string& path, const std::string& cacheKeyPath,
                                  std::string& error) const;

    // The media cache path where a transfer of `cacheKeyPath` would land on
    // this system (<root>/mediamax-cache/<key>, see media.put): the empty
    // string when no cache disk is configured. No existence check - a
    // pending-media record needs the path to watch, not the file's current
    // state.
    std::string cachePathForMedia(const std::string& cacheKeyPath) const;

    PlaybackEngine& m_Engine;
    MediaLibrary& m_Media;
    DisplayManager& m_Displays;
    mutable std::string m_SystemName; // mutable: system.set mutates it under lock
    mutable std::string m_CacheDisk;  // mutable: system.setCacheDisk mutates it under lock
    std::string m_HostName;           // machine computer name (status "hostname")
    std::string m_Ip;                 // machine primary IPv4 (status "ip")
    std::atomic<std::uint64_t> m_DroppedVideoFrames{0}; // status "droppedVideoFrames"
    MediaLoadFunction m_LoadMedia;
    DisplayAddedFunction m_DisplayAdded;
    DisplayRemovedFunction m_DisplayRemoved;
    ThumbnailFunction m_Thumbnail;
    // Render device state (see setRenderDeviceInfo / system.setRender
    // Device). Guarded by its own mutex: the app writes it from the frame
    // loop (switch progress), the command threads read it for status.
    mutable std::mutex m_RenderDeviceMutex;
    std::string m_RenderDeviceCurrent;
    std::string m_RenderDeviceType;
    std::vector<RenderDeviceInfo> m_RenderDeviceAvailable;
    // mutable: system.setRenderDevice (a const handler) flips it under the
    // lock, like m_SystemName under system.set.
    mutable bool m_RenderDeviceSwitching = false;
    RenderDeviceSwitchFunction m_RenderDeviceSwitchFn;
    DisksFunction m_DisksFn;
    CacheDiskRequestFunction m_CacheDiskRequestFn;
    CacheRootProviderFunction m_CacheRootProvider;
    // Cache-copy progress (status "cacheCopy"): the app's copy worker
    // writes it from its own thread, the command threads read it for
    // status - hence its own lock, like m_RenderDeviceMutex.
    mutable std::mutex m_CacheCopyMutex;
    bool m_CacheCopyActive = false;
    std::uint64_t m_CacheCopyBytes = 0;
    std::uint64_t m_CacheCopyTotal = 0;
    // Media-transfer session (media.put, then the media.putChunk stream):
    // the chunks accumulate in strict order into a temp file that sits
    // next to the final cache path; the final chunk verifies the running
    // SHA-256 and renames the temp file over the final path (atomic on
    // the same volume). Guarded by m_TransferMutex - the transfer state's
    // own lock, like the cache-copy progress above (the command handlers
    // also run under the server's command mutex; the explicit lock keeps
    // the ownership clear).
    struct TransferSession
    {
        std::string path; // the requested cache-relative path
        std::filesystem::path finalFile; // the cache file the transfer lands on
        std::filesystem::path tempFile; // <finalFile> + ".put-<token>.tmp"
        std::uint64_t expectedBytes = 0;
        std::string expectedChecksum; // sha256 hex, lowercase
        std::uint64_t received = 0;
        util::Sha256 hash;
        std::unique_ptr<std::ofstream> tempStream;
    };
    mutable std::mutex m_TransferMutex;
    mutable std::map<std::uint32_t, TransferSession> m_Transfers;
    mutable std::uint32_t m_NextTransferToken = 1;
    // Media pending from the last show load (see PendingMedia): written by
    // the show-load handlers, read by the app's frame-loop poll - hence its
    // own lock, like the transfer sessions above. The pending ids are
    // allocated from kPendingMediaIdBase upward (never reused), so a stale
    // entry can match neither a file-local show id nor a library id.
    mutable std::mutex m_PendingMutex;
    mutable std::vector<PendingMedia> m_PendingMedia;
    mutable std::atomic<std::uint32_t> m_NextPendingMediaId{kPendingMediaIdBase};

    // Log ring buffer (log.tail): serious engine messages the control
    // surface reports. Guarded by m_LogMutex: any thread can log through
    // pushLog (the logger calls it from wherever the message is produced),
    // the command threads read it for log.tail.
    struct LogEntry
    {
        std::uint64_t seq = 0; // monotonically increasing, per server
        LogLevel level = LogLevel::Info; // Warning or above (see pushLog)
        double time = 0.0; // epoch seconds when the message was produced
        std::string message;
    };
    mutable std::mutex m_LogMutex;
    std::deque<LogEntry> m_Log;
    std::uint64_t m_LogNextSeq = 1;
    // This engine run's identity (epoch seconds at construction): a client
    // comparing it across log.tail calls detects an engine restart and
    // resets its sequence cursor (a fresh run's ring starts at seq 1).
    const double m_LogStarted;
};

// Thrown by parameter helpers; caught by handle() and turned into a failure
// response with a descriptive message.
class ParameterError : public std::exception
{
public:
    explicit ParameterError(std::string message) : m_Message(std::move(message)) {}
    const char* what() const noexcept override { return m_Message.c_str(); }

private:
    std::string m_Message;
};

} // namespace mediamax::protocol
