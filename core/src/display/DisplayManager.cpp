#include "mediamax/display/DisplayManager.h"

namespace mediamax
{

Display DisplayManager::addDisplay(std::string name, std::int32_t x, std::int32_t y,
                                   std::int32_t width, std::int32_t height)
{
    Display display;
    display.id = m_IdAllocator.next();
    display.name = std::move(name);
    display.x = x;
    display.y = y;
    display.width = width;
    display.height = height;
    m_Displays.push_back(display);
    return display;
}

bool DisplayManager::removeDisplay(Id id)
{
    for (size_t i = 0; i < m_Displays.size(); ++i)
    {
        if (m_Displays[i].id == id)
        {
            m_Displays.erase(m_Displays.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

std::optional<Display*> DisplayManager::find(Id id)
{
    for (auto& d : m_Displays)
    {
        if (d.id == id)
        {
            return &d;
        }
    }
    return std::nullopt;
}

std::optional<const Display*> DisplayManager::find(Id id) const
{
    for (const auto& d : m_Displays)
    {
        if (d.id == id)
        {
            return &d;
        }
    }
    return std::nullopt;
}

bool DisplayManager::setDisplayGeometry(Id id, std::int32_t x, std::int32_t y, std::int32_t width,
                                        std::int32_t height)
{
    auto display = find(id);
    if (!display || width <= 0 || height <= 0)
    {
        return false;
    }
    (*display)->x = x;
    (*display)->y = y;
    (*display)->width = width;
    (*display)->height = height;
    return true;
}

bool DisplayManager::setDisplayBrightness(Id id, float brightness)
{
    auto display = find(id);
    if (!display)
    {
        return false;
    }
    (*display)->brightness = brightness;
    return true;
}

bool DisplayManager::setDisplayEnabled(Id id, bool enabled)
{
    auto display = find(id);
    if (!display)
    {
        return false;
    }
    (*display)->enabled = enabled;
    return true;
}

Rect2i DisplayManager::outputBounds() const
{
    Rect2i bounds;
    for (const auto& d : m_Displays)
    {
        if (!d.enabled || !d.valid())
        {
            continue;
        }
        bounds = bounds.unionWith(d.rect());
    }
    return bounds;
}

void DisplayManager::clear()
{
    m_Displays.clear();
}

} // namespace mediamax
