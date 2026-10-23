#pragma once

// A layer is one media element (video, image, audio or solid color) placed in
// a scene. Layers composite bottom to top by zIndex.

#include <string>

#include "mediamax/blend/Blend.h"
#include "mediamax/math/Transform2D.h"
#include "mediamax/show/KeyframeTrack.h"
#include "mediamax/time/Time.h"
#include "mediamax/util/Id.h"

namespace mediamax
{

struct Layer
{
    Id id = kInvalidId;
    std::string name;

    // Media to play. kInvalidId means "solid color layer" (uses `color`).
    Id mediaId = kInvalidId;
    Color4 color;

    // Media in/out points (within the media's own timeline). outPoint >=
    // inPoint; when reached the layer holds its last frame (or loops).
    Time inPoint;
    Time outPoint; // zero = "to end of media"

    bool loop = false;
    float rate = 1.f; // playback speed multiplier (positive)

    Transform2D transform;

    float opacity = 1.f;
    BlendMode blend = BlendMode::Normal;
    bool visible = true;
    int zIndex = 0; // higher = drawn on top

    // Optional animation tracks over show time. Empty tracks fall back to the
    // static fields above.
    KeyframeTrack opacityTrack;
    KeyframeTrack xTrack;
    KeyframeTrack yTrack;
    KeyframeTrack rotationTrack;
    KeyframeTrack scaleTrack; // uniform scale (x and y)

    bool operator==(const Layer& o) const;
    bool operator!=(const Layer& o) const { return !(*this == o); }
};

} // namespace mediamax
