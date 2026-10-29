#include "gui/ShowView.h"

#include <algorithm>
#include <cstring>

#include "backends/imgui_impl_vulkan.h"

namespace mediamax::gui
{

namespace
{

// FNV-1a over the thumbnail pixels: cheap enough to run per frame per
// media layer, and it tells textureFor() whether the pixels it was handed
// changed since the last upload (a video's representative frame advances).
std::uint64_t pixelHash(const std::vector<std::uint8_t>& rgba)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::uint8_t b : rgba)
    {
        hash ^= b;
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

// --- scene model ---------------------------------------------------------------

std::string ShowViewScene::displaySignature() const
{
    std::string s;
    for (const auto& d : displays)
    {
        s += std::to_string(d.id) + ":" + std::to_string(static_cast<int>(d.x)) + "," +
             std::to_string(static_cast<int>(d.y)) + "," + std::to_string(static_cast<int>(d.w)) +
             "," + std::to_string(static_cast<int>(d.h)) + ";";
    }
    return s;
}

ShowViewScene buildShowViewScene(const SystemConnection::Snapshot& snap)
{
    using json = nlohmann::json;
    ShowViewScene scene;

    const bool haveFs = snap.haveFrameState && snap.frameState.is_object();
    const bool haveSt = snap.haveStatus && snap.status.is_object();
    const json& fs = haveFs ? snap.frameState : json::object();
    const json& st = haveSt ? snap.status : json::object();

    // The display wall: frame.state (live geometry + enabled state) with the
    // status poll as fallback for engines that predate the command.
    const json* src = nullptr;
    if (haveFs && fs.contains("displays") && fs["displays"].is_array())
    {
        src = &fs["displays"];
    }
    else if (haveSt && st.contains("displays") && st["displays"].is_array())
    {
        src = &st["displays"];
    }
    if (src != nullptr)
    {
        scene.displays.reserve(src->size());
        for (const auto& d : *src)
        {
            ShowViewDisplay disp;
            disp.id = static_cast<std::uint32_t>(d.value("id", 0u));
            disp.name = d.value("name", std::string("display"));
            disp.x = static_cast<float>(d.value("x", 0.0));
            disp.y = static_cast<float>(d.value("y", 0.0));
            disp.w = static_cast<float>(d.value("width", 0.0));
            disp.h = static_cast<float>(d.value("height", 0.0));
            disp.enabled = d.value("enabled", true);
            scene.displays.push_back(std::move(disp));
        }
    }

    if (haveFs && fs.contains("sceneSize") && fs["sceneSize"].is_object())
    {
        scene.sceneW = static_cast<float>(fs["sceneSize"].value("width", 0.0));
        scene.sceneH = static_cast<float>(fs["sceneSize"].value("height", 0.0));
    }

    // The layers exist only in frame.state: an engine without the command
    // shows the wall only (the view degrades, it does not fail).
    if (haveFs && fs.contains("layers") && fs["layers"].is_array())
    {
        scene.layers.reserve(fs["layers"].size());
        for (const auto& l : fs["layers"])
        {
            ShowViewLayer layer;
            layer.id = static_cast<std::uint32_t>(l.value("id", 0u));
            layer.name = l.value("name", std::string());
            layer.content = l.value("content", std::string("color"));
            if (l.contains("color") && l["color"].is_array() && l["color"].size() == 4)
            {
                for (int c = 0; c < 4; ++c)
                {
                    layer.color[c] = static_cast<float>(l["color"][static_cast<size_t>(c)].get<double>());
                }
            }
            layer.mediaId = static_cast<std::uint32_t>(l.value("mediaId", 0u));
            layer.x = static_cast<float>(l.value("x", 0.0));
            layer.y = static_cast<float>(l.value("y", 0.0));
            if (l.contains("corners") && l["corners"].is_array() && l["corners"].size() == 4)
            {
                for (int i = 0; i < 4; ++i)
                {
                    const auto& c = l["corners"][static_cast<size_t>(i)];
                    if (c.is_array() && c.size() == 2)
                    {
                        layer.corners[static_cast<size_t>(2 * i)] =
                            static_cast<float>(c[0].get<double>());
                        layer.corners[static_cast<size_t>(2 * i + 1)] =
                            static_cast<float>(c[1].get<double>());
                    }
                }
            }
            layer.opacity = static_cast<float>(l.value("opacity", 1.0));
            layer.zIndex = l.value("zIndex", 0);
            scene.layers.push_back(std::move(layer));
        }
    }
    return scene;
}

// --- texture pool ----------------------------------------------------------------

bool ShowTexturePool::init(VkDevice device, VkPhysicalDevice physicalDevice, std::string& error)
{
    if (m_Ready)
    {
        return true;
    }
    m_Device = device;
    m_PhysicalDevice = physicalDevice;
    if (m_Device == VK_NULL_HANDLE || m_PhysicalDevice == VK_NULL_HANDLE)
    {
        error = "no Vulkan device";
        return false;
    }

    // The shared 1x1 white texture (kept as a normal pool entry, id 0 -
    // never a real media id; its upload rides the first flushUploads()).
    Entry white;
    white.path = "white";
    white.width = 1;
    white.height = 1;
    white.pending = {255, 255, 255, 255};
    white.pendingUpload = true;
    if (!createImage(white, 1, 1))
    {
        error = "vkCreateImage (show view white texture) failed";
        return false;
    }
    const VkDescriptorSet set =
        ImGui_ImplVulkan_AddTexture(white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (set == VK_NULL_HANDLE)
    {
        destroyEntry(white);
        error = "ImGui_ImplVulkan_AddTexture (white texture) failed";
        return false;
    }
    white.hasTexture = true;
    white.texture =
        ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(set)));
    m_White = white.texture;
    m_Textures[kWhiteId] = std::move(white);
    m_Ready = true;
    return true;
}

// Binds device memory to an image: picks the first memory type the driver
// allows (host visibility is not needed - the GPU only reads it) and
// allocates + binds exactly the required size.
bool ShowTexturePool::bindImageMemory(Entry& entry)
{
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_Device, entry.image, &memReqs);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &props);
    std::uint32_t typeIndex = 0;
    bool found = false;
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i)
    {
        if ((memReqs.memoryTypeBits & (1u << i)) != 0u)
        {
            typeIndex = i;
            found = true;
            break;
        }
    }
    if (!found)
    {
        return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = memReqs.size;
    alloc.memoryTypeIndex = typeIndex;
    if (vkAllocateMemory(m_Device, &alloc, nullptr, &entry.memory) != VK_SUCCESS ||
        vkBindImageMemory(m_Device, entry.image, entry.memory, 0) != VK_SUCCESS)
    {
        if (entry.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_Device, entry.memory, nullptr);
            entry.memory = VK_NULL_HANDLE;
        }
        return false;
    }
    return true;
}

