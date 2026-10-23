#pragma once

// A display is one physical output surface (a screen, projector or panel).
// Displays tile the output space; together they form the "wall".

#include <string>

#include "mediamax/display/WarpMap.h"
#include "mediamax/math/Rect.h"
#include "mediamax/util/Id.h"

namespace mediamax
{

struct Display
{
    Id id = kInvalidId;
    std::string name;

    // Position and size in output space (pixels).
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 1920;
    std::int32_t height = 1080;

    // Projection correction applied to this output.
    WarpMap warp;

    // Global brightness multiplier applied to this output (1.0 = 100%).
    float brightness = 1.f;

    bool enabled = true;

    Rect2i rect() const { return Rect2i(x, y, width, height); }
    bool valid() const { return width > 0 && height > 0; }
};

} // namespace mediamax
