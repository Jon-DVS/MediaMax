#include "mediamax/show/KeyframeTrack.h"

#include <algorithm>
#include <cmath>

namespace mediamax
{

void KeyframeTrack::setKey(Time time, float value, Interpolation interpolation)
{
    auto it = std::lower_bound(m_Keyframes.begin(), m_Keyframes.end(), time,
                               [](const Keyframe& kf, const Time& t) { return kf.time < t; });
    if (it != m_Keyframes.end() && it->time == time)
    {
        it->value = value;
        it->interpolation = interpolation;
        return;
    }
    m_Keyframes.insert(it, Keyframe{time, value, interpolation});
}

bool KeyframeTrack::removeKey(Time time)
{
    auto it = std::find_if(m_Keyframes.begin(), m_Keyframes.end(),
                           [&](const Keyframe& kf) { return kf.time == time; });
    if (it == m_Keyframes.end())
    {
        return false;
    }
    m_Keyframes.erase(it);
    return true;
}

void KeyframeTrack::clear()
{
    m_Keyframes.clear();
}

float KeyframeTrack::valueAt(Time time) const
{
    if (m_Keyframes.empty())
    {
        return 0.f;
    }
    if (time <= m_Keyframes.front().time)
    {
        return m_Keyframes.front().value;
    }
    if (time >= m_Keyframes.back().time)
    {
        return m_Keyframes.back().value;
    }

    // Find the first keyframe after `time`.
    auto it = std::upper_bound(m_Keyframes.begin(), m_Keyframes.end(), time,
                               [](const Time& t, const Keyframe& kf) { return t < kf.time; });
    const Keyframe& next = *it;
    const Keyframe& prev = *std::prev(it);

    if (prev.interpolation == Interpolation::Step)
    {
        return prev.value;
    }
    if (next.time == prev.time)
    {
        return next.value;
    }
    const double t0 = prev.time.toSeconds();
    const double t1 = next.time.toSeconds();
    const double f = (time.toSeconds() - t0) / (t1 - t0);
    return prev.value + (next.value - prev.value) * static_cast<float>(f);
}

std::string KeyframeTrack::interpolationToString(Interpolation i) const
{
    return i == Interpolation::Linear ? "linear" : "step";
}

Interpolation KeyframeTrack::interpolationFromString(const std::string& s) const
{
    return s == "step" ? Interpolation::Step : Interpolation::Linear;
}

} // namespace mediamax
