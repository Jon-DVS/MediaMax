// FFmpeg implementation of IMediaSource and file probing.
//
// One AVFormatContext per source. Video is decoded and scaled to RGBA8;
// audio is decoded and resampled to interleaved float32. Decoded frames are
// buffered in small in-memory queues so the consumer (media pump) can pull
// with a timeout without blocking on file IO.

#include "mediamax/media/FFmpegSupport.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <thread>
#include <vector>

// FFmpeg's public headers (FFmpeg >= 5) no longer wrap themselves in
// extern "C"; C++ translation units must establish C linkage themselves,
// otherwise references come out mangled and the linker cannot match them
// against the C-named symbols in the FFmpeg import libraries (LNK2019).
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "mediamax/util/Logger.h"

namespace mediamax::ffmpeg
{

namespace
{

constexpr size_t kMaxQueuedVideoFrames = 8;
constexpr size_t kMaxQueuedAudioFrames = 16;
constexpr int kAudioBlockFrames = 4096;

// Added to the resolved decoder thread count for the pumps' in-flight packet
// caps (FFmpegSource::m_VideoInFlightCap / m_AudioInFlightCap). A
// frame-threaded decoder's input latency is roughly thread_count plus the
// stream's reorder window (the H264 DPB size, 16, is the worst case), and a
// cap below that latency stalls the pipeline forever - the decoder withholds
// every output until the window is filled, and the pump stops feeding it.
// The decoder's own EAGAIN (its internal input buffer) is the true upper
// bound; this cap must only ever stay ABOVE the latency. See pumpVideo.
constexpr int kInFlightReorderMargin = 16;

// Packets of the *other* stream are parked here while one pump waits for a
// frame of its own stream. Video and audio pulls alternate on the shared
// demuxer, so without parking, the video pump would silently drop every
// audio packet it crosses (and vice versa), punching holes in the decoded
// audio and drifting A/V out of sync. The lot only ever holds the few
// packets read between two pumps of one stream; the cap is a memory bound
// against pathological containers.
constexpr size_t kMaxParkedPackets = 64;

// Return values of FFmpegSource::nextPacketForStream.
constexpr int kPacketGot = 0;
constexpr int kPacketEof = AVERROR_EOF;
constexpr int kPacketDeadline = -1;

Time toTime(int64_t value, const AVRational& timeBase)
{
    if (value == AV_NOPTS_VALUE || timeBase.den <= 0)
    {
        return Time::zero();
    }
    // Exact rational seconds (value * num / den). The double round-trip
    // (Time::fromSeconds) quantizes to the nearest microsecond, and a 1/60 s
    // frame step then alternates 16666/16667 us - enough to break the
    // frame-exact step arithmetic downstream (FrameDropCounter floors the
    // step ratio, so a one-frame advance of 16666 us counted as zero). The
    // product is safe int64 for realistic pts and time_base values.
    return Time(value * timeBase.num, timeBase.den);
}

FrameRate frameRateFromRational(const AVRational& r)
{
    if (r.num > 0 && r.den > 0)
    {
        return FrameRate::of(r.num, r.den);
    }
    return FrameRate::fps30();
}

bool isImageCodec(int codecId)
{
    switch (codecId)
    {
        case AV_CODEC_ID_PNG:
        case AV_CODEC_ID_MJPEG: // JPEG stills decode as MJPEG
        case AV_CODEC_ID_BMP:
        case AV_CODEC_ID_TIFF:
        case AV_CODEC_ID_WEBP:
        case AV_CODEC_ID_GIF:
        case AV_CODEC_ID_DPX:
        case AV_CODEC_ID_EXR:
            return true;
        default:
            return false;
    }
}

std::string errorString(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

// True when the video stream of an already-opened file is a single still
// image rather than a video. Standalone image files are read by the
// image2 family's pipe demuxers (bmp_pipe, png_pipe, image2_pipe, ...),
// which read exactly one file and report neither a frame count nor a
// duration -- such a stream is a single-frame image by construction.
// (Animated GIFs use the regular "gif" demuxer, which does report the
// frame count, so they are handled by the frame-count/duration checks.)
bool isSingleFrameImageStream(const AVFormatContext* ctx, const AVStream* video,
                              Time duration, FrameRate frameRate)
{
    const std::string iformatName =
        (ctx && ctx->iformat) ? ctx->iformat->name : "";
    const bool singleFileImageDemuxer =
        iformatName == "image2" ||
        (iformatName.size() > 5 &&
         iformatName.compare(iformatName.size() - 5, 5, "_pipe") == 0);
    const bool singleFrame = video->nb_frames == 1 ||
        (duration > Time::zero() && duration < Time::fromFrames(2, frameRate));
    return singleFrame || singleFileImageDemuxer;
}

// Fills the codec-level details (codec name, colorspace, bit depth, scan
// type, bit rate) from the probed streams. Called by both open() and
// probeFile() so the two paths stay in sync (see also
// isSingleFrameImageStream).
void fillCodecDetails(MediaMetadata& metadata, const AVFormatContext* ctx,
                      const AVStream* video, const AVStream* audio)
{
    if (video != nullptr)
    {
        const AVCodecParameters* par = video->codecpar;
        metadata.videoCodec = avcodec_get_name(par->codec_id);
        metadata.videoColorspace = av_color_space_name(par->color_space);
        // Bit depth: the container's per-sample depth when it reports one,
        // otherwise the depth of the pixel format's first component.
        if (par->bits_per_raw_sample > 0)
        {
            metadata.videoBitDepth = static_cast<std::uint32_t>(par->bits_per_raw_sample);
        }
        else if (par->format != AV_PIX_FMT_NONE)
        {
            if (const AVPixFmtDescriptor* desc =
                    av_pix_fmt_desc_get(static_cast<AVPixelFormat>(par->format)))
            {
                metadata.videoBitDepth = static_cast<std::uint32_t>(desc->comp[0].depth);
            }
        }
        // field_order is set by the codec parser (e.g. the H.264 SPS) when
        // the stream declares it; absent, the scan type stays unknown.
        switch (par->field_order)
        {
            case AV_FIELD_PROGRESSIVE:
                metadata.videoScanType = "progressive";
                break;
            case AV_FIELD_TT:
            case AV_FIELD_BB:
            case AV_FIELD_TB:
            case AV_FIELD_BT:
                metadata.videoScanType = "interlaced";
                break;
            default:
                break; // AV_FIELD_UNKNOWN
        }
        // The stream's bit rate (codec parameters in FFmpeg 9; AVStream no
        // longer carries it), then the container's overall rate.
        metadata.videoBitRate = par->bit_rate > 0 ? par->bit_rate
                              : (ctx != nullptr && ctx->bit_rate > 0 ? ctx->bit_rate : 0);
    }
    if (audio != nullptr)
    {
        metadata.audioCodec = avcodec_get_name(audio->codecpar->codec_id);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// FFmpegSource
// ---------------------------------------------------------------------------

class FFmpegSource : public IMediaSource
{
public:
    ~FFmpegSource() override;

    MediaMetadata metadata() const override { return m_Metadata; }
    bool seek(const Time& t) override;
    bool nextVideoFrame(VideoFrame& out, std::uint32_t timeoutMs) override;
    bool nextAudioFrame(AudioFrame& out, std::uint32_t timeoutMs) override;
    bool hasVideo() const override { return m_Metadata.hasVideo; }
    bool hasAudio() const override { return m_Metadata.hasAudio; }

private:
    friend std::shared_ptr<IMediaSource> createSource(const std::string&, std::string&);

    bool open(const std::string& path, std::string& error);
    bool setupVideo(std::string& error);
    bool setupAudio(std::string& error);
    void teardownStreams();

    bool pumpVideo(uint64_t deadlineMs);
    bool pumpAudio(uint64_t deadlineMs);
    // Recreates the video scaler at min(native, on-screen) (see
    // IMediaSource::setVideoOutputSizeHint).
    void setVideoOutputSizeHint(Size2i onScreenSize) override;
    // Parks the packet in m_Packet (the other stream's data) so the other
    // pump can decode it later instead of dropping it.
    void parkPacket();
    // Reads from the parking lot first, then the demuxer, until a packet for
    // `streamIndex` is found; packets of the other stream are parked.
    // Returns kPacketGot with the packet in out, kPacketEof when the input is
    // exhausted, kPacketDeadline when the deadline passed first.
    int nextPacketForStream(int streamIndex, AVPacket& out, uint64_t deadlineMs);
    void clearParkedPackets();
    void flushDecoders();
    void decodeVideoFrame(const AVFrame& frame);
    void decodeAudioFrame(const AVFrame& frame);

    MediaMetadata m_Metadata;

    AVFormatContext* m_FormatCtx = nullptr;

    AVStream* m_VideoStream = nullptr;
    AVStream* m_AudioStream = nullptr;
    AVCodecContext* m_VideoCodec = nullptr;
    AVCodecContext* m_AudioCodec = nullptr;
    SwsContext* m_Sws = nullptr;
    SwrContext* m_Swr = nullptr;

    AVPacket m_Packet;

    // Other-stream packets read but not yet decodable, held in file order
    // (see kMaxParkedPackets).
    std::deque<AVPacket> m_ParkedPackets;

    std::deque<VideoFrame> m_VideoQueue;
    std::deque<AudioFrame> m_AudioQueue;
    int m_VideoQueueDropped = 0;

    int m_VideoFinished = 0;
    int m_AudioFinished = 0;

    // Packets accepted by the decoder whose frames have not been received
    // yet (the frame-threaded decoder decodes them asynchronously). Bounded
    // by the pumps' in-flight caps; see pumpVideo.
    int m_VideoInFlight = 0;
    int m_AudioInFlight = 0;

    // In-flight packet caps for the pumps, derived from the resolved
    // (auto) decoder thread count after avcodec_open2: thread_count +
    // kInFlightReorderMargin. The defaults cover the window before setup
    // (the pumps do not run until the streams are set up).
    int m_VideoInFlightCap = 32;
    int m_AudioInFlightCap = 32;

    // Current sws output size (the decode resolution); see
    // setVideoOutputSizeHint.
    int m_SwsOutWidth = 0;
    int m_SwsOutHeight = 0;

    // (Rare failure indicator) Last popped video frame time: a jump of more
    // than two frame steps between consecutive pulls means frames were
    // skipped between decoded frames (see nextVideoFrame).
    bool m_HaveLastVideoTime = false;
    Time m_LastVideoTime;

    // A packet the decoder refused (send_packet EAGAIN: its input buffer was
    // still full because a frame is in flight). Re-offered on the next pull
    // for this stream instead of being dropped.
    AVPacket m_PendingPacket = {};
    bool m_HavePendingPacket = false;
};

FFmpegSource::~FFmpegSource()
{
    teardownStreams();
    clearParkedPackets();
    av_packet_unref(&m_Packet);
    av_packet_unref(&m_PendingPacket);
    if (m_FormatCtx)
    {
        avformat_close_input(&m_FormatCtx);
    }
}

void FFmpegSource::clearParkedPackets()
{
    for (AVPacket& parked : m_ParkedPackets)
    {
        av_packet_unref(&parked);
    }
    m_ParkedPackets.clear();
}

void FFmpegSource::teardownStreams()
{
    if (m_Sws)
    {
        sws_freeContext(m_Sws);
        m_Sws = nullptr;
    }
    if (m_Swr)
    {
        swr_free(&m_Swr);
        m_Swr = nullptr;
    }
    if (m_VideoCodec)
    {
        avcodec_free_context(&m_VideoCodec);
    }
    if (m_AudioCodec)
    {
        avcodec_free_context(&m_AudioCodec);
    }
    m_VideoStream = nullptr;
    m_AudioStream = nullptr;
}

bool FFmpegSource::open(const std::string& path, std::string& error)
{
    if (avformat_open_input(&m_FormatCtx, path.c_str(), nullptr, nullptr) < 0)
    {
        error = "cannot open file: " + path;
        return false;
    }
    if (avformat_find_stream_info(m_FormatCtx, nullptr) < 0)
    {
        error = "cannot read stream info: " + path;
        return false;
    }

    for (unsigned i = 0; i < m_FormatCtx->nb_streams; ++i)
    {
        AVStream* stream = m_FormatCtx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !m_VideoStream)
        {
            m_VideoStream = stream;
        }
        else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !m_AudioStream)
        {
            m_AudioStream = stream;
        }
    }

    if (!m_VideoStream && !m_AudioStream)
    {
        error = "no decodable video or audio stream in: " + path;
        return false;
    }

    // Populate metadata.
    m_Metadata.hasVideo = m_VideoStream != nullptr;
    m_Metadata.hasAudio = m_AudioStream != nullptr;

    if (m_VideoStream)
    {
        m_Metadata.width = static_cast<std::uint32_t>(m_VideoStream->codecpar->width);
        m_Metadata.height = static_cast<std::uint32_t>(m_VideoStream->codecpar->height);
        m_Metadata.frameRate = frameRateFromRational(m_VideoStream->avg_frame_rate);
        if (m_Metadata.frameRate == FrameRate::fps30() &&
            m_VideoStream->r_frame_rate.num > 0)
        {
            m_Metadata.frameRate = frameRateFromRational(m_VideoStream->r_frame_rate);
        }
    }
    if (m_AudioStream)
    {
        m_Metadata.audioChannels =
            static_cast<std::uint32_t>(m_AudioStream->codecpar->ch_layout.nb_channels);
        m_Metadata.audioSampleRate = m_AudioStream->codecpar->sample_rate;
    }
    fillCodecDetails(m_Metadata, m_FormatCtx, m_VideoStream, m_AudioStream);

    // Duration: container first, then stream, then estimate.
    if (m_FormatCtx->duration != AV_NOPTS_VALUE)
    {
        m_Metadata.duration = Time::fromMicroseconds(m_FormatCtx->duration);
    }
    else if (m_VideoStream && m_VideoStream->duration != AV_NOPTS_VALUE)
    {
        m_Metadata.duration = toTime(m_VideoStream->duration, m_VideoStream->time_base);
    }
    else if (m_AudioStream && m_AudioStream->duration != AV_NOPTS_VALUE)
    {
        m_Metadata.duration = toTime(m_AudioStream->duration, m_AudioStream->time_base);
    }

    // Classify images: a video stream of image codecs that represents a
    // single still frame (see isSingleFrameImageStream).
    if (m_VideoStream && isImageCodec(m_VideoStream->codecpar->codec_id) &&
        isSingleFrameImageStream(m_FormatCtx, m_VideoStream, m_Metadata.duration,
                                 m_Metadata.frameRate))
    {
        m_Metadata.type = MediaType::Image;
    }
    else if (m_VideoStream)
    {
        m_Metadata.type = MediaType::Video;
    }
    else
    {
        m_Metadata.type = MediaType::Audio;
    }

    if (!setupVideo(error))
    {
        return false;
    }
    if (!setupAudio(error))
    {
        return false;
    }
    return true;
}

bool FFmpegSource::setupVideo(std::string& error)
{
    if (!m_VideoStream)
    {
        return true; // nothing to do
    }

    const AVCodecDescriptor* desc = avcodec_descriptor_get(m_VideoStream->codecpar->codec_id);
    const AVCodec* codec = avcodec_find_decoder(m_VideoStream->codecpar->codec_id);
    if (!codec)
    {
        error = std::string("no decoder for video codec ") +
                (desc ? desc->name : "unknown");
        return false;
    }

    m_VideoCodec = avcodec_alloc_context3(codec);
    if (!m_VideoCodec)
    {
        error = "avcodec_alloc_context3 failed";
        return false;
    }
    if (avcodec_parameters_to_context(m_VideoCodec, m_VideoStream->codecpar) < 0)
    {
        error = "avcodec_parameters_to_context failed";
        return false;
    }
    m_VideoCodec->thread_count = 0; // auto
    if (avcodec_open2(m_VideoCodec, codec, nullptr) < 0)
    {
        error = "avcodec_open2 failed for video";
        return false;
    }
    // The resolved (auto) thread count drives the in-flight cap; see
    // kInFlightReorderMargin.
    m_VideoInFlightCap = m_VideoCodec->thread_count + kInFlightReorderMargin;

    m_Sws = sws_getContext(m_VideoCodec->width, m_VideoCodec->height,
                           m_VideoCodec->pix_fmt, m_VideoCodec->width, m_VideoCodec->height,
                           AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_Sws)
    {
        error = "sws_getContext failed";
        return false;
    }
    m_SwsOutWidth = m_VideoCodec->width;
    m_SwsOutHeight = m_VideoCodec->height;
    return true;
}

bool FFmpegSource::setupAudio(std::string& error)
{
    if (!m_AudioStream)
    {
        return true;
    }

    const AVCodec* codec = avcodec_find_decoder(m_AudioStream->codecpar->codec_id);
    if (!codec)
    {
        error = "no decoder for audio codec";
        return false;
    }

    m_AudioCodec = avcodec_alloc_context3(codec);
    if (!m_AudioCodec)
    {
        error = "avcodec_alloc_context3 failed";
        return false;
    }
    if (avcodec_parameters_to_context(m_AudioCodec, m_AudioStream->codecpar) < 0)
    {
        error = "avcodec_parameters_to_context failed";
        return false;
    }
    if (avcodec_open2(m_AudioCodec, codec, nullptr) < 0)
    {
        error = "avcodec_open2 failed for audio";
        return false;
    }
    m_AudioInFlightCap = m_AudioCodec->thread_count + kInFlightReorderMargin;

    // Convert to interleaved float32, preserving channels and rate; the
    // mixer handles final conversion to the output device format.
    if (swr_alloc_set_opts2(&m_Swr, &m_AudioCodec->ch_layout, AV_SAMPLE_FMT_FLT,
                            m_AudioCodec->sample_rate, &m_AudioCodec->ch_layout,
                            m_AudioCodec->sample_fmt, m_AudioCodec->sample_rate, 0, nullptr) < 0)
    {
        error = "swr_alloc_set_opts2 failed";
        return false;
    }
    if (swr_init(m_Swr) < 0)
    {
        error = "swr_init failed";
        return false;
    }
    return true;
}

void FFmpegSource::flushDecoders()
{
    m_VideoQueue.clear();
    m_AudioQueue.clear();
    clearParkedPackets();
    // A pending packet belongs to the pre-seek stream position; drop it.
    av_packet_unref(&m_PendingPacket);
    m_PendingPacket = {};
    m_HavePendingPacket = false;
    m_VideoFinished = 0;
    m_AudioFinished = 0;
    m_VideoInFlight = 0;
    m_AudioInFlight = 0;

    if (m_VideoCodec)
    {
        avcodec_flush_buffers(m_VideoCodec);
    }
    if (m_AudioCodec)
    {
        avcodec_flush_buffers(m_AudioCodec);
    }
}

bool FFmpegSource::seek(const Time& t)
{
    const int64_t targetUs = t.toMicroseconds();

    if (m_VideoStream)
    {
        const int64_t ts = av_rescale_q(targetUs, AVRational{1, 1000000},
                                        m_VideoStream->time_base);
        const int ret =
            av_seek_frame(m_FormatCtx, m_VideoStream->index, ts, AVSEEK_FLAG_BACKWARD);
        Logger::instance().debug("seek: video av_seek_frame t={}us ret={}", targetUs, ret);
    }
    if (m_AudioStream)
    {
        const int64_t ts = av_rescale_q(targetUs, AVRational{1, 1000000},
                                        m_AudioStream->time_base);
        const int ret =
            av_seek_frame(m_FormatCtx, m_AudioStream->index, ts, AVSEEK_FLAG_BACKWARD);
        Logger::instance().debug("seek: audio av_seek_frame t={}us ret={}", targetUs, ret);
    }
    flushDecoders();
    m_HaveLastVideoTime = false;
    return true;
}

namespace
{

uint64_t nowMilliseconds()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

void FFmpegSource::parkPacket()
{
    if (m_ParkedPackets.size() >= kMaxParkedPackets)
    {
        // Memory bound against pathological containers; dropping the oldest
        // parked packet is the only failure mode (a desync it will not
        // recover from anyway).
        av_packet_unref(&m_ParkedPackets.front());
        m_ParkedPackets.pop_front();
        Logger::instance().debug("packet parking lot full: dropped oldest parked packet");
    }
    AVPacket parked = {};
    av_packet_ref(&parked, &m_Packet);
    m_ParkedPackets.push_back(std::move(parked));
    av_packet_unref(&m_Packet);
}

int FFmpegSource::nextPacketForStream(int streamIndex, AVPacket& out, uint64_t deadlineMs)
{
    // A packet the decoder refused earlier comes before anything newly read
    // (it is the most recent packet of its stream).
    if (m_HavePendingPacket && m_PendingPacket.stream_index == streamIndex)
    {
        av_packet_move_ref(&out, &m_PendingPacket);
        m_HavePendingPacket = false;
        return kPacketGot;
    }
    // The demuxer work of this call is characterized (packets read, elapsed
    // time, parking-lot state) so a deadline/EOF failure below can be
    // explained from the log.
    const uint64_t startMs = nowMilliseconds();
    int readCount = 0;
    for (;;)
    {
        // The parking lot first: take the first packet of our stream, where
        // ever it sits. Removing one element of our stream from a file-order
        // FIFO keeps every stream's packets in order.
        for (size_t i = 0; i < m_ParkedPackets.size(); ++i)
        {
            if (m_ParkedPackets[i].stream_index == streamIndex)
            {
                av_packet_ref(&out, &m_ParkedPackets[i]);
                av_packet_unref(&m_ParkedPackets[i]);
                m_ParkedPackets.erase(m_ParkedPackets.begin() + static_cast<std::ptrdiff_t>(i));
                return kPacketGot;
            }
        }

        const int ret = av_read_frame(m_FormatCtx, &m_Packet);
        if (ret < 0)
        {
            Logger::instance().info("pkt stream {}: EOF after {} ms ({} read, lot {})",
                                    streamIndex, nowMilliseconds() - startMs, readCount,
                                    m_ParkedPackets.size());
            return kPacketEof;
        }
        ++readCount;
        if (m_Packet.stream_index == streamIndex)
        {
            av_packet_ref(&out, &m_Packet);
            av_packet_unref(&m_Packet);
            return kPacketGot;
        }
        // Other stream's packet: keep it for its pump instead of dropping it.
        parkPacket();

        // No sleep here: every loop iteration consumes one packet from the
        // demuxer (guaranteed progress), so a stall cannot spin; the
        // deadline check below bounds the work. Sleeping 1 ms per crossed
        // packet cost ~100 ms/s of decode time per source.
        if (nowMilliseconds() >= deadlineMs)
        {
            Logger::instance().info("pkt stream {}: deadline after {} ms ({} read, lot {})",
                                    streamIndex, nowMilliseconds() - startMs, readCount,
                                    m_ParkedPackets.size());
            return kPacketDeadline;
        }
    }
}

bool FFmpegSource::pumpVideo(uint64_t deadlineMs)
{
    // Feed the decoder until a frame is queued or the deadline passes:
    // drain completed frames, and keep sending while the decoder's window
    // is not full. The two phases must stay interleaved (not "send twice,
    // then wait"): an H264 stream with B-frames withholds its first output
    // until the whole mini-GOP has been sent (with two B-frames, five
    // packets in DTS order: I, P, B, B, B), so a pump that stops sending
    // before the window is filled waits for a frame that is withheld until
    // packets that never arrive.
    //
    // At most m_VideoInFlightCap packets in flight at any time (thread_count
    // + kInFlightReorderMargin): the cap must stay ABOVE the frame-threaded
    // decoder's input latency (roughly thread_count plus the stream's
    // reorder window) or the decoder withholds all output. It also bounds
    // the EOF flush burst, where every in-flight frame completes at once:
    // the burst lands in m_VideoQueue (capped at kMaxQueuedVideoFrames) and
    // overflow evicts the oldest burst frame. At the tail of a looping file
    // those frames sit inside the predecode skip window (see
    // MediaPump::preDecodeUnlocked), where they are deliberately dropped, so
    // the eviction is harmless.
    for (;;)
    {
        // Drain what the decoder has completed first: a completed frame
        // beats the deadline (it decodes even when it overran the caller's
        // budget slightly).
        AVFrame frame = {};
        for (;;)
        {
            const int recvRet = avcodec_receive_frame(m_VideoCodec, &frame);
            if (recvRet != 0)
            {
                if (recvRet != AVERROR(EAGAIN))
                {
                    Logger::instance().debug("Video receive_frame error: {}",
                                             errorString(recvRet));
                }
                break;
            }
            if (m_VideoInFlight > 0)
            {
                --m_VideoInFlight;
            }
            decodeVideoFrame(frame);
            av_frame_unref(&frame);
        }
        if (!m_VideoQueue.empty())
        {
            return true;
        }
        if (m_VideoFinished)
        {
            return false;
        }
        if (nowMilliseconds() >= deadlineMs)
        {
            return false;
        }
        if (m_VideoInFlight >= m_VideoInFlightCap)
        {
            // The decoder is ahead of us; give it a moment to complete.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        AVPacket packet = {};
        const int result = nextPacketForStream(m_VideoStream->index, packet, deadlineMs);
        if (result == kPacketDeadline)
        {
            return false;
        }
        if (result == kPacketEof)
        {
            // EOF: flush the decoder to drain its internal buffers.
            avcodec_send_packet(m_VideoCodec, nullptr);
            bool gotMore = false;
            AVFrame flushFrame = {};
            while (avcodec_receive_frame(m_VideoCodec, &flushFrame) == 0)
            {
                gotMore = true;
                if (m_VideoInFlight > 0)
                {
                    --m_VideoInFlight;
                }
                decodeVideoFrame(flushFrame);
                av_frame_unref(&flushFrame);
            }
            if (!gotMore)
            {
                m_VideoFinished = 1;
                // A normal flush yields the still-in-flight frames before
                // this point; the empty flush marks the true end of the
                // stream (kept as a tail-of-pass marker).
                Logger::instance().info("video pump: EOF with empty decoder flush");
            }
            return !m_VideoQueue.empty();
        }

        const int sendRet = avcodec_send_packet(m_VideoCodec, &packet);
        if (sendRet == AVERROR(EAGAIN))
        {
            // The decoder's input buffer is still full (more in flight
            // than the cap anticipates); keep the packet and re-offer it
            // on the next pass of this loop instead of dropping it.
            av_packet_move_ref(&m_PendingPacket, &packet);
            m_HavePendingPacket = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        av_packet_unref(&packet);
        if (sendRet != 0)
        {
            Logger::instance().debug("Video send_packet error: {}", errorString(sendRet));
        }
        else
        {
            ++m_VideoInFlight;
        }
    }
}

bool FFmpegSource::pumpAudio(uint64_t deadlineMs)
{
    // Feed the decoder until a block is queued or the deadline passes (see
    // pumpVideo for why the send and drain phases must stay interleaved).
    // The audio decoder is single-threaded and EAGAIN-limited, so the
    // in-flight cap never binds for it.
    for (;;)
    {
        AVFrame frame = {};
        for (;;)
        {
            const int recvRet = avcodec_receive_frame(m_AudioCodec, &frame);
            if (recvRet != 0)
            {
                if (recvRet != AVERROR(EAGAIN))
                {
                    Logger::instance().debug("Audio receive_frame error: {}",
                                             errorString(recvRet));
                }
                break;
            }
            if (m_AudioInFlight > 0)
            {
                --m_AudioInFlight;
            }
            decodeAudioFrame(frame);
            av_frame_unref(&frame);
        }
        if (!m_AudioQueue.empty())
        {
            return true;
        }
        if (m_AudioFinished)
        {
            return false;
        }
        if (nowMilliseconds() >= deadlineMs)
        {
            return false;
        }
        if (m_AudioInFlight >= m_AudioInFlightCap)
        {
            // The decoder is ahead of us; give it a moment to complete.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        AVPacket packet = {};
        const int result = nextPacketForStream(m_AudioStream->index, packet, deadlineMs);
        if (result == kPacketDeadline)
        {
            return false;
        }
        if (result == kPacketEof)
        {
            // EOF: flush the decoder to drain its internal buffers.
            avcodec_send_packet(m_AudioCodec, nullptr);
            bool gotMore = false;
            AVFrame flushFrame = {};
            while (avcodec_receive_frame(m_AudioCodec, &flushFrame) == 0)
            {
                gotMore = true;
                if (m_AudioInFlight > 0)
                {
                    --m_AudioInFlight;
                }
                decodeAudioFrame(flushFrame);
                av_frame_unref(&flushFrame);
            }
            if (!gotMore)
            {
                m_AudioFinished = 1;
                // A normal flush yields the still-in-flight blocks before
                // this point; the empty flush marks the true end of the
                // stream (kept as a tail-of-pass marker).
                Logger::instance().info("audio pump: EOF with empty decoder flush");
            }
            return !m_AudioQueue.empty();
        }

        const int sendRet = avcodec_send_packet(m_AudioCodec, &packet);
        if (sendRet == AVERROR(EAGAIN))
        {
            // The decoder's input buffer is still full; keep the packet and
            // re-offer it on the next pass of this loop instead of
            // dropping it.
            av_packet_move_ref(&m_PendingPacket, &packet);
            m_HavePendingPacket = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        av_packet_unref(&packet);
        if (sendRet != 0)
        {
            Logger::instance().debug("Audio send_packet error: {}", errorString(sendRet));
        }
        else
        {
            ++m_AudioInFlight;
        }
    }
}

bool FFmpegSource::nextVideoFrame(VideoFrame& out, std::uint32_t timeoutMs)
{
    if (!m_VideoStream)
    {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const uint64_t nowMs =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now.time_since_epoch())
                                  .count());
    const uint64_t deadlineMs = nowMs + timeoutMs;

    if (!pumpVideo(deadlineMs))
    {
        return false;
    }
    out = std::move(m_VideoQueue.front());
    m_VideoQueue.pop_front();
    // A healthy pull advances by one frame step; a jump of more than two
    // steps means frames were skipped somewhere between decoded frames
    // (rare failure indicator, kept at info).
    if (m_HaveLastVideoTime && (out.time - m_LastVideoTime).toSeconds() > 0.033)
    {
        Logger::instance().info(
            "video jump: {} -> {} ({:.3f} s, queue now {})", m_LastVideoTime.toSeconds(),
            out.time.toSeconds(), (out.time - m_LastVideoTime).toSeconds(),
            m_VideoQueue.size());
    }
    m_LastVideoTime = out.time;
    m_HaveLastVideoTime = true;
    return true;
}

bool FFmpegSource::nextAudioFrame(AudioFrame& out, std::uint32_t timeoutMs)
{
    if (!m_AudioStream)
    {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const uint64_t nowMs =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now.time_since_epoch())
                                  .count());
    const uint64_t deadlineMs = nowMs + timeoutMs;

    if (!pumpAudio(deadlineMs))
    {
        return false;
    }
    out = std::move(m_AudioQueue.front());
    m_AudioQueue.pop_front();
    return true;
}

// Helper: decode one video AVFrame into an RGBA VideoFrame and queue it.
void FFmpegSource::setVideoOutputSizeHint(Size2i onScreenSize)
{
    if (!m_VideoCodec || !m_Sws || !onScreenSize.valid())
    {
        return; // no scalable path (yet), or no hint
    }
    // Decode at min(native, on-screen) per axis: the display never shows
    // more pixels than the layer's on-screen size, so decoding the rest
    // wastes CPU (the scale) and upload bandwidth (the texture). Even
    // sizes keep the scaler's fast paths.
    const int nativeW = m_VideoCodec->width;
    const int nativeH = m_VideoCodec->height;
    int w = std::min(nativeW, static_cast<int>(onScreenSize.width)) & ~1;
    int h = std::min(nativeH, static_cast<int>(onScreenSize.height)) & ~1;
    if (w < 2)
    {
        w = 2;
    }
    if (h < 2)
    {
        h = 2;
    }
    if (w == m_SwsOutWidth && h == m_SwsOutHeight)
    {
        return; // idempotent while the hint matches the current output
    }
    SwsContext* sws =
        sws_getContext(nativeW, nativeH, m_VideoCodec->pix_fmt, w, h, AV_PIX_FMT_RGBA,
                       SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws)
    {
        Logger::instance().debug("sws_getContext failed for output size {}x{}", w, h);
        return; // keep the current size
    }
    sws_freeContext(m_Sws);
    m_Sws = sws;
    m_SwsOutWidth = w;
    m_SwsOutHeight = h;
    // Queued frames are at the old size; drop them. The consumer re-decodes
    // its lookahead quickly, and the render path re-creates the texture at
    // the new size (ensureMediaTexture), so the brief gap is the only cost.
    m_VideoQueue.clear();
}

void FFmpegSource::decodeVideoFrame(const AVFrame& frame)
{
    // The scaler may output below the native resolution (the on-screen size
    // hint, see setVideoOutputSizeHint); the frame is sized to the scaler's
    // output, not the decoder's.
    const int outW = m_SwsOutWidth > 0 ? m_SwsOutWidth : frame.width;
    const int outH = m_SwsOutHeight > 0 ? m_SwsOutHeight : frame.height;

    VideoFrame vf;
    vf.width = static_cast<std::uint32_t>(outW);
    vf.height = static_cast<std::uint32_t>(outH);
    vf.time = toTime(frame.pts, m_VideoStream->time_base);
    vf.rgba.resize(static_cast<size_t>(outW) * outH * 4);

    uint8_t* dst[4] = {vf.rgba.data(), nullptr, nullptr, nullptr};
    int dstStride[4] = {outW * 4, 0, 0, 0};
    // sws_scale returns the number of OUTPUT lines scaled (dst height);
    // the input height is frame.height, which differs when the scaler
    // downscales (the on-screen size hint).
    if (sws_scale(m_Sws, frame.data, frame.linesize, 0, frame.height, dst, dstStride) != outH)
    {
        Logger::instance().debug("sws_scale failed for video frame");
        return;
    }

    if (m_VideoQueue.size() >= kMaxQueuedVideoFrames)
    {
        m_VideoQueue.pop_front();
        ++m_VideoQueueDropped;
    }
    m_VideoQueue.push_back(std::move(vf));
}

// Helper: decode one audio AVFrame, resample to interleaved float32, and
// queue the result.
void FFmpegSource::decodeAudioFrame(const AVFrame& frame)
{
    const int inChannels = frame.ch_layout.nb_channels;
    const int outChannels = m_AudioCodec->ch_layout.nb_channels;
    if (inChannels <= 0 || outChannels <= 0)
    {
        return;
    }

    const int maxOutSamples = kAudioBlockFrames;
    std::vector<float> outSamples(static_cast<size_t>(maxOutSamples) * outChannels, 0.f);
    // Output is interleaved float: a single buffer.
    uint8_t* outPlane[1] = {reinterpret_cast<uint8_t*>(outSamples.data())};

    const uint8_t* inData[8] = {};
    for (int i = 0; i < inChannels && i < 8; ++i)
    {
        inData[i] = frame.data[i];
    }
    const int inSampleBytes =
        av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame.format)) > 0
            ? av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame.format))
            : 2;

    int converted = 0;
    int remaining = frame.nb_samples;
    while (remaining > 0 && converted < maxOutSamples)
    {
        const int thisBatch = std::min(remaining, maxOutSamples - converted);
        const int got = swr_convert(m_Swr, outPlane, thisBatch, inData, thisBatch);
        if (got <= 0)
        {
            break;
        }
        converted += got;
        remaining -= got;
        // Interleaved input formats have only plane 0; the rest of the
        // pointers are null and must not be advanced.
        for (int i = 0; i < inChannels && i < 8; ++i)
        {
            if (inData[i])
            {
                inData[i] += got * inSampleBytes;
            }
        }
    }

    if (converted > 0)
    {
        AudioFrame af;
        af.channels = static_cast<std::uint32_t>(outChannels);
        af.sampleRate = m_AudioCodec->sample_rate;
        af.time = toTime(frame.pts, m_AudioStream->time_base);
        af.samples.assign(outSamples.begin(),
                          outSamples.begin() + static_cast<size_t>(converted) * outChannels);
        if (m_AudioQueue.size() >= kMaxQueuedAudioFrames)
        {
            m_AudioQueue.pop_front();
        }
        m_AudioQueue.push_back(std::move(af));
    }
}

std::shared_ptr<IMediaSource> createSource(const std::string& path, std::string& error)
{
    auto source = std::make_shared<FFmpegSource>();
    if (!source->open(path, error))
    {
        return nullptr;
    }
    return source;
}

MediaMetadata probeFile(const std::string& path, std::string& error)
{
    MediaMetadata metadata;
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0)
    {
        error = "cannot open file: " + path;
        return metadata;
    }
    if (avformat_find_stream_info(ctx, nullptr) < 0)
    {
        error = "cannot read stream info: " + path;
        avformat_close_input(&ctx);
        return metadata;
    }

