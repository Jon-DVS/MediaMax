#pragma once

// The playback application: ties the headless core (playback engine, media
// library, displays, control protocol) to the render module (Vulkan context,
// swapchains, scene renderer) and to the OS (GLFW windows, miniaudio device).
//
// Thread model (one application, a few threads):
//
//   render thread (main)      owns the frame cadence. Each frame:
//                              - under m_StateMutex: engine.tick(), frameState(),
//                                display snapshot, pump target publication
//                              - outside the lock: deliverVideo() (texture
//                                uploads), record + present (see Swapchain.h)
//   media pump coordinator    the MediaPump's original pump thread: spawns and
//                              retires the decode workers (MediaPump)
//   decode worker threads     one per active media layer: each owns its layer's
//                              IMediaSource instance (single-consumer) and
//                              decodes video/audio toward the published targets
//   audio render thread       miniaudio callback: snapshotTargets() + mixAudio()
//   control client threads    ControlServer runs each command under its own
//                              handler mutex; handleCommand() additionally
//                              takes m_StateMutex so a command and a frame see
//                              one consistent engine state
//   (glfw event handling)     on the render thread (glfwWaitEventsTimeout)
//
// A media load (control command) can therefore stall the frame loop for the
// duration of the FFmpeg probe/decode-open. That is deliberate: correctness
// (one writer-visible-at-a-time view) over latency for an admin action.
// (A decode worker opening its layer's source happens on the worker's own
// thread, so a layer appearing in the show never stalls the frame loop.)

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include "Arguments.h"
#include "AudioSink.h"
#include "MediaPump.h"
#include "mediamax/display/DisplayManager.h"
#include "mediamax/media/MediaLibrary.h"
#include "mediamax/net/ControlServer.h"
#include "mediamax/playback/MetricStats.h"
#include "mediamax/playback/PlaybackEngine.h"
#include "mediamax/protocol/CommandServer.h"
#include "mediamax/render/SceneRenderer.h"
#include "mediamax/render/Swapchain.h"
#include "mediamax/render/VulkanContext.h"

namespace mediamax
{

class PlaybackApp
{
public:
    explicit PlaybackApp(AppArgs args);
    ~PlaybackApp();

    PlaybackApp(const PlaybackApp&) = delete;
    PlaybackApp& operator=(const PlaybackApp&) = delete;

    // Initializes everything and runs the frame loop until the last window
    // is closed. Returns the process exit code.
    int run();

private:
    // One physical output: a GLFW window, its Vulkan surface and swapchain,
    // and the present-model fence (see render/Swapchain.h).
    struct DisplaySurface
    {
        Id displayId = kInvalidId;
        GLFWwindow* window = nullptr;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        render::Swapchain swapchain;

        VkFence fence = VK_NULL_HANDLE;
        bool fencePending = false;
        std::uint32_t imageIndex = 0;

        int width = 0;
        int height = 0;
        bool sizeDirty = false;
        bool enabled = true;

        // Last always-on-top state applied to the window (the per-frame
        // sync reconciles it with the record, like enabled/geometry).
        bool appliedAlwaysOnTop = true;

        // Last record geometry applied to the window. pollWindowEvents()
        // keeps window and DisplayManager record in sync in both directions;
        // these values tell which side changed last, and applyGraceFrames
        // withholds the write-back direction while our own apply is still in
        // flight (the WM reports the new size a frame or two later).
        std::int32_t appliedX = 0, appliedY = 0, appliedW = 0, appliedH = 0;
        int applyGraceFrames = 0;

        // The monitor the window is on (the record's physical output), for
        // cheap change detection when the window is dragged to another
        // output. GLFW owns the monitor.
        GLFWmonitor* lastMonitor = nullptr;

        // Last warp offsets + interpolation uploaded (cheap change
        // detection; the mode counts because the same offsets sample
        // differently per mode).
        std::vector<std::array<float, 2>> lastWarpOffsets;
        WarpInterpolation lastWarpMode = WarpInterpolation::Bilinear;
    };

    // One frame's resolved state, taken under m_StateMutex.
    struct FrameSnapshot
    {
        FrameState fs;
        std::vector<Display> displays;
        Size2i sceneSize{0, 0};
        std::vector<LayerTarget> targets;
        bool playing = false;
    };

    // A surface operation requested by display.add / display.remove. The
    // command handler (client thread) only queues it; the frame loop
    // executes it, because window, Vulkan surface and swapchain work is
    // main-thread work.
    struct SurfaceOp
    {
        bool add = true;
        Id displayId = kInvalidId;
        std::string name;
        std::int32_t x = 0, y = 0, width = 0, height = 0;
    };

