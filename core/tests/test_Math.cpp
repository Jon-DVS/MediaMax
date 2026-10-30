// Transform2D and Rect2i behavior.

#include <doctest/doctest.h>

#include "mediamax/math/Rect.h"
#include "mediamax/math/Transform2D.h"

using namespace mediamax;

TEST_SUITE("Math")
{

TEST_CASE("Transform: identity maps media top-left to position (default anchor 0,0)")
{
    Transform2D t = Transform2D::identity();
    t.x = 100.f;
    t.y = 200.f;
    // The default anchor is the media's top-left: it lands on (x, y).
    const auto [px, py] = t.toOutputSpace(0.f, 0.f, Size2i{320, 180});
    CHECK(px == doctest::Approx(100.f));
    CHECK(py == doctest::Approx(200.f));
    // The media center is offset by half the media size.
    const auto [cx, cy] = t.toOutputSpace(0.5f, 0.5f, Size2i{320, 180});
    CHECK(cx == doctest::Approx(260.f));
    CHECK(cy == doctest::Approx(290.f));
}

TEST_CASE("Transform: scaling maps corners correctly")
{
    Transform2D t = Transform2D::identity();
    t.x = 0.f;
    t.y = 0.f;
    t.scaleX = 2.f;
    t.scaleY = 2.f;
    // Scale about the media center (the test is about scaling, not the
    // anchor default): set the anchor explicitly.
    t.anchorX = 0.5f;
    t.anchorY = 0.5f;
    const Size2i media{100, 50};

    // Media center stays at position (anchor 0.5).
    const auto [cx, cy] = t.toOutputSpace(0.5f, 0.5f, media);
    CHECK(cx == doctest::Approx(0.f));
    CHECK(cy == doctest::Approx(0.f));

    // Top-left corner: (0 - 0.5) * (2*100) = -100
    const auto [tlx, tly] = t.toOutputSpace(0.f, 0.f, media);
    CHECK(tlx == doctest::Approx(-100.f));
    CHECK(tly == doctest::Approx(-50.f));

    // Bottom-right: +100, +50
    const auto [brx, bry] = t.toOutputSpace(1.f, 1.f, media);
    CHECK(brx == doctest::Approx(100.f));
    CHECK(bry == doctest::Approx(50.f));
}

TEST_CASE("Transform: rotation by 90 degrees clockwise (y-down)")
{
    Transform2D t = Transform2D::identity();
    t.x = 0.f;
    t.y = 0.f;
    t.scaleX = 1.f;
    t.scaleY = 1.f;
    t.rotation = 3.14159265358979323846f / 2.f; // 90 deg
    // Rotate about the media center (the test is about rotation, not the
    // anchor default): set the anchor explicitly.
    t.anchorX = 0.5f;
    t.anchorY = 0.5f;
    const Size2i media{100, 40};

    // The point at media UV (1, 0.5) is +50px right of the anchor before
    // rotation; rotated 90deg clockwise in y-down space it points down
    // (+y).
    const auto [px, py] = t.toOutputSpace(1.f, 0.5f, media);
    CHECK(px == doctest::Approx(0.f).epsilon(1e-3));
    CHECK(py == doctest::Approx(50.f).epsilon(1e-3));
}

TEST_CASE("Transform: anchor shifts the rotation origin")
{
    // With anchor at (0, 0) and position (0, 0), the top-left corner is
    // fixed under rotation.
    Transform2D t = Transform2D::identity();
    t.anchorX = 0.f;
    t.anchorY = 0.f;
    t.rotation = 1.0f;
    const Size2i media{100, 100};
    const auto [px, py] = t.toOutputSpace(0.f, 0.f, media);
    CHECK(px == doctest::Approx(0.f).epsilon(1e-4));
    CHECK(py == doctest::Approx(0.f).epsilon(1e-4));
}

TEST_CASE("Transform: bounds for unrotated layer")
{
    Transform2D t = Transform2D::identity();
    t.x = 10.f;
    t.y = 20.f;
    t.scaleX = 2.f;
    t.scaleY = 2.f;
    // The expected bounds assume the media center is at the position (the
    // test is about bounds, not the anchor default): anchor explicitly.
    t.anchorX = 0.5f;
    t.anchorY = 0.5f;
    const Rect2f b = t.bounds(Size2i{100, 50});
    CHECK(b.x == doctest::Approx(-90.f));
    CHECK(b.y == doctest::Approx(-30.f));
    CHECK(b.width == doctest::Approx(200.f));
    CHECK(b.height == doctest::Approx(100.f));
}

TEST_CASE("Rect2i: contains and intersects")
{
    const Rect2i r(10, 10, 100, 100);
    CHECK(r.containsPoint(10, 10));
    CHECK(r.containsPoint(109, 109));
    CHECK_FALSE(r.containsPoint(110, 10)); // max is exclusive
    CHECK_FALSE(r.containsPoint(9, 10));

    CHECK(r.intersects(Rect2i(109, 109, 50, 50)));
    CHECK_FALSE(r.intersects(Rect2i(110, 0, 10, 10)));
    CHECK_FALSE(r.intersects(Rect2i(0, 0, 10, 10)));
}

TEST_CASE("Rect2i: union")
{
    const Rect2i a(0, 0, 100, 100);
    const Rect2i b(50, 60, 100, 100);
    const Rect2i u = a.unionWith(b);
    CHECK(u == Rect2i(0, 0, 150, 160));

    // Empty rect unions to the other.
    CHECK(Rect2i().unionWith(a) == a);
    CHECK(a.unionWith(Rect2i()) == a);
}

TEST_CASE("Rect2i: union with negative coordinates (multi-monitor)")
{
    const Rect2i left(-1920, 0, 1920, 1080);
    const Rect2i right(0, 100, 1920, 1080);
    const Rect2i u = left.unionWith(right);
    CHECK(u == Rect2i(-1920, 0, 3840, 1180));
}

} // TEST_SUITE("Math")
