// Render pipeline GPU tests.
//
// Each test needs a headless Vulkan context and a SceneRenderer; the setup
// helper returns early (with a message) when no usable GPU/driver is
// available, so the suite also runs on headless CI boxes. (doctest has no
// runtime skip macro; the cases report as passing with a note in that case.)

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "mediamax/blend/Blend.h"
#include "mediamax/display/WarpMap.h"
#include "mediamax/render/SceneRenderer.h"
#include "mediamax/render/VulkanContext.h"

using namespace mediamax;
using namespace mediamax::render;

namespace
{

// Shared context + renderer for all tests (created once).
struct Gpu
{
    VulkanContext context;
    SceneRenderer renderer;
    bool ok = false;
    bool initialized = false;
    std::string error;

    static Gpu& instance()
    {
        static Gpu gpu;
        return gpu;
    }
};

Gpu& gpu()
{
    auto& g = Gpu::instance();
    if (!g.initialized)
    {
        VulkanContextDesc desc;
        desc.appName = "MediaMax Render Test";
        if (VulkanContext::create(desc, g.context, g.error))
        {
            SceneRenderer::Desc rd;
            rd.device = g.context.nvrhiDevice();
            rd.sceneSize = Size2i{256, 256};
            g.ok = SceneRenderer::create(rd, g.renderer, g.error);
        }
        g.initialized = true;
    }
    return g;
}

// Test entry point: runs `fn` when a GPU is available, returns early
// otherwise (doctest has no runtime skip macro).
template <typename Fn>
void runGpuTest(const char* what, Fn&& fn)
{
    auto& g = gpu();
    if (!g.ok)
    {
        DOCTEST_MESSAGE("skipping " << what << ": no usable GPU (" << g.error << ")");
        return;
    }
    fn(g);
}

// Records one frame into a fresh command list and executes it (blocking).
void renderOneFrame(SceneRenderer& renderer, const SceneRenderer::FrameInput& frame)
{
    auto dev = gpu().context.nvrhiDevice();
    auto cmd = dev->createCommandList();
    cmd->open();
    renderer.recordFrame(cmd.Get(), frame);
    cmd->close();
    nvrhi::ICommandList* cmdPtr = cmd.Get();
    dev->executeCommandLists(&cmdPtr, 1);
    dev->waitForIdle();
}

struct Pixel
{
    int r, g, b, a;
};

// Reads an RGBA8 texture on the CPU (straight copy, no unpremultiply).
bool readBackTexture(nvrhi::DeviceHandle device, nvrhi::TextureHandle tex,
                     std::vector<std::uint8_t>& outRgba)
{
    const auto td = tex->getDesc();
    const std::uint32_t w = td.width, h = td.height;

    nvrhi::TextureDesc sd;
    sd.setWidth(w).setHeight(h).setFormat(nvrhi::Format::RGBA8_UNORM)
        .setDimension(nvrhi::TextureDimension::Texture2D);
    auto staging = device->createStagingTexture(sd, nvrhi::CpuAccessMode::Write);
    if (!staging)
    {
        return false;
    }
    auto cmd = device->createCommandList();
    cmd->open();
    cmd->copyTexture(staging.Get(), nvrhi::TextureSlice().setWidth(w).setHeight(h),
                     tex.Get(), nvrhi::TextureSlice());
    cmd->close();
    nvrhi::ICommandList* cmdPtr = cmd.Get();
    device->executeCommandLists(&cmdPtr, 1);
    device->waitForIdle();

    size_t rowPitch = 0;
    const auto* src = static_cast<const unsigned char*>(
        device->mapStagingTexture(staging.Get(), nvrhi::TextureSlice().setWidth(w).setHeight(h),
                                  nvrhi::CpuAccessMode::Read, &rowPitch));
    if (src == nullptr)
    {
        return false;
    }
    outRgba.resize(static_cast<size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
    {
        std::copy_n(src + static_cast<size_t>(y) * rowPitch, static_cast<size_t>(w) * 4,
                    outRgba.data() + static_cast<size_t>(y) * w * 4);
    }
    device->unmapStagingTexture(staging.Get());
    return true;
}

Pixel pixelAt(const std::vector<std::uint8_t>& rgba, std::uint32_t w, int x, int y)
{
    const size_t i = (static_cast<size_t>(y) * w + x) * 4;
    return Pixel{rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

void checkClose(const char* name, int actual, float expected01, int tol = 2)
{
    const int expected = static_cast<int>(std::lround(expected01 * 255.f));
    CHECK_MESSAGE(std::abs(actual - expected) <= tol,
                  name << " actual=" << actual << " expected~" << expected);
}

// A full-screen (256x256 scene) color layer.
SceneRenderer::LayerDraw colorLayer(Color4 c, int zIndex = 0)
{
    SceneRenderer::LayerDraw d;
    d.color = c;
    d.opacity = 1.f;
    d.transform = Transform2D{};
    d.transform.x = 128.f; // anchor at the scene center
    d.transform.y = 128.f;
    d.transform.scaleX = 256.f / 256.f;
    d.transform.scaleY = 256.f / 256.f;
    d.mediaSize = Size2i{256, 256};
    (void)zIndex;
    return d;
}

// A color layer whose quad is `size` px on a side, anchored at the scene
// center (128,128) unless the caller overrides the transform.
SceneRenderer::LayerDraw partialColorLayer(Color4 c, int size)
{
    SceneRenderer::LayerDraw d;
    d.color = c;
    d.opacity = 1.f;
    d.transform = Transform2D{};
    d.transform.x = 128.f;
    d.transform.y = 128.f;
    d.mediaSize = Size2i{size, size};
    return d;
}

// Offscreen "display" target: a plain render target used like a swapchain
// image (isSwapchainImage = false).
nvrhi::TextureHandle makeOffscreenTarget(nvrhi::DeviceHandle device, int w, int h)
{
    nvrhi::TextureDesc td;
    td.setWidth(w).setHeight(h).setFormat(nvrhi::Format::RGBA8_UNORM)
        .setDimension(nvrhi::TextureDimension::Texture2D)
        .setIsRenderTarget(true)
        .enableAutomaticStateTracking(nvrhi::ResourceStates::RenderTarget);
    return device->createTexture(td);
}

} // namespace

TEST_SUITE("Render")
{

TEST_CASE("Headless context and renderer creation")
{
    runGpuTest("context", [](Gpu& g) {
        CHECK(!g.context.deviceName().empty());
        CHECK(g.context.nvrhiDevice() != nullptr);
    });
}

TEST_CASE("Solid color layer renders to the scene")
{
    runGpuTest("color", [](Gpu& g) {
        SceneRenderer::FrameInput frame;
        frame.layers.push_back(colorLayer(Color4{0.25f, 0.5f, 0.75f, 1.f}));

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(g.renderer.readBackScene(rgba));
        CHECK(rgba.size() == 256u * 256u * 4u);

        const Pixel p = pixelAt(rgba, 256, 128, 128);
        checkClose("r", p.r, 0.25f);
        checkClose("g", p.g, 0.5f);
        checkClose("b", p.b, 0.75f);
        CHECK(p.a == 255);
    });
}

TEST_CASE("Empty frame is black with full alpha")
{
    runGpuTest("empty", [](Gpu& g) {
        SceneRenderer::FrameInput frame;
        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(g.renderer.readBackScene(rgba));
        const Pixel p = pixelAt(rgba, 256, 5, 5);
        CHECK(p.r <= 1);
        CHECK(p.g <= 1);
        CHECK(p.b <= 1);
        CHECK(p.a == 255);
    });
}

TEST_CASE("Layer compositing matches the CPU blend reference")
{
    runGpuTest("blend", [](Gpu& g) {
        const Color4 dst{0.2f, 0.4f, 0.6f, 1.f};   // bottom layer
        const Color4 src{0.9f, 0.1f, 0.1f, 0.5f}; // top layer, half opaque

        SceneRenderer::FrameInput frame;
        frame.layers.push_back(colorLayer(dst));
        frame.layers.push_back(colorLayer(src));

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(g.renderer.readBackScene(rgba));
        const Pixel p = pixelAt(rgba, 256, 128, 128);

        const Color4 expected = blend::apply(dst, src, BlendMode::Normal);
        checkClose("r", p.r, expected.r);
        checkClose("g", p.g, expected.g);
        checkClose("b", p.b, expected.b);
        checkClose("a", p.a, 1.f);
    });
}

TEST_CASE("Partial layer keeps the background outside its quad (2 layers)")
{
    // Regression: NVRHI render passes load the destination's previous content
    // (loadOp=LOAD), so the second layer's target must carry the first layer's
    // full-screen result before the pass; without that copy, everything the
    // first layer drew outside the second layer's quad was lost (even layer
    // counts left the final accumulation in a target that was never written
    // with the first layer).
    runGpuTest("partial-2", [](Gpu& g) {
        const Color4 bg{0.25f, 0.5f, 0.75f, 1.f};
        const Color4 fg{0.9f, 0.1f, 0.1f, 1.f};

        SceneRenderer::FrameInput frame;
        frame.layers.push_back(colorLayer(bg));
        frame.layers.push_back(partialColorLayer(fg, 128)); // covers (64..192)^2

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(g.renderer.readBackScene(rgba));

        // Inside the quad: the foreground.
        const Pixel inQuad = pixelAt(rgba, 256, 128, 128);
        checkClose("fg r", inQuad.r, fg.r);
        checkClose("fg g", inQuad.g, fg.g);
        checkClose("fg b", inQuad.b, fg.b);

        // Outside the quad: the background must survive the ping-pong.
        const Pixel outQuad = pixelAt(rgba, 256, 20, 20);
        checkClose("bg r", outQuad.r, bg.r);
        checkClose("bg g", outQuad.g, bg.g);
        checkClose("bg b", outQuad.b, bg.b);
        CHECK(outQuad.a == 255);
    });
}

TEST_CASE("Partial layers accumulate in order across the ping-pong (3 layers)")
{
    // Regression: the middle layer's result must be carried into the third
    // layer's destination (the ping-pong target written two passes earlier),
    // so the third layer blends over background+middle everywhere it covers.
    runGpuTest("partial-3", [](Gpu& g) {
        const Color4 bg{0.25f, 0.5f, 0.75f, 1.f};
        const Color4 mid{0.9f, 0.1f, 0.1f, 1.f};
        const Color4 top{0.1f, 0.9f, 0.1f, 0.5f};

        SceneRenderer::FrameInput frame;
        frame.layers.push_back(colorLayer(bg));
        frame.layers.push_back(partialColorLayer(mid, 128));          // (64..192)^2
        auto topLayer = partialColorLayer(top, 100);                  // x (118..218), y (78..178)
        topLayer.transform.x = 168.f;
        frame.layers.push_back(topLayer);

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(g.renderer.readBackScene(rgba));

        const Color4 midOverBg = blend::apply(bg, mid, BlendMode::Normal);
        const Color4 topOverMid = blend::apply(midOverBg, top, BlendMode::Normal);
        const Color4 topOverBg = blend::apply(bg, top, BlendMode::Normal);

        // Inside mid only (100 < 118).
        const Pixel pMid = pixelAt(rgba, 256, 100, 128);
        checkClose("mid r", pMid.r, midOverBg.r);
        checkClose("mid g", pMid.g, midOverBg.g);
        checkClose("mid b", pMid.b, midOverBg.b);

        // Inside mid + top (118 <= 160 < 192).
        const Pixel pBoth = pixelAt(rgba, 256, 160, 128);
        checkClose("both r", pBoth.r, topOverMid.r);
        checkClose("both g", pBoth.g, topOverMid.g);
        checkClose("both b", pBoth.b, topOverMid.b);

        // Inside top but outside mid (200 >= 192): the region that needs the
        // middle layer's contribution carried into the third layer's target.
        const Pixel pTopOnly = pixelAt(rgba, 256, 200, 128);
        checkClose("top r", pTopOnly.r, topOverBg.r);
        checkClose("top g", pTopOnly.g, topOverBg.g);
        checkClose("top b", pTopOnly.b, topOverBg.b);

        // Outside both: the background.
        const Pixel pBg = pixelAt(rgba, 256, 20, 20);
        checkClose("bg r", pBg.r, bg.r);
        checkClose("bg g", pBg.g, bg.g);
        checkClose("bg b", pBg.b, bg.b);
    });
}

TEST_CASE("All blend modes match the CPU reference")
{
    runGpuTest("blend modes", [](Gpu& g) {
        const Color4 dst{0.3f, 0.5f, 0.2f, 1.f};
        const Color4 src{0.8f, 0.7f, 0.4f, 0.6f};

        for (int mode = 0; mode < blend::kBlendModeCount; ++mode)
        {
            const auto blend = static_cast<BlendMode>(mode);
            SceneRenderer::FrameInput frame;
            frame.layers.push_back(colorLayer(dst));
            auto top = colorLayer(src);
            top.blend = blend;
            frame.layers.push_back(top);

            renderOneFrame(g.renderer, frame);

            std::vector<std::uint8_t> rgba;
            REQUIRE(g.renderer.readBackScene(rgba));
            const Pixel p = pixelAt(rgba, 256, 128, 128);

            const Color4 expected = blend::apply(dst, src, blend);
            const char* name = blend::toString(blend).c_str();
            const int expR = static_cast<int>(std::lround(expected.r * 255.f));
            const int expG = static_cast<int>(std::lround(expected.g * 255.f));
            const int expB = static_cast<int>(std::lround(expected.b * 255.f));
            CHECK_MESSAGE(std::abs(p.r - expR) <= 2,
                          name << " r=" << p.r << " expected~" << expR);
            CHECK_MESSAGE(std::abs(p.g - expG) <= 2,
                          name << " g=" << p.g << " expected~" << expG);
            CHECK_MESSAGE(std::abs(p.b - expB) <= 2,
                          name << " b=" << p.b << " expected~" << expB);
        }
    });
}

TEST_CASE("Layer opacity scales compositing")
{
    runGpuTest("opacity", [](Gpu& g) {
        const Color4 dst{0.f, 0.f, 0.f, 1.f};
        const Color4 src{1.f, 1.f, 1.f, 1.f};

        SceneRenderer::FrameInput frame;
        frame.layers.push_back(colorLayer(dst));
        auto top = colorLayer(src);
        top.opacity = 0.5f;
        frame.layers.push_back(top);

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(g.renderer.readBackScene(rgba));
        const Pixel p = pixelAt(rgba, 256, 128, 128);

        const Color4 expected = blend::apply(dst, {1.f, 1.f, 1.f, 0.5f}, BlendMode::Normal);
        checkClose("r", p.r, expected.r);
        checkClose("g", p.g, expected.g);
        checkClose("b", p.b, expected.b);
    });
}

TEST_CASE("Media texture upload and sampling round-trip")
{
    runGpuTest("media", [](Gpu& g) {
        // 64x64 gradient: rgb = (x/63, y/63, 0.5), alpha 1.
        const int w = 64, h = 64;
        std::vector<std::uint8_t> data(static_cast<size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                data[i + 0] = static_cast<std::uint8_t>(x * 255 / (w - 1));
                data[i + 1] = static_cast<std::uint8_t>(y * 255 / (h - 1));
                data[i + 2] = 128;
                data[i + 3] = 255;
            }
        }

        const Id layerId = 42;
        g.renderer.ensureMediaTexture(layerId, w, h);
        g.renderer.updateMediaTexture(layerId, data);

        SceneRenderer::FrameInput frame;
        auto d = colorLayer(Color4{});
        d.texture = g.renderer.mediaTexture(layerId);
        d.mediaSize = Size2i{w, h};
        REQUIRE(d.texture != nullptr);
        frame.layers.push_back(d);

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(g.renderer.readBackScene(rgba));
        // Sample several pixels; the texture is stretched to 256x256, so
        // pixel (128,128) maps to media (32,32).
        const Pixel p = pixelAt(rgba, 256, 128, 128);
        checkClose("r", p.r, 32.f / 63.f, 3);
        checkClose("g", p.g, 32.f / 63.f, 3);
        checkClose("b", p.b, 128.f / 255.f, 2);
    });
}

TEST_CASE("Display pass copies the scene to a target")
{
    runGpuTest("display", [](Gpu& g) {
        auto target = makeOffscreenTarget(g.context.nvrhiDevice(), 256, 256);
        REQUIRE(target != nullptr);
        std::string error;
        REQUIRE(g.renderer.ensureDisplayPipeline(nvrhi::Format::RGBA8_UNORM, error));

        SceneRenderer::FrameInput frame;
        frame.layers.push_back(colorLayer(Color4{0.2f, 0.4f, 0.8f, 1.f}));
        frame.displays.push_back(SceneRenderer::DisplayTarget{
            target.Get(), Rect2i{0, 0, 256, 256}, nullptr, 1.f, false});

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(readBackTexture(g.context.nvrhiDevice(), target, rgba));
        const Pixel p = pixelAt(rgba, 256, 10, 200);
        checkClose("r", p.r, 0.2f);
        checkClose("g", p.g, 0.4f);
        checkClose("b", p.b, 0.8f);
        CHECK(p.a == 255);
    });
}

TEST_CASE("Display brightness scales the output")
{
    runGpuTest("brightness", [](Gpu& g) {
        auto target = makeOffscreenTarget(g.context.nvrhiDevice(), 256, 256);
        std::string error;
        REQUIRE(g.renderer.ensureDisplayPipeline(nvrhi::Format::RGBA8_UNORM, error));

        SceneRenderer::FrameInput frame;
        frame.layers.push_back(colorLayer(Color4{1.f, 1.f, 1.f, 1.f}));
        frame.displays.push_back(SceneRenderer::DisplayTarget{
            target.Get(), Rect2i{0, 0, 256, 256}, nullptr, 0.5f, false});

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(readBackTexture(g.context.nvrhiDevice(), target, rgba));
        const Pixel p = pixelAt(rgba, 256, 128, 128);
        checkClose("r", p.r, 0.5f);
        checkClose("g", p.g, 0.5f);
        checkClose("b", p.b, 0.5f);
    });
}

TEST_CASE("Display pass with identity warp texture")
{
    runGpuTest("warp", [](Gpu& g) {
        auto target = makeOffscreenTarget(g.context.nvrhiDevice(), 256, 256);
        std::string error;
        REQUIRE(g.renderer.ensureDisplayPipeline(nvrhi::Format::RGBA8_UNORM, error));

        // Identity warp: the dense texture is the diagonal ramp.
        const WarpMap warp(11, 11);
        std::vector<unsigned char> warpData;
        warp.buildDenseTexture(warpData, 64);
        const Id displayId = 7;
        g.renderer.ensureWarpTexture(displayId, 64);
        g.renderer.updateWarpTexture(displayId, warpData);
        auto warpTex = g.renderer.warpTexture(displayId);
        REQUIRE(warpTex != nullptr);

        SceneRenderer::FrameInput frame;
        frame.layers.push_back(colorLayer(Color4{0.3f, 0.6f, 0.9f, 1.f}));
        frame.displays.push_back(SceneRenderer::DisplayTarget{
            target.Get(), Rect2i{0, 0, 256, 256}, warpTex.Get(), 1.f, false});

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(readBackTexture(g.context.nvrhiDevice(), target, rgba));
        // Identity warp: output equals the scene content.
        const Pixel p = pixelAt(rgba, 256, 33, 100);
        checkClose("r", p.r, 0.3f, 2);
        checkClose("g", p.g, 0.6f, 2);
        checkClose("b", p.b, 0.9f, 2);
    });
}

TEST_CASE("Layer transform positions the quad")
{
    runGpuTest("transform", [](Gpu& g) {
        // A 128x128 media quad placed with its anchor at (64,64): covers
        // (0..128, 0..128) of the scene.
        SceneRenderer::FrameInput frame;
        auto d = colorLayer(Color4{1.f, 0.f, 0.f, 1.f});
        d.transform = Transform2D{};
        d.transform.x = 64.f;
        d.transform.y = 64.f;
        d.mediaSize = Size2i{128, 128};
        d.color = Color4{1.f, 0.f, 0.f, 1.f};
        frame.layers.push_back(d);

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(g.renderer.readBackScene(rgba));
        // Inside the quad: red.
        const Pixel inQuad = pixelAt(rgba, 256, 30, 30);
        checkClose("r in", inQuad.r, 1.f, 2);
        checkClose("g in", inQuad.g, 0.f, 2);
        // Outside the quad: black (cleared scene).
        const Pixel outQuad = pixelAt(rgba, 256, 200, 200);
        CHECK(outQuad.r <= 2);
        CHECK(outQuad.g <= 2);
        CHECK(outQuad.b <= 2);
    });
}

TEST_CASE("Rotated layer keeps covering its center")
{
    runGpuTest("rotation", [](Gpu& g) {
        // 128x128 quad rotated 45 degrees around its center (128,128).
        SceneRenderer::FrameInput frame;
        auto d = colorLayer(Color4{0.f, 1.f, 0.f, 1.f});
        d.transform.rotation = 3.14159265f / 4.f;
        d.mediaSize = Size2i{128, 128};
        d.color = Color4{0.f, 1.f, 0.f, 1.f};
        frame.layers.push_back(d);

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(g.renderer.readBackScene(rgba));
        // The center of the quad is covered regardless of rotation.
        const Pixel p = pixelAt(rgba, 256, 128, 128);
        checkClose("g", p.g, 1.f, 2);
    });
}

} // TEST_SUITE("Render")
