#pragma once

// The scene renderer: composites one frame's layers into a scene render
// target and copies the scene onto each display's back buffer.
//
// Architecture:
//   - Two ping-pong scene render targets (RGBA8, premultiplied alpha).
//     Clearing A to (0,0,0,1) and compositing each visible layer in turn
//     (layer pass) produces the final scene in one of the two targets.
//   - The layer pass draws a CPU-positioned quad per layer; the fragment
//     shader samples the layer's media texture and composites it over the
//     previous target with the exact equations of core/src/blend/Blend.cpp
//     (premultiplied space, all five blend modes, selected per draw).
//   - The display pass draws one full-window quad per display: it samples
//     the final scene at the display's output-space rectangle, applies the
//     projection warp (dense RG8 warp texture) and brightness, and
//     unpremultiplies for the window. When the display carries a status
//     overlay texture (straight-alpha RGBA8, see core overlay/StatusOverlay),
//     the pass src-over blends it screen-aligned on top (no warp).
//
// The renderer is a pure recorder: it only records commands into a command
// list owned by the host (and flushes queued texture uploads into it). The
// host owns the frame cadence, the swapchains and the queue synchronization
// (see Swapchain.h).
//
// Thread safety: recordFrame() must be called from one thread (the render
// thread). The texture pool methods (ensure/update/remove + mediaTexture /
// warpTexture lookups) may be called from any thread (the media pump calls
// update* from its own thread); queued uploads are applied inside the next
// recordFrame() on the render thread.

#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "mediamax/blend/Blend.h"
#include "mediamax/math/Rect.h"
#include "mediamax/math/Transform2D.h"
#include "mediamax/util/Id.h"

namespace mediamax::render
{

class SceneRenderer
{
public:
    struct Desc
    {
        nvrhi::DeviceHandle device;
        // The output space (union of the display rectangles) in pixels.
        Size2i sceneSize;
    };

    // Creates the render targets, pipelines, samplers and the texture pool.
    static bool create(const Desc& desc, SceneRenderer& out, std::string& error);

    // Re-allocates the scene render targets (display layout changed).
    // Pending uploads are dropped.
    void setSceneSize(Size2i size);

    // --- Texture pool (thread-safe) ----------------------------------------

    // Creates (or re-creates, when the size changed) the texture used for a
    // media layer. Key: the layer id (a layer owns its texture even when two
    // layers share one media asset).
    void ensureMediaTexture(Id layerId, std::uint32_t width, std::uint32_t height);

    // Queues a new frame for the texture (tightly packed RGBA8). The pixel
    // buffer is moved into the upload queue (no copy - a 4K frame is 33 MB,
    // copied once per frame by the caller at most). Applied to the GPU inside
    // the next recordFrame(); no GPU work happens here.
    void updateMediaTexture(Id layerId, std::vector<std::uint8_t> rgbaData);

    // Removes the texture from the pool. Safe while the GPU is using it:
    // NVRHI keeps the resource alive until all submitted work is done.
    void removeMediaTexture(Id layerId);
    nvrhi::TextureHandle mediaTexture(Id layerId) const;

    // Same pool for per-display projection warp textures (RG8, square).
    void ensureWarpTexture(Id displayId, std::uint32_t size);
    void updateWarpTexture(Id displayId, const std::vector<unsigned char>& rgData);
    void removeWarpTexture(Id displayId);
    nvrhi::TextureHandle warpTexture(Id displayId) const;

    // Same pool for per-display status overlay textures (RGBA8, straight
    // alpha; the text is rasterized by core/src/overlay/StatusOverlay.cpp).
    void ensureOverlayTexture(Id displayId, std::uint32_t width, std::uint32_t height);
    void updateOverlayTexture(Id displayId, const std::vector<std::uint8_t>& rgbaData);
    void removeOverlayTexture(Id displayId);
    nvrhi::TextureHandle overlayTexture(Id displayId) const;

    // --- Display pass --------------------------------------------------------

    // (Re)creates the display pipeline for the given target format (the
    // swapchain format, or RGBA8 for offscreen targets). Called before the
    // first frame that uses the format.
    bool ensureDisplayPipeline(nvrhi::Format format, std::string& error);

    // Drops the cached per-image display framebuffers. Call after swapchain
    // images are recreated (the old image pointers are gone; keeping their
    // framebuffers would pin the destroyed images and risk key collisions).
    void invalidateDisplayFramebufferCache()
    {
        m_DisplayFBs.clear();
    }

    // --- Frame recording -------------------------------------------------------

    struct LayerDraw
    {
        nvrhi::TextureHandle texture; // null for color layers
        Color4 color{}; // for color layers
        Transform2D transform; // resolved (tracks applied)
        float opacity = 1.f;
        BlendMode blend = BlendMode::Normal;
        Size2i mediaSize{0, 0}; // media display size in px (Transform2D input)
    };

