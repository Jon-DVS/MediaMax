#pragma once

// FFmpeg-backed media decoding.
//
// Only compiled/available when MediaMax is built with -DMEDIAMAX_ENABLE_FFMPEG
// (the default). The playback engine installs these functions as the
// MediaLibrary's probe function and source factory.

#include <memory>
#include <string>

#include "mediamax/media/MediaLibrary.h"
#include "mediamax/media/MediaSource.h"

namespace mediamax::ffmpeg
{

// Probes a media file and returns its metadata without decoding frames.
// Returns an empty (invalid) metadata and sets `error` on failure.
MediaMetadata probeFile(const std::string& path, std::string& error);

// Creates a decode source for a media file. Returns nullptr and sets `error`
// on failure.
std::shared_ptr<IMediaSource> createSource(const std::string& path, std::string& error);

} // namespace mediamax::ffmpeg
