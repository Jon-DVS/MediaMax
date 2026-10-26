#include "mediamax/render/SceneRenderer.h"

#include <algorithm>
#include <array>

#include "mediamax/util/Logger.h"
#include "shaders/display_frag.h"
#include "shaders/display_vert.h"
#include "shaders/layer_frag.h"
#include "shaders/layer_vert.h"

namespace mediamax::render
{

namespace
{

using nvrhi::AllSubresources;
using nvrhi::DrawArguments;
using nvrhi::GraphicsState;
using nvrhi::ResourceStates;
using nvrhi::TextureSubresourceSet;

nvrhi::InputLayoutHandle makeInputLayout(nvrhi::DeviceHandle device, nvrhi::ShaderHandle vs)
{
    nvrhi::VertexAttributeDesc attrs[2]{};
    attrs[0]
        .setName("POSITION")
        .setFormat(nvrhi::Format::RG32_FLOAT)
        .setBufferIndex(0)
        .setOffset(0)
        .setElementStride(16);
    attrs[1]
        .setName("TEXCOORD0")
        .setFormat(nvrhi::Format::RG32_FLOAT)
        .setBufferIndex(0)
        .setOffset(8)
        .setElementStride(16);
    return device->createInputLayout(attrs, 2, vs.Get());
}

nvrhi::BindingLayoutHandle makeLayerLayout(nvrhi::DeviceHandle device)
{
    nvrhi::BindingLayoutDesc desc;
    desc.setVisibility(nvrhi::ShaderType::AllGraphics);
    // The GLSL declares the sampler at set 0 / binding 2. NVRHI's default
    // VulkanBindingOffsets places samplers at binding 128+slot (the DXC
    // register-space convention), which would put it at binding 130 and
    // leave the shader's binding 2 unbound (undefined, zero samples).
    // Override the sampler offset to 0 so slot N maps to binding N.
    desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setSamplerOffset(0));
    desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(1));
    desc.addItem(nvrhi::BindingLayoutItem::Sampler(2));
    desc.addItem(nvrhi::BindingLayoutItem::PushConstants(3, sizeof(SceneRenderer::LayerPush)));
    return device->createBindingLayout(desc);
}

nvrhi::BindingLayoutHandle makeDisplayLayout(nvrhi::DeviceHandle device)
{
    nvrhi::BindingLayoutDesc desc;
    desc.setVisibility(nvrhi::ShaderType::AllGraphics);
    // See makeLayerLayout for the sampler binding offset rationale.
    desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setSamplerOffset(0));
    desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(1));
    desc.addItem(nvrhi::BindingLayoutItem::Sampler(2));
    desc.addItem(nvrhi::BindingLayoutItem::PushConstants(3, sizeof(SceneRenderer::DisplayPush)));
    return device->createBindingLayout(desc);
}

nvrhi::RenderState makeRenderState()
{
    nvrhi::RenderState rs;
    // No fixed-function blending (compositing is done in the layer fragment
    // shader), no culling (rotated/negatively scaled quads flip winding),
    // no depth.
    rs.rasterState.setCullMode(nvrhi::RasterCullMode::None);
    return rs;
}

// Appends the 6 vertices of a quad (two triangles) to the vertex stream.
// Each vertex is (posX, posY, uvX, uvY).
void appendQuad(std::vector<float>& verts, std::array<std::array<float, 4>, 4> corners)
{
    const std::uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    for (std::uint32_t i : indices)
    {
        const auto& c = corners[i];
        verts.insert(verts.end(), c.begin(), c.end());
    }
}

void appendLayerQuad(std::vector<float>& verts, const Transform2D& t, const Size2i& mediaSize)
{
    std::array<std::array<float, 4>, 4> corners{};
    const float uvs[4][2] = {{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}};
    for (int i = 0; i < 4; ++i)
    {
        const auto [px, py] = t.toOutputSpace(uvs[i][0], uvs[i][1], mediaSize);
        corners[i] = {px, py, uvs[i][0], uvs[i][1]};
    }
    appendQuad(verts, corners);
}

