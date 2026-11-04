#pragma once

// The playback system's media cache: the folder where the control interface
// transfers the show's media (media.put, see docs/PROTOCOL.md) so playback
// runs from the local disk instead of a path that only exists on the
// control machine.
//
// The cache lives in a `mediamax-cache` folder at the root of a disk the
// operator selects per system (system.setCacheDisk; the choice persists in
// the show file's systems entry, `cacheDisk`). Everything here is plain
// filesystem work - no GPU, window or socket - so it stays in the headless
// core and is unit-testable on any machine.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace mediamax::media
{

// The cache folder name at the root of the cache disk (the playback engine
// creates it when the cache disk is set).
inline constexpr const char* kCacheFolder = "mediamax-cache";

// Normalizes a disk root the way the control surface enters it ("C", "C:",
// "c:\\", "C:\\" all mean the C: root) into its canonical form ("C:\\" on
// Windows, "/" on POSIX systems). Returns nullopt when the string is not a
// disk root: not a drive letter + colon (Windows) or something other than
// the root directory (POSIX), or with any path components beyond the root
// ("C:\Users" is a directory on a disk, not a disk).
std::optional<std::filesystem::path> normalizeDiskRoot(const std::string& disk);

// The cache directory for a (canonical) disk root: root / "mediamax-cache".
std::filesystem::path cacheDirFor(const std::filesystem::path& diskRoot);

// Creates the cache directory (and the disk root's subfolder chain). Fails
// (with `error`) when the disk root does not exist - a real disk root
// always does, so this is the missing-disk case, not a create-it case.
bool ensureCacheDir(const std::filesystem::path& diskRoot, std::string& error);

// The cache key for a show media path: the path as written when it is
// relative (the show's media layout is mirrored under the cache root), the
// file name when it is absolute (an absolute path is machine-specific and
// only its name is portable between the control and playback machines).
std::string cacheKeyFor(const std::string& mediaPath);

// The cached file for a show media path, when it exists on this system:
// cacheDirFor(root) / cacheKeyFor(mediaPath) for regular files only.
std::optional<std::filesystem::path> cachedPathFor(const std::filesystem::path& diskRoot,
                                                   const std::string& mediaPath);

// Progress of a cache copy (system.setCacheDisk across disks): cumulative
// bytes of the copied files and the total (known before the copy starts).
struct CacheCopyProgress
{
    std::uint64_t bytesCopied = 0;
    std::uint64_t bytesTotal = 0;
};

// Copies the old disk's cache contents (fromRoot / "mediamax-cache" / ...)
// to the new disk's cache directory (toRoot / "mediamax-cache" / ...),
// preserving the relative layout and overwriting files that already exist
// at the destination. The destination directory must exist (the caller
// ensured it with ensureCacheDir). A missing or empty source is a success
// with a zero total - the common first-time case. `onProgress` is called
// once with the full total before the first byte moves and once after each
// file completes; it may be empty. Returns false (with `error`) on I/O
// failure; partial copies are left in place, a re-run of the same change
// overwrites them (the copy is idempotent).
bool copyCacheTo(const std::filesystem::path& fromRoot, const std::filesystem::path& toRoot,
                 const std::function<void(const CacheCopyProgress&)>& onProgress,
                 std::string& error);

} // namespace mediamax::media