void ShowTexturePool::shutdown()
{
    for (auto& [id, entry] : m_Textures)
    {
        destroyEntry(entry);
    }
    m_Textures.clear();
    m_White = ImTextureRef();
    if (m_Staging != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_Device, m_Staging, nullptr);
        m_Staging = VK_NULL_HANDLE;
    }
    if (m_StagingMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_Device, m_StagingMemory, nullptr);
        m_StagingMemory = VK_NULL_HANDLE;
        m_StagingSize = 0;
    }
    m_Ready = false;
    m_Device = VK_NULL_HANDLE;
    m_PhysicalDevice = VK_NULL_HANDLE;
}

bool ShowTexturePool::createImage(Entry& entry, int width, int height)
{
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_Device, &info, nullptr, &entry.image) != VK_SUCCESS)
    {
        return false;
    }
    if (!bindImageMemory(entry))
    {
        destroyEntry(entry);
        return false;
    }
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = entry.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    VkComponentMapping swizzle{};
    swizzle.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    swizzle.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    swizzle.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    swizzle.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    view.components = swizzle;
    // Initialize the fields individually: the SDK 1.4.363 headers add a
    // fifth member (baseArrayLayer) between levelCount and layerCount, so a
    // 4-value brace-init would silently zero the layer count (GPU fault).
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;
    view.subresourceRange = range;
    if (vkCreateImageView(m_Device, &view, nullptr, &entry.view) != VK_SUCCESS)
    {
        destroyEntry(entry);
        return false;
    }
    return true;
}

