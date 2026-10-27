// Media: test pattern source determinism, library management, and (when
// built with FFmpeg) real file decoding of a generated BMP.

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

// Included at file scope (not inside a namespace) so the Win32 process API
// stays in the global namespace; only the FFmpeg tests use it.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "mediamax/media/MediaLibrary.h"
#include "mediamax/media/TestPatternSource.h"
#include "mediamax/util/File.h"

#ifdef MEDIAMAX_HAS_FFMPEG
#include "mediamax/media/FFmpegSupport.h"
#endif

using namespace mediamax;

namespace
{

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

} // namespace

TEST_SUITE("Media")
{

TEST_CASE("TestPatternSource: deterministic frames")
{
    TestPatternSource source(64, 32, FrameRate::fps30(), Time::fromSeconds(2.0),
                             TestPatternKind::ColorBars);
    const MediaMetadata meta = source.metadata();
    CHECK(meta.hasVideo);
    CHECK(meta.width == 64);
    CHECK(meta.height == 32);
    CHECK(meta.type == MediaType::Video);

    std::vector<unsigned char> a, b;
    source.renderFrameTo(Time::fromSeconds(0.5), a);
    source.renderFrameTo(Time::fromSeconds(0.5), b);
    CHECK(a == b); // same time -> same pixels

    std::vector<unsigned char> c;
    source.renderFrameTo(Time::fromSeconds(1.5), c);
    CHECK(a.size() == c.size());
    CHECK(a == c); // color bars do not move over time
}

TEST_CASE("TestPatternSource: solid color renders exactly")
{
    TestPatternSource source(16, 8, FrameRate::fps30(), Time::fromSeconds(1.0), TestPatternKind::Solid,
                             {0.5f, 0.25f, 1.f, 1.f});
    std::vector<unsigned char> pixels;
    source.renderFrameTo(Time::zero(), pixels);
    CHECK(pixels.size() == 16 * 8 * 4);
    for (size_t i = 0; i < pixels.size(); i += 4)
    {
        CHECK(pixels[i] == 128);       // 0.5 * 255 + 0.5
        CHECK(pixels[i + 1] == 64);    // 0.25 * 255 + 0.5
        CHECK(pixels[i + 2] == 255);
        CHECK(pixels[i + 3] == 255);
    }
}

TEST_CASE("TestPatternSource: color bars layout")
{
    TestPatternSource source(70, 10, FrameRate::fps30(), Time::fromSeconds(1.0),
                             TestPatternKind::ColorBars);
    std::vector<unsigned char> pixels;
    source.renderFrameTo(Time::zero(), pixels);

    // x=0 is in bar 0 (white): r=g=b=255
    CHECK(pixels[0] == 255);
    CHECK(pixels[1] == 255);
    CHECK(pixels[2] == 255);
    // x=69 is in bar 6 (blue): b=255, r=g=0
    const size_t idx69 = 69 * 4;
    CHECK(pixels[idx69] == 0);
    CHECK(pixels[idx69 + 1] == 0);
    CHECK(pixels[idx69 + 2] == 255);
}

TEST_CASE("TestPatternSource: frame stepping respects EOF")
{
    TestPatternSource source(8, 8, FrameRate::fps30(), Time::fromSeconds(0.1),
                             TestPatternKind::Solid);
    VideoFrame frame;
    CHECK(source.nextVideoFrame(frame, 0));
    CHECK(source.nextVideoFrame(frame, 0));
    CHECK(source.nextVideoFrame(frame, 0));
    CHECK_FALSE(source.nextVideoFrame(frame, 0)); // EOF
}

TEST_CASE("TestPatternSource: seek rewinds frame production")
{
    TestPatternSource source(8, 8, FrameRate::fps30(), Time::fromSeconds(1.0),
                             TestPatternKind::Solid);
    VideoFrame frame;
    CHECK(source.nextVideoFrame(frame, 0));
    CHECK(source.nextVideoFrame(frame, 0));
    CHECK(source.seek(Time::zero()));
    VideoFrame frame2;
    CHECK(source.nextVideoFrame(frame2, 0));
    CHECK(frame2.time == Time::zero());
}

TEST_CASE("MediaLibrary: load, find, unload")
{
    MediaLibrary library;
    library.setSourceFactory(
        [](const std::string& path, std::string& error) -> std::shared_ptr<IMediaSource> {
            if (path.empty())
            {
                error = "empty path";
                return nullptr;
            }
            return std::make_shared<TestPatternSource>(16, 16, FrameRate::fps30(),
                                                       Time::fromSeconds(1.0),
                                                       TestPatternKind::Solid);
        });

    std::string error;
    const Id a = library.load("a.mp4", error);
    const Id b = library.load("b.mp4", error);
    CHECK(a != kInvalidId);
    CHECK(b != kInvalidId);
    CHECK(a != b);
    CHECK(library.count() == 2);

    const auto* asset = *library.find(a);
    CHECK(asset->path == "a.mp4");
    CHECK(asset->metadata.hasVideo);

    CHECK(library.unload(a));
    CHECK_FALSE(library.unload(a));
    CHECK(library.count() == 1);
    CHECK_FALSE(library.find(a).has_value());
}

TEST_CASE("MediaLibrary: load failure returns invalid id and error")
{
    MediaLibrary library;
    library.setSourceFactory(
        [](const std::string&, std::string& error) -> std::shared_ptr<IMediaSource> {
            error = "boom";
            return nullptr;
        });
    std::string error;
    CHECK(library.load("x.mp4", error) == kInvalidId);
    CHECK(error == "boom");
}

TEST_CASE("MediaLibrary: no factory is a clean error")
{
    MediaLibrary library;
    std::string error;
    CHECK(library.load("x.mp4", error) == kInvalidId);
    CHECK_FALSE(error.empty());
}

TEST_CASE("MediaLibrary: the same file loads once (dedup by path)")
{
    std::atomic<int> factoryCalls{0};
    MediaLibrary library;
    library.setSourceFactory(
        [&factoryCalls](const std::string& path, std::string& error) -> std::shared_ptr<IMediaSource> {
            ++factoryCalls;
            if (path.empty())
            {
                error = "empty path";
                return nullptr;
            }
            return std::make_shared<TestPatternSource>(16, 16, FrameRate::fps30(),
                                                       Time::fromSeconds(1.0),
                                                       TestPatternKind::Solid);
        });

    std::string error;
    const Id a = library.load("clip.mp4", error);
    const Id b = library.load("clip.mp4", error);
    const Id c = library.load("./clip.mp4", error); // same file, spelled differently

    CHECK(a != kInvalidId);
    CHECK(b == a);
    CHECK(c == a);
    CHECK(factoryCalls.load() == 1); // the factory runs once for the file
    CHECK(library.count() == 1);
    // The first-registered spelling is kept in the asset record.
    CHECK((*library.find(a))->path == "clip.mp4");

    CHECK(library.unload(a));
    // After unload the file may be loaded again (fresh id, factory re-runs).
    const Id d = library.load("clip.mp4", error);
    CHECK(d != kInvalidId);
    CHECK(d != a);
    CHECK(factoryCalls.load() == 2);
}

TEST_CASE("MediaLibrary: a changed file is reloaded with the same id")
{
    // Uses a real file on disk: the change fingerprint is (size, last write
    // time), so both are varied explicitly to make the test deterministic.
    const auto path = std::filesystem::temp_directory_path() / "mediamax_dedup_test.bin";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    auto write = [&](const std::string& content) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
    };
    write("v1");

    std::atomic<int> factoryCalls{0};
    MediaLibrary library;
    library.setSourceFactory(
        [&factoryCalls](const std::string&, std::string& error) -> std::shared_ptr<IMediaSource> {
            ++factoryCalls;
            (void)error;
            return std::make_shared<TestPatternSource>(16, 16, FrameRate::fps30(),
                                                       Time::fromSeconds(1.0),
                                                       TestPatternKind::Solid);
        });

    std::string error;
    const Id a = library.load(std::string(path.string()), error);
    CHECK(a != kInvalidId);
    CHECK(factoryCalls.load() == 1);

    // Unchanged re-load: same id, no factory call.
    CHECK(library.load(std::string(path.string()), error) == a);
    CHECK(factoryCalls.load() == 1);

    // The file changes on disk (size + explicitly distinct mtime).
    write("v2-is-longer");
    std::filesystem::last_write_time(path,
                                     std::filesystem::file_time_type::clock::now() +
                                         std::chrono::hours(1),
                                     ec);
    REQUIRE_FALSE(ec);

    CHECK(library.load(std::string(path.string()), error) == a); // same id...
    CHECK(factoryCalls.load() == 2); // ...but the source was rebuilt
    CHECK(library.count() == 1);

    std::filesystem::remove(path, ec);
}