void appendWindowQuad(std::vector<float>& verts, float width, float height)
{
    // Note: assign per element with flat (expression) initializer lists.
    // C++20 aggregate-initialization (brace elision) only applies to
    // expression clauses, so a braced inner list like {{...}, {...}, ...}
    // would appertain to the array member as a whole (one element) and
    // leave the remaining clauses without a target (error C2078).
    std::array<std::array<float, 4>, 4> corners{};
    corners[0] = {0.f, 0.f, 0.f, 0.f};
    corners[1] = {width, 0.f, 1.f, 0.f};
    corners[2] = {width, height, 1.f, 1.f};
    corners[3] = {0.f, height, 0.f, 1.f};
    appendQuad(verts, corners);
}

} // namespace

bool SceneRenderer::create(const Desc& desc, SceneRenderer& out, std::string& error)
{
    if (!desc.device)
    {
        error = "null NVRHI device";
        return false;
    }
    if (!desc.sceneSize.valid())
    {
        error = "invalid scene size";
        return false;
    }
    auto& device = out.m_Device = desc.device;
    out.m_SceneSize = desc.sceneSize;

    // --- Sampler ----------------------------------------------------------------
    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    out.m_LinearSampler = device->createSampler(samplerDesc);
    if (!out.m_LinearSampler)
    {
        error = "failed to create sampler";
        return false;
    }

    // --- Shaders -------------------------------------------------------------------
    nvrhi::ShaderDesc layerVsDesc;
    layerVsDesc.setShaderType(nvrhi::ShaderType::Vertex).setDebugName("layer.vert");
    auto layerVs = device->createShader(layerVsDesc, layer_vert_spirv, layer_vert_spirv_size);

    nvrhi::ShaderDesc layerFsDesc;
    layerFsDesc.setShaderType(nvrhi::ShaderType::Pixel).setDebugName("layer.frag");
    auto layerFs = device->createShader(layerFsDesc, layer_frag_spirv, layer_frag_spirv_size);

    nvrhi::ShaderDesc displayVsDesc;
    displayVsDesc.setShaderType(nvrhi::ShaderType::Vertex).setDebugName("display.vert");
    out.m_DisplayVS = device->createShader(displayVsDesc, display_vert_spirv, display_vert_spirv_size);

    nvrhi::ShaderDesc displayFsDesc;
    displayFsDesc.setShaderType(nvrhi::ShaderType::Pixel).setDebugName("display.frag");
    out.m_DisplayFS = device->createShader(displayFsDesc, display_frag_spirv, display_frag_spirv_size);

    if (!layerVs || !layerFs || !out.m_DisplayVS || !out.m_DisplayFS)
    {
        error = "failed to create layer/display shaders";
        return false;
    }

    // --- Input layout / binding layouts -----------------------------------------------
    out.m_InputLayout = makeInputLayout(device, layerVs);
    out.m_LayerLayout = makeLayerLayout(device);
    out.m_DisplayLayout = makeDisplayLayout(device);
    if (!out.m_InputLayout || !out.m_LayerLayout || !out.m_DisplayLayout)
    {
        error = "failed to create input layout or binding layouts";
        return false;
    }

    // --- Pipelines ------------------------------------------------------------------------
    nvrhi::GraphicsPipelineDesc layerPipelineDesc;
    layerPipelineDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
        .setInputLayout(out.m_InputLayout.Get())
        .setVertexShader(layerVs.Get())
        .setFragmentShader(layerFs.Get())
        .addBindingLayout(out.m_LayerLayout.Get())
        .setRenderState(makeRenderState());
    nvrhi::FramebufferInfo sceneFbInfo;
    sceneFbInfo.addColorFormat(nvrhi::Format::RGBA8_UNORM);
    out.m_LayerPipeline = device->createGraphicsPipeline(layerPipelineDesc, sceneFbInfo);
    if (!out.m_LayerPipeline)
    {
        error = "failed to create layer pipeline";
        return false;
    }

    // --- White texture (color layers, null warp bindings) --------------------------------
    {
        nvrhi::TextureDesc td;
        td.setWidth(1).setHeight(1).setFormat(nvrhi::Format::RGBA8_UNORM)
            .setDimension(nvrhi::TextureDimension::Texture2D)
            .enableAutomaticStateTracking(ResourceStates::ShaderResource)
            .setDebugName("white");
        out.m_WhiteTexture = device->createTexture(td);
        if (!out.m_WhiteTexture)
        {
            error = "failed to create white texture";
            return false;
        }
        auto cmd = device->createCommandList();
        cmd->open();
        cmd->clearTextureFloat(out.m_WhiteTexture.Get(), AllSubresources, nvrhi::Color(1, 1, 1, 1));
        cmd->close();
        nvrhi::ICommandList* cmdPtr = cmd.Get();
        device->executeCommandLists(&cmdPtr, 1);
        device->waitForIdle();
    }

    // --- Scene targets --------------------------------------------------------------------------------
    if (!out.createSceneTargets(error))
    {
        return false;
    }
    return true;
}

