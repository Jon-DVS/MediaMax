#include "gui/GuiMain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "gui/ControlGui.h"
#include "gui/GuiCrash.h"
// Vulkan before GLFW: the GLFW header includes the Vulkan header (and the
// platform surface macro must already be in effect for the surface structs).
#include "gui/GuiVulkan.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32 // the native accessor section is opt-in
#include <GLFW/glfw3native.h> // glfwGetWin32Window
#include <dwmapi.h> // DwmSetWindowAttribute (dark title bar)
#endif

namespace mediamax::gui
{

namespace
{

void glfwErrorCallback(int error, const char* description)
{
    std::cerr << "GLFW error " << error << ": " << description << std::endl;
}

// OS file drop: GLFW's drop callback is a plain C function (no user pointer),
// and the app has exactly one window, so a file-scope queue is the channel
// between the callback and the per-frame payload emission in
// GuiRuntime::renderFrame. The dropped paths wait there until a frame
// publishes them as an external drag payload; the dockspace's drop target
// (ControlGuiApp::renderDockspace) consumes them on delivery.
std::vector<std::string> g_PendingDropPaths;
void fileDropCallback(GLFWwindow*, int count, const char** paths)
{
    g_PendingDropPaths.clear();
    for (int i = 0; i < count; ++i)
    {
        g_PendingDropPaths.emplace_back(paths[i]);
    }
}

// The UI is dark-themed; opt the window frame into dark mode so it does not
// show a white title bar over a near-black client area on light Windows
// themes. Best effort: a failure (older OS) just leaves the OS default.
void applyDarkTitleBar(GLFWwindow* window)
{
#if defined(_WIN32)
    // DWMWA_USE_IMMERSIVE_DARK_MODE: the constant was renamed in newer SDKs,
    // so use the literal (20) to stay source-compatible.
    constexpr DWORD kDarkModeAttribute = 20;
    const int enabled = 1;
    if (HWND hwnd = glfwGetWin32Window(window))
    {
        DwmSetWindowAttribute(hwnd, kDarkModeAttribute, &enabled, sizeof(enabled));
    }
#endif
}

// Shared frame renderer for the windowed and self-test loops: owns the
// Vulkan surface + the Dear ImGui back ends and renders one frame of
// ControlGuiApp into the window.
class GuiRuntime
{
public:
    bool init(GLFWwindow* window, ControlGuiApp* app, std::string& error);
    void shutdown();
    bool renderFrame();
    // Runs just before ImGui::NewFrame() (after the back end has polled the
    // real input): the self-test uses it to inject synthetic key events
    // (ImGuiIO::AddKeyEvent must land before NewFrame) so the hotkey path
    // can be exercised without physical input.
    std::function<void()> preNewFrameHook;
    // Vulkan handles for the self-test (it creates the staging buffer the
    // show view's thumbnail readback copies into).
    VkDevice device() const { return m_Vulkan.device(); }
    VkPhysicalDevice physicalDevice() const { return m_Vulkan.physicalDevice(); }
    // Blocks until the last rendered frame's command buffer completes (the
    // self-test's readback maps the copied pixels after this returns).
    void waitForFrame() { m_Vulkan.waitForFrame(); }

private:
    GLFWwindow* m_Window = nullptr;
    GuiVulkan m_Vulkan;
    ControlGuiApp* m_App = nullptr;
    bool m_ImGuiReady = false;
    // Set when the swapchain is stale (OUT_OF_DATE/SUBOPTIMAL from acquire
    // or present): forces a recreate on the next frame, even at unchanged
    // window size.
    bool m_ResizePending = false;
};

bool GuiRuntime::init(GLFWwindow* window, ControlGuiApp* app, std::string& error)
{
    m_Window = window;
    m_App = app;
    if (!m_Vulkan.init(window, error))
    {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Docking is an opt-in build feature: without this flag DockSpace() is a
    // no-op and the DockBuilder* calls below crash on a NULL node.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr; // the layout is deterministic (rebuilt each start)
    // Popups, modals and the file dialogs must be dragged by their top
    // window bar only: with Dear ImGui's default, a press-and-move on any
    // non-interactive part of the window grabs the window itself.
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplGlfw_InitForVulkan(window, true))
    {
        error = "Dear ImGui GLFW backend init failed";
        ImGui::DestroyContext();
        m_Vulkan.shutdown();
        return false;
    }

    ImGui_ImplVulkan_InitInfo info = {};
    info.ApiVersion = VK_API_VERSION_1_0;
    info.Instance = m_Vulkan.instance();
    info.PhysicalDevice = m_Vulkan.physicalDevice();
    info.Device = m_Vulkan.device();
    info.QueueFamily = m_Vulkan.queueFamily();
    info.Queue = m_Vulkan.queue();
    // The back end creates its own descriptor pool: every
    // ImGui_ImplVulkan_AddTexture consumes one
    // VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE descriptor from it (the font texture
    // plus the show view's thumbnails - one per media). 8 would exhaust on a
    // wall with a handful of media, and every later AddTexture would fail
    // silently (no CheckVkResultFn is set), drawing the white fallback
    // instead of the thumbnail; 128 keeps real shows covered.
    info.DescriptorPoolSize = 128;
    info.MinImageCount = 2;
    info.ImageCount = static_cast<std::uint32_t>(m_Vulkan.imageCount());
    info.PipelineInfoMain.RenderPass = m_Vulkan.renderPass();
    info.PipelineInfoMain.Subpass = 0;
    info.UseDynamicRendering = false;
    if (!ImGui_ImplVulkan_Init(&info))
    {
        error = "Dear ImGui Vulkan backend init failed";
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        m_Vulkan.shutdown();
        return false;
    }
    // The show view's texture pool (its AddTexture needs the back end's
    // state, hence after ImGui_ImplVulkan_Init; a failure degrades the view
    // to untextured previews, it does not abort the GUI).
    app->initShowView(m_Vulkan.device(), m_Vulkan.physicalDevice());
    m_ImGuiReady = true;
    return true;
}

void GuiRuntime::shutdown()
{
    if (m_ImGuiReady)
    {
        vkDeviceWaitIdle(m_Vulkan.device()); // no frame in flight
        // The pool's RemoveTexture uses the back end's state: release the
        // pool before the back end goes, and both before the device.
        m_App->shutdownShowView();
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        m_ImGuiReady = false;
    }
    m_Vulkan.shutdown();
}

bool GuiRuntime::renderFrame()
{
    // Resize the swap chain? Polling the size each frame (instead of a
    // resize callback) matches the official example; the size must be
    // non-zero - a minimized window reports 0x0 and its frames are skipped
    // entirely by the caller.
    int fbWidth = 0;
    int fbHeight = 0;
    glfwGetFramebufferSize(m_Window, &fbWidth, &fbHeight);
    if (m_Vulkan.ready() && fbWidth > 0 && fbHeight > 0 &&
        (m_ResizePending || m_Vulkan.width() != static_cast<std::uint32_t>(fbWidth) ||
         m_Vulkan.height() != static_cast<std::uint32_t>(fbHeight)))
    {
        // The back end's minimum is fixed at init (2); this mirrors the
        // example's call and is a no-op while it is unchanged.
        ImGui_ImplVulkan_SetMinImageCount(2);
        m_Vulkan.recreateSwapchain();
        m_ResizePending = false;
    }

    // The window is per-monitor-DPI-aware (GLFW enables PMv2 at init):
    // scale the UI to the system scale so text and widgets keep their
    // intended physical size (the dockspace fills the viewport, so the
    // layout ratios are unaffected).
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor())
    {
        float xScale = 1.0f;
        float yScale = 1.0f;
        glfwGetMonitorContentScale(monitor, &xScale, &yScale);
        ImGui::GetIO().FontGlobalScale = yScale;
    }

    // NewFrame order as in the official example (the Vulkan back end's
    // NewFrame is a no-op in this ImGui version but kept for parity).
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    if (preNewFrameHook)
    {
        preNewFrameHook(); // self-test key injection (see the member)
    }
    ImGui::NewFrame();
    // OS file drop delivered while polling this frame: publish the paths as
    // an EXTERNAL drag payload (this ImGui build has no OS file-drop API, so
    // the GLFW callback and the dockspace's drop target meet through the
    // classic drag/drop payload mechanism). An extern payload is consumed on
    // the frame AFTER the one it was emitted in (AcceptDragDropPayload flips
    // Delivery when the source's frame count is older than the current
    // frame and the target already accepted last frame): emit it exactly
    // once, here - re-emitting every frame would keep the source frame count
    // current and the payload would never deliver.
    if (!g_PendingDropPaths.empty())
    {
        std::string blob;
        for (const auto& path : g_PendingDropPaths)
        {
            blob += path;
            blob += '\0';
        }
        g_PendingDropPaths.clear();
        ImGui::BeginDragDropSource(
            ImGuiDragDropFlags_SourceExtern | ImGuiDragDropFlags_SourceNoPreviewTooltip);
        ImGui::SetDragDropPayload("MEDIMAX_FILEPATHS", blob.data(), blob.size());
        ImGui::EndDragDropSource();
    }
    m_App->frame();
    ImGui::Render();

    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f)
    {
        // No presentable size (e.g. iconified): skip render + present
        // (the example does the same).
        return true;
    }

    if (!m_Vulkan.beginFrame())
    {
        // The swapchain was stale (or missing): rebuild and retry next frame.
        m_ResizePending = true;
        return false;
    }
    // The show view's thumbnail uploads (created during frame()): transfer
    // commands, so they must be recorded outside the render pass - after
    // beginFrame(), before beginRenderPass() - and before the UI draw data
    // that samples them.
    m_App->flushShowTextureUploads(m_Vulkan.commandBuffer());
    m_Vulkan.beginRenderPass();
    ImGui_ImplVulkan_RenderDrawData(drawData, m_Vulkan.commandBuffer());
    VkResult presentResult = VK_SUCCESS;
    m_Vulkan.endFrame(presentResult);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
    {
        m_ResizePending = true;
    }
    return true;
}

} // namespace

