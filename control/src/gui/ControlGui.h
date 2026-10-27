#pragma once

// The control GUI: application state + per-frame rendering (Dear ImGui).
//
// Layout (dockspace): Systems + Displays (left), Playback (center), Show
// Editor (right) and Console (bottom). Every protocol method is reachable
// from the UI:
// transport, scene set/clear, live layer overrides, displays + warp grid,
// media load/unload, system name, show authoring (saved or pushed live via
// show.loadJson) and a raw command console as the catch-all. The Displays
// window lists every display of every online system (with its owning
// system) and can move a display to another system.

#include <array>
#include <chrono>
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

    // --- test hooks (driven by --gui-self-test; no effect in normal use) ----
    // Queues the given modal ("open", "saveas", "media", "pull", "about",
    // "move", "edit", "warp" or "mediadetails") to be opened by the next
    // frame(): the same state the UI buttons set plus OpenPopup() (or, for
    // "open"/"saveas"/"media", the ImGuiFileDialog OpenDialog). Deferred
    // because OpenPopup() must run inside a frame (it dereferences the
    // current ImGui window, which is NULL between frames). Returns false
    // for an unknown name.
    bool testOpenModal(const std::string& which);
    // True while the given modal is open: in Dear ImGui's popup stack, or
    // (for "open"/"saveas"/"media") in the ImGuiFileDialog instance.
    bool testModalOpen(const std::string& which) const;
    // Closes the given modal: clears its app-side flag (which makes
    // BeginPopupModal() tear the popup down on the next frame), or calls
    // ImGuiFileDialog::Close() for the file dialogs.
    bool testCloseModal(const std::string& which);
    // Renames the editor's show and marks it edited: gives the debounced
    // auto-push (see autoPushEditedShow) something to push during the frame
    // phase of the self-test, which then verifies the system received it.
    void testTouchEditor(const std::string& name);

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
    // Cross-system display list: every display of every online system with
    // its owning system, plus "Move..." (move a display to another system).
    void renderDisplaysWindow();
    void renderPlaybackWindow();
    void renderTransportBlock(const SystemConnection::Snapshot& snap);
    void renderScenesBlock(const SystemConnection::Snapshot& snap);
    void renderLayersBlock(const SystemConnection::Snapshot& snap);
    void renderDisplaysBlock(const SystemConnection::Snapshot& snap);
    // Opens the "Display settings" / "Projection warp" dialog for the given
    // display record, sending display.set / display.warp.set to `target`
    // (the system the record came from).
    void openDisplayEdit(const nlohmann::json& display, SystemConnection* target);
    void openWarpEdit(const nlohmann::json& display, SystemConnection* target);
    // Opens the "Media details" dialog for the given media record (the
    // system it came from is shown in the dialog title; the details are
    // re-read from that system's snapshot every frame, so an unloaded
    // entry degrades gracefully).
    void openMediaDetails(const nlohmann::json& media, SystemConnection* target);
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
    // --- Open/Save As file dialogs (ImGuiFileDialog) -------------------------
    // The two dialogs are ordinary ImGui windows owned by the vendored
    // ImGuiFileDialog instance (not BeginPopupModal popups), so they are
    // opened here and rendered top-level in renderFileDialogs() once per
    // frame (Display() is a no-op while the key is not open).
    void openFileDialog();
    void saveAsFileDialog();
    // Media Browse button (Show Editor > Media): multi-select file dialog
    // that appends one MediaReference per selected file.
    void addMediaFileDialog();
    void renderFileDialogs();
    // Maps a test-hook name to its ImGuiFileDialog key; null for the
    // popups (use modalPopupName() for those).
    static const char* fileDialogKey(const std::string& which);
    // Directory to start a file dialog in: the folder of the currently
    // loaded show, else the current working directory.
    std::string showDir() const;
    // Performs a modal open (same state the buttons set + OpenPopup). Must
    // run inside frame() while the modal host window is current.
    void openModal(const std::string& which);

    // --- helpers -------------------------------------------------------------

    void log(const std::string& line);
    // Sends a command to the active system; logs the exchange. When
    // `outResult` is non-null the result JSON is also returned.
    bool sendActive(const std::string& method, const nlohmann::json& params, std::string& error,
                    nlohmann::json* outResult = nullptr);
    void consoleSubmit();
    // Builds the show.loadJson params for pushing the editor's document to a
    // system: the document plus the directory of the file it was opened
    // from (mediaDir), so relative media paths resolve the same way a
    // show.load of that file would. An unsaved document carries no mediaDir.
    nlohmann::json showPushParams();
    void pushToActiveSystem();
    // Debounced auto-push of edited shows to every connected system
    // (show.loadJson with preserveState, so live edits do not restart
    // playback). Called from frame().
    void autoPushEditedShow();

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
    // Auto-push of edited shows to connected systems (default on): one
    // show.loadJson (preserveState) per connected system, at most once per
    // second per edit generation, kAutoPushDelay after the last edit.
    bool m_AutoPushEdits = true;
    std::uint64_t m_AutoPushedGeneration = 0; // last generation fully pushed
    std::chrono::steady_clock::time_point m_NextAutoPushAttempt{}; // retry throttle

    // Modals. (Open / Save As are ImGuiFileDialog windows, not popups - see
    // openFileDialog()/saveAsFileDialog(); no app-side open flags needed.)
    bool m_PullModal = false;
    std::string m_PullPath;
    bool m_AboutModal = false;
    // "Move display to system..." modal (the Displays window). The source
    // display's record is copied from the clicked row; the move sends
    // display.add to the target system and display.remove to the source.
    bool m_MoveModal = false;
    int m_MoveSourceSystem = -1; // index into m_Systems
    Id m_MoveDisplayId = kInvalidId;
    std::string m_MoveName;
    std::int32_t m_MoveX = 0, m_MoveY = 0, m_MoveWidth = 0, m_MoveHeight = 0;
    int m_MoveTarget = -1; // index into m_Systems
    std::string m_MoveError;
    // "Display settings" dialog (Edit... in the Displays block): per-display
    // x/y/w/h/brightness/enabled. m_EditTarget is the system the dialog was
    // opened from (modal, so it cannot be removed while open).
    bool m_EditModal = false;
    SystemConnection* m_EditTarget = nullptr;
    Id m_EditDisplayId = kInvalidId;
    std::string m_EditError;
    // "Projection warp" dialog (Warp... in the Displays block): the warp
    // grid editor, moved out of the Displays block into its own dialog.
    bool m_WarpModal = false;
    SystemConnection* m_WarpTarget = nullptr;
    Id m_WarpDisplayId = kInvalidId;
    std::string m_WarpError;
    // "Media details" dialog (Details... in the Media block): read-only
    // probe information for one media entry (codec, colorspace, bit depth,
    // scan type, bit rate). The entry is re-read from the target system's
    // snapshot while the dialog is open.
    bool m_MediaDetailsModal = false;
    SystemConnection* m_MediaDetailsTarget = nullptr;
    Id m_MediaDetailsId = kInvalidId;
    // Test hook: modal queued by testOpenModal(), opened by the next frame().
    std::string m_PendingTestOpenModal;

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

    // Per-layer widget state. Opacity/visible/blend keep the last values the
    // UI sent (the engine reports no effective values for them). x/y are
    // resynced from scene.list (it reports the active scene's effective
    // positions), except while a field is being edited or while a just-sent
    // value is still catching up in the polled snapshot.
    struct LayerLive
    {
        float opacity = 1.0f;
        bool visible = true;
        int blend = 0; // index into kBlendModes
        float x = 0.0f;
        float y = 0.0f;
    };
    std::map<Id, LayerLive> m_LayerLive;
    // When the UI last sent a layer's position (or cleared it: erased). The
    // polled snapshot lags the send, so the engine's reported position is
    // not trusted as a resync source until it has had time to arrive.
    std::map<Id, std::chrono::steady_clock::time_point> m_LayerPosSentAt;

    // "Display settings" dialog (Edit... in the Displays block). The working
    // values live in m_DisplayEdits, resynced from the snapshot every time
    // the dialog opens; Apply sends display.set to m_EditTarget.
    struct DisplayEdit
    {
        int x = 0;
        int y = 0;
        int width = 1920;
        int height = 1080;
        float brightness = 1.0f;
        bool enabled = true;
    };
    std::map<Id, DisplayEdit> m_DisplayEdits;

    // Warp editor state (Warp... in the Displays block), the last grid and
    // points per (system, display). The warp is not reported in status, so
    // the dialog starts from the last locally used values.
    struct WarpEdit
    {
        int n = 2;
        std::vector<std::array<float, 2>> points;
        bool valid = false; // identity grid filled in
    };
    std::map<const SystemConnection*, std::map<Id, WarpEdit>> m_WarpEdits;

    // Show editor selection (scenes tab).
    int m_SelectedSceneIdx = 0;
    int m_LastActive = -1; // to reset transient playback state on switch

    // Media tab probe result (last probe, shown under the list).
    std::string m_MediaProbeResult;

    bool m_ShutdownRequested = false;
    bool m_FirstFrame = true;
};

} // namespace mediamax::gui