bool SceneRenderer::createSceneTargets(std::string& error)
{
    for (int i = 0; i < 2; ++i)
    {
        nvrhi::TextureDesc td;
        td.setWidth(static_cast<std::uint32_t>(m_SceneSize.width))
            .setHeight(static_cast<std::uint32_t>(m_SceneSize.height))
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setDimension(nvrhi::TextureDimension::Texture2D)
            .setIsRenderTarget(true) // SRV is the TextureDesc default
            .enableAutomaticStateTracking(ResourceStates::RenderTarget)
            .setDebugName("scene rt " + std::to_string(i));
        m_SceneRT[i] = m_Device->createTexture(td);
        if (!m_SceneRT[i])
        {
            error = "failed to create scene render target";
            return false;
        }
        nvrhi::FramebufferDesc fbd;
        fbd.addColorAttachment(m_SceneRT[i].Get());
        m_SceneFB[i] = m_Device->createFramebuffer(fbd);
        if (!m_SceneFB[i])
        {
            error = "failed to create scene framebuffer";
            return false;
        }
    }
    m_FinalSceneIndex = 0;
    return true;
}

void SceneRenderer::setSceneSize(Size2i size)
{
    if (!size.valid() || size == m_SceneSize)
    {
        return;
    }
    m_SceneSize = size;
    std::string error;
    if (!createSceneTargets(error))
    {
        Logger::instance().error("setSceneSize failed: {}", error);
    }
    {
        std::lock_guard lock(m_UploadMutex);
        m_PendingUploads.clear();
    }
}

// --- Texture pool ---------------------------------------------------------------

void SceneRenderer::ensureMediaTexture(Id layerId, std::uint32_t width, std::uint32_t height)
{
    std::lock_guard lock(m_UploadMutex);
    auto it = m_MediaTextures.find(layerId);
    if (it != m_MediaTextures.end() && it->second.texture && it->second.width == width &&
        it->second.height == height)
    {
        return;
    }
    if (it != m_MediaTextures.end())
    {
        m_MediaTextures.erase(it); // size changed; the GPU keeps the old resource alive
    }
    nvrhi::TextureDesc td;
    td.setWidth(width).setHeight(height).setFormat(nvrhi::Format::RGBA8_UNORM)
        .setDimension(nvrhi::TextureDimension::Texture2D)
        .enableAutomaticStateTracking(ResourceStates::ShaderResource)
        .setDebugName("media " + std::to_string(layerId));
    m_MediaTextures[layerId] = PooledTexture{m_Device->createTexture(td), width, height};
}