void ShowTexturePool::destroyEntry(Entry& entry)
{
    if (entry.hasTexture)
    {
        ImGui_ImplVulkan_RemoveTexture(
            reinterpret_cast<VkDescriptorSet>(static_cast<std::uintptr_t>(entry.texture._TexID)));
        entry.hasTexture = false;
        entry.texture = ImTextureRef();
    }
    if (entry.view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_Device, entry.view, nullptr);
        entry.view = VK_NULL_HANDLE;
    }
    if (entry.image != VK_NULL_HANDLE)
    {
        vkDestroyImage(m_Device, entry.image, nullptr);
        entry.image = VK_NULL_HANDLE;
    }
    if (entry.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_Device, entry.memory, nullptr);
        entry.memory = VK_NULL_HANDLE;
    }
}

bool ShowTexturePool::ensureStaging(std::uint64_t bytes)
{
    if (bytes <= m_StagingSize)
    {
        return m_Staging != VK_NULL_HANDLE;
    }
    if (m_Staging != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_Device, m_Staging, nullptr);
    }
    if (m_StagingMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_Device, m_StagingMemory, nullptr);
        m_StagingMemory = VK_NULL_HANDLE;
        m_StagingSize = 0;
    }
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_Device, &info, nullptr, &m_Staging) != VK_SUCCESS)
    {
        return false;
    }
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_Device, m_Staging, &memReqs);
    VkMemoryPropertyFlags wanted =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &props);
    std::uint32_t typeIndex = 0;
    bool found = false;
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i)
    {
        if ((memReqs.memoryTypeBits & (1u << i)) != 0u &&
            (props.memoryTypes[i].propertyFlags & wanted) == wanted)
        {
            typeIndex = i;
            found = true;
            break;
        }
    }
    if (!found)
    {
        // No host-visible memory: drop the buffer, uploads stay pending
        // (the view draws the white fallback).
        vkDestroyBuffer(m_Device, m_Staging, nullptr);
        m_Staging = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = memReqs.size;
    alloc.memoryTypeIndex = typeIndex;
    if (vkAllocateMemory(m_Device, &alloc, nullptr, &m_StagingMemory) != VK_SUCCESS ||
        vkBindBufferMemory(m_Device, m_Staging, m_StagingMemory, 0) != VK_SUCCESS)
    {
        if (m_StagingMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_Device, m_StagingMemory, nullptr);
        }
        vkDestroyBuffer(m_Device, m_Staging, nullptr);
        m_Staging = VK_NULL_HANDLE;
        m_StagingMemory = VK_NULL_HANDLE;
        return false;
    }
    m_StagingSize = bytes;
    return true;
}

ImTextureRef ShowTexturePool::textureFor(std::uint32_t mediaId, const std::string& path,
                                         const std::vector<std::uint8_t>& rgba, int width,
                                         int height)
{
    if (!m_Ready || width <= 0 || height <= 0 ||
        rgba.size() != static_cast<std::size_t>(width) * height * 4)
    {
        return m_White;
    }
    const std::uint64_t hash = pixelHash(rgba);
    auto it = m_Textures.find(mediaId);
    if (it != m_Textures.end() && it->second.path == path && it->second.width == width &&
        it->second.height == height && it->second.pixelHash == hash)
    {
        // Unchanged asset: the texture is on the GPU already (a pending
        // upload is about to happen in this frame's flush).
        return it->second.texture;
    }
    if (it != m_Textures.end() && it->second.path == path && it->second.width == width &&
        it->second.height == height)
    {
        // Same asset, new pixels (the engine's thumbnail advanced, e.g. a
        // video's representative frame): re-upload into the existing image
        // rather than recreating the texture.
        it->second.pending = rgba;
        it->second.pixelHash = hash;
        it->second.pendingUpload = true;
        return it->second.texture;
    }
    if (it != m_Textures.end())
    {
        // The asset's path or size changed (a file reload): replace the
        // texture.
        destroyEntry(it->second);
        m_Textures.erase(it);
    }
    Entry entry;
    entry.path = path;
    entry.width = width;
    entry.height = height;
    entry.pending = rgba;
    entry.pixelHash = hash;
    entry.pendingUpload = true;
    if (!createImage(entry, width, height))
    {
        return m_White;
    }
    const VkDescriptorSet set =
        ImGui_ImplVulkan_AddTexture(entry.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (set == VK_NULL_HANDLE)
    {
        destroyEntry(entry);
        return m_White;
    }
    entry.hasTexture = true;
    entry.texture =
        ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(set)));
    m_Textures[mediaId] = std::move(entry);
    return m_Textures[mediaId].texture;
}

