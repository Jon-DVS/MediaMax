// Render pipeline GPU tests.
//
// Each test needs a headless Vulkan context and a SceneRenderer; the setup
// helper returns early (with a message) when no usable GPU/driver is
// available, so the suite also runs on headless CI boxes. (doctest has no
// runtime skip macro; the cases report as passing with a note in that case.)

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <system_error>
#include <vector>

// Included at file scope (not inside a namespace) so the Win32 process API
// stays in the global namespace; only the media tests use it.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "mediamax/blend/Blend.h"
#include "mediamax/display/WarpMap.h"
#include "mediamax/media/MediaPlaceholder.h"
#include "mediamax/render/SceneRenderer.h"
#include "mediamax/render/VulkanContext.h"

#ifdef MEDIAMAX_HAS_FFMPEG
#include "mediamax/media/FFmpegSupport.h"
#include "mediamax/media/VideoConversion.h"
#endif

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
    dev->runGarbageCollection();
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
    device->runGarbageCollection();

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
    // Explicit center anchor (the struct default is the media's top-left).
    d.transform.anchorX = 0.5f;
    d.transform.anchorY = 0.5f;
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
    // Explicit center anchor (the struct default is the media's top-left).
    d.transform.anchorX = 0.5f;
    d.transform.anchorY = 0.5f;
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

#ifdef MEDIAMAX_HAS_FFMPEG

// --- Media generation (issue 20: final video + image rendering) ------------

// Locates the ffmpeg CLI that ships with the fetched FFmpeg build (used to
// generate a real video file). Returns an empty path when it is not present.
std::filesystem::path findFfmpegCli()
{
    const std::filesystem::path dir(MEDIAMAX_FFMPEG_TOOLS_DIR);
    for (const char* name : {"ffmpeg.exe", "ffmpeg"})
    {
        const std::filesystem::path candidate = dir / name;
        if (std::filesystem::exists(candidate))
        {
            return candidate;
        }
    }
    return {};
}

// Runs a command line synchronously and returns the process exit code (-1 if
// it could not be started). On Windows this bypasses cmd.exe entirely:
// std::system routes through the shell, whose quote handling mangles the
// quoted executable/output paths.
int runCommandLine(const std::string& commandLine)
{
#if defined(_WIN32)
    // The test paths are ASCII; a byte-for-byte widening is correct here.
    std::wstring wide(commandLine.begin(), commandLine.end());
    STARTUPINFOW si{};
    si.cb = static_cast<DWORD>(sizeof(si));
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          nullptr, &si, &pi))
    {
        return -1;
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
#else
    return std::system(commandLine.c_str());
#endif
}

// Writes a minimal 24-bit BMP (rows bottom-up, padded to 4 bytes).
bool writeTestBmp(const std::filesystem::path& path, int width, int height,
                  const std::function<void(int x, int y, std::uint8_t& r, std::uint8_t& g,
                                           std::uint8_t& b)>& pixel)
{
    const int rowBytes = width * 3;
    const int paddedRow = (rowBytes + 3) & ~3;
    const int pixelDataSize = paddedRow * height;

    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
        return false;
    }

    // 14-byte file header.
    unsigned char header[14] = {'B', 'M'};
    const std::uint32_t fileSize = 14 + 40 + pixelDataSize;
    std::memcpy(header + 2, &fileSize, 4);
    std::uint32_t offset = 54;
    std::memcpy(header + 10, &offset, 4);

    // 40-byte DIB header.
    unsigned char dib[40] = {};
    std::uint32_t dibSize = 40;
    std::memcpy(dib, &dibSize, 4);
    std::int32_t w = width, h = height;
    std::memcpy(dib + 4, &w, 4);
    std::memcpy(dib + 8, &h, 4);
    std::uint16_t planes = 1, bpp = 24;
    std::memcpy(dib + 12, &planes, 2);
    std::memcpy(dib + 14, &bpp, 2);
    std::uint32_t imgSize = pixelDataSize;
    std::memcpy(dib + 20, &imgSize, 4);

    out.write(reinterpret_cast<const char*>(header), 14);
    out.write(reinterpret_cast<const char*>(dib), 40);

    std::vector<unsigned char> row(paddedRow, 0);
    for (int y = height - 1; y >= 0; --y) // bottom-up
    {
        for (int x = 0; x < width; ++x)
        {
            unsigned char r = 0, g = 0, b = 0;
            pixel(x, y, r, g, b);
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        out.write(reinterpret_cast<const char*>(row.data()), paddedRow);
    }
    return static_cast<bool>(out);
}

#endif // MEDIAMAX_HAS_FFMPEG

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

