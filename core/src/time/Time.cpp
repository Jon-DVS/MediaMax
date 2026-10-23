#include "mediamax/time/Time.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

#include "mediamax/time/FrameRate.h"

namespace mediamax
{

namespace
{

constexpr double kMicrosecondsPerSecond = 1e6;

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

// Reduces value/scale to lowest terms, scale > 0.
void reduce(std::int64_t& value, std::int64_t& scale)
{
    if (scale < 0)
    {
        value = -value;
        scale = -scale;
    }
    if (value == 0)
    {
        scale = 1;
        return;
    }
    const std::int64_t d = gcd(value, scale);
    if (d > 1)
    {
        value /= d;
        scale /= d;
    }
}

// Multiplies two rationals with overflow guard. The products that occur in a
// media server (hours at microsecond precision, 64-bit frame counts) fit
// comfortably in 64 bits; the guard keeps pathological input from UB-ing.
//
// Note the care around INT64_MIN / -1, which overflows (and raises a
// hardware divide error): the -1 operands are handled explicitly, and every
// division below uses a divisor of magnitude >= 2 or divides INT64_MAX by a
// negative value (both always fit).
bool mulChecked(std::int64_t a, std::int64_t b, std::int64_t& out)
{
    const auto kMax = std::numeric_limits<std::int64_t>::max();
    const auto kMin = std::numeric_limits<std::int64_t>::min();
    if (a == 0 || b == 0)
    {
        out = 0;
        return true;
    }
    if (a == -1)
    {
        out = -b;
        return true;
    }
    if (b == -1)
    {
        out = -a;
        return true;
    }
    if (a > 0)
    {
        if (b > 0)
        {
            if (a > kMax / b)
            {
                return false;
            }
        }
        else if (a > kMin / b) // b < 0, |b| >= 2: fits
        {
            return false;
        }
    }
    else if (b > 0)
    {
        if (a < kMin / b) // b > 0: fits
        {
            return false;
        }
    }
    else if (a < kMax / b) // b < 0, |b| >= 2: fits
    {
        return false;
    }
    out = a * b;
    return true;
}

} // namespace

Time::Time(std::int64_t value, std::int64_t scale) : m_Value(value), m_Scale(scale)
{
    if (m_Scale <= 0)
    {
        throw std::invalid_argument("Time: scale must be > 0");
    }
    reduce(m_Value, m_Scale);
}

Time Time::zero()
{
    return Time();
}

Time Time::fromSeconds(double seconds)
{
    const std::int64_t us = static_cast<std::int64_t>(std::llround(seconds * kMicrosecondsPerSecond));
    return Time(us, static_cast<std::int64_t>(kMicrosecondsPerSecond));
}

Time Time::fromMilliseconds(double milliseconds)
{
    return fromSeconds(milliseconds / 1000.0);
}

Time Time::fromMicroseconds(std::int64_t microseconds)
{
    return Time(microseconds, static_cast<std::int64_t>(kMicrosecondsPerSecond));
}

Time Time::fromFrames(std::int64_t frames, const FrameRate& fps)
{
    // frames / fps = frames * fps.den / fps.num seconds
    return Time(frames * fps.denominator(), fps.numerator());
}

double Time::toSeconds() const
{
    return static_cast<long double>(m_Value) / static_cast<long double>(m_Scale);
}

std::int64_t Time::toMicroseconds() const
{
    // value/scale * 1e6, rounded
    const long double us =
        static_cast<long double>(m_Value) * kMicrosecondsPerSecond /
        static_cast<long double>(m_Scale);
    return static_cast<std::int64_t>(std::llround(us));
}

std::int64_t Time::toFrames(const FrameRate& fps) const
{
    // t * fps = value/scale * num/den, rounded to nearest frame
    const long double frames =
        static_cast<long double>(m_Value) * static_cast<long double>(fps.numerator()) /
        (static_cast<long double>(m_Scale) * static_cast<long double>(fps.denominator()));
    return static_cast<std::int64_t>(std::llround(frames));
}

std::int64_t Time::toFramesCeil(const FrameRate& fps) const
{
    const long double frames =
        static_cast<long double>(m_Value) * static_cast<long double>(fps.numerator()) /
        (static_cast<long double>(m_Scale) * static_cast<long double>(fps.denominator()));
    return static_cast<std::int64_t>(std::ceil(frames - 1e-9));
}

Time Time::operator+(const Time& other) const
{
    // value/scale + o.value/o.scale = (value*o.scale + o.value*scale) / (scale*o.scale)
    std::int64_t a = 0;
    std::int64_t b = 0;
    std::int64_t den = 0;
    if (!mulChecked(m_Value, other.m_Scale, a) || !mulChecked(other.m_Value, m_Scale, b) ||
        !mulChecked(m_Scale, other.m_Scale, den) ||
        (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))
    {
        // Fall back to double for absurd magnitudes; precision is already
        // lost at this point, sign correctness is all that remains.
        return fromSeconds(toSeconds() + other.toSeconds());
    }
    return Time(a + b, den);
}

Time Time::operator-(const Time& other) const
{
    return *this + Time(-other.m_Value, other.m_Scale);
}

Time Time::operator-() const
{
    return Time(-m_Value, m_Scale);
}

Time& Time::operator+=(const Time& other)
{
    *this = *this + other;
    return *this;
}

Time& Time::operator-=(const Time& other)
{
    *this = *this - other;
    return *this;
}

Time Time::operator*(double factor) const
{
    return fromSeconds(toSeconds() * factor);
}

Time Time::operator/(double divisor) const
{
    if (divisor == 0.0)
    {
        throw std::invalid_argument("Time::operator/ zero divisor");
    }
    return fromSeconds(toSeconds() / divisor);
}

Time Time::operator%(const Time& mod) const
{
    if (mod <= Time::zero())
    {
        throw std::invalid_argument("Time::operator% requires a positive modulus");
    }
    const long double quotient = toSeconds() / mod.toSeconds();
    const long double n = std::floor(quotient);
    return fromSeconds(toSeconds() - n * mod.toSeconds());
}

bool Time::operator<(const Time& other) const
{
    // value/scale < o.value/o.scale  <=>  value*o.scale < o.value*scale
    // (both scales > 0). Use long double to avoid overflow for large times.
    return static_cast<long double>(m_Value) * other.m_Scale <
           static_cast<long double>(other.m_Value) * m_Scale;
}

bool Time::operator<=(const Time& other) const { return !(other < *this); }
bool Time::operator>(const Time& other) const { return other < *this; }
bool Time::operator>=(const Time& other) const { return !(*this < other); }

bool Time::operator==(const Time& other) const
{
    // Both operands are reduced to lowest terms, so two equal rationals are
    // structurally identical. Exact, no overflow, no precision loss.
    return m_Value == other.m_Value && m_Scale == other.m_Scale;
}

bool Time::operator!=(const Time& other) const { return !(*this == other); }

Time Time::min(const Time& a, const Time& b)
{
    return a < b ? a : b;
}

Time Time::max(const Time& a, const Time& b)
{
    return a > b ? a : b;
}

Time Time::clamp(const Time& low, const Time& high) const
{
    if (*this < low)
    {
        return low;
    }
    if (*this > high)
    {
        return high;
    }
    return *this;
}

std::string Time::toTimecode(const FrameRate& fps) const
{
    const std::int64_t frames = toFrames(fps);
    if (frames < 0)
    {
        return "-00:00:00:00";
    }
    const std::int64_t fpsNominal = fps.nominalFramesPerSecond();
    const std::int64_t ff = frames % fpsNominal;
    const std::int64_t totalSeconds = frames / fpsNominal;
    const std::int64_t hh = totalSeconds / 3600;
    const std::int64_t mm = (totalSeconds / 60) % 60;
    const std::int64_t ss = totalSeconds % 60;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld:%02lld",
                  static_cast<long long>(hh), static_cast<long long>(mm),
                  static_cast<long long>(ss), static_cast<long long>(ff));
    return buf;
}

} // namespace mediamax
