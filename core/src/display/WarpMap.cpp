#include "mediamax/display/WarpMap.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mediamax
{

namespace
{

// One dimension of the Catmull-Rom basis (a uniform cubic B-spline in
// interpolating form): the value at t in [0, 1] between nodes p1 and p2,
// given the neighbouring nodes p0 and p3. Exact at the nodes (t = 0 -> p1,
// t = 1 -> p2) and exact for linear data.
float catmullRom(float p0, float p1, float p2, float p3, float t)
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * ((2.f * p1) + (-p0 + p2) * t + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * t2 +
                   (-p0 + 3.f * p1 - 3.f * p2 + p3) * t3);
}

} // namespace

WarpMap::WarpMap(int rows, int cols, WarpInterpolation interpolation)
    : m_Rows(rows)
    , m_Cols(cols)
    , m_Interpolation(interpolation)
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
    if (m_Interpolation == WarpInterpolation::Spline)
    {
        return {u + splineChannel(m_OffsetU, u, v), v + splineChannel(m_OffsetV, u, v)};
    }

    // Bilinear: map the query into grid space.
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

float WarpMap::splineChannel(const std::vector<float>& channel, float u, float v) const
{
    // Map the query into grid space (same cell convention as the bilinear
    // path).
    const float gx = std::clamp(u, 0.f, 1.f) * static_cast<float>(m_Cols - 1);
    const float gy = std::clamp(v, 0.f, 1.f) * static_cast<float>(m_Rows - 1);

    const int x0 = std::min(static_cast<int>(std::floor(gx)), m_Cols - 2);
    const int y0 = std::min(static_cast<int>(std::floor(gy)), m_Rows - 2);
    const float fx = gx - static_cast<float>(x0);
    const float fy = gy - static_cast<float>(y0);

    // Tensor product: interpolate each of the four surrounding rows across
    // its (boundary-clamped) column neighbourhood, then interpolate the row
    // results the same way. Clamping duplicates the edge nodes, so the
    // surface passes exactly through the boundary nodes too.
    float rows[4];
    for (int r = -1; r <= 2; ++r)
    {
        const int y = std::clamp(y0 + r, 0, m_Rows - 1);
        float cols[4];
        for (int c = -1; c <= 2; ++c)
        {
            const int x = std::clamp(x0 + c, 0, m_Cols - 1);
            cols[static_cast<size_t>(c + 1)] = channel[static_cast<size_t>(y) * m_Cols + x];
        }
        rows[static_cast<size_t>(r + 1)] = catmullRom(cols[0], cols[1], cols[2], cols[3], fx);
    }
    return catmullRom(rows[0], rows[1], rows[2], rows[3], fy);
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

std::vector<std::array<float, 2>> WarpMap::pointPositions() const
{
    std::vector<std::array<float, 2>> out;
    out.reserve(m_OffsetU.size());
    for (int r = 0; r < m_Rows; ++r)
    {
        for (int c = 0; c < m_Cols; ++c)
        {
            out.push_back({pointPositionU(r, c), pointPositionV(r, c)});
        }
    }
    return out;
}

bool WarpMap::operator==(const WarpMap& o) const
{
    return m_Rows == o.m_Rows && m_Cols == o.m_Cols && m_Interpolation == o.m_Interpolation &&
           m_OffsetU == o.m_OffsetU && m_OffsetV == o.m_OffsetV;
}

bool WarpMap::operator!=(const WarpMap& o) const { return !(*this == o); }

} // namespace mediamax
