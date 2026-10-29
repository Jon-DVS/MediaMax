#include "gui/GuiVulkan.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include <GLFW/glfw3.h>

// The control app owns the vk-hpp default dispatcher for this process:
// vulkan.hpp only declares it, and exactly one translation unit per binary
// must define it (the render library's definition is not linked here).
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace mediamax::gui
{

namespace
{

// Preferred surface formats, in order (the official example's preference
// list): the first one offered by the surface with the sRGB colorspace wins.
const VkFormat kPreferredFormats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                      VK_FORMAT_B8G8R8_UNORM,    VK_FORMAT_R8G8B8_UNORM};

const VkColorSpaceKHR kPreferredColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;

const VkClearValue kClearValue = {{0.06f, 0.08f, 0.12f, 1.0f}};

} // namespace

GuiVulkan::~GuiVulkan()
{
    shutdown();
}

bool GuiVulkan::createInstanceAndSurface(GLFWwindow* window, std::string& error)
{
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "MediaMax Control";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "mediamax_control_gui";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    std::uint32_t requiredCount = 0;
    const char** required = glfwGetRequiredInstanceExtensions(&requiredCount);
    if (!required)
    {
        error = "GLFW could not determine the required Vulkan instance extensions";
        return false;
    }
    std::vector<const char*> instanceExtensions(required, required + requiredCount);
    // The device-side VK_KHR_swapchain_maintenance1 (present fences) is only
    // enabled when VK_KHR_surface_maintenance1 is present, which itself needs
    // VK_KHR_get_surface_capabilities2.
    instanceExtensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    instanceExtensions.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);

    // Verify the driver supports every extension GLFW asks for.
    std::uint32_t supportedCount = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &supportedCount, nullptr) != VK_SUCCESS)
    {
        error = "vkEnumerateInstanceExtensionProperties failed";
        return false;
    }
    std::vector<VkExtensionProperties> supported(supportedCount);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &supportedCount, supported.data()) !=
        VK_SUCCESS)
    {
        error = "vkEnumerateInstanceExtensionProperties failed";
        return false;
    }
    for (const char* name : instanceExtensions)
    {
        bool found = false;
        for (const auto& ext : supported)
        {
            if (std::strcmp(ext.extensionName, name) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            error = std::string("the Vulkan driver does not support the required extension: ") +
                    name;
            return false;
        }
    }

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();

    if (vkCreateInstance(&instanceInfo, nullptr, &m_Instance) != VK_SUCCESS)
    {
        error = "vkCreateInstance failed";
        return false;
    }

    // GLFW creates the window surface (the raw vk surface-create entry point
    // is not needed; the platform surface struct was dropped from the SDK
    // headers in favor of the loader/ICD path).
    if (glfwCreateWindowSurface(m_Instance, window, nullptr, &m_Surface) != VK_SUCCESS)
    {
        error = "glfwCreateWindowSurface failed";
        return false;
    }
    return true;
}

bool GuiVulkan::createDevice(std::string& error)
{
    std::uint32_t deviceCount = 0;
    if (vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr) != VK_SUCCESS ||
        deviceCount == 0)
    {
        error = "no Vulkan physical devices found";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data()) != VK_SUCCESS)
    {
        error = "vkEnumeratePhysicalDevices failed";
        return false;
    }

    for (const VkPhysicalDevice candidate : devices)
    {
        // The present fence (used to bound semaphore reuse between frames)
        // comes from the swapchain maintenance 1 extension; skip devices
        // that do not provide it.
        std::uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        if (extCount > 0)
        {
            vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, exts.data());
        }
        bool hasPresentFenceExt = false;
        for (const VkExtensionProperties& ext : exts)
        {
            if (std::strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) == 0)
            {
                hasPresentFenceExt = true;
            }
        }
        if (!hasPresentFenceExt)
        {
            continue;
        }

        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());

        for (std::uint32_t i = 0; i < familyCount; ++i)
        {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
            {
                continue;
            }
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, m_Surface, &presentSupport);
            if (!presentSupport)
            {
                continue;
            }

            m_PhysicalDevice = candidate;
            m_QueueFamily = i;

            float priority = 1.0f;
            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = m_QueueFamily;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &priority;

            // The swapchain entry points are a device extension: without
            // enabling it, vkCreateSwapchainKHR resolves to a NULL loader
            // pointer (which fast-fails the process on present).
            const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                              VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
            VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maint1{};
            maint1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
            maint1.swapchainMaintenance1 = VK_TRUE;
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &maint1;

            VkDeviceCreateInfo deviceInfo{};
            deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            deviceInfo.pNext = &features2;
            deviceInfo.queueCreateInfoCount = 1;
            deviceInfo.pQueueCreateInfos = &queueInfo;
            deviceInfo.enabledExtensionCount = 2;
            deviceInfo.ppEnabledExtensionNames = deviceExtensions;

            if (vkCreateDevice(candidate, &deviceInfo, nullptr, &m_Device) != VK_SUCCESS)
            {
                error = "vkCreateDevice failed";
                return false;
            }
            vkGetDeviceQueue(m_Device, m_QueueFamily, 0, &m_Queue);

            // Populate the default dispatcher (all four arguments are required
            // by the SDK's vulkan.hpp overload).
            VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Instance, vkGetInstanceProcAddr, m_Device,
                                               vkGetDeviceProcAddr);
            return true;
        }
    }

    error = "no Vulkan device with a graphics + present queue family and the " +
            std::string(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) + " extension found";
    return false;
}