TEST_CASE("Media placeholder: rasterized frame uploads and composites like media")
{
    runGpuTest("media placeholder", [](Gpu& g) {
        // The placeholder the playback app uploads for a layer whose media
        // is not loaded yet (core media/MediaPlaceholder): rasterize at the
        // scene size, upload it into the layer's media texture, composite,
        // and verify the scene holds the rasterized pixels. The frame is
        // opaque over the (0,0,0,1) clear, so the readback (which
        // unpremultiplies) equals the raster.
        const int w = 256, h = 256;
        std::vector<std::uint8_t> expected;
        media::renderMediaPlaceholder(w, h, "waiting-for-transfer.mp4", expected);
        REQUIRE(!expected.empty());

        const Id layerId = 43;
        g.renderer.ensureMediaTexture(layerId, w, h);
        g.renderer.updateMediaTexture(layerId, std::vector<std::uint8_t>(expected));

        SceneRenderer::FrameInput frame;
        auto d = colorLayer(Color4{});
        d.texture = g.renderer.mediaTexture(layerId);
        d.mediaSize = Size2i{w, h};
        REQUIRE(d.texture != nullptr);
        frame.layers.push_back(d);

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(g.renderer.readBackScene(rgba));
        REQUIRE(rgba.size() == expected.size());
        // 1:1 sampling at pixel centers is exact; allow one step of rounding
        // for driver differences. Sample a spread across the fill and the
        // text regions.
        auto closePixel = [&](int x, int y) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            CHECK_MESSAGE(std::abs(static_cast<int>(rgba[i]) - static_cast<int>(expected[i])) <= 1,
                          "r @ " << x << "," << y);
            CHECK_MESSAGE(
                std::abs(static_cast<int>(rgba[i + 1]) - static_cast<int>(expected[i + 1])) <= 1,
                "g @ " << x << "," << y);
            CHECK_MESSAGE(
                std::abs(static_cast<int>(rgba[i + 2]) - static_cast<int>(expected[i + 2])) <= 1,
                "b @ " << x << "," << y);
            CHECK(rgba[i + 3] == 255);
            CHECK(expected[i + 3] == 255);
        };
        closePixel(10, 10); // fill
        closePixel(245, 10); // fill
        closePixel(10, 245); // fill
        closePixel(245, 245); // fill
        closePixel(128, 128); // the text block
        closePixel(128, 100); // name line
        closePixel(128, 156); // hint line
        closePixel(64, 128); // left of the text block
    });
}

