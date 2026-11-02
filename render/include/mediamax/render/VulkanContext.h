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
#include <optional>
#include <string>
#include <vector>

namespace mediamax::render
{

// One physical device visible to the instance (the status result's
// "renderDevice.available" entries and the system.setRenderDevice target
// selection).
struct VulkanDeviceInfo
{
    std::uint32_t index = 0; // position in the instance enumeration
    std::string name;
    std::string type; // "Discrete GPU" / "Integrated GPU" / "Virtual GPU" / "CPU" / "Other"
    VkPhysicalDevice handle = VK_NULL_HANDLE;
};

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

    // Selects the physical device. When `preferredDevice` is non-empty, the
    // device with exactly that name (case-insensitive); otherwise the first
    // render-eligible device in enumeration order. A device that does not
    // support `preferredSurface` (when given) is never selected. Must
    // follow createInstance().
    bool initPhysicalDevice(VkSurfaceKHR preferredSurface, const std::string& preferredDevice,
                            std::string& error);

    // Default (first eligible) device selection.
    bool initPhysicalDevice(VkSurfaceKHR preferredSurface, std::string& error);

    // All physical devices the instance sees. Requires createInstance();
    // empty before that.
    std::vector<VulkanDeviceInfo> enumerateDevices() const;

    // Exact-name lookup (case-insensitive) into enumerateDevices();
    // std::nullopt when no device has that name.
    std::optional<VulkanDeviceInfo> findDevice(const std::string& name) const;

    // True when the device would pass initPhysicalDevice's filters: a
    // graphics queue family plus surface-present support when `surface`
    // is not VK_NULL_HANDLE. Instance-level queries (safe from any thread).
    bool canRenderOn(VkPhysicalDevice device, VkSurfaceKHR surface) const;

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
    // The selected device's type string (see VulkanDeviceInfo::type).
    const std::string& deviceType() const { return m_DeviceType; }

    // Destroys the logical device and the NVRHI wrapper, keeping the
    // instance: the teardown half of a render device switch (system.
    // setRenderDevice). The caller must first release everything NVRHI
    // (scene renderer, swapchains) and raw-Vulkan (fences) it owns and
    // quiesce the device (waitForIdle); afterwards initPhysicalDevice() /
    // initDevice() run again on the same instance.
    void resetDevice();

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
    std::string m_DeviceType;
};

} // namespace mediamax::render
