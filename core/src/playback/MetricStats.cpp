#include "mediamax/playback/MetricStats.h"

#include <algorithm>

namespace mediamax
{

MetricStats::MetricStats(std::size_t window)
    : m_Window(window == 0 ? 1 : window)
{
}

void MetricStats::record(double value)
{
    if (m_Samples.size() < m_Window)
    {
        m_Samples.push_back(value); // filling
    }
    else
    {
        m_Sum -= m_Samples[m_Next]; // evict the oldest
        m_Samples[m_Next] = value;
    }
    m_Next = (m_Next + 1) % m_Window;
    m_Sum += value;
    ++m_Count;
}

double MetricStats::current() const
{
    if (m_Samples.empty())
    {
        return 0.0;
    }
    // The newest sample sits just before the next write slot.
    const std::size_t idx =
        (m_Next + m_Samples.size() - 1) % m_Samples.size();
    return m_Samples[idx];
}

double MetricStats::min() const
{
    if (m_Samples.empty())
    {
        return 0.0;
    }
    return *std::min_element(m_Samples.begin(), m_Samples.end());
}

double MetricStats::max() const
{
    if (m_Samples.empty())
    {
        return 0.0;
    }
    return *std::max_element(m_Samples.begin(), m_Samples.end());
}

double MetricStats::avg() const
{
    if (m_Samples.empty())
    {
        return 0.0;
    }
    return m_Sum / static_cast<double>(m_Samples.size());
}

} // namespace mediamax
