#pragma once

// A minimal Vulkan surface for the control GUI (Dear ImGui rendering).
//
// The control app links the core only (no NVRHI, no mediamax_render), so this
// owns a small raw Vulkan context of its own: instance, window surface,
// device, swapchain, and one in-flight frame (two fences + two semaphores) -
// the standard immediate-mode setup. It also owns this process's vk-hpp default
// dispatcher (exactly one translation unit per binary must define it; see
// render/src/VulkanContext.cpp for the same pattern in the render side).

#include <cstdint>
#include <string>
#include <vector>

// The platform window-surface structs (VkSurfaceCreateInfoKHR & co.) are only
// declared when the matching platform macro is set; GLFW's vk surface
// functions expect the same macro to be in effect.
#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__APPLE__)
#define VK_USE_PLATFORM_MACOSX_SURFACE_EXT
#else
#define VK_USE_PLATFORM_XLIB_KHR
#endif
#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace mediamax::gui
{

class GuiVulkan
{
public:
    GuiVulkan() = default;
    ~GuiVulkan();

    GuiVulkan(const GuiVulkan&) = delete;
    GuiVulkan& operator=(const GuiVulkan&) = delete;

    // Creates the instance, window surface, device and swapchain.
    bool init(GLFWwindow* window, std::string& error);

    // Releases everything. Safe to call without init().
    void shutdown();

    bool ready() const { return m_Device != VK_NULL_HANDLE; }

    // Values for the Dear ImGui Vulkan back end.
    VkInstance instance() const { return m_Instance; }
    VkPhysicalDevice physicalDevice() const { return m_PhysicalDevice; }
    VkDevice device() const { return m_Device; }
    VkQueue queue() const { return m_Queue; }
    std::uint32_t queueFamily() const { return m_QueueFamily; }
    VkRenderPass renderPass() const { return m_RenderPass; }
    std::size_t imageCount() const { return m_Images.size(); }

    // Call when the window is resized (rebuilds the swapchain).
    void recreateSwapchain();

    // Begins the in-flight frame: waits on the fence, acquires an image,
    // begins the command buffer and render pass. Returns false when the
    // swapchain is out of date or suboptimal (call recreateSwapchain() and
    // retry); the frame is then not active and endFrame() must not run.
    bool beginFrame();

    // Ends the frame: closes the render pass, submits and presents.
    void endFrame(VkResult& presentResult);

    // The draw size (swapchain extent).
    std::uint32_t width() const { return m_Extent.width; }
    std::uint32_t height() const { return m_Extent.height; }

    // The command buffer the frame is recorded into (valid between
    // beginFrame() and endFrame()).
    VkCommandBuffer commandBuffer() const { return m_CommandBuffer; }

private:
    bool createInstanceAndSurface(GLFWwindow* window, std::string& error);
    bool createDevice(std::string& error);
    bool createSwapchain(std::string& error);
    void destroySwapchain();
    bool createRenderPass();
    bool createFramebuffers();

    GLFWwindow* m_Window = nullptr;
    VkInstance m_Instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice m_Device = VK_NULL_HANDLE;
    VkQueue m_Queue = VK_NULL_HANDLE;
    std::uint32_t m_QueueFamily = 0;
    VkRenderPass m_RenderPass = VK_NULL_HANDLE;
    VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
    VkFormat m_Format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D m_Extent{0, 0};
    std::vector<VkImage> m_Images;
    std::vector<VkImageView> m_ImageViews;
    std::vector<VkFramebuffer> m_Framebuffers;
    VkCommandPool m_CommandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;
    VkFence m_Fence = VK_NULL_HANDLE;
    VkFence m_PresentFence = VK_NULL_HANDLE;
    VkSemaphore m_ImageAvailable = VK_NULL_HANDLE;
    VkSemaphore m_Rendered = VK_NULL_HANDLE;
    std::uint32_t m_ImageIndex = 0;
};

} // namespace mediamax::gui
