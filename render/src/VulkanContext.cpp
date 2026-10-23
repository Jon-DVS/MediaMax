#include "mediamax/render/VulkanContext.h"

#include <algorithm>

#include "mediamax/util/Logger.h"

// NVRHI's Vulkan backend compiles vk-hpp with VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
// (nvrhi/src/vulkan/vulkan-backend.h). In dynamic-dispatch mode the header only
// declares the default dispatcher; exactly one translation unit in each final
// link must define it. This is that definition for every executable that links
// mediamax_render (the playback engine and the render test runner).
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace mediamax::render
{

namespace
{

// NVRHI error/message sink: forwards to the MediaMax logger.
class NvrhiMessageSink : public nvrhi::IMessageCallback
{
public:
    void message(nvrhi::MessageSeverity severity, const char* messageText) override
    {
        auto& log = Logger::instance();
        switch (severity)
        {
            case nvrhi::MessageSeverity::Info:
                log.info("NVRHI: {}", messageText);
                break;
            case nvrhi::MessageSeverity::Warning:
                log.warning("NVRHI: {}", messageText);
                break;
            case nvrhi::MessageSeverity::Error:
                log.error("NVRHI: {}", messageText);
                break;
        }
    }
};

std::string vkResultString(VkResult result)
{
    return std::string("VkResult ") + std::to_string(static_cast<int>(result)) +
           " (" + nvrhi::vulkan::resultToString(result) + ")";
}

// Deduplicates extension names (GLFW's list may overlap VK_KHR_surface).
std::vector<std::string> dedupe(std::vector<const char*> in)
{
    std::vector<std::string> out;
    for (const char* name : in)
    {
        if (std::find(out.begin(), out.end(), name) == out.end())
        {
            out.emplace_back(name);
        }
    }
    return out;
}

bool validationLayerAvailable()
{
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS || count == 0)
    {
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS)
    {
        return false;
    }
    for (const auto& layer : layers)
    {
        if (std::string(layer.layerName) == "VK_LAYER_KHRONOS_validation")
        {
            return true;
        }
    }
    return false;
}

// Finds a queue family with graphics support (transfer preferred).
bool findGraphicsQueueFamily(VkPhysicalDevice phys, std::uint32_t& outFamily, std::uint32_t& outCount)
{
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, families.data());

    for (std::uint32_t i = 0; i < count; ++i)
    {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
            (families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0)
        {
            outFamily = i;
            outCount = families[i].queueCount;
            return true;
        }
    }
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
        {
            outFamily = i;
            outCount = families[i].queueCount;
            return true;
        }
    }
    return false;
}

} // namespace

bool VulkanContext::createInstanceInternal(const VulkanContextDesc& desc, VulkanContext& out,
                                           std::string& error)
{
    if (!nvrhi::verifyHeaderVersion())
    {
        error = "NVRHI header version mismatch";
        return false;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = desc.appName;
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "MediaMax";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    std::vector<std::string> extensions = dedupe(desc.instanceExtensions);
    if (desc.surface != VK_NULL_HANDLE &&
        std::find(extensions.begin(), extensions.end(), VK_KHR_SURFACE_EXTENSION_NAME) == extensions.end())
    {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    }
    out.m_InstanceExtensions = extensions;
    std::vector<const char*> extNames;
    for (const auto& e : out.m_InstanceExtensions)
    {
        extNames.push_back(e.c_str());
    }

    std::vector<const char*> layers;
    if (desc.enableVulkanValidation)
    {
        if (validationLayerAvailable())
        {
            layers.push_back("VK_LAYER_KHRONOS_validation");
        }
        else
        {
            Logger::instance().warning(
                "Vulkan validation layer requested but not installed; continuing without it");
        }
    }

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(extNames.size());
    instanceInfo.ppEnabledExtensionNames = extNames.empty() ? nullptr : extNames.data();
    instanceInfo.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

    VkResult res = vkCreateInstance(&instanceInfo, nullptr, &out.m_Instance);
    if (res != VK_SUCCESS)
    {
        error = "vkCreateInstance failed: " + vkResultString(res);
        return false;
    }
    return true;
}

bool VulkanContext::createInstance(const VulkanContextDesc& desc, VulkanContext& out, std::string& error)
{
    return createInstanceInternal(desc, out, error);
}

bool VulkanContext::initPhysicalDevice(VkSurfaceKHR preferredSurface, std::string& error)
{
    if (m_Instance == VK_NULL_HANDLE)
    {
        error = "instance not created";
        return false;
    }
    m_HasSurface = preferredSurface != VK_NULL_HANDLE;

    std::uint32_t deviceCount = 0;
    VkResult res = vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);
    if (res != VK_SUCCESS || deviceCount == 0)
    {
        error = "no Vulkan physical devices found";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());

    bool found = false;
    for (VkPhysicalDevice phys : devices)
    {
        std::uint32_t family = 0, queueCount = 0;
        if (!findGraphicsQueueFamily(phys, family, queueCount))
        {
            continue;
        }
        if (m_HasSurface)
        {
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(phys, family, preferredSurface, &supported);
            if (!supported)
            {
                continue;
            }
        }
        m_PhysicalDevice = phys;
        m_GraphicsQueueFamily = family;
        found = true;
        break;
    }
    if (!found)
    {
        error = m_HasSurface ? "no physical device supports the window surface"
                             : "no physical device with a graphics queue found";
        return false;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_PhysicalDevice, &props);
    m_DeviceName = props.deviceName;
    Logger::instance().info("Vulkan device: {} (driver {}.{}.{})", m_DeviceName,
                            VK_VERSION_MAJOR(props.driverVersion),
                            VK_VERSION_MINOR(props.driverVersion),
                            VK_VERSION_PATCH(props.driverVersion));
    return true;
}

