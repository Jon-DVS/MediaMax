#pragma once

// Blend modes for compositing layers.
//
// Two implementations exist and must stay in sync:
//   1. The CPU reference (blend::apply) - source of truth, used by unit
//      tests.
//   2. The layer compositing fragment shader in
//      render/src/SceneRenderer.cpp (render/shaders/layer.frag) - the actual
//      GPU compositing path.
//
// Conventions:
//   - Layer source textures are straight-alpha; the scene render target is
//     premultiplied-alpha.
//   - Compositing happens in premultiplied alpha space in both
//     implementations; the GPU path unpremultiplies in the final display
//     pass.

#include <cstdint>
#include <string>

namespace mediamax
{

enum class BlendMode : std::uint8_t
{
    Normal = 0,  // alpha over
    Add = 1,     // additive light
    Subtract = 2,
    Multiply = 3,
    Screen = 4,
};

struct Color4
{
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
    float a = 1.f;
    bool operator==(const Color4&) const = default;
    bool operator!=(const Color4& o) const { return !(*this == o); }
};

namespace blend
{

// CPU reference implementation. Colors are straight (unmultiplied) alpha,
// all values in 0..1. Result is clamped to 0..1.
Color4 apply(const Color4& dst, const Color4& src, BlendMode mode);

// Number of distinct modes (used for pipeline arrays in the renderer).
constexpr std::uint8_t kBlendModeCount = 5;

std::string toString(BlendMode mode);

// Returns the default mode on parse failure.
BlendMode fromString(const std::string& name);

} // namespace blend

} // namespace mediamax
