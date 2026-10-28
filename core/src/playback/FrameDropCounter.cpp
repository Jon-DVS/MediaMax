// FrameDropCounter: exact frame-step arithmetic for the drop metric.

#include "mediamax/playback/FrameDropCounter.h"

#include <cmath>
#include <limits>

namespace mediamax
{

namespace
{

// The products below are exact for every time this project produces
// (frame-based rationals: value = frame count, scale = fps; or
// microsecond-based: scale <= 1e6). Guard the multiplication anyway: if
// the rational parts are large enough to overflow int64, fall back to
// double (the count is a metric, not a correctness input).
std::int64_t flooredRatio(std::int64_t num, std::int64_t den)
{
    if (num == 0 || den < std::int64_t(1))
    {
        return 0;
    }
    if (num <= std::numeric_limits<std::int64_t>::max() / den)
    {
        return num / den;
    }
    return static_cast<std::int64_t>(std::floor(
        static_cast<double>(num) / static_cast<double>(den)));
}

} // namespace

std::int64_t FrameDropCounter::frameSteps(const Time& from, const Time& to, const Time& step)
{
    if (to <= from || !step.isValid() || step.value() <= 0)
    {
        return 0;
    }
    const Time d = to - from;
    // floor((to - from) / step) = floor(d.value * step.scale / (d.scale * step.value))
    return flooredRatio(d.value() * step.scale(), d.scale() * step.value());
}

std::uint64_t FrameDropCounter::advance(const Time& target, const std::optional<Time>& displayed,
                                        const Time& frameStep)
{
    if (frameStep.isValid() && frameStep.value() > 0)
    {
        m_FrameStep = frameStep;
    }

    if (!m_Have)
    {
        m_Have = true;
        m_LastTarget = target;
        if (displayed)
        {
            m_HaveDisplayed = true;
            m_LastDisplayed = *displayed;
        }
        return 0;
    }

    // Backward jump of the playhead (loop wrap, seek): the frames in between
    // are not drops - restart the accounting against the new position.
    if (target < m_LastTarget)
    {
        m_LastTarget = target;
        if (displayed)
        {
            m_HaveDisplayed = true;
            m_LastDisplayed = *displayed;
        }
        return 0;
    }

    std::uint64_t dropped = 0;
    if (m_HaveDisplayed)
    {
        // How many frames became due while the playhead advanced...
        const std::int64_t due = frameSteps(m_LastTarget, target, frameStep);
        // ...and how many new frames actually reached the screen. A
        // nullopt display (starvation: nothing at or before the target)
        // counts as zero; a repeated frame counts as zero.
        const std::int64_t shown =
            displayed ? frameSteps(m_LastDisplayed, *displayed, frameStep) : 0;
        const std::int64_t delta = due - shown;
        if (delta > 0)
        {
            dropped = static_cast<std::uint64_t>(delta);
            m_Total += dropped;
        }
    }
    if (displayed)
    {
        m_HaveDisplayed = true;
        m_LastDisplayed = *displayed;
    }

    m_LastTarget = target;
    return dropped;
}

std::int64_t FrameDropCounter::backlog() const
{
    if (!m_Have || !m_HaveDisplayed)
    {
        return 0;
    }
    return frameSteps(m_LastDisplayed, m_LastTarget, m_FrameStep);
}

} // namespace mediamax
