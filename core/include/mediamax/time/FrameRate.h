#pragma once

// Rational frame rate (numerator/denominator frames per second).
//
// Rational representation is required for exact frame math: 29.97 fps is
// 30000/1001, which cannot be represented exactly as a double.

#include <cstdint>
#include <string>

namespace mediamax
{

class FrameRate
{
public:
    FrameRate() = default;

    // Creates a frame rate, reducing to lowest terms. num must be > 0, den >= 1.
    static FrameRate of(std::int64_t frames, std::int64_t seconds);

    // Common broadcast rates.
    static FrameRate fps23976(); // 23.976 = 24000/1001
    static FrameRate fps24();
    static FrameRate fps25();
    static FrameRate fps2997(); // 29.97  = 30000/1001
    static FrameRate fps30();
    static FrameRate fps50();
    static FrameRate fps5994(); // 59.94 = 60000/1001
    static FrameRate fps60();

    std::int64_t numerator() const { return m_Num; }
    std::int64_t denominator() const { return m_Den; }

    double framesPerSecond() const;
    double secondsPerFrame() const;

    // Whole-frame count per nominal display second (29.97 -> 30). Used for
    // timecode display (non-drop-frame convention).
    std::int64_t nominalFramesPerSecond() const;

    bool isInteger() const { return m_Den == 1; }

    bool operator==(const FrameRate& other) const { return m_Num == other.m_Num && m_Den == other.m_Den; }
    bool operator!=(const FrameRate& other) const { return !(*this == other); }

    // "30", "29.97", ...
    std::string toString() const;

private:
    FrameRate(std::int64_t num, std::int64_t den) : m_Num(num), m_Den(den) {}

    std::int64_t m_Num = 30;
    std::int64_t m_Den = 1;
};

} // namespace mediamax
