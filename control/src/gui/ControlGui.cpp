#include "gui/ControlGui.h"

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <utility>

#include <imgui.h>
// The one-shot initial dock layout (DockBuilder*) and
// ImGuiDockNodeFlags_DockSpace live in imgui_internal.h in this release.
#include <imgui_internal.h>
// std::string overloads for the text input widgets.
#include <imgui_stdlib.h>
// Open / Save As file dialogs (vendored, MIT - third_party/ImGuiFileDialog).
#include "ImGuiFileDialog.h"

#include "gui/GuiCrash.h"
#include "mediamax/net/ControlClient.h"
#include "mediamax/time/Timecode.h"
#include "mediamax/util/File.h"

namespace mediamax::gui
{

using json = nlohmann::json;

namespace
{

constexpr const char* kBlendModes[] = {"normal", "add", "subtract", "multiply", "screen"};
constexpr int kBlendModeCount = 5;

// A minimal 24-bit BMP (top half red, bottom half blue) for the live
// self-test's media.thumbnail check: it exercises the engine's real image
// decode path (the "mediamax:testpattern:" sources exist only in the
// engine's own --self-test mode).
bool writeThumbnailTestBmp(const std::string& path)
{
    constexpr int width = 320;
    constexpr int height = 180;
    const int rowPadded = ((width * 3) + 3) & ~3; // rows padded to 4 bytes
    const std::size_t dataSize = static_cast<std::size_t>(rowPadded) * height;
    std::string file;
    file.resize(54 + dataSize, '\0');
    auto putU32 = [&file](std::size_t at, std::uint32_t v) {
        std::memcpy(file.data() + at, &v, 4);
    };
    auto putU16 = [&file](std::size_t at, std::uint16_t v) {
        std::memcpy(file.data() + at, &v, 2);
    };
    file[0] = 'B';
    file[1] = 'M';
    putU32(2, static_cast<std::uint32_t>(file.size()));
    putU32(10, 54); // offset of the pixel data
    putU32(14, 40); // BITMAPINFOHEADER
    putU32(18, static_cast<std::uint32_t>(width));
    putU32(22, static_cast<std::uint32_t>(height)); // positive = bottom-up rows
    putU16(26, 1);                                  // planes
    putU16(28, 24);                                 // bits per pixel
    putU32(34, static_cast<std::uint32_t>(dataSize)); // BI_RGB (no compression)
    auto* pixels = file.data() + 54;
    for (int y = 0; y < height; ++y)
    {
        // Bottom-up storage: the first stored row is the bottom of the
        // image, so the red half ends up on top when displayed (stored
        // row y shows at display row height - 1 - y).
        const bool redHalf = (height - 1 - y) * 2 < height;
        auto* row = pixels + static_cast<std::size_t>(y) * rowPadded;
        for (int x = 0; x < width; ++x)
        {
            row[static_cast<std::size_t>(x) * 3 + 0] = redHalf ? 0 : 200;  // B
            row[static_cast<std::size_t>(x) * 3 + 1] = redHalf ? 0 : 0;    // G
            row[static_cast<std::size_t>(x) * 3 + 2] = redHalf ? 200 : 0;  // R
        }
    }
    return writeBinaryFile(path, file.data(), file.size());
}

constexpr const char* kInterpolations[] = {"linear", "step"};
constexpr int kInterpolationCount = 2;

constexpr size_t kLogLineLimit = 400; // max characters kept per console line
// The Playback tab extrapolates the timecode between 500 ms status polls;
// once the last good poll is older than this (4 missed polls) the poll is
// considered stale and the displayed clock is frozen instead of running on
// a dead connection.
constexpr double kStalePollSeconds = 2.0;

// The identity warp grid: n x n nodes on their ideal positions (the regular
// lattice from (0,0) to (1,1), row-major). Float division on purpose - the
// interior nodes sit at fractional positions.
std::vector<std::array<float, 2>> identityWarpPoints(int n)
{
    std::vector<std::array<float, 2>> pts;
    pts.reserve(static_cast<size_t>(n) * n);
    for (int row = 0; row < n; ++row)
    {
        for (int col = 0; col < n; ++col)
        {
            pts.push_back({static_cast<float>(col) / static_cast<float>(n - 1),
                           static_cast<float>(row) / static_cast<float>(n - 1)});
        }
    }
    return pts;
}

// One dimension of the Catmull-Rom basis (a uniform cubic B-spline in
// interpolating form; same math as core display/WarpMap.cpp): the value at
// t in [0,1] between nodes p1 and p2, given the neighbours p0 and p3.
float warpCatmullRom(float p0, float p1, float p2, float p3, float t)
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * ((2.f * p1) + (-p0 + p2) * t + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * t2 +
                   (-p0 + 3.f * p1 - 3.f * p2 + p3) * t3);
}

const ImVec4 kOkColor = {0.35f, 0.9f, 0.4f, 1.0f};
const ImVec4 kBadColor = {0.9f, 0.35f, 0.35f, 1.0f};
const ImVec4 kWarnColor = {1.0f, 0.8f, 0.3f, 1.0f};
// The currently active scene's button (Scenes block).
const ImVec4 kActiveSceneColor = {0.2f, 0.5f, 0.27f, 1.0f};
// The scene buttons are the primary operator control on the Playback window;
// a tall hit target is easier to press (especially on a touch panel) than the
// default single-line button height.
const float kSceneButtonHeight = 34.0f;

// The modal dialogs are opened from several places (the menu bar, the show
// editor) but must be Begun in one fixed context. In this Dear ImGui release
// OpenPopup()/BeginPopupModal() hash the popup name against the *current*
// window's ID stack - so the same name opened from two different windows
// yields two different popup ids, and BeginPopupModal() outside of any
// window (g.CurrentWindow == NULL) is a null dereference that never matches
// the id OpenPopup() pushed. The modals are therefore Begun inside a small
// invisible "##modalhost" window that is open for the whole frame, and
// OpenPopup() is called with the explicit id that BeginPopupModal() computes
// there. A top-level window's id is a plain name hash (context-free), and
// the host keeps no PushIDs, so the id is computable from anywhere:
//   host id = ImHashStr("##modalhost")          (window->ID)
//   popup id = ImHashStr(name, 0, host id)      (window->GetID(name))
constexpr const char* kModalHostName = "##modalhost";

// Modal dialogs open at a comfortable size: BeginPopupModal() would
// otherwise size the window to the content's *ideal* width, which for
// wrapped text and input fields leaves the dialog barely usable (the
// "dialogs open too small" complaint). The caller passes the width that
// fits the dialog's content; the height stays ideal (0) and the window
// remains user-resizable, so this only fixes the starting size.
void modalStartSize(float width)
{
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Appearing);
}

// Keys of the file dialogs (one shared instance, one dialog open at a
// time; the keys keep them independent).
constexpr const char* kOpenDialogKey = "OpenShow";
constexpr const char* kSaveAsDialogKey = "SaveAsShow";
constexpr const char* kMediaDialogKey = "AddMedia";
// The show files the dialogs accept (filter syntax: dot-prefixed
// extensions, comma separated).
constexpr const char* kShowFilter = ".json";
// Media the playback engine can load (FFmpeg sources: common video + still
// image formats), as named filter collections for the dialog's filter
// combo.
constexpr const char* kMediaFilter =
    "Video{.mp4,.mov,.avi,.mkv,.webm},Images{.png,.jpg,.jpeg,.bmp,.tga}";
// Reasonable minimum size for the file dialog windows (the user can still
// enlarge them).
const ImVec2 kFileDialogMinSize(640.0f, 440.0f);
const ImGuiWindowFlags kFileDialogFlags = ImGuiWindowFlags_NoDocking |
                                           ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoCollapse;

ImGuiID modalPopupId(const char* name)
{
    return ImHashStr(name, 0, ImHashStr(kModalHostName));
}

// Maps a test-hook name to the popup's display name (the string used in the
// BeginPopupModal() calls); null for unknown names and for the file dialogs
// (which are not popups - see fileDialogKey()).
const char* modalPopupName(const std::string& which)
{
    if (which == "pull")
    {
        return "Pull show from system";
    }
    if (which == "edit")
    {
        return "Display settings";
    }
    if (which == "syssettings")
    {
        return "System settings";
    }
    if (which == "warp")
    {
        return "Projection warp";
    }
    if (which == "about")
    {
        return "About MediaMax Control";
    }
    if (which == "mediadetails")
    {
        return "Media details";
    }
    if (which == "hotkeys")
    {
        return "Hot Keys";
    }
    return nullptr;
}

std::string truncate(const std::string& s, size_t limit)
{
    return s.size() <= limit ? s : s.substr(0, limit) + " ...";
}

} // namespace

// The hotkey table (Help > Hot Keys lists it; processHotkeys() fires it).
// See HotkeyBinding in ControlGui.h for the remap-ready design: bindings
// are data, so a future "edit bindings" UI only writes into these (and
// persists them).
const std::vector<ControlGuiApp::HotkeyBinding>& ControlGuiApp::hotkeyBindings()
{
    static const std::vector<HotkeyBinding> kBindings = {
        {"play_pause",
         "Play / Pause",
         "Toggle playback on the active system (play when stopped or paused, pause when "
         "playing).",
         ImGuiKey_Space,
         0},
    };
    return kBindings;
}

ControlGuiApp::ControlGuiApp()
{
}

ControlGuiApp::~ControlGuiApp()
{
    // SystemConnection destructors stop and join their poll threads.
}

void ControlGuiApp::processHotkeys()
{
    // A binding fires only when nothing else owns the keyboard: a text
    // field being typed into, a menu open, or any active widget sets
    // WantCaptureKeyboard (the previous frame's value - the input ground
    // truth for this frame).
    if (ImGui::GetIO().WantCaptureKeyboard)
    {
        return;
    }
    for (const HotkeyBinding& binding : hotkeyBindings())
    {
        // Exact chord (IsKeyChordPressed compares the modifier state
        // exactly): the main key pressed on its first frame (no
        // auto-repeat), a bare binding not firing under Ctrl+key.
        const ImGuiKeyChord chord = static_cast<ImGuiKeyChord>(binding.key | binding.mods);
        if (ImGui::IsKeyChordPressed(chord))
        {
            performHotkey(binding);
        }
    }
}

void ControlGuiApp::performHotkey(const HotkeyBinding& binding)
{
    if (std::strcmp(binding.id, "play_pause") == 0)
    {
        togglePlayback();
    }
    // New hotkeys: add the binding to hotkeyBindings() and a case here.
}

void ControlGuiApp::togglePlayback()
{
    SystemConnection* active = activeConnection();
    if (active == nullptr || !active->snapshot().haveStatus)
    {
        return; // nothing to toggle (same gate as the transport's buttons)
    }
    const auto snap = active->snapshot();
    const bool playing = snap.status.value("state", std::string()) == "playing";
    std::string error;
    sendActive(playing ? "playback.pause" : "playback.play", json::object(), error);
}

void ControlGuiApp::frame()
{
    // Hotkeys first: key state and the previous frame's keyboard capture
    // (text fields, open popups) are the frame's input ground truth.
    processHotkeys();
    if (m_Active != m_LastActive)
    {
        // Reset the transient playback edit state on system switch.
        m_LastActive = m_Active;
        m_HaveNameEdit = false;
        m_SeekHeld = false;
        // The show view's camera is fitted to the previous system's wall:
        // refit (the next renderShowViewWindow sees a new display
        // signature), and clear the fitted signature so even an identical
        // wall on the new system refits cleanly.
        m_ShowCam.fitted = false;
        m_ShowFittedSignature.clear();
    }
    // Invisible host for the modal dialogs (see kModalHostName). It is Begun
    // before the menu bar so the popup ids exist by the time a menu item can
    // fire, and Ended after renderModals() (which Begins the modals inside it).
    // A Begin/End pair around the whole frame is required: BeginPopupModal()
    // computes its id from the current window and would otherwise run with
    // g.CurrentWindow == NULL.
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1.0f, 1.0f), ImGuiCond_Always);
    ImGui::Begin(kModalHostName, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus);
    // Test-hook modal opens are deferred here: OpenPopup() must run inside a
    // frame with a current window (it dereferences g.CurrentWindow, which is
    // NULL between frames).
    if (!m_PendingTestOpenModal.empty())
    {
        const std::string which = m_PendingTestOpenModal;
        m_PendingTestOpenModal.clear();
        openModal(which);
    }
    if (m_PendingLiveDisplayEdit != kInvalidId)
    {
        const Id docId = m_PendingLiveDisplayEdit;
        m_PendingLiveDisplayEdit = kInvalidId;
        openDisplayEdit(docId);
    }
    renderMenuBar();
    renderDockspace();
    renderSystemsWindow();
    renderDisplaysWindow();
    renderShowViewWindow();
    renderPlaybackWindow();
    renderShowEditorWindow();
    renderConsoleWindow();
    renderModals();
    ImGui::End();
    // The show view wants live data (frame.state at 10 Hz) only while it is
    // visible; every other window is fine with the 500 ms status cadence.
    for (int i = 0; i < static_cast<int>(m_Systems.size()); ++i)
    {
        m_Systems[static_cast<size_t>(i)]->setLivePoll(
            m_ShowViewVisible && i == m_Active);
    }
    // The Open / Save As file dialogs own their windows; render them
    // top-level, outside the modal host. No-op while none is open.
    renderFileDialogs();
    if (m_FirstFrame)
    {
        m_FirstFrame = false;
    }
    // After the UI: the debounced auto-push of edited shows (no ImGui work).
    autoPushEditedShow();
}

// --- systems -----------------------------------------------------------------

void ControlGuiApp::syncSystemsToShow()
{
    // The show file is the system list's persistence target (issue 18):
    // every save and push serializes the current list into the document.
    // A direct document mutation - no touch(), so this does not by itself
    // mark the editor dirty or trigger an auto-push.
    Show& show = m_Editor.show();
    show.systems.clear();
    for (const auto& system : m_Systems)
    {
        show.systems.push_back({system->name(), system->host(), system->port()});
    }
}

