#pragma once

// The media library: registry of media assets available to the playback
// engine.
//
// Assets are loaded by file path through a pluggable source factory (the
// playback engine installs the FFmpeg-based factory when built with FFmpeg;
// tests install procedural factories). Loading a file that the installed
// factory cannot decode fails cleanly - the library never throws.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mediamax/media/MediaSource.h"
#include "mediamax/util/Id.h"

namespace mediamax
{

struct MediaAsset
{
    Id id = kInvalidId;
    std::string path;
    MediaMetadata metadata;
    std::shared_ptr<IMediaSource> source;
};

using MediaSourceFactory =
    std::function<std::shared_ptr<IMediaSource>(const std::string& path, std::string& error)>;
using MediaProbeFunction = std::function<MediaMetadata(const std::string& path, std::string& error)>;

class MediaLibrary
{
public:
    MediaLibrary() = default;

    // Sets the factory used to create sources for loaded files. Must be set
    // before load() is used.
    void setSourceFactory(MediaSourceFactory factory) { m_Factory = std::move(factory); }
    void setProbeFunction(MediaProbeFunction fn) { m_Probe = std::move(fn); }

    // Loads a media file: probes it, creates a source, registers the asset.
    // Returns the new asset id; kInvalidId (and an error) on failure.
    Id load(const std::string& path, std::string& error);

    // Probes a file without loading it.
    MediaMetadata probe(const std::string& path, std::string& error);

    // Removes an asset from the library (closes its decoder).
    bool unload(Id id);

    std::optional<MediaAsset*> find(Id id);
    std::optional<const MediaAsset*> find(Id id) const;

    // All asset ids (stable ordering: by id).
    std::vector<Id> assetIds() const;

    int count() const { return static_cast<int>(m_Assets.size()); }
    bool empty() const { return m_Assets.empty(); }

private:
    std::map<Id, MediaAsset> m_Assets;
    IdAllocator m_IdAllocator;
    MediaSourceFactory m_Factory;
    MediaProbeFunction m_Probe;
};

} // namespace mediamax