bool VulkanContext::initDevice(std::string& error)
{
    if (m_Instance == VK_NULL_HANDLE || m_PhysicalDevice == VK_NULL_HANDLE)
    {
        error = "instance/physical device not initialized";
        return false;
    }

    if (m_HasSurface)
    {
        m_DeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    const float priority = 1.f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = m_GraphicsQueueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    // char* view of the member strings (must stay alive for the NVRHI device).
    std::vector<const char*> deviceExtNames;
    for (const auto& e : m_DeviceExtensions)
    {
        deviceExtNames.push_back(e.c_str());
    }
    std::vector<const char*> instanceExtNames;
    for (const auto& e : m_InstanceExtensions)
    {
        instanceExtNames.push_back(e.c_str());
    }

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtNames.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtNames.empty() ? nullptr : deviceExtNames.data();

    VkResult res = vkCreateDevice(m_PhysicalDevice, &deviceInfo, nullptr, &m_Device);
    if (res != VK_SUCCESS)
    {
        error = "vkCreateDevice failed: " + vkResultString(res);
        return false;
    }
    vkGetDeviceQueue(m_Device, m_GraphicsQueueFamily, 0, &m_GraphicsQueue);

    // NVRHI is linked statically, so it does not initialize the vk-hpp
    // default dispatcher itself (that only happens for
    // NVRHI_SHARED_LIBRARY_BUILD; see nvrhi/src/vulkan/vulkan-device.cpp).
    // In dynamic-dispatch mode every vk-hpp call goes through the default
    // dispatcher's table, which is full of nulls until it is populated --
    // initialize it before any NVRHI/Vulkan work on this device.
    // All four arguments are required by the SDK's vulkan.hpp overload
    // (v1.4.363): the device proc-addr must be handed over explicitly.
    VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Instance, vkGetInstanceProcAddr, m_Device,
                                       vkGetDeviceProcAddr);

    static NvrhiMessageSink s_NvrhiSink;

    nvrhi::vulkan::DeviceDesc nd{};
    nd.errorCB = &s_NvrhiSink;
    nd.instance = m_Instance;
    nd.physicalDevice = m_PhysicalDevice;
    nd.device = m_Device;
    nd.graphicsQueue = m_GraphicsQueue;
    nd.graphicsQueueIndex = static_cast<int>(m_GraphicsQueueFamily);
    nd.instanceExtensions = instanceExtNames.empty() ? nullptr : instanceExtNames.data();
    nd.numInstanceExtensions = static_cast<std::size_t>(instanceExtNames.size());
    nd.deviceExtensions = deviceExtNames.empty() ? nullptr : deviceExtNames.data();
    nd.numDeviceExtensions = static_cast<std::size_t>(deviceExtNames.size());

    m_NvrhiDevice = nvrhi::vulkan::createDevice(nd);
    if (!m_NvrhiDevice)
    {
        error = "nvrhi::vulkan::createDevice failed";
        return false;
    }
    return true;
}

bool VulkanContext::create(const VulkanContextDesc& desc, VulkanContext& out, std::string& error)
{
    if (!createInstanceInternal(desc, out, error))
    {
        return false;
    }
    if (!out.initPhysicalDevice(desc.surface, error))
    {
        return false;
    }
    if (!out.initDevice(error))
    {
        return false;
    }
    return true;
}

VulkanContext::~VulkanContext()
{
    // The NVRHI device releases its handle on the VkDevice before we
    // destroy it (member destruction order: m_NvrhiDevice last of the three).
    if (m_NvrhiDevice)
    {
        m_NvrhiDevice = nullptr;
    }
    if (m_Device != VK_NULL_HANDLE)
    {
        vkDestroyDevice(m_Device, nullptr);
        m_Device = VK_NULL_HANDLE;
    }
    if (m_Instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(m_Instance, nullptr);
        m_Instance = VK_NULL_HANDLE;
    }
}

VulkanContext::VulkanContext(VulkanContext&& other) noexcept
{
    *this = std::move(other);
}

VulkanContext& VulkanContext::operator=(VulkanContext&& other) noexcept
{
    if (this != &other)
    {
        // Destroy our handles (if any), then take the other's.
        if (m_NvrhiDevice)
        {
            m_NvrhiDevice = nullptr;
        }
        if (m_Device != VK_NULL_HANDLE)
        {
            vkDestroyDevice(m_Device, nullptr);
        }
        if (m_Instance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(m_Instance, nullptr);
        }
        m_Instance = other.m_Instance;
        m_PhysicalDevice = other.m_PhysicalDevice;
        m_Device = other.m_Device;
        m_GraphicsQueue = other.m_GraphicsQueue;
        m_GraphicsQueueFamily = other.m_GraphicsQueueFamily;
        m_HasSurface = other.m_HasSurface;
        m_InstanceExtensions = std::move(other.m_InstanceExtensions);
        m_DeviceExtensions = std::move(other.m_DeviceExtensions);
        m_NvrhiDevice = std::move(other.m_NvrhiDevice);
        m_DeviceName = std::move(other.m_DeviceName);
        other.m_Instance = VK_NULL_HANDLE;
        other.m_PhysicalDevice = VK_NULL_HANDLE;
        other.m_Device = VK_NULL_HANDLE;
        other.m_GraphicsQueue = VK_NULL_HANDLE;
        other.m_GraphicsQueueFamily = 0;
        other.m_HasSurface = false;
    }
    return *this;
}

} // namespace mediamax::render
