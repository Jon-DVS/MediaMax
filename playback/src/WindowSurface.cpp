#include "WindowSurface.h"

#include "mediamax/util/Logger.h"

// GLFW's native (platform window handle) accessors must be exposed before
// including the native header. Expose only this platform's accessor: the
// header includes that platform's system headers, so exposing all three at
// once would pull in Windows, macOS and X11 headers together.
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
#define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <X11/Xlib.h>
#endif

namespace mediamax
{

bool createWindowSurface(VkInstance instance, GLFWwindow* window, VkSurfaceKHR& out, std::string& error)
{
    out = VK_NULL_HANDLE;
    if (window == nullptr || instance == VK_NULL_HANDLE)
    {
        error = "null window or instance";
        return false;
    }

#if defined(VK_USE_PLATFORM_WIN32_KHR)
    HWND hwnd = glfwGetWin32Window(window);
    if (hwnd == nullptr)
    {
        error = "failed to get the Win32 window handle";
        return false;
    }
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = hwnd;
    VkResult res = vkCreateWin32SurfaceKHR(instance, &info, nullptr, &out);
    if (res != VK_SUCCESS)
    {
        error = "vkCreateWin32SurfaceKHR failed: " + std::to_string(static_cast<int>(res));
        return false;
    }
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    Display* display = glfwGetX11Display();
    Window xid = glfwGetX11Window(window);
    if (display == nullptr || xid == 0)
    {
        error = "failed to get the X11 display/window";
        return false;
    }
    VkXlibSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    info.dpy = display;
    info.window = xid;
    info.applicationName = "MediaMax";
    VkResult res = vkCreateXlibSurfaceKHR(instance, &info, nullptr, &out);
    if (res != VK_SUCCESS)
    {
        error = "vkCreateXlibSurfaceKHR failed: " + std::to_string(static_cast<int>(res));
        return false;
    }
#elif defined(VK_USE_PLATFORM_MACOSX_SURFACE_EXT)
    // Cocoa/Metal surfaces are not supported yet; only Windows and X11 are
    // in the MediaMax platform matrix.
    error = "no window surface backend for this platform";
    return false;
#else
    error = "no window surface backend for this platform";
    return false;
#endif

    return true;
}

} // namespace mediamax
