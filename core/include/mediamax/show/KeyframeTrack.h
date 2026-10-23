#pragma once

// Keyframe track animating a single float property over show time.
//
// If a track has no keyframes the owning layer falls back to its static
// property value. With keyframes, valueAt() is a pure function of time:
//   - before the first keyframe: first keyframe value (hold)
//   - after the last keyframe: last keyframe value (hold)
//   - between: per-keyframe interpolation (Linear or Step)

#include <cstdint>
#include <vector>

#include "mediamax/time/Time.h"

namespace mediamax
{

enum class Interpolation : std::uint8_t
{
    Linear,
    Step,
};

struct Keyframe
{
    Time time;
    float value = 0.f;
    Interpolation interpolation = Interpolation::Linear;
    bool operator==(const Keyframe&) const = default;
    bool operator!=(const Keyframe& o) const { return !(*this == o); }
};

class KeyframeTrack
{
public:
    KeyframeTrack() = default;

    bool empty() const { return m_Keyframes.empty(); }

    // Inserts or updates the keyframe at `time`, keeping the list sorted by
    // time.
    void setKey(Time time, float value, Interpolation interpolation = Interpolation::Linear);

    bool removeKey(Time time);
    void clear();

    int keyCount() const { return static_cast<int>(m_Keyframes.size()); }

    // Pure function of time (see class docs). Returns 0 when empty.
    float valueAt(Time time) const;

    const std::vector<Keyframe>& keyframes() const { return m_Keyframes; }

    std::string interpolationToString(Interpolation i) const;
    Interpolation interpolationFromString(const std::string& s) const;

private:
    std::vector<Keyframe> m_Keyframes; // sorted by time
};

} // namespace mediamax
