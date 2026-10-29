#pragma once

// The control GUI: application state + per-frame rendering (Dear ImGui).
//
// Layout (dockspace): Systems (top-left) + Displays/Media tabs
// (bottom-left), a center column with the 2D Show view (top) over the
// Playback transport/scenes (bottom), Show Editor (right) and Console
// (bottom). Every protocol method is reachable from the UI:
// transport, scene set/clear, displays + warp grid, media load/unload,
// system name, show authoring (saved or pushed live via show.loadJson) and
// a raw command console as the catch-all. The Show view is a live 2D render
// of the active system's frame (frame.state) with a free camera; the
// Displays window lists the active system's displays and media in tabs.

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "gui/ShowEditor.h"
#include "gui/ShowView.h"
#include "gui/SystemConnection.h"

namespace mediamax::gui
{

class ControlGuiApp
{
public:
    // The system list is not loaded at startup (issue 18: it lives in the
    // show file) - File > Open brings it in with the show.
    ControlGuiApp();
    ~ControlGuiApp();

    ControlGuiApp(const ControlGuiApp&) = delete;
    ControlGuiApp& operator=(const ControlGuiApp&) = delete;

    // Renders the whole UI for one frame (menu bar + dockspace + windows).
    // Call once per frame, between ImGui::NewFrame() and ImGui::Render().
    void frame();

    bool shutdownRequested() const { return m_ShutdownRequested; }

    // --- show view (2D render) -------------------------------------------------
    // GPU texture pool setup/teardown (the runtime calls them around the
    // Dear ImGui Vulkan back end's lifetime; see ShowView.h).
    void initShowView(VkDevice device, VkPhysicalDevice physicalDevice);
    void shutdownShowView();
    // Records the pool's pending thumbnail uploads into the frame's command
    // buffer (the runtime calls this after beginFrame, before the UI draw).
    void flushShowTextureUploads(VkCommandBuffer cmd);

    // Self-test (see GuiMain): drives the same command path the UI uses
    // against a running playback engine; returns true when every step passed
    // and fills `report` with one PASS/FAIL line per step.
    bool selfTestSystem(const std::string& host, std::uint16_t port, std::string& report);

    // --- test hooks (driven by --gui-self-test; no effect in normal use) ----
    // Queues the given modal ("open", "saveas", "media", "pull", "about",
    // "edit", "warp", "mediadetails" or "syssettings") to be opened by the
    // next
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
    // Simulates an "Add cue" button press (the same code path the UI uses)
    // and returns the timeline's cue count afterwards. The self-test presses
    // it twice on the fresh editor: each press must add a cue (before the
    // fix the second press replaced the first, since the new cue's default
    // time equaled the last cue's and Show::addCue replaces on collision).
    std::size_t testAddCue();
    // Registers a system in the GUI list (the self-test's entries; nothing
    // is persisted - the show file is where the list is stored, issue 18).
    void testAddSystem(const std::string& name, const std::string& host, int port);
    // Loads a show file through the exact File > Open path (loadShowFile):
    // the self-test writes a small show document, loads it, and verifies
    // the engine received the loaded show (the load pushes it to every
    // connected system immediately).
    bool testLoadShowFile(const std::string& path);
    // True once a registered system reports its poll connection up.
    bool testSystemConnected(const std::string& name) const;
    // Gives the fresh self-test editor a deterministic scene (id 1) with one
    // color layer (id 10) and marks it edited: the live self-test's
    // auto-push then delivers the show the live checks inspect. When the
    // self-test BMP exists (see selfTestSystem), the scene also carries a
    // media layer over it: the frame phases then render the show view's
    // media path (thumbnail fetch + texture upload + textured quad).
    void testGiveEditorALayer();
    // Removes the self-test BMP from disk (the live check is over; the
    // engine already loaded it from the path the pushed show carries).
    // No-op in the headless run (no BMP was created).
    void testRemoveSelfTestBmp();
    // Sets the Display settings dialog's System dropdown (index into
    // m_Systems; the same state the combo sets) - the live self-test's
    // display-move check (issue 14).
    void testSelectDisplayEditSystem(int index);
    // Presses the Display settings Apply button: the exact action the UI
    // runs (display.set, or a move to the selected system - issue 14).
    bool testApplyDisplayEdit();
    // True when the active system's snapshot reports at least one display -
    // the same precondition the UI's Edit... button has (its row only
    // exists then). The live self-test waits for it before opening the
    // dialog (issue 14): a freshly connected system's first status poll
    // lands ~500 ms after connect and can still be in flight.
    bool testActiveHasDisplay() const;
    // Sets the Display settings dialog's name field (the same state the
    // dialog's Name input edits) - the live self-test's rename checks
    // (issue 15).
    void testSetDisplayEditName(const std::string& name);
    // True when the active system's snapshot reports a display with the
    // given id - the live self-test waits for the moved display to show
    // up before reopening the dialog (the snapshot polls every 500 ms, so
    // right after a move it can still list the removed record).
    bool testActiveSnapshotHasDisplay(Id displayId) const;
    // System settings dialog hooks (live self-test, issue 16): set the
    // dialog's Name / Port fields (the same state the inputs edit) and
    // press its Apply button.
    void testSetSystemSettingsName(const std::string& name);
    void testSetSystemSettingsPort(int port);
    bool testApplySystemSettings();
    // The system entry's name at `index` (empty when out of range).
    std::string testSystemNameAt(int index) const;
    // The machine name the engine at `index` reports (status "hostname";
    // empty when the snapshot has no status yet).
    std::string testSystemHostname(int index) const;
    // The Display settings dialog's current error line (empty when healthy);
    // the live self-test's move check reports it on failure.
    std::string testGetDisplayEditError() const { return m_EditError; }
    // One-line description of the Display settings dialog's state for the
    // live self-test: the target system's index (-1 when none), the display
    // id and the System dropdown's selection.
    std::string testDescribeDisplayEdit() const
    {
        int targetIdx = -1;
        for (int i = 0; i < static_cast<int>(m_Systems.size()); ++i)
        {
            if (m_Systems[static_cast<size_t>(i)].get() == m_EditTarget)
            {
                targetIdx = i;
                break;
            }
        }
        return "target=" + std::to_string(targetIdx) + " id=" + std::to_string(m_EditDisplayId) +
               " editSystem=" + std::to_string(m_EditSystem) + " active=" + std::to_string(m_Active);
    }

private:
    // --- systems -----------------------------------------------------------