bool GuiVulkan::createSwapchain(VkSwapchainKHR oldSwapchain, std::string& error)
{
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_PhysicalDevice, m_Surface, &caps) !=
        VK_SUCCESS)
    {
        error = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
        return false;
    }

    std::uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, nullptr);
    if (formatCount == 0)
    {
        error = "the surface supports no formats";
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount,
                                         formats.data());

    // Pick the preferred format (see the official example): a single
    // VK_FORMAT_UNDEFINED entry means "any format", so take the first
    // preference; otherwise the first preference that is offered with the
    // sRGB colorspace; otherwise whatever the surface offers first.
    VkSurfaceFormatKHR surfaceFormat = formats[0];
    if (formatCount == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
    {
        surfaceFormat = {kPreferredFormats[0], kPreferredColorSpace};
    }
    else
    {
        for (const VkFormat preferred : kPreferredFormats)
        {
            for (const VkSurfaceFormatKHR& f : formats)
            {
                if (f.format == preferred && f.colorSpace == kPreferredColorSpace)
                {
                    surfaceFormat = f;
                    break;
                }
            }
            if (surfaceFormat.format == preferred && surfaceFormat.colorSpace == kPreferredColorSpace)
            {
                break;
            }
        }
    }
    m_Format = surfaceFormat.format;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == std::numeric_limits<std::uint32_t>::max() ||
        extent.height == std::numeric_limits<std::uint32_t>::max() || extent.width == 0 ||
        extent.height == 0)
    {
        // Dynamic or unavailable extent (window minimized, or a driver that
        // reports nothing yet): fall back to the window's framebuffer size.
        int w = 0;
        int h = 0;
        glfwGetFramebufferSize(m_Window, &w, &h);
        extent = {static_cast<std::uint32_t>(std::max(1, w)),
                  static_cast<std::uint32_t>(std::max(1, h))};
    }
    m_Extent = extent;

    std::uint32_t imageCount = caps.minImageCount + 1; // one in flight + present
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
    {
        imageCount = caps.maxImageCount;
    }
    imageCount = std::max<std::uint32_t>(imageCount, 2);

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = m_Surface;
    info.minImageCount = imageCount;
    info.imageFormat = m_Format;
    info.imageColorSpace = surfaceFormat.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    // Let the driver reuse the old swapchain's buffer memory when possible
    // (the official example does the same via its H helpers).
    info.oldSwapchain = oldSwapchain;

    m_Swapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(m_Device, &info, nullptr, &m_Swapchain) != VK_SUCCESS)
    {
        error = "vkCreateSwapchainKHR failed";
        m_Swapchain = VK_NULL_HANDLE;
        return false;
    }

    std::uint32_t actualCount = 0;
    if (vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &actualCount, nullptr) != VK_SUCCESS)
    {
        error = "vkGetSwapchainImagesKHR failed";
        return false;
    }
    m_Images.resize(actualCount);
    if (vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &actualCount, m_Images.data()) !=
        VK_SUCCESS)
    {
        error = "vkGetSwapchainImagesKHR failed";
        return false;
    }
    m_ImageViews.resize(actualCount);
    for (std::uint32_t i = 0; i < actualCount; ++i)
    {
        // Explicit fields: this SDK's VkImageSubresourceRange carries an
        // extra baseArrayLayer member, so brace-init would silently shift.
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = m_Images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = m_Format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.baseMipLevel = 0;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.baseArrayLayer = 0;
        view.subresourceRange.layerCount = 1;
        if (vkCreateImageView(m_Device, &view, nullptr, &m_ImageViews[i]) != VK_SUCCESS)
        {
            error = "vkCreateImageView failed";
            return false;
        }
    }
    return true;
}

void GuiVulkan::destroySwapchainResources()
{
    // Framebuffers reference the image views: release them first.
    for (const VkFramebuffer fb : m_Framebuffers)
    {
        vkDestroyFramebuffer(m_Device, fb, nullptr);
    }
    m_Framebuffers.clear();
    for (const VkImageView view : m_ImageViews)
    {
        vkDestroyImageView(m_Device, view, nullptr);
    }
    m_ImageViews.clear();
    m_Images.clear();
}