TEST_CASE("MediaLibrary: a changed file that fails to decode keeps the old source")
{
    const auto path = std::filesystem::temp_directory_path() / "mediamax_dedup_test_fail.bin";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "v1";
    }

    int factoryCalls = 0;
    bool failNext = false;
    MediaLibrary library;
    library.setSourceFactory(
        [&](const std::string&, std::string& error) -> std::shared_ptr<IMediaSource> {
            ++factoryCalls;
            if (failNext)
            {
                error = "decode failed";
                return nullptr;
            }
            return std::make_shared<TestPatternSource>(16, 16, FrameRate::fps30(),
                                                       Time::fromSeconds(1.0),
                                                       TestPatternKind::Solid);
        });

    std::string error;
    const Id a = library.load(std::string(path.string()), error);
    CHECK(a != kInvalidId);
    REQUIRE(library.find(a).has_value());
    const auto* before = *library.find(a);

    // File changes, but the new content cannot be decoded.
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out << "-changed";
    }
    std::filesystem::last_write_time(path,
                                     std::filesystem::file_time_type::clock::now() +
                                         std::chrono::hours(2),
                                     ec);
    failNext = true;
    error.clear();
    CHECK(library.load(std::string(path.string()), error) == kInvalidId);
    CHECK(error == "decode failed");

    // The asset is intact: same id, old source still there.
    REQUIRE(library.find(a).has_value());
    const auto* after = *library.find(a);
    CHECK(after->source == before->source);

    // A subsequent load of the (still changed) file retries the reload.
    failNext = false;
    CHECK(library.load(std::string(path.string()), error) == a);
    CHECK(factoryCalls == 3);

    std::filesystem::remove(path, ec);
}