    // --- Initialization / shutdown ------------------------------------------

    bool initWindowsAndDisplays();
    bool initVulkan();
    bool initEngine();
    void shutdown();

    bool createSwapchainFor(DisplaySurface& ds, std::string& error);
    void handleWindowResizes();

    // --- Frame loop -----------------------------------------------------------

    FrameSnapshot takeFrameSnapshot(const Time& dt);
    std::vector<render::SceneRenderer::LayerDraw> buildLayerDraws(const FrameSnapshot& snap);
    void renderFrame(const FrameSnapshot& snap);
    void updateWarpTexture(DisplaySurface& ds, const Display& display);
    // Rasterizes the status text (system IP, display name/number, measured
    // fps, resolution, CPU/GPU frame time) and uploads it to the scene
    // renderer's per-display overlay pool (core overlay/StatusOverlay.h).
    // Called once per frame for each display with statusOverlay set.
    void updateStatusOverlay(DisplaySurface& ds, const Display& display);
    void pollWindowEvents();

    // --- Control protocol -------------------------------------------------------

    protocol::Response handleCommand(const protocol::Command& cmd);
    Id loadMediaFile(const std::string& path, std::string& error);
    // Executes queued display.add / display.remove surface ops (main thread).
    void processPendingSurfaceOps();
    void addSurfaceForDisplay(const SurfaceOp& op);
    void removeSurfaceForDisplay(Id displayId);
    // Undoes a display.add whose record was created but whose surface failed.
    void removeDisplayRecord(Id displayId);

    // --- State (guarded by m_StateMutex) -----------------------------------------

    AppArgs m_Args;
    std::mutex m_StateMutex;
    MediaLibrary m_Library;
    PlaybackEngine m_Engine;
    DisplayManager m_Displays;
    // display.add / display.remove work queued by the command handlers
    // (client thread, under m_StateMutex) and executed by the frame loop.
    std::vector<SurfaceOp> m_PendingSurfaceOps;
    std::unique_ptr<protocol::CommandServer> m_CommandServer;
    std::unique_ptr<ControlServer> m_ControlServer;

    // --- Rendering (declared before the pump: it references the scene) ------------

    std::unique_ptr<render::VulkanContext> m_Vulkan;
    render::SceneRenderer m_Scene;
    std::vector<std::unique_ptr<DisplaySurface>> m_Surfaces;
    std::vector<const char*> m_InstanceExtensions;

    // --- Status overlay (render thread) ---------------------------------------------

    // GPU time of the whole frame (NVRHI timer query recorded around
    // recordFrame). The single-flight model (step 1 of renderFrame waits the
    // previous frame's fence) makes the result readable before the query is
    // re-armed for the current frame.
    nvrhi::TimerQueryHandle m_FrameQuery;
    double m_GpuFrameMs = 0.0;  // previous frame's GPU time
    double m_CpuFrameMs = 0.0;  // previous frame's recordFrame wall time
    // Exponential moving average of the measured frame period (renderFrame
    // cadence); 0 until the second frame.
    double m_FrameFps = 0.0;
    // Rolling min/max/avg windows behind the status overlay's stats block
    // (current + min/max/avg over the last ~10 s of samples each).
    MetricStats m_FpsStats;
    MetricStats m_CpuMsStats;
    MetricStats m_GpuMsStats;
    // Total video frames dropped so far (high water, all layers; from the
    // media pump's FrameDropCounters). Shown in the status overlay and
    // reported in the status protocol result.
    std::uint64_t m_DroppedVideoFrames = 0;
    std::chrono::steady_clock::time_point m_LastFrameTime{};
    bool m_HaveLastFrameTime = false;
    // This system's IPv4 (gethostname/getaddrinfo, computed once in run()).
    std::string m_SystemIp;
    // This machine's computer name (gethostname, computed once in run());
    // reported in the status result's "hostname".
    std::string m_SystemHost;

    // --- Pump / audio (own locks) -------------------------------------------------

    MediaPump m_Pump;
    AudioSink m_Audio;
    std::atomic<bool> m_AudioActive{false};

    std::uint64_t m_PumpGeneration = 0;
    Time m_LastPublishedShowTime;
    bool m_HaveLastPublishedShowTime = false;

    std::atomic<bool> m_ShutdownRequested{false};
    bool m_GlfwActive = false;
};

} // namespace mediamax