int runGui()
{
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit())
    {
        std::cerr << "mediamax_control: glfwInit failed" << std::endl;
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // Size the initial window for the display's content scale (hiDPI).
    float xScale = 1.0f;
    float yScale = 1.0f;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor())
    {
        glfwGetMonitorContentScale(monitor, &xScale, &yScale);
    }
    const int width = static_cast<int>(1600 * xScale);
    const int height = static_cast<int>(900 * yScale);
    GLFWwindow* window = glfwCreateWindow(width, height, "MediaMax Control", nullptr, nullptr);
    if (!window)
    {
        std::cerr << "mediamax_control: could not create the GUI window" << std::endl;
        glfwTerminate();
        return 1;
    }
    // OS file drop: files dropped on the window add media to the show (the
    // callback queues the paths; renderFrame publishes them as a payload).
    glfwSetDropCallback(window, fileDropCallback);

    ControlGuiApp app;
    GuiRuntime runtime;
    std::string error;
    if (!runtime.init(window, &app, error))
    {
        std::cerr << "mediamax_control: GUI init failed: " << error << std::endl;
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    applyDarkTitleBar(window);
    installCrashDiagnostics(); // never die silently: report + message box

    while (!glfwWindowShouldClose(window) && !app.shutdownRequested())
    {
        glfwPollEvents();
        // A minimized window has no presentable extent: skip the frame
        // entirely (the official example does the same).
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED))
        {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }
        runtime.renderFrame();
    }

    runtime.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

