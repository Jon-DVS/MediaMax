#include "mediamax/media/MediaLibrary.h"

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
    return m_Assets.erase(id) > 0;
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