    SystemConnection* activeConnection() const;
    // The show file is the system list's persistence target (issue 18):
    // serialize the current list into the editor document (called before
    // every save and push).
    void syncSystemsToShow();
    // The show file is the system list's source of truth (issue 18): after
    // opening a show, make its system list the GUI's. Entries with the same
    // name+host+port reuse the existing connection (stays online); the rest
    // are removed and new ones connect.
    void applyShowSystems();
    // Saves the editor's show to `path`, syncing the system list first.
    bool saveEditorFile(const std::string& path, std::string& error);

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
    // The Displays window (left column, bottom): the active system's
    // displays and media library in tabs. Relinking a display to another
    // system is done from its Display settings dialog's System dropdown
    // (issue 14).
    void renderDisplaysWindow();
    void renderPlaybackWindow();
    void renderTransportBlock(const SystemConnection::Snapshot& snap);
    void renderScenesBlock(const SystemConnection::Snapshot& snap);
    void renderDisplaysBlock(const SystemConnection::Snapshot& snap);
    // The 2D show view (center, top): the active system's current frame as
    // a free-camera 2D picture - the display wall, the visible layers (color
    // quads, media with thumbnails, video badges) and the "Reset view"
    // overlay. Draws on the scene child's draw list (see ShowView.h).
    void renderShowViewWindow();
    // Fits the show view camera to the whole display wall (union of all
    // displays, ~10% margin; an unknown wall shows a 1920x1080 box),
    // centered in the scene child (childSize in screen px).
    void fitShowViewCamera(const ImVec2& childSize, const std::string& signature);
    // Opens the "Display settings" / "Projection warp" dialog for the given
    // display record. The settings dialog starts on `target` (the system the
    // record came from) and can relink the display to another system via its
    // System dropdown (Apply then moves it - issue 14); the warp dialog
    // sends display.warp.set to `target`.
    void openDisplayEdit(const nlohmann::json& display, SystemConnection* target);
    // The Display settings Apply button: display.set on the selected system,
    // or - when that is not the display's current system - a move there
    // (display.add + display.set on the target, display.remove on the
    // source; issue 14). Closes the dialog on success.
    bool applyDisplayEdit();
    // Opens the "System settings" dialog for the system at `index`
    // (issue 16).
    void openSystemSettings(int index);
    // The System settings Apply button: re-targets the entry (name /
    // host / port) - the connection is restarted when it was running.
    // Closes the dialog on success; marks the systems list dirty.
    bool applySystemSettings();
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
    // The "Add cue" button's action: appends a new timeline cue with
    // defaults (PlayScene for the first scene, name "cue", one second after
    // the last cue so it never collides with an existing time -
    // Show::addCue replaces a cue at an existing time).
    void addCueDefault();
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
    // Pushes the current document to the given systems: one show.loadJson
    // (preserveState) per system, per-target log lines. Returns true when
    // every target accepted it (or the list is empty); a partial failure
    // sets m_EditorError and returns false.
    bool pushShowToSystems(std::span<SystemConnection*> targets, const char* reason);
    // Pushes the current document to EVERY connected system.
    bool pushShowToAllConnected(const char* reason);
    // Loads a show file exactly as File > Open does: loads the document,
    // takes the system list from it, then immediately pushes the loaded
    // show to every connected system (a load is a statement of what the
    // systems should be playing, so it is not left to the next edit's
    // auto-push). On failure, fills `error`.
    bool loadShowFile(const std::string& path, std::string& error);
    // Debounced auto-push of edited shows to every connected system
    // (show.loadJson with preserveState, so live edits do not restart
    // playback). Called from frame().
    void autoPushEditedShow();

