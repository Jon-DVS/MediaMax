#pragma once

// Integer and floating point 2D rectangles and sizes.
//
// Coordinate conventions used across MediaMax:
//   - Output space is in pixels, origin at the top-left, x right, y down.
//   - Rectangles are [min, max) in both axes.

#include <algorithm>
#include <cstdint>

namespace mediamax
{

struct Size2i
{
    std::int32_t width = 0;
    std::int32_t height = 0;

    bool operator==(const Size2i& o) const { return width == o.width && height == o.height; }
    bool operator!=(const Size2i& o) const { return !(*this == o); }
    bool valid() const { return width > 0 && height > 0; }
};

struct Rect2f
{
    float x = 0.f;
    float y = 0.f;
    float width = 0.f;
    float height = 0.f;

    Rect2f() = default;
    Rect2f(float _x, float _y, float _w, float _h) : x(_x), y(_y), width(_w), height(_h) {}

    float minX() const { return x; }
    float minY() const { return y; }
    float maxX() const { return x + width; }
    float maxY() const { return y + height; }
};

struct Rect2i
{
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;

    Rect2i() = default;
    Rect2i(std::int32_t _x, std::int32_t _y, std::int32_t _w, std::int32_t _h)
        : x(_x), y(_y), width(_w), height(_h)
    {
    }

    std::int32_t minX() const { return x; }
    std::int32_t minY() const { return y; }
    std::int32_t maxX() const { return x + width; }
    std::int32_t maxY() const { return y + height; }
    Size2i size() const { return Size2i{width, height}; }

    bool containsPoint(std::int32_t px, std::int32_t py) const
    {
        return px >= minX() && px < maxX() && py >= minY() && py < maxY();
    }

    bool intersects(const Rect2i& o) const
    {
        return minX() < o.maxX() && o.minX() < maxX() && minY() < o.maxY() && o.minY() < maxY();
    }

    // Smallest rectangle containing both. Returns zero rect if either is empty.
    Rect2i unionWith(const Rect2i& o) const
    {
        if (width == 0 || height == 0)
        {
            return o;
        }
        if (o.width == 0 || o.height == 0)
        {
            return *this;
        }
        const std::int32_t x0 = std::min(minX(), o.minX());
        const std::int32_t y0 = std::min(minY(), o.minY());
        const std::int32_t x1 = std::max(maxX(), o.maxX());
        const std::int32_t y1 = std::max(maxY(), o.maxY());
        return Rect2i(x0, y0, x1 - x0, y1 - y0);
    }

    bool operator==(const Rect2i& o) const
    {
        return x == o.x && y == o.y && width == o.width && height == o.height;
    }
    bool operator!=(const Rect2i& o) const { return !(*this == o); }
};

} // namespace mediamax