    AVStream* video = nullptr;
    AVStream* audio = nullptr;
    for (unsigned i = 0; i < ctx->nb_streams; ++i)
    {
        AVStream* stream = ctx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !video)
        {
            video = stream;
        }
        else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !audio)
        {
            audio = stream;
        }
    }

    if (!video && !audio)
    {
        error = "no decodable video or audio stream in: " + path;
        avformat_close_input(&ctx);
        return metadata;
    }

    metadata.hasVideo = video != nullptr;
    metadata.hasAudio = audio != nullptr;

    if (video)
    {
        metadata.width = static_cast<std::uint32_t>(video->codecpar->width);
        metadata.height = static_cast<std::uint32_t>(video->codecpar->height);
        metadata.frameRate = frameRateFromRational(video->avg_frame_rate);
        // Same fallback as open(): avg_frame_rate can be 0/unknown while the
        // container still knows the real rate.
        if (metadata.frameRate == FrameRate::fps30() && video->r_frame_rate.num > 0)
        {
            metadata.frameRate = frameRateFromRational(video->r_frame_rate);
        }
    }
    if (audio)
    {
        metadata.audioChannels =
            static_cast<std::uint32_t>(audio->codecpar->ch_layout.nb_channels);
        metadata.audioSampleRate = audio->codecpar->sample_rate;
    }
    fillCodecDetails(metadata, ctx, video, audio);

    if (ctx->duration != AV_NOPTS_VALUE)
    {
        metadata.duration = Time::fromMicroseconds(ctx->duration);
    }
    else if (video && video->duration != AV_NOPTS_VALUE)
    {
        metadata.duration = toTime(video->duration, video->time_base);
    }
    else if (audio && audio->duration != AV_NOPTS_VALUE)
    {
        metadata.duration = toTime(audio->duration, audio->time_base);
    }

    if (video && isImageCodec(video->codecpar->codec_id) &&
        isSingleFrameImageStream(ctx, video, metadata.duration, metadata.frameRate))
    {
        metadata.type = MediaType::Image;
    }
    else if (video)
    {
        metadata.type = MediaType::Video;
    }
    else
    {
        metadata.type = MediaType::Audio;
    }

    avformat_close_input(&ctx);
    return metadata;
}

} // namespace mediamax::ffmpeg
