// Blend mode CPU reference: identities, boundaries, and consistency.

#include <doctest/doctest.h>

#include "mediamax/blend/Blend.h"

using namespace mediamax;

TEST_SUITE("Blend")
{

TEST_CASE("Normal: opaque source covers destination")
{
    const Color4 dst{0.1f, 0.2f, 0.3f, 1.f};
    const Color4 src{0.9f, 0.8f, 0.7f, 1.f};
    const Color4 out = blend::apply(dst, src, BlendMode::Normal);
    CHECK(out.r == doctest::Approx(0.9f));
    CHECK(out.g == doctest::Approx(0.8f));
    CHECK(out.b == doctest::Approx(0.7f));
    CHECK(out.a == doctest::Approx(1.f));
}

TEST_CASE("Normal: transparent source leaves destination untouched")
{
    const Color4 dst{0.1f, 0.2f, 0.3f, 1.f};
    const Color4 src{0.9f, 0.8f, 0.7f, 0.f};
    const Color4 out = blend::apply(dst, src, BlendMode::Normal);
    CHECK(out.r == doctest::Approx(0.1f));
    CHECK(out.g == doctest::Approx(0.2f));
    CHECK(out.b == doctest::Approx(0.3f));
    CHECK(out.a == doctest::Approx(1.f));
}

TEST_CASE("Normal: 50% alpha averages opaque colors")
{
    const Color4 dst{0.0f, 0.0f, 0.0f, 1.f};
    const Color4 src{1.f, 1.f, 1.f, 0.5f};
    const Color4 out = blend::apply(dst, src, BlendMode::Normal);
    CHECK(out.r == doctest::Approx(0.5f));
    CHECK(out.a == doctest::Approx(1.f));
}

TEST_CASE("Add: additive light saturates")
{
    const Color4 dst{0.5f, 0.0f, 0.0f, 1.f};
    const Color4 src{0.5f, 1.f, 0.5f, 1.f};
    const Color4 out = blend::apply(dst, src, BlendMode::Add);
    CHECK(out.r == doctest::Approx(1.f));
    CHECK(out.g == doctest::Approx(1.f));
    CHECK(out.b == doctest::Approx(0.5f));
}

TEST_CASE("Add: adding to black yields the source")
{
    const Color4 dst{0.f, 0.f, 0.f, 1.f};
    const Color4 src{0.25f, 0.5f, 0.75f, 1.f};
    const Color4 out = blend::apply(dst, src, BlendMode::Add);
    CHECK(out.r == doctest::Approx(0.25f));
    CHECK(out.g == doctest::Approx(0.5f));
    CHECK(out.b == doctest::Approx(0.75f));
}

TEST_CASE("Subtract: subtracting a color from white inverts it")
{
    const Color4 dst{1.f, 1.f, 1.f, 1.f};
    const Color4 src{0.25f, 0.5f, 0.75f, 1.f};
    const Color4 out = blend::apply(dst, src, BlendMode::Subtract);
    CHECK(out.r == doctest::Approx(0.75f));
    CHECK(out.g == doctest::Approx(0.5f));
    CHECK(out.b == doctest::Approx(0.25f));
}

TEST_CASE("Subtract: clamps at zero")
{
    const Color4 dst{0.2f, 0.2f, 0.2f, 1.f};
    const Color4 src{0.5f, 0.1f, 0.1f, 1.f};
    const Color4 out = blend::apply(dst, src, BlendMode::Subtract);
    CHECK(out.r == doctest::Approx(0.f));
    CHECK(out.g == doctest::Approx(0.1f));
}

TEST_CASE("Multiply: black multiplies to black, white preserves")
{
    const Color4 dst{0.8f, 0.4f, 0.2f, 1.f};
    const Color4 black{0.f, 0.f, 0.f, 1.f};
    const Color4 white{1.f, 1.f, 1.f, 1.f};

    const Color4 mulBlack = blend::apply(dst, black, BlendMode::Multiply);
    CHECK(mulBlack.r == doctest::Approx(0.f));

    const Color4 mulWhite = blend::apply(dst, white, BlendMode::Multiply);
    CHECK(mulWhite.r == doctest::Approx(0.8f));
    CHECK(mulWhite.g == doctest::Approx(0.4f));
    CHECK(mulWhite.b == doctest::Approx(0.2f));
}

TEST_CASE("Screen: screen of black is identity, screen of white saturates")
{
    const Color4 dst{0.3f, 0.4f, 0.5f, 1.f};
    const Color4 black{0.f, 0.f, 0.f, 1.f};
    const Color4 white{1.f, 1.f, 1.f, 1.f};

    const Color4 scrBlack = blend::apply(dst, black, BlendMode::Screen);
    CHECK(scrBlack.r == doctest::Approx(0.3f));

    const Color4 scrWhite = blend::apply(dst, white, BlendMode::Screen);
    CHECK(scrWhite.r == doctest::Approx(1.f));
}

TEST_CASE("Screen: formula matches 1 - (1-d)(1-s) for opaque colors")
{
    const Color4 dst{0.2f, 0.3f, 0.4f, 1.f};
    const Color4 src{0.5f, 0.6f, 0.7f, 1.f};
    const Color4 out = blend::apply(dst, src, BlendMode::Screen);
    CHECK(out.r == doctest::Approx(1.f - (1.f - 0.2f) * (1.f - 0.5f)));
    CHECK(out.g == doctest::Approx(1.f - (1.f - 0.3f) * (1.f - 0.6f)));
    CHECK(out.b == doctest::Approx(1.f - (1.f - 0.4f) * (1.f - 0.7f)));
}

TEST_CASE("Alpha: compositing two 50% layers gives 75% over transparent")
{
    const Color4 dst{1.f, 1.f, 1.f, 0.5f};
    const Color4 src{1.f, 1.f, 1.f, 0.5f};
    const Color4 out = blend::apply(dst, src, BlendMode::Normal);
    CHECK(out.a == doctest::Approx(0.75f));
}

TEST_CASE("String round trip for all modes")
{
    for (auto mode : {BlendMode::Normal, BlendMode::Add, BlendMode::Subtract, BlendMode::Multiply,
                      BlendMode::Screen})
    {
        CHECK(blend::fromString(blend::toString(mode)) == mode);
    }
    CHECK(blend::fromString("garbage") == BlendMode::Normal);
}

} // TEST_SUITE("Blend")
