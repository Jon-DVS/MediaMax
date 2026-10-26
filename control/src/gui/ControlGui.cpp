#include "gui/ControlGui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <utility>

#include <imgui.h>
// The one-shot initial dock layout (DockBuilder*) and
// ImGuiDockNodeFlags_DockSpace live in imgui_internal.h in this release.
#include <imgui_internal.h>
// std::string overloads for the text input widgets.
#include <imgui_stdlib.h>

#include "gui/GuiCrash.h"
#include "mediamax/net/ControlClient.h"
#include "mediamax/util/File.h"

namespace mediamax::gui
{

using json = nlohmann::json;

namespace
{

constexpr const char* kBlendModes[] = {"normal", "add", "subtract", "multiply", "screen"};
constexpr int kBlendModeCount = 5;

constexpr const char* kInterpolations[] = {"linear", "step"};
constexpr int kInterpolationCount = 2;

constexpr size_t kLogLineLimit = 400; // max characters kept per console line

const ImVec4 kOkColor = {0.35f, 0.9f, 0.4f, 1.0f};
const ImVec4 kBadColor = {0.9f, 0.35f, 0.35f, 1.0f};
const ImVec4 kWarnColor = {1.0f, 0.8f, 0.3f, 1.0f};

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

ImGuiID modalPopupId(const char* name)
{
    return ImHashStr(name, 0, ImHashStr(kModalHostName));
}

// Maps a test-hook name to the popup's display name (the string used in the
// BeginPopupModal() calls); null for unknown names.
const char* modalPopupName(const std::string& which)
{
    if (which == "open")
    {
        return "Open show...";
    }
    if (which == "saveas")
    {
        return "Save show as...";
    }
    if (which == "pull")
    {
        return "Pull show from system";
    }
    if (which == "about")
    {
        return "About MediaMax Control";
    }
    return nullptr;
}

std::string truncate(const std::string& s, size_t limit)
{
    return s.size() <= limit ? s : s.substr(0, limit) + " ...";
}

} // namespace

ControlGuiApp::ControlGuiApp(std::string systemsPath)
    : m_SystemsPath(std::move(systemsPath))
{
    loadSystemsConfig();
}

ControlGuiApp::~ControlGuiApp()
{
    // SystemConnection destructors stop and join their poll threads.
}

void ControlGuiApp::frame()
{
    if (m_Active != m_LastActive)
    {
        // Reset the transient playback edit state on system switch.
        m_LastActive = m_Active;
        m_HaveNameEdit = false;
        m_SeekHeld = false;
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
    renderMenuBar();
    renderDockspace();
    renderSystemsWindow();
    renderPlaybackWindow();
    renderShowEditorWindow();
    renderConsoleWindow();
    renderModals();
    ImGui::End();
    if (m_FirstFrame)
    {
        m_FirstFrame = false;
    }
}

// --- systems -----------------------------------------------------------------

void ControlGuiApp::loadSystemsConfig()
{
    auto file = readFile(m_SystemsPath);
    if (!file)
    {
        return; // no config: start with an empty list
    }
    json parsed = json::parse(*file, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("systems") || !parsed["systems"].is_array())
    {
        return;
    }
    for (const auto& entry : parsed["systems"])
    {
        m_Systems.push_back(std::make_unique<SystemConnection>(
            entry.value("name", "unnamed"), entry.value("host", "127.0.0.1"),
            static_cast<std::uint16_t>(entry.value("port", 47600))));
        m_Systems.back()->connect(); // auto-connect; the poll thread reports state
    }
    if (!m_Systems.empty())
    {
        m_Active = 0;
        m_LastActive = 0;
    }
}

bool ControlGuiApp::saveSystemsConfig(std::string& error)
{
    json arr = json::array();
    for (const auto& s : m_Systems)
    {
        arr.push_back({{"name", s->name()}, {"host", s->host()}, {"port", s->port()}});
    }
    json doc;
    doc["systems"] = arr;
    if (!writeFile(m_SystemsPath, doc.dump(2)))
    {
        error = "cannot write " + m_SystemsPath;
        return false;
    }
    m_ConfigDirty = false;
    return true;
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
    if (sendActive("show.loadJson", {{"show", m_Editor.documentJson()}}, error))
    {
        m_EditorError.clear();
    }
    else
    {
        m_EditorError = error;
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
            m_OpenModal = true;
            m_OpenSelection = -1;
            refreshOpenFiles();
            ImGui::OpenPopup(modalPopupId("Open show..."));
        }
        const bool canSave = !m_Editor.filePath().empty();
        if (ImGui::MenuItem("Save", nullptr, nullptr, canSave) && canSave)
        {
            std::string error;
            if (!m_Editor.saveFile(m_Editor.filePath(), error))
            {
                m_EditorError = error;
            }
        }
        if (ImGui::MenuItem("Save As..."))
        {
            m_SaveAsModal = true;
            m_SaveAsPath = m_Editor.filePath();
            ImGui::OpenPopup(modalPopupId("Save show as..."));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit"))
        {
            m_ShutdownRequested = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("System"))
    {
        if (ImGui::MenuItem("Save Systems List"))
        {
            std::string error;
            if (saveSystemsConfig(error))
            {
                log("systems list saved to " + m_SystemsPath);
            }
            else
            {
                log("systems list save failed: " + error);
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help"))
    {
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
        ImGuiID bottom = 0;
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, &bottom, &center);

        ImGui::DockBuilderDockWindow("Systems", left);
        ImGui::DockBuilderDockWindow("Show Editor", right);
        ImGui::DockBuilderDockWindow("Playback", center);
        ImGui::DockBuilderDockWindow("Console", bottom);
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
    m_ConfigDirty = true;
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
    if (!ImGui::Begin("Systems"))
    {
        ImGui::End();
        return;
    }
    ImGui::InputText("Config file", &m_SystemsPath);
    ImGui::SameLine();
    if (ImGui::Button("Save List##systems") || m_ConfigDirty)
    {
        std::string error;
        if (saveSystemsConfig(error))
        {
            log("systems list saved to " + m_SystemsPath);
        }
        else
        {
            log("systems list save failed: " + error);
        }
    }
    if (m_ConfigDirty)
    {
        ImGui::SameLine();
        ImGui::TextColored(kWarnColor, "(unsaved)");
    }
    ImGui::Spacing();
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
            ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed, 118.0f);
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
                    m_ConfigDirty = true;
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

// --- playback window ----------------------------------------------------------------

void ControlGuiApp::renderPlaybackWindow()
{
    // Begin() may return false; End() must still be called (see above).
    if (!ImGui::Begin("Playback"))
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
    ImGui::Separator();
    renderLayersBlock(snap);
    ImGui::Separator();
    renderDisplaysBlock(snap);
    ImGui::Separator();
    renderMediaBlock(snap);
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

    ImGui::Text("State: %s     %s     %s fps     scene %u", state.c_str(), timecode.c_str(),
                frameRate.c_str(), sceneId);
    if (duration < 0.0)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%.2f s / unlimited", time);
    }
    else
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%.2f / %.2f s", time, duration);
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
            m_Seek = time; // follow the live time while not dragging
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
    for (int i = 0; i < static_cast<int>(scenes.size()); ++i)
    {
        const json& s = scenes[static_cast<size_t>(i)];
        const std::string label = std::to_string(s.value("id", 0u)) + " - " + s.value("name", "?");
        ImGui::PushID(i);
        if (ImGui::Button(label.c_str(), ImVec2(-FLT_MAX, 0.0f)))
        {
            std::string error;
            sendActive("scene.set", {{"index", i}}, error);
        }
        if (s.value("active", false))
        {
            ImGui::SameLine();
            ImGui::TextColored(kOkColor, "active");
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Return to timeline (scene.clear)", ImVec2(-FLT_MAX, 0.0f)))
    {
        std::string error;
        sendActive("scene.clear", json::object(), error);
    }
}

void ControlGuiApp::renderLayersBlock(const SystemConnection::Snapshot& snap)
{
    ImGui::TextUnformatted("Layers (live overrides, active scene)");
    const json* activeScene = nullptr;
    if (snap.scenes.contains("scenes") && snap.scenes["scenes"].is_array())
    {
        for (const auto& s : snap.scenes["scenes"])
        {
            if (s.value("active", false))
            {
                activeScene = &s;
                break;
            }
        }
    }
    if (!activeScene || !activeScene->contains("layers"))
    {
        ImGui::TextDisabled("(no active scene)");
        return;
    }
    for (const auto& layer : (*activeScene)["layers"])
    {
        const Id layerId = static_cast<Id>(layer.value("id", 0u));
        const std::string name = layer.value("name", "?");
        ImGui::PushID(static_cast<int>(layerId));
        ImGui::Text("Layer %u - %s", layerId, name.c_str());
        LayerLive& lv = m_LayerLive[layerId];
        if (ImGui::SliderFloat("Opacity##op", &lv.opacity, 0.0f, 1.0f, "%.2f"))
        {
            std::string error;
            sendActive("layer.set", {{"layer", layerId}, {"opacity", lv.opacity}}, error);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Visible##vis", &lv.visible))
        {
            std::string error;
            sendActive("layer.set", {{"layer", layerId}, {"visible", lv.visible}}, error);
        }
        ImGui::SameLine();
        if (ImGui::Combo("Blend##blend", &lv.blend, kBlendModes, kBlendModeCount))
        {
            std::string error;
            sendActive("layer.set", {{"layer", layerId}, {"blend", kBlendModes[lv.blend]}}, error);
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear##clr"))
        {
            std::string error;
            sendActive("layer.clear", {{"layer", layerId}}, error);
        }
        ImGui::InputFloat("x##lx", &lv.x, 10.0f, 50.0f);
        ImGui::SameLine();
        ImGui::InputFloat("y##ly", &lv.y, 10.0f, 50.0f);
        ImGui::SameLine();
        if (ImGui::Button("Set position##pos"))
        {
            std::string error;
            sendActive("layer.set", {{"layer", layerId}, {"x", lv.x}, {"y", lv.y}}, error);
        }
        ImGui::PopID();
    }
}

void ControlGuiApp::renderDisplaysBlock(const SystemConnection::Snapshot& snap)
{
    ImGui::TextUnformatted("Displays");
    if (!snap.status.contains("displays") || snap.status["displays"].empty())
    {
        ImGui::TextDisabled("(none)");
        return;
    }
    const json& displays = snap.status["displays"];

    if (ImGui::BeginTable("displays", 9,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersOuter))
    {
        ImGui::TableSetupColumn("##id", ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableSetupColumn("name");
        ImGui::TableSetupColumn("##enabled", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("x", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("y", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("w", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("h", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("brightness", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        for (const auto& d : displays)
        {
            const Id id = static_cast<Id>(d.value("id", 0u));
            const std::string name = d.value("name", "display");
            DisplayEdit& de = m_DisplayEdits[id];
            if (!de.valid)
            {
                de.x = static_cast<int>(d.value("x", 0));
                de.y = static_cast<int>(d.value("y", 0));
                de.width = static_cast<int>(d.value("width", 1920));
                de.height = static_cast<int>(d.value("height", 1080));
                de.brightness = static_cast<float>(d.value("brightness", 1.0));
                de.enabled = d.value("enabled", true);
                de.valid = true;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", id);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name.c_str());
            ImGui::TableNextColumn();
            ImGui::Checkbox("on##en", &de.enabled);
            ImGui::TableNextColumn();
            ImGui::InputInt("##x", &de.x);
            ImGui::TableNextColumn();
            ImGui::InputInt("##y", &de.y);
            ImGui::TableNextColumn();
            ImGui::InputInt("##w", &de.width);
            ImGui::TableNextColumn();
            ImGui::InputInt("##h", &de.height);
            ImGui::TableNextColumn();
            ImGui::SliderFloat("##bright", &de.brightness, 0.0f, 1.0f, "%.2f");
            ImGui::TableNextColumn();
            if (ImGui::Button("Set##set"))
            {
                std::string error;
                sendActive("display.set",
                           {{"display", id}, {"x", de.x}, {"y", de.y}, {"width", de.width},
                            {"height", de.height}, {"brightness", de.brightness},
                            {"enabled", de.enabled}},
                           error);
            }
        }
        ImGui::EndTable();
    }

    // Warp grid editor per display.
    for (const auto& d : displays)
    {
        const Id id = static_cast<Id>(d.value("id", 0u));
        const std::string name = d.value("name", "display");
        if (!ImGui::CollapsingHeader(("Warp - " + name).c_str()))
        {
            continue;
        }
        ImGui::PushID(static_cast<int>(id));
        WarpEdit& we = m_WarpEdits[id];
        if (!we.valid)
        {
            we.n = 2;
            we.points = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
            we.valid = true;
        }
        auto resetToIdentity = [&we]() {
            for (size_t p = 0; p < we.points.size(); ++p)
            {
                const int row = static_cast<int>(p / we.n);
                const int col = static_cast<int>(p % we.n);
                we.points[p] = {static_cast<float>(col / (we.n - 1)),
                                static_cast<float>(row / (we.n - 1))};
            }
        };
        const char* grids[] = {"2x2", "3x3", "4x4"};
        int grid = we.n - 2;
        if (ImGui::Combo("Grid##grid", &grid, grids, 3))
        {
            we.n = grid + 2;
            we.points.clear();
            for (int row = 0; row < we.n; ++row)
            {
                for (int col = 0; col < we.n; ++col)
                {
                    we.points.push_back(
                        {static_cast<float>(col / (we.n - 1)), static_cast<float>(row / (we.n - 1))});
                }
            }
        }
        if (we.points.size() == static_cast<size_t>(we.n * we.n))
        {
            if (ImGui::BeginTable("warppoints", 3, ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("point");
                ImGui::TableSetupColumn("u");
                ImGui::TableSetupColumn("v");
                for (size_t p = 0; p < we.points.size(); ++p)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", static_cast<int>(p));
                    ImGui::TableNextColumn();
                    ImGui::InputFloat("##u", &we.points[p][0], 0.05f, 0.2f, "%.3f");
                    ImGui::TableNextColumn();
                    ImGui::InputFloat("##v", &we.points[p][1], 0.05f, 0.2f, "%.3f");
                }
                ImGui::EndTable();
            }
        }
        if (ImGui::Button("Identity##warp"))
        {
            resetToIdentity();
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply warp##warp"))
        {
            json pts = json::array();
            for (const auto& pt : we.points)
            {
                pts.push_back({pt[0], pt[1]});
            }
            std::string error;
            sendActive("display.warp.set", {{"display", id}, {"points", pts}}, error);
        }
        ImGui::PopID();
    }
}

void ControlGuiApp::renderMediaBlock(const SystemConnection::Snapshot& snap)
{
    ImGui::TextUnformatted("Media (engine library)");
    if (snap.status.contains("media"))
    {
        if (ImGui::BeginTable("media", 6,
                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersOuter))
        {
            ImGui::TableSetupColumn("##id", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("type");
            ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("duration", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("path");
            ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 64.0f);
            for (const auto& m : snap.status["media"])
            {
                const Id id = static_cast<Id>(m.value("id", 0u));
                ImGui::PushID(static_cast<int>(id));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", id);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m.value("type", "?").c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%dx%d", static_cast<int>(m.value("width", 0)),
                            static_cast<int>(m.value("height", 0)));
                ImGui::TableNextColumn();
                ImGui::Text("%.2fs", m.value("duration", 0.0));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(truncate(m.value("path", ""), 80).c_str());
                ImGui::TableNextColumn();
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

// --- show editor window ---------------------------------------------------------------

void ControlGuiApp::renderShowEditorWindow()
{
    // Begin() may return false; End() must still be called (see above).
    if (!ImGui::Begin("Show Editor"))
    {
        ImGui::End();
        return;
    }
    if (ImGui::Button("New"))
    {
        m_Editor.newShow();
        m_EditorError.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open..."))
    {
        m_OpenModal = true;
        m_OpenSelection = -1;
        refreshOpenFiles();
        ImGui::OpenPopup(modalPopupId("Open show..."));
    }
    ImGui::SameLine();
    const bool canSave = !m_Editor.filePath().empty();
    ImGui::BeginDisabled(!canSave);
    if (ImGui::Button("Save") && canSave)
    {
        std::string error;
        if (!m_Editor.saveFile(m_Editor.filePath(), error))
        {
            m_EditorError = error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Save As..."))
    {
        m_SaveAsModal = true;
        m_SaveAsPath = m_Editor.filePath();
        ImGui::OpenPopup(modalPopupId("Save show as..."));
    }
    ImGui::SameLine();
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

    ImGui::Spacing();
    const std::string where =
        m_Editor.filePath().empty() ? std::string("(unsaved)") : m_Editor.filePath();
    ImGui::Text("%s%s", where.c_str(), m_Editor.dirty() ? "  *dirty" : "");
    if (!m_EditorError.empty())
    {
        ImGui::TextColored(kBadColor, "%s", m_EditorError.c_str());
    }
    ImGui::Separator();

    if (ImGui::BeginTabBar("editortabs"))
    {
        if (ImGui::BeginTabItem("Show"))
        {
            renderShowTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Media"))
        {
            renderMediaTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Scenes"))
        {
            renderScenesTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Cues"))
        {
            renderCuesTab();
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
        Cue cue;
        cue.time = show.cues.empty() ? Time::zero() : show.cues.back().time;
        cue.name = "cue";
        const Id sceneId = show.scenes.empty() ? 1 : show.scenes.front().id;
        cue.action = PlayScene{sceneId};
        show.addCue(std::move(cue));
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
    if (!ImGui::Begin("Console"))
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

// --- modals ---------------------------------------------------------------------------

void ControlGuiApp::refreshOpenFiles()
{
    m_OpenFiles.clear();
    std::error_code ec;
    auto it = std::filesystem::directory_iterator(m_OpenDir, ec);
    if (ec)
    {
        return;
    }
    for (; it != std::filesystem::directory_iterator(); it.increment(ec))
    {
        std::error_code fec;
        if (it->is_regular_file(fec) && it->path().extension() == ".json")
        {
            m_OpenFiles.push_back(it->path().string());
        }
    }
    std::sort(m_OpenFiles.begin(), m_OpenFiles.end());
}

void ControlGuiApp::renderModals()
{
    if (m_OpenModal)
    {
        if (ImGui::BeginPopupModal("Open show...", &m_OpenModal))
        {
            ImGui::InputTextWithHint("##opendir", "directory", &m_OpenDir);
            ImGui::SameLine();
            if (ImGui::Button("Refresh##open"))
            {
                refreshOpenFiles();
            }
            if (ImGui::BeginChild("openfiles", ImVec2(-FLT_MAX, 220.0f), true))
            {
                for (size_t i = 0; i < m_OpenFiles.size(); ++i)
                {
                    if (ImGui::Selectable(m_OpenFiles[i].c_str(),
                                          m_OpenSelection == static_cast<int>(i)))
                    {
                        m_OpenSelection = static_cast<int>(i);
                    }
                }
                if (m_OpenFiles.empty())
                {
                    ImGui::TextDisabled("(no .json files)");
                }
            }
            ImGui::EndChild();
            if (ImGui::Button("Open##do") && m_OpenSelection >= 0 &&
                m_OpenSelection < static_cast<int>(m_OpenFiles.size()))
            {
                std::string error;
                if (m_Editor.loadFile(m_OpenFiles[static_cast<size_t>(m_OpenSelection)], error))
                {
                    m_EditorError.clear();
                    m_OpenModal = false;
                }
                else
                {
                    m_EditorError = error;
                }
            }
            ImGui::EndPopup();
        }
    }
    if (m_SaveAsModal)
    {
        if (ImGui::BeginPopupModal("Save show as...", &m_SaveAsModal))
        {
            ImGui::InputText("Path", &m_SaveAsPath);
            if (ImGui::Button("Save##saveas") && !m_SaveAsPath.empty())
            {
                std::string error;
                if (m_Editor.saveFile(m_SaveAsPath, error))
                {
                    m_EditorError.clear();
                    m_SaveAsModal = false;
                }
                else
                {
                    m_EditorError = error;
                }
            }
            ImGui::EndPopup();
        }
    }
    if (m_PullModal)
    {
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
}

// --- test hooks -----------------------------------------------------------------

bool ControlGuiApp::testOpenModal(const std::string& which)
{
    if (modalPopupName(which) == nullptr)
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
    // The same state the UI buttons/menus set before calling OpenPopup().
    // Must run inside frame() (the modal host window is current).
    const char* name = modalPopupName(which);
    if (name == nullptr)
    {
        return;
    }
    if (which == "open")
    {
        m_OpenModal = true;
        m_OpenSelection = -1;
        refreshOpenFiles();
    }
    else if (which == "saveas")
    {
        m_SaveAsModal = true;
        m_SaveAsPath = m_Editor.filePath();
    }
    else if (which == "pull")
    {
        m_PullModal = true;
    }
    else
    {
        m_AboutModal = true;
    }
    ImGui::OpenPopup(modalPopupId(name));
}

bool ControlGuiApp::testModalOpen(const std::string& which) const
{
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
    // Clearing the app-side flag is the documented close path: BeginPopupModal
    // sees *p_open == false and calls ClosePopupToLevel() itself.
    if (which == "open")
    {
        m_OpenModal = false;
        return true;
    }
    if (which == "saveas")
    {
        m_SaveAsModal = false;
        return true;
    }
    if (which == "pull")
    {
        m_PullModal = false;
        return true;
    }
    if (which == "about")
    {
        m_AboutModal = false;
        return true;
    }
    return false;
}

// --- self test ---------------------------------------------------------------------------

bool ControlGuiApp::selfTestSystem(const std::string& host, std::uint16_t port, std::string& report)
{
    // Drives the exact same command path the UI uses (SystemConnection),
    // against a running playback engine. The target show is replaced by a
    // deterministic one; run it against a scratch playback instance.
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

    step("scene.clear", conn.send("scene.clear", json::object(), result, error), error);
    step("scene.clear returns to timeline (scene 1)", result.value("scene", 0u) == 1u,
         "scene=" + std::to_string(result.value("scene", 0u)));

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
        step("display.warp.set identity",
             conn.send("display.warp.set",
                       {{"display", displayId},
                        {"points", {{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {1.0, 1.0}}}},
                       result,
                       error),
             error);
    }

    step("media.list", conn.send("media.list", json::object(), result, error), error);
    step("media.load failure path (expected error)",
         !conn.send("media.load", {{"path", "definitely_missing_media_xyz.mp4"}}, result, error),
         error);

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