    struct DisplayTarget
    {
        nvrhi::ITexture* image = nullptr; // swapchain image (or offscreen RT)
        Rect2i rect; // the display's rectangle in output space
        // Warp texture for this output; null = identity (the app also keeps
        // it null when the display's warp is switched off).
        nvrhi::ITexture* warp = nullptr;
        // Status overlay texture (straight-alpha RGBA8); null = no overlay.
        // Drawn screen-aligned on top of the unwarped scene output.
        nvrhi::ITexture* overlay = nullptr;
        float brightness = 1.f;
        // True for swapchain images: transitions the image back to the
        // Present state at the end of the pass (required for the present).
        // False for offscreen render targets (e.g. self-test).
        bool isSwapchainImage = true;
        // Warp-preview overlay (warp editor open on the control side): draw
        // the ideal node lattice and the node markers (at their current,
        // warped positions) over the output. Only takes effect with
        // warpRows/warpCols >= 2. (New fields after isSwapchainImage on
        // purpose: DisplayTarget is positional-aggregate-initialized in the
        // render tests.)
        bool warpPreview = false;
        int warpRows = 0;
        int warpCols = 0;
    };

    struct FrameInput
    {
        std::vector<LayerDraw> layers; // already sorted in draw order
        std::vector<DisplayTarget> displays;
    };

    // Records one full frame: queued texture uploads, scene clear, the layer
    // passes and one display pass per display.
    //
    // Swapchain images in `displays` must have their state tracking started
    // by the caller (beginTrackingTextureState(..., ResourceStates::Present))
    // before this call.
    void recordFrame(nvrhi::ICommandList* cmd, const FrameInput& frame);

    // The scene size in pixels.
    Size2i sceneSize() const { return m_SceneSize; }

    // Push constant blocks (must mirror the GLSL std140 layout exactly).
    // Public because the binding layout builders in the .cpp reference their
    // size; the layout is fixed by the shaders.
    struct LayerPush
    {
        float sceneSizeX, sceneSizeY;
        float opacity;
        int blendMode;
        float color[4];
        int useColor;
    };
    static_assert(sizeof(LayerPush) == 36);

    struct DisplayPush
    {
        float rectX, rectY, rectW, rectH;
        float sceneSizeX, sceneSizeY;
        float brightness;
        int useWarp;
        int useOverlay;
        int useWarpPreview;
        int warpRows;
        int warpCols;
    };
    static_assert(sizeof(DisplayPush) == 48);

    // --- Diagnostics / self-test ---------------------------------------------

    // Reads the most recent composited scene into an RGBA8 (unpremultiplied)
    // buffer, waiting for the GPU to finish. For offscreen self-tests.
    bool readBackScene(std::vector<std::uint8_t>& outRgba) const;

    SceneRenderer() = default;
    ~SceneRenderer() = default;

    SceneRenderer(SceneRenderer&& other) noexcept;
    SceneRenderer& operator=(SceneRenderer&& other) noexcept;
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

private:
    // Creates the two scene RTs + framebuffers for m_SceneSize.
    bool createSceneTargets(std::string& error);

    nvrhi::DeviceHandle m_Device;
    Size2i m_SceneSize{0, 0};

    // Scene ping-pong targets.
    nvrhi::TextureHandle m_SceneRT[2];
    nvrhi::FramebufferHandle m_SceneFB[2];
    int m_FinalSceneIndex = 0; // which RT holds the last recorded frame

    // Shared state.
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::SamplerHandle m_LinearSampler;
    nvrhi::BindingLayoutHandle m_LayerLayout;
    nvrhi::BindingLayoutHandle m_DisplayLayout;
    nvrhi::GraphicsPipelineHandle m_LayerPipeline;
    std::map<nvrhi::Format, nvrhi::GraphicsPipelineHandle> m_DisplayPipelines;
    nvrhi::ShaderHandle m_DisplayVS;
    nvrhi::ShaderHandle m_DisplayFS;
    nvrhi::TextureHandle m_WhiteTexture; // 1x1 white (color layers, null warp)
    nvrhi::TextureHandle m_TransparentTexture; // 1x1 transparent (null overlay)
    nvrhi::BufferHandle m_VertexBuffer;
    std::vector<float> m_VertexData; // current frame's vertices (vec2 pos, vec2 uv)

    // Per-display framebuffer cache (swapchain images / offscreen targets).
    std::unordered_map<const nvrhi::ITexture*, nvrhi::FramebufferHandle> m_DisplayFBs;

    // Texture pool.
    struct PooledTexture
    {
        nvrhi::TextureHandle texture;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    std::map<Id, PooledTexture> m_MediaTextures;
    std::map<Id, PooledTexture> m_WarpTextures;
    std::map<Id, PooledTexture> m_OverlayTextures;

    // Queued uploads (applied inside recordFrame on the render thread).
    // The pixel data is moved into the entry: each queued upload is one
    // texture's worth of pixels and at most one upload is queued per
    // texture per frame, so the shared-aliasing indirection was needless
    // (and cost a small heap allocation per upload).
    struct PendingUpload
    {
        enum class Kind : std::uint8_t
        {
            Media,
            Warp,
            Overlay,
        };
        Id id;
        Kind kind = Kind::Media;
        std::vector<std::uint8_t> data;
    };
    mutable std::mutex m_UploadMutex;
    std::vector<PendingUpload> m_PendingUploads;
};

} // namespace mediamax::render
