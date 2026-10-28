#pragma once

// Status overlay text rasterizer.
//
// The playback engine draws a small status overlay (software name, system
// IP, display name/number, physical output, resolution, and the fps / CPU /
// GPU frame-time metrics with their rolling min/max/avg) over a display
// when that display's `statusOverlay` flag is set. This module is the
// headless half of that feature: it rasterizes ASCII text with a built-in
// 8x11 monospace bitmap font into a tightly packed RGBA8 buffer (white
// glyphs on a transparent background, straight alpha) that the render
// library uploads to a texture and composites in the display pass.
//
// Keeping the rasterizer in the core (pure data + CPU pixels, no GPU, no
// window) makes it unit-testable on any machine, per the headless-core
// rule. The font table lives in the .cpp.

#include <cstdint>
#include <string>
#include <vector>

namespace mediamax::overlay
{

// Rasterize `text` into an RGBA8 buffer.
//
// - `scale` is an integer pixel multiplier for the 8x11 glyphs (1 => each
//   glyph is 8x11 px). Must be >= 1.
// - '\n' starts a new line; '\r' is ignored. Characters outside 32..126
//   render as blank space.
// - Each character occupies an 8-wide glyph cell plus a 1-px gap (9 px
//   advance at scale 1); each line is 11 tall plus a 1-px gap (12 px at
//   scale 1). The output canvas is the bounding box of all lines:
//   width = longestLineChars * 9 * scale, height = lineCount * 12 * scale.
// - Glyphs are white (255,255,255,255); everything else is transparent
//   (0,0,0,0). `outRgba` is tightly packed outWidth*outHeight*4 bytes,
//   row-major, top-left origin.
//
// Returns false and fills `error` when scale < 1. An empty (or all-newline)
// input produces zero-sized output and returns true.
bool renderText(const std::string& text, int scale, std::vector<std::uint8_t>& outRgba,
                std::uint32_t& outWidth, std::uint32_t& outHeight, std::string& error);

} // namespace mediamax::overlay
