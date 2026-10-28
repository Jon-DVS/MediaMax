#include "SelfTest.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "MediaPump.h"
#include "mediamax/display/DisplayManager.h"
#include "mediamax/media/FFmpegSupport.h"
#include "mediamax/media/TestPatternSource.h"
#include "mediamax/playback/PlaybackEngine.h"
#include "mediamax/show/Show.h"
#include "mediamax/util/Logger.h"
#include "mediamax/render/SceneRenderer.h"
#include "mediamax/render/VulkanContext.h"

namespace mediamax
{

namespace
{

constexpr double kSelfTestFps = 30.0;

// The demo show's media, generated procedurally (no files required).
constexpr const char* kBarsPath = "mediamax:testpattern:colorbars";
constexpr const char* kCheckerPath = "mediamax:testpattern:checkerboard";

std::string selfTestPrefix(const std::string& outPath)
{
    // "selftest.ppm" -> "selftest" (frame files become selftest_0000.ppm, ...)
    const std::size_t dot = outPath.find_last_of('.');
    const std::size_t slash = outPath.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
    {
        return outPath.substr(0, dot);
    }
    return outPath;
}

bool writePpm(const std::string& path, int width, int height, const std::vector<std::uint8_t>& rgba)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
        return false;
    }
    out << "P6\n" << width << " " << height << "\n255\n";
    for (int i = 0; i < width * height; ++i)
    {
        out.put(static_cast<char>(rgba[i * 4 + 0]));
        out.put(static_cast<char>(rgba[i * 4 + 1]));
        out.put(static_cast<char>(rgba[i * 4 + 2]));
    }
    return out.good();
}

// The procedural media source for the "mediamax:testpattern:<kind>" paths.
std::shared_ptr<IMediaSource> createTestPatternSource(const std::string& path, std::uint32_t width,
                                                      std::uint32_t height, std::string& error)
{
    if (path.compare(0, std::strlen("mediamax:testpattern:"), "mediamax:testpattern:") != 0)
    {
        error = "not a test pattern path";
        return nullptr;
    }
    const std::string kind = path.substr(std::strlen("mediamax:testpattern:"));
    TestPatternKind patternKind = TestPatternKind::ColorBars;
    if (kind == "checkerboard")
    {
        patternKind = TestPatternKind::Checkerboard;
    }
    else if (kind == "gradient")
    {
        patternKind = TestPatternKind::Gradient;
    }
    else if (kind != "colorbars")
    {
        error = "unknown test pattern kind: " + kind;
        return nullptr;
    }
    return std::make_shared<TestPatternSource>(width, height, FrameRate::fps30(), Time::fromSeconds(6.0),
                                               patternKind);
}

// The media probe for test pattern paths.
MediaMetadata probeTestPattern(const std::string& path, std::uint32_t width, std::uint32_t height,
                               std::string& error)
{
    MediaMetadata md;
    md.type = MediaType::Video;
    md.hasVideo = true;
    md.width = width;
    md.height = height;
    md.duration = Time::fromSeconds(6.0);
    md.frameRate = FrameRate::fps30();
    (void)path;
    (void)error;
    return md;
}

} // namespace

