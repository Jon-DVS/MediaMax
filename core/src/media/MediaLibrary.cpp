#include "mediamax/media/MediaLibrary.h"

#include <cctype>
#include <system_error>

#include "mediamax/util/Logger.h"

namespace mediamax
{

Id MediaLibrary::load(const std::string& path, std::string& error)
{
    if (!m_Factory)
    {
        error = "no media source factory installed";
        return kInvalidId;
    }

    const std::string key = pathKey(path);
    auto it = m_PathEntries.find(key);
    if (it != m_PathEntries.end())
    {
        const auto assetIt = m_Assets.find(it->second.id);
        if (assetIt != m_Assets.end())
        {
            // The file is already loaded: reuse the asset unless the file on
            // disk changed. (A missing file has a nullopt fingerprint on both
            // sides, so an unchanged missing file does not re-invoke the
            // factory.)
            const auto fingerprint = fileFingerprint(path);
            if (fingerprint == it->second.fingerprint)
            {
                Logger::instance().debug("Media already loaded: {} (asset #{})", path,
                                         it->second.id);
                return it->second.id;
            }

            // Changed: create the replacement first (a failure leaves the
            // asset intact), then swap it in. Keeping the same id means
            // references (scenes, in-flight pump channels) stay valid; the
            // pump holds shared_ptr copies, so the old source lives on until
            // its channels move over.
            std::shared_ptr<IMediaSource> source = m_Factory(path, error);
            if (!source)
            {
                if (error.empty())
                {
                    error = "failed to create media source for: " + path;
                }
                Logger::instance().warning("MediaLibrary::load reload failed for '{}': {}",
                                           path, error);
                return kInvalidId;
            }
            MediaAsset& asset = assetIt->second;
            asset.metadata = source->metadata();
            asset.source = std::move(source);
            it->second.fingerprint = fingerprint;
            Logger::instance().info("Reloaded changed media asset #{}: {}", asset.id, path);
            return asset.id;
        }
        // Stale entry (the asset is gone): drop it and load fresh.
        m_PathEntries.erase(it);
    }

    std::shared_ptr<IMediaSource> source = m_Factory(path, error);
    if (!source)
    {
        if (error.empty())
        {
            error = "failed to create media source for: " + path;
        }
        Logger::instance().warning("MediaLibrary::load failed for '{}': {}", path, error);
        return kInvalidId;
    }

    MediaAsset asset;
    asset.id = m_IdAllocator.next();
    asset.path = path;
    asset.metadata = source->metadata();
    asset.source = std::move(source);

    m_Assets[asset.id] = std::move(asset);
    m_PathEntries[key] = PathEntry{fileFingerprint(path), asset.id};
    Logger::instance().info("Loaded media asset #{}: {}", asset.id, path);
    return asset.id;
}

MediaMetadata MediaLibrary::probe(const std::string& path, std::string& error)
{
    if (!m_Probe)
    {
        error = "no probe function installed";
        return {};
    }
    return m_Probe(path, error);
}

bool MediaLibrary::unload(Id id)
{
    if (m_Assets.erase(id) == 0)
    {
        return false;
    }
    // Keep the dedup table in sync: the file may be loaded again with a fresh
    // id.
    for (auto it = m_PathEntries.begin(); it != m_PathEntries.end();)
    {
        if (it->second.id == id)
        {
            it = m_PathEntries.erase(it);
        }
        else
        {
            ++it;
        }
    }
    return true;
}

std::optional<std::pair<std::uintmax_t, std::filesystem::file_time_type>>
MediaLibrary::fileFingerprint(const std::string& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        return std::nullopt;
    }
    std::error_code mtimeEc;
    const auto mtime = std::filesystem::last_write_time(path, mtimeEc);
    if (mtimeEc)
    {
        return std::nullopt;
    }
    return std::make_pair(size, mtime);
}

std::string MediaLibrary::pathKey(const std::string& path)
{
    // weakly_canonical (not canonical): missing components are tolerated, so
    // a not-yet-existing file still gets a stable key.
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    std::string key = ec ? path : canonical.generic_string();
#if defined(_WIN32)
    // The Windows file system is case-insensitive: normalize case so
    // "Media\Clip.mp4" and "media\clip.mp4" dedupe to the same asset.
    for (char& c : key)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
#endif
    return key;
}

std::optional<MediaAsset*> MediaLibrary::find(Id id)
{
    auto it = m_Assets.find(id);
    if (it == m_Assets.end())
    {
        return std::nullopt;
    }
    return &it->second;
}

std::optional<const MediaAsset*> MediaLibrary::find(Id id) const
{
    auto it = m_Assets.find(id);
    if (it == m_Assets.end())
    {
        return std::nullopt;
    }
    return &it->second;
}

std::vector<Id> MediaLibrary::assetIds() const
{
    std::vector<Id> ids;
    ids.reserve(m_Assets.size());
    for (const auto& [id, _] : m_Assets)
    {
        ids.push_back(id);
    }
    return ids;
}

} // namespace mediamax
