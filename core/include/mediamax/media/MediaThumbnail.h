#pragma once

// Decodes a single representative frame of a media asset into RGBA pixels
// for control-interface previews (the media.thumbnail command).
//
// The function is deliberately self-contained: it opens its own source
// instance through the library's source factory. It never touches the
// library's own instance (that one is kept for probing/metadata - see
// MediaSource.h). Callers must pass a consistent snapshot of one asset
// (path + metadata), taken under the library's own synchronization (the
// app's state mutex); the decode itself runs safely on any thread.

#include <cstdint>
#include <string>
#include <vector>

#include "mediamax/media/MediaLibrary.h"
#include "mediamax/media/MediaTypes.h"

namespace mediamax::media
{

// Decodes a representative frame (video: the frame at ~10% of the duration,
// capped so short and long clips alike land in a representative region;
// image: the image itself) and returns it as RGBA8, aspect-fit into
// maxWidth x maxHeight without upscaling (an asset smaller than the box is
// returned at its native size).
//
// On success outRgba holds outWidth * outHeight * 4 bytes and returns true.
// On failure the out parameters are left unchanged and error describes the
// problem (no source factory, media without video content, the source could
// not be opened, or no frame could be decoded).
bool makeThumbnail(const MediaSourceFactory& factory, const std::string& path,
                   const MediaMetadata& metadata, int maxWidth, int maxHeight,
                   std::vector<std::uint8_t>& outRgba, int& outWidth, int& outHeight,
                   std::string& error);

} // namespace mediamax::media
