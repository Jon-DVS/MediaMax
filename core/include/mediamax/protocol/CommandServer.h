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
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "mediamax/display/DisplayManager.h"
#include "mediamax/media/MediaLibrary.h"
#include "mediamax/playback/PlaybackEngine.h"
#include "mediamax/protocol/Protocol.h"
#include "mediamax/util/Logger.h"

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
    nlohmann::json handleLogTail(const nlohmann::json& params) const;
    nlohmann::json handleShowLoad(const nlohmann::json& params) const;
    nlohmann::json handleShowLoadJson(const nlohmann::json& params) const;
    nlohmann::json handleShowSave(const nlohmann::json& params) const;
    nlohmann::json handleShowInfo() const;
    nlohmann::json handleMediaLoad(const nlohmann::json& params) const;
    nlohmann::json handleMediaList() const;
    nlohmann::json handleMediaUnload(const nlohmann::json& params) const;
    nlohmann::json handleMediaThumbnail(const nlohmann::json& params) const;
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

    PlaybackEngine& m_Engine;
    MediaLibrary& m_Media;
    DisplayManager& m_Displays;
    mutable std::string m_SystemName; // mutable: system.set mutates it under lock
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