    // --- state ---------------------------------------------------------------

    std::vector<std::unique_ptr<SystemConnection>> m_Systems;
    int m_Active = -1; // selected system index

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
    // "System settings" dialog (settings... in the Systems pane, issue 16):
    // the entry's identity and address (name / host / port - the same
    // fields the show file's `systems` list stores). m_SysSettingsIndex is
    // the entry's slot in m_Systems; retargeting keeps the slot, so
    // selections and stored pointers stay valid.
    bool m_SysSettingsModal = false;
    int m_SysSettingsIndex = -1;
    struct SystemSettingsEdit
    {
        std::string name;
        std::string host;
        int port = 47600;
        std::string error;
    };
    SystemSettingsEdit m_SysSettings;
    // "Display settings" dialog (Edit... in the Displays block): per-display
    // x/y/w/h/brightness/enabled. m_EditTarget is the system the dialog was
    // opened from; m_EditSystem is the System dropdown's selection (index
    // into m_Systems) - Apply to a different system moves the display there
    // (issue 14). The dialog is a modal, so neither can be removed while it
    // is open.
    bool m_EditModal = false;
    SystemConnection* m_EditTarget = nullptr;
    Id m_EditDisplayId = kInvalidId;
    int m_EditSystem = -1; // index into m_Systems (the System dropdown)
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

    // "Display settings" dialog (Edit... in the Displays block). The working
    // values live in m_DisplayEdits, resynced from the snapshot every time
    // the dialog opens; Apply sends display.set to the selected system (or
    // moves the display there - issue 14).
    struct DisplayEdit
    {
        int x = 0;
        int y = 0;
        int width = 1920;
        int height = 1080;
        float brightness = 1.0f;
        bool enabled = true; // round-tripped by Apply (the toggle itself lives
                             // in the Displays block, not the dialog)
        bool alwaysOnTop = true;
        bool vsync = true;
        std::string name; // carried through a move (display.add takes a name)
        std::string monitor; // physical output (read-only in the dialog)
    };
    std::map<Id, DisplayEdit> m_DisplayEdits;

    // Warp editor state (Warp... in the Displays block), the last grid,
    // points and interpolation per (system, display). Seeded from the
    // engine's current warp (the display JSON's `warp` object) when the
    // snapshot reports it, so the dialog opens on what is actually applied.
    struct WarpEdit
    {
        int n = 2;
        std::vector<std::array<float, 2>> points; // absolute positions, row-major
        bool spline = false;
        bool valid = false; // grid filled in
    };
    std::map<const SystemConnection*, std::map<Id, WarpEdit>> m_WarpEdits;
    // Node-based warp pad: the point index currently dragged with the mouse
    // (-1 when none). Transient - reset when the dialog opens or closes.
    int m_WarpDragNode = -1;

    // Show editor selection (scenes tab).
    int m_SelectedSceneIdx = 0;
    int m_LastActive = -1; // to reset transient playback state on switch

    // Show view (2D render) state. m_ShowScene is rebuilt every frame from
    // the active system's snapshot; the camera is in output-space pixels
    // (y down): screen = childOrigin + world * scale + offset. fitted is
    // false until the first fit (or a system switch); m_ShowFittedSignature
    // is the display signature the camera was last fitted to (a changed
    // wall auto-refits).
    struct ShowViewCamera
    {
        float scale = 0.f;
        float offsetX = 0.f;
        float offsetY = 0.f;
        bool fitted = false;
    };
    ShowViewCamera m_ShowCam;
    std::string m_ShowFittedSignature;
    ShowViewScene m_ShowScene;
    // The scene child's rect (screen px; zero when the window is hidden) -
    // the camera math's origin and the drawing bounds.
    ImVec2 m_ShowViewRectMin{0.f, 0.f};
    ImVec2 m_ShowViewRectMax{0.f, 0.f};
    bool m_ShowViewVisible = false; // the window was Begun this frame
    ShowTexturePool m_ShowTexPool;

    // The live self-test's scratch BMP (written by selfTestSystem, removed
    // by testRemoveSelfTestBmp after the frame phases).
    std::string m_SelfTestBmpPath;

    // Media tab probe result (last probe, shown under the list).
    std::string m_MediaProbeResult;

    bool m_ShutdownRequested = false;
    bool m_FirstFrame = true;
};

} // namespace mediamax::gui
