#pragma once

// Creates a VkSurfaceKHR for a GLFW window on the current platform.
//
// One surface per window is required for multi-display playback: each
// physical output has its own window and its own swapchain. The instance is
// supplied by the host (VulkanContext); GLFW provides the native window
// handle (Win32 HWND / X11 XID).

#include <string>

#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace mediamax
{

// Creates the window's surface on `instance`. Returns false with `error`
// set on failure. The caller owns (and must destroy) the surface.
bool createWindowSurface(VkInstance instance, GLFWwindow* window, VkSurfaceKHR& out, std::string& error);

// Keeps the window above all others (Win32 WS_EX_TOPMOST / EWMH
// _NET_WM_STATE_ABOVE). GLFW 3.4 has no always-on-top window hint, so the
// platform stacking attribute is toggled directly. Returns false on
// unsupported platforms or a bad window.
bool setWindowAlwaysOnTop(GLFWwindow* window, bool alwaysOnTop);

} // namespace mediamax