int runSelfTest(const AppArgs& args)
{
    auto& log = Logger::instance();
    const int width = args.width > 0 ? args.width : 1280;
    const int height = args.height > 0 ? args.height : 720;

    log.info("MediaMax self-test: {}x{} offscreen, {}s", width, height, args.selfTestSeconds);

    // --- Vulkan (headless) -----------------------------------------------------
    render::VulkanContextDesc vkDesc;
    vkDesc.appName = "MediaMax Self-Test";
    render::VulkanContext vk;
    std::string error;
    if (!render::VulkanContext::create(vkDesc, vk, error))
    {
        log.error("self-test: Vulkan initialization failed: {}", error);
        return 1;
    }
    log.info("self-test: Vulkan device: {}", vk.deviceName());
    nvrhi::DeviceHandle device = vk.nvrhiDevice();

    // --- Engine side -------------------------------------------------------------
    MediaLibrary library;
#if defined(MEDIAMAX_HAS_FFMPEG)
    library.setProbeFunction(
        [width, height](const std::string& path, std::string& err) -> MediaMetadata {
            if (path.rfind("mediamax:testpattern:", 0) == 0)
            {
                return probeTestPattern(path, width, height, err);
            }
            return ffmpeg::probeFile(path, err);
        });
    library.setSourceFactory(
        [width, height](const std::string& path, std::string& err) -> std::shared_ptr<IMediaSource> {
            if (path.rfind("mediamax:testpattern:", 0) == 0)
            {
                return createTestPatternSource(path, width, height, err);
            }
            return ffmpeg::createSource(path, err);
        });
#endif
    PlaybackEngine engine(library);
    DisplayManager displays;
    const Display display = displays.addDisplay("Self-Test", 0, 0, width, height);

    // --- Show: the user's file, or the built-in demo ------------------------------
    auto loadShowFile = [&](const std::string& path) -> bool {
        std::ifstream in(path);
        if (!in)
        {
            log.error("self-test: cannot read show file '{}'", path);
            return false;
        }
        nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
        if (json.is_discarded())
        {
            log.error("self-test: invalid JSON in show file '{}'", path);
            return false;
        }
        // fromJson throws on malformed content (type errors, invalid frame
        // rates); treat every throw as "invalid show file".
        std::string showError;
        std::optional<Show> show;
        try
        {
            show = Show::fromJson(json, showError);
        }
        catch (const std::exception& e)
        {
            showError = e.what();
        }
        if (!show)
        {
            log.error("self-test: invalid show file '{}': {}", path, showError);
            return false;
        }
        // Load the show's media and remap the file-local ids to library ids
        // (same mapping the control protocol performs).
        std::map<Id, Id> remap;
        for (const auto& ref : show->media)
        {
            std::string loadError;
            const Id libraryId = library.load(ref.path, loadError);
            if (libraryId == kInvalidId)
            {
                log.warning("self-test: show references missing media '{}': {}", ref.path, loadError);
                continue;
            }
            remap[ref.id] = libraryId;
        }
        for (auto& ref : show->media)
        {
            if (const auto it = remap.find(ref.id); it != remap.end())
            {
                ref.id = it->second;
            }
        }
        for (auto& scene : show->scenes)
        {
            for (auto& layer : scene.layers)
            {
                if (layer.mediaId != kInvalidId)
                {
                    if (const auto it = remap.find(layer.mediaId); it != remap.end())
                    {
                        layer.mediaId = it->second;
                    }
                }
            }
        }
        engine.loadShow(std::move(*show));
        return true;
    };

    if (!args.showPath.empty())
    {
        if (!loadShowFile(args.showPath))
        {
            return 1;
        }
    }
    else
    {
        // Built-in demo: two scenes of procedural media, six seconds.
        Show show;
        show.name = "MediaMax Self-Test Demo";
        show.frameRate = FrameRate::fps30();
        show.duration = Time::fromSeconds(6.0);

        MediaReference barsRef;
        barsRef.path = kBarsPath;
        MediaReference checkerRef;
        checkerRef.path = kCheckerPath;
        show.media = {barsRef, checkerRef};

        const Id barsId = library.load(kBarsPath, error);
        const Id checkerId = library.load(kCheckerPath, error);
        if (barsId == kInvalidId || checkerId == kInvalidId)
        {
            log.error("self-test: failed to create the demo media: {}", error);
            return 1;
        }

        Scene sceneBars;
        sceneBars.id = 1;
        sceneBars.name = "Color Bars";
        Layer barsLayer;
        barsLayer.id = 10;
        barsLayer.name = "Bars";
        barsLayer.mediaId = barsId;
        sceneBars.layers = {barsLayer};

        Scene sceneChecker;
        sceneChecker.id = 2;
        sceneChecker.name = "Checkerboard";
        Layer checkerLayer;
        checkerLayer.id = 11;
        checkerLayer.name = "Checker";
        checkerLayer.mediaId = checkerId;
        sceneChecker.layers = {checkerLayer};
        show.scenes = {sceneBars, sceneChecker};

        Cue startCue;
        startCue.time = Time::zero();
        startCue.name = "Start";
        startCue.action = PlayScene{sceneBars.id};
        Cue secondCue;
        secondCue.time = Time::fromSeconds(3.0);
        secondCue.name = "Scene 2";
        secondCue.action = PlayScene{sceneChecker.id};
        show.cues = {startCue, secondCue};

        engine.loadShow(std::move(show));
    }
    engine.play();

    // --- Render side -----------------------------------------------------------------
    render::SceneRenderer::Desc sceneDesc;
    sceneDesc.device = device;
    sceneDesc.sceneSize = Size2i{width, height};
    render::SceneRenderer scene;
    if (!render::SceneRenderer::create(sceneDesc, scene, error))
    {
        log.error("self-test: scene renderer failed: {}", error);
        return 1;
    }

    // The offscreen display target (RGBA8, the scene size).
    nvrhi::TextureDesc targetDesc;
    targetDesc.setWidth(static_cast<std::uint32_t>(width))
        .setHeight(static_cast<std::uint32_t>(height))
        .setFormat(nvrhi::Format::RGBA8_UNORM)
        .setDimension(nvrhi::TextureDimension::Texture2D)
        .setIsRenderTarget(true)
        // The texture is only ever a render target: tell NVRHI so its state
        // tracker knows the prior state in every command list and inserts
        // correct automatic barriers.
        .enableAutomaticStateTracking(nvrhi::ResourceStates::RenderTarget)
        .setDebugName("self-test display target");
    auto displayTarget = device->createTexture(targetDesc);
    if (!displayTarget)
    {
        log.error("self-test: failed to create the display target");
        return 1;
    }

    std::mutex libraryMutex; // the pump's library lock (no concurrent loads here)
    MediaPump pump(library, libraryMutex, scene);
    pump.start();

    // --- Frame loop ------------------------------------------------------------------
    const int totalFrames = args.selfTestSeconds * static_cast<int>(kSelfTestFps);
    const std::string prefix = selfTestPrefix(args.selfTestOut);
    const Time frameDelta = Time::fromSeconds(1.0 / kSelfTestFps);
    const auto framePeriod = std::chrono::milliseconds(static_cast<int>(1000.0 / kSelfTestFps));

    std::uint64_t generation = 0;
    bool haveLastTime = false;
    Time lastShowTime;

    int blackFrames = 0;
    int sampledFrames = 0;
    double maxMeanLuma = 0.0;

    for (int frame = 0; frame < totalFrames; ++frame)
    {
        const auto frameStart = std::chrono::steady_clock::now();

        engine.tick(frameDelta);
        const FrameState fs = engine.frameState();

        // Publish pump targets (the same logic the windowed app uses).
        std::vector<LayerTarget> targets;
        targets.reserve(fs.layers.size());
        for (const auto& lf : fs.layers)
        {
            LayerTarget t;
            t.layerId = lf.layerId;
            t.mediaId = lf.mediaId;
            t.mediaTime = lf.mediaTime;
            t.gain = lf.opacity;
            if (lf.mediaId != kInvalidId)
            {
                if (const auto asset = library.find(lf.mediaId))
                {
                    const MediaMetadata& md = (*asset)->metadata;
                    t.mediaSize = Size2i{static_cast<std::int32_t>(md.width), static_cast<std::int32_t>(md.height)};
                    t.hasAudio = md.hasAudio;
                    t.audioChannels = md.audioChannels;
                    t.audioSampleRate = md.audioSampleRate;
                    t.mediaDuration = md.duration;
                }
            }
            targets.push_back(std::move(t));
        }
        if (haveLastTime)
        {
            Time jump = fs.showTime - lastShowTime;
            if (jump.isNegative())
            {
                jump = -jump;
            }
            if (jump > Time::fromSeconds(0.25))
            {
                generation++;
            }
        }
        lastShowTime = fs.showTime;
        haveLastTime = true;
        pump.publishTargets(fs.showTime, targets, generation);

        // Build the frame.
        render::SceneRenderer::FrameInput frameInput;
        for (const auto& lf : fs.layers)
        {
            if (!lf.visible || lf.opacity <= 0.001f)
            {
                continue;
            }
            render::SceneRenderer::LayerDraw draw;
            if (lf.mediaId != kInvalidId)
            {
                const LayerTarget* target = nullptr;
                for (const auto& t : targets)
                {
                    if (t.layerId == lf.layerId)
                    {
                        target = &t;
                        break;
                    }
                }
                draw.texture = target != nullptr ? pump.deliverVideo(*target) : nullptr;
                draw.mediaSize = target != nullptr ? target->mediaSize : Size2i{1, 1};
            }
            else
            {
                draw.color = lf.color;
                draw.mediaSize = Size2i{width, height};
            }
            draw.transform = Transform2D{lf.x, lf.y, lf.rotation, lf.scaleX, lf.scaleY, lf.anchorX, lf.anchorY};
            draw.opacity = lf.opacity;
            draw.blend = lf.blend;
            frameInput.layers.push_back(std::move(draw));
        }
        render::SceneRenderer::DisplayTarget dt;
        dt.image = displayTarget.Get();
        dt.rect = display.rect();
        dt.brightness = display.brightness;
        dt.isSwapchainImage = false;
        frameInput.displays.push_back(std::move(dt));

        auto cmdList = device->createCommandList();
        cmdList->open();
        scene.recordFrame(cmdList.Get(), frameInput);
        cmdList->close();
        nvrhi::ICommandList* cmd = cmdList.Get();
        device->executeCommandLists(&cmd, 1);
        device->waitForIdle();
        device->runGarbageCollection();

        // Sample one frame per second: read the scene back and verify it is
        // not black.
        if (frame % static_cast<int>(kSelfTestFps) == 0)
        {
            std::vector<std::uint8_t> rgba;
            if (scene.readBackScene(rgba))
            {
                std::ostringstream name;
                name << prefix << "_" << std::setw(4) << std::setfill('0')
                     << (frame / static_cast<int>(kSelfTestFps)) << ".ppm";
                if (!writePpm(name.str(), width, height, rgba))
                {
                    log.error("self-test: failed to write {}", name.str());
                    return 1;
                }
                double sum = 0.0;
                for (std::size_t i = 0; i < rgba.size(); i += 4)
                {
                    sum += 0.2126 * rgba[i] + 0.7152 * rgba[i + 1] + 0.0722 * rgba[i + 2];
                }
                const double meanLuma = sum / std::max<std::size_t>(rgba.size() / 4, 1);
                maxMeanLuma = std::max(maxMeanLuma, meanLuma);
                sampledFrames++;
                if (meanLuma < 8.0)
                {
                    blackFrames++;
                }
                log.info("self-test: frame {} written ({}), mean luma {:.1f}", name.str(),
                         fs.showTime.toTimecode(engine.show().frameRate), meanLuma);
            }
        }

        // Pace at real time so the pump works like in the windowed app.
        const auto elapsed = std::chrono::steady_clock::now() - frameStart;
        if (elapsed < framePeriod)
        {
            std::this_thread::sleep_for(framePeriod - elapsed);
        }
    }

    pump.stop();
    device->waitForIdle();

    // --- Verdict --------------------------------------------------------------------
    if (sampledFrames == 0)
    {
        log.error("self-test: FAILED - no frames sampled");
        return 1;
    }
    if (blackFrames == sampledFrames)
    {
        log.error("self-test: FAILED - every sampled frame is black");
        return 1;
    }
    log.info("self-test: OK - {} frames rendered, {} sampled ({} not black), max mean luma {:.1f}", totalFrames,
             sampledFrames, sampledFrames - blackFrames, maxMeanLuma);
    return 0;
}

} // namespace mediamax
