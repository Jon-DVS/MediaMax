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

    // Projection correction applied to this output. The default is the
    // minimal identity grid: a display that has never been warped reports
    // (and edits from) 2x2 points, and display.warp.set replaces the whole
    // map (with the grid the client sent).
    WarpMap warp{2, 2};

    // Global brightness multiplier applied to this output (1.0 = 100%).
    float brightness = 1.f;

    bool enabled = true;

    // When true the playback engine keeps the display's window above all
    // other windows (the platform's topmost stacking state, applied by the
    // playback app). On by default: a playback window is an output surface,
    // not an application window the user works alongside.
    bool alwaysOnTop = true;

    // When true the playback engine draws a status overlay (system IP,
    // display name/number, measured frame rate, resolution and frame
    // timing) over this output. Off by default; see overlay/StatusOverlay.
    bool statusOverlay = false;

    // The physical output (OS monitor name) the display's window is on,
    // e.g. "NVIDIA GeForce RTX 4090". Empty when the window is not on a
    // monitor (off-screen) or the name is unknown. Maintained by the
    // playback engine, not the show author.
    std::string monitor;

    Rect2i rect() const { return Rect2i(x, y, width, height); }
    bool valid() const { return width > 0 && height > 0; }
};

} // namespace mediamax
