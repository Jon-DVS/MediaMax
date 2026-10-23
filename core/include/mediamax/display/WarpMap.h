#pragma once

// UV warp map for projection correction (projection mapping).
//
// A warp map is a grid of (rows x cols) control points. Each control point
// carries an offset, in output UV space, from its ideal position. The warp
// applied at any UV point is that point plus the bilinearly interpolated
// offset from the surrounding control points:
//
//     warped(uv) = uv + bilinear(interpolate offsets at uv)
//
// With all offsets zero the warp is the identity. Moving one corner control
// point shears only the region near that corner, which is the behavior
// projection-mapping tools (Resolume, WATCHOUT) use for lens correction and
// wall geometry.
//
// The class can also render the (small) control grid into a dense RG8
// texture that the GPU samples per-pixel; the texture stores the final warped
// UV per output texel, so the display shader needs a single texture fetch.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "mediamax/math/Rect.h"

namespace mediamax
{

class WarpMap
{
public:
    static constexpr int kDefaultRows = 11;
    static constexpr int kDefaultCols = 11;

    explicit WarpMap(int rows = kDefaultRows, int cols = kDefaultCols);

    int rows() const { return m_Rows; }
    int cols() const { return m_Cols; }

    // Ideal (unwarped) position of control point (row, col) in output UV.
    static std::pair<float, float> idealPoint(int row, int col, int rows, int cols);

    // Absolute target position of a control point in output UV (0..1).
    void setPointPosition(int row, int col, float u, float v);

    // Offset (delta from ideal position) of a control point in output UV.
    void setPointOffset(int row, int col, float du, float dv);

    float pointPositionU(int row, int col) const;
    float pointPositionV(int row, int col) const;

    // Full warp: input and output are output UV (0..1).
    std::pair<float, float> sample(float u, float v) const;

    void setIdentity();
    bool isIdentity() const;

    // Renders the dense warp texture (RG8, row-major, size x size) where each
    // texel stores the final warped UV for that output point. This is what
    // the GPU samples.
    void buildDenseTexture(std::vector<unsigned char>& outData, int size) const;

    // Serialization (JSON compatible plain values).
    std::vector<std::array<float, 2>> pointOffsets() const;

    bool operator==(const WarpMap& o) const;
    bool operator!=(const WarpMap& o) const;

private:
    int m_Rows;
    int m_Cols;
    // Offsets in output UV units, row-major [rows * cols].
    std::vector<float> m_OffsetU;
    std::vector<float> m_OffsetV;
};

} // namespace mediamax