TEST_CASE("Layer pass: YUV 4:2:0 media is converted on the GPU")
{
    runGpuTest("yuv layer", [](Gpu& g) {
        // Synthetic YUV 4:2:0 frames are uploaded as three plane textures
        // and converted YUV -> RGB by the layer shader (the float32 form of
        // core/src/media/VideoConversion.cpp). The expectation is a
        // double-precision reference that samples the planes at exactly the
        // coordinates the shader does - luma bilinear at v_uv, U/V bilinear
        // at the co-sited chroma coordinate (v_uv + 0.5 / mediaSize) - and
        // applies the ITU matrix, so the two must agree within float32 +
        // 8-bit rounding.
        const int tw = 256, th = 256; // target (scene) size

        // The normalized sample values (what the texture sampler returns).
        auto normalized = [](const VideoFrame& f, int plane) {
            const bool is10Bit = f.format == VideoPixelFormat::YUV420P10LE;
            const auto& p = f.planes[plane];
            std::vector<double> out(p.size() / (is10Bit ? 2 : 1));
            for (std::size_t i = 0; i < out.size(); ++i)
            {
                out[i] = is10Bit ? double(p[2 * i] | (p[2 * i + 1] << 8)) / 65535.0
                                 : double(p[i]) / 255.0;
            }
            return out;
        };

        // Bilinear at texel coordinates (texel i centered at i), clamped at
        // the edges like the sampler.
        auto bilinear = [](const std::vector<double>& plane, int w, int h, double u, double v) {
            auto axis = [&](int row, double t) {
                if (t < 0.0)
                {
                    return plane[static_cast<std::size_t>(row) * w];
                }
                if (t > w - 1.0)
                {
                    return plane[static_cast<std::size_t>(row) * w + (w - 1)];
                }
                const int i0 = static_cast<int>(t);
                const double f = t - i0;
                const int i1 = std::min(i0 + 1, w - 1);
                const std::size_t base = static_cast<std::size_t>(row) * w;
                return plane[base + i0] + (plane[base + i1] - plane[base + i0]) * f;
            };
            if (v < 0.0)
            {
                return axis(0, u);
            }
            if (v > h - 1.0)
            {
                return axis(h - 1, u);
            }
            const int r0 = static_cast<int>(v);
            const double f = v - r0;
            const int r1 = std::min(r0 + 1, h - 1);
            return axis(r0, u) + (axis(r1, u) - axis(r0, u)) * f;
        };

        // The reference matrix (double, independent of the production code).
        auto referenceRgb = [](double y, double cb, double cr, int space, bool fullRange) {
            double yn, cbn, crn;
            if (fullRange)
            {
                yn = y;
                cbn = cb - 0.5;
                crn = cr - 0.5;
            }
            else
            {
                yn = (y - 16.0 / 255.0) * (255.0 / 219.0);
                cbn = (cb - 128.0 / 255.0) * (255.0 / 224.0);
                crn = (cr - 128.0 / 255.0) * (255.0 / 224.0);
            }
            double a, b, c, d;
            if (space == 0) // BT601
            {
                a = 1.402;
                b = 0.34414;
                c = 0.71414;
                d = 1.772;
            }
            else if (space == 2) // BT2020
            {
                a = 1.8814;
                b = 0.1197;
                c = 0.6812;
                d = 2.1145;
            }
            else // BT709 (1) / Unknown (3)
            {
                a = 1.5748;
                b = 0.1873;
                c = 0.4681;
                d = 1.8556;
            }
            const auto cl = [](double v) { return std::max(0.0, std::min(1.0, v)); };
            return std::array<double, 3>{cl(yn + a * crn), cl(yn - b * cbn - c * crn),
                                         cl(yn + d * cbn)};
        };

        // Renders the layer's texture full-bleed into the 256x256 scene
        // (the same wiring as PlaybackApp::buildLayerDraws) and reads it
        // back.
        auto renderFullBleed = [&](Id layerId, int w, int h) {
            SceneRenderer::FrameInput frame;
            auto d = colorLayer(Color4{});
            d.texture = g.renderer.mediaTexture(layerId);
            REQUIRE(d.texture != nullptr);
            const auto set = g.renderer.mediaTextureSet(layerId);
            d.textureU = set.plane1;
            d.textureV = set.plane2;
            d.mediaFormat = set.format;
            d.mediaColorSpace = set.colorSpace;
            d.mediaFullRange = set.fullRange;
            d.mediaSize = Size2i{w, h};
            d.transform = Transform2D{};
            d.transform.anchorX = 0.5f;
            d.transform.anchorY = 0.5f;
            d.transform.x = 128.f;
            d.transform.y = 128.f;
            d.transform.scaleX = 256.f / static_cast<float>(w);
            d.transform.scaleY = 256.f / static_cast<float>(h);
            frame.layers.push_back(d);
            renderOneFrame(g.renderer, frame);
            std::vector<std::uint8_t> rgba;
            REQUIRE(g.renderer.readBackScene(rgba));
            return rgba;
        };

        // Compares every target pixel against the double reference.
        auto checkCase = [&](Id layerId, const VideoFrame& f, int space, int tol) {
            const int w = static_cast<int>(f.width), h = static_cast<int>(f.height);
            const auto yv = normalized(f, 0);
            const auto uPlane = normalized(f, 1);
            const auto vPlane = normalized(f, 2);
            const int chw = w / 2, chh = h / 2;
            const auto rgba = renderFullBleed(layerId, w, h);
            int maxDelta = 0, wx = -1, wy = -1;
            for (int Y = 0; Y < th; ++Y)
            {
                for (int X = 0; X < tw; ++X)
                {
                    // The media UV of the fragment center (the quad is
                    // full-bleed and the UV is linear over it).
                    const double uvx = (X + 0.5) / 256.0;
                    const double uvy = (Y + 0.5) / 256.0;
                    // The shader samples luma at v_uv and the chroma planes
                    // at v_uv + 0.5 / mediaSize (the co-sited 4:2:0
                    // mapping); in texel coordinates (texel i at i).
                    const auto rgb = referenceRgb(
                        bilinear(yv, w, h, uvx * w - 0.5, uvy * h - 0.5),
                        bilinear(uPlane, chw, chh, (uvx + 0.5 / w) * chw - 0.5,
                                 (uvy + 0.5 / h) * chh - 0.5),
                        bilinear(vPlane, chw, chh, (uvx + 0.5 / w) * chw - 0.5,
                                 (uvy + 0.5 / h) * chh - 0.5),
                        space, f.fullRange);
                    const std::size_t i = (static_cast<std::size_t>(Y) * tw + X) * 4;
                    for (int ch = 0; ch < 3; ++ch)
                    {
                        const int delta = std::abs(
                            static_cast<int>(rgba[i + ch]) -
                            static_cast<int>(std::lround(rgb[ch] * 255.0)));
                        if (delta > maxDelta)
                        {
                            maxDelta = delta;
                            wx = X;
                            wy = Y;
                        }
                    }
                }
            }
            CHECK_MESSAGE(maxDelta <= tol,
                          "YUV max delta " << maxDelta << " (tolerance " << tol
                          << ") at (" << wx << ", " << wy << ")");
        };

        // --- Case 1: 8-bit, limited range, BT.709 (64x36) -------------------
        // Luma ramps across x (71..180 -> Y' in [0.25, 0.75]), U ramps
        // across rows at half resolution (113..143), V is neutral: every
        // channel stays inside [0, 1] (no clamping), so the GPU result must
        // be pure linear math.
        {
            const int w = 64, h = 36;
            VideoFrame f;
            f.width = w;
            f.height = h;
            f.format = VideoPixelFormat::YUV420P;
            f.colorSpace = YuvColorSpace::BT709;
            f.fullRange = false;
            f.planes.resize(3);
            f.planes[0].resize(static_cast<std::size_t>(w) * h);
            f.planes[1].resize(static_cast<std::size_t>(w / 2) * (h / 2));
            f.planes[2].assign(static_cast<std::size_t>(w / 2) * (h / 2), 128);
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    f.planes[0][static_cast<std::size_t>(y) * w + x] =
                        static_cast<std::uint8_t>(std::lround(71.0 + 109.0 * x / (w - 1)));
                }
                for (int cx = 0; cx < w / 2; ++cx)
                {
                    f.planes[1][static_cast<std::size_t>(y / 2) * (w / 2) + cx] =
                        static_cast<std::uint8_t>(
                            std::lround(113.0 + 30.0 * (y / 2) / (h / 2 - 1)));
                }
            }
            const Id layerId = 300;
            g.renderer.ensureMediaTexture(layerId, w, h, VideoPixelFormat::YUV420P);
            g.renderer.updateMediaTexture(layerId, f); // copied (f is reused below)
            checkCase(layerId, f, static_cast<int>(YuvColorSpace::BT709), 2);
        }

        // --- Case 2: 10-bit, limited range, BT.709 (32x16, R16 planes) ------
        // Luma ramps across x (256..768 of 1023), U/V neutral 512. The
        // 10-bit samples use the scaled 16-bit UNORM storage (the same
        // mapping copyVideoPlane applies at decode time).
        {
            const int w = 32, h = 16;
            VideoFrame f;
            f.width = w;
            f.height = h;
            f.format = VideoPixelFormat::YUV420P10LE;
            f.colorSpace = YuvColorSpace::BT709;
            f.fullRange = false;
            f.planes.resize(3);
            f.planes[0].resize(static_cast<std::size_t>(w) * h * 2);
            f.planes[1].resize(static_cast<std::size_t>(w / 2) * (h / 2) * 2);
            f.planes[2].resize(static_cast<std::size_t>(w / 2) * (h / 2) * 2);
            auto put10 = [&f](int plane, std::size_t i, std::uint32_t v10) {
                const std::uint32_t v16 = v10 * 65535u / 1023u;
                f.planes[plane][2 * i] = static_cast<std::uint8_t>(v16 & 0xFFu);
                f.planes[plane][2 * i + 1] = static_cast<std::uint8_t>((v16 >> 8) & 0xFFu);
            };
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    put10(0, static_cast<std::size_t>(y) * w + x,
                          static_cast<std::uint32_t>(std::lround(256.0 + 512.0 * x / (w - 1))));
                }
                for (int cx = 0; cx < w / 2; ++cx)
                {
                    put10(1, static_cast<std::size_t>(y / 2) * (w / 2) + cx, 512);
                    put10(2, static_cast<std::size_t>(y / 2) * (w / 2) + cx, 512);
                }
            }
            const Id layerId = 301;
            g.renderer.ensureMediaTexture(layerId, w, h, VideoPixelFormat::YUV420P10LE);
            g.renderer.updateMediaTexture(layerId, f);
            checkCase(layerId, f, static_cast<int>(YuvColorSpace::BT709), 2);
        }

        // --- Case 3: 8-bit, FULL range, BT.601 (16x16, constant) ------------
        // Wires the matrix/range push constants differently; R clamps at
        // 1.0 (both implementations clamp identically on a constant frame).
        {
            const int w = 16, h = 16;
            VideoFrame f;
            f.width = w;
            f.height = h;
            f.format = VideoPixelFormat::YUV420P;
            f.colorSpace = YuvColorSpace::BT601;
            f.fullRange = true;
            f.planes.resize(3);
            f.planes[0].assign(static_cast<std::size_t>(w) * h, 200);
            f.planes[1].assign(static_cast<std::size_t>(w / 2) * (h / 2), 100);
            f.planes[2].assign(static_cast<std::size_t>(w / 2) * (h / 2), 200);
            const Id layerId = 302;
            g.renderer.ensureMediaTexture(layerId, w, h, VideoPixelFormat::YUV420P);
            g.renderer.updateMediaTexture(layerId, f);
            checkCase(layerId, f, static_cast<int>(YuvColorSpace::BT601), 2);
        }
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
            target.Get(), Rect2i{0, 0, 256, 256}, nullptr, nullptr, 1.f, false});

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
            target.Get(), Rect2i{0, 0, 256, 256}, nullptr, nullptr, 0.5f, false});

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

        // Identity warp: the dense textures are the diagonal ramp (forward)
        // and the identity-with-coverage (inverse).
        const WarpMap warp(11, 11);
        std::vector<unsigned char> warpData, invData;
        warp.buildDenseTexture(warpData, 64, 64);
        warp.buildInverseDenseTexture(invData, 64, 64);
        const Id displayId = 7;
        g.renderer.ensureWarpTexture(displayId, 64, 64);
        g.renderer.updateWarpTexture(displayId, warpData);
        g.renderer.ensureWarpInvTexture(displayId, 64, 64);
        g.renderer.updateWarpInvTexture(displayId, invData);
        auto warpTex = g.renderer.warpTexture(displayId);
        auto warpInvTex = g.renderer.warpInvTexture(displayId);
        REQUIRE(warpTex != nullptr);
        REQUIRE(warpInvTex != nullptr);

        SceneRenderer::FrameInput frame;
        frame.layers.push_back(colorLayer(Color4{0.3f, 0.6f, 0.9f, 1.f}));
        SceneRenderer::DisplayTarget targetDesc;
        targetDesc.image = target.Get();
        targetDesc.rect = Rect2i{0, 0, 256, 256};
        targetDesc.warp = warpTex.Get();
        targetDesc.warpInv = warpInvTex.Get();
        targetDesc.isSwapchainImage = false;
        frame.displays.push_back(targetDesc);

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

