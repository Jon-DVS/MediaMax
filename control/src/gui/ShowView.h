#pragma once

// The 2D "show view" for the control GUI: a live, top-down picture of what
// the playback system is displaying - the display wall as labelled
// rectangles, plus the currently shown layers (solid colors, media with
// their thumbnails, video badges) with the same transformed geometry the
// engine renders.
//
// The scene model (ShowViewScene) is rebuilt every frame from the active
// system's snapshot: the display wall and layer quads come from the
// "frame.state" poll (the status poll is the fallback for the wall on older
// engines). The drawing happens on the Dear ImGui draw list of the show
// view window's scene child - the same raw Vulkan context the GUI already
// owns (the control app links the core only; a second vk-hpp dispatcher for
// NVRHI is forbidden by the repo's invariants). Layer previews are the
// engine's media.thumbnail payload (fetched by the poll thread, see
// SystemConnection) drawn through the ImGui Vulkan back end's user-texture
// path: ImTextureID is a VkDescriptorSet (ImGui_ImplVulkan_AddTexture).

// Same platform macros as GuiVulkan.h (they must be in effect for the
// window-surface declarations the two translation units share).
#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__APPLE__)
#define VK_USE_PLATFORM_MACOSX_SURFACE_EXT
#else
#define VK_USE_PLATFORM_XLIB_KHR
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>

#include "gui/SystemConnection.h"

namespace mediamax::gui
{

// One display of the wall (output-space pixels, y down).
struct ShowViewDisplay
{
    std::uint32_t id = 0;
    std::string name;
    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;
    bool enabled = true;
};

// One visible layer of the current frame, with its content footprint
// projected to output-space pixels (the engine's Transform2D already
// applied - the view draws the quad it is given).
struct ShowViewLayer
{
    std::uint32_t id = 0;
    std::string name;
    std::string content; // "color" | "image" | "video"
    float color[4] = {0.f, 0.f, 0.f, 1.f}; // color layers only
    std::uint32_t mediaId = 0; // media layers only (0 = none)
    float corners[8] = {}; // TL, TR, BR, BL (clockwise), output-space px
    // The effective transform position (output-space px): the same
    // quantity the "layer.set" x/y params take - dragging the quad by a
    // delta moves this by the delta (a pure final translation).
    float x = 0.f;
    float y = 0.f;
    float opacity = 1.f;
    int zIndex = 0;
};

// The parsed, per-frame model of what the system shows.
struct ShowViewScene
{
    float sceneW = 0.f; // the engine's output-space size (union of enabled
    float sceneH = 0.f; // displays; 0 when unknown)
    std::vector<ShowViewDisplay> displays;
    std::vector<ShowViewLayer> layers; // in draw order (zIndex ascending)

    // A signature of the display wall (ids + geometry): the camera
    // auto-refits when it changes (a display moved or the system switched).
    // Empty when the wall is unknown.
    std::string displaySignature() const;
};

// Builds the scene model from a system snapshot. Displays: frame.state
// (live geometry + enabled) with the status poll as fallback; layers:
// frame.state only (an engine without the command shows the wall only).
ShowViewScene buildShowViewScene(const SystemConnection::Snapshot& snap);

// GPU-side texture pool for the show view's layer previews + the shared
// 1x1 white texture (fallback for media before their thumbnail arrives).
// Owns a VkImage/ImageView per thumbnail registered with the Dear ImGui
// Vulkan back end (ImGui_ImplVulkan_AddTexture); the pixel upload is
// recorded into the frame's command buffer by flushUploads().
//
// Thread confinement: UI thread only (the poll thread never touches it).
class ShowTexturePool
{
public:
    ShowTexturePool() = default;
    ~ShowTexturePool() { shutdown(); }

    ShowTexturePool(const ShowTexturePool&) = delete;
    ShowTexturePool& operator=(const ShowTexturePool&) = delete;

    // Creates the shared white texture. Call once after
    // ImGui_ImplVulkan_Init (AddTexture needs the back end's state); `error`
    // explains a failure (the pool then stays not-ready and the view draws
    // untextured). `physicalDevice` is kept for the upload staging's memory
    // query.
    bool init(VkDevice device, VkPhysicalDevice physicalDevice, std::string& error);

