#pragma once

// The audio output device (miniaudio).
//
// A single stereo float32 device at 48 kHz. The MediaPump's mix callback is
// invoked from the miniaudio render thread; it must be fast (no blocking, no
// locks held longer than a few microseconds - the pump's channel locks are
// fine because the pump decodes in 2 ms sleep slices between them).
//
// MINIAUDIO_IMPLEMENTATION is compiled exactly once, in AudioSink.cpp.

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

struct ma_context;
struct ma_device; // forward declarations (miniaudio)

namespace mediamax
{

class AudioSink
{
public:
    static constexpr int kSampleRate = 48000;

    // Fills `frameCount` interleaved stereo frames (outStereo has
    // frameCount * 2 floats). Invoked on the miniaudio render thread.
    using MixerCallback = std::function<void(float* outStereo, int frameCount)>;

    // Defined in the .cpp (where ma_device/ma_context are complete): the
    // destructor destroys the unique_ptr members, which requires the
    // complete types. start() heap-allocates the structs so they outlive
    // the miniaudio worker thread.
    AudioSink();
    ~AudioSink();

    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;

    // Creates and starts the device with the given mixer. Returns false (and
    // logs) when no audio device is available; playback still works without
    // audio in that case.
    bool start(MixerCallback mixer);

    // Stops and releases the device. Idempotent.
    void stop();

    bool active() const { return m_Active.load(); }

    // Runs the mixer (or fills silence when inactive). Called from the
    // miniaudio render thread.
    void mix(float* outStereo, int frameCount)
    {
        if (m_Active.load() && m_Mixer)
        {
            m_Mixer(outStereo, frameCount);
        }
        else
        {
            for (int i = 0; i < frameCount; ++i)
            {
                outStereo[i * 2 + 0] = 0.f;
                outStereo[i * 2 + 1] = 0.f;
            }
        }
    }

private:
    std::unique_ptr<ma_context> m_Context;
    std::unique_ptr<ma_device> m_Device;
    std::atomic<bool> m_Active{false};
    MixerCallback m_Mixer; // written before the device starts, read on the render thread
};

} // namespace mediamax
