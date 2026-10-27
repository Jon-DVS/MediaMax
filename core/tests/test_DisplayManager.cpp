// DisplayManager: multi-display wall management.

#include <doctest/doctest.h>

#include "mediamax/display/DisplayManager.h"

using namespace mediamax;

TEST_SUITE("DisplayManager")
{

TEST_CASE("Adding displays allocates unique ids")
{
    DisplayManager mgr;
    const auto a = mgr.addDisplay("left", 0, 0, 1920, 1080);
    const auto b = mgr.addDisplay("right", 1920, 0, 1920, 1080);
    CHECK(a.id != b.id);
    CHECK(mgr.count() == 2);
}

TEST_CASE("Output bounds cover the whole wall")
{
    DisplayManager mgr;
    mgr.addDisplay("top-left", 0, 0, 1920, 1080);
    mgr.addDisplay("top-right", 1920, 0, 1920, 1080);
    mgr.addDisplay("bottom-left", 0, 1080, 1920, 1080);
    mgr.addDisplay("bottom-right", 1920, 1080, 1920, 1080);

    const Rect2i bounds = mgr.outputBounds();
    CHECK(bounds == Rect2i(0, 0, 3840, 2160));
}

TEST_CASE("The status overlay flag is per display, off by default")
{
    DisplayManager mgr;
    const auto a = mgr.addDisplay("a", 0, 0, 100, 100);
    const auto b = mgr.addDisplay("b", 500, 0, 100, 100);
    CHECK((*mgr.find(a.id))->statusOverlay == false);
    CHECK((*mgr.find(b.id))->statusOverlay == false);

    CHECK(mgr.setDisplayStatusOverlay(a.id, true));
    CHECK((*mgr.find(a.id))->statusOverlay == true);
    CHECK((*mgr.find(b.id))->statusOverlay == false);
    CHECK_FALSE(mgr.setDisplayStatusOverlay(42, true));
}

TEST_CASE("Disabled displays do not contribute to bounds")
{
    DisplayManager mgr;
    const auto a = mgr.addDisplay("a", 0, 0, 100, 100);
    const auto b = mgr.addDisplay("b", 500, 0, 100, 100);
    mgr.setDisplayEnabled(b.id, false);

    CHECK(mgr.outputBounds() == Rect2i(0, 0, 100, 100));
}

TEST_CASE("Negative coordinates (multi-monitor) are handled")
{
    DisplayManager mgr;
    mgr.addDisplay("left", -1920, 0, 1920, 1080);
    mgr.addDisplay("primary", 0, 0, 1920, 1080);
    CHECK(mgr.outputBounds() == Rect2i(-1920, 0, 3840, 1080));
}

TEST_CASE("Remove and find")
{
    DisplayManager mgr;
    const auto a = mgr.addDisplay("a", 0, 0, 100, 100);
    const auto b = mgr.addDisplay("b", 100, 0, 100, 100);

    CHECK(mgr.find(a.id).has_value());
    CHECK(mgr.find(999).has_value() == false);

    CHECK(mgr.removeDisplay(a.id));
    CHECK_FALSE(mgr.removeDisplay(a.id));
    CHECK(mgr.count() == 1);
    CHECK(mgr.find(b.id).has_value());
}

TEST_CASE("Geometry updates validate sizes")
{
    DisplayManager mgr;
    const auto a = mgr.addDisplay("a", 0, 0, 100, 100);

    CHECK(mgr.setDisplayGeometry(a.id, 5, 6, 200, 100));
    const auto* d = *mgr.find(a.id);
    CHECK(d->x == 5);
    CHECK(d->y == 6);
    CHECK(d->width == 200);

    CHECK_FALSE(mgr.setDisplayGeometry(a.id, 0, 0, 0, 100)); // zero width invalid
    CHECK(mgr.setDisplayGeometry(42, 0, 0, 100, 100) == false);
}

TEST_CASE("Brightness and enable flags")
{
    DisplayManager mgr;
    const auto a = mgr.addDisplay("a", 0, 0, 100, 100);
    CHECK(mgr.setDisplayBrightness(a.id, 0.5f));
    CHECK((*mgr.find(a.id))->brightness == doctest::Approx(0.5f));
    CHECK(mgr.setDisplayEnabled(a.id, false));
    CHECK((*mgr.find(a.id))->enabled == false);
    CHECK_FALSE(mgr.setDisplayBrightness(42, 0.5f));
}

TEST_CASE("Empty manager has zero bounds")
{
    DisplayManager mgr;
    CHECK(mgr.empty());
    CHECK(mgr.outputBounds() == Rect2i());
}

} // TEST_SUITE("DisplayManager")