void SceneRenderer::updateMediaTexture(Id layerId, const std::vector<std::uint8_t>& rgbaData)
{
    std::lock_guard lock(m_UploadMutex);
    if (!m_MediaTextures.count(layerId))
    {
        return;
    }
    m_PendingUploads.push_back(
        PendingUpload{layerId, false, std::make_shared<const std::vector<std::uint8_t>>(rgbaData)});
}

void SceneRenderer::removeMediaTexture(Id layerId)
{
    std::lock_guard lock(m_UploadMutex);
    m_MediaTextures.erase(layerId);
    m_PendingUploads.erase(
        std::remove_if(m_PendingUploads.begin(), m_PendingUploads.end(),
                       [&](const PendingUpload& u) { return !u.isWarp && u.id == layerId; }),
        m_PendingUploads.end());
}

nvrhi::TextureHandle SceneRenderer::mediaTexture(Id layerId) const
{
    std::lock_guard lock(m_UploadMutex);
    auto it = m_MediaTextures.find(layerId);
    return it == m_MediaTextures.end() ? nullptr : it->second.texture;
}

void SceneRenderer::ensureWarpTexture(Id displayId, std::uint32_t size)
{
    std::lock_guard lock(m_UploadMutex);
    auto it = m_WarpTextures.find(displayId);
    if (it != m_WarpTextures.end() && it->second.texture && it->second.width == size)
    {
        return;
    }
    if (it != m_WarpTextures.end())
    {
        m_WarpTextures.erase(it);
    }
    nvrhi::TextureDesc td;
    td.setWidth(size).setHeight(size).setFormat(nvrhi::Format::RG8_UNORM)
        .setDimension(nvrhi::TextureDimension::Texture2D)
        .enableAutomaticStateTracking(ResourceStates::ShaderResource)
        .setDebugName("warp " + std::to_string(displayId));
    m_WarpTextures[displayId] = PooledTexture{m_Device->createTexture(td), size, size};
}

void SceneRenderer::updateWarpTexture(Id displayId, const std::vector<unsigned char>& rgData)
{
    std::lock_guard lock(m_UploadMutex);
    if (!m_WarpTextures.count(displayId))
    {
        return;
    }
    m_PendingUploads.push_back(
        PendingUpload{displayId, true, std::make_shared<const std::vector<std::uint8_t>>(rgData.begin(), rgData.end())});
}

void SceneRenderer::removeWarpTexture(Id displayId)
{
    std::lock_guard lock(m_UploadMutex);
    m_WarpTextures.erase(displayId);
    m_PendingUploads.erase(
        std::remove_if(m_PendingUploads.begin(), m_PendingUploads.end(),
                       [&](const PendingUpload& u) { return u.isWarp && u.id == displayId; }),
        m_PendingUploads.end());
}

nvrhi::TextureHandle SceneRenderer::warpTexture(Id displayId) const
{
    std::lock_guard lock(m_UploadMutex);
    auto it = m_WarpTextures.find(displayId);
    return it == m_WarpTextures.end() ? nullptr : it->second.texture;
}

// --- Display pipeline --------------------------------------------------------------

bool SceneRenderer::ensureDisplayPipeline(nvrhi::Format format, std::string& error)
{
    if (format == nvrhi::Format::UNKNOWN)
    {
        error = "unknown target format";
        return false;
    }
    if (m_DisplayPipelines.count(format))
    {
        return true;
    }
    if (!m_DisplayVS || !m_DisplayFS)
    {
        error = "display shaders unavailable";
        return false;
    }

    nvrhi::GraphicsPipelineDesc desc;
    desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
        .setInputLayout(m_InputLayout.Get())
        .setVertexShader(m_DisplayVS.Get())
        .setFragmentShader(m_DisplayFS.Get())
        .addBindingLayout(m_DisplayLayout.Get())
        .setRenderState(makeRenderState());
    nvrhi::FramebufferInfo fbInfo;
    fbInfo.addColorFormat(format);
    auto pipeline = m_Device->createGraphicsPipeline(desc, fbInfo);
    if (!pipeline)
    {
        error = "failed to create display pipeline for format " +
                std::string(nvrhi::getFormatInfo(format).name);
        return false;
    }
    m_DisplayPipelines[format] = pipeline;
    return true;
}

