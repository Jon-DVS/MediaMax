// YUV -> RGB conversion: the CPU reference (VideoConversion.h) and the
// FFmpegSource YUV plane fast path (8-bit and 10-bit 4:2:0, color metadata,
// the swscale fallback for other formats, the thumbnail hint path).

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

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

#include "mediamax/media/MediaThumbnail.h"
#include "mediamax/media/MediaTypes.h"
#include "mediamax/media/VideoConversion.h"

#ifdef MEDIAMAX_HAS_FFMPEG
#include "mediamax/media/FFmpegSupport.h"
#endif

using namespace mediamax;

namespace
{

// Tolerance for comparing float32 results (the conversion is float32 math;
// the expectations below are computed in double).
constexpr double kFloatTol = 1e-4;
// Tolerance for 8-bit output levels (the double-precision expectation vs
// the float32 conversion + 8-bit rounding).
constexpr int kByteTol = 2;

bool nearValue(double a, double b, double tol = kFloatTol)
{
    return std::abs(a - b) <= tol;
}

// Checks that `actual` (an 8-bit level) is within kByteTol of the
// double-precision `expected` level (0..255).
void checkLevel(const char* what, int actual, double expected)
{
    const int e = std::lround(static_cast<float>(expected));
    const bool inRange = actual >= e - kByteTol && actual <= e + kByteTol;
    CHECK_MESSAGE(inRange, "%s: actual %d outside [%d..%d] (expected ~%.2f)", what, actual,
                  e - kByteTol, e + kByteTol, expected);
}

// Appends a 10-bit sample (0..1023) as a little-endian 16-bit pair.
void push10(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

// Reads a little-endian 16-bit sample at pair index i.
std::uint16_t read16(const std::vector<std::uint8_t>& in, std::size_t i)
{
    return static_cast<std::uint16_t>(in[2 * i] | (in[2 * i + 1] << 8));
}

// A YUV420P test frame: w x h (even), constant luma, the U/V planes as given
// (half resolution, row-major). Limited BT.709 by default.
VideoFrame makeYuv420pFrame(std::uint32_t w, std::uint32_t h, std::uint8_t luma,
                            const std::vector<std::uint8_t>& u, const std::vector<std::uint8_t>& v)
{
    VideoFrame frame;
    frame.width = w;
    frame.height = h;
    frame.format = VideoPixelFormat::YUV420P;
    frame.colorSpace = YuvColorSpace::BT709;
    frame.fullRange = false;
    frame.planes.resize(3);
    frame.planes[0].assign(static_cast<std::size_t>(w) * h, luma);
    frame.planes[1] = u;
    frame.planes[2] = v;
    return frame;
}

} // namespace

TEST_SUITE("VideoConversion")
{

TEST_CASE("yuvToRgb: limited-range endpoints")
{
    // Limited range: luma 16 -> black, 235 -> white; chroma 128 is neutral.
    const float y16 = 16.0f / 255.0f;
    const float y235 = 235.0f / 255.0f;
    const float c128 = 128.0f / 255.0f;

    const auto black = yuvToRgb(y16, c128, c128, YuvColorSpace::BT709, false);
    CHECK(nearValue(black[0], 0.0));
    CHECK(nearValue(black[1], 0.0));
    CHECK(nearValue(black[2], 0.0));

    const auto white = yuvToRgb(y235, c128, c128, YuvColorSpace::BT709, false);
    CHECK(nearValue(white[0], 1.0));
    CHECK(nearValue(white[1], 1.0));
    CHECK(nearValue(white[2], 1.0));

    // Neutral luma 128: Y' = (128 - 16) / 219, all channels equal.
    const auto gray = yuvToRgb(c128, c128, c128, YuvColorSpace::BT709, false);
    CHECK(nearValue(gray[0], 0.5114155));
    CHECK(nearValue(gray[1], 0.5114155));
    CHECK(nearValue(gray[2], 0.5114155));
}

TEST_CASE("yuvToRgb: full-range endpoints and clamping")
{
    // Full range: 0 -> black, 1 -> white, chroma 0.5 is neutral.
    const auto black = yuvToRgb(0.0f, 0.5f, 0.5f, YuvColorSpace::BT709, true);
    CHECK(nearValue(black[0], 0.0));
    CHECK(nearValue(black[1], 0.0));
    CHECK(nearValue(black[2], 0.0));

    const auto white = yuvToRgb(1.0f, 0.5f, 0.5f, YuvColorSpace::BT709, true);
    CHECK(nearValue(white[0], 1.0));
    CHECK(nearValue(white[1], 1.0));
    CHECK(nearValue(white[2], 1.0));

    // Saturated chroma: red at luma 0.5 pushes R past 1 (clamped); G drops.
    // Full-range BT.709: Cr' = 1 - 0.5 = 0.5, Cb' = 0.
    const auto red = yuvToRgb(0.5f, 0.5f, 1.0f, YuvColorSpace::BT709, true);
    CHECK(nearValue(red[0], 1.0, 1e-3)); // 0.5 + 1.5748 * 0.5 = 1.2874, clamped
    CHECK(nearValue(red[1], 0.26595, 1e-3)); // 0.5 - 0.4681 * 0.5
    CHECK(nearValue(red[2], 0.5, 1e-3));
}

TEST_CASE("yuvToRgb: per-space matrix coefficients")
{
    // For each matrix (and the Unknown -> BT.709 fallback): luma 0.5 with
    // chroma at +0.25 (full range: cb = cr = 0.75) must apply the ITU
    // coefficients R = Y' + a*Cr', G = Y' - b*Cb' - c*Cr', B = Y' + d*Cb'.
    struct Case
    {
        YuvColorSpace space;
        double a, b, c, d;
    };
    const std::vector<Case> cases = {
        {YuvColorSpace::BT601, 1.402, 0.34414, 0.71414, 1.772},
        {YuvColorSpace::BT709, 1.5748, 0.1873, 0.4681, 1.8556},
        {YuvColorSpace::BT2020, 1.8814, 0.1197, 0.6812, 2.1145},
        {YuvColorSpace::Unknown, 1.5748, 0.1873, 0.4681, 1.8556},
    };
    for (const Case& tc : cases)
    {
        const auto rgb = yuvToRgb(0.5f, 0.75f, 0.75f, tc.space, true);
        CHECK(nearValue(rgb[0], std::min(1.0, 0.5 + 0.25 * tc.a), 1e-3));
        CHECK(nearValue(rgb[1], std::min(1.0, 0.5 - 0.25 * tc.b - 0.25 * tc.c), 1e-3));
        CHECK(nearValue(rgb[2], std::min(1.0, 0.5 + 0.25 * tc.d), 1e-3));
    }

    // Limited vs full must differ for the same stored values (the range
    // scaling), and the matrices must differ for chroma content.
    const auto lim = yuvToRgb(0.5f, 0.7f, 0.3f, YuvColorSpace::BT709, false);
    const auto ful = yuvToRgb(0.5f, 0.7f, 0.3f, YuvColorSpace::BT709, true);
    CHECK(std::abs(lim[0] - ful[0]) > 0.02);
    const auto m601 = yuvToRgb(0.5f, 0.7f, 0.3f, YuvColorSpace::BT601, true);
    CHECK(std::abs(m601[0] - ful[0]) > 0.01);
}

TEST_CASE("yuvChromaCoord: co-sited 4:2:0 mapping")
{
    // Luma (x, y) reads the U/V plane at ((x + 1) / w, (y + 1) / h):
    // even luma positions land exactly on the U/V texel center, odd ones
    // exactly on the boundary between two U/V texels (a 50/50 blend).
    const auto c00 = yuvChromaCoord(0, 0, 64, 36);
    CHECK(nearValue(c00[0], 1.0 / 64.0));
    CHECK(nearValue(c00[1], 1.0 / 36.0));

    const auto cEven = yuvChromaCoord(2, 2, 64, 36);
    CHECK(nearValue(cEven[0], 3.0 / 64.0));
    CHECK(nearValue(cEven[1], 3.0 / 36.0));

    const auto cOdd = yuvChromaCoord(3, 1, 64, 36);
    CHECK(nearValue(cOdd[0], 4.0 / 64.0));
    CHECK(nearValue(cOdd[1], 2.0 / 36.0));

    // The last luma texel maps to the plane edge (1.0); the sampler's edge
    // replicate and the reference's clamped bilinear agree there.
    const auto cEdge = yuvChromaCoord(63, 35, 64, 36);
    CHECK(nearValue(cEdge[0], 1.0));
    CHECK(nearValue(cEdge[1], 1.0));
}

TEST_CASE("scalePlane10To16: exact endpoint mapping")
{
    std::vector<std::uint8_t> in;
    push10(in, 0);
    push10(in, 1);
    push10(in, 512);
    push10(in, 1023);
    std::vector<std::uint8_t> out;
    scalePlane10To16(in, out);

    REQUIRE(out.size() == 8);
    // 1023 -> 65535 exactly; the other samples are sample * 65535 / 1023.
    CHECK(read16(out, 0) == 0);
    CHECK(read16(out, 1) == 1u * 65535u / 1023u);
    CHECK(read16(out, 2) == 512u * 65535u / 1023u);
    CHECK(read16(out, 3) == 65535u);
}

TEST_CASE("convertYuvFrameToRgba: constant frame")
{
    // Constant luma 128, neutral chroma 128 (limited BT.709): every pixel
    // is Y' * 255 = (128 - 16) / 219 * 255 = 130.4 -> 130.
    const std::uint32_t w = 16, h = 16;
    const std::vector<std::uint8_t> u(8 * 8, 128), v(8 * 8, 128);
    const VideoFrame frame = makeYuv420pFrame(w, h, 128, u, v);

    std::vector<std::uint8_t> rgba;
    REQUIRE(convertYuvFrameToRgba(frame, rgba));
    REQUIRE(rgba.size() == static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < w * h; ++i)
    {
        CHECK(rgba[i * 4 + 0] == 130);
        CHECK(rgba[i * 4 + 1] == 130);
        CHECK(rgba[i * 4 + 2] == 130);
        CHECK(rgba[i * 4 + 3] == 255);
    }
}

TEST_CASE("convertYuvFrameToRgba: chroma upsampling at even and odd luma")
{
    // 4x4, full range so the expectations stay simple. Luma 128 -> Y' =
    // 128/255. The U plane (2x2) steps across x: left block 85, right 170;
    // V is neutral. Even luma x reads its block exactly, odd x blends 50/50
    // (the co-sited mapping); y follows the same rule in its own axis.
    const std::uint32_t w = 4, h = 4;
    const std::vector<std::uint8_t> u = {85, 170, 85, 170};
    const std::vector<std::uint8_t> v(4, 128);
    VideoFrame frame = makeYuv420pFrame(w, h, 128, u, v);
    frame.fullRange = true;

    std::vector<std::uint8_t> rgba;
    REQUIRE(convertYuvFrameToRgba(frame, rgba));

    const double yn = 128.0 / 255.0; // full range
    for (int y = 0; y < 4; ++y)
    {
        for (int x = 0; x < 4; ++x)
        {
            // Expected chroma at the co-sited bilinear coordinate (double
            // precision, clamped like the sampler's edge replicate). The U
            // plane is constant along y (both rows {85, 170}), so the
            // vertical blend is a no-op and only the horizontal one applies.
            const double cu = x / 2.0;
            const int u0 = static_cast<int>(cu);
            const double fu = cu - u0;
            const int u1 = std::min(u0 + 1, 1);
            const double cbLevel = ((1 - fu) * u[u0] + fu * u[u1]) / 255.0;
            const double cbn = cbLevel - 0.5;
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            checkLevel("r", rgba[i + 0], (yn + 0.0) * 255.0);
            checkLevel("g", rgba[i + 1], (yn - 0.1873 * cbn) * 255.0);
            checkLevel("b", rgba[i + 2], (yn + 1.8556 * cbn) * 255.0);
        }
    }

    // Structural: U rises across x, so the blue level rises with it (x=2
    // reads the right block exactly, x=1 the blend, x=0 the left block).
    const auto bAt = [&rgba](int x, int y) {
        return rgba[(static_cast<std::size_t>(y) * w + x) * 4 + 2];
    };
    CHECK(bAt(0, 0) < bAt(1, 0));
    CHECK(bAt(1, 0) < bAt(2, 0));
}

TEST_CASE("convertYuvFrameToRgba: 10-bit planes through the scaled storage")
{
    // 10-bit constant frame: luma 512 (of 1023), neutral chroma 512, full
    // range. The scaled storage holds 512 * 65535 / 1023 per sample; the
    // conversion must return ~512/1023 * 255 = 127.4 -> 127 per channel.
    const std::uint32_t w = 16, h = 16;
    std::vector<std::uint8_t> y, c;
    for (std::size_t i = 0; i < w * h; ++i)
    {
        push10(y, 512);
    }
    for (std::size_t i = 0; i < (w / 2) * (h / 2); ++i)
    {
        push10(c, 512);
    }

    VideoFrame frame;
    frame.width = w;
    frame.height = h;
    frame.format = VideoPixelFormat::YUV420P10LE;
    frame.colorSpace = YuvColorSpace::BT709;
    frame.fullRange = true;
    frame.planes.resize(3);
    scalePlane10To16(y, frame.planes[0]);
    scalePlane10To16(c, frame.planes[1]);
    scalePlane10To16(c, frame.planes[2]);

    std::vector<std::uint8_t> rgba;
    REQUIRE(convertYuvFrameToRgba(frame, rgba));
    // 512 * 65535 / 1023 / 65535 * 255 = 127.35 (the scaled-storage
    // quantization is far below the 8-bit rounding).
    for (std::size_t i = 0; i < w * h; ++i)
    {
        checkLevel("r10", rgba[i * 4 + 0], 127.35);
        checkLevel("g10", rgba[i * 4 + 1], 127.35);
        checkLevel("b10", rgba[i * 4 + 2], 127.35);
        CHECK(rgba[i * 4 + 3] == 255);
    }
}

TEST_CASE("convertYuvFrameToRgba: error paths")
{
    const std::uint32_t w = 8, h = 8;
    const std::vector<std::uint8_t> u(16, 128), v(16, 128);
    VideoFrame frame = makeYuv420pFrame(w, h, 128, u, v);
    std::vector<std::uint8_t> rgba;

    // Non-YUV formats are not convertible.
    frame.format = VideoPixelFormat::RGBA;
    CHECK_FALSE(convertYuvFrameToRgba(frame, rgba));
    frame.format = VideoPixelFormat::YUV420P;

    // Odd dimensions break the 4:2:0 contract.
    frame.width = 9;
    CHECK_FALSE(convertYuvFrameToRgba(frame, rgba));
    frame.width = w;

    // Undersized planes are rejected.
    frame.planes[0].resize(1);
    CHECK_FALSE(convertYuvFrameToRgba(frame, rgba));
}

#ifdef MEDIAMAX_HAS_FFMPEG

namespace
{

// Locates the ffmpeg CLI that ships with the fetched FFmpeg build.
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

// Generates one second of the solid color `color` at 64x36 (10 fps) with
// the given codec arguments; returns the file path, or an empty path on
// failure (the caller skips).
std::filesystem::path makeSolidVideo(const std::string& suffix, const std::string& color,
                                     const std::string& codecArgs)
{
    const auto ffmpeg = findFfmpegCli();
    if (ffmpeg.empty())
    {
        return {};
    }
    const auto dir = std::filesystem::temp_directory_path() / "mediamax-yuv-tests";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    static std::atomic<std::uint64_t> counter{0};
    const auto path = dir / (suffix + "-" + std::to_string(counter.fetch_add(1)) + ".mp4");

    const std::string cmd = "\"" + ffmpeg.string() + "\" -hide_banner -loglevel error -y "
        "-f lavfi -i color=c=" + color + ":s=64x36:d=1:r=10 " + codecArgs + " " +
        "\"" + path.string() + "\"";
    if (runCommandLine(cmd) != 0)
    {
        std::filesystem::remove(path);
        return {};
    }
    return path;
}

// One pixel of a 64x36 RGBA buffer (returns the RGBA byte at the channel).
int levelAt(const std::vector<std::uint8_t>& rgba, int x, int y, int channel)
{
    return rgba[(static_cast<std::size_t>(y) * 64 + x) * 4 + channel];
}

} // namespace

TEST_CASE("FFmpegSource: 8-bit yuv420p decodes through the plane fast path")
{
    const auto path = makeSolidVideo("h264", "0x204060",
                                     "-c:v libx264 -preset ultrafast -pix_fmt yuv420p");
    if (path.empty())
    {
        std::fprintf(stderr, "[skip] ffmpeg CLI not available; skipping YUV fast path test\n");
        return;
    }

    std::string error;
    auto source = ffmpeg::createSource(path.string(), error);
    REQUIRE(source != nullptr);

    VideoFrame frame;
    REQUIRE(source->nextVideoFrame(frame, 5000));
    CHECK(frame.width == 64);
    CHECK(frame.height == 36);
    CHECK(frame.format == VideoPixelFormat::YUV420P);
    REQUIRE(frame.planes.size() == 3);
    CHECK(frame.planes[0].size() == static_cast<std::size_t>(64) * 36);
    CHECK(frame.planes[1].size() == static_cast<std::size_t>(32) * 18);
    CHECK(frame.planes[2].size() == static_cast<std::size_t>(32) * 18);
    // The decoder/container reports a concrete matrix (64x36 is SD: the
    // encoder's VUI usually says SMPTE 170M); the range is the video
    // default (limited) unless the container says otherwise.
    CHECK(frame.colorSpace != YuvColorSpace::Unknown);
    CHECK_FALSE(frame.fullRange);

    // The converted pixels must carry the encoded color (a solid color
    // survives the YUV 4:2:0 round trip within a few levels).
    std::vector<std::uint8_t> rgba;
    REQUIRE(convertYuvFrameToRgba(frame, rgba));
    checkLevel("r", levelAt(rgba, 32, 18, 0), 32.0);
    checkLevel("g", levelAt(rgba, 32, 18, 1), 64.0);
    checkLevel("b", levelAt(rgba, 32, 18, 2), 96.0);

    source.reset();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("FFmpegSource: 10-bit yuv420p10le decodes through the plane fast path")
{
    const auto path = makeSolidVideo("hevc", "0x204060",
                                     "-c:v libx265 -preset ultrafast -pix_fmt yuv420p10le");
    if (path.empty())
    {
        std::fprintf(stderr, "[skip] ffmpeg CLI not available; skipping 10-bit YUV test\n");
        return;
    }

    std::string error;
    auto source = ffmpeg::createSource(path.string(), error);
    REQUIRE(source != nullptr);

    VideoFrame frame;
    REQUIRE(source->nextVideoFrame(frame, 5000));
    CHECK(frame.width == 64);
    CHECK(frame.height == 36);
    CHECK(frame.format == VideoPixelFormat::YUV420P10LE);
    REQUIRE(frame.planes.size() == 3);
    CHECK(frame.planes[0].size() == static_cast<std::size_t>(64) * 36 * 2);
    CHECK(frame.planes[1].size() == static_cast<std::size_t>(32) * 18 * 2);
    CHECK(frame.planes[2].size() == static_cast<std::size_t>(32) * 18 * 2);
    CHECK(frame.colorSpace != YuvColorSpace::Unknown);

    std::vector<std::uint8_t> rgba;
    REQUIRE(convertYuvFrameToRgba(frame, rgba));
    checkLevel("r", levelAt(rgba, 32, 18, 0), 32.0);
    checkLevel("g", levelAt(rgba, 32, 18, 1), 64.0);
    checkLevel("b", levelAt(rgba, 32, 18, 2), 96.0);

    source.reset();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("FFmpegSource: non-4:2:0 formats keep the swscale-to-RGBA path")
{
    const auto path = makeSolidVideo("yuv422", "0x204060",
                                     "-c:v libx264 -preset ultrafast -pix_fmt yuv422p");
    if (path.empty())
    {
        std::fprintf(stderr, "[skip] ffmpeg CLI not available; skipping fallback test\n");
        return;
    }

    std::string error;
    auto source = ffmpeg::createSource(path.string(), error);
    REQUIRE(source != nullptr);

    VideoFrame frame;
    REQUIRE(source->nextVideoFrame(frame, 5000));
    CHECK(frame.format == VideoPixelFormat::RGBA);
    REQUIRE(frame.planes.size() == 1);
    CHECK(frame.planes[0].size() == static_cast<std::size_t>(64) * 36 * 4);

    source.reset();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("FFmpegSource: the size hint switches a YUV source to RGBA")
{
    const auto path = makeSolidVideo("hint", "0x204060",
                                     "-c:v libx264 -preset ultrafast -pix_fmt yuv420p");
    if (path.empty())
    {
        std::fprintf(stderr, "[skip] ffmpeg CLI not available; skipping hint test\n");
        return;
    }

    std::string error;

    // Direct hint on a raw source instance: a YUV fast-path source must
    // switch to the swscale path and produce RGBA at the hinted size.
    auto source = ffmpeg::createSource(path.string(), error);
    REQUIRE(source != nullptr);
    source->setVideoOutputSizeHint(Size2i{32, 18});
    VideoFrame frame;
    REQUIRE(source->nextVideoFrame(frame, 5000));
    CHECK(frame.format == VideoPixelFormat::RGBA);
    CHECK(frame.width == 32);
    CHECK(frame.height == 18);
    REQUIRE(frame.planes.size() == 1);
    CHECK(frame.planes[0].size() == static_cast<std::size_t>(32) * 18 * 4);
    source.reset();

    // The thumbnail path (hint + decode + fit) works on the same source:
    // it returns RGBA at the aspect-fit target size.
    const MediaMetadata meta = ffmpeg::probeFile(path.string(), error);
    REQUIRE(meta.valid());
    REQUIRE(meta.hasVideo);
    std::vector<std::uint8_t> rgba;
    int w = 0, h = 0;
    REQUIRE(media::makeThumbnail(ffmpeg::createSource, path.string(), meta, 32, 32, rgba, w, h,
                                 error));
    CHECK(w == 32);
    CHECK(h == 18); // aspect-fit of 64x36 into 32x32
    CHECK(rgba.size() == static_cast<std::size_t>(w) * h * 4);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

#endif // MEDIAMAX_HAS_FFMPEG

} // TEST_SUITE("VideoConversion")