void ControlGuiApp::applyShowSystems()
{
    // The show file is the system list's source of truth (issue 18): the
    // document's list becomes the GUI's. Entries with the same
    // name+host+port reuse the existing connection (it stays online);
    // everything else is dropped, and new entries connect.
    const Show& show = m_Editor.show();
    std::vector<std::unique_ptr<SystemConnection>> next;
    for (const auto& ref : show.systems)
    {
        std::unique_ptr<SystemConnection> conn;
        for (size_t i = 0; i < m_Systems.size(); ++i)
        {
            if (m_Systems[i]->name() == ref.name && m_Systems[i]->host() == ref.host &&
                m_Systems[i]->port() == ref.port)
            {
                conn = std::move(m_Systems[i]);
                m_Systems.erase(m_Systems.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        if (!conn)
        {
            conn = std::make_unique<SystemConnection>(ref.name, ref.host, ref.port);
            conn->connect();
        }
        next.push_back(std::move(conn));
    }
    m_Systems = std::move(next); // the rest are dropped (poll threads stopped)
    m_Active = m_Systems.empty() ? -1 : 0;
    m_LastActive = m_Active;
    // The per-dialog stored targets are invalid with a new list; the
    // dialogs are modal, so none can be open when a show is opened.
    m_EditTarget = nullptr;
    m_WarpTarget = nullptr;
    m_SysSettingsIndex = -1;
    log("show opened: " + std::to_string(m_Systems.size()) +
        (m_Systems.size() == 1 ? " system" : " systems") + " from the document");
}

bool ControlGuiApp::saveEditorFile(const std::string& path, std::string& error)
{
    syncSystemsToShow();
    return m_Editor.saveFile(path, error);
}

SystemConnection* ControlGuiApp::activeConnection() const
{
    if (m_Active < 0 || m_Active >= static_cast<int>(m_Systems.size()))
    {
        return nullptr;
    }
    return m_Systems[static_cast<size_t>(m_Active)].get();
}

// --- console / command helpers -------------------------------------------------

void ControlGuiApp::log(const std::string& line)
{
    m_ConsoleLog.push_back(truncate(line, kLogLineLimit));
    while (m_ConsoleLog.size() > kMaxLogLines)
    {
        m_ConsoleLog.erase(m_ConsoleLog.begin());
    }
}

bool ControlGuiApp::sendActive(const std::string& method, const json& params, std::string& error,
                               json* outResult)
{
    SystemConnection* active = activeConnection();
    if (!active)
    {
        error = "no system selected";
        return false;
    }
    log("> " + method + (params.is_null() || params.empty() ? "" : " " + params.dump()));
    json result;
    if (!active->send(method, params, result, error))
    {
        log("  <= error: " + error);
        return false;
    }
    log("  <= " + result.dump());
    if (outResult)
    {
        *outResult = std::move(result);
    }
    return true;
}

void ControlGuiApp::consoleSubmit()
{
    const size_t begin = m_ConsoleInput.find_first_not_of(" \t");
    if (begin == std::string::npos)
    {
        m_ConsoleInput.clear();
        return;
    }
    const size_t end = m_ConsoleInput.find_last_not_of(" \t");
    const std::string line = m_ConsoleInput.substr(begin, end - begin + 1);
    const size_t space = line.find(' ');
    const std::string method = space == std::string::npos ? line : line.substr(0, space);
    json params = json::object();
    if (space != std::string::npos)
    {
        params = json::parse(line.substr(space + 1), nullptr, false);
        if (params.is_discarded())
        {
            log("invalid json-params: " + line.substr(space + 1));
            return;
        }
    }
    m_ConsoleInput.clear();
    std::string error;
    sendActive(method, params, error);
}

nlohmann::json ControlGuiApp::showPushParams()
{
    syncSystemsToShow(); // the pushed document carries the system list (issue 18)
    nlohmann::json params = {{"show", m_Editor.documentJson()}, {"preserveState", true}};
    // Relative media paths in the document resolve against the directory of
    // the file the editor has open (mediaDir), so a push behaves like a
    // show.load of that file on the system. An unsaved document has no
    // directory; the system then uses its own working directory.
    const std::string& file = m_Editor.filePath();
    if (!file.empty())
    {
        const std::filesystem::path dir = std::filesystem::path(file).parent_path();
        if (!dir.empty())
        {
            params["mediaDir"] = dir.string();
        }
    }
    return params;
}

void ControlGuiApp::pushToActiveSystem()
{
    SystemConnection* active = activeConnection();
    if (!active)
    {
        m_EditorError = "no system selected";
        return;
    }
    log("pushing show '" + m_Editor.show().name + "' to " + active->name() + " (show.loadJson)");
    std::string error;
    // preserveState: a running system keeps playing (state, time, overrides)
    // instead of restarting at 0.
    if (sendActive("show.loadJson", showPushParams(), error))
    {
        m_EditorError.clear();
    }
    else
    {
        m_EditorError = error;
    }
}

bool ControlGuiApp::pushShowToSystems(std::span<SystemConnection*> targets, const char* reason)
{
    if (targets.empty())
    {
        return true; // nothing to do; the caller decides how to count that
    }
    json params = showPushParams();
    bool allOk = true;
    for (SystemConnection* target : targets)
    {
        json result;
        std::string error;
        if (target->send("show.loadJson", params, result, error))
        {
            log("pushed show '" + m_Editor.show().name + "' to " + target->name() + " (" +
                reason + ")");
        }
        else
        {
            log("show push to " + target->name() + " failed (" + reason + "): " + error);
            allOk = false;
        }
    }
    if (allOk)
    {
        m_EditorError.clear();
    }
    else
    {
        // The console log carries the per-target errors; the editor line
        // just says the push did not reach everything.
        m_EditorError = "show push failed for some systems (see console)";
    }
    return allOk;
}

bool ControlGuiApp::pushShowToAllConnected(const char* reason)
{
    std::vector<SystemConnection*> targets;
    for (auto& system : m_Systems)
    {
        if (system->pollConnected())
        {
            targets.push_back(system.get());
        }
    }
    return pushShowToSystems(targets, reason);
}

bool ControlGuiApp::loadShowFile(const std::string& path, std::string& error)
{
    if (!m_Editor.loadFile(path, error))
    {
        return false; // `error` from ShowEditor::loadFile
    }
    // The show file carries the system list (issue 18): the document's
    // entries become the GUI's connections.
    applyShowSystems();
    // Push the loaded show to every connected system right away - see
    // loadShowFile's header comment.
    pushShowToAllConnected("show load");
    return true;
}

// kAutoPushDelay: how long after the last edit before an auto-push goes out
// (keeps a burst of edits to a single push), and the retry throttle after a
// failed one.
constexpr auto kAutoPushDelay = std::chrono::seconds(1);

void ControlGuiApp::autoPushEditedShow()
{
    if (!m_AutoPushEdits || !m_Editor.dirty())
    {
        return;
    }
    const std::uint64_t generation = m_Editor.generation();
    if (generation == m_AutoPushedGeneration)
    {
        return; // this edit burst was already pushed (or is not yet dirty)
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - m_Editor.lastTouched() < kAutoPushDelay)
    {
        return; // debounce: the user is still editing
    }
    if (now < m_NextAutoPushAttempt)
    {
        return; // retry throttle after a failed push
    }
    std::vector<SystemConnection*> targets;
    for (auto& system : m_Systems)
    {
        if (system->pollConnected())
        {
            targets.push_back(system.get());
        }
    }
    if (targets.empty())
    {
        // Nothing to push to: do NOT mark the generation pushed, so an edit
        // made while offline still goes out when a system comes online.
        return;
    }
    m_NextAutoPushAttempt = now + kAutoPushDelay;

    // Retry on a later frame (throttled) until every system accepted it.
    if (pushShowToSystems(targets, "auto-push"))
    {
        m_AutoPushedGeneration = generation;
    }
}

// --- menu bar + layout ----------------------------------------------------------

void ControlGuiApp::renderMenuBar()
{
    if (!ImGui::BeginMainMenuBar())
    {
        return;
    }
    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("New Show"))
        {
            m_Editor.newShow();
            m_EditorError.clear();
        }
        if (ImGui::MenuItem("Open..."))
        {
            openFileDialog();
        }
        const bool canSave = !m_Editor.filePath().empty();
        if (ImGui::MenuItem("Save", nullptr, nullptr, canSave) && canSave)
        {
            std::string error;
            if (!saveEditorFile(m_Editor.filePath(), error))
            {
                m_EditorError = error;
            }
        }
        if (ImGui::MenuItem("Save As..."))
        {
            saveAsFileDialog();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit"))
        {
            m_ShutdownRequested = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help"))
    {
        if (ImGui::MenuItem("Hot Keys"))
        {
            m_HotKeysModal = true;
            ImGui::OpenPopup(modalPopupId("Hot Keys"));
        }
        if (ImGui::MenuItem("About"))
        {
            m_AboutModal = true;
            ImGui::OpenPopup(modalPopupId("About MediaMax Control"));
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void ControlGuiApp::renderDockspace()
{
    // One dockspace filling the whole viewport.
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##dockspace", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
    ImGuiID dockspaceId = ImGui::GetID("MainDockspace");
    if (m_FirstFrame)
    {
        // One-shot initial layout, applied on every start so the UI is
        // deterministic (user rearrangements persist for the session).
        if (ImGui::DockBuilderGetNode(dockspaceId) != nullptr)
        {
            ImGui::DockBuilderRemoveNode(dockspaceId);
        }
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodePos(dockspaceId, viewport->WorkPos);
        ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

        // Split the root into the final layout. Each split replaces the
        // node it operates on, so the "remaining" (opposite) node ID must
        // be threaded through a dedicated variable: passing &dockspaceId
        // here clobbers the root ID, and the DockSpace() call below would
        // then target the center node on the first frame. That node keeps
        // the DockSpace flag and a private host window forever (child
        // nodes normally inherit their host from their parent, but
        // IsDockSpace() nodes use their own), so the Playback pane
        // renders into a window that is never submitted again, and once
        // ImGui's memory compaction (ConfigMemoryCompactTimer, 60 s)
        // frees that host's draw-list channels the docking code crashes
        // with a write-to-NULL in ImDrawListSplitter::SetCurrentChannel.
        ImGuiID center = dockspaceId;
        ImGuiID left = 0;
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.16f, &left, &center);
        ImGuiID right = 0;
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, &right, &center);
        // The center column splits vertically: the 2D show view on top, the
        // transport + scenes playback pane below.
        ImGuiID centerBottom = 0;
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.42f, &centerBottom, &center);

        // The left column holds the system list (top) and the
        // displays/media window (bottom).
        ImGuiID leftBottom = 0;
        ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.5f, &leftBottom, &left);

        ImGui::DockBuilderDockWindow("Systems", left);
        ImGui::DockBuilderDockWindow("Displays", leftBottom);
        ImGui::DockBuilderDockWindow("Show view", center);
        // Playback and Console share one tab block (dock order = tab
        // order: Playback first, Console second).
        ImGui::DockBuilderDockWindow("Playback", centerBottom);
        ImGui::DockBuilderDockWindow("Console", centerBottom);
        ImGui::DockBuilderDockWindow("Show Editor", right);
        // Finish with the root ID (the one given to DockBuilderAddNode);
        // DockSpace() below must receive the same root every frame.
        ImGui::DockBuilderFinish(dockspaceId);
    }
    checkDockInvariants(dockspaceId);
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    ImGui::End();
}

// Dear ImGui's docking keeps the node tree and the single DockSpace host
// window in a strict relation: during DockNodeUpdate(), every node
// reachable from the dockspace root takes its HostWindow from its parent
// (child nodes set `node->HostWindow = node->ParentNode->HostWindow`), so
// all nodes share the root's freshly Begin()-ed host, whose DrawList Begin()
// split into the docking channels. If a node ever holds a foreign/stale
// HostWindow (a window whose draw list was never split), the next
// DockNodeUpdate crashes with a write-to-NULL in
// ImDrawListSplitter::SetCurrentChannel. This check walks the tree before
// DockSpace() each frame and records any drift, so such a crash is always
// preceded by a diagnostic (console + crash log).
void ControlGuiApp::checkDockInvariants(std::uint32_t dockspaceId)
{
    const ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspaceId);
    if (root == nullptr)
    {
        return;
    }

    auto describeWindow = [](const ImGuiWindow* w) {
        if (w == nullptr)
        {
            return std::string("null");
        }
        char buf[320];
        std::snprintf(buf, sizeof(buf), "'%s' id=0x%08X flags=0x%08X active=%d",
                      w->Name != nullptr ? w->Name : "(null)", w->ID,
                      static_cast<unsigned int>(w->Flags), w->Active ? 1 : 0);
        return std::string(buf);
    };
    auto nodeId = [](const ImGuiDockNode* n) -> std::string {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08X", n != nullptr ? n->ID : 0);
        return buf;
    };

    const ImGuiWindow* expectedHost = root->HostWindow;
    std::vector<std::string> issues;

    // Host sanity (frame 2+; on the first frame the host window is created
    // by the DockSpace() call that follows this check).
    if (!m_FirstFrame)
    {
        if (expectedHost == nullptr)
        {
            issues.push_back("dockspace root " + nodeId(root) + " has no host window before DockSpace()");
        }
        else if ((expectedHost->Flags & ImGuiWindowFlags_DockNodeHost) == 0)
        {
            issues.push_back("dockspace host window is not a DockNodeHost: " + describeWindow(expectedHost));
        }
        else if (expectedHost->DrawList != nullptr &&
                 expectedHost->DrawList->_Splitter._Channels.Capacity == 0)
        {
            char buf[320];
            std::snprintf(buf, sizeof(buf),
                          "dockspace host '%s' draw list lost its docking channels before DockSpace() "
                          "(splitter{current=%d count=%d})",
                          expectedHost->Name != nullptr ? expectedHost->Name : "(null)",
                          expectedHost->DrawList->_Splitter._Current,
                          expectedHost->DrawList->_Splitter._Count);
            issues.push_back(buf);
        }
    }

    // Tree walk: every child must point back at the node that references it,
    // and (once the first DockSpace ran) every node's HostWindow must be the
    // root's host.
    const int kMaxDepth = 16;
    auto walk = [&](auto&& self, const ImGuiDockNode* parent, int depth) -> void {
        if (depth >= kMaxDepth)
        {
            return;
        }
        for (int slot = 0; slot < 2; ++slot)
        {
            const ImGuiDockNode* child = parent->ChildNodes[slot];
            if (child == nullptr)
            {
                continue;
            }
            if (child->ParentNode != parent)
            {
                issues.push_back("node " + nodeId(child) + " (slot " + std::to_string(slot) + " of " +
                                 nodeId(parent) + ") has parent " + nodeId(child->ParentNode) +
                                 ", not the node that references it");
            }
            if (!m_FirstFrame && child->HostWindow != nullptr && child->HostWindow != expectedHost)
            {
                issues.push_back("node " + nodeId(child) + " holds foreign host window " +
                                 describeWindow(child->HostWindow) + " (expected " +
                                 describeWindow(expectedHost) + ")");
            }
            if (child->HostWindow != nullptr &&
                (child->HostWindow->Flags & ImGuiWindowFlags_DockNodeHost) == 0)
            {
                issues.push_back("node " + nodeId(child) + " host window lacks the DockNodeHost flag: " +
                                 describeWindow(child->HostWindow));
            }
            self(self, child, depth + 1);
        }
    };
    walk(walk, root, 0);

    if (issues.empty())
    {
        return;
    }
    std::string text = "dock invariant: ";
    for (size_t i = 0; i < issues.size(); ++i)
    {
        if (i > 0)
        {
            text += " | ";
        }
        text += issues[i];
    }
    // A persistent violation must not spam the 500-line console: log only
    // when the reported state changes.
    static std::string s_Last;
    if (text == s_Last)
    {
        return;
    }
    s_Last = text;
    log(text);
    appendDiagnostic(text);
}

// --- systems window ---------------------------------------------------------------

void ControlGuiApp::addSystem(const std::string& name, const std::string& host, int port)
{
    m_Systems.push_back(
        std::make_unique<SystemConnection>(name, host, static_cast<std::uint16_t>(port)));
    m_Systems.back()->connect(); // dials the running engine; the poll thread reports state
    m_Active = static_cast<int>(m_Systems.size()) - 1;
    log("added system " + name + " (" + host + ":" + std::to_string(port) + "); connecting...");
    m_NewName.clear();
    m_SysTestResult.clear();
    m_SysTestOk = false;
}

void ControlGuiApp::testSystemAddress(const std::string& host, int port)
{
    // One-shot probe (explicit user action, so a <= ~2 s UI-thread block is
    // acceptable). Uses a throwaway connection: the poll connection of an
    // already-registered system must not be disturbed.
    m_SysTestOk = false;
    mediamax::ControlClient client;
    if (!client.connect(host, static_cast<std::uint16_t>(port), 2000))
    {
        m_SysTestResult = "unreachable: " + host + ":" + std::to_string(port) + " (no connection)";
        return;
    }
    json result;
    std::string error;
    if (client.request("ping", json::object(), result, error, 2000))
    {
        m_SysTestOk = true;
        m_SysTestResult = "reachable: system '" + result.value("system", std::string("unnamed")) + "'";
    }
    else
    {
        m_SysTestResult = "unreachable: " + error;
    }
    client.disconnect();
}

void ControlGuiApp::renderSystemsWindow()
{
    // Begin() may return false (collapsed/fully clipped window) but the
    // window is already on the stack: a matching End() is still required
    // (ImGui's Begin/End are asymmetric, unlike BeginPopup/EndPopup etc.).
    if (!ImGui::Begin("Systems", nullptr, ImGuiWindowFlags_HorizontalScrollbar))
    {
        ImGui::End();
        return;
    }
    // The system list is stored in the show file (issue 18): File > Open
    // loads it, File > Save (and pushes) write it back.
    ImGui::TextDisabled(
        "Systems are stored in the show file: File > Open loads them, File > Save writes them "
        "back.");
    ImGui::Separator();

    // "Add system" — always visible at the top of the pane. This section used
    // to sit below the systems table, where it scrolled out of view as soon
    // as the list grew, so the button looked missing.
    ImGui::TextUnformatted("Add system");
    ImGui::InputTextWithHint("##newname", "name", &m_NewName);
    ImGui::InputText("##newhost", &m_NewHost);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("##newport", &m_NewPort);
    ImGui::SameLine();
    if (ImGui::Button("Test##systest"))
    {
        testSystemAddress(m_NewHost, m_NewPort);
    }
    if (!m_SysTestResult.empty())
    {
        ImGui::TextWrapped("%s", m_SysTestResult.c_str());
    }
    const bool canAdd = !m_NewName.empty() && !m_NewHost.empty() && m_NewPort >= 1 && m_NewPort <= 65535;
    ImGui::BeginDisabled(!canAdd);
    if (ImGui::Button("Add system##add"))
    {
        addSystem(m_NewName, m_NewHost, m_NewPort);
    }
    ImGui::EndDisabled();
    if (!canAdd)
    {
        ImGui::TextDisabled("needs a name, a host and a port (1-65535)");
    }
    ImGui::Spacing();
    ImGui::Separator();

    // The system list scrolls on its own so a long list can never push the
    // form (or the error line) out of view again.
    if (ImGui::BeginChild("systemlist", ImVec2(0.0f, 0.0f), false))
    {
        if (ImGui::BeginTable("systems", 4,
                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersOuter))
        {
            ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthFixed, 64.0f);
            ImGui::TableSetupColumn("name");
            ImGui::TableSetupColumn("address");
            ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed, 186.0f);
            for (size_t i = 0; i < m_Systems.size(); ++i)
            {
                auto& conn = *m_Systems[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (conn.pollConnected())
                {
                    ImGui::TextColored(kOkColor, "online");
                }
                else
                {
                    ImGui::TextColored(kBadColor, "offline");
                }
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(conn.name().c_str(), m_Active == static_cast<int>(i)))
                {
                    m_Active = static_cast<int>(i);
                }
                ImGui::PopID();
                // The computer the engine runs on (status "hostname") under
                // the custom name (issue 16).
                const SystemConnection::Snapshot sysSnap = conn.snapshot();
                if (sysSnap.haveStatus)
                {
                    const std::string machine = sysSnap.status.value("hostname", "");
                    if (!machine.empty())
                    {
                        ImGui::TextDisabled("on %s", machine.c_str());
                    }
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted((conn.host() + ":" + std::to_string(conn.port())).c_str());
                ImGui::TableNextColumn();
                if (ImGui::Button(conn.active() ? "Disconnect" : "Connect##sys"))
                {
                    if (conn.active())
                    {
                        conn.disconnect();
                        log(conn.name() + " disconnected");
                    }
                    else
                    {
                        conn.connect();
                        log(conn.name() + " connecting...");
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("settings##sysset"))
                {
                    openSystemSettings(static_cast<int>(i));
                }
                ImGui::SameLine();
                if (ImGui::Button("x##rm"))
                {
                    const std::string name = conn.name();
                    conn.disconnect();
                    m_Systems.erase(m_Systems.begin() + static_cast<std::ptrdiff_t>(i));
                    if (m_Active >= static_cast<int>(m_Systems.size()))
                    {
                        m_Active = static_cast<int>(m_Systems.size()) - 1;
                    }
                    else if (m_Active > static_cast<int>(i))
                    {
                        --m_Active;
                    }
                    log("removed system " + name);
                    break;
                }
            }
            ImGui::EndTable();
        }
        if (m_Systems.empty())
        {
            ImGui::TextDisabled("(no systems - add one above)");
        }

        // Errors of the selected system, if any.
        if (SystemConnection* active = activeConnection(); active && !active->snapshot().lastError.empty())
        {
            ImGui::Spacing();
            ImGui::TextColored(kBadColor, "%s", active->snapshot().lastError.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// --- displays window (active system: displays + media in tabs) -------------------------

void ControlGuiApp::renderDisplaysWindow()
{
    // Begin() may return false; End() must still be called (see above).
    if (!ImGui::Begin("Displays", nullptr, ImGuiWindowFlags_HorizontalScrollbar))
    {
        ImGui::End();
        return;
    }
    // The Displays tab is document-driven (the show file's display wall):
    // it is listed and editable with no system connected. The Media tab
    // still needs a live system (the media library is per engine).
    if (ImGui::BeginTabBar("##displaystab"))
    {
        if (ImGui::BeginTabItem("Displays"))
        {
            renderDisplaysBlock();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Media"))
        {
            SystemConnection* active = activeConnection();
            if (!active || !active->snapshot().haveStatus)
            {
                ImGui::TextDisabled(active ? "Waiting for status..."
                                           : "Select a system (left) and make sure it is online.");
            }
            else
            {
                renderMediaBlock(active->snapshot());
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// --- playback window ----------------------------------------------------------------

void ControlGuiApp::renderPlaybackWindow()
{
    // Begin() may return false; End() must still be called (see above).
    if (!ImGui::Begin("Playback", nullptr, ImGuiWindowFlags_HorizontalScrollbar))
    {
        ImGui::End();
        return;
    }
    SystemConnection* active = activeConnection();
    if (!active)
    {
        ImGui::TextDisabled("Select a system (left) and make sure it is online.");
        ImGui::End();
        return;
    }
    const auto snap = active->snapshot();
    ImGui::Text("%s  (%s:%d)", active->name().c_str(), active->host().c_str(), active->port());
    if (!snap.haveStatus)
    {
        if (!snap.lastError.empty())
        {
            ImGui::TextColored(kBadColor, "%s", snap.lastError.c_str());
        }
        else
        {
            ImGui::TextDisabled("Waiting for status...");
        }
        ImGui::End();
        return;
    }
    renderTransportBlock(snap);
    ImGui::Separator();
    renderScenesBlock(snap);
    ImGui::End();
}

void ControlGuiApp::renderTransportBlock(const SystemConnection::Snapshot& snap)
{
    const json& st = snap.status;
    const std::string state = st.value("state", "?");
    const std::string timecode = st.value("timecode", "?");
    const double time = st.value("time", 0.0);
    const double duration = st.value("duration", -1.0);
    const std::string frameRate = st.value("frameRate", "?");
    const Id sceneId = static_cast<Id>(st.value("scene", 0u));

    // Smooth timecode: the poll thread only refreshes the snapshot every
    // 500 ms, but the clock should advance every UI frame. While the engine
    // is playing, the reported time and timecode are extrapolated forward
    // with the UI's own wall clock, anchored on the engine's last reported
    // timecode (see mediamax/time/Timecode.h) so the frame field stays
    // exact. The extrapolation freezes once the last good poll is older
    // than kStalePollSeconds, so a dead connection cannot keep the clock
    // running; when the engine is not playing the clock shows the reported
    // values unchanged.
    double shownTime = time;
    std::string shownTimecode = timecode;
    if (state == "playing")
    {
        double fps = 0.0;
        std::int64_t nominalFps = 0;
        if (parseFrameRateString(frameRate, fps, nominalFps))
        {
            const double age = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count() -
                               snap.lastUpdate;
            if (age >= 0.0 && age < kStalePollSeconds)
            {
                const std::int64_t baseFrames = timecodeToFrames(timecode, nominalFps);
                if (baseFrames >= 0)
                {
                    shownTime = time + age;
                    shownTimecode =
                        framesToTimecode(baseFrames + static_cast<std::int64_t>(std::llround(age * fps)),
                                         nominalFps);
                }
            }
        }
    }

    // Current scene: the id plus its name (looked up from scene.list), so
    // the operator sees what is playing, not just a number. With several
    // scenes active at once this is the engine's primary scene; the full
    // active set is visible (highlighted) in the Scenes block below. With
    // no scene active (the operator's manual set is empty) it reads
    // "no scene".
    std::string sceneLabel;
    if (sceneId == kInvalidId)
    {
        sceneLabel = "no scene";
    }
    else
    {
        sceneLabel = "scene " + std::to_string(sceneId);
        if (snap.scenes.contains("scenes") && snap.scenes["scenes"].is_array())
        {
            for (const auto& s : snap.scenes["scenes"])
            {
                if (s.is_object() && s.value("id", 0u) == sceneId)
                {
                    sceneLabel += " " + s.value("name", std::string());
                    break;
                }
            }
        }
    }

    ImGui::Text("State: %s     %s     %s fps     %s", state.c_str(), shownTimecode.c_str(),
                frameRate.c_str(), sceneLabel.c_str());
    if (duration < 0.0)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%.2f s / unlimited", shownTime);
    }
    else
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%.2f / %.2f s", shownTime, duration);
    }

    if (ImGui::Button("Play", ImVec2(64, 0)))
    {
        std::string error;
        sendActive("playback.play", json::object(), error);
    }
    ImGui::SameLine();
    if (ImGui::Button("Pause", ImVec2(64, 0)))
    {
        std::string error;
        sendActive("playback.pause", json::object(), error);
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop", ImVec2(64, 0)))
    {
        std::string error;
        sendActive("playback.stop", json::object(), error);
    }

    if (duration > 0.0)
    {
        if (!m_SeekHeld)
        {
            m_Seek = shownTime; // follow the live time while not dragging
        }
        float seek = static_cast<float>(m_Seek);
        if (ImGui::SliderFloat("t##seek", &seek, 0.0f, static_cast<float>(duration), "%.2f s"))
        {
            m_Seek = seek;
            std::string error;
            sendActive("playback.seek", {{"time", m_Seek}}, error);
        }
        m_SeekHeld = ImGui::IsItemActive();
    }
    else
    {
        ImGui::InputDouble("Seek to (s)##seekabs", &m_SeekInput, 1.0, 5.0, "%.2f");
        ImGui::SameLine();
        if (ImGui::Button("Go##seekabs"))
        {
            std::string error;
            sendActive("playback.seek", {{"time", m_SeekInput}}, error);
        }
    }

    const std::string remoteName = st.value("system", "");
    if (!m_HaveNameEdit)
    {
        m_SystemNameEdit = remoteName;
        m_HaveNameEdit = true;
    }
    ImGui::InputText("System name##sysname", &m_SystemNameEdit);
    ImGui::SameLine();
    if (ImGui::Button("Rename##sysname") && m_SystemNameEdit != remoteName)
    {
        std::string error;
        sendActive("system.set", {{"name", m_SystemNameEdit}}, error);
    }
}

void ControlGuiApp::renderScenesBlock(const SystemConnection::Snapshot& snap)
{
    ImGui::TextUnformatted("Scenes");
    if (!snap.scenes.contains("scenes") || !snap.scenes["scenes"].is_array())
    {
        ImGui::TextDisabled("(no scenes - load a show)");
        return;
    }
    const json& scenes = snap.scenes["scenes"];

    // One row per scene: a full-width button labelled "<id> - <name>" and a
    // status column. Clicking a scene toggles it in/out of the active set
    // (scene.toggle) - several scenes may be active at once, composited in
    // list order. Active scenes are highlighted on their button AND flagged
    // in the status column - the flag lives in its own column because a
    // full-width button leaves no room on the same line.
    if (ImGui::BeginTable("scenes", 2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersOuter))
    {
        ImGui::TableSetupColumn("scene", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##active", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        for (int i = 0; i < static_cast<int>(scenes.size()); ++i)
        {
            const json& s = scenes[static_cast<size_t>(i)];
            const bool active = s.value("active", false);
            const std::string label =
                std::to_string(s.value("id", 0u)) + " - " + s.value("name", "?");
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (active)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, kActiveSceneColor);
            }
            if (ImGui::Button(label.c_str(), ImVec2(-FLT_MAX, kSceneButtonHeight)))
            {
                std::string error;
                sendActive("scene.toggle", {{"index", i}}, error);
            }
            if (active)
            {
                ImGui::PopStyleColor();
            }
            ImGui::TableNextColumn();
            if (active)
            {
                ImGui::TextColored(kOkColor, "active");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Return to timeline (scene.clear)", ImVec2(-FLT_MAX, kSceneButtonHeight)))
    {
        std::string error;
        sendActive("scene.clear", json::object(), error);
    }
}

void ControlGuiApp::addShowDisplay()
{
    // A new panel tiled right of the document's current wall at 1920x1080
    // (an empty wall starts at the origin). The name is the display's
    // identity, so it must be unique within the document.
    Show& show = m_Editor.show();
    std::int32_t wallRight = 0;
    for (const auto& d : show.displays)
    {
        wallRight = std::max(wallRight, d.x + std::max(0, d.width));
    }
    ShowDisplay display;
    display.id = ShowEditor::nextDisplayId(show);
    int n = 1;
    do
    {
        display.name = "Display " + std::to_string(n++);
    } while (std::any_of(show.displays.begin(), show.displays.end(),
                         [&](const ShowDisplay& d) { return d.name == display.name; }));
    display.x = wallRight;
    display.y = 0;
    show.displays.push_back(display);
    m_Editor.touch();
    log("display '" + display.name + "' added to the show (the wall)");
}

void ControlGuiApp::removeShowDisplay(Id docDisplayId)
{
    Show& show = m_Editor.show();
    for (auto it = show.displays.begin(); it != show.displays.end(); ++it)
    {
        if (it->id == docDisplayId)
        {
            log("display '" + it->name + "' removed from the show (the systems drop it on the "
                "next show push)");
            show.displays.erase(it);
            m_Editor.touch();
            return;
        }
    }
}

SystemConnection* ControlGuiApp::mappedSystemFor(const std::string& displayName) const
{
    for (const auto& system : m_Systems)
    {
        if (!system->pollConnected())
        {
            continue;
        }
        const auto snap = system->snapshot();
        if (!snap.haveStatus || !snap.status.contains("displays") || !snap.status["displays"].is_array())
        {
            continue;
        }
        for (const auto& d : snap.status["displays"])
        {
            if (d.value("name", "") == displayName)
            {
                return system.get();
            }
        }
    }
    return nullptr;
}

Id ControlGuiApp::liveDisplayIdFor(const SystemConnection* system,
                                   const std::string& displayName) const
{
    if (system == nullptr)
    {
        return kInvalidId;
    }
    const auto snap = system->snapshot();
    if (!snap.haveStatus || !snap.status.contains("displays") || !snap.status["displays"].is_array())
    {
        return kInvalidId;
    }
    for (const auto& d : snap.status["displays"])
    {
        if (d.value("name", "") == displayName)
        {
            return static_cast<Id>(d.value("id", 0u));
        }
    }
    return kInvalidId;
}

void ControlGuiApp::openDisplayEdit(Id docDisplayId)
{
    const ShowDisplay* sd = nullptr;
    for (const auto& d : m_Editor.show().displays)
    {
        if (d.id == docDisplayId)
        {
            sd = &d;
            break;
        }
    }
    if (sd == nullptr)
    {
        return; // the entry went away (removed by another edit)
    }
    // Resync from the document entry so the dialog always starts from what
    // the show file says (an old edit must not survive).
    DisplayEdit& de = m_DisplayEdits[docDisplayId];
    de.x = sd->x;
    de.y = sd->y;
    de.width = sd->width;
    de.height = sd->height;
    de.brightness = sd->brightness;
    de.enabled = sd->enabled;
    de.alwaysOnTop = sd->alwaysOnTop;
    de.statusOverlay = sd->statusOverlay;
    de.vsync = sd->vsync;
    de.name = sd->name;
    // The physical output is engine state: show it while the entry runs on
    // a mapped system, otherwise leave it blank.
    de.monitor.clear();
    m_EditTarget = mappedSystemFor(sd->name);
    if (m_EditTarget != nullptr)
    {
        const auto snap = m_EditTarget->snapshot();
        if (snap.haveStatus && snap.status.contains("displays") &&
            snap.status["displays"].is_array())
        {
            for (const auto& d : snap.status["displays"])
            {
                if (d.value("name", "") == sd->name)
                {
                    de.monitor = d.value("monitor", "");
                    break;
                }
            }
        }
    }
    m_EditDisplayId = docDisplayId;
    // The System dropdown starts on the entry's current mapping.
    m_EditSystem = -1;
    for (int i = 0; i < static_cast<int>(m_Systems.size()); ++i)
    {
        if (m_Systems[static_cast<size_t>(i)].get() == m_EditTarget)
        {
            m_EditSystem = i;
            break;
        }
    }
    m_EditError.clear();
    m_EditModal = true;
    ImGui::OpenPopup(modalPopupId("Display settings"));
}

Id ControlGuiApp::ensureShowDisplayEntryForLive(const ShowViewDisplay& live)
{
    // The settings dialog edits show-document entries: find the entry with
    // this live display's name, or add one from the live record when the
    // document does not define the display yet (it came from an older show
    // file or was created on the engine directly).
    Show& show = m_Editor.show();
    ShowDisplay* found = nullptr;
    for (auto& d : show.displays)
    {
        if (d.name == live.name)
        {
            found = &d;
            break;
        }
    }
    if (found == nullptr)
    {
        ShowDisplay d;
        d.id = ShowEditor::nextDisplayId(show);
        d.name = live.name;
        d.x = static_cast<std::int32_t>(std::llround(live.x));
        d.y = static_cast<std::int32_t>(std::llround(live.y));
        d.width = static_cast<std::int32_t>(std::llround(live.w));
        d.height = static_cast<std::int32_t>(std::llround(live.h));
        show.displays.push_back(std::move(d));
        m_Editor.touch();
        log("show view: display '" + live.name +
            "' added to the show document from the live wall");
        for (auto& d2 : show.displays)
        {
            if (d2.name == live.name)
            {
                found = &d2;
                break;
            }
        }
    }
    return found != nullptr ? found->id : kInvalidId;
}

void ControlGuiApp::openDisplayEditForLiveDisplay(const ShowViewDisplay& live)
{
    const Id docId = ensureShowDisplayEntryForLive(live);
    if (docId != kInvalidId)
    {
        openDisplayEdit(docId);
    }
}

bool ControlGuiApp::applyDisplayEdit()
{
    if (m_EditDisplayId == kInvalidId)
    {
        return false;
    }
    DisplayEdit& de = m_DisplayEdits[m_EditDisplayId];
    m_EditError.clear();
    if (de.name.empty())
    {
        m_EditError = "the display name must not be empty";
        return false;
    }
    if (de.width <= 0 || de.height <= 0)
    {
        m_EditError = "the display size must be positive";
        return false;
    }
    ShowDisplay* sd = nullptr;
    for (auto& d : m_Editor.show().displays)
    {
        if (d.id == m_EditDisplayId)
        {
            sd = &d;
            break;
        }
    }
    if (sd == nullptr)
    {
        m_EditError = "the display is no longer in the show";
        m_EditModal = false;
        return false;
    }
    const std::string oldName = sd->name;

    // 1) Persistence: the dialog's values become the show file's wall
    //    entry. The auto-push (the touch below) propagates the wall to
    //    every connected system - the engine builds it on the next show
    //    load; with no system connected the settings are simply saved.
    sd->name = de.name;
    sd->x = de.x;
    sd->y = de.y;
    sd->width = de.width;
    sd->height = de.height;
    sd->brightness = de.brightness;
    sd->enabled = de.enabled;
    sd->alwaysOnTop = de.alwaysOnTop;
    sd->statusOverlay = de.statusOverlay;
    sd->vsync = de.vsync;
    m_Editor.touch();

    // 2) Live effect: while the entry runs on a connected system (a live
    //    display with the old name), apply the values there at once.
    //    Selecting a different system in the dialog's System dropdown moves
    //    the live display there (a temporary override - the show's wall is
    //    re-applied on the next show load; issue 14).
    SystemConnection* mapped = mappedSystemFor(oldName);
    SystemConnection* selected =
        (m_EditSystem >= 0 && m_EditSystem < static_cast<int>(m_Systems.size()))
            ? m_Systems[static_cast<size_t>(m_EditSystem)].get()
            : nullptr;
    if (mapped != nullptr && selected != nullptr)
    {
        const Id liveId = liveDisplayIdFor(mapped, oldName);
        nlohmann::json result;
        std::string error;
        if (selected != mapped)
        {
            // Move: recreate the display on the selected system with the
            // dialog's values, then remove it from the mapped system.
            // Create first: if the add fails the source display is
            // untouched; if the remove fails afterwards, the error says
            // exactly which side now holds the display.
            if (!selected->send("display.add",
                                {{"x", de.x}, {"y", de.y}, {"width", de.width},
                                 {"height", de.height}, {"name", de.name}},
                                result, error))
            {
                m_EditError = "display.add on '" + selected->name() + "' failed: " + error;
                return false;
            }
            const Id newId = static_cast<Id>(result.value("id", 0u));
            // display.add does not carry brightness/enabled/flags: apply
            // the dialog's values to the recreated display.
            if (!selected->send("display.set",
                                {{"display", newId}, {"brightness", de.brightness},
                                 {"enabled", de.enabled}, {"alwaysOnTop", de.alwaysOnTop},
                                 {"statusOverlay", de.statusOverlay}, {"vsync", de.vsync}},
                                result, error))
            {
                m_EditError = "display.set on '" + selected->name() +
                              "' after the move failed: " + error;
                return false;
            }
            if (liveId != kInvalidId &&
                !mapped->send("display.remove", {{"display", liveId}}, result, error))
            {
                m_EditError = "display added on '" + selected->name() +
                              "' but the remove on '" + mapped->name() + "' failed: " + error;
                return false;
            }
            log("moved display '" + de.name + "' from '" + mapped->name() + "' to " +
                selected->name() + " (live id " + std::to_string(newId) + ")");
        }
        else if (liveId != kInvalidId)
        {
            if (!mapped->send("display.set",
                              {{"display", liveId}, {"name", de.name},
                               {"x", de.x}, {"y", de.y},
                               {"width", de.width}, {"height", de.height},
                               {"brightness", de.brightness}, {"enabled", de.enabled},
                               {"alwaysOnTop", de.alwaysOnTop},
                               {"statusOverlay", de.statusOverlay}, {"vsync", de.vsync}},
                              result, error))
            {
                m_EditError = "display.set on '" + mapped->name() + "' failed: " + error;
                return false;
            }
            log("display '" + de.name + "' settings applied on " + mapped->name());
        }
    }
    // Unmapped (no system runs this display): the document write above is
    // the whole change; the wall takes effect on the next show push.
    m_EditModal = false;
    return true;
}

void ControlGuiApp::openSystemSettings(int index)
{
    if (index < 0 || index >= static_cast<int>(m_Systems.size()))
    {
        return;
    }
    // The same fields the show file's `systems` list stores (issue 16): the
    // entry's identity and address.
    const SystemConnection& conn = *m_Systems[static_cast<size_t>(index)];
    m_SysSettings.name = conn.name();
    m_SysSettings.host = conn.host();
    m_SysSettings.port = static_cast<int>(conn.port());
    m_SysSettings.error.clear();
    m_SysSettingsIndex = index;
    m_SysSettingsModal = true;
    ImGui::OpenPopup(modalPopupId("System settings"));
}

bool ControlGuiApp::applySystemSettings()
{
    if (m_SysSettingsIndex < 0 || m_SysSettingsIndex >= static_cast<int>(m_Systems.size()))
    {
        return false;
    }
    if (m_SysSettings.name.empty() || m_SysSettings.host.empty() || m_SysSettings.port < 1 ||
        m_SysSettings.port > 65535)
    {
        m_SysSettings.error = "needs a name, a host and a port (1-65535)";
        return false;
    }
    auto& conn = *m_Systems[static_cast<size_t>(m_SysSettingsIndex)];
    const bool wasActive = conn.active();
    conn.retarget(m_SysSettings.name, m_SysSettings.host,
                  static_cast<std::uint16_t>(m_SysSettings.port));
    log("system settings applied: " + m_SysSettings.name + " (" + m_SysSettings.host + ":" +
        std::to_string(m_SysSettings.port) + ")" + (wasActive ? "; reconnecting" : ""));
    m_SysSettingsModal = false;
    return true;
}

void ControlGuiApp::openMediaDetails(const json& media, SystemConnection* target)
{
    if (target == nullptr)
    {
        return;
    }
    m_MediaDetailsTarget = target;
    m_MediaDetailsId = static_cast<Id>(media.value("id", 0u));
    m_MediaDetailsModal = true;
    ImGui::OpenPopup(modalPopupId("Media details"));
}

void ControlGuiApp::openWarpEdit(Id docDisplayId)
{
    const ShowDisplay* sd = nullptr;
    for (const auto& d : m_Editor.show().displays)
    {
        if (d.id == docDisplayId)
        {
            sd = &d;
            break;
        }
    }
    if (sd == nullptr)
    {
        return; // the entry went away (removed by another edit)
    }
    WarpEdit& we = m_WarpEdits[docDisplayId];
    // Open the editor on the document entry's warp: the show file's wall is
    // what gets pushed to the systems (the engine applies it on show load).
    const WarpMap& warp = sd->warp;
    we.n = warp.rows();
    we.spline = warp.interpolation() == WarpInterpolation::Spline;
    we.points = warp.pointPositions();
    // The node pad is a square; the warp the engine accepts is square too
    // (display.warp.set), so anything else degrades to the 2x2 identity.
    we.valid = warp.cols() == we.n && we.n >= 2 &&
               static_cast<int>(we.points.size()) == we.n * we.n;
    if (!we.valid)
    {
        we.n = 2;
        we.spline = false;
        we.points = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
        we.valid = true;
    }
    m_WarpDragNode = -1;
    // The live preview target: the system that runs a same-named display
    // (nullptr when the entry is unmapped - Apply then only edits the show).
    m_WarpTarget = mappedSystemFor(sd->name);
    m_WarpDisplayId = docDisplayId;
    m_WarpError.clear();
    m_WarpModal = true;
    ImGui::OpenPopup(modalPopupId("Projection warp"));
}

void ControlGuiApp::renderDisplaysBlock()
{
    // The wall is the show document's display list (the persistence target
    // for display settings): listed and editable with no system connected.
    // "Add display" works offline - the entry is a document edit, the
    // auto-push propagates it to connected systems, and the engine builds
    // the wall on the next show load.
    Show& show = m_Editor.show();
    if (ImGui::Button("Add display"))
    {
        addShowDisplay();
    }
    if (show.displays.empty())
    {
        ImGui::TextDisabled("(none - Add display defines the show's wall)");
        return;
    }

    // Bulk enable/disable for the whole wall (document edits; the
    // auto-push propagates them). The per-display toggle is the "on"
    // checkbox in the table below.
    if (ImGui::Button("Enable all##dispenall"))
    {
        for (auto& d : show.displays)
        {
            d.enabled = true;
        }
        m_Editor.touch();
    }
    ImGui::SameLine();
    if (ImGui::Button("Disable all##dispenoff"))
    {
        for (auto& d : show.displays)
        {
            d.enabled = false;
        }
        m_Editor.touch();
    }

    // One row per document display. The System column shows the connected
    // system that currently runs a same-named live display ("unmapped"
    // when none does); the on/overlay checkboxes edit the document entry
    // and apply live to the mapped system at once; Edit/Warp open the
    // settings and warp dialogs for the entry.
    if (ImGui::BeginTable("displays", 11,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersOuter))
    {
        ImGui::TableSetupColumn("name");
        ImGui::TableSetupColumn("system", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("x", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("y", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("w", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("h", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("brightness", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("overlay", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 196.0f);
        for (auto& d : show.displays)
        {
            // The live mapping: the first connected system running a
            // same-named display (the wall entry's "System" column).
            SystemConnection* mapped = mappedSystemFor(d.name);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(d.name.c_str());
            ImGui::TableNextColumn();
            if (mapped != nullptr)
            {
                ImGui::TextUnformatted(mapped->name().c_str());
            }
            else
            {
                ImGui::TextDisabled("unmapped");
            }
            ImGui::TableNextColumn();
            ImGui::Text("%d", d.x);
            ImGui::TableNextColumn();
            ImGui::Text("%d", d.y);
            ImGui::TableNextColumn();
            ImGui::Text("%d", d.width);
            ImGui::TableNextColumn();
            ImGui::Text("%d", d.height);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", static_cast<double>(d.brightness));
            ImGui::TableNextColumn();
            // Enabled (document value; applied live to the mapped system
            // at once, so a connected wall reacts immediately).
            if (ImGui::Checkbox(("##enabledisp" + std::to_string(d.id)).c_str(), &d.enabled))
            {
                m_Editor.touch();
                if (mapped != nullptr)
                {
                    const Id liveId = liveDisplayIdFor(mapped, d.name);
                    if (liveId != kInvalidId)
                    {
                        std::string error;
                        nlohmann::json result;
                        if (!mapped->send("display.set",
                                          {{"display", liveId}, {"enabled", d.enabled}}, result,
                                          error))
                        {
                            log("display.set (enabled) on " + mapped->name() + " failed: " + error);
                        }
                    }
                }
            }
            ImGui::TableNextColumn();
            // Per-display status overlay (system IP, display name/number,
            // measured fps, resolution and frame timing drawn by the
            // engine; core overlay/StatusOverlay).
            if (ImGui::Checkbox(("##overlaydisp" + std::to_string(d.id)).c_str(),
                                &d.statusOverlay))
            {
                m_Editor.touch();
                if (mapped != nullptr)
                {
                    const Id liveId = liveDisplayIdFor(mapped, d.name);
                    if (liveId != kInvalidId)
                    {
                        std::string error;
                        nlohmann::json result;
                        if (!mapped->send("display.set",
                                          {{"display", liveId},
                                           {"statusOverlay", d.statusOverlay}},
                                          result, error))
                        {
                            log("display.set (overlay) on " + mapped->name() + " failed: " +
                                error);
                        }
                    }
                }
            }
            ImGui::TableNextColumn();
            if (ImGui::Button(("Edit##editdisp" + std::to_string(d.id)).c_str()))
            {
                openDisplayEdit(d.id);
            }
            ImGui::SameLine();
            if (ImGui::Button(("Warp##warpdisp" + std::to_string(d.id)).c_str()))
            {
                openWarpEdit(d.id);
            }
            ImGui::SameLine();
            if (ImGui::Button(("Remove##removedisp" + std::to_string(d.id)).c_str()))
            {
                removeShowDisplay(d.id);
            }
        }
        ImGui::EndTable();
    }
}

void ControlGuiApp::renderMediaBlock(const SystemConnection::Snapshot& snap)
{
    if (snap.status.contains("media"))
    {
        if (ImGui::BeginTable("media", 7,
                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersOuter))
        {
            ImGui::TableSetupColumn("##id", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("type");
            ImGui::TableSetupColumn("fps", ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("duration", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("path");
            ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 122.0f);
            for (const auto& m : snap.status["media"])
            {
                const Id id = static_cast<Id>(m.value("id", 0u));
                ImGui::PushID(static_cast<int>(id));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", id);
                ImGui::TableNextColumn();
                const std::string type = m.value("type", "?");
                ImGui::TextUnformatted(type.c_str());
                ImGui::TableNextColumn();
                // The frame rate only exists for video entries (probe result
                // of the loaded file); images/audio have none.
                if (type == "video" && m.contains("frameRate"))
                {
                    ImGui::TextUnformatted(m["frameRate"].get<std::string>().c_str());
                }
                else
                {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableNextColumn();
                ImGui::Text("%dx%d", static_cast<int>(m.value("width", 0)),
                            static_cast<int>(m.value("height", 0)));
                ImGui::TableNextColumn();
                ImGui::Text("%.2fs", m.value("duration", 0.0));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(truncate(m.value("path", ""), 80).c_str());
                ImGui::TableNextColumn();
                if (ImGui::Button("Details##det"))
                {
                    openMediaDetails(m, activeConnection());
                }
                ImGui::SameLine();
                if (ImGui::Button("Unload##un"))
                {
                    std::string error;
                    sendActive("media.unload", {{"id", id}}, error);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    else
    {
        ImGui::TextDisabled("(none)");
    }
    ImGui::InputTextWithHint("##mediapath",
                             "media file path, relative to the engine (e.g. media/clip.mp4)",
                             &m_MediaPathInput);
    ImGui::SameLine();
    if (ImGui::Button("Load##media"))
    {
        std::string error;
        sendActive("media.load", {{"path", m_MediaPathInput}}, error);
    }
}

// --- show view window (2D render) -----------------------------------------------------

namespace
{

// The scene's display / layer record for an id (nullptr when the item left
// the scene between the selection and the next snapshot).
const ShowViewDisplay* findShowViewDisplay(const ShowViewScene& scene, std::uint32_t id)
{
    for (const auto& d : scene.displays)
    {
        if (d.id == id)
        {
            return &d;
        }
    }
    return nullptr;
}

const ShowViewLayer* findShowViewLayer(const ShowViewScene& scene, std::uint32_t id)
{
    for (const auto& l : scene.layers)
    {
        if (l.id == id)
        {
            return &l;
        }
    }
    return nullptr;
}

// True when (wx, wy) is inside the convex quad (corners TL, TR, BR, BL in
// consistent order): the sign test on the four edge cross products.
bool pointInShowViewQuad(float wx, float wy, const float corners[8])
{
    int sign = 0;
    for (int i = 0; i < 4; ++i)
    {
        const int j = (i + 1) % 4;
        const float ax = corners[static_cast<std::size_t>(2 * i)];
        const float ay = corners[static_cast<std::size_t>(2 * i + 1)];
        const float bx = corners[static_cast<std::size_t>(2 * j)];
        const float by = corners[static_cast<std::size_t>(2 * j + 1)];
        const float cross = (bx - ax) * (wy - ay) - (by - ay) * (wx - ax);
        const int s = (cross > 0.f) ? 1 : (cross < 0.f ? -1 : 0);
        if (s != 0)
        {
            if (sign == 0)
            {
                sign = s;
            }
            else if (sign != s)
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace

void ControlGuiApp::initShowView(VkDevice device, VkPhysicalDevice physicalDevice)
{
    std::string error;
    if (!m_ShowTexPool.init(device, physicalDevice, error))
    {
        log("show view: " + error);
    }
}

void ControlGuiApp::shutdownShowView()
{
    m_ShowTexPool.shutdown();
}

void ControlGuiApp::flushShowTextureUploads(VkCommandBuffer cmd)
{
    m_ShowTexPool.flushUploads(cmd);
    if (showUploadHook)
    {
        showUploadHook(cmd); // the self-test's thumbnail readback
    }
}

void ControlGuiApp::fitShowViewCamera(const ImVec2& childSize, const std::string& signature)
{
    // Fit the union of ALL displays (disabled ones included - the operator
    // wants to see the whole wall) with ~10% margin, centered. An unknown
    // wall shows a default 1920x1080 box so the view is never empty.
    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
    bool have = false;
    for (const auto& d : m_ShowScene.displays)
    {
        if (d.w <= 0.f || d.h <= 0.f)
        {
            continue;
        }
        if (!have)
        {
            minX = d.x;
            minY = d.y;
            maxX = d.x + d.w;
            maxY = d.y + d.h;
            have = true;
        }
        else
        {
            minX = std::min(minX, d.x);
            minY = std::min(minY, d.y);
            maxX = std::max(maxX, d.x + d.w);
            maxY = std::max(maxY, d.y + d.h);
        }
    }
    const float spanX = have ? maxX - minX : 1920.f;
    const float spanY = have ? maxY - minY : 1080.f;
    const float centerX = have ? (minX + maxX) * 0.5f : 960.f;
    const float centerY = have ? (minY + maxY) * 0.5f : 540.f;
    const float fitW = spanX * 1.2f; // span + 10% margin per side
    const float fitH = spanY * 1.2f;
    if (childSize.x <= 0.f || childSize.y <= 0.f || fitW <= 0.f || fitH <= 0.f)
    {
        m_ShowCam.scale = 0.25f;
        m_ShowCam.offsetX = 0.f;
        m_ShowCam.offsetY = 0.f;
    }
    else
    {
        m_ShowCam.scale = std::min(childSize.x / fitW, childSize.y / fitH);
        m_ShowCam.offsetX = childSize.x * 0.5f - centerX * m_ShowCam.scale;
        m_ShowCam.offsetY = childSize.y * 0.5f - centerY * m_ShowCam.scale;
    }
    m_ShowCam.fitted = true;
    m_ShowFittedSignature = signature;
}

void ControlGuiApp::renderShowViewWindow()
{
    m_ShowViewEditPopupOpen = false;
    // The scene fills the window content edge to edge (NoBackground); the
    // mode radios, the "Reset view" button and the selection readout are
    // the only widgets over it.
    const bool visible =
        ImGui::Begin("Show view", nullptr, ImGuiWindowFlags_NoBackground);
    m_ShowViewVisible = false;
    if (!visible)
    {
        ImGui::End();
        return;
    }
    m_ShowViewVisible = true;

    // The top-right control strip: [Display][Layer][Show]  [Reset view],
    // right-aligned. It sits one row below the content top - at y=8 the old
    // Reset button sat under the docked window's title bar (issue: the
    // centralise button was clipped). Computed now so the scene input
    // below can ignore the strip's rect.
    const ImVec2 winSize = ImGui::GetWindowSize();
    const ImVec2 contentOrigin = ImGui::GetCursorScreenPos();
    const float frameH = ImGui::GetFrameHeight();
    const float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
    const auto widgetWidth = [](const char* text) {
        return ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.f;
    };
    const float wReset = widgetWidth("Reset view");
    const float wShow = widgetWidth("Show");
    const float wLayer = widgetWidth("Layer");
    const float wDisplay = widgetWidth("Display");
    const float stripRight = winSize.x - 8.f;
    const float stripY = frameH + 8.f;
    const float xReset = stripRight - wReset;
    const float xShow = xReset - itemSpacing - wShow;
    const float xLayer = xShow - itemSpacing - wLayer;
    const float xDisplay = xLayer - itemSpacing - wDisplay;
    const ImVec2 stripMin(contentOrigin.x + xDisplay, contentOrigin.y + stripY);
    const ImVec2 stripMax(contentOrigin.x + stripRight, contentOrigin.y + stripY + frameH);

    SystemConnection* active = activeConnection();
    const SystemConnection::Snapshot snap =
        active ? active->snapshot() : SystemConnection::Snapshot{};
    m_ShowScene = buildShowViewScene(snap);
    const std::string signature = m_ShowScene.displaySignature();

    // Drop a selection whose item left the scene (the engine removed it,
    // or the active system switched).
    if (m_ShowViewSelDisplay != kInvalidId &&
        findShowViewDisplay(m_ShowScene, m_ShowViewSelDisplay) == nullptr)
    {
        m_ShowViewSelDisplay = kInvalidId;
    }
    if (m_ShowViewSelLayer != kInvalidId &&
        findShowViewLayer(m_ShowScene, m_ShowViewSelLayer) == nullptr)
    {
        m_ShowViewSelLayer = kInvalidId;
    }

    if (!ImGui::BeginChild("##showscene", ImVec2(0.0f, 0.0f)))
    {
        ImGui::EndChild();
        ImGui::End();
        return;
    }
    const ImVec2 childMin = ImGui::GetCursorScreenPos();
    const ImVec2 childSize = ImGui::GetContentRegionAvail();
    m_ShowViewRectMin = childMin;
    m_ShowViewRectMax = ImVec2(childMin.x + childSize.x, childMin.y + childSize.y);

    // Request the thumbnails of the media layers (the poll thread fetches
    // them; the request is idempotent with backoff inside the connection)
    // and drop the textures of media that left the scene.
    std::vector<std::uint32_t> liveMedia;
    if (active && active->pollConnected() && !m_ShowScene.layers.empty())
    {
        std::map<std::uint32_t, std::string> mediaPaths;
        if (snap.status.contains("media") && snap.status["media"].is_array())
        {
            for (const auto& m : snap.status["media"])
            {
                mediaPaths[static_cast<std::uint32_t>(m.value("id", 0u))] =
                    m.value("path", std::string());
            }
        }
        for (const auto& layer : m_ShowScene.layers)
        {
            if (layer.mediaId == 0)
            {
                continue;
            }
            liveMedia.push_back(layer.mediaId);
            const auto it = mediaPaths.find(layer.mediaId);
            if (it != mediaPaths.end())
            {
                active->requestThumbnail(layer.mediaId, it->second);
            }
        }
    }
    if (m_ShowTexPool.ready())
    {
        m_ShowTexPool.evictAllExcept(liveMedia);
    }

    // Camera: fit on the first frame of a system, and again whenever the
    // display wall changes (a display moved or the system switched). User
    // zoom/pan is transient: it survives until the next such change. The
    // refit is held off while a drag is in flight (the drag's own
    // display.set moves the wall, and refitting would fight it) and for a
    // short settle after it (the wall's signature keeps updating for a
    // poll period after the release).
    const bool dragSettling =
        m_ShowDragEndTime != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() < m_ShowDragEndTime + std::chrono::seconds(2);
    if (!m_ShowCam.fitted ||
        (signature != m_ShowFittedSignature && !m_ShowDragActive && !dragSettling))
    {
        fitShowViewCamera(childSize, signature);
    }

    // Draw the scene on the child's draw list (auto-clipped to the child
    // rect; no manual scissor needed). World = output-space pixels (y
    // down); screen = childMin + world * scale + offset.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = childMin;
    auto project = [this, origin](float wx, float wy) -> ImVec2 {
        return ImVec2(origin.x + wx * m_ShowCam.scale + m_ShowCam.offsetX,
                      origin.y + wy * m_ShowCam.scale + m_ShowCam.offsetY);
    };
    // The inverse of project (sx/sy are child-relative screen px): the
    // grid below draws world-space lines across the visible extent.
    auto unproject = [this](float sx, float sy) -> ImVec2 {
        return ImVec2((sx - m_ShowCam.offsetX) / m_ShowCam.scale,
                      (sy - m_ShowCam.offsetY) / m_ShowCam.scale);
    };
    dl->AddRectFilled(childMin, m_ShowViewRectMax,
                      ImGui::ColorConvertFloat4ToU32(ImVec4(0.10f, 0.10f, 0.12f, 1.0f)));

    // The faint world-space grid: the step adapts to the zoom so the lines
    // stay at least ~24 px apart on screen; the origin lines (x=0, y=0)
    // are the reference and are drawn brighter and thicker.
    if (m_ShowCam.scale > 0.0f)
    {
        const ImVec2 wMin = unproject(0.0f, 0.0f);
        const ImVec2 wMax = unproject(childSize.x, childSize.y);
        static const float kGridSteps[] = {25.f, 50.f, 100.f, 250.f, 500.f, 1000.f, 2000.f,
                                           5000.f, 10000.f, 25000.f, 100000.f};
        float step = kGridSteps[std::size(kGridSteps) - 1];
        for (float s : kGridSteps)
        {
            if (s * m_ShowCam.scale >= 24.f)
            {
                step = s;
                break;
            }
        }
        const ImU32 gridFaint = ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, 1.f, 1.f, 0.07f));
        const ImU32 gridOrigin = ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, 1.f, 1.f, 0.28f));
        // The step choice already bounds the line count; the caps are a
        // safety net for a degenerate camera.
        if ((wMax.x - wMin.x) / step <= 200.f)
        {
            for (float gx = std::floor(wMin.x / step) * step; gx <= wMax.x; gx += step)
            {
                const bool isOrigin = std::fabs(gx) < step * 0.01f;
                dl->AddLine(project(gx, wMin.y), project(gx, wMax.y),
                            isOrigin ? gridOrigin : gridFaint, isOrigin ? 2.0f : 1.0f);
            }
        }
        if ((wMax.y - wMin.y) / step <= 200.f)
        {
            for (float gy = std::floor(wMin.y / step) * step; gy <= wMax.y; gy += step)
            {
                const bool isOrigin = std::fabs(gy) < step * 0.01f;
                dl->AddLine(project(wMin.x, gy), project(wMax.x, gy),
                            isOrigin ? gridOrigin : gridFaint, isOrigin ? 2.0f : 1.0f);
            }
        }
    }

    // The display wall. The selected display is highlighted; its label -
    // name, resolution and the system it is on - shows only while selected.
    const ImU32 selCol = ImGui::ColorConvertFloat4ToU32(ImVec4(0.25f, 0.75f, 1.0f, 1.0f));
    for (const auto& d : m_ShowScene.displays)
    {
        if (d.w <= 0.f || d.h <= 0.f)
        {
            continue;
        }
        const ImVec2 p1 = project(d.x, d.y);
        const ImVec2 p2 = project(d.x + d.w, d.y + d.h);
        const bool selected = d.id == m_ShowViewSelDisplay;
        const ImU32 col = selected ? selCol
                                   : d.enabled ? ImGui::GetColorU32(ImGuiCol_Text)
                                               : ImGui::GetColorU32(ImGuiCol_TextDisabled);
        dl->AddRect(p1, p2, col, 0.0f, ImDrawFlags_None,
                    selected ? 3.0f : (d.enabled ? 2.0f : 1.0f));
        if (selected)
        {
            std::string label = d.name + "  \u00b7  " + std::to_string(static_cast<int>(d.w)) +
                                "x" + std::to_string(static_cast<int>(d.h));
            const std::string systemName = active ? active->name() : std::string();
            if (!systemName.empty())
            {
                label += "  \u00b7  " + systemName;
            }
            dl->AddText(ImVec2(p1.x + 4.f, p1.y + 2.f), selCol, label.c_str());
        }
    }

    // The visible layers (in draw order). The selected layer gets the
    // highlight outline; its label shows only while selected.
    for (const auto& layer : m_ShowScene.layers)
    {
        ImVec2 c[4];
        for (int i = 0; i < 4; ++i)
        {
            c[i] = project(layer.corners[static_cast<size_t>(2 * i)],
                           layer.corners[static_cast<size_t>(2 * i + 1)]);
        }
        const bool selected = layer.id == m_ShowViewSelLayer;
        const ImU32 outline = selected ? selCol : ImGui::GetColorU32(ImGuiCol_TextDisabled);
        if (layer.content == "color")
        {
            const ImU32 fill = ImGui::ColorConvertFloat4ToU32(ImVec4(
                layer.color[0], layer.color[1], layer.color[2],
                layer.color[3] * layer.opacity));
            dl->AddQuadFilled(c[0], c[1], c[2], c[3], fill);
            dl->AddQuad(c[0], c[1], c[2], c[3], outline, selected ? 2.5f : 1.0f);
        }
        else if (m_ShowTexPool.ready())
        {
            // Media: the thumbnail texture (a white fallback until it
            // arrives). The pool replaces the texture on a path/size change
            // and re-uploads on new pixels.
            ImTextureRef tex = m_ShowTexPool.whiteTexture();
            int tw = 0;
            int th = 0;
            std::vector<std::uint8_t> rgba;
            if (active && active->thumbnailFor(layer.mediaId, tw, th, rgba))
            {
                std::string path;
                if (snap.status.contains("media") && snap.status["media"].is_array())
                {
                    for (const auto& m : snap.status["media"])
                    {
                        if (m.value("id", 0u) == layer.mediaId)
                        {
                            path = m.value("path", std::string());
                            break;
                        }
                    }
                }
                tex = m_ShowTexPool.textureFor(layer.mediaId, path, rgba, tw, th);
            }
            const ImU32 tint =
                ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, 1.f, 1.f, layer.opacity));
            dl->PushTextureID(tex);
            dl->PrimQuadUV(c[0], c[1], c[2], c[3], ImVec2(0.f, 0.f), ImVec2(1.f, 0.f),
                           ImVec2(1.f, 1.f), ImVec2(0.f, 1.f), tint);
            dl->PopTextureID();
            dl->AddQuad(c[0], c[1], c[2], c[3], outline, selected ? 2.5f : 1.0f);
            if (layer.content == "video")
            {
                // Play badge: screen-constant (a world-space size would
                // zoom with the camera), at the projected top-left corner.
                const ImVec2 b(c[0].x + 5.f, c[0].y + 5.f);
                dl->AddTriangleFilled(b, ImVec2(b.x + 11.f, b.y + 7.f),
                                      ImVec2(b.x + 5.f, b.y + 14.f),
                                      ImGui::GetColorU32(ImGuiCol_Text));
            }
        }
        else
        {
            // The pool is not ready (init failure): a neutral quad keeps the
            // layer visible.
            dl->AddQuadFilled(c[0], c[1], c[2], c[3],
                              ImGui::ColorConvertFloat4ToU32(
                                  ImVec4(0.5f, 0.5f, 0.5f, layer.opacity)));
            dl->AddQuad(c[0], c[1], c[2], c[3], outline, selected ? 2.5f : 1.0f);
        }
        if (selected && !layer.name.empty())
        {
            dl->AddText(ImVec2(c[0].x + 3.f, c[0].y + 2.f), selCol, layer.name.c_str());
        }
    }
    ImGui::EndChild();

    // The top-right control strip: the view-mode radios + Reset view.
    // Rendered before the scene input below so a press on them is already
    // in ImGui's active-id state when the input block runs.
    ImGui::SetCursorPos(ImVec2(xDisplay, stripY));
    if (ImGui::RadioButton("Display##showmode", m_ShowViewMode == ShowViewMode::Display))
    {
        m_ShowViewMode = ShowViewMode::Display;
    }
    ImGui::SetCursorPos(ImVec2(xLayer, stripY));
    if (ImGui::RadioButton("Layer##showmode", m_ShowViewMode == ShowViewMode::Layer))
    {
        m_ShowViewMode = ShowViewMode::Layer;
    }
    ImGui::SetCursorPos(ImVec2(xShow, stripY));
    if (ImGui::RadioButton("Show##showmode", m_ShowViewMode == ShowViewMode::Show))
    {
        m_ShowViewMode = ShowViewMode::Show;
    }
    ImGui::SetCursorPos(ImVec2(xReset, stripY));
    if (ImGui::Button("Reset view##showreset"))
    {
        fitShowViewCamera(childSize, signature);
    }

    // Scene input: wheel-zoom around the cursor, middle-drag pan, and the
    // left button for selection/drag (the mode decides what a click
    // selects; a drag moves the selected item live). The strip's rect is
    // excluded, and everything is ignored while any widget owns the mouse
    // or a popup/modal is open (their click closes or drives them).
    {
        const ImGuiIO& io = ImGui::GetIO();
        // With ConfigWindowsMoveFromTitleBarOnly, a left press on a
        // window's empty space makes ImGui grab the window's MoveId
        // (StartMouseMovingWindow() sets the ActiveId, then cancels the
        // move because the press is outside the title bar). The window
        // does not move, but the grab keeps IsAnyItemActive() true while
        // the button is held: it must not block the scene's own left
        // drag. Only a real widget (a different ActiveId) blocks it.
        bool mouseFree = !ImGui::IsAnyItemActive();
        ImGuiContext& ctx = *ImGui::GetCurrentContext();
        if (!mouseFree && ctx.ActiveIdWindow != nullptr &&
            ctx.ActiveId == ctx.ActiveIdWindow->MoveId)
        {
            mouseFree = true;
        }
        const bool popupOpen = ctx.OpenPopupStack.Size > 0;
        const bool inScene =
            ImGui::IsMouseHoveringRect(m_ShowViewRectMin, m_ShowViewRectMax) &&
            !ImGui::IsMouseHoveringRect(stripMin, stripMax);
        if (m_ShowCam.scale > 0.0f && mouseFree && !popupOpen && inScene)
        {
            if (io.MouseWheel != 0.0f)
            {
                const ImVec2 mouse = ImGui::GetMousePos();
                const float wx = (mouse.x - childMin.x - m_ShowCam.offsetX) / m_ShowCam.scale;
                const float wy = (mouse.y - childMin.y - m_ShowCam.offsetY) / m_ShowCam.scale;
                m_ShowCam.scale =
                    ImClamp(m_ShowCam.scale * std::pow(1.15f, io.MouseWheel), 0.01f, 32.0f);
                m_ShowCam.offsetX = mouse.x - childMin.x - wx * m_ShowCam.scale;
                m_ShowCam.offsetY = mouse.y - childMin.y - wy * m_ShowCam.scale;
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
            {
                m_ShowCam.offsetX += io.MouseDelta.x;
                m_ShowCam.offsetY += io.MouseDelta.y;
            }

            const ImVec2 mouse = ImGui::GetMousePos();
            const float wx = (mouse.x - childMin.x - m_ShowCam.offsetX) / m_ShowCam.scale;
            const float wy = (mouse.y - childMin.y - m_ShowCam.offsetY) / m_ShowCam.scale;

            if (m_ShowViewMode != ShowViewMode::Show)
            {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    // Hit-test: the topmost (last-drawn) item under the
                    // cursor; a hit on nothing clears the selection.
                    const ShowViewDisplay* hitDisplay = nullptr;
                    const ShowViewLayer* hitLayer = nullptr;
                    if (m_ShowViewMode == ShowViewMode::Display)
                    {
                        for (auto it = m_ShowScene.displays.rbegin();
                             it != m_ShowScene.displays.rend(); ++it)
                        {
                            if (it->w > 0.f && it->h > 0.f && wx >= it->x &&
                                wx <= it->x + it->w && wy >= it->y && wy <= it->y + it->h)
                            {
                                hitDisplay = &*it;
                                break;
                            }
                        }
                    }
                    else
                    {
                        for (auto it = m_ShowScene.layers.rbegin();
                             it != m_ShowScene.layers.rend(); ++it)
                        {
                            if (pointInShowViewQuad(wx, wy, it->corners))
                            {
                                hitLayer = &*it;
                                break;
                            }
                        }
                    }
                    m_ShowViewSelDisplay = (hitDisplay != nullptr) ? hitDisplay->id : kInvalidId;
                    m_ShowViewSelLayer = (hitLayer != nullptr) ? hitLayer->id : kInvalidId;
                    // Arm a live drag of the hit item (the press point is
                    // the origin; IsMouseDragging gates the first move).
                    if (hitDisplay != nullptr)
                    {
                        m_ShowDragActive = true;
                        m_ShowDragIsDisplay = true;
                        m_ShowDragId = hitDisplay->id;
                        m_ShowDragStartWX = wx;
                        m_ShowDragStartWY = wy;
                        m_ShowDragOrigX = hitDisplay->x;
                        m_ShowDragOrigY = hitDisplay->y;
                    }
                    else if (hitLayer != nullptr)
                    {
                        m_ShowDragActive = true;
                        m_ShowDragIsDisplay = false;
                        m_ShowDragId = hitLayer->id;
                        m_ShowDragStartWX = wx;
                        m_ShowDragStartWY = wy;
                        m_ShowDragOrigX = hitLayer->x;
                        m_ShowDragOrigY = hitLayer->y;
                    }
                    else
                    {
                        m_ShowDragActive = false;
                    }
                }
                else if (m_ShowDragActive &&
                         ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    // The live move: the position at the press plus the
                    // world delta since (the transform position is a pure
                    // final translation, so the delta shifts the quad
                    // regardless of rotation/anchor/scale).
                    const float dx = wx - m_ShowDragStartWX;
                    const float dy = wy - m_ShowDragStartWY;
                    std::string error;
                    if (m_ShowDragIsDisplay)
                    {
                        sendActive(
                            "display.set",
                            {{"display", m_ShowDragId},
                             {"x", static_cast<std::int32_t>(std::llround(m_ShowDragOrigX + dx))},
                             {"y", static_cast<std::int32_t>(std::llround(m_ShowDragOrigY + dy))}},
                            error);
                    }
                    else
                    {
                        sendActive("layer.set",
                                   {{"layer", m_ShowDragId},
                                    {"x", m_ShowDragOrigX + dx},
                                    {"y", m_ShowDragOrigY + dy}},
                                   error);
                    }
                    // The wall signature moves with our own display.set:
                    // mark the camera fitted to it so the auto-refit neither
                    // fights the drag nor jumps on release.
                    m_ShowFittedSignature = signature;
                }
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    if (m_ShowDragActive)
                    {
                        // The drag is over: accept the wall it produced
                        // (no refit jump) and start the settle grace.
                        m_ShowFittedSignature = signature;
                        m_ShowDragEndTime = std::chrono::steady_clock::now();
                    }
                    m_ShowDragActive = false;
                }
            }
            if (m_ShowViewSelDisplay != kInvalidId &&
                ImGui::IsMouseReleased(ImGuiMouseButton_Right))
            {
                // Right release over the selected display: context menu.
                const ShowViewDisplay* sel =
                    findShowViewDisplay(m_ShowScene, m_ShowViewSelDisplay);
                if (sel != nullptr && wx >= sel->x && wx <= sel->x + sel->w &&
                    wy >= sel->y && wy <= sel->y + sel->h)
                {
                    ImGui::OpenPopup("Show view display");
                }
            }
        }
    }

    // The right-click context menu over the selected display.
    if (ImGui::BeginPopup("Show view display"))
    {
        m_ShowViewEditPopupOpen = true;
        if (ImGui::MenuItem("Edit...") && m_ShowViewSelDisplay != kInvalidId)
        {
            const ShowViewDisplay* sel =
                findShowViewDisplay(m_ShowScene, m_ShowViewSelDisplay);
            if (sel != nullptr)
            {
                openDisplayEditForLiveDisplay(*sel);
            }
        }
        ImGui::EndPopup();
    }

    // The top-left readout: what is selected (the display's name,
    // resolution and system - or the layer's name), on a dark chip so it
    // stays readable over the scene. Drawn on the window's draw list after
    // the child, so it sits on top of the scene.
    {
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        std::string text;
        const ShowViewDisplay* selDisplay =
            m_ShowViewSelDisplay != kInvalidId
                ? findShowViewDisplay(m_ShowScene, m_ShowViewSelDisplay)
                : nullptr;
        if (selDisplay != nullptr)
        {
            text = selDisplay->name + "  \u00b7  " +
                   std::to_string(static_cast<int>(selDisplay->w)) + "x" +
                   std::to_string(static_cast<int>(selDisplay->h));
            const std::string systemName = active ? active->name() : std::string();
            if (!systemName.empty())
            {
                text += "  \u00b7  " + systemName;
            }
        }
        else
        {
            const ShowViewLayer* selLayer =
                m_ShowViewSelLayer != kInvalidId
                    ? findShowViewLayer(m_ShowScene, m_ShowViewSelLayer)
                    : nullptr;
            if (selLayer != nullptr)
            {
                text = "'" + selLayer->name + "'";
            }
        }
        if (text.empty())
        {
            text = "No selection";
        }
        const float tsx = contentOrigin.x + 8.f;
        const float tsy = contentOrigin.y + 8.f;
        const ImVec2 tsize = ImGui::CalcTextSize(text.c_str());
        const float pad = 4.f;
        wdl->AddRectFilled(ImVec2(tsx - pad, tsy - pad),
                           ImVec2(tsx + tsize.x + pad, tsy + tsize.y + pad),
                           ImGui::ColorConvertFloat4ToU32(ImVec4(0.f, 0.f, 0.f, 0.55f)));
        wdl->AddText(ImVec2(tsx, tsy), ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
    }
    ImGui::End();
}

// --- show editor window ---------------------------------------------------------------

void ControlGuiApp::renderShowEditorWindow()
{
    // Begin() may return false; End() must still be called (see above).
    if (!ImGui::Begin("Show Editor", nullptr, ImGuiWindowFlags_HorizontalScrollbar))
    {
        ImGui::End();
        return;
    }
    // The editor lives in a narrow docked pane; the file actions (New /
    // Open / Save / Save As) live in the File menu only (issue 19), so the
    // toolbar is just the system actions row.
    SystemConnection* active = activeConnection();
    ImGui::BeginDisabled(!active);
    if (ImGui::Button("Push to system") && active)
    {
        pushToActiveSystem();
    }
    ImGui::SameLine();
    if (ImGui::Button("Pull from system...") && active)
    {
        m_PullModal = true;
        m_PullPath = m_Editor.filePath();
        ImGui::OpenPopup(modalPopupId("Pull show from system"));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    // Auto-push: edits are sent to every connected system (debounced,
    // preserving playback state) without the Push button.
    if (ImGui::Checkbox("Auto-push edits##autopush", &m_AutoPushEdits))
    {
        log(std::string("auto-push edits ") + (m_AutoPushEdits ? "enabled" : "disabled"));
    }

    ImGui::Spacing();
    // The file path can exceed the pane width; wrap instead of clipping.
    const std::string where =
        m_Editor.filePath().empty() ? std::string("(unsaved)") : m_Editor.filePath();
    ImGui::TextWrapped("%s%s", where.c_str(), m_Editor.dirty() ? "  *dirty" : "");
    if (!m_EditorError.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kBadColor);
        ImGui::TextWrapped("%s", m_EditorError.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();

    // The pane is a fixed-size docking slot, so long tab content (many cues,
    // layer details) scrolls instead of being clipped below the pane; the
    // tab bar stays pinned above the scroll region.
    if (ImGui::BeginTabBar("editortabs"))
    {
        if (ImGui::BeginTabItem("Show"))
        {
            if (ImGui::BeginChild("##showtab", ImVec2(0.0f, 0.0f), false))
            {
                renderShowTab();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Media"))
        {
            if (ImGui::BeginChild("##mediatab", ImVec2(0.0f, 0.0f), false))
            {
                renderMediaTab();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Scenes"))
        {
            if (ImGui::BeginChild("##scenestab", ImVec2(0.0f, 0.0f), false))
            {
                renderScenesTab();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Cues"))
        {
            if (ImGui::BeginChild("##cuestab", ImVec2(0.0f, 0.0f), false))
            {
                renderCuesTab();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void ControlGuiApp::renderShowTab()
{
    Show& show = m_Editor.show();
    if (ImGui::InputText("Name", &show.name))
    {
        m_Editor.touch();
    }
    int num = static_cast<int>(show.frameRate.numerator());
    int den = static_cast<int>(show.frameRate.denominator());
    ImGui::InputInt("Frame rate (num)##frn", &num);
    ImGui::SameLine();
    ImGui::InputInt("frame rate (den)##frd", &den);
    if ((num != static_cast<int>(show.frameRate.numerator()) ||
         den != static_cast<int>(show.frameRate.denominator())) &&
        num > 0 && den > 0)
    {
        show.frameRate = FrameRate::of(num, den);
        m_Editor.touch();
    }
    float duration = static_cast<float>(show.duration.toSeconds());
    if (ImGui::InputFloat("Duration (s, 0 = unlimited)", &duration, 1.0f, 5.0f, "%.1f"))
    {
        show.duration = Time::fromSeconds(std::max(0.0, static_cast<double>(duration)));
        m_Editor.touch();
    }
    ImGui::Spacing();
    ImGui::Text("%d scene(s), %d media reference(s), %d cue(s)",
                static_cast<int>(show.scenes.size()), static_cast<int>(show.media.size()),
                static_cast<int>(show.cues.size()));
}

void ControlGuiApp::renderMediaTab()
{
    Show& show = m_Editor.show();
    if (ImGui::BeginTable("editmedia", 4,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersOuter))
    {
        ImGui::TableSetupColumn("##id", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("path");
        ImGui::TableSetupColumn("##probe", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        for (size_t i = 0; i < show.media.size(); ++i)
        {
            MediaReference& ref = show.media[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", ref.id);
            ImGui::TableNextColumn();
            if (ImGui::InputText("##path", &ref.path))
            {
                m_Editor.touch();
            }
            ImGui::TableNextColumn();
            if (ImGui::Button("Probe##probe"))
            {
                // Loads the file on the active system and shows the result.
                json result;
                std::string error;
                if (sendActive("media.load", {{"path", ref.path}}, error, &result))
                {
                    m_MediaProbeResult = "loaded as id " +
                                         std::to_string(static_cast<unsigned>(result.value("id", 0u))) +
                                         " (" + result.value("type", "?") + ", " +
                                         std::to_string(static_cast<int>(result.value("width", 0))) +
                                         "x" + std::to_string(static_cast<int>(result.value("height", 0))) +
                                         ")";
                }
                else
                {
                    m_MediaProbeResult = "probe failed: " + error;
                }
            }
            ImGui::TableNextColumn();
            if (ImGui::Button("x##rm"))
            {
                show.media.erase(show.media.begin() + static_cast<std::ptrdiff_t>(i));
                m_Editor.touch();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Add media reference"))
    {
        MediaReference ref;
        ref.id = ShowEditor::nextMediaId(show);
        ref.path = "media/clip.mp4";
        show.media.push_back(ref);
        m_Editor.touch();
    }
    ImGui::SameLine();
    // Pick existing files with the file dialog (multi-select); the selected
    // paths are stored as-is (they must exist on the systems the show is
    // pushed to).
    if (ImGui::Button("Browse...##mediabrowse"))
    {
        addMediaFileDialog();
    }
    if (!m_MediaProbeResult.empty())
    {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", m_MediaProbeResult.c_str());
    }
}

void ControlGuiApp::renderScenesTab()
{
    Show& show = m_Editor.show();

    ImGui::BeginTable("scenetabs", 2, ImGuiTableFlags_SizingStretchProp);
    ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthFixed, 240.0f);
    ImGui::TableSetupColumn("##ops");
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (ImGui::BeginChild("scenelist", ImVec2(0.0f, 140.0f), true))
    {
        for (int i = 0; i < static_cast<int>(show.scenes.size()); ++i)
        {
            const Scene& scene = show.scenes[static_cast<size_t>(i)];
            const std::string label = std::to_string(scene.id) + " - " + scene.name;
            if (ImGui::Selectable(label.c_str(), m_SelectedSceneIdx == i))
            {
                m_SelectedSceneIdx = i;
            }
        }
        if (show.scenes.empty())
        {
            ImGui::TextDisabled("(no scenes)");
        }
    }
    ImGui::EndChild();
    ImGui::TableNextColumn();
    if (ImGui::Button("Add scene"))
    {
        Scene scene;
        scene.id = ShowEditor::nextSceneId(show);
        scene.name = "Scene " + std::to_string(scene.id);
        show.scenes.push_back(std::move(scene));
        m_SelectedSceneIdx = static_cast<int>(show.scenes.size()) - 1;
        m_Editor.touch();
    }
    ImGui::SameLine();
    const bool hasSel = m_SelectedSceneIdx >= 0 && m_SelectedSceneIdx < static_cast<int>(show.scenes.size());
    if (ImGui::Button("Remove scene") && hasSel)
    {
        show.scenes.erase(show.scenes.begin() + m_SelectedSceneIdx);
        if (m_SelectedSceneIdx >= static_cast<int>(show.scenes.size()))
        {
            m_SelectedSceneIdx = static_cast<int>(show.scenes.size()) - 1;
        }
        m_Editor.touch();
    }
    // Reorder within the list: the list order is the compositing order when
    // several scenes are active at once (first = bottom-most). Cues reference
    // scenes by id, so reordering does not change the timeline.
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasSel || m_SelectedSceneIdx <= 0);
    if (ImGui::Button("Up##sceneup"))
    {
        std::swap(show.scenes[m_SelectedSceneIdx - 1], show.scenes[m_SelectedSceneIdx]);
        --m_SelectedSceneIdx;
        m_Editor.touch();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasSel || m_SelectedSceneIdx + 1 >= static_cast<int>(show.scenes.size()));
    if (ImGui::Button("Down##scenedown"))
    {
        std::swap(show.scenes[m_SelectedSceneIdx], show.scenes[m_SelectedSceneIdx + 1]);
        ++m_SelectedSceneIdx;
        m_Editor.touch();
    }
    ImGui::EndDisabled();
    ImGui::EndTable();

    if (!hasSel)
    {
        ImGui::TextDisabled("Add or select a scene to edit its layers.");
        return;
    }
    Scene& scene = show.scenes[static_cast<size_t>(m_SelectedSceneIdx)];

    ImGui::InputText("Scene name", &scene.name);
    ImGui::SameLine();
    ImGui::Text("id %u", scene.id);
    ImGui::Separator();

    for (size_t j = 0; j < scene.layers.size(); ++j)
    {
        Layer& layer = scene.layers[j];
        char label[160];
        std::snprintf(label, sizeof(label), "Layer %u - %s", layer.id, layer.name.c_str());
        if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_None))
        {
            renderLayerDetails(layer);
            ImGui::TreePop();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(("Remove##layer" + std::to_string(j)).c_str()))
        {
            scene.layers.erase(scene.layers.begin() + static_cast<std::ptrdiff_t>(j));
            m_Editor.touch();
            break;
        }
    }
    if (ImGui::Button("Add layer"))
    {
        Layer layer;
        layer.id = ShowEditor::nextLayerId(show);
        layer.name = "Layer " + std::to_string(layer.id);
        scene.layers.push_back(std::move(layer));
        m_Editor.touch();
    }
}

void ControlGuiApp::renderLayerDetails(Layer& layer)
{
    Show& show = m_Editor.show();
    bool changed = false;

    if (ImGui::InputText("Name##lname", &layer.name))
    {
        changed = true;
    }

    // Media source: solid color or a media reference of this show.
    std::vector<std::string> items;
    std::vector<const char*> itemPtrs;
    int selected = 0;
    items.push_back("solid color (no media)");
    for (const auto& ref : show.media)
    {
        items.push_back(std::to_string(ref.id) + " - " + ref.path);
    }
    for (const auto& s : items)
    {
        itemPtrs.push_back(s.c_str());
    }
    for (size_t i = 0; i < show.media.size(); ++i)
    {
        if (show.media[i].id == layer.mediaId)
        {
            selected = static_cast<int>(i) + 1;
            break;
        }
    }
    if (ImGui::Combo("Media", &selected, itemPtrs.data(), static_cast<int>(itemPtrs.size())))
    {
        layer.mediaId = selected == 0 ? kInvalidId : show.media[static_cast<size_t>(selected - 1)].id;
        changed = true;
    }
    if (layer.mediaId == kInvalidId)
    {
        if (ImGui::ColorEdit4("Color", &layer.color.r))
        {
            changed = true;
        }
    }
    else
    {
        float inPoint = static_cast<float>(layer.inPoint.toSeconds());
        if (ImGui::InputFloat("In point (s)", &inPoint, 0.1f, 0.5f, "%.2f"))
        {
            layer.inPoint = Time::fromSeconds(std::max(0.0, static_cast<double>(inPoint)));
            changed = true;
        }
        ImGui::SameLine();
        float outPoint = static_cast<float>(layer.outPoint.toSeconds());
        if (ImGui::InputFloat("Out point (s, 0 = end)", &outPoint, 0.1f, 0.5f, "%.2f"))
        {
            layer.outPoint = Time::fromSeconds(std::max(0.0, static_cast<double>(outPoint)));
            changed = true;
        }
        if (ImGui::Checkbox("Loop", &layer.loop))
        {
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::InputFloat("Rate", &layer.rate, 0.05f, 0.2f, "%.2f"))
        {
            if (layer.rate <= 0.0f)
            {
                layer.rate = 0.01f; // the engine requires a positive rate
            }
            changed = true;
        }
    }

    ImGui::SeparatorText("Transform");
    if (ImGui::InputFloat("x", &layer.transform.x, 10.0f, 50.0f))
    {
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::InputFloat("y", &layer.transform.y, 10.0f, 50.0f))
    {
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::InputFloat("rotation (rad)", &layer.transform.rotation, 0.01f, 0.05f, "%.3f"))
    {
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::InputFloat("scaleX", &layer.transform.scaleX, 0.01f, 0.05f, "%.2f"))
    {
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::InputFloat("scaleY", &layer.transform.scaleY, 0.01f, 0.05f, "%.2f"))
    {
        changed = true;
    }
    if (ImGui::InputFloat("anchorX (0..1)", &layer.transform.anchorX, 0.01f, 0.05f, "%.2f"))
    {
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::InputFloat("anchorY (0..1)", &layer.transform.anchorY, 0.01f, 0.05f, "%.2f"))
    {
        changed = true;
    }

    ImGui::SeparatorText("Appearance");
    if (ImGui::SliderFloat("Opacity", &layer.opacity, 0.0f, 1.0f, "%.2f"))
    {
        changed = true;
    }
    int blendIdx = static_cast<int>(layer.blend);
    if (ImGui::Combo("Blend", &blendIdx, kBlendModes, kBlendModeCount))
    {
        layer.blend = static_cast<BlendMode>(blendIdx);
        changed = true;
    }
    if (ImGui::Checkbox("Visible", &layer.visible))
    {
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::InputInt("zIndex", &layer.zIndex))
    {
        changed = true;
    }
    if (changed)
    {
        m_Editor.touch();
    }
    renderLayerTracks(layer);
}

void ControlGuiApp::renderLayerTracks(Layer& layer)
{
    struct TrackEntry
    {
        const char* label;
        KeyframeTrack* track;
    };
    const TrackEntry tracks[] = {
        {"opacity", &layer.opacityTrack},
        {"x", &layer.xTrack},
        {"y", &layer.yTrack},
        {"rotation", &layer.rotationTrack},
        {"scale", &layer.scaleTrack},
    };
    for (const auto& entry : tracks)
    {
        KeyframeTrack& track = *entry.track;
        char header[64];
        std::snprintf(header, sizeof(header), "%s (%d keyframe(s))", entry.label, track.keyCount());
        if (!ImGui::CollapsingHeader(header))
        {
            continue;
        }
        ImGui::PushID(entry.label);
        bool trackChanged = false;
        const std::vector<Keyframe>& keys = track.keyframes();
        for (size_t k = 0; k < keys.size(); ++k)
        {
            const Keyframe& kf = keys[k];
            float t = static_cast<float>(kf.time.toSeconds());
            float v = kf.value;
            int interp = kf.interpolation == Interpolation::Step ? 1 : 0;
            ImGui::PushID(static_cast<int>(k));
            if (ImGui::InputFloat("t##t", &t, 0.1f, 0.5f, "%.2f"))
            {
                const double newT = std::max(0.0, static_cast<double>(t));
                if (std::fabs(newT - kf.time.toSeconds()) > 1e-9)
                {
                    track.removeKey(kf.time);
                }
                track.setKey(Time::fromSeconds(newT), v, kf.interpolation);
                trackChanged = true;
            }
            ImGui::SameLine();
            if (ImGui::InputFloat("v##v", &v, 0.05f, 0.2f, "%.2f"))
            {
                track.setKey(kf.time, v, kf.interpolation);
                trackChanged = true;
            }
            ImGui::SameLine();
            if (ImGui::Combo("##i", &interp, kInterpolations, kInterpolationCount))
            {
                track.setKey(kf.time, v, interp == 1 ? Interpolation::Step : Interpolation::Linear);
                trackChanged = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x##rm"))
            {
                track.removeKey(kf.time);
                trackChanged = true;
            }
            ImGui::PopID();
        }
        float addT = 0.0f;
        float addV = 0.0f;
        ImGui::InputFloat("##addt", &addT, 0.1f, 0.5f, "%.2f");
        ImGui::SameLine();
        ImGui::InputFloat("##addv", &addV, 0.05f, 0.2f, "%.2f");
        ImGui::SameLine();
        if (ImGui::Button("Add keyframe"))
        {
            track.setKey(Time::fromSeconds(std::max(0.0, static_cast<double>(addT))), addV, Interpolation::Linear);
            trackChanged = true;
        }
        ImGui::SameLine();
        if (track.keyCount() > 0 && ImGui::Button("Clear track"))
        {
            track.clear();
            trackChanged = true;
        }
        if (trackChanged)
        {
            m_Editor.touch();
        }
        ImGui::PopID();
    }
}

void ControlGuiApp::addCueDefault()
{
    Show& show = m_Editor.show();
    Cue cue;
    // One second after the last cue (or t=0 on an empty timeline):
    // Show::addCue replaces a cue at an existing time, so a default equal
    // to the last cue's time would silently overwrite it instead of adding
    // a row (the button stopped adding cues after the first press).
    cue.time = show.cues.empty() ? Time::zero() : show.cues.back().time + Time::fromSeconds(1.0);
    cue.name = "cue";
    const Id sceneId = show.scenes.empty() ? 1 : show.scenes.front().id;
    cue.action = PlayScene{sceneId};
    show.addCue(std::move(cue));
}

void ControlGuiApp::renderCuesTab()
{
    Show& show = m_Editor.show();
    for (size_t i = 0; i < show.cues.size(); ++i)
    {
        Cue& cue = show.cues[i];
        ImGui::PushID(static_cast<int>(i));
        float t = static_cast<float>(cue.time.toSeconds());
        if (ImGui::InputFloat("##ctime", &t, 0.1f, 0.5f, "%.2f"))
        {
            const Time newTime = Time::fromSeconds(std::max(0.0, static_cast<double>(t)));
            if (newTime != cue.time)
            {
                // The timeline is kept sorted; move the cue by remove + insert.
                Cue copy = cue;
                copy.time = newTime;
                show.cues.erase(show.cues.begin() + static_cast<std::ptrdiff_t>(i));
                show.addCue(std::move(copy));
                m_Editor.touch();
                ImGui::PopID();
                --i;
                continue;
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputText("##cname", &cue.name))
        {
            m_Editor.touch();
        }
        ImGui::SameLine();
        std::string actionName;
        std::visit(
            [&actionName](const auto& a) {
                using T = std::decay_t<decltype(a)>;
                if constexpr (std::is_same_v<T, PlayScene>)
                {
                    actionName = "playScene";
                }
                else if constexpr (std::is_same_v<T, StopPlayback>)
                {
                    actionName = "stop";
                }
                else if constexpr (std::is_same_v<T, PausePlayback>)
                {
                    actionName = "pause";
                }
                else if constexpr (std::is_same_v<T, SeekTo>)
                {
                    actionName = "seek";
                }
                else if constexpr (std::is_same_v<T, SetLayerOpacity>)
                {
                    actionName = "setLayerOpacity";
                }
                else if constexpr (std::is_same_v<T, SetLayerVisible>)
                {
                    actionName = "setLayerVisible";
                }
                else if constexpr (std::is_same_v<T, SetLayerBlend>)
                {
                    actionName = "setLayerBlend";
                }
                else if constexpr (std::is_same_v<T, SetLayerPosition>)
                {
                    actionName = "setLayerPosition";
                }
            },
            cue.action);
        ImGui::Text("%s", actionName.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x##rm"))
        {
            show.cues.erase(show.cues.begin() + static_cast<std::ptrdiff_t>(i));
            m_Editor.touch();
            ImGui::PopID();
            --i;
            continue;
        }
        renderCueAction(cue);
        ImGui::PopID();
        ImGui::Spacing();
    }
    if (ImGui::Button("Add cue"))
    {
        addCueDefault();
        m_Editor.touch();
    }
}

void ControlGuiApp::renderCueAction(Cue& cue)
{
    Show& show = m_Editor.show();

    // Scene picker (for playScene).
    std::vector<Id> sceneIds;
    std::vector<std::string> sceneLabels;
    for (const auto& scene : show.scenes)
    {
        sceneLabels.push_back(std::to_string(scene.id) + " - " + scene.name);
        sceneIds.push_back(scene.id);
    }
    std::vector<const char*> scenePtrs;
    for (const auto& s : sceneLabels)
    {
        scenePtrs.push_back(s.c_str());
    }
    // Layer picker (all layers of all scenes).
    std::vector<Id> layerIds;
    std::vector<std::string> layerLabels;
    for (const auto& scene : show.scenes)
    {
        for (const auto& layer : scene.layers)
        {
            layerLabels.push_back(std::to_string(layer.id) + " - " + layer.name + " (scene " +
                                  std::to_string(scene.id) + ")");
            layerIds.push_back(layer.id);
        }
    }
    std::vector<const char*> layerPtrs;
    for (const auto& s : layerLabels)
    {
        layerPtrs.push_back(s.c_str());
    }
    auto sceneCombo = [&sceneIds, &scenePtrs](const char* label, Id& id) -> bool {
        int idx = 0;
        for (size_t i = 0; i < sceneIds.size(); ++i)
        {
            if (sceneIds[i] == id)
            {
                idx = static_cast<int>(i);
                break;
            }
        }
        if (ImGui::Combo(label, &idx, scenePtrs.data(), static_cast<int>(scenePtrs.size())) &&
            !sceneIds.empty())
        {
            id = sceneIds[static_cast<size_t>(idx)];
            return true;
        }
        return false;
    };
    auto layerCombo = [&layerIds, &layerPtrs](const char* label, Id& id) -> bool {
        int idx = 0;
        for (size_t i = 0; i < layerIds.size(); ++i)
        {
            if (layerIds[i] == id)
            {
                idx = static_cast<int>(i);
                break;
            }
        }
        if (ImGui::Combo(label, &idx, layerPtrs.data(), static_cast<int>(layerPtrs.size())) &&
            !layerIds.empty())
        {
            id = layerIds[static_cast<size_t>(idx)];
            return true;
        }
        return false;
    };

    bool changed = false;
    std::visit(
        [&changed, &cue, &sceneCombo, &layerCombo](const auto& a) {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, PlayScene>)
            {
                if (sceneCombo("Scene", std::get<PlayScene>(cue.action).scene))
                {
                    changed = true;
                }
            }
            else if constexpr (std::is_same_v<T, StopPlayback> ||
                               std::is_same_v<T, PausePlayback>)
            {
                ImGui::TextDisabled("(no parameters)");
            }
            else if constexpr (std::is_same_v<T, SeekTo>)
            {
                float t = static_cast<float>(std::get<SeekTo>(cue.action).time.toSeconds());
                if (ImGui::InputFloat("Time (s)", &t, 0.1f, 0.5f, "%.2f"))
                {
                    std::get<SeekTo>(cue.action).time = Time::fromSeconds(std::max(0.0, static_cast<double>(t)));
                    changed = true;
                }
            }
            else if constexpr (std::is_same_v<T, SetLayerOpacity>)
            {
                auto& sa = std::get<SetLayerOpacity>(cue.action);
                if (layerCombo("Layer", sa.layer))
                {
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::SliderFloat("Value", &sa.value, 0.0f, 1.0f, "%.2f"))
                {
                    changed = true;
                }
            }
            else if constexpr (std::is_same_v<T, SetLayerVisible>)
            {
                auto& sa = std::get<SetLayerVisible>(cue.action);
                if (layerCombo("Layer", sa.layer))
                {
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Checkbox("Visible", &sa.value))
                {
                    changed = true;
                }
            }
            else if constexpr (std::is_same_v<T, SetLayerBlend>)
            {
                auto& sa = std::get<SetLayerBlend>(cue.action);
                if (layerCombo("Layer", sa.layer))
                {
                    changed = true;
                }
                ImGui::SameLine();
                int idx = static_cast<int>(sa.mode);
                if (ImGui::Combo("Mode", &idx, kBlendModes, kBlendModeCount))
                {
                    sa.mode = static_cast<BlendMode>(idx);
                    changed = true;
                }
            }
            else if constexpr (std::is_same_v<T, SetLayerPosition>)
            {
                auto& sa = std::get<SetLayerPosition>(cue.action);
                if (layerCombo("Layer", sa.layer))
                {
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::InputFloat("x", &sa.x, 10.0f, 50.0f))
                {
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::InputFloat("y", &sa.y, 10.0f, 50.0f))
                {
                    changed = true;
                }
            }
        },
        cue.action);
    if (changed)
    {
        m_Editor.touch();
    }
}

// --- console window ------------------------------------------------------------------

void ControlGuiApp::renderConsoleWindow()
{
    // Begin() may return false; End() must still be called (see above).
    if (!ImGui::Begin("Console", nullptr, ImGuiWindowFlags_HorizontalScrollbar))
    {
        ImGui::End();
        return;
    }
    if (SystemConnection* active = activeConnection(); active)
    {
        ImGui::TextDisabled("target: %s (%s:%d)", active->name().c_str(), active->host().c_str(),
                            active->port());
    }
    else
    {
        ImGui::TextDisabled("target: (none - select a system)");
    }
    if (ImGui::BeginTable("consolein", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("input");
        ImGui::TableSetupColumn("##send", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::InputTextWithHint("##cmd", "method [json-params]   e.g.  playback.seek {\"time\": 5}",
                                 &m_ConsoleInput);
        const bool inputActive = ImGui::IsItemActive();
        ImGui::TableNextColumn();
        if (ImGui::Button("Send") ||
            (inputActive && (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))))
        {
            consoleSubmit();
        }
        ImGui::EndTable();
    }
    if (ImGui::BeginChild("consolelog", ImVec2(0.0f, -30.0f), true))
    {
        for (const auto& line : m_ConsoleLog)
        {
            ImGui::TextUnformatted(line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    if (ImGui::Button("Clear log"))
    {
        m_ConsoleLog.clear();
    }
    ImGui::End();
}

// --- Open / Save As file dialogs (ImGuiFileDialog) --------------------------------------

const char* ControlGuiApp::fileDialogKey(const std::string& which)
{
    if (which == "open")
    {
        return kOpenDialogKey;
    }
    if (which == "saveas")
    {
        return kSaveAsDialogKey;
    }
    if (which == "media")
    {
        return kMediaDialogKey;
    }
    return nullptr;
}

std::string ControlGuiApp::showDir() const
{
    if (!m_Editor.filePath().empty())
    {
        const std::filesystem::path p(m_Editor.filePath());
        if (p.has_parent_path())
        {
            return p.parent_path().string();
        }
    }
    return ".";
}

void ControlGuiApp::openFileDialog()
{
    IGFD::FileDialogConfig config;
    config.path = showDir();
    IGFD::FileDialog::Instance()->OpenDialog(kOpenDialogKey, "Open show...", kShowFilter, config);
}

void ControlGuiApp::saveAsFileDialog()
{
    IGFD::FileDialogConfig config;
    config.path = showDir();
    config.fileName =
        m_Editor.filePath().empty() ? std::string("show.json")
                                    : std::filesystem::path(m_Editor.filePath()).filename().string();
    IGFD::FileDialog::Instance()->OpenDialog(kSaveAsDialogKey, "Save show as...", kShowFilter,
                                             config);
}

void ControlGuiApp::addMediaFileDialog()
{
    IGFD::FileDialogConfig config;
    config.path = showDir();
    config.countSelectionMax = 0; // multi-select: one MediaReference per file
    IGFD::FileDialog::Instance()->OpenDialog(kMediaDialogKey, "Add media files...", kMediaFilter,
                                             config);
}

void ControlGuiApp::renderFileDialogs()
{
    // One shared instance, one dialog at a time. Display() returns true when
    // a result was obtained (OK or Cancel); the result is read, then the
    // dialog is closed.
    IGFD::FileDialog* dlg = IGFD::FileDialog::Instance();

    if (dlg->Display(kOpenDialogKey, kFileDialogFlags, kFileDialogMinSize))
    {
        if (dlg->IsOk())
        {
            const auto selection = dlg->GetSelection();
            if (!selection.empty())
            {
                // Load + sync the system list + push the loaded show to
                // every connected system (see loadShowFile).
                std::string error;
                if (!loadShowFile(selection.begin()->second, error))
                {
                    m_EditorError = error;
                }
            }
        }
        dlg->Close();
    }

    if (dlg->Display(kSaveAsDialogKey, kFileDialogFlags, kFileDialogMinSize))
    {
        if (dlg->IsOk())
        {
            const std::string path = dlg->GetFilePathName();
            if (!path.empty())
            {
                std::string error;
                if (saveEditorFile(path, error))
                {
                    m_EditorError.clear();
                }
                else
                {
                    m_EditorError = error;
                }
            }
        }
        dlg->Close();
    }

    if (dlg->Display(kMediaDialogKey, kFileDialogFlags, kFileDialogMinSize))
    {
        if (dlg->IsOk())
        {
            Show& show = m_Editor.show();
            for (const auto& [name, path] : dlg->GetSelection())
            {
                (void)name;
                MediaReference ref;
                ref.id = ShowEditor::nextMediaId(show);
                ref.path = path;
                show.media.push_back(ref);
            }
            m_Editor.touch();
        }
        dlg->Close();
    }
}

// --- modals ---------------------------------------------------------------------------

void ControlGuiApp::renderModals()
{
    if (m_PullModal)
    {
        modalStartSize(460.0f);
        if (ImGui::BeginPopupModal("Pull show from system", &m_PullModal))
        {
            ImGui::TextWrapped(
                "Saves the show currently loaded on the system to a path on the system's "
                "filesystem (show.save). If that path is accessible from this machine, open it "
                "with File > Open.");
            ImGui::InputText("Path (on the system)", &m_PullPath);
            SystemConnection* active = activeConnection();
            if (ImGui::Button("Pull##pull") && active && !m_PullPath.empty())
            {
                std::string error;
                if (sendActive("show.save", {{"path", m_PullPath}}, error))
                {
                    m_PullModal = false;
                }
                else
                {
                    m_EditorError = error;
                }
            }
            ImGui::EndPopup();
        }
    }
    if (m_AboutModal)
    {
        modalStartSize(460.0f);
        if (ImGui::BeginPopupModal("About MediaMax Control", &m_AboutModal))
        {
            ImGui::Text("MediaMax control interface  (version %s)", MEDIAMAX_VERSION);
            ImGui::Text("Control protocol v%d   |   Dear ImGui %s", mediamax::protocol::kProtocolVersion,
                        ImGui::GetVersion());
            ImGui::Text("%d system%s configured", static_cast<int>(m_Systems.size()),
                        m_Systems.size() == 1 ? "" : "s");
            ImGui::TextWrapped(
                "Systems: connect to any number of playback engines.  Playback: live transport, "
                "scenes, layer overrides, displays and warp.  Show Editor: author and push "
                "show.json documents (show.loadJson).  Console: any raw protocol command.");
            if (ImGui::Button("Close##about"))
            {
                m_AboutModal = false;
            }
            ImGui::EndPopup();
        }
    }
    if (m_HotKeysModal)
    {
        modalStartSize(560.0f);
        if (ImGui::BeginPopupModal("Hot Keys", &m_HotKeysModal))
        {
            ImGui::TextWrapped(
                "Global shortcuts. They fire when no text field or popup has the keyboard - "
                "typing in a field or the console is never intercepted.");
            ImGui::Spacing();
            // One row per binding (hotkeyBindings() is the single source of
            // truth, so the list tracks the table).
            if (ImGui::BeginTable("##hotkeys", 3,
                                  ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("action");
                ImGui::TableSetupColumn("what it does");
                ImGui::TableHeadersRow();
                for (const HotkeyBinding& binding : hotkeyBindings())
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(ImGui::GetKeyChordName(
                        static_cast<ImGuiKeyChord>(binding.key | binding.mods)));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(binding.label);
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", binding.help);
                }
                ImGui::EndTable();
            }
            if (ImGui::Button("Close##hotkeysclose"))
            {
                m_HotKeysModal = false;
            }
            ImGui::EndPopup();
        }
    }
    if (m_SysSettingsModal)
    {
        modalStartSize(440.0f);
        if (ImGui::BeginPopupModal("System settings", &m_SysSettingsModal))
        {
            // The entry this dialog edits (issue 16). The index can go
            // stale if the system is removed while the dialog is open.
            if (m_SysSettingsIndex >= 0 && m_SysSettingsIndex < static_cast<int>(m_Systems.size()))
            {
                auto& conn = *m_Systems[static_cast<size_t>(m_SysSettingsIndex)];
                // System info: the machine the engine runs on, as the
                // engine reports it (status "hostname"/"ip") - the computer
                // name alongside the custom name entered here (issue 16).
                const SystemConnection::Snapshot snap = conn.snapshot();
                if (snap.haveStatus)
                {
                    const std::string machine = snap.status.value("hostname", "");
                    const std::string ip = snap.status.value("ip", "");
                    if (!machine.empty() || !ip.empty())
                    {
                        ImGui::TextDisabled("System info: computer %s, IP %s",
                                            machine.empty() ? "unknown" : machine.c_str(),
                                            ip.empty() ? "unknown" : ip.c_str());
                        ImGui::Separator();
                    }
                }
                ImGui::InputText("Name##sysname2", &m_SysSettings.name);
                ImGui::InputText("IP address##syshost", &m_SysSettings.host);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                ImGui::InputInt("Port##sysport", &m_SysSettings.port);
                if (!m_SysSettings.error.empty())
                {
                    ImGui::TextColored(kBadColor, "%s", m_SysSettings.error.c_str());
                }
                // Changing the address reconnects the system (the entry
                // keeps its slot, so selections and pointers stay valid).
                if (m_SysSettings.host != conn.host() ||
                    m_SysSettings.port != static_cast<int>(conn.port()))
                {
                    ImGui::TextDisabled("(changing the address reconnects this system)");
                }
                ImGui::Spacing();
                if (ImGui::Button("Apply##sysapply"))
                {
                    applySystemSettings();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel##syscancel"))
                {
                    m_SysSettingsModal = false;
                }
            }
            else
            {
                ImGui::TextDisabled("The system no longer exists.");
                if (ImGui::Button("Close##sysclose"))
                {
                    m_SysSettingsModal = false;
                    m_SysSettingsIndex = -1;
                }
            }
            ImGui::EndPopup();
        }
    }
    if (m_EditModal)
    {
        modalStartSize(420.0f);
        if (ImGui::BeginPopupModal("Display settings", &m_EditModal))
        {
            if (m_EditDisplayId != kInvalidId)
            {
                DisplayEdit& de = m_DisplayEdits[m_EditDisplayId];
                // Which show wall entry this is, and where it runs live:
                // the connected system that has a same-named display (if
                // any), and the physical output its window is on (issue 15).
                if (m_EditTarget != nullptr)
                {
                    ImGui::Text("Display '%s' on %s", de.name.c_str(), m_EditTarget->name().c_str());
                }
                else
                {
                    ImGui::Text("Display '%s'", de.name.c_str());
                    ImGui::TextDisabled(
                        "(unmapped: no connected system runs this display - Apply saves it to "
                        "the show; the systems pick it up on the next show push)");
                }
                ImGui::TextDisabled("Physical output: %s",
                                     de.monitor.empty() ? "unknown" : de.monitor.c_str());
                ImGui::Spacing();
                ImGui::InputText("Name##editname", &de.name);
                ImGui::Spacing();
                // The system this display's live settings apply to (issue
                // 14): picking a different system and pressing Apply moves
                // the live display there (recreated with these settings,
                // then removed from the old system) instead of just
                // applying them. The show's wall itself is one list - every
                // system that loads the show builds it - so a live move is
                // a temporary override.
                int sysIdx = m_EditSystem;
                if (sysIdx < 0 || sysIdx >= static_cast<int>(m_Systems.size()))
                {
                    sysIdx = -1;
                    for (int i = 0; i < static_cast<int>(m_Systems.size()); ++i)
                    {
                        if (m_Systems[static_cast<size_t>(i)].get() == m_EditTarget)
                        {
                            sysIdx = i;
                            break;
                        }
                    }
                }
                const char* sysPreview =
                    sysIdx >= 0 ? m_Systems[static_cast<size_t>(sysIdx)]->name().c_str()
                                : "(no system)";
                const bool haveSystems = !m_Systems.empty();
                ImGui::BeginDisabled(!haveSystems);
                if (ImGui::BeginCombo("System##editsys", sysPreview))
                {
                    for (int i = 0; i < static_cast<int>(m_Systems.size()); ++i)
                    {
                        SystemConnection& sys = *m_Systems[static_cast<size_t>(i)];
                        const std::string label =
                            sys.name() + (sys.pollConnected() ? std::string() : "  (offline)");
                        if (ImGui::Selectable(label.c_str(), i == sysIdx))
                        {
                            sysIdx = i;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
                if (!haveSystems)
                {
                    ImGui::TextDisabled("(no system connected)");
                }
                m_EditSystem = sysIdx;
                ImGui::Spacing();
                ImGui::InputInt("X (px)##editx", &de.x);
                ImGui::InputInt("Y (px)##edity", &de.y);
                ImGui::InputInt("Width (px)##editw", &de.width);
                ImGui::InputInt("Height (px)##edith", &de.height);
                ImGui::SliderFloat("Brightness##editbright", &de.brightness, 0.0f, 1.0f, "%.2f");
                // The Enabled toggle lives in the Displays block (main
                // interface); the dialog still round-trips de.enabled so
                // Apply (and a move) keep the current state.
                ImGui::Checkbox("Always on top##editaot", &de.alwaysOnTop);
                // Vsync (swapchain present mode): on = FIFO (compositor
                // paced), off = MAILBOX. A change recreates the display's
                // swapchain on the engine (see Display::vsync).
                ImGui::Checkbox("Vsync##editvsync", &de.vsync);
                if (ImGui::Button("Apply##editapply"))
                {
                    applyDisplayEdit();
                }
                if (!m_EditError.empty())
                {
                    ImGui::TextColored(kBadColor, "%s", m_EditError.c_str());
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel##editcancel"))
                {
                    m_EditModal = false;
                }
            }
            else
            {
                ImGui::TextDisabled("(no display selected)");
                if (ImGui::Button("Close##editclose"))
                {
                    m_EditModal = false;
                }
            }
            ImGui::EndPopup();
        }
        if (!m_EditModal)
        {
            // The modal went down (X or Cancel): drop the target so it can
            // never dangle past the frame it was opened in.
            m_EditTarget = nullptr;
            m_EditDisplayId = kInvalidId;
        }
    }
    if (m_WarpModal)
    {
        modalStartSize(560.0f);
        if (ImGui::BeginPopupModal("Projection warp", &m_WarpModal))
        {
            if (m_WarpDisplayId != kInvalidId)
            {
                WarpEdit& we = m_WarpEdits[m_WarpDisplayId];
                if (!we.valid)
                {
                    we.n = 2;
                    we.spline = false;
                    we.points = identityWarpPoints(we.n);
                    we.valid = true;
                }
                // Which show wall entry this is, and where it runs live
                // (the entry may have been removed from the show while the
                // dialog was open: degrade the label, Apply reports it).
                const ShowDisplay* sd = nullptr;
                for (const auto& d : m_Editor.show().displays)
                {
                    if (d.id == m_WarpDisplayId)
                    {
                        sd = &d;
                        break;
                    }
                }
                if (m_WarpTarget != nullptr)
                {
                    ImGui::Text("Display '%s' on %s",
                                sd != nullptr ? sd->name.c_str() : "(unknown)",
                                m_WarpTarget->name().c_str());
                }
                else
                {
                    ImGui::Text("Display '%s'", sd != nullptr ? sd->name.c_str() : "(unknown)");
                    ImGui::TextDisabled(
                        "(unmapped: no connected system runs this display - Apply saves the "
                        "warp to the show only)");
                }
                ImGui::TextWrapped(
                    "Drag the nodes to shape the projection (display-local UV, (0,0) top-left "
                    "to (1,1) bottom-right). Grid mode blends between the nodes linearly; "
                    "spline mode runs a smooth cubic B-spline through them (the surface passes "
                    "exactly through each node). Identity is an undistorted rectangle.");

                // Interpolation mode + grid size. Changing the grid size
                // resets to the identity lattice (nodes cannot be
                // resampled).
                const char* modes[] = {"Grid (bilinear)", "Spline (cubic B-spline)"};
                int mode = we.spline ? 1 : 0;
                if (ImGui::Combo("Mode##warpmode", &mode, modes, 2))
                {
                    we.spline = mode == 1;
                }
                ImGui::SameLine();
                const char* grids[] = {"2x2", "3x3", "4x4", "5x5", "6x6"};
                int grid = we.n - 2;
                if (grid < 0 || grid >= 5)
                {
                    grid = 0;
                }
                if (ImGui::Combo("Grid##warpgird", &grid, grids, 5))
                {
                    we.n = grid + 2;
                    we.points = identityWarpPoints(we.n);
                    m_WarpDragNode = -1;
                }

                // --- node pad ------------------------------------------------
                // A square child region the display's UV space maps onto.
                // The nodes are hit-tested and dragged with the mouse; the
                // grid (or the spline isoparametric curves through the
                // nodes) is drawn behind them.
                const float padW =
                    std::clamp(ImGui::GetContentRegionAvail().x, 160.0f, 520.0f);
                ImGui::BeginChild("warppad##warpmodal", ImVec2(padW, padW), true);
                {
                    const float padL = ImGui::GetWindowPos().x;
                    const float padT = ImGui::GetWindowPos().y;
                    const float padR = padL + ImGui::GetWindowContentRegionMax().x;
                    const float padB = padT + ImGui::GetWindowContentRegionMax().y;
                    auto toPx = [padL, padT, padR, padB](float u, float v) {
                        return ImVec2(padL + u * (padR - padL), padT + v * (padB - padT));
                    };
                    auto toUv = [padL, padT, padR, padB](const ImVec2& p) {
                        return ImVec2((p.x - padL) / (padR - padL), (p.y - padT) / (padB - padT));
                    };
                    const ImVec2 mouse = ImGui::GetIO().MousePos;
                    const bool mouseInPad =
                        mouse.x >= padL && mouse.x <= padR && mouse.y >= padT && mouse.y <= padB;

                    // Hit-test (the last node drawn is on top).
                    int hover = -1;
                    if (mouseInPad)
                    {
                        for (int p = static_cast<int>(we.points.size()) - 1; p >= 0; --p)
                        {
                            const ImVec2 np = toPx(we.points[p][0], we.points[p][1]);
                            const float dx = mouse.x - np.x;
                            const float dy = mouse.y - np.y;
                            if (dx * dx + dy * dy <= 100.0f) // 10 px
                            {
                                hover = p;
                                break;
                            }
                        }
                    }
                    if (m_WarpDragNode < 0 && hover >= 0 && ImGui::IsMouseClicked(0) &&
                        ImGui::IsWindowHovered())
                    {
                        m_WarpDragNode = hover;
                    }
                    if (m_WarpDragNode >= 0 && m_WarpDragNode < static_cast<int>(we.points.size()))
                    {
                        if (!ImGui::IsMouseDown(0))
                        {
                            m_WarpDragNode = -1;
                        }
                        else
                        {
                            // The node follows the mouse, clamped to the
                            // display's UV space (the cursor may run off the
                            // pad while dragging).
                            const ImVec2 uv = toUv(mouse);
                            we.points[m_WarpDragNode] = {std::clamp<float>(uv.x, 0.f, 1.f),
                                                         std::clamp<float>(uv.y, 0.f, 1.f)};
                        }
                    }
                    if (hover >= 0)
                    {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }

                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const ImU32 lineColor = ImGui::GetColorU32(ImGuiCol_Separator);
                    if (we.spline)
                    {
                        // Isoparametric curves: a Catmull-Rom spline through
                        // each row of nodes and through each column (the
                        // surface's exact curves at the node rows/columns;
                        // boundary nodes clamped, like the core samples).
                        constexpr int kCurveSteps = 16;
                        auto splineLine = [&](int index, bool horizontal) {
                            std::vector<ImVec2> pts;
                            pts.reserve(static_cast<size_t>(we.n - 1) * (kCurveSteps + 1));
                            for (int seg = 0; seg < we.n - 1; ++seg)
                            {
                                for (int s = 0; s <= kCurveSteps; ++s)
                                {
                                    if (seg > 0 && s == 0)
                                    {
                                        continue; // join, not a duplicate point
                                    }
                                    const int i0 = std::max(seg - 1, 0);
                                    const int i1 = seg;
                                    const int i2 = seg + 1;
                                    const int i3 = std::min(seg + 2, we.n - 1);
                                    const float t = static_cast<float>(s) / kCurveSteps;
                                    auto node = [&](int i) {
                                        return horizontal ? we.points[index * we.n + i]
                                                          : we.points[i * we.n + index];
                                    };
                                    const float u = warpCatmullRom(node(i0)[0], node(i1)[0],
                                                                   node(i2)[0], node(i3)[0], t);
                                    const float v = warpCatmullRom(node(i0)[1], node(i1)[1],
                                                                   node(i2)[1], node(i3)[1], t);
                                    pts.push_back(toPx(u, v));
                                }
                            }
                            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), lineColor,
                                            false, 1.0f);
                        };
                        for (int i = 0; i < we.n; ++i)
                        {
                            splineLine(i, true);
                            splineLine(i, false);
                        }
                    }
                    else
                    {
                        // Straight segments between adjacent nodes.
                        for (int row = 0; row < we.n; ++row)
                        {
                            for (int col = 0; col + 1 < we.n; ++col)
                            {
                                const ImVec2 a = toPx(we.points[row * we.n + col][0],
                                                      we.points[row * we.n + col][1]);
                                const ImVec2 b = toPx(we.points[row * we.n + col + 1][0],
                                                      we.points[row * we.n + col + 1][1]);
                                dl->AddLine(a, b, lineColor, 1.0f);
                            }
                        }
                        for (int col = 0; col < we.n; ++col)
                        {
                            for (int row = 0; row + 1 < we.n; ++row)
                            {
                                const ImVec2 a = toPx(we.points[row * we.n + col][0],
                                                      we.points[row * we.n + col][1]);
                                const ImVec2 b = toPx(we.points[(row + 1) * we.n + col][0],
                                                      we.points[(row + 1) * we.n + col][1]);
                                dl->AddLine(a, b, lineColor, 1.0f);
                            }
                        }
                    }
                    // The nodes themselves (dragged / hovered ones on top,
                    // larger).
                    for (int pass = 0; pass < 2; ++pass)
                    {
                        for (int p = 0; p < static_cast<int>(we.points.size()); ++p)
                        {
                            const bool hot = (p == hover || p == m_WarpDragNode);
                            if ((pass == 1) != hot)
                            {
                                continue;
                            }
                            const ImVec2 np = toPx(we.points[p][0], we.points[p][1]);
                            const float r = hot ? 5.5f : 4.0f;
                            dl->AddCircleFilled(np, r,
                                                hot ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                                                    : ImGui::GetColorU32(ImGuiCol_FrameBg));
                            dl->AddCircle(np, r,
                                          hot ? ImGui::GetColorU32(ImGuiCol_Text)
                                              : ImGui::GetColorU32(ImGuiCol_Border),
                                          12);
                        }
                    }
                }
                ImGui::EndChild();

                if (ImGui::Button("Identity##warp"))
                {
                    we.points = identityWarpPoints(we.n);
                    m_WarpDragNode = -1;
                }
                ImGui::SameLine();
                if (ImGui::Button("Apply warp##warp"))
                {
                    m_WarpError.clear();
                    // Persist first: the grid becomes the show entry's warp
                    // (the auto-push propagates it; the engine applies it on
                    // the next show load).
                    ShowDisplay* entry = nullptr;
                    for (auto& d : m_Editor.show().displays)
                    {
                        if (d.id == m_WarpDisplayId)
                        {
                            entry = &d;
                            break;
                        }
                    }
                    if (entry == nullptr)
                    {
                        m_WarpError = "the display is no longer in the show";
                        m_WarpModal = false;
                    }
                    else
                    {
                        WarpMap warp(we.n, we.n,
                                     we.spline ? WarpInterpolation::Spline
                                               : WarpInterpolation::Bilinear);
                        const int count =
                            std::min(we.n * we.n, static_cast<int>(we.points.size()));
                        for (int i = 0; i < count; ++i)
                        {
                            warp.setPointPosition(i / we.n, i % we.n,
                                                  we.points[static_cast<size_t>(i)][0],
                                                  we.points[static_cast<size_t>(i)][1]);
                        }
                        entry->warp = std::move(warp);
                        m_Editor.touch();
                        // Live preview while the entry is mapped: the same
                        // grid, sent to the mapped system's same-named
                        // display now (the show push applies it a second
                        // later anyway).
                        if (m_WarpTarget != nullptr)
                        {
                            json pts = json::array();
                            for (const auto& pt : we.points)
                            {
                                pts.push_back({pt[0], pt[1]});
                            }
                            nlohmann::json result;
                            std::string error;
                            const Id liveId = liveDisplayIdFor(m_WarpTarget, entry->name);
                            if (liveId != kInvalidId &&
                                m_WarpTarget->send("display.warp.set",
                                                   {{"display", liveId}, {"points", pts},
                                                    {"mode", we.spline ? "spline" : "grid"}},
                                                   result, error))
                            {
                                log("warp (" + std::string(we.spline ? "spline" : "grid") +
                                    ", " + std::to_string(we.n) + "x" + std::to_string(we.n) +
                                    ") saved to display '" + entry->name + "' (live on " +
                                    m_WarpTarget->name() + ")");
                                m_WarpModal = false;
                            }
                            else
                            {
                                m_WarpError =
                                    (liveId == kInvalidId
                                         ? "no live display named '" + entry->name + "' on " +
                                               m_WarpTarget->name() + ": "
                                         : error + "; ") +
                                    "the warp is saved in the show, but the live update failed";
                            }
                        }
                        else
                        {
                            log("warp (" + std::string(we.spline ? "spline" : "grid") +
                                ", " + std::to_string(we.n) + "x" + std::to_string(we.n) +
                                ") saved to display '" + entry->name + "' (no system mapped)");
                            m_WarpModal = false;
                        }
                    }
                }
                if (!m_WarpError.empty())
                {
                    ImGui::TextColored(kBadColor, "%s", m_WarpError.c_str());
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel##warpcancel"))
                {
                    m_WarpModal = false;
                }
            }
            else
            {
                ImGui::TextDisabled("(no display selected)");
                if (ImGui::Button("Close##warpclose"))
                {
                    m_WarpModal = false;
                }
            }
            ImGui::EndPopup();
        }
        if (!m_WarpModal)
        {
            m_WarpTarget = nullptr;
            m_WarpDisplayId = kInvalidId;
            m_WarpDragNode = -1;
        }
    }
    if (m_MediaDetailsModal)
    {
        modalStartSize(420.0f);
        if (ImGui::BeginPopupModal("Media details", &m_MediaDetailsModal))
        {
            // A copy of the target system's live snapshot, kept alive for the
            // whole block: the entry pointer below points into it (reading
            // the snapshot fresh each frame also makes the dialog track the
            // engine, and degrade cleanly if the entry is unloaded).
            const auto snap = (m_MediaDetailsTarget != nullptr)
                                  ? m_MediaDetailsTarget->snapshot()
                                  : SystemConnection::Snapshot{};
            const json* entry = nullptr;
            if (snap.haveStatus && snap.status.contains("media") &&
                snap.status["media"].is_array())
            {
                for (const auto& e : snap.status["media"])
                {
                    if (e.is_object() &&
                        static_cast<Id>(e.value("id", 0u)) == m_MediaDetailsId)
                    {
                        entry = &e;
                        break;
                    }
                }
            }
            if (entry == nullptr)
            {
                ImGui::TextWrapped("Media entry %u is not loaded on the system anymore.",
                                   m_MediaDetailsId);
            }
            else
            {
                const json& m = *entry;
                ImGui::TextWrapped("%u - %s", m_MediaDetailsId, m.value("path", "?").c_str());
                if (m_MediaDetailsTarget != nullptr)
                {
                    ImGui::TextDisabled("on %s", m_MediaDetailsTarget->name().c_str());
                }
                if (ImGui::BeginTable("mediadetails", 2, ImGuiTableFlags_SizingFixedFit))
                {
                    // One label/value row; an empty value renders as "-"
                    // (the container/codec does not report it).
                    auto detailRow = [&m](const char* label, const std::string& value) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(label);
                        ImGui::TableNextColumn();
                        if (value.empty())
                        {
                            ImGui::TextDisabled("-");
                        }
                        else
                        {
                            ImGui::TextUnformatted(value.c_str());
                        }
                    };
                    detailRow("Type", m.value("type", std::string()));
                    detailRow("Codec", m.value("codec", std::string()));
                    detailRow("Color space", m.value("colorspace", std::string()));
                    const std::uint32_t bitDepth = static_cast<std::uint32_t>(m.value("bitDepth", 0u));
                    detailRow("Bit depth", bitDepth > 0 ? std::to_string(bitDepth) + " bit" : "");
                    detailRow("Scan type", m.value("scanType", std::string()));
                    std::string rate;
                    const std::int64_t bitRate = m.value("bitRate", 0);
                    if (bitRate > 0)
                    {
                        char buf[48];
                        if (bitRate >= 1000000)
                        {
                            std::snprintf(buf, sizeof(buf), "%.2f Mbit/s",
                                          static_cast<double>(bitRate) / 1e6);
                        }
                        else if (bitRate >= 1000)
                        {
                            std::snprintf(buf, sizeof(buf), "%.0f Kbit/s",
                                          static_cast<double>(bitRate) / 1e3);
                        }
                        else
                        {
                            std::snprintf(buf, sizeof(buf), "%lld bit/s",
                                          static_cast<long long>(bitRate));
                        }
                        rate = buf;
                    }
                    detailRow("Bit rate", rate);
                    detailRow("Audio codec", m.value("audioCodec", std::string()));
                    detailRow("Resolution", std::to_string(m.value("width", 0)) + "x" +
                                               std::to_string(m.value("height", 0)));
                    detailRow("Frame rate", m.value("frameRate", std::string()));
                    char dur[32];
                    std::snprintf(dur, sizeof(dur), "%.2f s", m.value("duration", 0.0));
                    detailRow("Duration", dur);
                    ImGui::EndTable();
                }
                ImGui::SameLine();
                if (ImGui::Button("Close##mediadetails"))
                {
                    m_MediaDetailsModal = false;
                }
            }
            ImGui::EndPopup();
        }
        if (!m_MediaDetailsModal)
        {
            // The modal went down (X or Close): drop the target so it can
            // never dangle past the frame it was opened in.
            m_MediaDetailsTarget = nullptr;
            m_MediaDetailsId = kInvalidId;
        }
    }
}

// --- test hooks -----------------------------------------------------------------

bool ControlGuiApp::testOpenModal(const std::string& which)
{
    if (fileDialogKey(which) == nullptr && modalPopupName(which) == nullptr)
    {
        return false;
    }
    // Deferred to the next frame(): OpenPopup() dereferences the current
    // ImGui window, which is NULL between frames.
    m_PendingTestOpenModal = which;
    return true;
}

void ControlGuiApp::openModal(const std::string& which)
{
    // The same state the UI buttons/menus set. Must run inside frame()
    // (the modal host window is current for the popups; the file dialogs
    // only need a valid ImGui context).
    if (which == "open")
    {
        openFileDialog();
        return;
    }
    if (which == "saveas")
    {
        saveAsFileDialog();
        return;
    }
    if (which == "media")
    {
        addMediaFileDialog();
        return;
    }
    const char* name = modalPopupName(which);
    if (name == nullptr)
    {
        return;
    }
    if (which == "pull")
    {
        m_PullModal = true;
    }
    else if (which == "edit" || which == "warp")
    {
        // The real buttons fill the state from a table row (a show document
        // display); the test hook uses the document's first display (the
        // helpers open the popup themselves, so return early).
        bool opened = false;
        if (!m_Editor.show().displays.empty())
        {
            const Id docId = m_Editor.show().displays.front().id;
            if (which == "edit")
            {
                openDisplayEdit(docId);
            }
            else
            {
                openWarpEdit(docId);
            }
            opened = true;
        }
        if (!opened)
        {
            // The show has no displays yet: still open the dialog (it shows
            // the "no display selected" line); the hook only needs it to
            // open and close.
            if (which == "edit")
            {
                m_EditModal = true;
                m_EditError.clear();
            }
            else
            {
                m_WarpModal = true;
                m_WarpError.clear();
            }
            ImGui::OpenPopup(modalPopupId(name));
        }
        return;
    }
    else if (which == "mediadetails")
    {
        // The real button fills the state from a media table row; the test
        // hook uses the active system's first media entry when it has one.
        bool opened = false;
        if (SystemConnection* active = activeConnection())
        {
            const auto snap = active->snapshot();
            if (snap.haveStatus && snap.status.contains("media") &&
                !snap.status["media"].empty())
            {
                openMediaDetails(snap.status["media"][0], active);
                opened = true;
            }
        }
        if (!opened)
        {
            // No media loaded: still open the dialog (it shows the
            // "not loaded anymore" line); the hook only needs it to open
            // and close.
            m_MediaDetailsTarget = nullptr;
            m_MediaDetailsId = kInvalidId;
            m_MediaDetailsModal = true;
            ImGui::OpenPopup(modalPopupId(name));
        }
        return;
    }
    else if (which == "syssettings")
    {
        // The real button fills the state from a row; the test hook opens
        // it for the active system (headless runs have no systems, and the
        // pane shows no rows then - nothing to open).
        if (m_Systems.empty())
        {
            return;
        }
        const int index =
            (m_Active >= 0 && m_Active < static_cast<int>(m_Systems.size())) ? m_Active : 0;
        openSystemSettings(index);
        return;
    }
    else if (which == "hotkeys")
    {
        m_HotKeysModal = true;
    }
    else
    {
        m_AboutModal = true;
    }
    ImGui::OpenPopup(modalPopupId(name));
}

bool ControlGuiApp::testModalOpen(const std::string& which) const
{
    if (const char* key = fileDialogKey(which))
    {
        return IGFD::FileDialog::Instance()->IsOpened(key);
    }
    const char* name = modalPopupName(which);
    if (name == nullptr)
    {
        return false;
    }
    // IsPopupOpen() has no context-free (id) overload in this release, so
    // check the popup stack directly for the id BeginPopupModal() will use.
    const ImGuiID id = modalPopupId(name);
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    for (const ImGuiPopupData& popup : g.OpenPopupStack)
    {
        if (popup.PopupId == id)
        {
            return true;
        }
    }
    return false;
}

bool ControlGuiApp::testCloseModal(const std::string& which)
{
    if (fileDialogKey(which) != nullptr)
    {
        // Close() is immediate (clears the open key); the window stops
        // rendering on the next frame.
        IGFD::FileDialog::Instance()->Close();
        return true;
    }
    // Clearing the app-side flag is the documented close path: BeginPopupModal
    // sees *p_open == false and calls ClosePopupToLevel() itself.
    if (which == "pull")
    {
        m_PullModal = false;
        return true;
    }
    if (which == "edit")
    {
        m_EditModal = false;
        // Tear down the dialog state the same way the in-frame close paths
        // do (renderModals): a test close between frames skips that reset,
        // and a stale target must never survive into a later open.
        m_EditTarget = nullptr;
        m_EditDisplayId = kInvalidId;
        m_EditSystem = -1;
        return true;
    }
    if (which == "warp")
    {
        m_WarpModal = false;
        return true;
    }
    if (which == "syssettings")
    {
        m_SysSettingsModal = false;
        m_SysSettingsIndex = -1;
        return true;
    }
    if (which == "about")
    {
        m_AboutModal = false;
        return true;
    }
    if (which == "hotkeys")
    {
        m_HotKeysModal = false;
        return true;
    }
    if (which == "mediadetails")
    {
        m_MediaDetailsModal = false;
        return true;
    }
    return false;
}

void ControlGuiApp::testTouchEditor(const std::string& name)
{
    m_Editor.show().name = name;
    m_Editor.touch();
}

std::size_t ControlGuiApp::testAddCue()
{
    // The exact action of the "Add cue" button (no ImGui involved).
    addCueDefault();
    m_Editor.touch();
    return m_Editor.show().cues.size();
}

void ControlGuiApp::testAddSystem(const std::string& name, const std::string& host, int port)
{
    addSystem(name, host, port);
}

bool ControlGuiApp::testSystemConnected(const std::string& name) const
{
    for (const auto& system : m_Systems)
    {
        if (system->name() == name && system->pollConnected())
        {
            return true;
        }
    }
    return false;
}

bool ControlGuiApp::testLoadShowFile(const std::string& path)
{
    std::string error;
    return loadShowFile(path, error);
}

Id ControlGuiApp::testAddDisplay()
{
    const Id id = ShowEditor::nextDisplayId(m_Editor.show());
    addShowDisplay();
    return id;
}

int ControlGuiApp::testSeedDisplaysFromActive()
{
    SystemConnection* active = activeConnection();
    if (active == nullptr)
    {
        return 0;
    }
    const auto snap = active->snapshot();
    if (!snap.haveStatus || !snap.status.contains("displays") || !snap.status["displays"].is_array())
    {
        return 0;
    }
    const json& displays = snap.status["displays"];
    if (displays.empty())
    {
        return 0;
    }
    Show& show = m_Editor.show();
    int seeded = 0;
    for (const auto& d : displays)
    {
        ShowDisplay sd;
        sd.id = ShowEditor::nextDisplayId(show);
        sd.name = d.value("name", "display");
        sd.x = static_cast<std::int32_t>(d.value("x", 0));
        sd.y = static_cast<std::int32_t>(d.value("y", 0));
        sd.width = static_cast<std::int32_t>(d.value("width", 1920));
        sd.height = static_cast<std::int32_t>(d.value("height", 1080));
        sd.brightness = static_cast<float>(d.value("brightness", 1.0));
        sd.enabled = d.value("enabled", true);
        sd.alwaysOnTop = d.value("alwaysOnTop", true);
        sd.statusOverlay = d.value("statusOverlay", false);
        sd.vsync = d.value("vsync", true);
        if (d.contains("warp") && d["warp"].is_object())
        {
            const json& w = d["warp"];
            const int rows = static_cast<int>(w.value("rows", 2));
            const int cols = static_cast<int>(w.value("cols", 2));
            const bool spline = w.value("mode", "grid") == "spline";
            if (rows >= 2 && cols >= 2 && w.contains("points") && w["points"].is_array() &&
                w["points"].size() == static_cast<std::size_t>(rows * cols))
            {
                WarpMap warp(rows, cols,
                             spline ? WarpInterpolation::Spline : WarpInterpolation::Bilinear);
                int i = 0;
                for (const auto& p : w["points"])
                {
                    if (p.is_array() && p.size() == 2)
                    {
                        warp.setPointPosition(i / cols, i % cols,
                                               static_cast<float>(p[0].get<double>()),
                                               static_cast<float>(p[1].get<double>()));
                    }
                    ++i;
                }
                sd.warp = std::move(warp);
            }
        }
        show.displays.push_back(std::move(sd));
        ++seeded;
    }
    // No touch: seeding mirrors live state into the document without
    // marking the show edited (an auto-push must not fire for it).
    return seeded;
}

std::string ControlGuiApp::testDisplayMappedSystem(const std::string& displayName) const
{
    SystemConnection* mapped = mappedSystemFor(displayName);
    return mapped != nullptr ? mapped->name() : std::string();
}

bool ControlGuiApp::testDockLayoutPlaybackConsoleTabs() const
{
    // The default layout must tab Playback and Console together, in that
    // order. The dock node's Windows list is unordered (Dear ImGui keeps
    // the user-visible tab order in the node's TabBar), so check both the
    // shared node and the tab index.
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiWindow* playback = nullptr;
    const ImGuiWindow* console = nullptr;
    for (int wi = 0; wi < g.Windows.Size; ++wi)
    {
        ImGuiWindow* w = g.Windows.Data[static_cast<std::size_t>(wi)];
        if (w->Name == nullptr)
        {
            continue;
        }
        if (std::string(w->Name) == "Playback")
        {
            playback = w;
        }
        else if (std::string(w->Name) == "Console")
        {
            console = w;
        }
    }
    if (playback == nullptr || console == nullptr || playback->DockNode == nullptr ||
        playback->DockNode != console->DockNode)
    {
        return false; // windows missing or not in the same tab block
    }
    const ImGuiDockNode* node = playback->DockNode;
    if (node->TabBar == nullptr || node->TabBar->Tabs.Size < 2)
    {
        return false;
    }
    int playbackTab = -1;
    int consoleTab = -1;
    for (int i = 0; i < node->TabBar->Tabs.Size; ++i)
    {
        const ImGuiTabItem& tab = node->TabBar->Tabs.Data[static_cast<std::size_t>(i)];
        if (tab.Window == playback)
        {
            playbackTab = i;
        }
        if (tab.Window == console)
        {
            consoleTab = i;
        }
    }
    return playbackTab >= 0 && consoleTab > playbackTab;
}

void ControlGuiApp::testGiveEditorALayer()
{
    Show& show = m_Editor.show();
    Scene scene;
    scene.id = 1;
    scene.name = "A";
    Layer layer;
    layer.id = 10;
    layer.name = "red";
    layer.color = {1.0f, 0.0f, 0.0f, 1.0f};
    scene.layers.push_back(layer);
    if (!m_SelfTestBmpPath.empty())
    {
        // A media layer over the self-test BMP (320x180, scaled to cover the
        // 1920x1080 wall): the auto-push delivers it to the engine, so the
        // frame phases exercise the show view's media path (thumbnail +
        // texture + textured quad) against a real system.
        MediaReference ref;
        ref.id = 1;
        ref.path = m_SelfTestBmpPath;
        show.media.push_back(ref);
        Layer mediaLayer;
        mediaLayer.id = 12;
        mediaLayer.name = "testbmp";
        mediaLayer.mediaId = 1;
        mediaLayer.transform.x = 960.f;
        mediaLayer.transform.y = 540.f;
        mediaLayer.transform.scaleX = 6.f;
        mediaLayer.transform.scaleY = 6.f;
        scene.layers.push_back(mediaLayer);
    }
    show.scenes.push_back(std::move(scene));
    m_Editor.touch();
}

void ControlGuiApp::testRemoveSelfTestBmp()
{
    if (!m_SelfTestBmpPath.empty())
    {
        std::error_code fsError;
        std::filesystem::remove(m_SelfTestBmpPath, fsError); // best effort
        m_SelfTestBmpPath.clear();
    }
}

// --- show view test hooks (live self-test) --------------------------------------

void ControlGuiApp::testShowViewSetMode(const std::string& mode)
{
    if (mode == "display")
    {
        m_ShowViewMode = ShowViewMode::Display;
    }
    else if (mode == "layer")
    {
        m_ShowViewMode = ShowViewMode::Layer;
    }
    else if (mode == "show")
    {
        m_ShowViewMode = ShowViewMode::Show;
    }
}

bool ControlGuiApp::testShowViewCamera(float& scale, float& childMinX, float& childMinY,
                                       float& childMaxX, float& childMaxY) const
{
    if (m_ShowCam.scale <= 0.f)
    {
        return false;
    }
    scale = m_ShowCam.scale;
    childMinX = m_ShowViewRectMin.x;
    childMinY = m_ShowViewRectMin.y;
    childMaxX = m_ShowViewRectMax.x;
    childMaxY = m_ShowViewRectMax.y;
    return true;
}

bool ControlGuiApp::testShowViewProject(float wx, float wy, float& sx, float& sy) const
{
    if (m_ShowCam.scale <= 0.f)
    {
        return false;
    }
    sx = m_ShowViewRectMin.x + wx * m_ShowCam.scale + m_ShowCam.offsetX;
    sy = m_ShowViewRectMin.y + wy * m_ShowCam.scale + m_ShowCam.offsetY;
    return true;
}

Id ControlGuiApp::testShowViewSelectedDisplay() const
{
    return m_ShowViewSelDisplay;
}

Id ControlGuiApp::testShowViewSelectedLayer() const
{
    return m_ShowViewSelLayer;
}

bool ControlGuiApp::testShowViewEditPopupOpen() const
{
    return m_ShowViewEditPopupOpen;
}

bool ControlGuiApp::testShowViewEditSelectedDisplay()
{
    if (m_ShowViewSelDisplay == kInvalidId)
    {
        return false;
    }
    const ShowViewDisplay* sel = findShowViewDisplay(m_ShowScene, m_ShowViewSelDisplay);
    if (sel == nullptr)
    {
        return false;
    }
    // The document part is safe between frames; the dialog itself is
    // deferred to the next frame() - openDisplayEdit calls ImGui::OpenPopup,
    // which needs a current window (g.CurrentWindow is NULL between frames).
    m_PendingLiveDisplayEdit = ensureShowDisplayEntryForLive(*sel);
    return m_PendingLiveDisplayEdit != kInvalidId;
}

bool ControlGuiApp::testShowTextureUploaded(std::uint32_t mediaId) const
{
    return m_ShowTexPool.textureUploaded(mediaId);
}

bool ControlGuiApp::testShowTextureSize(std::uint32_t mediaId, int& width,
                                        int& height) const
{
    return m_ShowTexPool.textureSize(mediaId, width, height);
}

bool ControlGuiApp::testShowTextureReadback(std::uint32_t mediaId, VkCommandBuffer cmd,
                                            VkBuffer dst) const
{
    return m_ShowTexPool.readbackInto(mediaId, cmd, dst);
}

void ControlGuiApp::testRemoveDocumentDisplay(const std::string& name)
{
    Show& show = m_Editor.show();
    for (auto it = show.displays.begin(); it != show.displays.end(); ++it)
    {
        if (it->name == name)
        {
            removeShowDisplay(it->id);
            return;
        }
    }
}

void ControlGuiApp::testSelectDisplayEditSystem(int index)
{
    m_EditSystem = index;
}

bool ControlGuiApp::testApplyDisplayEdit()
{
    // The exact action of the dialog's Apply button (no ImGui involved).
    return applyDisplayEdit();
}

bool ControlGuiApp::testActiveHasDisplay() const
{
    const SystemConnection* active = activeConnection();
    if (active == nullptr)
    {
        return false;
    }
    const auto snap = active->snapshot();
    return snap.haveStatus && snap.status.contains("displays") && !snap.status["displays"].empty();
}

void ControlGuiApp::testSetDisplayEditName(const std::string& name)
{
    // The dialog's Name input edits this field; guard the stale-target case
    // (no display selected).
    if (m_EditDisplayId != kInvalidId)
    {
        m_DisplayEdits[m_EditDisplayId].name = name;
    }
}

bool ControlGuiApp::testActiveSnapshotHasDisplay(Id displayId) const
{
    const SystemConnection* active = activeConnection();
    if (active == nullptr)
    {
        return false;
    }
    const auto snap = active->snapshot();
    if (!snap.haveStatus || !snap.status.contains("displays") ||
        !snap.status["displays"].is_array())
    {
        return false;
    }
    for (const auto& d : snap.status["displays"])
    {
        if (d.value("id", 0u) == displayId)
        {
            return true;
        }
    }
    return false;
}

void ControlGuiApp::testSetSystemSettingsName(const std::string& name)
{
    // The dialog's Name input edits this field.
    m_SysSettings.name = name;
}

void ControlGuiApp::testSetSystemSettingsPort(int port)
{
    // The dialog's Port input edits this field.
    m_SysSettings.port = port;
}

bool ControlGuiApp::testApplySystemSettings()
{
    // The exact action of the dialog's Apply button (no ImGui involved).
    return applySystemSettings();
}

std::string ControlGuiApp::testSystemNameAt(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_Systems.size()))
    {
        return std::string();
    }
    return m_Systems[static_cast<size_t>(index)]->name();
}

std::string ControlGuiApp::testSystemHostname(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_Systems.size()))
    {
        return std::string();
    }
    const auto snap = m_Systems[static_cast<size_t>(index)]->snapshot();
    if (!snap.haveStatus)
    {
        return std::string();
    }
    return snap.status.value("hostname", std::string());
}

// --- self test ---------------------------------------------------------------------------

bool ControlGuiApp::selfTestSystem(const std::string& host, std::uint16_t port, std::string& report)
{
    // Drives the exact same command path the UI uses (SystemConnection),
    // against a running playback engine. The target show is replaced by a
    // deterministic one; run it against a scratch playback instance.
    //
    // The system is also registered in the app itself: the UI (and the
    // auto-push of edited shows) must see it as a real system.
    addSystem("self-test", host, port);
    SystemConnection conn("self-test", host, port);
    bool allOk = true;
    auto step = [&](const std::string& name, bool ok, const std::string& detail = "") {
        report += (ok ? "PASS " : "FAIL ") + name;
        if (!detail.empty())
        {
            report += " - " + detail;
        }
        report += "\n";
        if (!ok)
        {
            allOk = false;
        }
    };
    json result;
    std::string error;

    step("ping", conn.send("ping", json::object(), result, error), error);
    const std::string systemName = result.value("system", "");

    step("status", conn.send("status", json::object(), result, error), error);

    // A minimal deterministic show: two color scenes, start at scene 1, stop
    // at 30 s. No scene cue in between, so scene.clear always returns to
    // scene 1 within the test's time budget.
    Show show;
    show.name = "GUI Self-Test";
    show.frameRate = FrameRate::fps30();
    show.duration = Time::fromSeconds(30.0);
    Scene sceneA;
    sceneA.id = 1;
    sceneA.name = "A";
    Layer layerA;
    layerA.id = 10;
    layerA.name = "red";
    layerA.color = {1.0f, 0.0f, 0.0f, 1.0f};
    sceneA.layers.push_back(layerA);
    Scene sceneB;
    sceneB.id = 2;
    sceneB.name = "B";
    Layer layerB;
    layerB.id = 20;
    layerB.name = "blue";
    layerB.color = {0.0f, 0.0f, 1.0f, 1.0f};
    sceneB.layers.push_back(layerB);
    show.scenes = {sceneA, sceneB};
    Cue start;
    start.time = Time::zero();
    start.name = "start";
    start.action = PlayScene{1};
    show.addCue(std::move(start));
    Cue stop;
    stop.time = Time::fromSeconds(30.0);
    stop.name = "stop";
    stop.action = StopPlayback{};
    show.addCue(std::move(stop));

    step("show.loadJson", conn.send("show.loadJson", {{"show", show.toJson()}}, result, error), error);
    conn.send("status", json::object(), result, error);
    step("status after load (stopped, scene 1)",
         result.value("state", "") == "stopped" && result.value("scene", 0u) == 1u,
         "state=" + result.value("state", "?") + " scene=" + std::to_string(result.value("scene", 0u)));

    step("playback.play", conn.send("playback.play", json::object(), result, error), error);
    step("scene.list", conn.send("scene.list", json::object(), result, error), error);

    step("scene.set index 1", conn.send("scene.set", {{"index", 1}}, result, error), error);
    conn.send("status", json::object(), result, error);
    step("status after scene.set (scene 2)", result.value("scene", 0u) == 2u,
         "scene=" + std::to_string(result.value("scene", 0u)));

    // Multiple scenes can be active at once: with scene 2 manually active,
    // toggling scene 1 on adds it - composited in list order.
    step("scene.toggle index 0 (adds scene 1)",
         conn.send("scene.toggle", {{"index", 0}}, result, error), error);
    auto activeScenes = [&result]() {
        std::vector<std::uint64_t> ids;
        if (result.contains("activeScenes") && result["activeScenes"].is_array())
        {
            for (const auto& v : result["activeScenes"])
            {
                ids.push_back(v.get<std::uint64_t>());
            }
        }
        return ids;
    };
    step("toggle: both scenes active in list order",
         result.value("active", false) && activeScenes() == std::vector<std::uint64_t>{1, 2},
         "activeScenes size=" + std::to_string(activeScenes().size()));

    step("scene.toggle index 0 again (removes scene 1)",
         conn.send("scene.toggle", {{"index", 0}}, result, error), error);
    step("toggle: only scene 2 remains",
         activeScenes() == std::vector<std::uint64_t>{2},
         "activeScenes size=" + std::to_string(activeScenes().size()));

    // The last manual scene can be toggled off: the manual set stays in
    // effect but empty - no scene active (black output, status scene 0)
    // until a scene is toggled back on or the operator returns to the
    // timeline.
    step("scene.toggle index 1 again (removes scene 2)",
         conn.send("scene.toggle", {{"index", 1}}, result, error), error);
    step("toggle: no scene active (empty manual set)",
         !result.value("active", false) && activeScenes().empty(),
         "activeScenes size=" + std::to_string(activeScenes().size()));
    conn.send("status", json::object(), result, error);
    step("status with no scene active reports scene 0", result.value("scene", 0u) == 0u,
         "scene=" + std::to_string(result.value("scene", 0u)));
    step("scene.toggle index 1 (adds scene 2 back)",
         conn.send("scene.toggle", {{"index", 1}}, result, error), error);
    step("toggle: scene 2 active again",
         activeScenes() == std::vector<std::uint64_t>{2},
         "activeScenes size=" + std::to_string(activeScenes().size()));

    step("scene.clear", conn.send("scene.clear", json::object(), result, error), error);
    step("scene.clear returns to timeline (scene 1)", result.value("scene", 0u) == 1u,
         "scene=" + std::to_string(result.value("scene", 0u)));

    // frame.state: the show view's live data - the wall plus the resolved
    // layer quads (scene 1 is active: one solid-color layer).
    const bool fsSent = conn.send("frame.state", json::object(), result, error);
    const bool fsShape = fsSent &&
                         result.contains("sceneSize") &&
                         result["sceneSize"].value("width", 0) > 0 &&
                         result.contains("displays") && result["displays"].is_array() &&
                         !result["displays"].empty() &&
                         result.contains("layers") && result["layers"].is_array() &&
                         !result["layers"].empty() &&
                         result["layers"][0].value("content", "") == "color" &&
                         result["layers"][0].contains("corners") &&
                         result["layers"][0]["corners"].size() == 4;
    step("frame.state (wall + resolved layer quads)", fsShape,
         fsSent ? (fsShape ? std::string() : result.dump()) : error);

    step("layer.set opacity 0.5",
         conn.send("layer.set", {{"layer", 10}, {"opacity", 0.5}}, result, error), error);
    step("layer.clear", conn.send("layer.clear", {{"layer", 10}}, result, error), error);

    step("display.list", conn.send("display.list", json::object(), result, error), error);
    if (result.contains("displays") && !result["displays"].empty())
    {
        const Id displayId = static_cast<Id>(result["displays"][0].value("id", 0u));
        step("display.set brightness",
             conn.send("display.set", {{"display", displayId}, {"brightness", 1.0}}, result, error),
             error);
        // Vsync: the result must carry the new value; the engine reacts on
        // its next frame by recreating the display's swapchain (FIFO <->
        // MAILBOX present mode), and the frame phases keep rendering from
        // the recreated swapchain.
        step("display.set vsync off (swapchain -> mailbox)",
             conn.send("display.set", {{"display", displayId}, {"vsync", false}}, result, error) &&
                 result.value("vsync", true) == false,
             error);
        step("display.set vsync on (restore fifo)",
             conn.send("display.set", {{"display", displayId}, {"vsync", true}}, result, error) &&
                 result.value("vsync", false) == true,
             error);
        step("display.warp.set identity",
             conn.send("display.warp.set",
                       {{"display", displayId},
                        {"points", {{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {1.0, 1.0}}}},
                       result,
                       error),
             error);
    }

    step("media.list", conn.send("media.list", json::object(), result, error), error);
    const bool missingLoadFailed =
        !conn.send("media.load", {{"path", "definitely_missing_media_xyz.mp4"}}, result, error);
    step("media.load failure path (expected error)", missingLoadFailed, error);

    // media.thumbnail: the show view's preview path - load a real image
    // (a small BMP written for this check; "mediamax:testpattern:" exists
    // only in the engine's own --self-test mode) and request a 160x90
    // thumbnail (base64 RGBA payload).
    const std::string bmpPath = (std::filesystem::temp_directory_path() /
                                 ("mm_gui_selftest_" +
                                  std::to_string(std::hash<std::thread::id>{}(
                                      std::this_thread::get_id())) +
                                  ".bmp"))
                                    .string();
    // Kept on disk through the frame phases: the auto-pushed show (see
    // testGiveEditorALayer) references it, and the engine loads it from the
    // path when it processes the push. testRemoveSelfTestBmp() removes it.
    m_SelfTestBmpPath = bmpPath;
    const bool bmpWritten = writeThumbnailTestBmp(bmpPath);
    const bool bmpLoadOk =
        bmpWritten && conn.send("media.load", {{"path", bmpPath}}, result, error);
    // Read `error` only after the send (function-argument evaluation order
    // is unspecified; the detail must reflect this call, not the previous).
    step("media.load (test bmp)", bmpLoadOk,
         bmpWritten ? error : "could not write the test bmp");
    const int thumbId = result.value("id", -1);
    {
        const bool sent = conn.send("media.thumbnail",
                                    {{"id", thumbId}, {"width", 160}, {"height", 90}}, result,
                                    error);
        const bool ok = sent && result.value("width", 0) > 0 &&
                        result.value("height", 0) > 0 && !result.value("data", "").empty();
        step("media.thumbnail (160x90, base64 rgba)",
             ok,
             ok ? std::to_string(result.value("width", 0)) + "x" +
                      std::to_string(result.value("height", 0))
                : error);
        if (thumbId > 0)
        {
            // Leave the engine's media library as we found it (the file
            // itself stays for the frame phases - testRemoveSelfTestBmp).
            conn.send("media.unload", {{"id", thumbId}}, result, error);
        }
    }

    step("system.set", conn.send("system.set", {{"name", "gui-self-test"}}, result, error), error);
    step("system.set restored",
         conn.send("system.set", {{"name", systemName.empty() ? std::string("system") : systemName}},
                   result,
                   error),
         error);

    // Leave the engine quiet.
    conn.send("playback.stop", json::object(), result, error);
    return allOk;
}

} // namespace mediamax::gui
