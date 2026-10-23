#include "mediamax/time/FrameRate.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace mediamax
{

namespace
{

std::int64_t gcd(std::int64_t a, std::int64_t b)
{
    a = a < 0 ? -a : a;
    b = b < 0 ? -b : b;
    while (b != 0)
    {
        const std::int64_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

} // namespace

FrameRate FrameRate::of(std::int64_t frames, std::int64_t seconds)
{
    if (frames <= 0 || seconds <= 0)
    {
        throw std::invalid_argument("FrameRate::of requires frames > 0 and seconds > 0");
    }
    const std::int64_t d = gcd(frames, seconds);
    return FrameRate(frames / d, seconds / d);
}

FrameRate FrameRate::fps23976() { return of(24000, 1001); }
FrameRate FrameRate::fps24() { return of(24, 1); }
FrameRate FrameRate::fps25() { return of(25, 1); }
FrameRate FrameRate::fps2997() { return of(30000, 1001); }
FrameRate FrameRate::fps30() { return of(30, 1); }
FrameRate FrameRate::fps50() { return of(50, 1); }
FrameRate FrameRate::fps5994() { return of(60000, 1001); }
FrameRate FrameRate::fps60() { return of(60, 1); }

double FrameRate::framesPerSecond() const
{
    return static_cast<double>(m_Num) / static_cast<double>(m_Den);
}

double FrameRate::secondsPerFrame() const
{
    return static_cast<double>(m_Den) / static_cast<double>(m_Num);
}

std::int64_t FrameRate::nominalFramesPerSecond() const
{
    // Nearest whole frame count per second: 30000/1001 -> 30.
    return (m_Num + m_Den / 2) / m_Den;
}

std::string FrameRate::toString() const
{
    if (m_Den == 1)
    {
        return std::to_string(m_Num);
    }
    // Two-decimal representation is standard for broadcast rates.
    const double fps = framesPerSecond();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", fps);
    std::string s(buf);
    // Trim trailing zeros: 29.97 stays, 30.00 -> "30" is handled above.
    while (s.size() > 3 && s.back() == '0')
    {
        s.pop_back();
    }
    if (!s.empty() && s.back() == '.')
    {
        s.pop_back();
    }
    return s;
}

} // namespace mediamax
