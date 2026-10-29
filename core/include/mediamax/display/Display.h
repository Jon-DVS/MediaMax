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

    // Projection warp on/off (on by default). When off the renderer skips
    // the warp entirely - no warp texture upload, no per-pixel lookup -
    // so a wall that does not need correction pays nothing for it. The
    // warp grid itself is kept (the toggle does not reset it).
    bool warpEnabled = true;

    // Transient (not persisted in the show file): when true the renderer
    // draws the warp grid and the node markers over this output, so the
    // operator can see the control lattice on the physical screen while
    // adjusting the warp in the control GUI (the GUI sets it while the
    // warp editor is open and clears it again when the editor closes).
    bool warpPreview = false;

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

    // Vertical synchronization of this output's swapchain (on by default).
    // When on the swapchain uses the FIFO present mode (one image queued per
    // refresh, the compositor paces the output); when off it uses MAILBOX
    // (falling back to FIFO on surfaces that do not support it) so new
    // frames reach the next refresh boundary without the acquire blocking
    // on a full FIFO queue. Changing it at runtime recreates the display's
    // swapchain.
    bool vsync = true;

    // The physical output (OS monitor name) the display's window is on,
    // e.g. "NVIDIA GeForce RTX 4090". Empty when the window is not on a
    // monitor (off-screen) or the name is unknown. Maintained by the
    // playback engine, not the show author.
    std::string monitor;

    Rect2i rect() const { return Rect2i(x, y, width, height); }
    bool valid() const { return width > 0 && height > 0; }
};

} // namespace mediamax
