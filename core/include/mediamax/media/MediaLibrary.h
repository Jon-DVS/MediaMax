#pragma once

// The media library: registry of media assets available to the playback
// engine.
//
// Assets are loaded by file path through a pluggable source factory (the
// playback engine installs the FFmpeg-based factory when built with FFmpeg;
// tests install procedural factories). Loading a file that the installed
// factory cannot decode fails cleanly - the library never throws.
//
// Media is deduplicated by file: the same file (canonicalized path) is
// loaded once. Re-loading an unchanged file returns the existing asset id;
// a file that changed on disk (size or last-write time) is reloaded in
// place, keeping its id. The fingerprint is deliberately cheap (stat-based,
// not a content hash): media files can be multi-gigabyte.

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

    // A copy of the installed source factory (may be empty). The playback
    // engine's media pump opens one fresh source instance per active layer
    // with it (a source is single-consumer, and a layer needs its own
    // decode position - see MediaPump); the library's own instance stays
    // with the library for probing/metadata.
    MediaSourceFactory sourceFactory() const { return m_Factory; }

    // Loads a media file: probes it, creates a source, registers the asset.
    // Returns the asset id; kInvalidId (and an error) on failure.
    //
    // Deduplication by file (see the class docs): a path that is already
    // loaded with an unchanged file returns the existing id without
    // touching the factory; a changed file is reloaded in place (same id).
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
    // Change fingerprint of a file on disk: (size, last write time).
    // nullopt when the file does not exist or cannot be stat'ed.
    static std::optional<std::pair<std::uintmax_t, std::filesystem::file_time_type>>
    fileFingerprint(const std::string& path);
    // Normalized dedup key for a path: weakly canonical (missing components
    // are tolerated) and lowercased on case-insensitive file systems.
    static std::string pathKey(const std::string& path);

    // One dedup entry per loaded file.
    struct PathEntry
    {
        std::optional<std::pair<std::uintmax_t, std::filesystem::file_time_type>> fingerprint;
        Id id = kInvalidId;
    };

    std::map<Id, MediaAsset> m_Assets;
    std::map<std::string, PathEntry> m_PathEntries;
    IdAllocator m_IdAllocator;
    MediaSourceFactory m_Factory;
    MediaProbeFunction m_Probe;
};

} // namespace mediamax
