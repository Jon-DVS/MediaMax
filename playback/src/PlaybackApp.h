#pragma once

// The playback application: ties the headless core (playback engine, media
// library, displays, control protocol) to the render module (Vulkan context,
// swapchains, scene renderer) and to the OS (GLFW windows, miniaudio device).
//
// Thread model (one application, five threads):
//
//   render thread (main)      owns the frame cadence. Each frame:
//                              - under m_StateMutex: engine.tick(), frameState(),
//                                display snapshot, pump target publication
//                              - outside the lock: deliverVideo() (texture
//                                uploads), record + present (see Swapchain.h)
//   media pump thread         consumes the IMediaSource decoders toward the
//                              published targets (MediaPump)
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

#include <array>
#include <atomic>
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

        // Last record geometry applied to the window. pollWindowEvents()
        // keeps window and DisplayManager record in sync in both directions;
        // these values tell which side changed last, and applyGraceFrames
        // withholds the write-back direction while our own apply is still in
        // flight (the WM reports the new size a frame or two later).
        std::int32_t appliedX = 0, appliedY = 0, appliedW = 0, appliedH = 0;
        int applyGraceFrames = 0;

        // Last warp offsets uploaded (cheap change detection).
        std::vector<std::array<float, 2>> lastWarpOffsets;
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
    void pollWindowEvents();

    // --- Control protocol -------------------------------------------------------

    protocol::Response handleCommand(const protocol::Command& cmd);
    Id loadMediaFile(const std::string& path, std::string& error);

    // --- State (guarded by m_StateMutex) -----------------------------------------

    AppArgs m_Args;
    std::mutex m_StateMutex;
    MediaLibrary m_Library;
    PlaybackEngine m_Engine;
    DisplayManager m_Displays;
    std::unique_ptr<protocol::CommandServer> m_CommandServer;
    std::unique_ptr<ControlServer> m_ControlServer;

    // --- Rendering (declared before the pump: it references the scene) ------------

    std::unique_ptr<render::VulkanContext> m_Vulkan;
    render::SceneRenderer m_Scene;
    std::vector<std::unique_ptr<DisplaySurface>> m_Surfaces;
    std::vector<const char*> m_InstanceExtensions;

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
