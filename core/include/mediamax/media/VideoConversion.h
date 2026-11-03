#pragma once

// The YUV -> RGB conversion shared by the CPU and the GPU.
//
// YUV420 frames are uploaded as plane textures (Y, U, V) and converted to
// RGB in the GPU layer pass (render/shaders/layer.frag) - the GPU only
// samples textures and applies the conversion per pixel, so the CPU never
// converts video (the old swscale path cost a full YUV -> RGBA pass at the
// native resolution per frame on the decode thread). The shader re-
// implements the math of this header, exactly as the blend shader re-
// implements core/src/blend/Blend.cpp: the two must stay in sync.
//
// Convention: the plane values are the UNORM-normalized storage values
// (8-bit: sample / 255; 10-bit: the scaled 16-bit sample / 65535, see
// scalePlane10To16), i.e. what the texture sampler returns.
//
// 4:2:0 chroma upsampling uses the co-sited mapping: the luma texel (x, y)
// reads the chroma of its 2x2 block exactly, and the luma texels between
// blocks blend 50/50 (the standard hardware mapping; yuvChromaCoord gives
// the chroma texture coordinate the GPU's linear sampler uses, and the CPU
// reference implements the same bilinear over the U/V planes).

#include <array>
#include <cstdint>
#include <vector>

#include "mediamax/media/MediaTypes.h"

namespace mediamax
{

// The YUV -> RGB conversion (the GPU layer pass must match this): given the
// UNORM-normalized plane values (0..1) of one luma texel - Y plus the
// bilinearly upsampled U (cb) and V (cr) at its chroma coordinate - return
// straight (non-premultiplied) RGB clamped to [0, 1]. `fullRange` selects
// the source range: limited (video: 16-235 luma, 16-240 chroma) or full
// (0-255). `space` is the matrix (see YuvColorSpace); Unknown falls back to
// BT.709.
std::array<float, 3> yuvToRgb(float y, float cb, float cr, YuvColorSpace space, bool fullRange);

// The chroma texture coordinate (in [0, 1) over the half-resolution U/V
// planes) for the luma texel (x, y) of a width x height frame - the
// co-sited 4:2:0 mapping the GPU's linear sampler implements (see the file
// comment).
std::array<float, 2> yuvChromaCoord(int x, int y, std::uint32_t width, std::uint32_t height);

// Scales a 10-bit little-endian plane (samples 0..1023 in 2 bytes each) to
// 16-bit UNORM storage (0..65535), mapping 1023 to 65535 exactly (the
// storage the GPU texture and the CPU reference both consume).
void scalePlane10To16(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out);

// Full-frame CPU reference: converts a YUV420P or YUV420P10LE frame to a
// tightly packed RGBA8 buffer (alpha 255) with the same math and chroma
// upsampling as the GPU layer pass. For tests and for CPU-only consumers;
// the playback pipeline converts on the GPU.
bool convertYuvFrameToRgba(const VideoFrame& frame, std::vector<std::uint8_t>& outRgba);

} // namespace mediamax
