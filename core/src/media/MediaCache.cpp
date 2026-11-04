#include "mediamax/media/MediaCache.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <vector>

namespace mediamax::media
{

namespace
{

// Surrounding whitespace in an entered disk root ("C: ") is an editor
// artifact, not part of the root.
std::string trimmed(const std::string& in)
{
    const auto begin = in.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
    {
        return std::string();
    }
    const auto end = in.find_last_not_of(" \t\r\n");
    return in.substr(begin, end - begin + 1);
}

// Streams `src` to `dst` (overwriting) with a bounded buffer; reports a
// clean error string on failure.
bool copyFileContents(const std::filesystem::path& src, const std::filesystem::path& dst,
                      std::string& error)
{
    constexpr std::size_t kChunkBytes = 1u << 20; // 1 MiB
    std::ifstream in(src, std::ios::binary);
    if (!in)
    {
        error = "cannot open " + src.string() + " for reading";
        return false;
    }
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        error = "cannot open " + dst.string() + " for writing";
        return false;
    }
    // Heap-allocated: a 1 MiB buffer must not sit on the stack (the copy
    // runs on the command thread and the copy worker, both with ordinary
    // stack sizes).
    std::vector<std::uint8_t> chunk(kChunkBytes);
    while (in.read(reinterpret_cast<char*>(chunk.data()),
                   static_cast<std::streamsize>(chunk.size())) ||
           in.gcount() > 0)
    {
        out.write(reinterpret_cast<const char*>(chunk.data()), in.gcount());
        if (!in && !out)
        {
            error = "copy failed: " + src.string();
            return false;
        }
    }
    out.flush();
    if (!out)
    {
        error = "copy failed (write): " + dst.string();
        return false;
    }
    return true;
}

} // namespace

std::optional<std::filesystem::path> normalizeDiskRoot(const std::string& disk)
{
    const std::string s = trimmed(disk);
#if defined(_WIN32)
    // "C", "C:", "c:\" (and the "/" spelling) all mean the C: root;
    // anything with more than the root separator is a directory on a disk,
    // not a disk.
    if (s.empty())
    {
        return std::nullopt;
    }
    const char letter = s[0];
    if (!std::isalpha(static_cast<unsigned char>(letter)))
    {
        return std::nullopt;
    }
    const bool bareLetter = s.size() == 1;
    const bool letterColon = s.size() == 2 && s[1] == ':';
    const bool rootSeparator =
        s.size() == 3 && s[1] == ':' && (s[2] == '\\' || s[2] == '/');
    if (bareLetter || letterColon || rootSeparator)
    {
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
        return std::filesystem::path(std::string(1, upper) + ":\\");
    }
    return std::nullopt;
#else
    (void)s;
    if (s == "/")
    {
        return std::filesystem::path("/");
    }
    return std::nullopt;
#endif
}

std::filesystem::path cacheDirFor(const std::filesystem::path& diskRoot)
{
    return diskRoot / kCacheFolder;
}

bool ensureCacheDir(const std::filesystem::path& diskRoot, std::string& error)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(diskRoot, ec) || ec)
    {
        error = "disk root not found: " + diskRoot.string();
        return false;
    }
    const std::filesystem::path dir = cacheDirFor(diskRoot);
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        error = "cannot create cache directory " + dir.string() + ": " + ec.message();
        return false;
    }
    return true;
}

std::string cacheKeyFor(const std::string& mediaPath)
{
    const std::filesystem::path p(mediaPath);
    if (!p.is_absolute())
    {
        return mediaPath; // the show's relative layout is the cache layout
    }
    const std::string leaf = p.filename().string();
    return leaf.empty() ? mediaPath : leaf;
}

std::optional<std::filesystem::path> cachedPathFor(const std::filesystem::path& diskRoot,
                                                   const std::string& mediaPath)
{
    const std::filesystem::path p = cacheDirFor(diskRoot) / cacheKeyFor(mediaPath);
    std::error_code ec;
    if (!ec && std::filesystem::is_regular_file(p, ec) && !ec)
    {
        return p;
    }
    return std::nullopt;
}

bool copyCacheTo(const std::filesystem::path& fromRoot, const std::filesystem::path& toRoot,
                 const std::function<void(const CacheCopyProgress&)>& onProgress,
                 std::string& error)
{
    const std::filesystem::path from = cacheDirFor(fromRoot);
    const std::filesystem::path to = cacheDirFor(toRoot);

    std::error_code ec;
    if (!std::filesystem::exists(from, ec) || ec)
    {
        // Nothing cached yet (or the source disappeared mid-change): there
        // is nothing to copy.
        if (onProgress)
        {
            onProgress({0, 0});
        }
        return true;
    }
    if (!std::filesystem::is_directory(from, ec) || ec)
    {
        error = "cache source is not a directory: " + from.string();
        return false;
    }

    // Pass 1: the file list and the total, so progress can report a real
    // percentage from the first callback.
    std::vector<std::filesystem::path> files;
    std::uint64_t total = 0;
    for (std::filesystem::recursive_directory_iterator it(
             from, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec))
    {
        if (it->is_regular_file(ec) && !ec)
        {
            const std::uint64_t size = static_cast<std::uint64_t>(it->file_size(ec));
            files.push_back(it->path());
            total += size;
        }
    }
    if (ec)
    {
        error = "cannot list " + from.string() + ": " + ec.message();
        return false;
    }
    if (onProgress)
    {
        onProgress({0, total});
    }
    if (files.empty())
    {
        // Nothing to move: success, the destination never comes into play.
        return true;
    }

    // The destination must exist once there is something to move (the app
    // ensures it for the new disk before starting the copy).
    if (!std::filesystem::is_directory(to, ec) || ec)
    {
        error = "cache directory missing: " + to.string() + " (ensure it first)";
        return false;
    }

    // Pass 2: stream each file (a cache can hold multi-gigabyte video; the
    // buffer keeps the memory flat).
    std::uint64_t copied = 0;
    for (const auto& file : files)
    {
        const std::filesystem::path rel = std::filesystem::relative(file, from, ec);
        if (ec)
        {
            error = "cannot resolve " + file.string() + " against " + from.string();
            return false;
        }
        const std::filesystem::path dest = to / rel;
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec)
        {
            error = "cannot create " + dest.parent_path().string() + ": " + ec.message();
            return false;
        }
        if (!copyFileContents(file, dest, error))
        {
            return false;
        }
        copied += static_cast<std::uint64_t>(std::filesystem::file_size(file, ec));
        if (onProgress)
        {
            onProgress({copied, total});
        }
    }
    return true;
}

} // namespace mediamax::media