TEST_CASE("Display pass warps the content into the lattice region (inverse map)")
{
    runGpuTest("warp content", [](Gpu& g) {
        auto target = makeOffscreenTarget(g.context.nvrhiDevice(), 256, 256);
        std::string error;
        REQUIRE(g.renderer.ensureDisplayPipeline(nvrhi::Format::RGBA8_UNORM, error));

        // A 2x2 lattice with the top-left node pulled inward to (0.2, 0.2):
        // F(u, v) = (u, v) + 0.2(1 - u)(1 - v). The content shrinks toward
        // the bottom-right; the top-left corner of the surface is outside
        // the lattice region and must render black.
        WarpMap warp(2, 2);
        warp.setPointPosition(0, 0, 0.2f, 0.2f);
        std::vector<unsigned char> warpData, invData;
        warp.buildDenseTexture(warpData, 64, 64);
        warp.buildInverseDenseTexture(invData, 64, 64);
        const Id displayId = 71;
        g.renderer.ensureWarpTexture(displayId, 64, 64);
        g.renderer.updateWarpTexture(displayId, warpData);
        g.renderer.ensureWarpInvTexture(displayId, 64, 64);
        g.renderer.updateWarpInvTexture(displayId, invData);
        auto warpTex = g.renderer.warpTexture(displayId);
        auto warpInvTex = g.renderer.warpInvTexture(displayId);
        REQUIRE(warpTex != nullptr);
        REQUIRE(warpInvTex != nullptr);

        // Scene: left half red, right half green (content u < 0.5 = red).
        const auto halfLayer = [](Color4 c, bool left) {
            SceneRenderer::LayerDraw d;
            d.color = c;
            d.opacity = 1.f;
            d.transform = Transform2D{};
            d.transform.anchorX = 0.5f;
            d.transform.anchorY = 0.5f;
            d.transform.x = left ? 64.f : 192.f; // center of each half
            d.transform.y = 128.f;
            d.transform.scaleX = 0.5f; // 128 px wide (256 px media)
            d.transform.scaleY = 1.f;
            d.mediaSize = Size2i{256, 256};
            return d;
        };
        SceneRenderer::FrameInput frame;
        frame.layers.push_back(halfLayer(Color4{1.f, 0.f, 0.f, 1.f}, true));
        frame.layers.push_back(halfLayer(Color4{0.f, 1.f, 0.f, 1.f}, false));
        SceneRenderer::DisplayTarget targetDesc;
        targetDesc.image = target.Get();
        targetDesc.rect = Rect2i{0, 0, 256, 256};
        targetDesc.warp = warpTex.Get();
        targetDesc.warpInv = warpInvTex.Get();
        targetDesc.isSwapchainImage = false;
        frame.displays.push_back(targetDesc);

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(readBackTexture(g.context.nvrhiDevice(), target, rgba));

        // The pocket: deep in the uncovered top-left corner, black.
        const Pixel pocket = pixelAt(rgba, 256, 8, 8);
        checkClose("pocket r", pocket.r, 0.f, 2);
        checkClose("pocket g", pocket.g, 0.f, 2);
        checkClose("pocket b", pocket.b, 0.f, 2);
        // Red content at q = (0.25, 0.5) lands at p = F(q) = (0.325, 0.575)
        // => pixel (83, 147).
        const Pixel red = pixelAt(rgba, 256, 83, 147);
        checkClose("red r", red.r, 1.f, 2);
        checkClose("red g", red.g, 0.f, 2);
        // Green content at q = (0.75, 0.5) lands at p = F(q) = (0.775, 0.525)
        // => pixel (198, 134).
        const Pixel green = pixelAt(rgba, 256, 198, 134);
        checkClose("green g", green.g, 1.f, 2);
        checkClose("green r", green.r, 0.f, 2);
    });
}

