#include "mediamax/show/Layer.h"

#include "mediamax/show/KeyframeTrack.h"

namespace mediamax
{

namespace
{

bool tracksEqual(const KeyframeTrack& a, const KeyframeTrack& b)
{
    const auto& ka = a.keyframes();
    const auto& kb = b.keyframes();
    if (ka.size() != kb.size())
    {
        return false;
    }
    for (size_t i = 0; i < ka.size(); ++i)
    {
        if (ka[i] != kb[i])
        {
            return false;
        }
    }
    return true;
}

} // namespace

bool Layer::operator==(const Layer& o) const
{
    return id == o.id && name == o.name && mediaId == o.mediaId && color == o.color &&
           inPoint == o.inPoint && outPoint == o.outPoint && loop == o.loop &&
           rate == o.rate && transform == o.transform && opacity == o.opacity &&
           blend == o.blend && visible == o.visible && zIndex == o.zIndex &&
           tracksEqual(opacityTrack, o.opacityTrack) &&
           tracksEqual(xTrack, o.xTrack) &&
           tracksEqual(yTrack, o.yTrack) &&
           tracksEqual(rotationTrack, o.rotationTrack) &&
           tracksEqual(scaleTrack, o.scaleTrack);
}

} // namespace mediamax