#ifdef MEDIAMAX_HAS_FFMPEG
namespace
{

// Locates the ffmpeg CLI that ships with the fetched FFmpeg build (used to
// generate real media files). Returns an empty path when it is not present.
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
// quoted executable/output paths ("system cannot find the path specified").
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

} // namespace

TEST_CASE("FFmpeg: probe and decode a generated BMP (image media)")
{
    const auto path = std::filesystem::temp_directory_path() / "mediamax_test.bmp";
    const int w = 8, h = 8;
    CHECK(writeTestBmp(path, w, h, [](int x, int y, std::uint8_t& r, std::uint8_t& g,
                                      std::uint8_t& b) {
        r = static_cast<std::uint8_t>(x * 32);
        g = static_cast<std::uint8_t>(y * 32);
        b = 128;
    }));

    std::string error;
    const MediaMetadata meta = ffmpeg::probeFile(path.string(), error);
    REQUIRE(meta.valid());
    CHECK(meta.type == MediaType::Image);
    CHECK(meta.hasVideo);
    CHECK_FALSE(meta.hasAudio);
    CHECK(meta.width == 8);
    CHECK(meta.height == 8);
    // Codec details are reported for images too (the BMP pipe demuxer knows
    // its codec).
    CHECK(meta.videoCodec == "bmp");
    CHECK(meta.videoBitDepth == 8);

    auto source = ffmpeg::createSource(path.string(), error);
    REQUIRE(source != nullptr);

    VideoFrame frame;
    REQUIRE(source->nextVideoFrame(frame, 1000));
    CHECK(frame.width == 8);
    CHECK(frame.height == 8);

    // Verify a few decoded pixels (BGR in the file -> RGBA after decode).
    auto pixelAt = [&frame](int x, int y) -> std::array<std::uint8_t, 4> {
        const size_t i = (static_cast<size_t>(y) * frame.width + x) * 4;
        return {frame.rgba[i], frame.rgba[i + 1], frame.rgba[i + 2], frame.rgba[i + 3]};
    };
    const auto p00 = pixelAt(0, 0);
    CHECK(p00[0] == 0);     // r = 0*32
    CHECK(p00[1] == 0);     // g = 0*32
    CHECK(p00[2] == 128);   // b
    CHECK(p00[3] == 255);

    const auto p77 = pixelAt(7, 7);
    CHECK(p77[0] == 224); // 7*32
    CHECK(p77[1] == 224); // 7*32
    CHECK(p77[2] == 128);

    // Second read is EOF for an image.
    CHECK_FALSE(source->nextVideoFrame(frame, 100));

    // Release the source before deleting the file: on Windows the
    // AVFormatContext keeps the file handle open for the source's lifetime.
    source.reset();
    std::filesystem::remove(path);
}