TEST_CASE("Display pass draws the warp preview overlay (grid + nodes)")
{
    runGpuTest("warp preview", [](Gpu& g) {
        auto target = makeOffscreenTarget(g.context.nvrhiDevice(), 256, 256);
        std::string error;
        REQUIRE(g.renderer.ensureDisplayPipeline(nvrhi::Format::RGBA8_UNORM, error));

        SceneRenderer::FrameInput frame;
        frame.layers.push_back(colorLayer(Color4{0.3f, 0.6f, 0.9f, 1.f}));
        SceneRenderer::DisplayTarget targetDesc;
        targetDesc.image = target.Get();
        targetDesc.rect = Rect2i{0, 0, 256, 256};
        targetDesc.isSwapchainImage = false;
        targetDesc.warpPreview = true;
        targetDesc.warpRows = 3;
        targetDesc.warpCols = 3;
        frame.displays.push_back(targetDesc);

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(readBackTexture(g.context.nvrhiDevice(), target, rgba));
        // Scene reference (far from any grid line or node marker).
        const Pixel scene = pixelAt(rgba, 256, 96, 64);
        checkClose("scene r", scene.r, 0.3f, 2);
        checkClose("scene g", scene.g, 0.6f, 2);
        checkClose("scene b", scene.b, 0.9f, 2);
        // The grid: a white reference line at u = 0.5 (a 3x3 lattice on a
        // 256 px display => the middle vertical line runs through x = 128).
        // It is anti-aliased to ~half coverage here, so it must be clearly
        // brighter than the scene, not exactly white.
        const Pixel line = pixelAt(rgba, 256, 128, 64);
        CHECK_MESSAGE(line.r > scene.r + 15, "grid line r actual=" << line.r);
        CHECK_MESSAGE(line.g > scene.g + 15, "grid line g actual=" << line.g);
        // The nodes: a marker at each node's current position - the center
        // node of the 3x3 lattice sits at (128, 128). The marker is
        // yellowish (1.0, 0.85, 0.25) at full coverage in its center: much
        // redder and bluer-down than the scene under it.
        const Pixel node = pixelAt(rgba, 256, 128, 128);
        CHECK_MESSAGE(node.r > 250, "node marker r actual=" << node.r);
        CHECK_MESSAGE(node.g > 200, "node marker g actual=" << node.g);
        CHECK_MESSAGE(node.b < 100, "node marker b actual=" << node.b);
    });
}

