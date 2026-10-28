#pragma once

// Rolling statistics for a fast-changing metric: the current sample plus
// the min, max and average over the window of most recent samples.
//
// The playback status overlay shows per-frame metrics (fps, CPU/GPU frame
// time) whose per-frame value jitters too fast to read. The rolling
// min/max/avg columns move slowly and are stable enough to glance at; a
// one-off spike (startup, a window resize) ages out of the window instead
// of pinning an all-time minimum/maximum.

#include <cstddef>
#include <vector>

namespace mediamax
{

class MetricStats
{
public:
    // `window` is the number of most recent samples retained (>= 1). The
    // default of 600 samples is ~10 s at 60 fps.
    explicit MetricStats(std::size_t window = 600);

    // Record a new sample; the oldest is evicted once the window is full.
    void record(double value);

    // Samples recorded since construction (0 before the first record).
    std::size_t count() const
    {
        return m_Count;
    }

    // The most recent sample (0 when no sample has been recorded).
    double current() const;

    // Min / max / average over the retained window (all 0 when empty).
    double min() const;
    double max() const;
    double avg() const;

private:
    std::size_t m_Window;
    // Ring buffer of the retained samples (grown to m_Window while filling,
    // then holding exactly the window).
    std::vector<double> m_Samples;
    std::size_t m_Next = 0;  // next write slot (== m_Samples.size() while filling)
    std::size_t m_Count = 0; // total samples recorded
    double m_Sum = 0.0;
};

} // namespace mediamax
