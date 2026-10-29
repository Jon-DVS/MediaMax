#pragma once

// UV warp map for projection correction (projection mapping).
//
// A warp map is a grid of (rows x cols) control points. Each control point
// carries an offset, in output UV space, from its ideal position. The warp
// applied at any UV point is that point plus an interpolation of the
// surrounding control points' offsets:
//
//     warped(uv) = uv + interpolate(offsets at uv)
//
// With all offsets zero the warp is the identity. Moving one corner control
// point shears only the region near that corner, which is the behavior
// projection-mapping tools (Resolume, WATCHOUT) use for lens correction and
// wall geometry.
//
// Two interpolations between the control points (WarpInterpolation):
//
//   Bilinear - the classic grid warp: each output point takes the weighted
//     average of the four surrounding offsets. Piecewise bilinear.
//
//   Spline - a tensor-product cubic B-spline through the node offsets in
//     Catmull-Rom form, so the surface passes exactly through every
//     control point (the node's target position is exact) and is C1
//     continuous between them. Boundary nodes are clamped (duplicated),
//     which keeps the surface anchored to the grid edges. The smooth
//     option, for projector lens correction and curved walls.
//
// The class can also render the (small) control grid into a dense RG8
// texture that the GPU samples per-pixel; the texture stores the final warped
// UV per output texel, so the display shader needs a single texture fetch
// (the interpolation scheme is entirely CPU-side).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "mediamax/math/Rect.h"

namespace mediamax
{

// Interpolation between a WarpMap's control points (see the class comment).
enum class WarpInterpolation
{
    Bilinear,
    Spline
};

class WarpMap
{
public:
    static constexpr int kDefaultRows = 11;
    static constexpr int kDefaultCols = 11;

    explicit WarpMap(int rows = kDefaultRows, int cols = kDefaultCols,
                     WarpInterpolation interpolation = WarpInterpolation::Bilinear);

    int rows() const { return m_Rows; }
    int cols() const { return m_Cols; }
    WarpInterpolation interpolation() const { return m_Interpolation; }

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

    // Absolute target positions of all control points in output UV
    // (row-major) - the same layout the protocol's display.warp.set
    // `points` parameter uses.
    std::vector<std::array<float, 2>> pointPositions() const;

    bool operator==(const WarpMap& o) const;
    bool operator!=(const WarpMap& o) const;

private:
    // One offset channel evaluated at output UV (u, v) with the
    // tensor-product Catmull-Rom interpolation (the Spline mode).
    float splineChannel(const std::vector<float>& channel, float u, float v) const;

    int m_Rows;
    int m_Cols;
    WarpInterpolation m_Interpolation;
    // Offsets in output UV units, row-major [rows * cols].
    std::vector<float> m_OffsetU;
    std::vector<float> m_OffsetV;
};

} // namespace mediamax
