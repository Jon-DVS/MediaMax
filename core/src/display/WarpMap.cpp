#include "mediamax/display/WarpMap.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mediamax
{

WarpMap::WarpMap(int rows, int cols)
    : m_Rows(rows)
    , m_Cols(cols)
    , m_OffsetU(static_cast<size_t>(rows) * cols, 0.f)
    , m_OffsetV(static_cast<size_t>(rows) * cols, 0.f)
{
    if (rows < 2 || cols < 2)
    {
        throw std::invalid_argument("WarpMap needs at least a 2x2 grid");
    }
}

std::pair<float, float> WarpMap::idealPoint(int row, int col, int rows, int cols)
{
    // Control points sit on the boundary: first/last rows and columns map to
    // 0 and 1 in output UV.
    const float u = cols > 1 ? static_cast<float>(col) / static_cast<float>(cols - 1) : 0.f;
    const float v = rows > 1 ? static_cast<float>(row) / static_cast<float>(rows - 1) : 0.f;
    return {u, v};
}

void WarpMap::setPointPosition(int row, int col, float u, float v)
{
    if (row < 0 || row >= m_Rows || col < 0 || col >= m_Cols)
    {
        throw std::out_of_range("WarpMap::setPointPosition: point out of range");
    }
    const auto [iu, iv] = idealPoint(row, col, m_Rows, m_Cols);
    const size_t idx = static_cast<size_t>(row) * m_Cols + col;
    m_OffsetU[idx] = u - iu;
    m_OffsetV[idx] = v - iv;
}

void WarpMap::setPointOffset(int row, int col, float du, float dv)
{
    if (row < 0 || row >= m_Rows || col < 0 || col >= m_Cols)
    {
        throw std::out_of_range("WarpMap::setPointOffset: point out of range");
    }
    const size_t idx = static_cast<size_t>(row) * m_Cols + col;
    m_OffsetU[idx] = du;
    m_OffsetV[idx] = dv;
}

float WarpMap::pointPositionU(int row, int col) const
{
    const auto [iu, iv] = idealPoint(row, col, m_Rows, m_Cols);
    const size_t idx = static_cast<size_t>(row) * m_Cols + col;
    return iu + m_OffsetU[idx];
}

float WarpMap::pointPositionV(int row, int col) const
{
    const auto [iu, iv] = idealPoint(row, col, m_Rows, m_Cols);
    const size_t idx = static_cast<size_t>(row) * m_Cols + col;
    return iv + m_OffsetV[idx];
}

std::pair<float, float> WarpMap::sample(float u, float v) const
{
    // Map the query into grid space.
    const float gx = std::clamp(u, 0.f, 1.f) * static_cast<float>(m_Cols - 1);
    const float gy = std::clamp(v, 0.f, 1.f) * static_cast<float>(m_Rows - 1);

    const int x0 = std::min(static_cast<int>(std::floor(gx)), m_Cols - 2);
    const int y0 = std::min(static_cast<int>(std::floor(gy)), m_Rows - 2);
    const float fx = gx - static_cast<float>(x0);
    const float fy = gy - static_cast<float>(y0);

    const size_t i00 = static_cast<size_t>(y0) * m_Cols + x0;
    const size_t i10 = i00 + 1;
    const size_t i01 = i00 + m_Cols;
    const size_t i11 = i01 + 1;

    const float du = (m_OffsetU[i00] * (1.f - fx) + m_OffsetU[i10] * fx) * (1.f - fy) +
                     (m_OffsetU[i01] * (1.f - fx) + m_OffsetU[i11] * fx) * fy;
    const float dv = (m_OffsetV[i00] * (1.f - fx) + m_OffsetV[i10] * fx) * (1.f - fy) +
                     (m_OffsetV[i01] * (1.f - fx) + m_OffsetV[i11] * fx) * fy;

    return {u + du, v + dv};
}

void WarpMap::setIdentity()
{
    std::fill(m_OffsetU.begin(), m_OffsetU.end(), 0.f);
    std::fill(m_OffsetV.begin(), m_OffsetV.end(), 0.f);
}

bool WarpMap::isIdentity() const
{
    for (size_t i = 0; i < m_OffsetU.size(); ++i)
    {
        if (m_OffsetU[i] != 0.f || m_OffsetV[i] != 0.f)
        {
            return false;
        }
    }
    return true;
}

void WarpMap::buildDenseTexture(std::vector<unsigned char>& outData, int size) const
{
    if (size < 2)
    {
        throw std::invalid_argument("WarpMap::buildDenseTexture: size must be >= 2");
    }
    outData.assign(static_cast<size_t>(size) * size * 2, 0);

    for (int y = 0; y < size; ++y)
    {
        // v = 0 at the top row (matches the shader's texture orientation).
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        for (int x = 0; x < size; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const auto [wu, wv] = sample(u, v);

            const unsigned char ru = static_cast<unsigned char>(
                std::clamp(static_cast<int>(std::lround(wu * 255.f)), 0, 255));
            const unsigned char gv = static_cast<unsigned char>(
                std::clamp(static_cast<int>(std::lround(wv * 255.f)), 0, 255));

            const size_t idx = (static_cast<size_t>(y) * size + x) * 2;
            outData[idx] = ru;
            outData[idx + 1] = gv;
        }
    }
}

std::vector<std::array<float, 2>> WarpMap::pointOffsets() const
{
    std::vector<std::array<float, 2>> out;
    out.reserve(m_OffsetU.size());
    for (size_t i = 0; i < m_OffsetU.size(); ++i)
    {
        out.push_back({m_OffsetU[i], m_OffsetV[i]});
    }
    return out;
}

bool WarpMap::operator==(const WarpMap& o) const
{
    return m_Rows == o.m_Rows && m_Cols == o.m_Cols && m_OffsetU == o.m_OffsetU &&
           m_OffsetV == o.m_OffsetV;
}

bool WarpMap::operator!=(const WarpMap& o) const { return !(*this == o); }

} // namespace mediamax
