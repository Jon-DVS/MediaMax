#include "gui/GuiMain.h"

#include <chrono>
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
    info.DescriptorPoolSize = 8; // the back end creates its own pool
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
    ImGui::NewFrame();
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
    GLFWwindow* window = glfwCreateWindow(1280, 720, "MediaMax Control (self-test)", nullptr,
                                          nullptr);
    if (!window)
    {
        std::cerr << "mediamax_control: could not create the self-test window" << std::endl;
        glfwTerminate();
        return 1;
    }

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

    // 3) Modal dialogs: open each dialog the UI buttons open and verify
    //    Dear ImGui actually shows it, then verify it closes again. This is
    //    the regression guard for "clicking Open / Save As / About does
    //    nothing": the modal BeginPopupModal() calls must run in a window
    //    context whose popup ids match the OpenPopup() ids (see
    //    kModalHostName in ControlGui.cpp).
    std::string modalReport;
    bool modalOk = true;
    std::vector<const char*> modalNames = {"about", "open", "saveas", "media", "pull", "edit",
                                           "warp", "mediadetails"};
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

    // 3b) Show editor "Add cue" (issue 12): each press must add a cue. The
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

    // 4c) Display settings dialog (issues 14 + 15): the dialog's System
    //     dropdown relinks a display to another system - Apply with a
    //     different selection moves it (display.add + display.set on the
    //     target, display.remove on the source) - and its Name field
    //     renames it (display.set carries the name). Open the dialog on
    //     the engine's first display (it opened on the active system,
    //     self-test-2 after 4b), rename it, point the System dropdown at
    //     the other system and Apply: verify the engine holds the display
    //     under a fresh id (display ids never repeat) with the new name
    //     and the same geometry and brightness. Then reopen the dialog on
    //     the moved display, rename it back and Apply on its own system:
    //     a plain display.set, no move.
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
        // The dialog opens from the active system's snapshot (the UI's
        // Edit... button only exists on a display row, which only exists
        // when the snapshot reports displays). The active system's first
        // status poll lands ~500 ms after its connect (step 4b), which can
        // still be in flight here - wait for it before opening.
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
        liveReport += std::string(snapHasDisplay ? "PASS " : "FAIL ") +
                      "active system snapshot reports the displays\n";
        liveOk = liveOk && snapHasDisplay;
        if (haveDisplay && snapHasDisplay)
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
            // The dialog opened on the active system (index 1, self-test-2);
            // rename the display (issue 15) and select the other system so
            // Apply is a move that carries the new name (display.add takes
            // it), then press the dialog's Apply button (the exact action
            // the UI runs).
            app.testSetDisplayEditName(movedName);
            app.testSelectDisplayEditSystem(0);
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
        // the active system's first display, now the moved one, with the
        // dropdown on its owning system - rename it back and Apply: a
        // plain display.set with the new name, no move.
        bool renamedOk = false;
        nlohmann::json after2;
        if (movedOk)
        {
            // The active system's snapshot polls every 500 ms; wait until
            // it reports the moved display, or the reopen could open on
            // the stale (already removed) record.
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
    return (liveOk && modalOk && rendered > 0) ? 0 : 1;
}

} // namespace mediamax::gui