TEST_CASE("FFmpeg: probe reports a clean error for missing files")
{
    std::string error;
    const MediaMetadata meta =
        ffmpeg::probeFile("/nonexistent/mediamax/missing.mp4", error);
    CHECK_FALSE(meta.valid());
    CHECK_FALSE(error.empty());
}

TEST_CASE("FFmpeg: probe reports codec details (codec, colorspace, depth, scan, rate)")
{
    const auto ffmpeg = findFfmpegCli();
    if (ffmpeg.empty())
    {
        std::fprintf(stderr,
                     "[skip] ffmpeg CLI not available; skipping codec details test\n");
        return;
    }

    const auto dir = std::filesystem::temp_directory_path() / "mediamax-tests";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    static std::atomic<std::uint64_t> counter{0};
    const auto path =
        dir / ("codec-details-" + std::to_string(counter.fetch_add(1)) + ".mp4");

    // 1 s of 64x36@30 h264 (yuv420p, progressive) with an aac tone - the
    // codec-level fields must come back filled from the container/codec.
    const std::string cmd = "\"" + ffmpeg.string() + "\" -hide_banner -loglevel error -y "
        "-f lavfi -i testsrc=duration=1:size=64x36:rate=30 "
        "-f lavfi -i sine=frequency=440:sample_rate=48000:duration=1 "
        "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac " +
        "\"" + path.string() + "\"";
    const int rc = runCommandLine(cmd);
    if (rc != 0)
    {
        std::fprintf(stderr, "[skip] ffmpeg failed to generate the test file (rc=%d)\n", rc);
        std::filesystem::remove(path);
        return;
    }

    std::string error;
    const MediaMetadata meta = ffmpeg::probeFile(path.string(), error);
    REQUIRE(meta.valid());
    CHECK(meta.type == MediaType::Video);
    CHECK(meta.videoCodec == "h264");
    CHECK(meta.audioCodec == "aac");
    CHECK(meta.videoBitDepth == 8); // yuv420p
    CHECK(!meta.videoColorspace.empty());
    CHECK(meta.videoScanType == "progressive");
    CHECK(meta.videoBitRate > 0);

    std::filesystem::remove(path);
}

