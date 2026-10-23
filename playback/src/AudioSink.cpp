#include "AudioSink.h"

// Exactly one translation unit compiles the miniaudio implementation.
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "mediamax/util/Logger.h"

namespace mediamax
{

namespace
{

void deviceCallback(ma_device* device, void* output, const void* input, std::uint32_t frameCount)
{
    (void)input;
    auto* self = device != nullptr ? static_cast<AudioSink*>(device->pUserData) : nullptr;
    if (self == nullptr)
    {
        return;
    }
    self->mix(static_cast<float*>(output), static_cast<int>(frameCount));
}

} // namespace

AudioSink::AudioSink() = default;

AudioSink::~AudioSink()
{
    stop();
}

bool AudioSink::start(MixerCallback mixer)
{
    if (m_Active.load())
    {
        return true;
    }
    if (!mixer)
    {
        Logger::instance().error("audio sink: no mixer callback given");
        return false;
    }
    m_Mixer = std::move(mixer);

    // The context and device must outlive the worker thread, so they are heap
    // owned (the unique_ptrs delete them; stop() uninits first).
    ma_context* context = new ma_context();
    if (ma_context_init(nullptr, 0, nullptr, context) != MA_SUCCESS)
    {
        delete context;
        Logger::instance().warning("audio sink: no audio backend available (continuing without audio)");
        return false;
    }
    m_Context.reset(context);

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = static_cast<ma_uint32>(kSampleRate);
    config.periodSizeInFrames = 512;
    config.dataCallback = deviceCallback;
    config.pUserData = this; // deviceCallback reaches it via device->pUserData

    ma_device* device = new ma_device();
    if (ma_device_init(m_Context.get(), &config, device) != MA_SUCCESS)
    {
        // ma_device_init releases its own resources on failure.
        delete device;
        ma_context_uninit(m_Context.get());
        m_Context.reset();
        Logger::instance().warning("audio sink: failed to open the playback device (continuing without audio)");
        return false;
    }
    if (ma_device_start(device) != MA_SUCCESS)
    {
        ma_device_uninit(device);
        delete device;
        ma_context_uninit(m_Context.get());
        m_Context.reset();
        Logger::instance().warning("audio sink: failed to start the playback device (continuing without audio)");
        return false;
    }
    m_Device.reset(device);
    m_Active.store(true);
    Logger::instance().info("audio sink: 48 kHz stereo device running");
    return true;
}

void AudioSink::stop()
{
    if (m_Device)
    {
        // ma_device_stop joins the render thread, so no callback is in
        // flight when we release the device.
        ma_device_stop(m_Device.get());
        ma_device_uninit(m_Device.get());
        m_Device.reset();
    }
    if (m_Context)
    {
        ma_context_uninit(m_Context.get());
        m_Context.reset();
    }
    m_Active.store(false);
    m_Mixer = nullptr;
}

} // namespace mediamax
