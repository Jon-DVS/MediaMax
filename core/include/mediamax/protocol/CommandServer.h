#pragma once

// The command server: maps control-protocol commands onto the playback
// engine, media library and display manager.
//
// It is engine-agnostic in the same sense as PlaybackEngine: no GPU, no
// network - given a command it produces a response. This makes the entire
// control surface unit-testable (see core/tests/test_CommandServer.cpp), and
// lets the control interface validate commands before sending.

#include <exception>
#include <functional>
#include <string>
#include <vector>

#include "mediamax/display/DisplayManager.h"
#include "mediamax/media/MediaLibrary.h"
#include "mediamax/playback/PlaybackEngine.h"
#include "mediamax/protocol/Protocol.h"

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

class CommandServer
{
public:
    CommandServer(PlaybackEngine& engine, MediaLibrary& media, DisplayManager& displays,
                  std::string systemName)
        : m_Engine(engine)
        , m_Media(media)
        , m_Displays(displays)
        , m_SystemName(std::move(systemName))
    {
    }

    // Sets the media-loading callback (see MediaLoadFunction).
    void setMediaLoadFunction(MediaLoadFunction fn) { m_LoadMedia = std::move(fn); }

    // Sets the display add/remove callbacks (see above).
    void setDisplayAddedFunction(DisplayAddedFunction fn) { m_DisplayAdded = std::move(fn); }
    void setDisplayRemovedFunction(DisplayRemovedFunction fn) { m_DisplayRemoved = std::move(fn); }

    // Handles a decoded command. Never throws; all failures become responses
    // with ok=false.
    Response handle(const Command& cmd) const;

    // Static helpers used by both the command implementations and tests.
    static std::vector<std::string> supportedMethods();

private:
    // --- Individual command handlers (all return a result JSON) -------------

    nlohmann::json handlePing() const;
    nlohmann::json handleStatus() const;
    nlohmann::json handleSystemSet(const nlohmann::json& params) const;
    nlohmann::json handleShowLoad(const nlohmann::json& params) const;
    nlohmann::json handleShowLoadJson(const nlohmann::json& params) const;
    nlohmann::json handleShowSave(const nlohmann::json& params) const;
    nlohmann::json handleShowInfo() const;
    nlohmann::json handleMediaLoad(const nlohmann::json& params) const;
    nlohmann::json handleMediaList() const;
    nlohmann::json handleMediaUnload(const nlohmann::json& params) const;
    nlohmann::json handlePlaybackPlay(const nlohmann::json& params) const;
    nlohmann::json handlePlaybackPause() const;
    nlohmann::json handlePlaybackStop() const;
    nlohmann::json handlePlaybackSeek(const nlohmann::json& params) const;
    nlohmann::json handleSceneSet(const nlohmann::json& params) const;
    nlohmann::json handleSceneList() const;
    nlohmann::json handleSceneClear() const;
    nlohmann::json handleLayerSet(const nlohmann::json& params) const;
    nlohmann::json handleLayerClear(const nlohmann::json& params) const;
    nlohmann::json handleDisplayAdd(const nlohmann::json& params) const;
    nlohmann::json handleDisplayList() const;
    nlohmann::json handleDisplayRemove(const nlohmann::json& params) const;
    nlohmann::json handleDisplaySet(const nlohmann::json& params) const;
    nlohmann::json handleDisplayWarpSet(const nlohmann::json& params) const;

    // Parses a show document, loads and remaps its media, and makes it
    // active. Shared by show.load (after the file read) and show.loadJson.
    // `context` is only used in error messages ("show file" / "show JSON").
    // `preserveState` keeps the live playback state across the load (see
    // PlaybackEngine::loadShow).
    nlohmann::json applyShowDocument(const nlohmann::json& parsed, const char* context,
                                     bool preserveState) const;

    // Parameter extraction helpers (throw protocol::ParameterError).
    static std::string requireString(const nlohmann::json& params, const char* key);
    static double requireNumber(const nlohmann::json& params, const char* key);
    static int requireInt(const nlohmann::json& params, const char* key);
    static std::optional<std::string> optionalString(const nlohmann::json& params, const char* key);
    static bool optionalBool(const nlohmann::json& params, const char* key);

    PlaybackEngine& m_Engine;
    MediaLibrary& m_Media;
    DisplayManager& m_Displays;
    mutable std::string m_SystemName; // mutable: system.set mutates it under lock
    MediaLoadFunction m_LoadMedia;
    DisplayAddedFunction m_DisplayAdded;
    DisplayRemovedFunction m_DisplayRemoved;
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
