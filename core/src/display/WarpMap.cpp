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

// 2D cross product (the z component of the 3D cross).
float cross2(float ax, float ay, float bx, float by)
{
    return ax * by - ay * bx;
}

// One lattice cell of the inverse build: the warped corner positions (the
// surface passes exactly through the nodes in both interpolation modes)
// expressed as a local bilinear frame A + b*u + c*v + d*u*v (u, v in
// [0,1]^2), its bounding box (a superset of the cell's image: the cell
// quadrilateral for bilinear, the 3x3 control neighbourhood for spline -
// the B-spline convex-hull property) and the ideal UV of the (0,0) corner.
struct InverseCell
{
    float minU, minV, maxU, maxV;
    float aU, aV; // corner (0,0) position
    float bU, bV; // corner (1,0) - corner (0,0)
    float cU, cV; // corner (0,1) - corner (0,0)
    float dU, dV; // corner(1,1) - corner(1,0) - corner(0,1) + corner(0,0)
    float iu, iv; // ideal UV of corner (0,0)
};

// Exact algebraic inverse of the bilinear (hyperbolic) cell map
// F(u,v) = A + b*u + c*v + d*u*v. With P = target - A = b*u + c*v + d*u*v
// and x = 2D cross product, crossing out one unknown at a time:
//     P x b = v * ((d x b) - (b x c))      =>  v = (P x b) / ((d x b) - (b x c))
//     P x c = u * ((b x c) - v * (c x d))  =>  u = (P x c) / ((b x c) - v * (c x d))
// The solution satisfies the equation exactly (no iteration) but may fall
// outside the unit cell (when the target is outside the cell's image): the
// caller validates the domain and the residual.
bool solveBilinearCell(const InverseCell& cell, float pu, float pv, float& lu, float& lv)
{
    const float px = pu - cell.aU;
    const float py = pv - cell.aV;
    const float dBc = cross2(cell.bU, cell.bV, cell.cU, cell.cV); // b x c
    const float dDb = cross2(cell.dU, cell.dV, cell.bU, cell.bV); // d x b
    const float dCd = cross2(cell.cU, cell.cV, cell.dU, cell.dV); // c x d
    const float denV = dDb - dBc;
    if (std::fabs(denV) < 1e-12f)
    {
        return false; // (near-)degenerate cell
    }
    lv = cross2(px, py, cell.bU, cell.bV) / denV;
    const float denU = dBc - lv * dCd;
    if (std::fabs(denU) < 1e-12f)
    {
        return false;
    }
    lu = cross2(px, py, cell.cU, cell.cV) / denU;
    return true;
}

