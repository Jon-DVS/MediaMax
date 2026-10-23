#pragma once

// Exact time value, stored as a reduced rational number of seconds
// (value / scale).
//
// Media servers require frame-exact timing: 10000 frames at 29.97 fps is
// exactly 10000/29.97 seconds. A fixed-point integer (ns/us) or a double
// accumulates rounding error that is unacceptable for cue triggering,
// timecode display and lip-sync. All arithmetic on Time keeps the result
// exact for the operations a media server performs.

#include <cstdint>
#include <string>

namespace mediamax
{

class FrameRate;

class Time
{
public:
    Time() = default;

    // Exact rational seconds. scale must be > 0. The fraction is reduced to
    // lowest terms (sign carried by value).
    Time(std::int64_t value, std::int64_t scale);

    static Time zero();

    // Double-based constructors round to the nearest microsecond. This is the
    // precision boundary for external (UI/file) time values; internal frame
    // math never loses precision.
    static Time fromSeconds(double seconds);
    static Time fromMilliseconds(double milliseconds);
    static Time fromMicroseconds(std::int64_t microseconds);

    // Exact frame time: frame N at the given frame rate.
    static Time fromFrames(std::int64_t frames, const FrameRate& fps);

    bool isValid() const { return m_Scale > 0; }
    bool isNegative() const { return m_Value < 0; }
    bool isZero() const { return m_Value == 0; }

    double toSeconds() const;
    std::int64_t toMicroseconds() const;

    // Frame index, rounded to the nearest frame.
    std::int64_t toFrames(const FrameRate& fps) const;
    // Frame index, rounded up (conservative for "has the frame ended" checks).
    std::int64_t toFramesCeil(const FrameRate& fps) const;

    Time operator+(const Time& other) const;
    Time operator-(const Time& other) const;
    Time operator-() const;
    Time& operator+=(const Time& other);
    Time& operator-=(const Time& other);

    // Multiply by a real factor (rounded to the nearest microsecond).
    Time operator*(double factor) const;
    Time operator/(double divisor) const;

    // Remainder (like fmod): this - n*mod, n = floor(this/mod), sign of `this`.
    // Requires mod > 0.
    Time operator%(const Time& mod) const;

    bool operator<(const Time& other) const;
    bool operator<=(const Time& other) const;
    bool operator>(const Time& other) const;
    bool operator>=(const Time& other) const;
    bool operator==(const Time& other) const;
    bool operator!=(const Time& other) const;

    static Time min(const Time& a, const Time& b);
    static Time max(const Time& a, const Time& b);
    Time clamp(const Time& low, const Time& high) const;

    // Raw rational parts (reduced, scale > 0).
    std::int64_t value() const { return m_Value; }
    std::int64_t scale() const { return m_Scale; }

    // Timecode string "HH:MM:SS:FF" using the non-drop-frame convention.
    std::string toTimecode(const FrameRate& fps) const;

private:
    std::int64_t m_Value = 0;
    std::int64_t m_Scale = 1;
};

} // namespace mediamax
