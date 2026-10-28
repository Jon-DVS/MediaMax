#pragma once

// Counts video frames that were due for display but were never shown.
//
// The caller (the media pump's video delivery path) invokes advance() on
// every presentation pass for a layer, passing:
//   - target:    the layer's target media time (playhead position in the
//                media's timeline, loop already applied by the engine)
//   - displayed: the media time of the frame that is actually on screen
//                now (the newest decoded frame at or before the target),
//                nullopt when no frame has been displayed yet
//   - frameStep: the duration of one media frame (Time::fromFrames(1, fps))
//
// In steady state (the presentation shows one new frame per playhead step)
// the count stays at 0. When presentation lags the playhead - frames were
// decoded but never made it to the screen, or the same frame is shown
// repeatedly while the playhead runs on - each missed frame is counted
// exactly once. When presentation catches up, the backlog shrinks without
// being counted again. A backward jump of the target (loop wrap, seek)
// restarts the accounting.
//
// All math is exact (integer arithmetic on the rational Time values); the
// count is a high-water total that never decreases.

#include <cstdint>
#include <optional>

#include "mediamax/time/Time.h"

namespace mediamax
{

class FrameDropCounter
{
public:
    // Records one presentation pass. Returns the number of frames counted
    // as dropped by this call (0 in steady state).
    std::uint64_t advance(const Time& target, const std::optional<Time>& displayed,
                          const Time& frameStep);

    // Total frames dropped so far (high water, never decreases).
    std::uint64_t total() const { return m_Total; }

    // Current display lag in whole frames (0 when nothing has been
    // displayed yet): how far the on-screen frame sits behind the target.
    std::int64_t backlog() const;

    void reset()
    {
        m_Have = false;
        m_HaveDisplayed = false;
        m_LastTarget = Time();
        m_LastDisplayed = Time();
        m_FrameStep = Time();
        m_Total = 0;
    }

private:
    // The number of whole frame steps between `from` and `to` (0 when
    // `to <= from` or the step is invalid), exact integer math.
    static std::int64_t frameSteps(const Time& from, const Time& to, const Time& step);

    bool m_Have = false;
    bool m_HaveDisplayed = false;
    Time m_LastTarget;
    Time m_LastDisplayed;
    Time m_FrameStep; // last step passed to advance() (stable per media)
    std::uint64_t m_Total = 0;
};

} // namespace mediamax