// --- Frame recording --------------------------------------------------------------------

void SceneRenderer::recordFrame(nvrhi::ICommandList* cmd, const FrameInput& frame)
{
    if (cmd == nullptr || m_SceneRT[0] == nullptr)
    {
        return;
    }

    // --- 1. Flush queued texture uploads ------------------------------------------
    {
        std::vector<PendingUpload> pending;
        {
            std::lock_guard lock(m_UploadMutex);
            pending.swap(m_PendingUploads);
        }
        for (const auto& up : pending)
        {
            auto& pool = up.isWarp ? m_WarpTextures : m_MediaTextures;
            auto it = pool.find(up.id);
            if (it == pool.end() || !it->second.texture)
            {
                continue;
            }
            const size_t expected = up.isWarp
                ? static_cast<size_t>(it->second.width) * it->second.height * 2
                : static_cast<size_t>(it->second.width) * it->second.height * 4;
            if (up.data->size() != expected)
            {
                continue; // size mismatch (texture was re-created); drop
            }
            cmd->writeTexture(it->second.texture.Get(), 0, 0, up.data->data(), 0, 0);
        }
    }

    // --- 2. Clear both scene targets (black, alpha 1) ------------------------------------
    // NVRHI always begins render passes with loadOp=LOAD (the load op is not
    // configurable), so any pixel a layer quad does not cover keeps the
    // target's previous content. Clearing both ping-pong targets at the
    // start of the frame guarantees a fully defined scene: the initial
    // "previous" (target 0) and the first layer's destination (target 1).
    cmd->clearTextureFloat(m_SceneRT[0].Get(), AllSubresources, nvrhi::Color(0, 0, 0, 1));
    cmd->clearTextureFloat(m_SceneRT[1].Get(), AllSubresources, nvrhi::Color(0, 0, 0, 1));

    // --- 3. Vertex data: layer quads, then display quads ------------------------------------
    m_VertexData.clear();
    for (const auto& layer : frame.layers)
    {
        appendLayerQuad(m_VertexData, layer.transform, layer.mediaSize);
    }
    const std::uint32_t displayVertexStart = static_cast<std::uint32_t>(m_VertexData.size() / 4);
    for (const auto& d : frame.displays)
    {
        if (d.image != nullptr)
        {
            appendWindowQuad(m_VertexData, static_cast<float>(d.rect.width),
                             static_cast<float>(d.rect.height));
        }
    }
    if (!m_VertexData.empty())
    {
        const size_t bytes = m_VertexData.size() * sizeof(float);
        if (!m_VertexBuffer || m_VertexBuffer->getDesc().byteSize < bytes)
        {
            nvrhi::BufferDesc bd;
            bd.setByteSize(std::max(bytes, size_t(65536)))
                .setIsVertexBuffer(true)
                // The buffer is only ever used as a vertex buffer: tell NVRHI
                // the invariant so its state tracker knows the buffer's state
                // in every new command list (no "unknown prior state" errors,
                // correct automatic barriers).
                .enableAutomaticStateTracking(ResourceStates::VertexBuffer)
                .setDebugName("frame vertices");
            m_VertexBuffer = m_Device->createBuffer(bd);
        }
        cmd->writeBuffer(m_VertexBuffer.Get(), m_VertexData.data(), bytes);
    }

    // --- 4. Layer pass (ping-pong compositing) ---------------------------------------------------
    nvrhi::ITexture* prev = m_SceneRT[0].Get();
    nvrhi::ITexture* next = m_SceneRT[1].Get();
    std::uint32_t vertexStart = 0;
    const auto& sceneSize = m_SceneSize;

    for (std::size_t layerIndex = 0; layerIndex < frame.layers.size(); ++layerIndex)
    {
        const auto& layer = frame.layers[layerIndex];

        // NVRHI render passes always load the destination's existing content
        // (loadOp=LOAD is not configurable), so the pixels this layer's quad
        // does not cover keep whatever the destination held before the pass.
        // The first layer's destination was cleared to black above (the
        // correct "no layers yet" state); every later destination holds
        // stale content from an earlier frame, so carry the current
        // accumulation over before drawing. Without this, scenes whose
        // layers do not all span the full screen drop everything drawn
        // before the final layer everywhere but its own quad.
        if (layerIndex > 0)
        {
            cmd->copyTexture(next,
                             nvrhi::TextureSlice()
                                 .setWidth(static_cast<std::uint32_t>(sceneSize.width))
                                 .setHeight(static_cast<std::uint32_t>(sceneSize.height)),
                             prev,
                             nvrhi::TextureSlice());
        }

        nvrhi::ITexture* mediaTex =
            layer.texture.Get() != nullptr ? layer.texture.Get() : m_WhiteTexture.Get();

        // Per-draw binding set: NVRHI tracks its resources and places the
        // automatic barriers (e.g. scene RT: render target -> shader resource
        // between ping-pong passes).
        nvrhi::BindingSetDesc bindingDesc;
        bindingDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, mediaTex));
        bindingDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(1, prev));
        bindingDesc.addItem(nvrhi::BindingSetItem::Sampler(2, m_LinearSampler.Get()));
        auto bindings = m_Device->createBindingSet(bindingDesc, m_LayerLayout.Get());
        if (!bindings)
        {
            Logger::instance().error("failed to create layer binding set; skipping layer");
            continue;
        }

        LayerPush pc{};
        pc.sceneSizeX = static_cast<float>(sceneSize.width);
        pc.sceneSizeY = static_cast<float>(sceneSize.height);
        pc.opacity = layer.opacity;
        pc.blendMode = static_cast<int>(layer.blend);
        pc.color[0] = layer.color.r;
        pc.color[1] = layer.color.g;
        pc.color[2] = layer.color.b;
        pc.color[3] = layer.color.a;
        pc.useColor = layer.texture ? 0 : 1;

        nvrhi::GraphicsState gs;
        gs.setPipeline(m_LayerPipeline.Get())
            .setFramebuffer(next == m_SceneRT[0].Get() ? m_SceneFB[0].Get() : m_SceneFB[1].Get())
            .setViewport(nvrhi::ViewportState()
                             .addViewportAndScissorRect(nvrhi::Viewport(
                                 static_cast<float>(sceneSize.width), static_cast<float>(sceneSize.height))))
            .addBindingSet(bindings.Get())
            .addVertexBuffer(nvrhi::VertexBufferBinding()
                                 .setBuffer(m_VertexBuffer.Get())
                                 .setSlot(0)
                                 .setOffset(0));
        cmd->setGraphicsState(gs);

        // Push constants must be set after setGraphicsState: NVRHI records
        // vkCmdPushConstants immediately with the currently bound pipeline
        // layout, which is null until the first setGraphicsState of the
        // command list (vkCmdPushConstants with a null layout is a spec
        // violation and the data is silently dropped).
        cmd->setPushConstants(&pc, sizeof(pc));

        DrawArguments args;
        args.vertexCount = 6;
        args.instanceCount = 1;
        args.startVertexLocation = vertexStart;
        cmd->draw(args);

        vertexStart += 6;
        std::swap(prev, next);
    }
    m_FinalSceneIndex = static_cast<int>(frame.layers.size() % 2);
    nvrhi::ITexture* scene = m_SceneRT[m_FinalSceneIndex].Get();

    // --- 5. Display pass ----------------------------------------------------------------------------------
    std::uint32_t displayVertexCursor = displayVertexStart;
    for (const auto& d : frame.displays)
    {
        if (d.image == nullptr)
        {
            displayVertexCursor += 6;
            continue;
        }
        auto pipelineIt = m_DisplayPipelines.find(d.image->getDesc().format);
        if (pipelineIt == m_DisplayPipelines.end())
        {
            std::string error;
            if (!ensureDisplayPipeline(d.image->getDesc().format, error))
            {
                Logger::instance().error("display pass skipped: {}", error);
                displayVertexCursor += 6;
                continue;
            }
            pipelineIt = m_DisplayPipelines.find(d.image->getDesc().format);
        }

        auto fbIt = m_DisplayFBs.find(d.image);
        if (fbIt == m_DisplayFBs.end())
        {
            nvrhi::FramebufferDesc fbd;
            fbd.addColorAttachment(d.image);
            fbIt = m_DisplayFBs.emplace(d.image, m_Device->createFramebuffer(fbd)).first;
        }

        nvrhi::BindingSetDesc bindingDesc;
        bindingDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, scene));
        bindingDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(
            1, d.warp != nullptr ? d.warp : m_WhiteTexture.Get()));
        bindingDesc.addItem(nvrhi::BindingSetItem::Sampler(2, m_LinearSampler.Get()));
        auto bindings = m_Device->createBindingSet(bindingDesc, m_DisplayLayout.Get());
        if (!bindings)
        {
            Logger::instance().error("failed to create display binding set; skipping display");
            displayVertexCursor += 6;
            continue;
        }

        DisplayPush pc{};
        pc.rectX = static_cast<float>(d.rect.x);
        pc.rectY = static_cast<float>(d.rect.y);
        pc.rectW = static_cast<float>(d.rect.width);
        pc.rectH = static_cast<float>(d.rect.height);
        pc.sceneSizeX = static_cast<float>(sceneSize.width);
        pc.sceneSizeY = static_cast<float>(sceneSize.height);
        pc.brightness = d.brightness;
        pc.useWarp = d.warp ? 1 : 0;

        nvrhi::GraphicsState gs;
        gs.setPipeline(pipelineIt->second.Get())
            .setFramebuffer(fbIt->second.Get())
            .setViewport(nvrhi::ViewportState()
                             .addViewportAndScissorRect(nvrhi::Viewport(
                                 static_cast<float>(d.rect.width), static_cast<float>(d.rect.height))))
            .addBindingSet(bindings.Get())
            .addVertexBuffer(nvrhi::VertexBufferBinding()
                                 .setBuffer(m_VertexBuffer.Get())
                                 .setSlot(0)
                                 .setOffset(0));
        cmd->setGraphicsState(gs);

        // Push constants after setGraphicsState (see the layer pass above):
        // vkCmdPushConstants is recorded with the currently bound pipeline
        // layout, which is null before the first setGraphicsState.
        cmd->setPushConstants(&pc, sizeof(pc));

        DrawArguments args;
        args.vertexCount = 6;
        args.instanceCount = 1;
        args.startVertexLocation = displayVertexCursor;
        cmd->draw(args);
        displayVertexCursor += 6;

        if (d.isSwapchainImage)
        {
            // The image must be in the Present state when vkQueuePresentKHR
            // is called; transition it back before the submit.
            cmd->setTextureState(d.image, AllSubresources, ResourceStates::Present);
            cmd->commitBarriers();
        }
    }
}

