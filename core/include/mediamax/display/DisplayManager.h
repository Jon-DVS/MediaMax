#pragma once

// Manages the set of displays (the output wall).
//
// The union of all enabled display rectangles defines the output space -
// the virtual canvas the scene is rendered into before being sliced across
// displays.

#include <functional>
#include <optional>
#include <vector>

#include "mediamax/display/Display.h"

namespace mediamax
{

class DisplayManager
{
public:
    DisplayManager() = default;

    // Adds a display with a freshly allocated id.
    Display addDisplay(std::string name, std::int32_t x, std::int32_t y, std::int32_t width,
                       std::int32_t height);

    bool removeDisplay(Id id);

    // Copies the manager's display list (cheap: displays are value types).
    const std::vector<Display>& displays() const { return m_Displays; }
    std::vector<Display> displayList() const { return m_Displays; }

    // Pointer to a live display for mutation; nullopt if not found.
    std::optional<Display*> find(Id id);
    std::optional<const Display*> find(Id id) const;

    bool setDisplayGeometry(Id id, std::int32_t x, std::int32_t y, std::int32_t width,
                            std::int32_t height);
    bool setDisplayBrightness(Id id, float brightness);
    bool setDisplayEnabled(Id id, bool enabled);
    bool setDisplayAlwaysOnTop(Id id, bool alwaysOnTop);
    bool setDisplayStatusOverlay(Id id, bool statusOverlay);
    // Vsync (swapchain present mode) for this output.
    bool setDisplayVsync(Id id, bool vsync);
    // The display name (shown in the UI and the status overlay).
    bool setDisplayName(Id id, std::string name);
    // The physical output the display's window is on (the playback engine
    // reports it from the window manager; empty = not on a monitor).
    bool setDisplayMonitor(Id id, std::string monitor);

    // Union of all enabled display rectangles. Zero rect when no displays are
    // enabled.
    Rect2i outputBounds() const;

    int count() const { return static_cast<int>(m_Displays.size()); }
    bool empty() const { return m_Displays.empty(); }

    void clear();

private:
    std::vector<Display> m_Displays;
    IdAllocator m_IdAllocator;
};

} // namespace mediamax