TEST_CASE("Display pass blends the status overlay on top")
{
    runGpuTest("overlay", [](Gpu& g) {
        auto target = makeOffscreenTarget(g.context.nvrhiDevice(), 256, 256);
        std::string error;
        REQUIRE(g.renderer.ensureDisplayPipeline(nvrhi::Format::RGBA8_UNORM, error));

        // 64x32 overlay: opaque red block in the top-left quarter (uv
        // 0..0.5 x 0..0.5 => screen pixels 0..128), transparent elsewhere.
        const std::uint32_t ow = 64, oh = 32;
        std::vector<std::uint8_t> ovData(ow * oh * 4, 0);
        for (std::uint32_t y = 0; y < oh / 2; ++y)
        {
            for (std::uint32_t x = 0; x < ow / 2; ++x)
            {
                std::uint8_t* px = ovData.data() + (static_cast<size_t>(y) * ow + x) * 4;
                px[0] = 255;
                px[3] = 255;
            }
        }
        const Id displayId = 42;
        g.renderer.ensureOverlayTexture(displayId, ow, oh);
        g.renderer.updateOverlayTexture(displayId, ovData);
        auto overlayTex = g.renderer.overlayTexture(displayId);
        REQUIRE(overlayTex != nullptr);

        SceneRenderer::FrameInput frame;
        frame.layers.push_back(colorLayer(Color4{0.2f, 0.4f, 0.8f, 1.f}));
        frame.displays.push_back(SceneRenderer::DisplayTarget{
            target.Get(), Rect2i{0, 0, 256, 256}, nullptr, overlayTex.Get(), 1.f, false});

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(readBackTexture(g.context.nvrhiDevice(), target, rgba));
        // Under the opaque overlay block: the overlay color, not the scene.
        const Pixel under = pixelAt(rgba, 256, 32, 32);
        checkClose("r under", under.r, 1.f, 2);
        checkClose("g under", under.g, 0.f, 2);
        checkClose("b under", under.b, 0.f, 2);
        // Outside the inked block (right of the overlay's 0.5 uv edge):
        // the scene shows through the transparent overlay.
        const Pixel through = pixelAt(rgba, 256, 200, 50);
        checkClose("r through", through.r, 0.2f);
        checkClose("g through", through.g, 0.4f);
        checkClose("b through", through.b, 0.8f);
        CHECK(through.a == 255);
    });
}