void GuiVulkan::destroySwapchain()
{
    destroySwapchainResources();
    if (m_Swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
        m_Swapchain = VK_NULL_HANDLE;
    }
}

bool GuiVulkan::createRenderPass()
{
    // Single color attachment: one per swapchain image (bound through the
    // framebuffers below); the render pass is cleared so the UI always
    // redraws fully. Created exactly once: it is format-bound and the format
    // never changes, so it (and the Dear ImGui pipeline compiled against
    // it) stays valid across swapchain recreates.
    VkAttachmentDescription color{};
    color.format = m_Format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // The attachment is presented: the contents must be defined at the end
    // of the pass (DONT_CARE would leave the presented pixels undefined).
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;

    if (m_RenderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
        m_RenderPass = VK_NULL_HANDLE;
    }
    return vkCreateRenderPass(m_Device, &info, nullptr, &m_RenderPass) == VK_SUCCESS;
}

bool GuiVulkan::createFramebuffers()
{
    m_Framebuffers.resize(m_ImageViews.size());
    for (std::uint32_t i = 0; i < m_ImageViews.size(); ++i)
    {
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = m_RenderPass;
        info.width = m_Extent.width;
        info.height = m_Extent.height;
        info.layers = 1;
        info.attachmentCount = 1;
        info.pAttachments = &m_ImageViews[i];
        if (vkCreateFramebuffer(m_Device, &info, nullptr, &m_Framebuffers[i]) != VK_SUCCESS)
        {
            for (std::uint32_t j = 0; j < i; ++j)
            {
                vkDestroyFramebuffer(m_Device, m_Framebuffers[j], nullptr);
            }
            m_Framebuffers.clear();
            return false;
        }
    }
    return true;
}

bool GuiVulkan::init(GLFWwindow* window, std::string& error)
{
    if (ready())
    {
        return true;
    }
    auto fail = [&](const char* what) -> bool {
        error = std::string(what);
        shutdown(); // releases whatever init created so far
        return false;
    };

    m_Window = window;
    if (!createInstanceAndSurface(window, error))
    {
        shutdown();
        return false;
    }
    if (!createDevice(error))
    {
        shutdown();
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_QueueFamily;
    if (vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_CommandPool) != VK_SUCCESS)
    {
        return fail("vkCreateCommandPool failed");
    }
    VkCommandBufferAllocateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    bufferInfo.commandPool = m_CommandPool;
    bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    bufferInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_Device, &bufferInfo, &m_CommandBuffer) != VK_SUCCESS)
    {
        return fail("vkAllocateCommandBuffers failed");
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    // Two fences, one in-flight frame: the submit fence bounds the command
    // buffer, the present fence bounds the semaphore pair (the present
    // must have consumed m_Rendered before the next frame re-signals it).
    if (vkCreateFence(m_Device, &fenceInfo, nullptr, &m_Fence) != VK_SUCCESS ||
        vkCreateFence(m_Device, &fenceInfo, nullptr, &m_PresentFence) != VK_SUCCESS)
    {
        return fail("vkCreateFence failed");
    }
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &m_ImageAvailable) != VK_SUCCESS ||
        vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &m_Rendered) != VK_SUCCESS)
    {
        return fail("vkCreateSemaphore failed");
    }

    if (!createSwapchain(VK_NULL_HANDLE, error))
    {
        shutdown();
        return false;
    }
    if (!createRenderPass())
    {
        return fail("vkCreateRenderPass failed");
    }
    if (!createFramebuffers())
    {
        return fail("vkCreateFramebuffer failed");
    }
    return true;
}

void GuiVulkan::recreateSwapchain()
{
    if (!ready())
    {
        return;
    }
    // No frame is in flight here: beginFrame() waits for the previous
    // frame's present before starting a new one, so the queue can be
    // drained safely.
    vkDeviceWaitIdle(m_Device);
    const VkSwapchainKHR oldSwapchain = m_Swapchain;
    // The old image views and framebuffers belong to the old swapchain.
    destroySwapchainResources();
    std::string error;
    // Create the new swapchain first (hinting the old one so the driver can
    // reuse its buffer memory, as the official example does). A failed
    // create leaves m_Swapchain missing; beginFrame() detects that and the
    // next frame retries with the current size. The render pass is not
    // touched: it is format-bound and the format never changes, so the
    // Dear ImGui pipeline compiled against it stays valid.
    if (createSwapchain(oldSwapchain, error))
    {
        createFramebuffers();
    }
    if (oldSwapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_Device, oldSwapchain, nullptr);
    }
}