TEST_CASE("FFmpeg: alternating video/audio pulls keep the audio continuous (A/V sync)")
{
    // Regression: the video and audio pumps share one demuxer, and the
    // video pump used to drop every audio packet it crossed while filling
    // the video lookahead. MediaPump pulls video in bursts and audio in
    // bursts, so the drops became holes in the decoded audio and A/V
    // drifted out of sync. The packet parking lot must keep the audio
    // unbroken under the same pull pattern.
    const auto ffmpeg = findFfmpegCli();
    if (ffmpeg.empty())
    {
        std::fprintf(stderr, "[skip] ffmpeg CLI not available; skipping A/V interleave test\n");
        return;
    }

    const auto dir = std::filesystem::temp_directory_path() / "mediamax-tests";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    static std::atomic<std::uint64_t> counter{0};
    const auto path = dir / ("av-sync-" + std::to_string(counter.fetch_add(1)) + ".mp4");

    // 2 s of 64x36@30 test pattern with a mono 440 Hz tone at 48 kHz. mp4
    // interleaves the streams, so the demuxer hands the packets out
    // alternately.
    const std::string cmd = "\"" + ffmpeg.string() + "\" -hide_banner -loglevel error -y "
        "-f lavfi -i testsrc=duration=2:size=64x36:rate=30 "
        "-f lavfi -i sine=frequency=440:sample_rate=48000:duration=2 "
        "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac " +
        "\"" + path.string() + "\"";
    const int rc = runCommandLine(cmd);
    if (rc != 0)
    {
        std::fprintf(stderr, "[skip] ffmpeg failed to generate the test file (rc=%d)\n", rc);
        std::filesystem::remove(path);
        return;
    }

    std::string error;
    auto source = ffmpeg::createSource(path.string(), error);
    REQUIRE(source != nullptr);
    const MediaMetadata meta = source->metadata();
    REQUIRE(meta.hasVideo);
    REQUIRE(meta.hasAudio);
    const double sampleRate = meta.audioSampleRate > 0.0 ? meta.audioSampleRate : 48000.0;

    // Pull the way MediaPump does: a video burst, then an audio burst,
    // repeated until both streams are exhausted.
    VideoFrame vf;
    AudioFrame af;
    int videoFrames = 0;
    double audioSeconds = 0.0;
    double maxGapSeconds = 0.0;
    double lastAudioEnd = 0.0;
    bool haveAudio = false;
    for (int pass = 0; pass < 500; ++pass)
    {
        int gotVideo = 0;
        for (int i = 0; i < 10; ++i)
        {
            if (source->nextVideoFrame(vf, 100))
            {
                ++gotVideo;
                ++videoFrames;
            }
            else
            {
                break;
            }
        }
        int gotAudio = 0;
        for (int i = 0; i < 20; ++i)
        {
            if (!source->nextAudioFrame(af, 100))
            {
                break;
            }
            if (af.channels == 0 || af.samples.empty())
            {
                break;
            }
            ++gotAudio;
            const double start = af.time.toSeconds();
            const double duration =
                static_cast<double>(af.samples.size() / af.channels) / sampleRate;
            if (haveAudio)
            {
                const double gap = start - lastAudioEnd;
                if (gap > maxGapSeconds)
                {
                    maxGapSeconds = gap;
                }
            }
            else
            {
                haveAudio = true;
            }
            lastAudioEnd = start + duration;
            audioSeconds += duration;
        }
        if (gotVideo == 0 && gotAudio == 0)
        {
            break; // both streams exhausted
        }
    }

    source.reset(); // Windows: the format context holds the file handle
    std::filesystem::remove(path);

    CHECK(videoFrames > 0);
    REQUIRE(haveAudio);
    // The full 2 s of audio must have been decoded (tolerating encoder
    // head/tail), and no single hole may exceed 10 ms - a dropped 1024
    // sample AAC packet at 48 kHz is a ~21 ms hole.
    CHECK(audioSeconds > 1.8);
    CHECK(maxGapSeconds < 0.010);
}
#endif // MEDIAMAX_HAS_FFMPEG

} // TEST_SUITE("Media")