TEST_CASE("Layer transform positions the quad")
{
    runGpuTest("transform", [](Gpu& g) {
        // A 128x128 media quad placed with its (explicit center) anchor at
        // (64,64): covers (0..128, 0..128) of the scene.
        SceneRenderer::FrameInput frame;
        auto d = colorLayer(Color4{1.f, 0.f, 0.f, 1.f});
        d.transform = Transform2D{};
        d.transform.anchorX = 0.5f;
        d.transform.anchorY = 0.5f;
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

TEST_CASE("Final render: decoded video and image media reach the display")
{
    // Issue 20: the end-to-end media path on the GPU. A real video file
    // (generated with the ffmpeg CLI) and a real image file (hand-written
    // BMP) are decoded through the same FFmpeg source factory the playback
    // app installs, uploaded as media textures exactly like the media pump
    // (straight RGBA8), composited over a background color layer, and
    // verified in the display target - the final output.
#ifdef MEDIAMAX_HAS_FFMPEG
    runGpuTest("video+image media", [](Gpu& g) {
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "mediamax_render_media";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const auto videoPath = dir / "final_render_video.mp4";
        const auto imagePath = dir / "final_render_image.bmp";

        // --- Video: one second of solid 0x204060, 64x64, h264/yuv420p.
        const auto ffmpegCli = findFfmpegCli();
        if (ffmpegCli.empty())
        {
            std::filesystem::remove(videoPath, ec);
            DOCTEST_MESSAGE("skipping: ffmpeg CLI not available (cannot generate the video)");
            return;
        }
        const std::string cmd = "\"" + ffmpegCli.string() + "\" -hide_banner -loglevel error -y "
            "-f lavfi -i color=c=0x204060:s=64x64:d=1:r=10 "
            "-c:v libx264 -preset ultrafast -pix_fmt yuv420p " +
            "\"" + videoPath.string() + "\"";
        if (runCommandLine(cmd) != 0)
        {
            std::filesystem::remove(videoPath, ec);
            DOCTEST_MESSAGE("skipping: ffmpeg failed to generate the video");
            return;
        }

        // --- Image: a 64x64 BMP, left half orange, right half yellow.
        REQUIRE(writeTestBmp(imagePath, 64, 64, [](int x, int, std::uint8_t& r, std::uint8_t& g,
                                                   std::uint8_t& b) {
            if (x < 32)
            {
                r = 192;
                g = 48;
                b = 0;
            }
            else
            {
                r = 192;
                g = 192;
                b = 48;
            }
        }));

        std::string error;

        // --- Decode (the same source factory the playback engine installs).
        // The libx264 file decodes through the YUV plane fast path (even
        // 4:2:0), so convert it to RGBA with the CPU reference - exactly
        // what the media pump does before the GPU conversion path lands.
        auto videoSource = ffmpeg::createSource(videoPath.string(), error);
        REQUIRE(videoSource != nullptr);
        VideoFrame videoFrame;
        REQUIRE(videoSource->seek(Time::zero()));
        REQUIRE(videoSource->nextVideoFrame(videoFrame, 2000));
        CHECK(videoFrame.width == 64);
        CHECK(videoFrame.height == 64);
        CHECK(videoFrame.format == VideoPixelFormat::YUV420P);
        std::vector<std::uint8_t> videoRgba;
        REQUIRE(convertYuvFrameToRgba(videoFrame, videoRgba));
        videoSource.reset(); // release the file handle (Windows keeps it open)

        auto imageSource = ffmpeg::createSource(imagePath.string(), error);
        REQUIRE(imageSource != nullptr);
        VideoFrame imageFrame;
        REQUIRE(imageSource->nextVideoFrame(imageFrame, 2000));
        CHECK(imageFrame.width == 64);
        CHECK(imageFrame.height == 64);
        CHECK(imageFrame.format == VideoPixelFormat::RGBA);
        imageSource.reset();
        std::filesystem::remove(videoPath, ec);
        std::filesystem::remove(imagePath, ec);

        // --- Upload as media textures (straight RGBA8, like the media pump).
        const Id videoLayerId = 100;
        const Id imageLayerId = 101;
        g.renderer.ensureMediaTexture(videoLayerId, 64, 64);
        g.renderer.updateMediaTexture(videoLayerId, std::move(videoRgba));
        g.renderer.ensureMediaTexture(imageLayerId, 64, 64);
        g.renderer.updateMediaTexture(imageLayerId, std::move(imageFrame.planes[0]));

        // --- Compose: background color, the video quad (left half of the
        //     scene), the image quad (right half); read back the display
        //     target (the final output).
        const Color4 bg{128.f / 255.f, 128.f / 255.f, 128.f / 255.f, 1.f};
        auto target = makeOffscreenTarget(g.context.nvrhiDevice(), 256, 256);
        REQUIRE(target != nullptr);
        REQUIRE(g.renderer.ensureDisplayPipeline(nvrhi::Format::RGBA8_UNORM, error));

        SceneRenderer::FrameInput frame;
        frame.layers.push_back(colorLayer(bg));
        auto videoDraw = colorLayer(Color4{});
        videoDraw.texture = g.renderer.mediaTexture(videoLayerId);
        videoDraw.mediaSize = Size2i{128, 128};
        videoDraw.transform.x = 64.f; // covers (0..128, 64..192)
        videoDraw.transform.y = 128.f;
        REQUIRE(videoDraw.texture != nullptr);
        frame.layers.push_back(videoDraw);
        auto imageDraw = colorLayer(Color4{});
        imageDraw.texture = g.renderer.mediaTexture(imageLayerId);
        imageDraw.mediaSize = Size2i{128, 128};
        imageDraw.transform.x = 192.f; // covers (128..256, 64..192)
        imageDraw.transform.y = 128.f;
        REQUIRE(imageDraw.texture != nullptr);
        frame.layers.push_back(imageDraw);
        frame.displays.push_back(SceneRenderer::DisplayTarget{
            target.Get(), Rect2i{0, 0, 256, 256}, nullptr, nullptr, 1.f, false});

        renderOneFrame(g.renderer, frame);

        std::vector<std::uint8_t> rgba;
        REQUIRE(readBackTexture(g.context.nvrhiDevice(), target, rgba));

        // Video region: the decoded clip's color (h264/yuv420p may shift a
        // solid color by a couple of levels).
        const Pixel video = pixelAt(rgba, 256, 64, 128);
        checkClose("video r", video.r, 32.f / 255.f, 3);
        checkClose("video g", video.g, 64.f / 255.f, 3);
        checkClose("video b", video.b, 96.f / 255.f, 3);
        CHECK(video.a == 255);

        // Image region: both halves of the split BMP (proves the decoded
        // content, not a flat fill, reached the sampler).
        const Pixel imgLeft = pixelAt(rgba, 256, 160, 128);
        checkClose("img L r", imgLeft.r, 192.f / 255.f, 2);
        checkClose("img L g", imgLeft.g, 48.f / 255.f, 2);
        checkClose("img L b", imgLeft.b, 0.f / 255.f, 2);
        const Pixel imgRight = pixelAt(rgba, 256, 224, 128);
        checkClose("img R r", imgRight.r, 192.f / 255.f, 2);
        checkClose("img R g", imgRight.g, 192.f / 255.f, 2);
        checkClose("img R b", imgRight.b, 48.f / 255.f, 2);
        CHECK(imgRight.a == 255);

        // Background above the quads (y=20 < 64): the color layer survives
        // the final composite.
        const Pixel bgPixel = pixelAt(rgba, 256, 128, 20);
        checkClose("bg r", bgPixel.r, 128.f / 255.f, 2);
        checkClose("bg g", bgPixel.g, 128.f / 255.f, 2);
        checkClose("bg b", bgPixel.b, 128.f / 255.f, 2);
        CHECK(bgPixel.a == 255);
    });
#else
    DOCTEST_MESSAGE("skipping: built without FFmpeg");
#endif
}

TEST_CASE("Render device enumeration and reset/re-init (device switch)")
{
    runGpuTest("device enumeration", [](Gpu& g) {
        // The instance enumerates at least the device it was created on,
        // and the current device is among them with a type string.
        const auto devices = g.context.enumerateDevices();
        REQUIRE(!devices.empty());
        for (const auto& d : devices)
        {
            CHECK(!d.name.empty());
            CHECK(!d.type.empty());
        }
        const auto current = g.context.findDevice(g.context.deviceName());
        REQUIRE(current.has_value());
        CHECK(current->name == g.context.deviceName());
        CHECK(!g.context.deviceType().empty());
        CHECK(g.context.canRenderOn(current->handle, VK_NULL_HANDLE));

        // Case-insensitive exact-name lookup; a bogus name is a miss.
        std::string folded = g.context.deviceName();
        for (auto& c : folded)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        CHECK(g.context.findDevice(folded).has_value());
        CHECK(!g.context.findDevice("no such device").has_value());
    });

    // The destructive half: the exact teardown + re-init sequence the
    // playback app's frame loop performs for system.setRenderDevice
    // (quiesce, reset renderer, destroy the NVRHI/Vk device, re-select the
    // physical device by name, rebuild). It targets the SAME device, so
    // the shared Gpu stays usable for the other tests; a failure
    // mid-sequence recovers before the test returns.
    runGpuTest("device switch rebuild", [](Gpu& g) {
        const std::string currentName = g.context.deviceName();
        g.context.nvrhiDevice()->waitForIdle();
        g.renderer.reset();
        g.context.resetDevice();

        std::string error;
        const bool reinitOk =
            g.context.initPhysicalDevice(VK_NULL_HANDLE, currentName, error) &&
            g.context.initDevice(error);
        if (!reinitOk)
        {
            // Best-effort recovery: the following tests need a working device.
            g.context.initPhysicalDevice(VK_NULL_HANDLE, std::string(), error);
            g.context.initDevice(error);
            SceneRenderer::Desc rd;
            rd.device = g.context.nvrhiDevice();
            rd.sceneSize = Size2i{256, 256};
            SceneRenderer::create(rd, g.renderer, error);
            DOCTEST_MESSAGE("device re-init failed: " << error);
            return;
        }
        REQUIRE(g.context.deviceName() == currentName);

        SceneRenderer::Desc rd;
        rd.device = g.context.nvrhiDevice();
        rd.sceneSize = Size2i{256, 256};
        REQUIRE(SceneRenderer::create(rd, g.renderer, error));

        SceneRenderer::FrameInput frame;
        frame.layers.push_back(colorLayer(Color4{0.25f, 0.5f, 0.75f, 1.f}));
        renderOneFrame(g.renderer, frame);
        std::vector<std::uint8_t> rgba;
        REQUIRE(g.renderer.readBackScene(rgba));
        const Pixel p = pixelAt(rgba, 256, 128, 128);
        checkClose("r", p.r, 0.25f);
        checkClose("g", p.g, 0.5f);
        checkClose("b", p.b, 0.75f);
        CHECK(p.a == 255);
    });
}

} // TEST_SUITE("Render")
