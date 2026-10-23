#pragma once

// Vulkan instance / physical device / logical device bootstrap.
//
// The context owns the raw Vulkan handles and wraps the logical device in an
// NVRHI device (nvrhi::vulkan::createDevice). All higher-level rendering
// (swapchains, scene render targets, pipelines) hangs off the NVRHI device.
//
// Two bootstrap modes:
//   - create(): single call. Headless (tests, self-test) or when the caller
//     already holds a surface.
//   - createInstance() / initPhysicalDevice() / initDevice(): two-phase
//     bootstrap for multi-window apps, where the window surfaces must be
//     created between the instance and the device (the device needs the
//     swapchain extension, the surfaces need the instance).
//
// Surfaces are always host-owned; the context only borrows one for physical
// device selection and the swapchain-extension decision.
//
// MediaMax uses Vulkan on all platforms; there is no other backend.

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mediamax::render
{

struct VulkanContextDesc
{
    const char* appName = "MediaMax";
    // Instance extensions the host requires (e.g. GLFW's window surface
    // extensions). Empty for headless contexts.
    std::vector<const char*> instanceExtensions;
    // Surface for physical device selection and the swapchain-extension
    // decision; VK_NULL_HANDLE for headless contexts. Host-owned.
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    // Enable the Khronos validation layer when it is installed.
    bool enableVulkanValidation = false;
};

class VulkanContext
{
public:
    // Full bootstrap in one call (see the class comment). Returns false with
    // `error` set on failure.
    static bool create(const VulkanContextDesc& desc, VulkanContext& out, std::string& error);

    // --- Two-phase bootstrap (multi-window) ---------------------------------

    // Creates the instance only (with the requested extensions).
    static bool createInstance(const VulkanContextDesc& desc, VulkanContext& out, std::string& error);

    // Selects the physical device (preferring one that supports
    // `preferredSurface`, when given). Must follow createInstance().
    bool initPhysicalDevice(VkSurfaceKHR preferredSurface, std::string& error);

    // Creates the logical device (with the swapchain extension when
    // hasSurface()) and the NVRHI wrapper. Must follow initPhysicalDevice().
    bool initDevice(std::string& error);

    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(VulkanContext&& other) noexcept;
    VulkanContext& operator=(VulkanContext&& other) noexcept;
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // The NVRHI device: all resource creation and command lists go through
    // it. Null until initDevice() has run.
    nvrhi::DeviceHandle nvrhiDevice() const { return m_NvrhiDevice; }
    nvrhi::vulkan::IDevice* vulkanDevice() const { return m_NvrhiDevice.Get(); }

    VkInstance instance() const { return m_Instance; }
    VkPhysicalDevice physicalDevice() const { return m_PhysicalDevice; }
    VkDevice device() const { return m_Device; }
    VkQueue graphicsQueue() const { return m_GraphicsQueue; }
    std::uint32_t graphicsQueueFamilyIndex() const { return m_GraphicsQueueFamily; }

    // True when the device was created with the swapchain extension.
    bool hasSurface() const { return m_HasSurface; }
    const std::string& deviceName() const { return m_DeviceName; }

private:
    // Shared by create() and createInstance().
    static bool createInstanceInternal(const VulkanContextDesc& desc, VulkanContext& out,
                                       std::string& error);

    VkInstance m_Instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice m_Device = VK_NULL_HANDLE;
    VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
    std::uint32_t m_GraphicsQueueFamily = 0;
    bool m_HasSurface = false;
    // Extension names kept for the NVRHI device wrapper (must outlive it).
    std::vector<std::string> m_InstanceExtensions;
    std::vector<std::string> m_DeviceExtensions;
    nvrhi::vulkan::DeviceHandle m_NvrhiDevice;
    std::string m_DeviceName;
};

} // namespace mediamax::render
