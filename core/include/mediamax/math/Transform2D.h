#pragma once

// 2D transform for layers in output space.
//
// A layer's media (width x height pixels) is scaled to (scaleX * width,
// scaleY * height), rotated around the anchor point (expressed in media UV,
// 0..1) and placed so the anchor lands at (x, y) in output space.
//
// Output space is in pixels, origin top-left, y down. Rotation is in radians,
// positive = clockwise (because y grows downward).

#include <cmath>

#include "mediamax/math/Rect.h"

namespace mediamax
{

struct Transform2D
{
    float x = 0.f;
    float y = 0.f;
    float rotation = 0.f; // radians, clockwise positive
    float scaleX = 1.f;
    float scaleY = 1.f;
    float anchorX = 0.f; // in media UV (0..1); default = the media's top-left
    float anchorY = 0.f;

    static Transform2D identity()
    {
        return Transform2D{};
    }

    // Maps a media UV point (0..1 within the media) to output space pixels,
    // given the media's display size in pixels.
    std::pair<float, float> toOutputSpace(float mediaU, float mediaV, const Size2i mediaSize) const
    {
        const float dx = scaleX * static_cast<float>(mediaSize.width);
        const float dy = scaleY * static_cast<float>(mediaSize.height);
        const float localX = (mediaU - anchorX) * dx;
        const float localY = (mediaV - anchorY) * dy;

        const float c = std::cos(rotation);
        const float s = std::sin(rotation);
        // Clockwise rotation in a y-down coordinate system: +x (right)
        // rotates toward +y (down).
        const float rx = localX * c - localY * s;
        const float ry = localX * s + localY * c;

        return {x + rx, y + ry};
    }

    // The output-space rectangle covered by the media when unrotated
    // (useful for culling and preview bounds).
    Rect2f bounds(const Size2i mediaSize) const
    {
        const auto [minX, minY] = toOutputSpace(0.f, 0.f, mediaSize);
        const auto [maxX, maxY] = toOutputSpace(1.f, 1.f, mediaSize);
        return Rect2f(std::min(minX, maxX), std::min(minY, maxY),
                      std::abs(maxX - minX), std::abs(maxY - minY));
    }

    bool operator==(const Transform2D& o) const
    {
        return x == o.x && y == o.y && rotation == o.rotation && scaleX == o.scaleX &&
               scaleY == o.scaleY && anchorX == o.anchorX && anchorY == o.anchorY;
    }
    bool operator!=(const Transform2D& o) const { return !(*this == o); }
};

} // namespace mediamax
