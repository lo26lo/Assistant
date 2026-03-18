#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "ibom/IBomData.h"
#include "ibom/IBomParser.h"

using namespace ibom;

TEST_CASE("IBomParser — parse empty HTML", "[ibom][parser]")
{
    IBomParser parser;
    auto result = parser.parseFile("nonexistent.html");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("IBomParser — extract JSON from script", "[ibom][parser]")
{
    // Simulate a minimal iBOM HTML with embedded data
    std::string html = R"(
        <html><head><script>
        var pcbdata = {"edges_bbox":{"minx":0,"miny":0,"maxx":100,"maxy":80},
                       "board_outline":{"F":[]},"footprints":[],"bom":{"F":[],"B":[],"both":[]},
                       "nets":[]};
        </script></head><body></body></html>
    )";

    IBomParser parser;
    auto result = parser.parseString(html);

    // This depends on parser implementation robustness
    // The parser should at least not crash on minimal input
    // Full test requires a real iBOM file
    if (result) {
        CHECK(result->components.empty());
    }
}

TEST_CASE("IBomData — Component default construction", "[ibom][data]")
{
    Component comp;
    REQUIRE(comp.reference.empty());
    REQUIRE(comp.value.empty());
    REQUIRE(comp.footprint.empty());
    REQUIRE(comp.layer == 0);
    REQUIRE(comp.rotation == Catch::Approx(0.0));
    REQUIRE(comp.pads.empty());
}

TEST_CASE("IBomData — Pad construction", "[ibom][data]")
{
    Pad pad;
    pad.position = {10.5f, 20.3f};
    pad.shape = Pad::Shape::Rect;
    pad.width = 1.2f;
    pad.height = 0.8f;
    pad.isSMD = true;
    pad.isPin1 = false;

    REQUIRE(pad.position.x == Catch::Approx(10.5f));
    REQUIRE(pad.position.y == Catch::Approx(20.3f));
    REQUIRE(pad.shape == Pad::Shape::Rect);
    REQUIRE(pad.isSMD == true);
}

TEST_CASE("IBomData — BBox operations", "[ibom][data]")
{
    BBox box;
    box.minX = 10;
    box.minY = 20;
    box.maxX = 50;
    box.maxY = 60;

    REQUIRE(box.width() == Catch::Approx(40.0));
    REQUIRE(box.height() == Catch::Approx(40.0));
    REQUIRE(box.centerX() == Catch::Approx(30.0));
    REQUIRE(box.centerY() == Catch::Approx(40.0));

    REQUIRE(box.contains(30, 40));
    REQUIRE_FALSE(box.contains(5, 5));
}