// Newton iteration for F(q) = p (forward-difference Jacobian: 3 sample()
// calls per step, exact for the bilinear mode), seeded at (qu, qv). The
// iterate is clamped to the [0,1]^2 domain each step. Returns true when the
// residual drops below the tolerance (also re-checked after the loop).
bool newtonSolveInverse(const WarpMap& map, float pu, float pv, float& qu, float& qv)
{
    constexpr float kTol = 1e-5f; // residual tolerance (UV units)
    constexpr float kEps = 1e-4f; // finite-difference step
    constexpr int kMaxIter = 10;
    for (int i = 0; i < kMaxIter; ++i)
    {
        const auto [fu, fv] = map.sample(qu, qv);
        const float dx = fu - pu;
        const float dy = fv - pv;
        if (dx * dx + dy * dy < kTol * kTol)
        {
            return true;
        }
        const auto [fuU, fvU] = map.sample(qu + kEps, qv);
        const auto [fuV, fvV] = map.sample(qu, qv + kEps);
        const float a = (fuU - fu) / kEps; // dF_u/dq_u
        const float b = (fvU - fv) / kEps; // dF_v/dq_u
        const float c = (fuV - fu) / kEps; // dF_u/dq_v
        const float d = (fvV - fv) / kEps; // dF_v/dq_v
        const float det = a * d - b * c;
        if (std::fabs(det) < 1e-12f)
        {
            return false; // singular Jacobian (folded / degenerate lattice)
        }
        qu = std::clamp(qu - (d * dx - b * dy) / det, 0.f, 1.f);
        qv = std::clamp(qv - (-c * dx + a * dy) / det, 0.f, 1.f);
    }
    const auto [fu, fv] = map.sample(qu, qv);
    const float dx = fu - pu;
    const float dy = fv - pv;
    return dx * dx + dy * dy < 16.f * kTol * kTol;
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

void WarpMap::buildInverseDenseTexture(std::vector<unsigned char>& outData, int size) const
{
    if (size < 2)
    {
        throw std::invalid_argument("WarpMap::buildInverseDenseTexture: size must be >= 2");
    }
    outData.assign(static_cast<size_t>(size) * size * 4, 0);
    // Uncovered texels are black; the alpha channel is always 255.
    for (size_t i = 3; i < outData.size(); i += 4)
    {
        outData[i] = 255;
    }

    // The identity fast path: the inverse is the identity, fully covered.
    if (isIdentity())
    {
        for (int y = 0; y < size; ++y)
        {
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            const unsigned char gv = static_cast<unsigned char>(std::lround(v * 255.f));
            for (int x = 0; x < size; ++x)
            {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                const unsigned char ru =
                    static_cast<unsigned char>(std::lround(u * 255.f));
                unsigned char* px =
                    outData.data() + (static_cast<size_t>(y) * size + x) * 4;
                px[0] = ru;
                px[1] = gv;
                px[2] = 255;
            }
        }
        return;
    }

    // Node positions (the surface passes exactly through its nodes in both
    // interpolation modes) in output UV.
    const int cellsX = m_Cols - 1;
    const int cellsY = m_Rows - 1;
    const size_t nodeCount = static_cast<size_t>(m_Rows) * m_Cols;
    std::vector<float> nodeU(nodeCount), nodeV(nodeCount);
    for (int r = 0; r < m_Rows; ++r)
    {
        for (int c = 0; c < m_Cols; ++c)
        {
            const auto [iu, iv] = idealPoint(r, c, m_Rows, m_Cols);
            const size_t idx = static_cast<size_t>(r) * m_Cols + c;
            nodeU[idx] = iu + m_OffsetU[idx];
            nodeV[idx] = iv + m_OffsetV[idx];
        }
    }

    // Per lattice cell: the corner frame + the bounding box of its image
    // (the cell quadrilateral for bilinear; the 3x3 control neighbourhood
    // for spline - the B-spline convex-hull property keeps the cell's image
    // inside it). The box is the cheap candidate test: a point outside it
    // cannot be covered by the cell.
    std::vector<InverseCell> cells;
    cells.reserve(static_cast<size_t>(cellsX) * cellsY);
    for (int y = 0; y < cellsY; ++y)
    {
        for (int x = 0; x < cellsX; ++x)
        {
            const size_t i00 = static_cast<size_t>(y) * m_Cols + x;
            const size_t i10 = i00 + 1;
            const size_t i01 = i00 + m_Cols;
            const size_t i11 = i01 + 1;
            InverseCell cell;
            cell.aU = nodeU[i00];
            cell.aV = nodeV[i00];
            cell.bU = nodeU[i10] - cell.aU;
            cell.bV = nodeV[i10] - cell.aV;
            cell.cU = nodeU[i01] - cell.aU;
            cell.cV = nodeV[i01] - cell.aV;
            cell.dU = nodeU[i11] - nodeU[i10] - nodeU[i01] + nodeU[i00];
            cell.dV = nodeV[i11] - nodeV[i10] - nodeV[i01] + nodeV[i00];
            cell.iu = static_cast<float>(x) / static_cast<float>(cellsX);
            cell.iv = static_cast<float>(y) / static_cast<float>(cellsY);
            cell.minU = cell.maxU = nodeU[i00];
            cell.minV = cell.maxV = nodeV[i00];
            const auto absorb = [&](size_t idx) {
                cell.minU = std::min(cell.minU, nodeU[idx]);
                cell.maxU = std::max(cell.maxU, nodeU[idx]);
                cell.minV = std::min(cell.minV, nodeV[idx]);
                cell.maxV = std::max(cell.maxV, nodeV[idx]);
            };
            if (m_Interpolation == WarpInterpolation::Bilinear)
            {
                absorb(i10);
                absorb(i01);
                absorb(i11);
            }
            else
            {
                for (int r = -1; r <= 2; ++r)
                {
                    const int ry = std::clamp(y + r, 0, m_Rows - 1);
                    for (int c = -1; c <= 2; ++c)
                    {
                        const int cx = std::clamp(x + c, 0, m_Cols - 1);
                        absorb(static_cast<size_t>(ry) * m_Cols + cx);
                    }
                }
            }
            cells.push_back(cell);
        }
    }

    const float duStep = 1.f / static_cast<float>(cellsX);
    const float dvStep = 1.f / static_cast<float>(cellsY);
    constexpr float kBoxMargin = 1e-6f; // float slack for boundary texels

    for (int y = 0; y < size; ++y)
    {
        const float pv = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        for (int x = 0; x < size; ++x)
        {
            const float pu = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);

            for (const InverseCell& cell : cells)
            {
                if (pu < cell.minU - kBoxMargin || pu > cell.maxU + kBoxMargin ||
                    pv < cell.minV - kBoxMargin || pv > cell.maxV + kBoxMargin)
                {
                    continue;
                }

                // Seed: project p onto the cell's corner parallelogram (a
                // cheap stand-in for the bilinear weights of p).
                const float px = pu - cell.aU;
                const float py = pv - cell.aV;
                const float e11 = cell.bU * cell.bU + cell.bV * cell.bV;
                const float e12 = cell.bU * cell.cU + cell.bV * cell.cV;
                const float e22 = cell.cU * cell.cU + cell.cV * cell.cV;
                const float det = e11 * e22 - e12 * e12;
                float lu, lv;
                if (det > 1e-12f)
                {
                    const float puE = px * cell.bU + py * cell.bV;
                    const float pvE = px * cell.cU + py * cell.cV;
                    lu = (e22 * puE - e12 * pvE) / det;
                    lv = (e11 * pvE - e12 * puE) / det;
                    lu = std::clamp(lu, 0.f, 1.f);
                    lv = std::clamp(lv, 0.f, 1.f);
                }
                else
                {
                    lu = 0.5f;
                    lv = 0.5f;
                }
                float qu = cell.iu + lu * duStep;
                float qv = cell.iv + lv * dvStep;

                bool ok = false;
                if (m_Interpolation == WarpInterpolation::Bilinear)
                {
                    // Exact algebraic solve of the cell's bilinear map.
                    float luS, lvS;
                    if (solveBilinearCell(cell, pu, pv, luS, lvS) &&
                        luS >= -1e-4f && luS <= 1.f + 1e-4f && lvS >= -1e-4f &&
                        lvS <= 1.f + 1e-4f)
                    {
                        luS = std::clamp(luS, 0.f, 1.f);
                        lvS = std::clamp(lvS, 0.f, 1.f);
                        const float su = cell.iu + luS * duStep;
                        const float sv = cell.iv + lvS * dvStep;
                        const auto [fu, fv] = sample(su, sv);
                        const float dx = fu - pu;
                        const float dy = fv - pv;
                        if (dx * dx + dy * dy < 1e-4f * 1e-4f)
                        {
                            qu = su;
                            qv = sv;
                            ok = true;
                        }
                    }
                }
                if (!ok)
                {
                    // Newton (spline mode, or a near-degenerate bilinear
                    // cell the algebraic solve rejected).
                    ok = newtonSolveInverse(*this, pu, pv, qu, qv);
                }
                if (ok)
                {
                    unsigned char* pxl =
                        outData.data() + (static_cast<size_t>(y) * size + x) * 4;
                    pxl[0] = static_cast<unsigned char>(
                        std::clamp(static_cast<int>(std::lround(qu * 255.f)), 0, 255));
                    pxl[1] = static_cast<unsigned char>(
                        std::clamp(static_cast<int>(std::lround(qv * 255.f)), 0, 255));
                    pxl[2] = 255;
                    break; // a valid lattice assigns the point to one cell
                }
            }
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
