#include "mediamax/render/Swapchain.h"

#include <algorithm>

#include "mediamax/util/Logger.h"

namespace mediamax::render
{

namespace
{

// The formats a swapchain may have (what create() accepts).
bool nvrhiFormatFor(VkFormat vkFormat, nvrhi::Format& out)
{
    switch (vkFormat)
    {
        case VK_FORMAT_B8G8R8A8_UNORM: out = nvrhi::Format::BGRA8_UNORM; return true;
        case VK_FORMAT_B8G8R8A8_SRGB:  out = nvrhi::Format::SBGRA8_UNORM; return true;
        case VK_FORMAT_R8G8B8A8_UNORM: out = nvrhi::Format::RGBA8_UNORM; return true;
        case VK_FORMAT_R8G8B8A8_SRGB:  out = nvrhi::Format::SRGBA8_UNORM; return true;
        default: return false;
    }
}

std::string vkResultString(VkResult result)
{
    return std::string("VkResult ") + std::to_string(static_cast<int>(result)) +
           " (" + nvrhi::vulkan::resultToString(result) + ")";
}

} // namespace

bool Swapchain::create(const Desc& desc, std::string& error)
{
    if (desc.surface == VK_NULL_HANDLE || desc.device == VK_NULL_HANDLE ||
        desc.nvrhiDevice == nullptr || desc.width <= 0 || desc.height <= 0)
    {
        error = "invalid swapchain description";
        return false;
    }
    m_Surface = desc.surface;
    m_PhysicalDevice = desc.physicalDevice;
    m_Device = desc.device;
    m_NvrhiDevice = desc.nvrhiDevice;
    m_Vsync = desc.vsync;
    return build(desc, desc.width, desc.height, error);
}

bool Swapchain::build(const Desc& desc, int width, int height, std::string& error)
{
    // Surface capabilities.
    VkSurfaceCapabilitiesKHR caps{};
    VkResult res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(desc.physicalDevice, desc.surface, &caps);
    if (res != VK_SUCCESS)
    {
        error = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: " + vkResultString(res);
        return false;
    }

    // Format selection: prefer plain B8G8R8A8_UNORM (no sRGB conversion in
    // the driver; our pipeline does its own color math).
    std::uint32_t formatCount = 0;
    res = vkGetPhysicalDeviceSurfaceFormatsKHR(desc.physicalDevice, desc.surface, &formatCount, nullptr);
    if (res != VK_SUCCESS || formatCount == 0)
    {
        error = "vkGetPhysicalDeviceSurfaceFormatsKHR failed: " + vkResultString(res);
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(desc.physicalDevice, desc.surface, &formatCount, formats.data());

    VkFormat chosen = VK_FORMAT_B8G8R8A8_UNORM;
    bool haveChosen = false;
    for (const auto& f : formats)
    {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM)
        {
            haveChosen = true;
            break;
        }
    }
    if (!haveChosen)
    {
        chosen = formats[0].format;
    }
    nvrhi::Format nvrhiFormat = nvrhi::Format::UNKNOWN;
    if (!nvrhiFormatFor(chosen, nvrhiFormat))
    {
        error = "unsupported swapchain format " +
                std::to_string(static_cast<int>(chosen));
        return false;
    }

    // Image count: a few frames of headroom (single-flight present model).
    std::uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
    {
        imageCount = caps.maxImageCount;
    }
    imageCount = std::max<std::uint32_t>(imageCount, 3);

    // Present mode: FIFO (vsync on, compositor-paced) or MAILBOX (vsync
    // off: acquire does not block on a full image queue, so a fresh frame
    // reaches the next refresh boundary sooner; the compositor still flips
    // at the display's refresh rate). Surfaces that do not support mailbox
    // fall back to FIFO.
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!desc.vsync)
    {
        std::uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(desc.physicalDevice, desc.surface, &modeCount,
                                                  nullptr);
        if (modeCount > 0)
        {
            std::vector<VkPresentModeKHR> modes(modeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(desc.physicalDevice, desc.surface, &modeCount,
                                                      modes.data());
            for (const VkPresentModeKHR m : modes)
            {
                if (m == VK_PRESENT_MODE_MAILBOX_KHR)
                {
                    presentMode = m;
                    break;
                }
            }
        }
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = desc.surface;
    info.minImageCount = imageCount;
    info.imageFormat = chosen;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent.width = static_cast<std::uint32_t>(width);
    info.imageExtent.height = static_cast<std::uint32_t>(height);
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;

    res = vkCreateSwapchainKHR(desc.device, &info, nullptr, &m_Swapchain);
    if (res != VK_SUCCESS)
    {
        error = "vkCreateSwapchainKHR failed: " + vkResultString(res);
        return false;
    }

    res = vkGetSwapchainImagesKHR(desc.device, m_Swapchain, &imageCount, nullptr);
    if (res != VK_SUCCESS)
    {
        error = "vkGetSwapchainImagesKHR failed: " + vkResultString(res);
        return false;
    }
    m_RawImages.resize(imageCount);
    vkGetSwapchainImagesKHR(desc.device, m_Swapchain, &imageCount, m_RawImages.data());

    m_Format = chosen;
    m_Width = width;
    m_Height = height;
    m_Vsync = desc.vsync;
    m_Images.clear();
    m_Images.reserve(imageCount);
    for (std::uint32_t i = 0; i < imageCount; ++i)
    {
        nvrhi::TextureDesc td;
        td.setWidth(static_cast<std::uint32_t>(width))
            .setHeight(static_cast<std::uint32_t>(height))
            .setFormat(nvrhiFormat)
            .setDimension(nvrhi::TextureDimension::Texture2D)
            .setIsRenderTarget(true)
            .setDebugName("swapchain image " + std::to_string(i));
        auto handle = m_NvrhiDevice->createHandleForNativeTexture(
            nvrhi::ObjectTypes::VK_Image, nvrhi::Object(m_RawImages[i]), td);
        if (!handle)
        {
            error = "failed to wrap swapchain image in NVRHI";
            return false;
        }
        m_Images.push_back(std::move(handle));
    }
    return true;
}

bool Swapchain::recreate(int width, int height, bool vsync, std::string& error)
{
    if (width <= 0 || height <= 0)
    {
        error = "invalid size";
        return false;
    }
    releaseImages();
    if (m_Swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
        m_Swapchain = VK_NULL_HANDLE;
    }
    Desc desc{};
    desc.surface = m_Surface;
    desc.physicalDevice = m_PhysicalDevice;
    desc.device = m_Device;
    desc.nvrhiDevice = m_NvrhiDevice;
    desc.width = width;
    desc.height = height;
    desc.vsync = vsync;
    return build(desc, width, height, error);
}

void Swapchain::destroy()
{
    releaseImages();
    if (m_Swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
        m_Swapchain = VK_NULL_HANDLE;
    }
    m_Width = 0;
    m_Height = 0;
}

void Swapchain::releaseImages()
{
    m_Images.clear();
    m_RawImages.clear();
}

bool Swapchain::acquire(std::uint32_t& outImageIndex, std::uint32_t timeoutMs, std::string& error)
{
    if (m_Swapchain == VK_NULL_HANDLE)
    {
        error = "swapchain not created";
        return false;
    }
    // Synchronous acquire (no semaphore): blocks until the driver has a free
    // image. See Swapchain.h for the present model. A timeout means no image
    // became free (e.g. a wedged present); outImageIndex is not trustworthy,
    // so it is a failure like any other.
    VkResult res = vkAcquireNextImageKHR(m_Device, m_Swapchain,
                                         static_cast<std::uint64_t>(timeoutMs) * 1000000ull,
                                         VK_NULL_HANDLE, VK_NULL_HANDLE, &outImageIndex);
    if (res != VK_SUCCESS)
    {
        error = "vkAcquireNextImageKHR failed: " + vkResultString(res);
        return false;
    }
    return true;
}

bool Swapchain::present(VkQueue queue, std::uint32_t imageIndex, std::string& error)
{
    if (m_Swapchain == VK_NULL_HANDLE)
    {
        error = "swapchain not created";
        return false;
    }
    const std::uint32_t index = imageIndex;
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.swapchainCount = 1;
    info.pSwapchains = &m_Swapchain;
    info.pImageIndices = &index;

    VkResult res = vkQueuePresentKHR(queue, &info);
    if (res != VK_SUCCESS)
    {
        // Out of date (resize) is expected and handled by the caller.
        error = "vkQueuePresentKHR failed: " + vkResultString(res);
        return false;
    }
    return true;
}

nvrhi::TextureHandle Swapchain::image(std::uint32_t index) const
{
    if (index >= m_Images.size())
    {
        return nullptr;
    }
    return m_Images[index];
}

Swapchain::~Swapchain()
{
    destroy();
}

Swapchain::Swapchain(Swapchain&& other) noexcept
{
    *this = std::move(other);
}

Swapchain& Swapchain::operator=(Swapchain&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        m_Swapchain = other.m_Swapchain;
        m_RawImages = std::move(other.m_RawImages);
        m_Images = std::move(other.m_Images);
        m_Format = other.m_Format;
        m_Width = other.m_Width;
        m_Height = other.m_Height;
        m_Surface = other.m_Surface;
        m_PhysicalDevice = other.m_PhysicalDevice;
        m_Device = other.m_Device;
        m_NvrhiDevice = other.m_NvrhiDevice;
        other.m_Swapchain = VK_NULL_HANDLE;
        other.m_Surface = VK_NULL_HANDLE;
        other.m_PhysicalDevice = VK_NULL_HANDLE;
        other.m_Device = VK_NULL_HANDLE;
        other.m_NvrhiDevice = nullptr;
    }
    return *this;
}

} // namespace mediamax::render
