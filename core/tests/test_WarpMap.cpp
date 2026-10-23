// WarpMap: grid warp math and dense texture generation.

#include <stdexcept>

#include <doctest/doctest.h>

#include "mediamax/display/WarpMap.h"

using namespace mediamax;

TEST_SUITE("WarpMap")
{

TEST_CASE("Identity warp leaves points unchanged")
{
    const WarpMap warp(11, 11);
    CHECK(warp.isIdentity());
    for (float v = 0.f; v <= 1.f; v += 0.125f)
    {
        for (float u = 0.f; u <= 1.f; u += 0.125f)
        {
            const auto [wu, wv] = warp.sample(u, v);
            CHECK(wu == doctest::Approx(u).epsilon(1e-5));
            CHECK(wv == doctest::Approx(v).epsilon(1e-5));
        }
    }
}

TEST_CASE("Moving a corner point shears only its neighborhood")
{
    WarpMap warp(3, 3);
    // Move the top-right corner (row 0, col 2) 0.1 left.
    const auto [iu, iv] = WarpMap::idealPoint(0, 2, 3, 3);
    warp.setPointPosition(0, 2, iu - 0.1f, iv);

    // The corner itself moves exactly 0.1 left.
    const auto [cu, cv] = warp.sample(1.f, 0.f);
    CHECK(cu == doctest::Approx(0.9f).epsilon(1e-5));
    CHECK(cv == doctest::Approx(0.f).epsilon(1e-5));

    // The opposite corner is unaffected.
    const auto [ou, ov] = warp.sample(0.f, 0.f);
    CHECK(ou == doctest::Approx(0.f).epsilon(1e-5));
    CHECK(ov == doctest::Approx(0.f).epsilon(1e-5));

    // A point halfway into the affected cell is displaced half as much
    // (bilinear). Note: u=0.5 sits on the untouched middle control point.
    const auto [mu, mv] = warp.sample(0.75f, 0.f);
    CHECK(mu == doctest::Approx(0.75f - 0.05f).epsilon(1e-5));
    CHECK(mv == doctest::Approx(0.f).epsilon(1e-5));
}

TEST_CASE("Offset is additive with the identity point")
{
    WarpMap warp(5, 5);
    warp.setPointOffset(2, 2, 0.05f, -0.02f); // center point

    const auto [centerIdealU, centerIdealV] = WarpMap::idealPoint(2, 2, 5, 5);
    const auto [u, v] = warp.sample(centerIdealU, centerIdealV);
    CHECK(u == doctest::Approx(centerIdealU + 0.05f).epsilon(1e-5));
    CHECK(v == doctest::Approx(centerIdealV - 0.02f).epsilon(1e-5));
}

TEST_CASE("Set and get point positions agree")
{
    WarpMap warp(4, 4);
    warp.setPointPosition(0, 0, 0.02f, 0.03f);
    warp.setPointPosition(3, 3, 0.98f, 0.97f);

    CHECK(warp.pointPositionU(0, 0) == doctest::Approx(0.02f).epsilon(1e-6));
    CHECK(warp.pointPositionV(0, 0) == doctest::Approx(0.03f).epsilon(1e-6));
    CHECK(warp.pointPositionU(3, 3) == doctest::Approx(0.98f).epsilon(1e-6));
    CHECK(warp.pointPositionV(3, 3) == doctest::Approx(0.97f).epsilon(1e-6));

    CHECK(warp.pointOffsets().size() == 16);
    CHECK_FALSE(warp.isIdentity());
}

TEST_CASE("Dense texture encodes the warp")
{
    WarpMap warp(11, 11);
    warp.setPointPosition(0, 0, 0.01f, 0.01f); // shift top-left corner slightly

    std::vector<unsigned char> tex;
    const int size = 64;
    warp.buildDenseTexture(tex, size);
    CHECK(tex.size() == static_cast<size_t>(size) * size * 2);

    // Center texel of the identity-ish region is close to its own uv.
    const int cx = size / 2, cy = size / 2;
    const float texU = tex[(cy * size + cx) * 2] / 255.f;
    const float texV = tex[(cy * size + cx) * 2 + 1] / 255.f;
    const auto [sampleU, sampleV] = warp.sample((cx + 0.5f) / size, (cy + 0.5f) / size);
    CHECK(texU == doctest::Approx(sampleU).epsilon(1.f / 255.f + 1e-6));
    CHECK(texV == doctest::Approx(sampleV).epsilon(1.f / 255.f + 1e-6));

    // Identity warp texture is the diagonal ramp.
    const WarpMap identity(11, 11);
    std::vector<unsigned char> idTex;
    identity.buildDenseTexture(idTex, 4);
    // corner (0,0) texel stores ~ (0.5/4, 0.5/4) = 0.125 -> ~32
    CHECK(idTex[0] == doctest::Approx(32).epsilon(1));
    CHECK(idTex[1] == doctest::Approx(32).epsilon(1));
    // top-right texel (x=3,y=0): u=3.5/4=0.875 -> ~223, v ~32
    CHECK(idTex[(0 * 4 + 3) * 2] == doctest::Approx(223).epsilon(1));
    CHECK(idTex[(0 * 4 + 3) * 2 + 1] == doctest::Approx(32).epsilon(1));
}

TEST_CASE("Grid boundaries: control points sit exactly on 0 and 1")
{
    CHECK(WarpMap::idealPoint(0, 0, 11, 11).first == doctest::Approx(0.f));
    CHECK(WarpMap::idealPoint(0, 10, 11, 11).first == doctest::Approx(1.f));
    CHECK(WarpMap::idealPoint(10, 0, 11, 11).second == doctest::Approx(1.f));
    CHECK(WarpMap::idealPoint(5, 5, 11, 11).first == doctest::Approx(0.5f));
}

TEST_CASE("WarpMap rejects degenerate grids")
{
    CHECK_THROWS_AS(WarpMap(1, 1), std::invalid_argument);
    CHECK_THROWS_AS(WarpMap(2, 1), std::invalid_argument);
}

} // TEST_SUITE("WarpMap")
