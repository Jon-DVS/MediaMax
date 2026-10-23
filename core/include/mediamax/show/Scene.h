#pragma once

// A scene is a named set of layers. Only one scene is active at a time; the
// show timeline switches between scenes with cues.

#include <string>
#include <vector>

#include "mediamax/show/Layer.h"
#include "mediamax/util/Id.h"

namespace mediamax
{

struct Scene
{
    Id id = kInvalidId;
    std::string name;
    std::vector<Layer> layers;

    const Layer* findLayer(Id layerId) const;
    Layer* findLayer(Id layerId);
};

} // namespace mediamax
