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
//     projection warp and brightness, and unpremultiplies for the window.
//     The warp is the display's dense INVERSE warp texture (RGBA8: rg = the
//     content UV that lands on the texel, b = coverage; see
//     WarpMap::buildInverseDenseTexture) - the lattice defines the region of
//     the final surface the content is warped to, and uncovered texels
//     render black. A second (forward, RG8) warp texture supplies the
//     warp-preview node markers. When the display carries a status overlay
//     texture (straight-alpha RGBA8, see core overlay/StatusOverlay), the
//     pass src-over blends it screen-aligned on top (no warp).
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
#include "mediamax/media/MediaTypes.h"
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

    // Releases every NVRHI resource this renderer owns (pipelines,
    // samplers, scene targets, texture pools, pending uploads) and returns
    // to the default-constructed state: the teardown half of a render
    // device switch, called on the render thread right before the NVRHI
    // device is destroyed (VulkanContext::resetDevice). create() then
    // re-initializes the same object on the new device (the media pump
    // references it in place, so the object must not be replaced).
    void reset();

    // --- Texture pool (thread-safe) ----------------------------------------

    // Creates (or re-creates, when the size or format changed) the texture
    // used for a media layer. Key: the layer id (a layer owns its texture
    // even when two layers share one media asset). RGBA frames are one
    // RGBA8 texture; YUV 4:2:0 frames are three textures - luma at full
    // size (R8 for 8-bit, R16_UNORM for 10-bit) and U/V at half size -
    // which the layer pass converts to RGB in the shader (see layer.frag;
    // core/src/media/VideoConversion.cpp is the CPU reference of the
    // conversion).
    void ensureMediaTexture(Id layerId, std::uint32_t width, std::uint32_t height,
                            VideoPixelFormat format = VideoPixelFormat::RGBA);

    // Queues a new frame for the texture. The pixel buffers are moved into
    // the upload queue (no copy - a 4K frame is 33 MB, copied once per
    // frame by the caller at most). Applied to the GPU inside the next
    // recordFrame(); no GPU work happens here.
    void updateMediaTexture(Id layerId, VideoFrame frame);
    // The RGBA convenience form (a single tightly packed plane).
    void updateMediaTexture(Id layerId, std::vector<std::uint8_t> rgbaData);

    // Removes the texture from the pool. Safe while the GPU is using it:
    // NVRHI keeps the resource alive until all submitted work is done.
    void removeMediaTexture(Id layerId);
    nvrhi::TextureHandle mediaTexture(Id layerId) const;

    // The full texture set of a media layer: the primary texture (RGBA, or
    // the YUV luma plane), the U/V plane textures (null for RGBA frames) and
    // the color metadata of the frame queued into the textures (set when an
    // upload is queued). Default values when the layer has no texture.
    struct MediaTextureSet
    {
        nvrhi::TextureHandle texture = nullptr;
        nvrhi::TextureHandle plane1 = nullptr; // U/Cb (YUV 4:2:0)
        nvrhi::TextureHandle plane2 = nullptr; // V/Cr (YUV 4:2:0)
        VideoPixelFormat format = VideoPixelFormat::RGBA;
        YuvColorSpace colorSpace = YuvColorSpace::BT709;
        bool fullRange = false;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    MediaTextureSet mediaTextureSet(Id layerId) const;

    // Same pool for per-display projection warp textures (RG8, sized to the
    // display; see PlaybackApp): the forward map, sampled by the display
    // pass for the warp-preview node markers (see
    // WarpMap::buildDenseTexture).
    void ensureWarpTexture(Id displayId, std::uint32_t width, std::uint32_t height);
    void updateWarpTexture(Id displayId, const std::vector<unsigned char>& rgData);
    nvrhi::TextureHandle warpTexture(Id displayId) const;

    // Same pool for the per-display dense INVERSE warp textures (RGB32_FLOAT,
    // sized to the display: R = content u, G = content v, B = coverage; see
    // WarpMap::buildInverseDenseTexture) - the display pass's content map.
    void ensureWarpInvTexture(Id displayId, std::uint32_t width, std::uint32_t height);
    void updateWarpInvTexture(Id displayId, const std::vector<unsigned char>& rgbData);
    nvrhi::TextureHandle warpInvTexture(Id displayId) const;

    // Removes both warp textures (forward + inverse) and their queued
    // uploads from the pool. Safe while the GPU is using them: NVRHI keeps
    // the resources alive until all submitted work is done.
    void removeWarpTexture(Id displayId);

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
        // YUV 4:2:0 media (mediaFormat != RGBA): the U/V plane textures
        // (created with the luma texture, half its size); null for RGBA
        // layers. The layer pass converts YUV -> RGB in the shader with the
        // push constants' mediaFormat/colorSpace/fullRange (see layer.frag).
        nvrhi::TextureHandle textureU;
        nvrhi::TextureHandle textureV;
        VideoPixelFormat mediaFormat = VideoPixelFormat::RGBA;
        YuvColorSpace mediaColorSpace = YuvColorSpace::BT709;
        bool mediaFullRange = false;
    };

    struct DisplayTarget
    {
        nvrhi::ITexture* image = nullptr; // swapchain image (or offscreen RT)
        Rect2i rect; // the display's rectangle in output space
        // Warp textures for this output; null = no warp (the app also keeps
        // them null when the display's warp is switched off or still the
        // identity). `warp` is the forward map (preview node markers);
        // `warpInv` is the inverse content map (RGB32_FLOAT: R = content u,
        // G = content v, B = coverage) the scene is sampled through.
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
        // Inverse warp texture (see `warp`); appended after warpCols on
        // purpose (the positional-aggregate note above).
        nvrhi::ITexture* warpInv = nullptr;
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
        // YUV 4:2:0 conversion (mediaFormat != RGBA): the media texture
        // size in pixels (the co-sited chroma offset in the shader) and the
        // color metadata of the frame in the textures. Declared as two
        // floats (not a vec2 in the GLSL) so the C++ natural layout (all
        // 4-byte aligned) matches the std140 offsets byte for byte.
        float mediaSizeX, mediaSizeY;
        int mediaFormat; // mediamax::VideoPixelFormat ordinal
        int colorSpace; // mediamax::YuvColorSpace ordinal
        int fullRange; // 0 = limited (video), 1 = full (JPEG)
    };
    static_assert(sizeof(LayerPush) == 56);

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
        nvrhi::TextureHandle texture; // RGBA, or the YUV luma plane
        nvrhi::TextureHandle plane1; // YUV U/Cb plane (null for single-plane)
        nvrhi::TextureHandle plane2; // YUV V/Cr plane (null for single-plane)
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        // What the texture(s) hold, and the color metadata of the frame
        // queued into them (the media pool only; the other pools keep the
        // defaults). colorSpace/fullRange update when an upload is queued.
        VideoPixelFormat format = VideoPixelFormat::RGBA;
        YuvColorSpace colorSpace = YuvColorSpace::BT709;
        bool fullRange = false;
    };
    std::map<Id, PooledTexture> m_MediaTextures;
    std::map<Id, PooledTexture> m_WarpTextures;
    std::map<Id, PooledTexture> m_WarpInvTextures;
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
            WarpInv,
            Overlay,
        };
        Id id;
        Kind kind = Kind::Media;
        std::vector<std::uint8_t> data; // primary (RGBA, or YUV luma)
        std::vector<std::uint8_t> data1; // YUV U/Cb plane
        std::vector<std::uint8_t> data2; // YUV V/Cr plane
        // The pixel layout of `data`/`data1`/`data2` (the flush validates
        // the plane sizes against the pool entry's dimensions). The color
        // metadata (matrix/range) is set on the pool at queue time, not
        // carried here.
        VideoPixelFormat format = VideoPixelFormat::RGBA;
    };
    mutable std::mutex m_UploadMutex;
    std::vector<PendingUpload> m_PendingUploads;
};

} // namespace mediamax::render
