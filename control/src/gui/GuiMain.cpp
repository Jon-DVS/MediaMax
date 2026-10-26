#include "gui/GuiMain.h"

#include <cstdint>
#include <iostream>
#include <string>

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
    m_ImGuiReady = true;
    return true;
}

void GuiRuntime::shutdown()
{
    if (m_ImGuiReady)
    {
        vkDeviceWaitIdle(m_Vulkan.device()); // no frame in flight
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

int runGui(const std::string& systemsPath)
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

    ControlGuiApp app(systemsPath);
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

    // A config path that does not exist: the self-test UI starts with no
    // systems (deterministic; no side effects on real systems).
    ControlGuiApp app("gui_selftest_no_systems_config.json");
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
        const std::string host = liveSystem.substr(0, colon);
        std::uint16_t port = 47600;
        try
        {
            port = static_cast<std::uint16_t>(std::stoi(liveSystem.substr(colon + 1)));
        }
        catch (const std::exception&)
        {
            std::cerr << "mediamax_control: bad port in " << liveSystem << std::endl;
            runtime.shutdown();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 2;
        }
        liveOk = app.selfTestSystem(host, port, liveReport);
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

    runtime.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();

    std::cout << "gui-self-test: rendered " << rendered << "/" << frames << " frames" << std::endl;
    if (!liveSystem.empty())
    {
        std::cout << liveReport;
        std::cout << (liveOk ? "gui-self-test: live system OK" : "gui-self-test: live system FAILED")
                  << std::endl;
    }
    return (liveOk && rendered > 0) ? 0 : 1;
}

} // namespace mediamax::gui
