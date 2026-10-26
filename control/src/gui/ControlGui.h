#pragma once

// The control GUI: application state + per-frame rendering (Dear ImGui).
//
// Layout (dockspace): Systems (left), Playback (center), Show Editor (right)
// and Console (bottom). Every protocol method is reachable from the UI:
// transport, scene set/clear, live layer overrides, displays + warp grid,
// media load/unload, system name, show authoring (saved or pushed live via
// show.loadJson) and a raw command console as the catch-all.

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gui/ShowEditor.h"
#include "gui/SystemConnection.h"

namespace mediamax::gui
{

class ControlGuiApp
{
public:
    explicit ControlGuiApp(std::string systemsPath);
    ~ControlGuiApp();

    ControlGuiApp(const ControlGuiApp&) = delete;
    ControlGuiApp& operator=(const ControlGuiApp&) = delete;

    // Renders the whole UI for one frame (menu bar + dockspace + windows).
    // Call once per frame, between ImGui::NewFrame() and ImGui::Render().
    void frame();

    bool shutdownRequested() const { return m_ShutdownRequested; }

    // Self-test (see GuiMain): drives the same command path the UI uses
    // against a running playback engine; returns true when every step passed
    // and fills `report` with one PASS/FAIL line per step.
    bool selfTestSystem(const std::string& host, std::uint16_t port, std::string& report);

private:
    // --- systems -----------------------------------------------------------

    SystemConnection* activeConnection() const;
    void loadSystemsConfig();
    bool saveSystemsConfig(std::string& error);

    // --- UI sections ---------------------------------------------------------

    void renderMenuBar();
    void renderDockspace();
    // Walks the dock node tree before DockSpace() and records any drift
    // (foreign/stale HostWindow, broken parent links, host draw list that
    // lost its docking channels) to the console + crash log. See
    // checkDockInvariants() in ControlGui.cpp.
    void checkDockInvariants(std::uint32_t dockspaceId);
    void renderSystemsWindow();
    // Registers and auto-connects a new system (the "Add system" form).
    void addSystem(const std::string& name, const std::string& host, int port);
    // One-shot ping against a not-yet-registered address (the "Test" button);
    // blocking (<= ~2 s), run from the UI thread on explicit user action.
    void testSystemAddress(const std::string& host, int port);
    void renderPlaybackWindow();
    void renderTransportBlock(const SystemConnection::Snapshot& snap);
    void renderScenesBlock(const SystemConnection::Snapshot& snap);
    void renderLayersBlock(const SystemConnection::Snapshot& snap);
    void renderDisplaysBlock(const SystemConnection::Snapshot& snap);
    void renderMediaBlock(const SystemConnection::Snapshot& snap);
    void renderShowEditorWindow();
    void renderShowTab();
    void renderMediaTab();
    void renderScenesTab();
    void renderCuesTab();
    void renderLayerDetails(Layer& layer);
    void renderLayerTracks(Layer& layer);
    void renderCueAction(Cue& cue);
    void renderConsoleWindow();
    void renderModals();
    void refreshOpenFiles();

    // --- helpers -------------------------------------------------------------

    void log(const std::string& line);
    // Sends a command to the active system; logs the exchange. When
    // `outResult` is non-null the result JSON is also returned.
    bool sendActive(const std::string& method, const nlohmann::json& params, std::string& error,
                    nlohmann::json* outResult = nullptr);
    void consoleSubmit();
    void pushToActiveSystem();

    // --- state ---------------------------------------------------------------

    std::string m_SystemsPath;
    std::vector<std::unique_ptr<SystemConnection>> m_Systems;
    int m_Active = -1; // selected system index
    bool m_ConfigDirty = false;

    // "Add system" form.
    std::string m_NewName;
    std::string m_NewHost = "127.0.0.1";
    int m_NewPort = 47600;
    std::string m_SysTestResult; // last "Test" outcome, shown under the form
    bool m_SysTestOk = false;

    ShowEditor m_Editor;
    std::string m_EditorError;

    // Modals.
    bool m_OpenModal = false;
    std::string m_OpenDir = ".";
    std::vector<std::string> m_OpenFiles;
    int m_OpenSelection = -1;
    bool m_SaveAsModal = false;
    std::string m_SaveAsPath;
    bool m_PullModal = false;
    std::string m_PullPath;
    bool m_AboutModal = false;

    // Console.
    std::string m_ConsoleInput;
    std::vector<std::string> m_ConsoleLog;
    static constexpr size_t kMaxLogLines = 500;

    // Playback edit state (for the active system).
    double m_Seek = 0.0;
    bool m_SeekHeld = false; // the seek slider is being dragged
    double m_SeekInput = 0.0; // for shows without a duration
    std::string m_SystemNameEdit;
    bool m_HaveNameEdit = false;
    std::string m_MediaPathInput;

    // Last values the UI sent per layer (live overrides are not reported back
    // in status, so the widgets keep their own state).
    struct LayerLive
    {
        float opacity = 1.0f;
        bool visible = true;
        int blend = 0; // index into kBlendModes
        float x = 0.0f;
        float y = 0.0f;
    };
    std::map<Id, LayerLive> m_LayerLive;

    struct DisplayEdit
    {
        int x = 0;
        int y = 0;
        int width = 1920;
        int height = 1080;
        float brightness = 1.0f;
        bool enabled = true;
        bool valid = false; // filled from a snapshot
    };
    std::map<Id, DisplayEdit> m_DisplayEdits;

    struct WarpEdit
    {
        int n = 2;
        std::vector<std::array<float, 2>> points;
        bool valid = false; // filled from a snapshot
    };
    std::map<Id, WarpEdit> m_WarpEdits;

    // Show editor selection (scenes tab).
    int m_SelectedSceneIdx = 0;
    int m_LastActive = -1; // to reset transient playback state on switch

    // Media tab probe result (last probe, shown under the list).
    std::string m_MediaProbeResult;

    bool m_ShutdownRequested = false;
    bool m_FirstFrame = true;
};

} // namespace mediamax::gui
