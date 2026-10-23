#pragma once

// A swapchain for one window: the raw VkSwapchainKHR plus NVRHI-wrapped
// image handles for rendering.
//
// Present model (single-flight, one frame in flight at a time):
//   1. vkWaitForFences / vkResetFences   - previous frame fully done
//   2. acquire()                         - synchronous (pSemaphore = NULL),
//                                          blocks until an image is free
//   3. record + NVRHI executeCommandLists
//   4. vkQueueSubmit (no work, fence)    - fence signaled when the queue work
//                                          (our render) has completed
//   5. present() per display             - ordered after the render on the
//                                          same queue
//
// NVRHI's submit always carries a VkTimelineSemaphoreSubmitInfo (its queue
// tracking semaphore), which the spec forbids mixing with binary semaphores;
// binary synchronization (acquire semaphore, present wait) is therefore
// avoided entirely: acquire is synchronous and the present is queue-ordered
// behind the render work.

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mediamax::render
{

class Swapchain
{
public:
    struct Desc
    {
        // Raw Vulkan handles of the context that owns the surface.
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        // NVRHI device used to wrap the swapchain images as renderable
        // textures.
        nvrhi::vulkan::IDevice* nvrhiDevice = nullptr;

        int width = 0;
        int height = 0;
    };

    // Creates the swapchain (FIFO present mode, 3-4 images). Returns false
    // with `error` set on failure.
    bool create(const Desc& desc, std::string& error);

    // Destroys and recreates the swapchain at a new size (window resize).
    // The queue must be idle (call the device's waitForIdle first) or the
    // destroy blocks on in-flight images.
    bool recreate(int width, int height, std::string& error);

    // Destroys the swapchain (images and wrapped textures are released).
    // The queue must be idle (call the device's waitForIdle first).
    void destroy();

    // Synchronously acquires the next back buffer. Blocks up to timeoutMs
    // until the driver has a free image.
    bool acquire(std::uint32_t& outImageIndex, std::uint32_t timeoutMs, std::string& error);

    // Presents the acquired image. The image must be in the Present state
    // (ResourceStates::Present). Ordered after all previously submitted
    // queue work.
    bool present(VkQueue queue, std::uint32_t imageIndex, std::string& error);

    // The NVRHI texture handle of the image (isRenderTarget).
    nvrhi::TextureHandle image(std::uint32_t index) const;

    VkFormat format() const { return m_Format; }
    int width() const { return m_Width; }
    int height() const { return m_Height; }
    int imageCount() const { return static_cast<int>(m_Images.size()); }
    bool valid() const { return m_Swapchain != VK_NULL_HANDLE; }

    Swapchain() = default;
    ~Swapchain();

    Swapchain(Swapchain&& other) noexcept;
    Swapchain& operator=(Swapchain&& other) noexcept;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

private:
    bool build(const Desc& desc, int width, int height, std::string& error);
    void releaseImages();

    VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> m_RawImages;
    std::vector<nvrhi::TextureHandle> m_Images;
    VkFormat m_Format = VK_FORMAT_B8G8R8A8_UNORM;
    int m_Width = 0;
    int m_Height = 0;

    // Kept for recreate().
    VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice m_Device = VK_NULL_HANDLE;
    nvrhi::vulkan::IDevice* m_NvrhiDevice = nullptr;
};

} // namespace mediamax::render