    // Releases everything. Call before ImGui_ImplVulkan_Shutdown and the
    // device teardown. Safe to call without init() (and repeated).
    void shutdown();

    bool ready() const { return m_Ready; }
    // The shared 1x1 white texture (valid after a successful init()).
    ImTextureRef whiteTexture() const { return m_White; }

    // The texture to draw the given thumbnail with. Creates the Vulkan image
    // and registers it with the back end on first use; the pixel upload is
    // recorded by flushUploads() (same frame, before the draw). When the
    // asset's path changes (a file reload) the old texture is destroyed and
    // replaced. Returns the white texture when not ready or on failure.
    ImTextureRef textureFor(std::uint32_t mediaId, const std::string& path,
                            const std::vector<std::uint8_t>& rgba, int width, int height);

    // Destroys the textures of media ids not in `liveIds` (the engine
    // unloaded them between polls). The white texture always survives. Safe
    // to call every frame: beginFrame() has waited for the previous frame's
    // present, so no destroyed texture is in flight (see GuiVulkan).
    void evictAllExcept(const std::vector<std::uint32_t>& liveIds);

    // Records the pending pixel uploads (two barriers + a copy each) into
    // the frame's command buffer. The commands are transfer-only: call with
    // the command buffer begun but BEFORE the frame's render pass is opened
    // (GuiRuntime::renderFrame records them between beginFrame() and
    // beginRenderPass(), ahead of the UI draw that samples the textures).
    void flushUploads(VkCommandBuffer cmd);

    // --- test-only accessors (the GUI self-test's thumbnail readback) -----
    // True once the texture for `mediaId` exists and its pixels have been
    // recorded by a flushUploads() (uploaded by the next present).
    bool textureUploaded(std::uint32_t mediaId) const;
    // The texture's pixel size (false when there is no texture for the id).
    bool textureSize(std::uint32_t mediaId, int& width, int& height) const;
    // Records a copy of the texture's current GPU pixels into `dst`
    // (a caller-created host-visible, host-coherent TRANSFER_DST buffer of
    // at least width*height*4 bytes) into `cmd`: barrier to TRANSFER_SRC,
    // copy, barrier back to SHADER_READ_ONLY (the layout the UI samples).
    // Call `cmd` in the same command buffer and before its render pass,
    // after this frame's flushUploads(); map `dst` once the submit fence
    // signals (the rows are tightly packed, top-left origin). Does not
    // modify the pool (the texture keeps its layout).
    bool readbackInto(std::uint32_t mediaId, VkCommandBuffer cmd, VkBuffer dst) const;

private:
    struct Entry
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        bool hasTexture = false;
        ImTextureRef texture;
        std::string path;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> pending; // pixels waiting for the upload
        bool pendingUpload = false;
        std::uint64_t stagingOffset = 0; // assigned in flushUploads()
        // FNV-1a of the last pixels handed to textureFor: lets the pool
        // re-upload a changed thumbnail (the engine's representative frame
        // can advance for video) without recreating the texture.
        std::uint64_t pixelHash = 0;
    };

    // Creates the image + view for an entry (the texture registration is the
    // caller's job - it needs the back end).
    bool createImage(Entry& entry, int width, int height);
    // Allocates and binds device memory to entry's image.
    bool bindImageMemory(Entry& entry);
    void destroyEntry(Entry& entry);
    // Grows the staging buffer to fit `bytes` (recreates it when larger).
    bool ensureStaging(std::uint64_t bytes);

    static constexpr std::uint32_t kWhiteId = 0; // never a real media id

    VkDevice m_Device = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    bool m_Ready = false;
    ImTextureRef m_White;
    std::map<std::uint32_t, Entry> m_Textures;
    // Upload staging (grown on demand, one map per flush).
    VkBuffer m_Staging = VK_NULL_HANDLE;
    VkDeviceMemory m_StagingMemory = VK_NULL_HANDLE;
    std::uint64_t m_StagingSize = 0;
};

} // namespace mediamax::gui