void ShowTexturePool::evictAllExcept(const std::vector<std::uint32_t>& liveIds)
{
    if (!m_Ready)
    {
        return;
    }
    for (auto it = m_Textures.begin(); it != m_Textures.end();)
    {
        const bool live =
            it->first == kWhiteId ||
            std::find(liveIds.begin(), liveIds.end(), it->first) != liveIds.end();
        if (live)
        {
            ++it;
        }
        else
        {
            destroyEntry(it->second);
            it = m_Textures.erase(it);
        }
    }
}

void ShowTexturePool::flushUploads(VkCommandBuffer cmd)
{
    if (!m_Ready)
    {
        return;
    }
    // Assign staging offsets to the pending uploads (one contiguous blob).
    std::vector<Entry*> pending;
    std::uint64_t needed = 0;
    for (auto& [id, entry] : m_Textures)
    {
        if (entry.pendingUpload)
        {
            entry.stagingOffset = needed;
            needed += entry.pending.size();
            pending.push_back(&entry);
        }
    }
    if (pending.empty())
    {
        return;
    }
    if (!ensureStaging(needed))
    {
        // No staging: leave the uploads pending (retried next frame) and
        // draw the white fallback in the meantime.
        return;
    }

    // One map / memcpy / unmap for all pending uploads.
    void* mapped = nullptr;
    if (vkMapMemory(m_Device, m_StagingMemory, 0, needed, 0, &mapped) != VK_SUCCESS)
    {
        return;
    }
    auto* base = static_cast<std::uint8_t*>(mapped);
    for (Entry* entry : pending)
    {
        std::memcpy(base + entry->stagingOffset, entry->pending.data(), entry->pending.size());
    }
    vkUnmapMemory(m_Device, m_StagingMemory);

    for (Entry* entry : pending)
    {
        // Two transitions: UNDEFINED -> TRANSFER_DST_OPTIMAL for the copy,
        // then -> SHADER_READ_ONLY_OPTIMAL (the layout
        // ImGui_ImplVulkan_AddTexture registered the texture in). Each
        // barrier's stage/access masks must match its dst (TRANSFER_WRITE
        // is not a legal dst access for the fragment stage).
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;

        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = entry->image;
        toTransfer.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toTransfer);

        VkBufferImageCopy region{};
        region.bufferOffset = entry->stagingOffset;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {static_cast<std::uint32_t>(entry->width),
                              static_cast<std::uint32_t>(entry->height), 1};
        vkCmdCopyBufferToImage(cmd, m_Staging, entry->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier toSampled{};
        toSampled.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSampled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toSampled.dstAccessMask = 0;
        toSampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toSampled.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSampled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSampled.image = entry->image;
        toSampled.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toSampled);
        entry->pendingUpload = false;
        entry->pending.clear();
    }
}

// --- test-only accessors ---------------------------------------------------------

bool ShowTexturePool::textureUploaded(std::uint32_t mediaId) const
{
    const auto it = m_Textures.find(mediaId);
    return it != m_Textures.end() && it->second.hasTexture && !it->second.pendingUpload;
}

bool ShowTexturePool::textureSize(std::uint32_t mediaId, int& width, int& height) const
{
    const auto it = m_Textures.find(mediaId);
    if (it == m_Textures.end() || !it->second.hasTexture)
    {
        return false;
    }
    width = it->second.width;
    height = it->second.height;
    return true;
}

bool ShowTexturePool::readbackInto(std::uint32_t mediaId, VkCommandBuffer cmd, VkBuffer dst) const
{
    const auto it = m_Textures.find(mediaId);
    if (it == m_Textures.end() || !it->second.hasTexture)
    {
        return false;
    }
    const Entry& e = it->second;
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    // The texture is sampled in SHADER_READ_ONLY_OPTIMAL: barrier to
    // TRANSFER_SRC for the copy, then back (the UI samples it again next
    // frame).
    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = e.image;
    toSrc.subresourceRange = range;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {static_cast<std::uint32_t>(e.width), static_cast<std::uint32_t>(e.height),
                          1};
    vkCmdCopyImageToBuffer(cmd, e.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);

    VkImageMemoryBarrier back{};
    back.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    back.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back.image = e.image;
    back.subresourceRange = range;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &back);
    return true;
}

} // namespace mediamax::gui
