#include "mediamax/time/Timecode.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace mediamax
{

namespace
{

// Parses one non-negative decimal field of a timecode string: the field runs
// from `pos` to the next ':' (or the end of the string). Stores the value
// and advances `pos` past the field. Returns false on an empty or
// non-numeric field or on overflow.
bool parseTimecodeField(const std::string& s, size_t& pos, std::int64_t& value)
{
    const size_t end = s.find(':', pos);
    const size_t stop = (end == std::string::npos) ? s.size() : end;
    if (stop == pos)
    {
        return false; // empty field
    }
    std::int64_t v = 0;
    for (size_t i = pos; i < stop; ++i)
    {
        const char c = s[i];
        if (c < '0' || c > '9')
        {
            return false;
        }
        const std::int64_t digit = c - '0';
        if (v > (std::numeric_limits<std::int64_t>::max() - digit) / 10)
        {
            return false; // overflow: not a countable timecode
        }
        v = v * 10 + digit;
    }
    value = v;
    pos = stop;
    return true;
}

} // namespace

std::int64_t timecodeToFrames(const std::string& timecode, std::int64_t nominalFps)
{
    if (nominalFps <= 0 || timecode.empty() || timecode.front() == '-')
    {
        return -1;
    }
    std::int64_t fields[4] = {0, 0, 0, 0};
    size_t pos = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (!parseTimecodeField(timecode, pos, fields[i]))
        {
            return -1;
        }
        if (i < 3)
        {
            if (pos >= timecode.size() || timecode[pos] != ':')
            {
                return -1; // missing ':' separator
            }
            ++pos;
        }
    }
    if (pos != timecode.size())
    {
        return -1; // trailing garbage (e.g. "00:00:00:00:00")
    }
    const std::int64_t hh = fields[0], mm = fields[1], ss = fields[2], ff = fields[3];
    if (mm >= 60 || ss >= 60 || ff >= nominalFps)
    {
        return -1;
    }
    return ((hh * 3600 + mm * 60 + ss) * nominalFps) + ff;
}

std::string framesToTimecode(std::int64_t frames, std::int64_t nominalFps)
{
    if (frames < 0 || nominalFps <= 0)
    {
        return "-00:00:00:00";
    }
    const std::int64_t totalSeconds = frames / nominalFps;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld:%02lld",
                  static_cast<long long>(totalSeconds / 3600),
                  static_cast<long long>((totalSeconds / 60) % 60),
                  static_cast<long long>(totalSeconds % 60),
                  static_cast<long long>(frames % nominalFps));
    return buf;
}

bool parseFrameRateString(const std::string& text, double& framesPerSecond, std::int64_t& nominalFps)
{
    // A plain decimal number (digits, at most one '.'): stod would also
    // accept "inf", hex floats and skip leading whitespace, none of which
    // the protocol ever sends.
    size_t dots = 0;
    bool haveDigit = false;
    for (const char c : text)
    {
        if (c == '.')
        {
            ++dots;
        }
        else if (c >= '0' && c <= '9')
        {
            haveDigit = true;
        }
        else
        {
            return false;
        }
    }
    if (!haveDigit || dots > 1)
    {
        return false;
    }
    try
    {
        framesPerSecond = std::stod(text);
    }
    catch (const std::out_of_range&)
    {
        return false;
    }
    if (!std::isfinite(framesPerSecond) || framesPerSecond <= 0.0)
    {
        return false;
    }
    nominalFps = std::llround(framesPerSecond);
    return nominalFps > 0;
}

} // namespace mediamax
