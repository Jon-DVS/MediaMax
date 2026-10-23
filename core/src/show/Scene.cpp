#include "mediamax/show/Scene.h"

namespace mediamax
{

const Layer* Scene::findLayer(Id layerId) const
{
    for (const auto& layer : layers)
    {
        if (layer.id == layerId)
        {
            return &layer;
        }
    }
    return nullptr;
}

Layer* Scene::findLayer(Id layerId)
{
    for (auto& layer : layers)
    {
        if (layer.id == layerId)
        {
            return &layer;
        }
    }
    return nullptr;
}

} // namespace mediamax
