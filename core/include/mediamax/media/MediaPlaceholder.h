#pragma once

// The media placeholder: the frame a media layer draws while its file is
// not available on this system (pending media - the show loaded before the
// media was transferred into the media cache, or the file has not been
// copied to the original path yet): a dark fill with the media's file name
// centered on the canvas and a hint line below it.
//
// Headless: pure CPU pixels (straight-alpha RGBA8), no GPU or window, so it
// is unit-testable on any machine (the text reuses the core
// overlay/StatusOverlay bitmap font). The playback app rasterizes it once
// per (file name, size) and uploads it into the layer's media texture at
// the scene size, so the placeholder fills the layer's slot 1:1 and follows
// the layer's transform like the media would.

#include <cstdint>
#include <string>
#include <vector>

namespace mediamax::media
{

// Rasterizes the placeholder for `fileName` into `outRgba`
// (width * height * 4 bytes, row-major, top-left origin, straight alpha):
// a dark fill, the file name centered on the canvas in white, and a fixed
// hint line ("media not available on this system") below it in grey. The
// text scale adapts to the canvas (the block never outgrows the canvas;
// the scale is clamped to at least 1). `fileName` is middle-truncated when
// it would not fit; non-ASCII characters render as blank (the font covers
// 32..126). width or height <= 0 yields an empty buffer.
void renderMediaPlaceholder(int width, int height, const std::string& fileName,
                            std::vector<std::uint8_t>& outRgba);

} // namespace mediamax::media