bool GuiVulkan::beginFrame()
{
    if (m_Swapchain == VK_NULL_HANDLE || m_Framebuffers.empty())
    {
        return false; // missing swapchain (init not run, or a failed recreate)
    }
    // Wait for the previous frame to be *presented*, not just submitted: the
    // present consumes m_Rendered, which the next submit re-signals.
    VkFence bothFences[2] = {m_Fence, m_PresentFence};
    if (vkWaitForFences(m_Device, 2, bothFences, VK_TRUE, UINT64_MAX) != VK_SUCCESS ||
        vkResetFences(m_Device, 2, bothFences) != VK_SUCCESS)
    {
        return false;
    }

    // UINT64_MAX, as in the official example: the previous frame's present
    // has already completed (the fence wait above), so an image is available
    // and the wait cannot block indefinitely. A finite timeout risks leaving
    // m_ImageIndex stale on VK_TIMEOUT.
    const VkResult acquire =
        vkAcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX, m_ImageAvailable,
                              VK_NULL_HANDLE, &m_ImageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR || acquire == VK_SUBOPTIMAL_KHR)
    {
        return false; // stale swapchain: recreate and retry on the next frame
    }
    if (acquire != VK_SUCCESS && acquire != VK_TIMEOUT)
    {
        return false;
    }
    if (m_ImageIndex >= m_Framebuffers.size())
    {
        return false; // defensive: never index the framebuffer array out of range
    }

    // ONE_TIME_SUBMIT buffers must be reset before every new recording.
    if (vkResetCommandBuffer(m_CommandBuffer, 0) != VK_SUCCESS)
    {
        return false;
    }
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(m_CommandBuffer, &beginInfo) != VK_SUCCESS)
    {
        return false;
    }
    return true;
}

void GuiVulkan::beginRenderPass()
{
    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = m_RenderPass;
    passInfo.framebuffer = m_Framebuffers[m_ImageIndex];
    passInfo.renderArea = {{0, 0}, m_Extent};
    passInfo.clearValueCount = 1;
    passInfo.pClearValues = &kClearValue;
    vkCmdBeginRenderPass(m_CommandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
}

void GuiVulkan::endFrame(VkResult& presentResult)
{
    vkCmdEndRenderPass(m_CommandBuffer);
    vkEndCommandBuffer(m_CommandBuffer);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &m_ImageAvailable;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_CommandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &m_Rendered;
    vkQueueSubmit(m_Queue, 1, &submitInfo, m_Fence);

    VkSwapchainPresentFenceInfoKHR presentFenceInfo{};
    presentFenceInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR;
    presentFenceInfo.swapchainCount = 1;
    presentFenceInfo.pFences = &m_PresentFence;

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = &presentFenceInfo;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &m_Rendered;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_Swapchain;
    presentInfo.pImageIndices = &m_ImageIndex;
    presentResult = vkQueuePresentKHR(m_Queue, &presentInfo);
}

void GuiVulkan::waitForFrame()
{
    if (m_Device == VK_NULL_HANDLE || m_Fence == VK_NULL_HANDLE)
    {
        return;
    }
    vkWaitForFences(m_Device, 1, &m_Fence, VK_TRUE, UINT64_MAX);
}

void GuiVulkan::shutdown()
{
    // Handles partial init state: every resource is released only when it
    // exists.
    if (m_Device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(m_Device);
        destroySwapchain();
        if (m_RenderPass != VK_NULL_HANDLE)
        {
            vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
            m_RenderPass = VK_NULL_HANDLE;
        }
        if (m_CommandBuffer != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &m_CommandBuffer);
            m_CommandBuffer = VK_NULL_HANDLE;
        }
        if (m_CommandPool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(m_Device, m_CommandPool, nullptr);
            m_CommandPool = VK_NULL_HANDLE;
        }
        if (m_Fence != VK_NULL_HANDLE)
        {
            vkDestroyFence(m_Device, m_Fence, nullptr);
            m_Fence = VK_NULL_HANDLE;
        }
        if (m_PresentFence != VK_NULL_HANDLE)
        {
            vkDestroyFence(m_Device, m_PresentFence, nullptr);
            m_PresentFence = VK_NULL_HANDLE;
        }
        if (m_ImageAvailable != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(m_Device, m_ImageAvailable, nullptr);
            m_ImageAvailable = VK_NULL_HANDLE;
        }
        if (m_Rendered != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(m_Device, m_Rendered, nullptr);
            m_Rendered = VK_NULL_HANDLE;
        }
        vkDestroyDevice(m_Device, nullptr);
        m_Device = VK_NULL_HANDLE;
        m_Queue = VK_NULL_HANDLE;
    }
    if (m_Surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
        m_Surface = VK_NULL_HANDLE;
    }
    if (m_Instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(m_Instance, nullptr);
        m_Instance = VK_NULL_HANDLE;
    }
    m_PhysicalDevice = VK_NULL_HANDLE;
    m_Window = nullptr;
}

} // namespace mediamax::gui
