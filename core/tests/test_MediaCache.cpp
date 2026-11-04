// The media cache helpers: disk-root normalization, the cache directory,
// the cache key, and the copy of a cache across disks.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "mediamax/media/MediaCache.h"

using namespace mediamax;
using namespace mediamax::media;

namespace
{

// A unique temp directory acting as a stand-in disk root (the helpers work
// with any existing directory; the disk-root shape is validated separately).
struct TempRoots
{
    std::filesystem::path from;
    std::filesystem::path to;

    TempRoots()
    {
        static std::atomic<std::uint64_t> counter{0};
        const auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        const auto base = std::filesystem::temp_directory_path() / "mediamax-tests" /
                          ("cachetest-" + std::to_string(ns) + "-" +
                           std::to_string(counter.fetch_add(1)));
        from = base / "from";
        to = base / "to";
        std::error_code ec;
        std::filesystem::create_directories(from, ec);
        std::filesystem::create_directories(to, ec);
    }

    ~TempRoots()
    {
        std::error_code ec;
        std::filesystem::remove_all(from, ec);
        std::filesystem::remove_all(to, ec);
    }

    static void writeFile(const std::filesystem::path& path, const std::string& contents)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
    }
};

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

} // namespace

TEST_SUITE("MediaCache")
{

TEST_CASE("normalizeDiskRoot accepts the spellings the UI produces")
{
#if defined(_WIN32)
    CHECK(normalizeDiskRoot("C")->string() == "C:\\");
    CHECK(normalizeDiskRoot("C:")->string() == "C:\\");
    CHECK(normalizeDiskRoot("c:\\")->string() == "C:\\");
    CHECK(normalizeDiskRoot("C:/")->string() == "C:\\");
    CHECK(normalizeDiskRoot("  D:  ")->string() == "D:\\");
    CHECK(normalizeDiskRoot("Z:") != std::nullopt);
    // Not roots: path components beyond the drive root, missing colon, a
    // number where the letter belongs.
    CHECK(normalizeDiskRoot("C:\\Users") == std::nullopt);
    CHECK(normalizeDiskRoot("C:Users") == std::nullopt);
    CHECK(normalizeDiskRoot("C\\") == std::nullopt);
    CHECK(normalizeDiskRoot("CD:") == std::nullopt);
    CHECK(normalizeDiskRoot("1:") == std::nullopt);
    CHECK(normalizeDiskRoot("") == std::nullopt);
    CHECK(normalizeDiskRoot("   ") == std::nullopt);
#else
    CHECK(normalizeDiskRoot("/")->string() == "/");
    CHECK(normalizeDiskRoot("") == std::nullopt);
    CHECK(normalizeDiskRoot("/home") == std::nullopt);
#endif
}

TEST_CASE("cacheDirFor appends the cache folder")
{
#if defined(_WIN32)
    const auto dir = cacheDirFor("C:\\");
    CHECK(dir.filename().string() == kCacheFolder);
    CHECK(dir.parent_path().string() == "C:\\");
#else
    const auto dir = cacheDirFor("/");
    CHECK(dir.filename().string() == kCacheFolder);
    CHECK(dir.parent_path().string() == "/");
#endif
}

TEST_CASE("cacheKeyFor mirrors the show layout and names absolute paths")
{
    CHECK(cacheKeyFor("media/colorbars.mp4") == "media/colorbars.mp4");
    CHECK(cacheKeyFor("logo.png") == "logo.png");
#if defined(_WIN32)
    CHECK(cacheKeyFor("C:\\shows\\media\\clip.mp4") == "clip.mp4");
#else
    CHECK(cacheKeyFor("/shows/media/clip.mp4") == "clip.mp4");
#endif
}

TEST_CASE("ensureCacheDir creates the folder inside an existing root")
{
    TempRoots roots;
    std::string error;
    REQUIRE(ensureCacheDir(roots.from, error));
    CHECK(std::filesystem::is_directory(roots.from / kCacheFolder));

    // A root that is not there is the missing-disk case: a clean failure.
    CHECK_FALSE(ensureCacheDir(roots.from / "no-such-root", error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("cachedPathFor finds files under the cache layout")
{
    TempRoots roots;
    const auto cache = roots.from / kCacheFolder;
    TempRoots::writeFile(cache / "media" / "a.mp4", "A");
    TempRoots::writeFile(cache / "a.png", "P");

    CHECK(cachedPathFor(roots.from, "media/a.mp4") != std::nullopt);
    CHECK(cachedPathFor(roots.from, "a.png") != std::nullopt);
    CHECK(cachedPathFor(roots.from, "media/missing.mp4") == std::nullopt);
    CHECK(cachedPathFor(roots.from, "elsewhere") == std::nullopt);
    // Absolute media paths resolve by name.
#if defined(_WIN32)
    CHECK(cachedPathFor(roots.from, "C:\\control\\shows\\a.png") != std::nullopt);
#else
    CHECK(cachedPathFor(roots.from, "/control/shows/a.png") != std::nullopt);
#endif
    // A root without the cache folder has nothing.
    CHECK(cachedPathFor(roots.to, "a.png") == std::nullopt);
}

TEST_CASE("copyCacheTo moves the cache tree across disks with progress")
{
    TempRoots roots;
    const auto fromCache = roots.from / kCacheFolder;
    const auto toCache = roots.to / kCacheFolder;
    TempRoots::writeFile(fromCache / "a.mp4", "AAA");
    TempRoots::writeFile(fromCache / "sub" / "b.png", "BB");
    const std::uint64_t totalExpected = 5; // 3 + 2

    // The destination cache directory must exist first (the app ensures
    // it for the new disk).
    std::string error;
    REQUIRE(ensureCacheDir(roots.to, error));

    std::vector<std::pair<std::uint64_t, std::uint64_t>> progress;
    REQUIRE(copyCacheTo(
        roots.from, roots.to,
        [&](const CacheCopyProgress& p) { progress.emplace_back(p.bytesCopied, p.bytesTotal); },
        error));
    CHECK(readAll(toCache / "a.mp4") == "AAA");
    CHECK(readAll(toCache / "sub" / "b.png") == "BB");

    // Progress: the total up front, then cumulative bytes after each file,
    // ending at the total.
    REQUIRE(progress.size() >= 2);
    CHECK(progress.front().first == 0);
    CHECK(progress.front().second == totalExpected);
    CHECK(progress.back().first == totalExpected);
    CHECK(progress.back().second == totalExpected);

    // Re-running the same change overwrites in place (idempotent).
    TempRoots::writeFile(fromCache / "a.mp4", "AAAA");
    REQUIRE(copyCacheTo(roots.from, roots.to, {}, error));
    CHECK(readAll(toCache / "a.mp4") == "AAAA");
}

TEST_CASE("copyCacheTo with an empty or missing source is a zero success")
{
    TempRoots roots;
    std::string error;
    // No source cache at all (first-time disk change).
    CacheCopyProgress last{0, 0};
    REQUIRE(copyCacheTo(roots.from, roots.to,
                        [&](const CacheCopyProgress& p) { last = p; }, error));
    CHECK(last.bytesTotal == 0);

    // A source cache directory that is there but empty.
    std::error_code ec;
    std::filesystem::create_directories(roots.from / kCacheFolder, ec);
    REQUIRE(copyCacheTo(roots.from, roots.to,
                        [&](const CacheCopyProgress& p) { last = p; }, error));
    CHECK(last.bytesTotal == 0);

    // A destination that was never ensured is a clean failure.
    TempRoots::writeFile(roots.from / kCacheFolder / "x.mp4", "X");
    CHECK_FALSE(copyCacheTo(roots.from, roots.to / "not-ensured", {}, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("copyCacheTo reports a missing destination cleanly")
{
    TempRoots roots;
    TempRoots::writeFile(roots.from / kCacheFolder / "a.mp4", "A");
    // The destination root was never ensured (no cache directory): a clean
    // failure with an error, not a throw.
    std::string error;
    CHECK_FALSE(copyCacheTo(roots.from, roots.to / "not-ensured", {}, error));
    CHECK_FALSE(error.empty());
}

} // TEST_SUITE("MediaCache")
