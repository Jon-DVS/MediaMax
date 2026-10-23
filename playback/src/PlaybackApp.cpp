#include "PlaybackApp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

#include "WindowSurface.h"
#include "mediamax/media/FFmpegSupport.h"
#include "mediamax/show/Show.h"
#include "mediamax/util/Logger.h"

namespace mediamax
{

namespace
{

// A frame-loop stall (e.g. a media load under the state mutex) must not
// teleport the playhead; the show simply falls behind the wall clock and
// catches up.
constexpr double kMaxFrameDeltaSeconds = 0.5;

// Fence wait budget (ms) per frame.
constexpr std::uint32_t kFenceTimeoutMs = 2000;
constexpr std::uint32_t kAcquireTimeoutMs = 100;

// Dense warp texture size (display pass).
constexpr std::uint32_t kWarpTextureSize = 512;

} // namespace

PlaybackApp::PlaybackApp(AppArgs args)
    : m_Args(std::move(args))
    , m_Engine(m_Library)
    , m_Pump(m_Library, m_StateMutex, m_Scene)
{
}

PlaybackApp::~PlaybackApp()
{
    shutdown();
}

int PlaybackApp::run()
{
    auto& log = Logger::instance();
    log.info("MediaMax playback starting (system: {}, {} display{})",
             m_Args.systemName.empty() ? "unnamed" : m_Args.systemName,
             m_Args.displays.empty() ? 1 : m_Args.displays.size(),
             m_Args.displays.size() == 1 ? "" : "s");

    if (!initWindowsAndDisplays())
    {
        return 1;
    }
    if (!initVulkan())
    {
        shutdown();
        return 1;
    }
    if (!initEngine())
    {
        shutdown();
        return 1;
    }

    // Defensive quiesce before the first swapchain work (initVulkan already
    // waits for the scene renderer's one-time GPU op to finish).
    m_Vulkan->nvrhiDevice()->waitForIdle();

    for (auto& sp : m_Surfaces)
    {
        std::string error;
        if (!createSwapchainFor(*sp, error))
        {
            log.error("display {}: {}", sp->displayId, error);
        }
    }

    {
        std::lock_guard lock(m_StateMutex);
        m_Scene.setSceneSize(m_Displays.outputBounds().size());
    }

    // Auto-play the startup show (under the state mutex: the control server
    // is already serving commands by this point).
    {
        std::lock_guard lock(m_StateMutex);
        m_Engine.play();
    }

    auto lastTime = std::chrono::steady_clock::now();
    while (!m_ShutdownRequested.load())
    {
        glfwWaitEventsTimeout(0.016);
        pollWindowEvents();
        if (m_ShutdownRequested.load())
        {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        Time dt = Time::fromSeconds(std::chrono::duration<double>(now - lastTime).count());
        lastTime = now;
        if (dt.isNegative())
        {
            dt = Time::zero();
        }
        if (dt > Time::fromSeconds(kMaxFrameDeltaSeconds))
        {
            dt = Time::fromSeconds(kMaxFrameDeltaSeconds);
        }

        const FrameSnapshot snap = takeFrameSnapshot(dt);
        renderFrame(snap);
    }

    shutdown();
    return 0;
}

// --- Initialization / shutdown ------------------------------------------------

bool PlaybackApp::initWindowsAndDisplays()
{
    auto& log = Logger::instance();
    if (!glfwInit())
    {
        log.error("GLFW initialization failed");
        return false;
    }
    m_GlfwActive = true;
    if (!glfwVulkanSupported())
    {
        log.error("Vulkan is not supported by GLFW on this system");
        return false;
    }

    std::uint32_t extCount = 0;
    const char** exts = glfwGetRequiredInstanceExtensions(&extCount);
    for (std::uint32_t i = 0; i < extCount; ++i)
    {
        m_InstanceExtensions.push_back(exts[i]);
    }

    // One window per physical output; a single default display without
    // --display flags.
    std::vector<DisplaySpec> specs = m_Args.displays;
    if (specs.empty())
    {
        specs.push_back(DisplaySpec{m_Args.width, m_Args.height, 0, 0});
    }

    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        auto ds = std::make_unique<DisplaySurface>();
        const std::string title = "MediaMax - Display " + std::to_string(i + 1);
        ds->window = glfwCreateWindow(specs[i].width, specs[i].height, title.c_str(), nullptr, nullptr);
        if (ds->window == nullptr)
        {
            log.error("failed to create window for display {} ({}x{} at {},{})", i + 1, specs[i].width,
                      specs[i].height, specs[i].x, specs[i].y);
            return false;
        }
        glfwSetWindowPos(ds->window, specs[i].x, specs[i].y);
        ds->width = specs[i].width;
        ds->height = specs[i].height;
        m_Surfaces.push_back(std::move(ds));
    }

    std::lock_guard lock(m_StateMutex);
    for (std::size_t i = 0; i < m_Surfaces.size(); ++i)
    {
        const Display& d = m_Displays.addDisplay("Display " + std::to_string(i + 1), specs[i].x, specs[i].y,
                                                 specs[i].width, specs[i].height);
        m_Surfaces[i]->displayId = d.id;
    }
    return true;
}

bool PlaybackApp::initVulkan()
{
    auto& log = Logger::instance();
    std::string error;

    // Phase 1: the instance (with GLFW's surface extensions).
    render::VulkanContextDesc desc;
    desc.appName = "MediaMax Playback";
    desc.instanceExtensions = m_InstanceExtensions;
    m_Vulkan = std::make_unique<render::VulkanContext>();
    if (!render::VulkanContext::createInstance(desc, *m_Vulkan, error))
    {
        log.error("Vulkan instance creation failed: {}", error);
        return false;
    }

    // Phase 2: one surface per window (they need the instance).
    for (auto& sp : m_Surfaces)
    {
        if (!createWindowSurface(m_Vulkan->instance(), sp->window, sp->surface, error))
        {
            log.error("display {}: {}", sp->displayId, error);
            return false;
        }
    }

    // Phase 3: physical device (one supporting the primary surface) + the
    // logical device with the swapchain extension + the NVRHI wrapper.
    const VkSurfaceKHR primary = m_Surfaces.empty() ? VK_NULL_HANDLE : m_Surfaces[0]->surface;
    if (!m_Vulkan->initPhysicalDevice(primary, error))
    {
        log.error("Vulkan physical device selection failed: {}", error);
        return false;
    }
    if (!m_Vulkan->initDevice(error))
    {
        log.error("Vulkan device creation failed: {}", error);
        return false;
    }

    // Phase 4: the scene renderer (needs the NVRHI device; the scene size
    // follows the display layout established in initWindowsAndDisplays).
    // Its setup does a synchronous GPU op (white texture) and waits for
    // idle, so the queue is quiescent when the swapchain work begins.
    {
        Size2i sceneSize{0, 0};
        {
            std::lock_guard lock(m_StateMutex);
            sceneSize = m_Displays.outputBounds().size();
        }
        render::SceneRenderer::Desc sceneDesc;
        sceneDesc.device = m_Vulkan->nvrhiDevice();
        sceneDesc.sceneSize = sceneSize;
        if (!render::SceneRenderer::create(sceneDesc, m_Scene, error))
        {
            log.error("scene renderer creation failed: {}", error);
            return false;
        }
    }

    log.info("Vulkan context ready: {}", m_Vulkan->deviceName());
    return true;
}

void PlaybackApp::shutdown()
{
    auto& log = Logger::instance();

    m_ControlServer.reset(); // stops the accept thread + client threads
    m_Pump.stop();
    m_Audio.stop();

    if (m_Vulkan && m_Vulkan->nvrhiDevice())
    {
        m_Vulkan->nvrhiDevice()->waitForIdle();
    }

    for (auto& sp : m_Surfaces)
    {
        DisplaySurface& ds = *sp;
        if (ds.swapchain.valid())
        {
            ds.swapchain.destroy();
        }
        if (ds.fence != VK_NULL_HANDLE && m_Vulkan)
        {
            vkDestroyFence(m_Vulkan->device(), ds.fence, nullptr);
            ds.fence = VK_NULL_HANDLE;
        }
        if (ds.window != nullptr)
        {
            glfwDestroyWindow(ds.window);
            ds.window = nullptr;
        }
        if (ds.surface != VK_NULL_HANDLE && m_Vulkan)
        {
            vkDestroySurfaceKHR(m_Vulkan->instance(), ds.surface, nullptr);
            ds.surface = VK_NULL_HANDLE;
        }
    }
    m_Surfaces.clear();

    // m_Vulkan is intentionally NOT reset here: the member destructor order
    // (m_Scene before m_Vulkan) requires the VkDevice to outlive the scene
    // renderer's NVRHI resources, which release themselves on destruction.

    if (m_GlfwActive)
    {
        glfwTerminate();
        m_GlfwActive = false;
    }
    log.info("MediaMax playback stopped");
}

bool PlaybackApp::createSwapchainFor(DisplaySurface& ds, std::string& error)
{
    render::Swapchain::Desc desc;
    desc.surface = ds.surface;
    desc.physicalDevice = m_Vulkan->physicalDevice();
    desc.device = m_Vulkan->device();
    desc.nvrhiDevice = m_Vulkan->vulkanDevice();
    desc.width = ds.width;
    desc.height = ds.height;
    if (!ds.swapchain.create(desc, error))
    {
        return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(m_Vulkan->device(), &fenceInfo, nullptr, &ds.fence) != VK_SUCCESS)
    {
        error = "vkCreateFence failed";
        return false;
    }
    return true;
}

void PlaybackApp::handleWindowResizes()
{
    for (auto& sp : m_Surfaces)
    {
        DisplaySurface& ds = *sp;
        if (ds.window == nullptr)
        {
            continue;
        }
        int w = 0, h = 0;
        glfwGetFramebufferSize(ds.window, &w, &h);
        if (w > 1 && h > 1 && (w != ds.width || h != ds.height || !ds.swapchain.valid()))
        {
            ds.width = w;
            ds.height = h;
            ds.sizeDirty = true;
        }
    }

    if (std::none_of(m_Surfaces.begin(), m_Surfaces.end(),
                     [](const auto& sp) { return sp->sizeDirty; }))
    {
        return;
    }

    // Swapchain recreation requires a quiescent queue.
    m_Vulkan->nvrhiDevice()->waitForIdle();
    for (auto& sp : m_Surfaces)
    {
        DisplaySurface& ds = *sp;
        if (!ds.sizeDirty)
        {
            continue;
        }
        ds.sizeDirty = false;
        std::string error;
        if (ds.swapchain.valid())
        {
            if (!ds.swapchain.recreate(ds.width, ds.height, error))
            {
                Logger::instance().error("display {} swapchain recreate failed: {}", ds.displayId, error);
            }
        }
        else if (!createSwapchainFor(ds, error))
        {
            Logger::instance().error("display {} swapchain create failed: {}", ds.displayId, error);
        }
        // The display's output-space size follows its window.
        {
            std::lock_guard lock(m_StateMutex);
            if (const auto display = m_Displays.find(ds.displayId))
            {
                m_Displays.setDisplayGeometry(ds.displayId, (*display)->x, (*display)->y, ds.width,
                                              ds.height);
            }
        }
    }
    m_Scene.invalidateDisplayFramebufferCache();
}

// --- Frame loop ----------------------------------------------------------------

PlaybackApp::FrameSnapshot PlaybackApp::takeFrameSnapshot(const Time& dt)
{
    std::lock_guard lock(m_StateMutex);

    m_Engine.tick(dt);
    FrameSnapshot snap;
    snap.fs = m_Engine.frameState();
    snap.displays = m_Displays.displayList();
    snap.sceneSize = m_Displays.outputBounds().size();
    snap.playing = m_Engine.state() == PlayState::Playing;
    m_AudioActive.store(snap.playing);

    // The pump re-seeks channels when the show time jumps (seek command,
    // SeekTo cue, stop-to-start). Loop wraps are caught by the pump itself
    // (per-channel backward jump).
    if (m_HaveLastPublishedShowTime)
    {
        Time jump = snap.fs.showTime - m_LastPublishedShowTime;
        if (jump.isNegative())
        {
            jump = -jump;
        }
        if (jump > Time::fromSeconds(0.25))
        {
            m_PumpGeneration++;
        }
    }
    m_LastPublishedShowTime = snap.fs.showTime;
    m_HaveLastPublishedShowTime = true;

    snap.targets.reserve(snap.fs.layers.size());
    for (const auto& lf : snap.fs.layers)
    {
        LayerTarget t;
        t.layerId = lf.layerId;
        t.mediaId = lf.mediaId;
        t.mediaTime = lf.mediaTime;
        t.gain = lf.opacity;
        if (lf.mediaId != kInvalidId)
        {
            if (const auto asset = m_Library.find(lf.mediaId))
            {
                const MediaMetadata& md = (*asset)->metadata;
                t.mediaSize = Size2i{static_cast<std::int32_t>(md.width), static_cast<std::int32_t>(md.height)};
                t.hasAudio = md.hasAudio;
                t.audioChannels = md.audioChannels;
                t.audioSampleRate = md.audioSampleRate;
                t.mediaDuration = md.duration;
            }
            // The loop flag lives on the Layer, not the resolved frame.
            if (snap.fs.activeSceneId != kInvalidId)
            {
                if (const auto* scene = m_Engine.show().findScene(snap.fs.activeSceneId))
                {
                    if (const auto* layer = scene->findLayer(lf.layerId))
                    {
                        t.loop = layer->loop;
                    }
                }
            }
        }
        snap.targets.push_back(std::move(t));
    }
    m_Pump.publishTargets(snap.fs.showTime, snap.targets, m_PumpGeneration);
    return snap;
}

std::vector<render::SceneRenderer::LayerDraw> PlaybackApp::buildLayerDraws(const FrameSnapshot& snap)
{
    std::vector<render::SceneRenderer::LayerDraw> draws;
    draws.reserve(snap.fs.layers.size());
    for (const auto& lf : snap.fs.layers)
    {
        if (!lf.visible || lf.opacity <= 0.001f)
        {
            continue;
        }
        render::SceneRenderer::LayerDraw draw;
        if (lf.mediaId != kInvalidId)
        {
            const LayerTarget* target = nullptr;
            for (const auto& t : snap.targets)
            {
                if (t.layerId == lf.layerId)
                {
                    target = &t;
                    break;
                }
            }
            draw.texture = target != nullptr ? m_Pump.deliverVideo(*target) : nullptr;
            draw.mediaSize = target != nullptr ? target->mediaSize : Size2i{1, 1};
        }
        else
        {
            draw.texture = nullptr; // color layer: the renderer uses its white texture
            draw.color = lf.color;
            draw.mediaSize = m_Scene.sceneSize(); // a color layer fills the output space
        }
        draw.transform = Transform2D{lf.x, lf.y, lf.rotation, lf.scaleX, lf.scaleY, lf.anchorX, lf.anchorY};
        draw.opacity = lf.opacity;
        draw.blend = lf.blend;
        draws.push_back(std::move(draw));
    }
    return draws;
}

void PlaybackApp::renderFrame(const FrameSnapshot& snap)
{
    nvrhi::DeviceHandle device = m_Vulkan->nvrhiDevice();
    if (!device)
    {
        return;
    }

    handleWindowResizes();
    if (snap.sceneSize.valid() && snap.sceneSize != m_Scene.sceneSize())
    {
        m_Scene.setSceneSize(snap.sceneSize);
        m_Scene.invalidateDisplayFramebufferCache();
    }

    // 1. Wait for the previous frame to be fully done (single-flight present
    //    model, see render/Swapchain.h).
    VkDevice vkDevice = m_Vulkan->device();
    for (auto& sp : m_Surfaces)
    {
        DisplaySurface& ds = *sp;
        if (ds.fencePending)
        {
            const VkResult wait = vkWaitForFences(vkDevice, 1, &ds.fence, VK_TRUE,
                                                  static_cast<std::uint64_t>(kFenceTimeoutMs) * 1000000ull);
            if (wait == VK_SUCCESS)
            {
                vkResetFences(vkDevice, 1, &ds.fence);
            }
            else
            {
                Logger::instance().warning("frame fence wait timed out (display {})", ds.displayId);
            }
            ds.fencePending = false;
        }
    }

    // 2. Acquire back buffers synchronously and start state tracking.
    auto cmdList = device->createCommandList();
    cmdList->open();
    std::vector<DisplaySurface*> presenting;
    for (auto& sp : m_Surfaces)
    {
        DisplaySurface& ds = *sp;
        if (!ds.swapchain.valid() || !ds.enabled)
        {
            continue;
        }
        std::string error;
        std::uint32_t index = 0;
        if (!ds.swapchain.acquire(index, kAcquireTimeoutMs, error))
        {
            Logger::instance().debug("acquire failed (display {}): {}; skipping this frame", ds.displayId, error);
            continue;
        }
        ds.imageIndex = index;
        nvrhi::ITexture* image = ds.swapchain.image(index).Get();
        cmdList->beginTrackingTextureState(image, nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
        presenting.push_back(&ds);
    }
    if (presenting.empty())
    {
        cmdList->close();
        return;
    }

    // 3. Build the frame and record it.
    render::SceneRenderer::FrameInput frame;
    frame.layers = buildLayerDraws(snap);
    for (DisplaySurface* ds : presenting)
    {
        const Display* display = nullptr;
        for (const auto& d : snap.displays)
        {
            if (d.id == ds->displayId)
            {
                display = &d;
                break;
            }
        }
        if (display == nullptr)
        {
            continue;
        }
        render::SceneRenderer::DisplayTarget target;
        target.image = ds->swapchain.image(ds->imageIndex).Get();
        target.rect = display->rect();
        target.brightness = display->brightness;
        target.isSwapchainImage = true;
        if (!display->warp.isIdentity())
        {
            updateWarpTexture(*ds, *display);
            target.warp = m_Scene.warpTexture(display->id).Get();
        }
        frame.displays.push_back(std::move(target));
    }

    m_Scene.recordFrame(cmdList.Get(), frame);
    cmdList->close();

    // 4. Submit the render, then an empty submit per display that signals the
    //    frame fence (signaled when all prior queue work = our render is
    //    done), then present (queue-ordered behind the render).
    nvrhi::ICommandList* cmd = cmdList.Get();
    device->executeCommandLists(&cmd, 1);

    VkQueue queue = m_Vulkan->graphicsQueue();
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    for (DisplaySurface* ds : presenting)
    {
        vkQueueSubmit(queue, 1, &submit, ds->fence);
        ds->fencePending = true;

        std::string error;
        if (!ds->swapchain.present(queue, ds->imageIndex, error))
        {
            // VK_SUBOPTIMAL_KHR / out-of-date on resize are expected; the
            // next frame recreates the swapchain.
            Logger::instance().debug("present failed (display {}): {}", ds->displayId, error);
        }
    }
}

void PlaybackApp::updateWarpTexture(DisplaySurface& ds, const Display& display)
{
    const std::vector<std::array<float, 2>> offsets = display.warp.pointOffsets();
    if (offsets == ds.lastWarpOffsets)
    {
        return;
    }
    ds.lastWarpOffsets = offsets;
    m_Scene.ensureWarpTexture(display.id, kWarpTextureSize);
    std::vector<unsigned char> data;
    display.warp.buildDenseTexture(data, static_cast<int>(kWarpTextureSize));
    m_Scene.updateWarpTexture(display.id, data);
}

void PlaybackApp::pollWindowEvents()
{
    glfwPollEvents();
    for (auto& sp : m_Surfaces)
    {
        DisplaySurface& ds = *sp;
        if (ds.window != nullptr)
        {
            if (glfwWindowShouldClose(ds.window))
            {
                m_ShutdownRequested.store(true);
                return;
            }
            // Keep the enabled flag in sync with DisplayManager (display.set).
            bool enabled = true;
            {
                std::lock_guard lock(m_StateMutex);
                if (const auto display = m_Displays.find(ds.displayId))
                {
                    enabled = (*display)->enabled;
                }
            }
            if (enabled != ds.enabled)
            {
                ds.enabled = enabled;
                if (enabled)
                {
                    glfwShowWindow(ds.window);
                }
                else
                {
                    glfwHideWindow(ds.window);
                }
            }
        }
    }
}

// --- Control protocol -------------------------------------------------------------

protocol::Response PlaybackApp::handleCommand(const protocol::Command& cmd)
{
    std::lock_guard lock(m_StateMutex);
    return m_CommandServer->handle(cmd);
}

Id PlaybackApp::loadMediaFile(const std::string& path, std::string& error)
{
    // Called with m_StateMutex held (the command handler holds it).
    return m_Library.load(path, error);
}

// --- Engine setup ---------------------------------------------------------------------

bool PlaybackApp::initEngine()
{
    auto& log = Logger::instance();

#if defined(MEDIAMAX_HAS_FFMPEG)
    m_Library.setProbeFunction(&ffmpeg::probeFile);
    m_Library.setSourceFactory(&ffmpeg::createSource);
#else
    log.warning("built without FFmpeg: file media cannot be loaded");
#endif

    m_CommandServer = std::make_unique<protocol::CommandServer>(
        m_Engine, m_Library, m_Displays, m_Args.systemName.empty() ? "MediaMax" : m_Args.systemName);
    m_CommandServer->setMediaLoadFunction(
        [this](const std::string& path, std::string& error) { return loadMediaFile(path, error); });

    // Startup show: go through the same command handler as the network
    // protocol (media loading + id remapping + loadShow).
    if (!m_Args.showPath.empty())
    {
        protocol::Command cmd;
        cmd.method = "show.load";
        cmd.params = {{"path", m_Args.showPath}};
        const protocol::Response resp = m_CommandServer->handle(cmd);
        if (!resp.ok)
        {
            log.error("failed to load show '{}': {}", m_Args.showPath, resp.error);
            return false;
        }
        log.info("show '{}' loaded ({} scenes, {} media)", m_Engine.show().name, m_Engine.show().scenes.size(),
                 m_Engine.show().media.size());
    }
    else
    {
        log.info("no --show given: running with an empty show (load one via the control interface)");
    }

    m_ControlServer = std::make_unique<ControlServer>(m_Args.host, m_Args.port);
    m_ControlServer->setHandler([this](const protocol::Command& cmd) { return handleCommand(cmd); });
    if (!m_ControlServer->start())
    {
        log.error("control server failed to bind {}:{}; continuing without the control interface", m_Args.host,
                  m_Args.port);
        m_ControlServer.reset();
    }
    else
    {
        log.info("control server listening on {}:{}", m_Args.host, m_ControlServer->port());
    }

    m_Pump.start();
    m_Audio.start([this](float* stereo, int frameCount) {
        if (!m_AudioActive.load())
        {
            // Paused: hold silence (the targets' media time is frozen, so
            // mixing would replay the same samples forever).
            for (int i = 0; i < frameCount; ++i)
            {
                stereo[i * 2 + 0] = 0.f;
                stereo[i * 2 + 1] = 0.f;
            }
            return;
        }
        m_Pump.mixAudio(m_Pump.snapshotTargets(), stereo, frameCount);
    });

    return true;
}

} // namespace mediamax