// --- Diagnostics -------------------------------------------------------------------------------

bool SceneRenderer::readBackScene(std::vector<std::uint8_t>& outRgba) const
{
    const std::uint32_t w = static_cast<std::uint32_t>(m_SceneSize.width);
    const std::uint32_t h = static_cast<std::uint32_t>(m_SceneSize.height);
    if (w == 0 || h == 0 || m_SceneRT[m_FinalSceneIndex] == nullptr)
    {
        return false;
    }

    nvrhi::TextureDesc sd;
    sd.setWidth(w).setHeight(h).setFormat(nvrhi::Format::RGBA8_UNORM)
        .setDimension(nvrhi::TextureDimension::Texture2D);
    auto staging = m_Device->createStagingTexture(sd, nvrhi::CpuAccessMode::Write);
    if (!staging)
    {
        return false;
    }

    auto cmd = m_Device->createCommandList();
    cmd->open();
    cmd->copyTexture(staging.Get(),
                     nvrhi::TextureSlice().setWidth(w).setHeight(h),
                     m_SceneRT[m_FinalSceneIndex].Get(),
                     nvrhi::TextureSlice());
    cmd->close();
    nvrhi::ICommandList* cmdPtr = cmd.Get();
    m_Device->executeCommandLists(&cmdPtr, 1);
    m_Device->waitForIdle();

    size_t rowPitch = 0;
    const auto* src = static_cast<const unsigned char*>(m_Device->mapStagingTexture(
        staging.Get(), nvrhi::TextureSlice().setWidth(w).setHeight(h), nvrhi::CpuAccessMode::Read, &rowPitch));
    if (src == nullptr)
    {
        return false;
    }

    outRgba.resize(static_cast<size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
    {
        const unsigned char* row = src + static_cast<size_t>(y) * rowPitch;
        unsigned char* dst = outRgba.data() + static_cast<size_t>(y) * w * 4;
        for (std::uint32_t x = 0; x < w; ++x)
        {
            const unsigned char r = row[x * 4 + 0];
            const unsigned char g = row[x * 4 + 1];
            const unsigned char b = row[x * 4 + 2];
            const unsigned char a = row[x * 4 + 3];
            // Unpremultiply (the scene RT stores premultiplied alpha).
            const float af = a / 255.f;
            const float inv = af > 1e-6f ? 1.f / af : 0.f;
            dst[x * 4 + 0] = static_cast<unsigned char>(std::clamp(r * inv, 0.f, 255.f));
            dst[x * 4 + 1] = static_cast<unsigned char>(std::clamp(g * inv, 0.f, 255.f));
            dst[x * 4 + 2] = static_cast<unsigned char>(std::clamp(b * inv, 0.f, 255.f));
            dst[x * 4 + 3] = 255;
        }
    }
    m_Device->unmapStagingTexture(staging.Get());
    return true;
}

SceneRenderer::SceneRenderer(SceneRenderer&& other) noexcept
{
    *this = std::move(other);
}

SceneRenderer& SceneRenderer::operator=(SceneRenderer&& other) noexcept
{
    if (this != &other)
    {
        m_Device = std::move(other.m_Device);
        m_SceneSize = other.m_SceneSize;
        m_SceneRT[0] = std::move(other.m_SceneRT[0]);
        m_SceneRT[1] = std::move(other.m_SceneRT[1]);
        m_SceneFB[0] = std::move(other.m_SceneFB[0]);
        m_SceneFB[1] = std::move(other.m_SceneFB[1]);
        m_FinalSceneIndex = other.m_FinalSceneIndex;
        m_InputLayout = std::move(other.m_InputLayout);
        m_LinearSampler = std::move(other.m_LinearSampler);
        m_LayerLayout = std::move(other.m_LayerLayout);
        m_DisplayLayout = std::move(other.m_DisplayLayout);
        m_LayerPipeline = std::move(other.m_LayerPipeline);
        m_DisplayPipelines = std::move(other.m_DisplayPipelines);
        m_DisplayVS = std::move(other.m_DisplayVS);
        m_DisplayFS = std::move(other.m_DisplayFS);
        m_WhiteTexture = std::move(other.m_WhiteTexture);
        m_VertexBuffer = std::move(other.m_VertexBuffer);
        m_VertexData = std::move(other.m_VertexData);
        m_DisplayFBs = std::move(other.m_DisplayFBs);
        m_MediaTextures = std::move(other.m_MediaTextures);
        m_WarpTextures = std::move(other.m_WarpTextures);
        other.m_SceneSize = {0, 0};
        other.m_FinalSceneIndex = 0;
    }
    return *this;
}

} // namespace mediamax::render