int runGuiSelfTest(int frames, const std::string& liveSystem)
{
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit())
    {
        std::cerr << "mediamax_control: glfwInit failed" << std::endl;
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); // hidden window
    // 1080p hidden surface: the show editor's layer details (transform +
    // appearance + the keyframes submenu) need the full height to render
    // unclipped, which the self-test's real-UI clicks require.
    GLFWwindow* window = glfwCreateWindow(1280, 1080, "MediaMax Control (self-test)", nullptr,
                                          nullptr);
    if (!window)
    {
        std::cerr << "mediamax_control: could not create the self-test window" << std::endl;
        glfwTerminate();
        return 1;
    }
    // Same OS file drop plumbing as the windowed GUI (the hidden window
    // never receives drops, but the frame path stays identical).
    glfwSetDropCallback(window, fileDropCallback);

    // The self-test UI starts with no systems (deterministic; no side
    // effects on real systems).
    ControlGuiApp app;
    GuiRuntime runtime;
    std::string error;
    if (!runtime.init(window, &app, error))
    {
        std::cerr << "mediamax_control: GUI self-test init failed: " << error << std::endl;
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    // 1) The scripted live command sequence (same path the UI uses), when a
    //    running playback engine was given.
    bool liveOk = true;
    std::string liveReport;
    std::string liveHost;
    std::uint16_t livePort = 0;
    bool haveLiveSystem = false;
    std::string autoPushName;
    if (!liveSystem.empty())
    {
        const size_t colon = liveSystem.rfind(':');
        if (colon == std::string::npos)
        {
            std::cerr << "mediamax_control: --gui-self-test-system expects host:port" << std::endl;
            runtime.shutdown();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 2;
        }
        liveHost = liveSystem.substr(0, colon);
        livePort = 47600;
        try
        {
            livePort = static_cast<std::uint16_t>(std::stoi(liveSystem.substr(colon + 1)));
        }
        catch (const std::exception&)
        {
            std::cerr << "mediamax_control: bad port in " << liveSystem << std::endl;
            runtime.shutdown();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 2;
        }
        liveOk = app.selfTestSystem(liveHost, livePort, liveReport);
        haveLiveSystem = true;
        if (liveOk)
        {
            // Edit the editor's show: the frames below give the debounced
            // auto-push time to run, and step 4 verifies the system received
            // the edit (issue 8).
            autoPushName = "AutoPush self-test";
            app.testTouchEditor(autoPushName);
            // A deterministic layer: the auto-push below delivers this show
            // to the engine, so the frame phases render a real layer in the
            // show view (the live check above inspected the same command
            // path via frame.state).
            app.testGiveEditorALayer();
        }
    }

    // 2) Render N full UI frames into the hidden window.
    int rendered = 0;
    for (int i = 0; i < frames; ++i)
    {
        glfwPollEvents();
        if (runtime.renderFrame())
        {
            ++rendered;
        }
    }

    // 2b) Default dock layout: Playback and Console must share one tab
    //     block, in the order Playback, Console (renderDockspace).
    const bool layoutOk = app.testDockLayoutPlaybackConsoleTabs();
    std::cout << (layoutOk ? "PASS" : "FAIL")
              << " default layout tabs Playback and Console together (order: Playback, Console)"
              << std::endl;

    // 3) Modal dialogs: open each dialog the UI buttons open and verify
    //    Dear ImGui actually shows it, then verify it closes again. This is
    //    the regression guard for "clicking Open / Save As / About does
    //    nothing": the modal BeginPopupModal() calls must run in a window
    //    context whose popup ids match the OpenPopup() ids (see
    //    kModalHostName in ControlGui.cpp).
    std::string modalReport;
    bool modalOk = true;
    // Windows are movable by their title bar only (GuiRuntime::init sets
    // io.ConfigWindowsMoveFromTitleBarOnly): a content-area press must not
    // drag the popup with it.
    const bool titleBarOnly = ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly;
    modalOk = modalOk && titleBarOnly;
    modalReport += std::string(titleBarOnly ? "PASS " : "FAIL ") +
                   "windows are movable only by their title bar\n";
    // The Display settings / warp dialogs edit show-document entries (the
    // persistent wall), so the modal loop below needs one: headless adds
    // one through the offline Add display button (which must work with no
    // system connected), live mirrors the engine's live display into the
    // document. The System column's mapping is checked on the same entry:
    // the offline one is "unmapped", the mirrored one maps to its system.
    if (haveLiveSystem)
    {
        // The active system's snapshot polls every 500 ms; wait until it
        // reports displays before seeding from it.
        bool snapHasDisplay = false;
        const auto seedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!snapHasDisplay && std::chrono::steady_clock::now() < seedDeadline)
        {
            snapHasDisplay = app.testActiveHasDisplay();
            if (!snapHasDisplay)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
        }
        const int seeded = snapHasDisplay ? app.testSeedDisplaysFromActive() : 0;
        const bool seededOk = seeded > 0;
        modalOk = modalOk && seededOk;
        modalReport += std::string(seededOk ? "PASS " : "FAIL ") +
                       "display wall seeded into the show document (live mirror, " +
                       std::to_string(seeded) + " entries)\n";
        const std::string mapped = app.testDisplayMappedSystem("Display 1");
        const bool mappedOk = mapped == "self-test";
        modalOk = modalOk && mappedOk;
        modalReport += std::string(mappedOk ? "PASS " : "FAIL ") +
                       "seeded display is mapped to its live system ('" + mapped + "')\n";
    }
    else
    {
        const Id added = app.testAddDisplay();
        const bool addedOk = added != kInvalidId;
        modalOk = modalOk && addedOk;
        modalReport += std::string(addedOk ? "PASS " : "FAIL ") +
                       "Add display works with no system connected (document entry, id " +
                       std::to_string(added) + ")\n";
        const std::string mapped = app.testDisplayMappedSystem("Display 1");
        const bool mappedOk = mapped.empty();
        modalOk = modalOk && mappedOk;
        modalReport += std::string(mappedOk ? "PASS " : "FAIL ") +
                       "offline display entry is marked unmapped\n";
    }
    std::vector<const char*> modalNames = {"about", "open", "saveas", "media", "pull", "edit",
                                           "warp", "mediadetails", "hotkeys"};
    if (haveLiveSystem)
    {
        // The System settings dialog's button sits on a system row; the
        // headless run has no systems, so it is not openable there.
        modalNames.push_back("syssettings");
    }
    for (const char* which : modalNames)
    {
        if (!app.testOpenModal(which))
        {
            modalOk = false;
            modalReport += "FAIL modal " + std::string(which) + " - unknown modal name\n";
            continue;
        }
        bool shown = false;
        for (int i = 0; i < 3 && !shown; ++i)
        {
            glfwPollEvents();
            if (runtime.renderFrame())
            {
                shown = app.testModalOpen(which);
            }
        }
        app.testCloseModal(which);
        bool stillOpen = true;
        for (int i = 0; i < 3 && stillOpen; ++i)
        {
            glfwPollEvents();
            runtime.renderFrame();
            stillOpen = app.testModalOpen(which);
        }
        const bool ok = shown && !stillOpen;
        modalOk = modalOk && ok;
        modalReport += (ok ? "PASS modal " : "FAIL modal ") + std::string(which);
        if (!shown)
        {
            modalReport += " - did not open";
        }
        else if (stillOpen)
        {
            modalReport += " - did not close";
        }
        modalReport += "\n";
    }

    // 3a) Displays tree (Displays tab rework): the tab renders one labeled
    //     tree node per document display (the old table is gone). The
    //     offline entry added above must appear in it.
    {
        const auto& entries = app.testDisplaysTreeEntries();
        std::string names;
        for (const auto& n : entries)
        {
            if (!names.empty())
            {
                names += ", ";
            }
            names += n;
        }
        const bool treeOk = !entries.empty() && entries.front() == "Display 1";
        modalOk = modalOk && treeOk;
        modalReport += std::string(treeOk ? "PASS " : "FAIL ") +
                       "Displays tab is a tree view (entries: " + names + ")\n";
    }
    // 3b) Warp editor pad: open it on the document's display and verify
    //     the pad's view state - aspect-ratioed to the display (the
    //     offline entry is 1920x1080 => 16:9), the whole-lattice view
    //     (zoom 1, pan 0), the nodes' screen positions and the reset
    //     button - then close it again.
    {
        app.testOpenModal("warp");
        bool shown = false;
        for (int i = 0; i < 3 && !shown; ++i)
        {
            glfwPollEvents();
            if (runtime.renderFrame())
            {
                shown = app.testModalOpen("warp");
            }
        }
        float aspect = 0.0f;
        float zoom = 0.0f;
        ImVec2 pan{0.0f, 0.0f};
        float nodeX = 0.0f;
        float nodeY = 0.0f;
        float resetX = 0.0f;
        float resetY = 0.0f;
        const bool infoOk =
            shown && app.testWarpEditorOpen() && app.testWarpPadInfo(aspect, zoom, pan);
        const bool viewOk = infoOk &&
                            std::abs(aspect - 1920.0f / 1080.0f) < 1e-3f && // 16:9 pad
                            zoom == 1.0f && pan.x == 0.0f && pan.y == 0.0f;
        int winW = 0;
        int winH = 0;
        glfwGetFramebufferSize(window, &winW, &winH);
        const bool nodeOk = app.testWarpNodeScreenPos(0, nodeX, nodeY) && nodeX > 0.0f &&
                            nodeY > 0.0f && nodeX < static_cast<float>(winW) &&
                            nodeY < static_cast<float>(winH);
        const bool resetOk = app.testWarpResetButtonPos(resetX, resetY) && resetX > 0.0f &&
                             resetY > 0.0f;
        const bool padOk = shown && infoOk && viewOk && nodeOk && resetOk;
        modalOk = modalOk && padOk;
        modalReport += std::string(padOk ? "PASS " : "FAIL ") +
                       "warp editor: aspect-ratioed pad at the whole-lattice view" +
                       (padOk ? std::string()
                              : " (shown=" + std::string(shown ? "y" : "n") +
                                    " aspect=" + std::to_string(aspect) +
                                    " zoom=" + std::to_string(zoom) + " pan=" +
                                    std::to_string(pan.x) + "," +
                                    std::to_string(pan.y) + " node=" +
                                    std::to_string(nodeX) + "," + std::to_string(nodeY) +
                                    " reset=" + std::to_string(resetX) + "," +
                                    std::to_string(resetY) + ")") +
                       "\n";
        app.testCloseModal("warp");
        bool stillOpen = true;
        for (int i = 0; i < 3 && stillOpen; ++i)
        {
            glfwPollEvents();
            runtime.renderFrame();
            stillOpen = app.testModalOpen("warp");
        }
        modalOk = modalOk && !stillOpen;
    }

    // 3c) Show editor "Add cue" (issue 12): each press must add a cue. The
    //     self-test editor starts with an empty timeline, so the first press
    //     adds one cue (at t=0) and the second must add another - before the
    //     fix the new cue's default time equalled the last cue's, so
    //     Show::addCue replaced it and the timeline never grew past one.
    const std::size_t cuesAfter1 = app.testAddCue();
    const std::size_t cuesAfter2 = app.testAddCue();
    const bool addCueOk = cuesAfter1 == 1 && cuesAfter2 == 2;
    modalOk = modalOk && addCueOk;
    modalReport += std::string(addCueOk ? "PASS " : "FAIL ") +
                   "editor Add cue adds on every press (cues: " + std::to_string(cuesAfter1) + ", " +
                   std::to_string(cuesAfter2) + ")\n";

    // 3d) Show view interaction state (headless): the mode setters and the
    //     selection are pure GUI state - no system needed.
    {
        app.testShowViewSetMode("layer");
        app.testShowViewSetMode("show");
        const bool svStateOk = app.testShowViewSelectedDisplay() == kInvalidId &&
                               app.testShowViewSelectedLayer() == kInvalidId &&
                               !app.testShowViewEditPopupOpen();
        app.testShowViewSetMode("display");
        modalOk = modalOk && svStateOk;
        modalReport += std::string(svStateOk ? "PASS " : "FAIL ") +
                       "show view mode/selection state\n";
    }

    // 3e) Show editor rework: stable tree ids (renaming a layer must not
    //     fold its node), the layer reorder (list order = compositing order
    //     for equal zIndex), the keyframes submenu (folded by default; add /
    //     remove must keep the headers open), the cue action dropdown, and
    //     the drop-to-add media path.
    {
        auto frame = [&] {
            glfwPollEvents();
            runtime.renderFrame();
        };
        // Three frames: move, press, release. The mouse move must land on a
        // SEPARATE frame before the press: tab items use the AllowOverlap
        // hit-test (ItemHoverable), which only accepts a click if the item
        // was already hovered on the previous frame.
        auto clickAt = [&](float sx, float sy) {
            runtime.preNewFrameHook = [sx, sy] {
                ImGui::GetIO().AddMousePosEvent(sx, sy);
            };
            glfwPollEvents();
            runtime.renderFrame();
            runtime.preNewFrameHook = [sx, sy] {
                ImGui::GetIO().AddMousePosEvent(sx, sy);
                ImGui::GetIO().AddMouseButtonEvent(0, true);
            };
            glfwPollEvents();
            runtime.renderFrame();
            runtime.preNewFrameHook = [sx, sy] {
                ImGui::GetIO().AddMousePosEvent(sx, sy);
                ImGui::GetIO().AddMouseButtonEvent(0, false);
            };
            glfwPollEvents();
            runtime.renderFrame();
            runtime.preNewFrameHook = nullptr;
        };
        float x = 0.f;
        float y = 0.f;
        float w = 0.f;
        float h = 0.f;
        app.testEnsureEditorSceneLayer();
        // The Scenes tab, through the real tab button.
        const bool tabOk = app.testEditorItemRect(kInvalidId, "tab-scenes", x, y, w, h);
        if (tabOk)
        {
            clickAt(x + w * 0.5f, y + h * 0.5f);
        }
        frame();
        // Layer node: open it, rename the layer (the same model edit the
        // Name input performs), and the node must stay open - a label-
        // derived tree id folded it on every keystroke.
        bool renameOk = false;
        bool nodeOpen = true;
        if (tabOk && app.testEditorItemRect(10, "node", x, y, w, h))
        {
            clickAt(x + w * 0.25f, y + h * 0.5f); // over the label, not the row's buttons
            app.testEditorItemOpen(10, "node", nodeOpen);
            if (nodeOpen && app.testSetLayerName(10, "renamed-red"))
            {
                frame();
                app.testEditorItemOpen(10, "node", nodeOpen);
                app.testSetLayerName(10, "red");
                renameOk = nodeOpen;
            }
        }
        modalOk = modalOk && renameOk;
        modalReport += std::string(renameOk ? "PASS " : "FAIL ") +
                       "editor: layer node stays open when the layer is renamed\n";
        // Layer reorder: move layer 10 down one slot and back up, through
        // the same swap the Scenes tab's Up/Down buttons run. The neighbor
        // is whatever the list holds (the live self-test has a media layer
        // between 10 and 11), so the check is "exactly one slot down, then
        // back to the original order" - and the original order must always
        // be restored, or the later checks (keyframes, cue defaults) run
        // against a half-moved list.
        bool reorderOk = false;
        {
            const auto scene1Ids = [&] {
                std::vector<Id> ids;
                for (const auto& [sid, lid] : app.testShowSceneLayers())
                {
                    if (sid == 1)
                    {
                        ids.push_back(lid);
                    }
                }
                return ids;
            };
            const auto before = scene1Ids();
            const auto it10 = std::find(before.begin(), before.end(), 10);
            if (it10 != before.end() &&
                app.testMoveLayer(0, static_cast<int>(it10 - before.begin()), false))
            {
                const auto moved = scene1Ids();
                const auto m10 = std::find(moved.begin(), moved.end(), 10);
                const bool oneSlotDown = m10 != moved.end() &&
                                         static_cast<int>(m10 - moved.begin()) ==
                                             static_cast<int>(it10 - before.begin()) + 1;
                if (oneSlotDown &&
                    app.testMoveLayer(0, static_cast<int>(m10 - moved.begin()), true))
                {
                    reorderOk = scene1Ids() == before;
                }
            }
        }
        modalOk = modalOk && reorderOk;
        modalReport += std::string(reorderOk ? "PASS " : "FAIL ") +
                       "editor: layers can be reordered within the list\n";
        // Keyframes: one submenu per layer, folded by default; adding and
        // removing an entry must keep the headers open - a count-derived
        // header id re-folded them on every change ("cannot add or remove").
        bool kfOk = false;
        bool kfOpen = true;
        if (app.testEditorItemOpen(10, "keyframes", kfOpen))
        {
            kfOk = !kfOpen; // folded by default
        }
        if (kfOk && app.testEditorItemRect(10, "keyframes", x, y, w, h))
        {
            clickAt(x + w * 0.5f, y + h * 0.5f);
            app.testEditorItemOpen(10, "keyframes", kfOpen);
            kfOk = kfOpen;
        }
        if (kfOk && app.testEditorItemRect(10, "opacity", x, y, w, h))
        {
            clickAt(x + w * 0.5f, y + h * 0.5f);
            app.testEditorItemOpen(10, "opacity", kfOpen);
            kfOk = kfOpen;
        }
        if (kfOk && app.testEditorItemRect(10, "addopacity", x, y, w, h))
        {
            clickAt(x + w * 0.5f, y + h * 0.5f); // the "Add keyframe" button
            frame(); // let the new keyframe row render (its remove button)
            kfOk = app.testLayerTrackKeyCount(10, "opacity") == 1;
            app.testEditorItemOpen(10, "opacity", kfOpen);
            kfOk = kfOk && kfOpen; // the add must not re-fold the track
        }
        if (kfOk && app.testEditorItemRect(10, "rmopacity", x, y, w, h))
        {
            clickAt(x + w * 0.5f, y + h * 0.5f); // the first keyframe's "x" button
            kfOk = app.testLayerTrackKeyCount(10, "opacity") == 0;
            app.testEditorItemOpen(10, "keyframes", kfOpen);
            kfOk = kfOk && kfOpen; // the remove must not re-fold the submenu
        }
        modalOk = modalOk && kfOk;
        modalReport += std::string(kfOk ? "PASS " : "FAIL ") +
                       "editor: keyframes submenu folds by default; add/remove keeps it open\n";
        // Cue action dropdown: switching the selection converts the action
        // to the new type with that type's defaults (scene / layer picked
        // from the show).
        bool cueOk = app.testCueActionType(0) == "playScene";
        cueOk = cueOk && app.testSetCueAction(0, 4) &&
                app.testCueActionType(0) == "setLayerOpacity" && app.testCueActionLayer(0) == 10;
        cueOk = cueOk && app.testSetCueAction(0, 1) && app.testCueActionType(0) == "stop";
        cueOk = cueOk && app.testSetCueAction(0, 0) && app.testCueActionType(0) == "playScene";
        modalOk = modalOk && cueOk;
        modalReport += std::string(cueOk ? "PASS " : "FAIL ") +
                       "editor: cue action dropdown switches the action (defaults follow)\n";
        // Drop-to-add media: the exact production path the window's OS file
        // drop target runs (dedup by path, first occurrence wins).
        const int added =
            app.testAddDroppedFiles({"selftest-drop-a.mp4", "selftest-drop-a.mp4",
                                     "selftest-drop-b.png"});
        const auto mediaPaths = app.testShowMediaPaths();
        auto hasPath = [&mediaPaths](const std::string& p) {
            return std::find(mediaPaths.begin(), mediaPaths.end(), p) != mediaPaths.end();
        };
        bool dropOk = added == 2 && hasPath("selftest-drop-a.mp4") && hasPath("selftest-drop-b.png");
        if (dropOk && !mediaPaths.empty())
        {
            // Re-dropping a path the show already references adds nothing
            // (live: the seeded BMP; headless: drop-a from above).
            dropOk = app.testAddDroppedFiles({mediaPaths.front()}) == 0;
        }
        modalOk = modalOk && dropOk;
        modalReport += std::string(dropOk ? "PASS " : "FAIL ") +
                       "editor: dropped files add media references (dedup by path)\n";
    }

    // 4) Auto-push (issue 8): the editor edit from step 1 must have reached
    //    the system via the debounced auto-push during the frame phases.
    //    Keep rendering frames (the push happens in frame()) while polling
    //    show.info for the pushed show. Wait for the FINAL document (name +
    //    the two cues step 3b added), not just the name: an earlier push
    //    already carries the name, and step 4b must not race the last push -
    //    a show reload clears the layer overrides that check verifies.
    if (haveLiveSystem && !autoPushName.empty())
    {
        SystemConnection check("auto-push-check", liveHost, livePort);
        nlohmann::json result;
        std::string checkError;
        bool arrived = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!arrived && std::chrono::steady_clock::now() < deadline)
        {
            glfwPollEvents();
            runtime.renderFrame();
            if (check.send("show.info", nlohmann::json::object(), result, checkError) &&
                result.value("name", std::string()) == autoPushName &&
                result.value("cues", 0) == 2)
            {
                arrived = true;
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
        }
        std::cout << (arrived ? "PASS" : "FAIL") << " auto-push - show.info name = "
                  << (arrived ? autoPushName : "<not received>") << std::endl;
        liveOk = liveOk && arrived;

        // The pushed show's media layer must be live in the engine's frame:
        // it is what the show view renders (thumbnail + texture) during the
        // frame phases, so its absence would mean the view never drew media.
        nlohmann::json fsResult;
        std::string fsCmdError;
        bool haveMediaLayer = false;
        if (arrived && check.send("frame.state", nlohmann::json::object(), fsResult, fsCmdError))
        {
            for (const auto& l : fsResult.value("layers", nlohmann::json::array()))
            {
                if (l.value("name", "") == "testbmp" && l.value("mediaId", 0u) != 0)
                {
                    haveMediaLayer = true;
                    break;
                }
            }
        }
        liveReport += std::string(haveMediaLayer ? "PASS " : "FAIL ") +
                      "pushed show's media layer is live in frame.state (show view data)\n";
        liveOk = liveOk && haveMediaLayer;

        // Issue 18: the pushed document carries the GUI's system list (the
        // show file is where the systems live). The engine stores the field
        // and show.save writes the document back - pull it and check the
        // list round-tripped (at this moment it is exactly [self-test]).
        const std::string docPath =
            (std::filesystem::temp_directory_path() / "it18_engine_show.json").string();
        std::error_code fsError;
        std::filesystem::remove(docPath, fsError);
        nlohmann::json saveResult;
        std::string saveError;
        const bool docSaved =
            check.send("show.save", nlohmann::json({{"path", docPath}}), saveResult, saveError);
        bool systemsInDoc = false;
        std::string systemsDetail = saveError;
        if (docSaved)
        {
            std::ifstream in(docPath);
            const nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
            if (doc.is_object() && doc.contains("systems") && doc["systems"].is_array())
            {
                for (const auto& entry : doc["systems"])
                {
                    if (entry.value("name", std::string()) == "self-test" &&
                        entry.value("host", std::string()) == liveHost &&
                        entry.value("port", 0) == static_cast<int>(livePort))
                    {
                        systemsInDoc = true;
                        break;
                    }
                }
            }
            if (!systemsInDoc)
            {
                systemsDetail = "document has no matching system entry";
            }
        }
        std::cout << (systemsInDoc ? "PASS" : "FAIL")
                  << " auto-pushed document carries the system list (show.save round-trip)"
                  << (systemsInDoc ? std::string() : " - " + systemsDetail) << std::endl;
        liveOk = liveOk && systemsInDoc;
    }

    // 4e) Space hotkey (play/pause): inject the real key event into
    //     ImGui's input queue for the next frames (the same path the
    //     backend uses for physical keys - AddKeyEvent must land before
    //     NewFrame, hence the runtime hook) and verify the active engine's
    //     playback state toggles. Bindings only fire when no widget owns
    //     the keyboard; nothing is focused at this point.
    if (haveLiveSystem && liveOk)
    {
        SystemConnection hk("hotkey-check", liveHost, livePort);
        nlohmann::json st;
        std::string stError;
        const bool haveState =
            hk.send("status", nlohmann::json::object(), st, stError) && st.contains("state");
        const std::string s0 = haveState ? st["state"].get<std::string>() : std::string();
        const std::string expect = (s0 == "playing") ? "paused" : "playing";
        bool hotkeyOk = false;
        std::string s1;
        if (haveState)
        {
            // A press is a down->up pair: the down edge fires the binding
            // on the frame where it is seen.
            runtime.preNewFrameHook = [] { ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, true); };
            glfwPollEvents();
            runtime.renderFrame();
            runtime.preNewFrameHook = [] { ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, false); };
            glfwPollEvents();
            runtime.renderFrame();
            runtime.preNewFrameHook = nullptr;
            // The toggle command is synchronous (sent during the frame), so
            // the engine has applied it by now.
            nlohmann::json st1;
            std::string st1Error;
            s1 = hk.send("status", nlohmann::json::object(), st1, st1Error) && st1.contains("state")
                     ? st1["state"].get<std::string>()
                     : std::string();
            hotkeyOk = (s1 == expect);
        }
        liveReport += std::string(hotkeyOk ? "PASS " : "FAIL ") +
                      "space hotkey toggled playback (" + s0 + " -> " +
                      (hotkeyOk ? expect : "<" + s1 + ">") + ")\n";
        liveOk = liveOk && hotkeyOk;
    }

    // 4e+) Show view (issue: show view improvements + thumbnail rendering):
    //      the engine still runs the auto-pushed show (display 1 at the
    //      origin, the red color layer, the testbmp media layer scaled to
    //      cover the wall). Verify the thumbnail actually reached the GPU
    //      (pixel readback of the pool's texture), the mode/selection/drag
    //      interaction (synthetic mouse events - the same injection path
    //      as the hotkey check), the context menu's Edit... action, and
    //      that a drag moves the live display and layer (both restored).
    if (haveLiveSystem && liveOk)
    {
        SystemConnection sv("show-view-check", liveHost, livePort);
        nlohmann::json fs;
        std::string fsError;
        nlohmann::json dispJson;
        nlohmann::json layerJson;
        if (sv.send("frame.state", nlohmann::json::object(), fs, fsError) &&
            fs.contains("displays") && fs["displays"].size() >= 1)
        {
            dispJson = fs["displays"][0];
            if (fs.contains("layers"))
            {
                for (const auto& l : fs["layers"])
                {
                    if (l.value("name", std::string()) == "testbmp")
                    {
                        layerJson = l;
                        break;
                    }
                }
            }
        }
        if (dispJson.is_null() || layerJson.is_null())
        {
            liveReport += "FAIL show view - no wall / testbmp layer in frame.state\n";
            liveOk = false;
        }
        else
        {
            const std::uint32_t dispId = dispJson["id"].get<std::uint32_t>();
            const int dispX0 = dispJson["x"].get<int>();
            const int dispY0 = dispJson["y"].get<int>();
            const int dispW0 = dispJson["width"].get<int>();
            const int dispH0 = dispJson["height"].get<int>();
            const std::string dispName = dispJson["name"].get<std::string>();
            const std::uint32_t layerId = layerJson["id"].get<std::uint32_t>();
            const std::uint32_t mediaId = layerJson["mediaId"].get<std::uint32_t>();
            const double layerX0 = layerJson["x"].get<double>();
            const double layerY0 = layerJson["y"].get<double>();

            auto sendRaw = [&](const std::string& method, const nlohmann::json& params,
                               nlohmann::json& out) {
                std::string rawError;
                return sv.send(method, params, out, rawError);
            };
            // One synthetic click: press + release at a screen point (the
            // events are queued before NewFrame, like the hotkey check).
            auto clickAt = [&](float sx, float sy) {
                runtime.preNewFrameHook = [sx, sy] {
                    ImGui::GetIO().AddMousePosEvent(sx, sy);
                    ImGui::GetIO().AddMouseButtonEvent(0, true);
                };
                glfwPollEvents();
                runtime.renderFrame();
                runtime.preNewFrameHook = [sx, sy] {
                    ImGui::GetIO().AddMousePosEvent(sx, sy);
                    ImGui::GetIO().AddMouseButtonEvent(0, false);
                };
                glfwPollEvents();
                runtime.renderFrame();
                runtime.preNewFrameHook = nullptr;
            };
            // One synthetic drag: press at (sx0, sy0), move to (+dx, +dy)
            // in three steps (past ImGui's drag threshold), release.
            auto dragAt = [&](float sx0, float sy0, float dx, float dy) {
                runtime.preNewFrameHook = [sx0, sy0] {
                    ImGui::GetIO().AddMousePosEvent(sx0, sy0);
                    ImGui::GetIO().AddMouseButtonEvent(0, true);
                };
                glfwPollEvents();
                runtime.renderFrame();
                for (int i = 1; i <= 3; ++i)
                {
                    const float mx = sx0 + dx * static_cast<float>(i) / 3.f;
                    const float my = sy0 + dy * static_cast<float>(i) / 3.f;
                    runtime.preNewFrameHook = [mx, my] {
                        ImGui::GetIO().AddMousePosEvent(mx, my);
                    };
                    glfwPollEvents();
                    runtime.renderFrame();
                }
                const float mx = sx0 + dx;
                const float my = sy0 + dy;
                runtime.preNewFrameHook = [mx, my] {
                    ImGui::GetIO().AddMousePosEvent(mx, my);
                    ImGui::GetIO().AddMouseButtonEvent(0, false);
                };
                glfwPollEvents();
                runtime.renderFrame();
                runtime.preNewFrameHook = nullptr;
            };

            // 1) The thumbnail's pixels actually reached the GPU: wait for
            //    the pool's upload, copy the texture out on one frame
            //    (showUploadHook records a copy into a host-visible
            //    staging buffer) and check the test BMP's halves (top red /
            //    bottom blue).
            bool thumbOk = false;
            std::string thumbDetail;
            {
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (!app.testShowTextureUploaded(mediaId) &&
                       std::chrono::steady_clock::now() < deadline)
                {
                    glfwPollEvents();
                    runtime.renderFrame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                int tw = 0;
                int th = 0;
                if (!app.testShowTextureUploaded(mediaId) ||
                    !app.testShowTextureSize(mediaId, tw, th))
                {
                    thumbDetail = "texture never uploaded (size " + std::to_string(tw) + "x" +
                                  std::to_string(th) + ")";
                }
                else
                {
                    VkBufferCreateInfo bci{};
                    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                    bci.size = static_cast<VkDeviceSize>(tw) * static_cast<VkDeviceSize>(th) * 4;
                    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                    VkBuffer staging = VK_NULL_HANDLE;
                    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
                    if (vkCreateBuffer(runtime.device(), &bci, nullptr, &staging) != VK_SUCCESS)
                    {
                        thumbDetail = "vkCreateBuffer (readback staging) failed";
                    }
                    else
                    {
                        VkMemoryRequirements reqs;
                        vkGetBufferMemoryRequirements(runtime.device(), staging, &reqs);
                        VkPhysicalDeviceMemoryProperties props;
                        vkGetPhysicalDeviceMemoryProperties(runtime.physicalDevice(), &props);
                        bool memOk = false;
                        for (std::uint32_t i = 0; i < props.memoryTypeCount && !memOk; ++i)
                        {
                            const VkMemoryPropertyFlags wanted =
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                            if ((reqs.memoryTypeBits & (1u << i)) != 0 &&
                                (props.memoryTypes[i].propertyFlags & wanted) == wanted)
                            {
                                VkMemoryAllocateInfo ai{};
                                ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                                ai.allocationSize = reqs.size;
                                ai.memoryTypeIndex = i;
                                if (vkAllocateMemory(runtime.device(), &ai, nullptr, &stagingMem) ==
                                    VK_SUCCESS)
                                {
                                    memOk = vkBindBufferMemory(runtime.device(), staging,
                                                               stagingMem, 0) == VK_SUCCESS;
                                }
                            }
                        }
                        if (!memOk)
                        {
                            thumbDetail = "host-visible staging memory unavailable";
                        }
                        else
                        {
                            // One frame with the readback armed: the hook
                            // records the copy after the pool's uploads.
                            app.showUploadHook = [&app, mediaId, staging](VkCommandBuffer cmd) {
                                app.testShowTextureReadback(mediaId, cmd, staging);
                            };
                            runtime.renderFrame();
                            runtime.waitForFrame();
                            app.showUploadHook = nullptr;
                            void* mapped = nullptr;
                            if (vkMapMemory(runtime.device(), stagingMem, 0, bci.size, 0,
                                            &mapped) != VK_SUCCESS)
                            {
                                thumbDetail = "vkMapMemory (readback) failed";
                            }
                            else
                            {
                                const auto* px = static_cast<const std::uint8_t*>(mapped);
                                // The test BMP: top half red, bottom half blue.
                                // Sample well inside each half (the thumbnail
                                // is aspect-fit into 160x90 for the 320x180
                                // source).
                                const int cx = tw / 2;
                                const std::uint8_t* pTop =
                                    px + (static_cast<std::size_t>(th / 4) * tw + cx) * 4;
                                const std::uint8_t* pBot =
                                    px + (static_cast<std::size_t>(th * 3 / 4) * tw + cx) * 4;
                                const bool topRed =
                                    pTop[0] > 128 && pTop[1] < 64 && pTop[2] < 64;
                                const bool botBlue =
                                    pBot[0] < 64 && pBot[1] < 64 && pBot[2] > 128;
                                thumbDetail = "top=" + std::to_string(pTop[0]) + "," +
                                              std::to_string(pTop[1]) + "," +
                                              std::to_string(pTop[2]) + " bottom=" +
                                              std::to_string(pBot[0]) + "," +
                                              std::to_string(pBot[1]) + "," +
                                              std::to_string(pBot[2]);
                                thumbOk = topRed && botBlue;
                                vkUnmapMemory(runtime.device(), stagingMem);
                            }
                        }
                        if (staging != VK_NULL_HANDLE)
                        {
                            vkDestroyBuffer(runtime.device(), staging, nullptr);
                        }
                        if (stagingMem != VK_NULL_HANDLE)
                        {
                            vkFreeMemory(runtime.device(), stagingMem, nullptr);
                        }
                    }
                }
            }
            liveReport += std::string(thumbOk ? "PASS " : "FAIL ") +
                          "show view thumbnail readback (" + thumbDetail + ")\n";
            liveOk = liveOk && thumbOk;

            // 2) The camera is fitted (frames rendered all along): grab it
            //    to project world points into clicks.
            float scale = 0.f;
            float camMinX = 0.f, camMinY = 0.f, camMaxX = 0.f, camMaxY = 0.f;
            const bool haveCam = app.testShowViewCamera(scale, camMinX, camMinY, camMaxX, camMaxY);
            liveReport += std::string(haveCam ? "PASS " : "FAIL ") +
                          "show view camera fitted (scale " + std::to_string(scale) + ")\n";
            liveOk = liveOk && haveCam;

            if (haveCam)
            {
                float sx = 0.f;
                float sy = 0.f;

                // 3) Display mode: a click on the display selects it.
                app.testShowViewSetMode("display");
                app.testShowViewProject(dispX0 + dispW0 * 0.5f, dispY0 + dispH0 * 0.5f, sx, sy);
                clickAt(sx, sy);
                const bool dispSelOk = app.testShowViewSelectedDisplay() == dispId &&
                                       app.testShowViewSelectedLayer() == kInvalidId;
                liveReport += std::string(dispSelOk ? "PASS " : "FAIL ") +
                              "display mode click selects the display (id " +
                              std::to_string(dispId) + ")\n";
                liveOk = liveOk && dispSelOk;

                // 4) Layer mode: the same point selects the topmost layer
                //    (the testbmp media layer, which covers the wall).
                app.testShowViewSetMode("layer");
                clickAt(sx, sy);
                const bool layerSelOk = app.testShowViewSelectedLayer() == layerId &&
                                        app.testShowViewSelectedDisplay() == kInvalidId;
                liveReport += std::string(layerSelOk ? "PASS " : "FAIL ") +
                              "layer mode click selects the topmost layer (id " +
                              std::to_string(layerId) + ")\n";
                liveOk = liveOk && layerSelOk;

                // 5) Show mode: clicks select nothing (the selection from
                //    the previous mode persists, unchanged).
                app.testShowViewSetMode("show");
                clickAt(sx, sy);
                const bool showModeOk = app.testShowViewSelectedLayer() == layerId &&
                                        app.testShowViewSelectedDisplay() == kInvalidId;
                liveReport += std::string(showModeOk ? "PASS " : "FAIL ") +
                              "show mode clicks select nothing\n";
                liveOk = liveOk && showModeOk;

                // 6) Clicking empty space (inside the fitted view, outside
                //    the display) deselects.
                app.testShowViewSetMode("display");
                app.testShowViewProject(-60.f, -36.f, sx, sy);
                clickAt(sx, sy);
                const bool deselectOk = app.testShowViewSelectedDisplay() == kInvalidId &&
                                        app.testShowViewSelectedLayer() == kInvalidId;
                liveReport += std::string(deselectOk ? "PASS " : "FAIL ") +
                              "clicking empty space deselects\n";
                liveOk = liveOk && deselectOk;

                // 7) Dragging the selected display moves the LIVE engine
                //    display (display.set): press on the display, move
                //    +30/+15 screen px, release - the world delta is the
                //    screen delta divided by the camera scale.
                const float dragDx = 30.f;
                const float dragDy = 15.f;
                app.testShowViewProject(dispX0 + dispW0 * 0.5f, dispY0 + dispH0 * 0.5f, sx, sy);
                dragAt(sx, sy, dragDx, dragDy);
                nlohmann::json dv;
                nlohmann::json dummy;
                bool dragDispOk = false;
                std::string dragDispDetail;
                if (sendRaw("display.list", nlohmann::json::object(), dv) &&
                    dv.contains("displays"))
                {
                    int gotX = 0;
                    int gotY = 0;
                    bool found = false;
                    for (const auto& d : dv["displays"])
                    {
                        if (d.value("id", 0u) == dispId)
                        {
                            found = true;
                            gotX = d.value("x", 0);
                            gotY = d.value("y", 0);
                        }
                    }
                    const double expX = dispX0 + dragDx / scale;
                    const double expY = dispY0 + dragDy / scale;
                    dragDispOk = found && std::abs(gotX - expX) <= 2.0 &&
                                 std::abs(gotY - expY) <= 2.0;
                    dragDispDetail = "got " + std::to_string(gotX) + "," +
                                     std::to_string(gotY) + " want ~" +
                                     std::to_string(static_cast<int>(expX)) + "," +
                                     std::to_string(static_cast<int>(expY));
                }
                else
                {
                    dragDispDetail = "display.list failed";
                }
                liveReport += std::string(dragDispOk ? "PASS " : "FAIL ") +
                              "display drag moves the live display (" + dragDispDetail + ")\n";
                liveOk = liveOk && dragDispOk;
                // Restore the original geometry (the scratch engine's real
                // window moved).
                sendRaw("display.set",
                        {{"display", dispId}, {"x", dispX0}, {"y", dispY0}}, dummy);

                // 8) Dragging the selected layer moves the engine's layer
                //    position (layer.set, read back from frame.state x/y).
                //    The press lands inset from the quad corner: Dear ImGui
                //    floors mouse positions (ImFloor in UpdateMouseInputs),
                //    so a press projected exactly at (layerX0, layerY0) can
                //    unproject up to one screen pixel outside the quad and
                //    hit nothing.
                app.testShowViewSetMode("layer");
                const float inset = 4.0f / scale;
                app.testShowViewProject(static_cast<float>(layerX0) + inset,
                                        static_cast<float>(layerY0) + inset, sx, sy);
                dragAt(sx, sy, dragDx, dragDy);
                nlohmann::json fs2;
                bool dragLayerOk = false;
                std::string dragLayerDetail;
                if (sendRaw("frame.state", nlohmann::json::object(), fs2) &&
                    fs2.contains("layers"))
                {
                    double gotX = 0.0;
                    double gotY = 0.0;
                    bool found = false;
                    for (const auto& l : fs2["layers"])
                    {
                        if (l.value("id", 0u) == layerId)
                        {
                            found = true;
                            gotX = l.value("x", 0.0);
                            gotY = l.value("y", 0.0);
                        }
                    }
                    const double expX = layerX0 + dragDx / scale;
                    const double expY = layerY0 + dragDy / scale;
                    dragLayerOk = found && std::abs(gotX - expX) <= 2.0 &&
                                  std::abs(gotY - expY) <= 2.0;
                    dragLayerDetail = "got " + std::to_string(gotX) + "," +
                                      std::to_string(gotY) + " want ~" +
                                      std::to_string(static_cast<int>(expX)) + "," +
                                      std::to_string(static_cast<int>(expY));
                }
                else
                {
                    dragLayerDetail = "frame.state failed";
                }
                liveReport += std::string(dragLayerOk ? "PASS " : "FAIL ") +
                              "layer drag moves the layer (frame.state x/y " + dragLayerDetail +
                              ")\n";
                liveOk = liveOk && dragLayerOk;
                // Restore the original position.
                sendRaw("layer.set",
                        {{"layer", layerId}, {"x", layerX0}, {"y", layerY0}}, dummy);

                // 9) The right-click context menu over the selected display:
                //    drop the document's display entry first so the Edit...
                //    action runs its "add from live" path; a right click
                //    over the selected display opens the menu; Edit... opens
                //    the settings dialog on the (re-)added entry.
                app.testRemoveDocumentDisplay(dispName);
                app.testShowViewSetMode("display");
                app.testShowViewProject(dispX0 + dispW0 * 0.5f, dispY0 + dispH0 * 0.5f, sx, sy);
                clickAt(sx, sy);
                {
                    runtime.preNewFrameHook = [sx, sy] {
                        ImGui::GetIO().AddMousePosEvent(sx, sy);
                        ImGui::GetIO().AddMouseButtonEvent(1, true);
                    };
                    glfwPollEvents();
                    runtime.renderFrame();
                    runtime.preNewFrameHook = [sx, sy] {
                        ImGui::GetIO().AddMousePosEvent(sx, sy);
                        ImGui::GetIO().AddMouseButtonEvent(1, false);
                    };
                    glfwPollEvents();
                    runtime.renderFrame();
                    runtime.preNewFrameHook = nullptr;
                }
                bool popupSeen = false;
                for (int i = 0; i < 10 && !popupSeen; ++i)
                {
                    glfwPollEvents();
                    runtime.renderFrame();
                    popupSeen = app.testShowViewEditPopupOpen();
                }
                const bool editActionOk = popupSeen && app.testShowViewEditSelectedDisplay();
                bool editShown = false;
                for (int i = 0; i < 10 && !editShown; ++i)
                {
                    glfwPollEvents();
                    runtime.renderFrame();
                    editShown = app.testModalOpen("edit");
                }
                app.testCloseModal("edit");
                bool editClosed = false;
                for (int i = 0; i < 10 && !editClosed; ++i)
                {
                    glfwPollEvents();
                    runtime.renderFrame();
                    editClosed = !app.testModalOpen("edit");
                }
                const bool ctxOk = editActionOk && editShown && editClosed;
                liveReport += std::string(ctxOk ? "PASS " : "FAIL ") +
                              "show view right-click menu: Edit... opens the settings dialog\n";
                liveOk = liveOk && ctxOk;

                // The Edit... action re-added the display entry (an editor
                // touch): a debounced auto-push (1 s) will re-apply the
                // document to the engine. Let it land NOW - while the engine
                // still runs this show, where it is a no-op - rather than
                // racing the load-push check below.
                {
                    const auto settleUntil =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(1400);
                    while (std::chrono::steady_clock::now() < settleUntil)
                    {
                        glfwPollEvents();
                        runtime.renderFrame();
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
                app.testShowViewSetMode("display");
            }
        }
    }

    // 4a) Loading a show (File > Open) pushes it to every connected system
    //     right away: the self-test writes a small show file whose
    //     `systems` list carries the "self-test" entry (so
    //     applyShowSystems reuses the live connection), loads it through
    //     the same routine the Open dialog uses, and polls show.info for
    //     the loaded name.
    if (haveLiveSystem && !autoPushName.empty())
    {
        const std::string loadedName = "Load push check";
        const std::string loadDocPath =
            (std::filesystem::temp_directory_path() / "gui_load_push_show.json").string();
        {
            std::ofstream out(loadDocPath);
            out << nlohmann::json{{"app", "mediamax-show"},
                                  {"version", 1},
                                  {"name", loadedName},
                                  {"frameRate", {{"num", 60}, {"den", 1}}},
                                  {"media", nlohmann::json::array()},
                                  {"scenes", nlohmann::json::array()},
                                  {"cues", nlohmann::json::array()},
                                  {"systems", {{{"name", "self-test"},
                                                {"host", liveHost},
                                                {"port", static_cast<int>(livePort)}}}}}
                        .dump(2);
        }
        const bool loadOk = app.testLoadShowFile(loadDocPath);
        bool arrived = false;
        if (loadOk)
        {
            SystemConnection check("load-push-check", liveHost, livePort);
            nlohmann::json result;
            std::string checkError;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!arrived && std::chrono::steady_clock::now() < deadline)
            {
                glfwPollEvents();
                runtime.renderFrame();
                if (check.send("show.info", nlohmann::json::object(), result, checkError) &&
                    result.value("name", std::string()) == loadedName)
                {
                    arrived = true;
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                }
            }
        }
        std::error_code fsError;
        std::filesystem::remove(loadDocPath, fsError);
        std::cout << ((loadOk && arrived) ? "PASS" : "FAIL")
                  << " loading a show pushes it to connected systems (show.info name = "
                  << (arrived ? loadedName : "<not received>") << ")" << std::endl;
        liveOk = liveOk && loadOk && arrived;
    }

    // 4b) A second system entry on the same engine: proves multi-system
    //     polling and - since addSystem activates the entry it adds - makes
    //     this the active system the display-move check in 4c needs.
    if (haveLiveSystem && !autoPushName.empty() && liveOk)
    {
        app.testAddSystem("self-test-2", liveHost, livePort);
        bool secondUp = false;
        const auto upDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!secondUp && std::chrono::steady_clock::now() < upDeadline)
        {
            secondUp = app.testSystemConnected("self-test-2");
            if (!secondUp)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
        }
        liveReport += std::string(secondUp ? "PASS " : "FAIL ") +
                      "second system connected (display-move target)\n";
        liveOk = liveOk && secondUp;
    }

    // 4c) Display settings dialog (issues 14 + 15): the dialog edits the
    //     show document's wall entry (persistence) and applies it live on
    //     the mapped system. Its System dropdown targets the live apply -
    //     Apply with a different selection moves the live display (display.
    //     add + display.set on the target, display.remove on the mapped
    //     source) - and its Name field renames the entry (display.set
    //     carries the name). Step 4a replaced the document with a
    //     displays-less show, so mirror the engine's live display into it
    //     again first. Then: open the dialog on the entry, rename it,
    //     point the System dropdown at the OTHER system and Apply - a move
    //     (the engine holds the display under a fresh id, display ids never
    //     repeat, same geometry and brightness). Reopen the dialog on the
    //     moved entry (the dropdown defaults to its mapped system), rename
    //     it back and Apply: a plain display.set, no move.
    if (haveLiveSystem && !autoPushName.empty() && liveOk)
    {
        const std::string movedName = "Self-test moved";
        SystemConnection listConn("self-test-displays", liveHost, livePort);
        nlohmann::json before;
        std::string listError;
        const bool haveDisplay =
            listConn.send("display.list", nlohmann::json::object(), before, listError) &&
            before.contains("displays") && !before["displays"].empty();

        const int beforeCount = haveDisplay ? static_cast<int>(before["displays"].size()) : 0;
        const std::uint64_t oldId =
            haveDisplay ? before["displays"][0].value("id", 0u) : 0u;
        const int oldX = haveDisplay ? before["displays"][0].value("x", 0) : 0;
        const int oldY = haveDisplay ? before["displays"][0].value("y", 0) : 0;
        const int oldW = haveDisplay ? before["displays"][0].value("width", 0) : 0;
        const int oldH = haveDisplay ? before["displays"][0].value("height", 0) : 0;
        const double oldBrightness =
            haveDisplay ? before["displays"][0].value("brightness", -1.0) : -1.0;

        bool opened = false;
        // The dialog opens on a show-document display (the UI's Edit...
        // button only exists on a wall row). The active system's first
        // status poll lands ~500 ms after its connect (step 4b), which can
        // still be in flight here - wait for it, then mirror its live
        // displays into the document (step 4a's loaded show has none).
        bool snapHasDisplay = false;
        const auto snapDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!snapHasDisplay && std::chrono::steady_clock::now() < snapDeadline)
        {
            snapHasDisplay = app.testActiveHasDisplay();
            if (!snapHasDisplay)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
        }
        const int reseeded = snapHasDisplay ? app.testSeedDisplaysFromActive() : 0;
        const bool reseededOk = reseeded > 0;
        liveReport += std::string(snapHasDisplay && reseededOk ? "PASS " : "FAIL ") +
                      "active system snapshot reports the displays (wall re-mirrored into "
                      "the show, " +
                      std::to_string(reseeded) + " entries)\n";
        liveOk = liveOk && snapHasDisplay && reseededOk;
        if (haveDisplay && snapHasDisplay && reseededOk)
        {
            app.testOpenModal("edit"); // deferred: the next frame opens it
            for (int i = 0; i < 3 && !opened; ++i)
            {
                glfwPollEvents();
                if (runtime.renderFrame())
                {
                    opened = app.testModalOpen("edit");
                }
            }
        }

        bool applied = false;
        if (opened)
        {
            // The dialog opened on the show's wall entry; its System
            // dropdown defaults to the entry's mapped system (index 0,
            // self-test - the first connection running the same-named
            // display). Rename the entry (issue 15) and select the OTHER
            // system (index 1) so Apply is a live move that carries the
            // new name (display.add takes it), then press the dialog's
            // Apply button (the exact action the UI runs).
            app.testSetDisplayEditName(movedName);
            app.testSelectDisplayEditSystem(1);
            applied = app.testApplyDisplayEdit();
        }

        bool movedOk = false;
        std::uint64_t newId = 0;
        nlohmann::json after;
        if (applied)
        {
            std::string afterError;
            if (listConn.send("display.list", nlohmann::json::object(), after, afterError) &&
                after.contains("displays") &&
                after["displays"].size() == static_cast<size_t>(beforeCount))
            {
                bool oldGone = true;
                bool recreated = false;
                for (const auto& d : after["displays"])
                {
                    if (d.value("id", 0u) == oldId)
                    {
                        oldGone = false;
                    }
                    if (d.value("id", 0u) != oldId && d.value("name", "") == movedName &&
                        d.value("x", -1) == oldX && d.value("y", -1) == oldY &&
                        d.value("width", -1) == oldW && d.value("height", -1) == oldH &&
                        d.value("brightness", -1.0) == oldBrightness)
                    {
                        recreated = true;
                        newId = d.value("id", 0u);
                    }
                }
                movedOk = oldGone && recreated;
            }
        }
        liveReport += std::string(movedOk ? "PASS " : "FAIL ") +
                      "display settings System dropdown moved the display (old id " +
                      std::to_string(oldId) + " gone, renamed, geometry/brightness kept)" +
                      (movedOk
                           ? std::string()
                           : "; " + app.testDescribeDisplayEdit() + " opened=" +
                                 std::string(opened ? "y" : "n") +
                                 " applied=" + std::string(applied ? "y" : "n") +
                                 " editError='" + app.testGetDisplayEditError() + "' before=" +
                                 before.dump() + " after=" + after.dump()) +
                      "\n";
        liveOk = liveOk && movedOk;

        // Same-system rename (issue 15): reopen the dialog - it opens on
        // the show's wall entry, now the moved one, with the dropdown on
        // its mapped system - rename it back and Apply: a plain display.set
        // with the new name, no move.
        bool renamedOk = false;
        nlohmann::json after2;
        if (movedOk)
        {
            // The active system's snapshot polls every 500 ms; wait until
            // it reports the moved display, so the entry's mapping (and the
            // dialog's dropdown default) is fresh before the reopen.
            bool snapHasNew = false;
            const auto snap2Deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!snapHasNew && std::chrono::steady_clock::now() < snap2Deadline)
            {
                snapHasNew = app.testActiveSnapshotHasDisplay(static_cast<Id>(newId));
                if (!snapHasNew)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                }
            }
            if (snapHasNew)
            {
                app.testOpenModal("edit"); // deferred: the next frame opens it
                bool opened2 = false;
                for (int i = 0; i < 3 && !opened2; ++i)
                {
                    glfwPollEvents();
                    if (runtime.renderFrame())
                    {
                        opened2 = app.testModalOpen("edit");
                    }
                }
                if (opened2)
                {
                    app.testSetDisplayEditName("Display 1");
                    const bool applied2 = app.testApplyDisplayEdit();
                    std::string after2Error;
                    if (applied2 &&
                        listConn.send("display.list", nlohmann::json::object(), after2, after2Error) &&
                        after2.contains("displays"))
                    {
                        for (const auto& d : after2["displays"])
                        {
                            if (d.value("id", 0u) == newId && d.value("name", "") == "Display 1")
                            {
                                renamedOk = true;
                            }
                        }
                    }
                }
            }
            liveReport += std::string(renamedOk ? "PASS " : "FAIL ") +
                          "display settings Name field renamed the display in place (no move)" +
                          (renamedOk
                               ? std::string()
                               : "; " + app.testDescribeDisplayEdit() +
                                     " editError='" + app.testGetDisplayEditError() + "'") +
                          "\n";
            liveOk = liveOk && renamedOk;
        }

        // Physical output (issue 15): every display object carries the OS
        // monitor its window is on. The record is filled in when the
        // engine's queued surface op runs (after the display.add
        // response), so assert the field's presence and report the value
        // informationally - a headless engine legitimately reports none.
        {
            const nlohmann::json& list = renamedOk ? after2 : after;
            bool monitorPresent =
                list.contains("displays") && !list["displays"].empty();
            std::string monitorValue;
            if (monitorPresent)
            {
                for (const auto& d : list["displays"])
                {
                    if (!d.contains("monitor") || !d["monitor"].is_string())
                    {
                        monitorPresent = false;
                        break;
                    }
                }
                if (list["displays"][0].contains("monitor") &&
                    list["displays"][0]["monitor"].is_string())
                {
                    monitorValue = list["displays"][0]["monitor"].get<std::string>();
                }
            }
            liveReport += std::string(monitorPresent ? "PASS " : "FAIL ") +
                          "display records report the physical output (monitor field)" +
                          (monitorValue.empty() ? std::string()
                                                : " monitor='" + monitorValue + "'") +
                          "\n";
            liveOk = liveOk && monitorPresent;
        }

        // Close the dialog if a failure path left it open (Apply closes
        // it on success); a few frames let the popup tear down, as the
        // modal loop above does.
        app.testCloseModal("edit");
        for (int i = 0; i < 3; ++i)
        {
            glfwPollEvents();
            runtime.renderFrame();
        }
    }

    // 4d) System info + settings (issue 16): status reports the machine
    //     the engine runs on (computer name + IP), and the per-system
    //     settings dialog edits the entry's name / address / port. Verify
    //     the machine identity on a fresh connection and in the app's
    //     snapshot, rename the active system entry (index 1, self-test-2)
    //     through the dialog (the entry reconnects), then re-target its
    //     port (a closed port goes offline; switching back comes online).
    if (haveLiveSystem && liveOk)
    {
        SystemConnection infoConn("self-test-sysinfo", liveHost, livePort);
        nlohmann::json status;
        std::string statusError;
        std::string machine, machineIp;
        bool haveInfo = infoConn.send("status", nlohmann::json::object(), status, statusError) &&
                        status.contains("hostname") && status["hostname"].is_string() &&
                        status.contains("ip") && status["ip"].is_string();
        if (haveInfo)
        {
            machine = status["hostname"].get<std::string>();
            machineIp = status["ip"].get<std::string>();
        }
        const bool infoOk = haveInfo && !machine.empty() && !machineIp.empty() &&
                            !app.testSystemHostname(1).empty();
        liveReport += std::string(infoOk ? "PASS " : "FAIL ") +
                      "system info reports the computer name and IP (hostname='" + machine +
                      "', ip='" + machineIp + "')\n";
        liveOk = liveOk && infoOk;

        // Rename through the settings dialog: the entry reconnects and
        // keeps its slot (selections and stored pointers stay valid).
        bool renamedOk = false;
        if (infoOk)
        {
            app.testOpenModal("syssettings"); // deferred: the next frame opens it
            bool opened = false;
            for (int i = 0; i < 3 && !opened; ++i)
            {
                glfwPollEvents();
                if (runtime.renderFrame())
                {
                    opened = app.testModalOpen("syssettings");
                }
            }
            if (opened)
            {
                app.testSetSystemSettingsName("self-test-2-r");
                const bool applied = app.testApplySystemSettings();
                if (applied)
                {
                    const auto deadline =
                        std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (!app.testSystemConnected("self-test-2-r") &&
                           std::chrono::steady_clock::now() < deadline)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    renamedOk = app.testSystemNameAt(1) == "self-test-2-r" &&
                                app.testSystemConnected("self-test-2-r");
                }
            }
            app.testCloseModal("syssettings");
            for (int i = 0; i < 3; ++i)
            {
                glfwPollEvents();
                runtime.renderFrame();
            }
            liveReport += std::string(renamedOk ? "PASS " : "FAIL ") +
                          "system settings renamed the system entry (reconnected)\n";
            liveOk = liveOk && renamedOk;
        }

        // Re-target the entry's port: a closed port goes offline (the poll
        // thread can no longer reach the engine), switching back comes
        // online again with a fresh status - the dialog's address path.
        bool retargetOk = false;
        const int badPort = (livePort < 65535) ? livePort + 1 : livePort - 1;
        if (renamedOk)
        {
            app.testOpenModal("syssettings");
            bool opened = false;
            for (int i = 0; i < 3 && !opened; ++i)
            {
                glfwPollEvents();
                if (runtime.renderFrame())
                {
                    opened = app.testModalOpen("syssettings");
                }
            }
            bool wentOffline = false;
            if (opened)
            {
                app.testSetSystemSettingsPort(badPort);
                const bool applied = app.testApplySystemSettings();
                if (applied)
                {
                    const auto deadline =
                        std::chrono::steady_clock::now() + std::chrono::seconds(6);
                    while (!wentOffline && std::chrono::steady_clock::now() < deadline)
                    {
                        wentOffline = !app.testSystemConnected("self-test-2-r");
                        if (!wentOffline)
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(150));
                        }
                    }
                }
            }
            app.testCloseModal("syssettings");
            for (int i = 0; i < 3; ++i)
            {
                glfwPollEvents();
                runtime.renderFrame();
            }
            bool backOnline = false;
            if (wentOffline)
            {
                app.testOpenModal("syssettings");
                bool opened2 = false;
                for (int i = 0; i < 3 && !opened2; ++i)
                {
                    glfwPollEvents();
                    if (runtime.renderFrame())
                    {
                        opened2 = app.testModalOpen("syssettings");
                    }
                }
                if (opened2)
                {
                    app.testSetSystemSettingsPort(static_cast<int>(livePort));
                    const bool applied = app.testApplySystemSettings();
                    if (applied)
                    {
                        const auto deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(6);
                        // Wait for a fresh status too: pollConnected goes up
                        // at connect, the hostname a poll later.
                        std::string host;
                        while (!backOnline && std::chrono::steady_clock::now() < deadline)
                        {
                            host = app.testSystemHostname(1);
                            backOnline = app.testSystemConnected("self-test-2-r") && !host.empty();
                            if (!backOnline)
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                            }
                        }
                    }
                }
                retargetOk = backOnline && app.testSystemNameAt(1) == "self-test-2-r";
            }
            app.testCloseModal("syssettings");
            for (int i = 0; i < 3; ++i)
            {
                glfwPollEvents();
                runtime.renderFrame();
            }
            liveReport += std::string(retargetOk ? "PASS " : "FAIL ") +
                          "system settings re-targeted the address (" +
                          std::to_string(livePort) + " -> " + std::to_string(badPort) +
                          " offline -> " + std::to_string(livePort) + " back online)\n";
            liveOk = liveOk && retargetOk;
        }
    }

    // 4w) Displays tree + warp editor (Displays tab rework + warp editor
    //     overhaul): the tree lists the document's displays with labeled
    //     controls; the warp editor drives the live engine in real time -
    //     while it is open the display shows the warp grid + nodes
    //     (display.set warpPreview), node edits stream through (display.
    //     warp.set), sticky selection + arrow-key nudge adjust one node,
    //     and scroll zoom / the reset button adjust the pad's view.
    if (haveLiveSystem && liveOk)
    {
        SystemConnection warpConn("warp-check", liveHost, livePort);
        nlohmann::json list;
        std::string listError;
        bool haveDisplay = false;
        std::uint32_t liveDispId = 0;
        int dispW = 0;
        int dispH = 0;
        if (warpConn.send("display.list", nlohmann::json::object(), list, listError) &&
            list.contains("displays"))
        {
            for (const auto& d : list["displays"])
            {
                if (d.value("name", std::string()) == "Display 1")
                {
                    haveDisplay = true;
                    liveDispId = d.value("id", 0u);
                    dispW = d.value("width", 0);
                    dispH = d.value("height", 0);
                    break;
                }
            }
        }
        // The tree lists the document's wall (the mirrored "Display 1").
        bool treeOk = false;
        for (const auto& n : app.testDisplaysTreeEntries())
        {
            if (n == "Display 1")
            {
                treeOk = true;
                break;
            }
        }
        liveReport += std::string(treeOk ? "PASS " : "FAIL ") +
                      "Displays tree lists the show's display wall\n";
        liveOk = liveOk && treeOk && haveDisplay;
        if (haveDisplay)
        {
            auto sendRaw = [&](const std::string& method, const nlohmann::json& params,
                               nlohmann::json& out) {
                std::string rawError;
                return warpConn.send(method, params, out, rawError);
            };
            auto statusDisplay = [&](std::uint32_t id, nlohmann::json& out) {
                nlohmann::json st;
                if (!sendRaw("status", nlohmann::json::object(), st) ||
                    !st.contains("displays"))
                {
                    return false;
                }
                for (const auto& d : st["displays"])
                {
                    if (d.value("id", 0u) == id)
                    {
                        out = d;
                        return true;
                    }
                }
                return false;
            };
            // display.set warpEnabled round-trips through the engine (the
            // renderer skips the warp entirely when it is off).
            bool enabledOk = false;
            nlohmann::json setResult;
            if (sendRaw("display.set", {{"display", liveDispId}, {"warpEnabled", false}},
                        setResult))
            {
                enabledOk = setResult.value("warpEnabled", true) == false &&
                            sendRaw("display.set",
                                    {{"display", liveDispId}, {"warpEnabled", true}}, setResult);
                enabledOk = enabledOk && setResult.value("warpEnabled", false) == true;
            }
            liveReport += std::string(enabledOk ? "PASS " : "FAIL ") +
                          "display.set warpEnabled round-trips (off -> on)\n";
            liveOk = liveOk && enabledOk;
            // The warp editor drives the live display: open it (the
            // document's first entry, mapped to its live system).
            app.testOpenModal("warp");
            bool editorShown = false;
            for (int i = 0; i < 3 && !editorShown; ++i)
            {
                glfwPollEvents();
                if (runtime.renderFrame())
                {
                    editorShown = app.testModalOpen("warp");
                }
            }
            bool previewOk = false;
            nlohmann::json rec;
            if (editorShown && statusDisplay(liveDispId, rec))
            {
                // The overlay is on in the GUI's state AND on the engine.
                previewOk =
                    app.testWarpPreviewSent() && rec.value("warpPreview", false) == true;
            }
            liveReport += std::string(previewOk ? "PASS " : "FAIL ") +
                          "warp editor open turns the live preview on (status warpPreview)\n";
            liveOk = liveOk && previewOk;
            // The pad is aspect-ratioed to the display and starts at the
            // whole-lattice view (zoom 1, pan 0).
            float aspect = 0.0f;
            float zoom = 0.0f;
            ImVec2 pan{0.0f, 0.0f};
            const bool aspectOk = editorShown && app.testWarpPadInfo(aspect, zoom, pan) &&
                                  dispW > 0 && dispH > 0 &&
                                  std::abs(aspect -
                                           static_cast<float>(dispW) /
                                           static_cast<float>(dispH)) < 1e-3f &&
                                  zoom == 1.0f && pan.x == 0.0f && pan.y == 0.0f;
            liveReport += std::string(aspectOk ? "PASS " : "FAIL ") +
                          "warp pad is aspect-ratioed to the display (" + std::to_string(dispW) +
                          "x" + std::to_string(dispH) + ", pad aspect " +
                          std::to_string(aspect) + ")\n";
            liveOk = liveOk && aspectOk;
            // The pad's screen position settles when the hidden window's
            // framebuffer size settles (the popup re-centres on the viewport
            // change): render frames until two consecutive frames report the
            // same pad rect, and only then read the node positions to click.
            float padX = -1.0f;
            float padY = -1.0f;
            float padW = -1.0f;
            float padH = -1.0f;
            float prevPadX = 0.0f;
            float prevPadY = 0.0f;
            bool padStable = false;
            for (int i = 0; i < 8 && !padStable; ++i)
            {
                glfwPollEvents();
                runtime.renderFrame();
                app.testWarpPadRect(padX, padY, padW, padH);
                padStable = (padX == prevPadX && padY == prevPadY);
                prevPadX = padX;
                prevPadY = padY;
            }
            // One synthetic click: press + release at a screen point (the
            // same injection path as the show-view checks).
            auto clickAt = [&](float sx, float sy) {
                runtime.preNewFrameHook = [sx, sy] {
                    ImGui::GetIO().AddMousePosEvent(sx, sy);
                    ImGui::GetIO().AddMouseButtonEvent(0, true);
                };
                glfwPollEvents();
                runtime.renderFrame();
                runtime.preNewFrameHook = [sx, sy] {
                    ImGui::GetIO().AddMousePosEvent(sx, sy);
                    ImGui::GetIO().AddMouseButtonEvent(0, false);
                };
                glfwPollEvents();
                runtime.renderFrame();
                runtime.preNewFrameHook = nullptr;
            };
            // Sticky selection: a click selects the node under the cursor
            // and the selection survives the release (the u/v fields below
            // the pad stay on it).
            int selNode = -2;
            float selU = -1.0f;
            float selV = -1.0f;
            bool selectionOk = false;
            float nodeX = 0.0f;
            float nodeY = 0.0f;
            if (aspectOk && app.testWarpNodeScreenPos(0, nodeX, nodeY))
            {
                clickAt(nodeX, nodeY);
                selectionOk = app.testWarpSelection(selNode, selU, selV) && selNode == 0 &&
                              selU >= 0.0f && selU < 0.01f && selV >= 0.0f && selV < 0.01f;
            }
            liveReport += std::string(selectionOk ? "PASS " : "FAIL ") +
                          "warp editor node click is a sticky selection (node " +
                          std::to_string(selNode) + " at u=" + std::to_string(selU) + ", v=" +
                          std::to_string(selV) + ")\n";
            liveOk = liveOk && selectionOk;
            // Arrow-key nudge: the selected node moves a small fraction -
            // and the engine's warp follows (the editor streams every
            // change as display.warp.set while it is open).
            bool nudgeOk = false;
            if (selectionOk)
            {
                runtime.preNewFrameHook = [] {
                    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
                };
                glfwPollEvents();
                runtime.renderFrame();
                runtime.preNewFrameHook = [] {
                    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, false);
                };
                glfwPollEvents();
                runtime.renderFrame();
                runtime.preNewFrameHook = nullptr;
                const bool moved = app.testWarpSelection(selNode, selU, selV) && selNode == 0 &&
                                   selU > 0.002f && selU < 0.003f && selV < 0.001f;
                const bool engineFollowed =
                    statusDisplay(liveDispId, rec) && rec.contains("warp") &&
                    rec["warp"]["points"].size() >= 1 &&
                    rec["warp"]["points"][0][0].get<double>() > 0.002 &&
                    rec["warp"]["points"][0][1].get<double>() < 0.001;
                nudgeOk = moved && engineFollowed;
            }
            liveReport += std::string(nudgeOk ? "PASS " : "FAIL ") +
                          "arrow keys nudge the selected node (u " + std::to_string(selU) +
                          ", engine warp follows)\n";
            liveOk = liveOk && nudgeOk;
            // Scroll zoom: the view zooms in around the cursor, and the
            // "Reset view" button brings back the whole lattice.
            bool zoomOk = false;
            float resetX = 0.0f;
            float resetY = 0.0f;
            float zoom2 = 0.0f;
            ImVec2 pan2{0.0f, 0.0f};
            float zoom3 = 0.0f;
            ImVec2 pan3{0.0f, 0.0f};
            if (nudgeOk)
            {
                float wheelX = 0.0f;
                float wheelY = 0.0f;
                if (app.testWarpNodeScreenPos(3, wheelX, wheelY))
                {
                    // The wheel must be its own frame: with the event-queue
                    // trickling (the default), a mouse move queued in the
                    // same frame drops the wheel event.
                    runtime.preNewFrameHook = [wheelX, wheelY] {
                        ImGui::GetIO().AddMousePosEvent(wheelX, wheelY);
                    };
                    glfwPollEvents();
                    runtime.renderFrame();
                    runtime.preNewFrameHook = [] {
                        ImGui::GetIO().AddMouseWheelEvent(0.0f, 1.0f);
                    };
                    glfwPollEvents();
                    runtime.renderFrame();
                    runtime.preNewFrameHook = nullptr;
                    zoomOk = app.testWarpPadInfo(aspect, zoom2, pan2) && zoom2 > 1.0f;
                    if (zoomOk && app.testWarpResetButtonPos(resetX, resetY))
                    {
                        clickAt(resetX, resetY);
                        zoomOk = app.testWarpPadInfo(aspect, zoom3, pan3) && zoom3 == 1.0f &&
                                 pan3.x == 0.0f && pan3.y == 0.0f;
                    }
                }
            }
            liveReport += std::string(zoomOk ? "PASS " : "FAIL ") +
                          "warp pad scroll-zooms and reset restores the whole-lattice view" +
                          (zoomOk
                               ? std::string()
                               : " (zoom " + std::to_string(zoom2) +
                                     " after wheel, " + std::to_string(zoom3) +
                                     " after reset)") +
                          "\n";
            liveOk = liveOk && zoomOk;
            // Closing the editor turns the live preview off again.
            app.testCloseModal("warp");
            bool stillOpen = true;
            for (int i = 0; i < 3 && stillOpen; ++i)
            {
                glfwPollEvents();
                runtime.renderFrame();
                stillOpen = app.testModalOpen("warp");
            }
            bool previewOff = !stillOpen && !app.testWarpPreviewSent();
            if (previewOff && statusDisplay(liveDispId, rec))
            {
                previewOff = rec.value("warpPreview", true) == false;
            }
            liveReport += std::string(previewOff ? "PASS " : "FAIL ") +
                          "closing the warp editor turns the live preview off\n";
            liveOk = liveOk && previewOff;
        }
    }

    // The live check is over (the engine loaded the pushed show's media from
    // the test bmp's path); remove the scratch file. No-op headless.
    app.testRemoveSelfTestBmp();

    runtime.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();

    std::cout << "gui-self-test: rendered " << rendered << "/" << frames << " frames" << std::endl;
    std::cout << modalReport;
    std::cout << (modalOk ? "gui-self-test: modals OK" : "gui-self-test: modals FAILED") << std::endl;
    if (!liveSystem.empty())
    {
        std::cout << liveReport;
        std::cout << (liveOk ? "gui-self-test: live system OK" : "gui-self-test: live system FAILED")
                  << std::endl;
    }
    return (liveOk && modalOk && layoutOk && rendered > 0) ? 0 : 1;
}

} // namespace mediamax::gui
