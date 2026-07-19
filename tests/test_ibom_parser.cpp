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

TEST_CASE("IBomParser — parseString needs both config and pcbdata", "[ibom][parser]")
{
    IBomParser parser;

    // pcbdata alone (no config var) → parse bails: this is exactly why the
    // old smoke test never actually parsed anything.
    const std::string noConfig =
        R"(<script>var pcbdata = {"footprints":[],"bom":{"both":[]}};</script>)";
    REQUIRE_FALSE(parser.parseString(noConfig).has_value());

    // config + empty pcbdata → valid, zero components.
    const std::string empty =
        R"(<script>var config = {};
           var pcbdata = {"edges_bbox":{"minx":0,"miny":0,"maxx":10,"maxy":10},
                          "footprints":[],"bom":{"both":[]},"nets":{}};</script>)";
    auto r = parser.parseString(empty);
    REQUIRE(r.has_value());
    CHECK(r->components.empty());
    CHECK(r->boardInfo.boardBBox.width() == Catch::Approx(10.0));
}

TEST_CASE("IBomParser — full parse extracts components and cross-references BOM",
          "[ibom][parser]")
{
    // A faithful minimal iBOM: config + pcbdata with two footprints (one per
    // side), a bom.both group per part carrying value/footprint in bom.fields
    // (footprint objects themselves don't — the parser must cross-reference).
    const std::string html = R"HTML(<html><script>
      var config = {"dark_mode":false,"fields":["Value","Footprint"]};
      var pcbdata = {
        "edges_bbox":{"minx":0,"miny":0,"maxx":100,"maxy":80},
        "footprints":[
          {"ref":"R1","layer":"F","center":[20,40],
           "bbox":{"pos":[20,40],"size":[2,1],"relpos":[-1,-0.5],"angle":0},
           "pads":[{"num":"1","net":"GND","pos":[19,40],"size":[0.6,0.6]}]},
          {"ref":"C1","layer":"B","val":"100nF","center":[80,20],
           "bbox":{"pos":[80,20],"size":[2,2],"relpos":[-1,-1],"angle":0}}
        ],
        "bom":{"both":[[["R1",0]],[["C1",1]]],
               "fields":{"0":["10k","R_0402"],"1":["100nF","C_0402"]}},
        "nets":{}
      };
    </script></html>)HTML";

    IBomParser parser;
    auto r = parser.parseString(html);
    REQUIRE(r.has_value());
    REQUIRE(r->components.size() == 2);

    const auto& r1 = r->components[0];
    CHECK(r1.reference == "R1");
    CHECK(r1.layer == Layer::Front);
    CHECK(r1.pads.size() == 1);
    // val was absent on the footprint → filled from BOM fields.
    CHECK(r1.value == "10k");
    CHECK(r1.footprint == "R_0402");

    const auto& c1 = r->components[1];
    CHECK(c1.reference == "C1");
    CHECK(c1.layer == Layer::Back);
    // val was present on the footprint → cross-reference must NOT overwrite it.
    CHECK(c1.value == "100nF");
    CHECK(c1.footprint == "C_0402");
}

TEST_CASE("IBomParser — footprint without 'center' falls back to bbox center (ERREUR #56)",
          "[ibom][parser]")
{
    // The degenerate-layout regression: with no "center" field, Component
    // position must derive from the bbox, not stay (0,0) — otherwise every
    // part collapses to the origin and component re-anchor sees a degenerate
    // layout.
    const std::string html = R"HTML(<script>
      var config = {};
      var pcbdata = {"edges_bbox":{"minx":0,"miny":0,"maxx":100,"maxy":80},
        "footprints":[
          {"ref":"U1","layer":"F",
           "bbox":{"pos":[80,20],"size":[4,2],"relpos":[-2,-1],"angle":0}}
        ],
        "bom":{"both":[]},"nets":{}};
    </script>)HTML";

    IBomParser parser;
    auto r = parser.parseString(html);
    REQUIRE(r.has_value());
    REQUIRE(r->components.size() == 1);
    const auto& u1 = r->components[0];
    CHECK(u1.position.x == Catch::Approx(80.0));
    CHECK(u1.position.y == Catch::Approx(20.0));
    CHECK(u1.position.x != Catch::Approx(0.0));  // the bug this guards against
}

TEST_CASE("IBomParser — rotated bbox reconstructs axis-aligned bounds",
          "[ibom][parser]")
{
    // iBOM stores pos/size/relpos/angle, not min/max. A 10x2 box rotated 90°
    // must yield 2x10 axis-aligned bounds — naive pos+size would be wrong.
    const std::string html = R"HTML(<script>
      var config = {};
      var pcbdata = {"edges_bbox":{"minx":0,"miny":0,"maxx":100,"maxy":100},
        "footprints":[
          {"ref":"J1","layer":"F","center":[50,50],
           "bbox":{"pos":[50,50],"size":[10,2],"relpos":[-5,-1],"angle":90}}
        ],
        "bom":{"both":[]},"nets":{}};
    </script>)HTML";

    IBomParser parser;
    auto r = parser.parseString(html);
    REQUIRE(r.has_value());
    REQUIRE(r->components.size() == 1);
    const auto& bb = r->components[0].bbox;
    // width ~2 (was 10 before rotation), height ~10 (was 2).
    CHECK(bb.width() == Catch::Approx(2.0).margin(0.01));
    CHECK(bb.height() == Catch::Approx(10.0).margin(0.01));
    CHECK(bb.center().x == Catch::Approx(50.0).margin(0.01));
    CHECK(bb.center().y == Catch::Approx(50.0).margin(0.01));
}

TEST_CASE("IBomParser — LZString decompression terminates on corrupted input",
          "[ibom][parser][lzstring]")
{
    IBomParser parser;

    SECTION("empty input") {
        REQUIRE_FALSE(parser.decompressLZString("").has_value());
    }

    SECTION("garbage base64 returns without hanging") {
        // Deterministic pseudo-random base64 alphabet stream — simulates a
        // truncated/corrupted compressed block inside an iBOM HTML.
        static const std::string alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string garbage;
        unsigned seed = 12345;
        for (int i = 0; i < 50000; ++i) {
            seed = seed * 1103515245u + 12345u;
            garbage.push_back(alphabet[(seed >> 16) % alphabet.size()]);
        }

        // Must terminate (bad-code error, end-of-data, or output-size guard)
        // — any outcome is fine as long as it returns instead of spinning
        // or exhausting memory.
        auto result = parser.decompressLZString(garbage);
        if (result) {
            // The guard caps expansion at 1000x input + 1 MiB (in UTF-16
            // chars); the returned UTF-8 string is at most 3 bytes per char.
            CHECK(result->size() <= (garbage.size() * 1000 + (1 << 20)) * 3);
        }
    }

    SECTION("non-base64 characters do not crash") {
        auto result = parser.decompressLZString("!!!###$$$%%%");
        (void)result; // termination without crash is the assertion
    }
}

TEST_CASE("IBomData — Component default construction", "[ibom][data]")
{
    Component comp;
    REQUIRE(comp.reference.empty());
    REQUIRE(comp.value.empty());
    REQUIRE(comp.footprint.empty());
    REQUIRE(comp.layer == Layer::Front);
    REQUIRE(comp.rotation == Catch::Approx(0.0));
    REQUIRE(comp.pads.empty());
}

TEST_CASE("IBomData — Pad construction", "[ibom][data]")
{
    Pad pad;
    pad.position = {10.5, 20.3};
    pad.shape = Pad::Shape::Rect;
    pad.sizeX = 1.2;
    pad.sizeY = 0.8;
    pad.isSMD = true;
    pad.isPin1 = false;

    REQUIRE(pad.position.x == Catch::Approx(10.5));
    REQUIRE(pad.position.y == Catch::Approx(20.3));
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
    REQUIRE(box.center().x == Catch::Approx(30.0));
    REQUIRE(box.center().y == Catch::Approx(40.0));

    // Check a point inside the box
    Point2D inside{30, 40};
    REQUIRE(inside.x >= box.minX);
    REQUIRE(inside.x <= box.maxX);
    REQUIRE(inside.y >= box.minY);
    REQUIRE(inside.y <= box.maxY);

    // Check a point outside the box
    Point2D outside{5, 5};
    bool isInside = outside.x >= box.minX && outside.x <= box.maxX &&
                    outside.y >= box.minY && outside.y <= box.maxY;
    REQUIRE_FALSE(isInside);
}

TEST_CASE("IBomParser — board-outline arcs carry startangle/endangle",
          "[ibom][parser]")
{
    // iBOM encodes an arc as centre (`start`), radius and start/end angles in
    // degrees. Before these were parsed, the minimap had to skip rounded board
    // corners entirely (a guessed sweep would draw phantom edges).
    const std::string html = R"HTML(<script>
      var config = {};
      var pcbdata = {"edges_bbox":{"minx":0,"miny":0,"maxx":100,"maxy":80},
        "edges":[
          {"type":"segment","start":[0,0],"end":[100,0],"width":0.1},
          {"type":"arc","start":[95,5],"radius":5,
           "startangle":-90,"endangle":0,"width":0.1}
        ],
        "footprints":[],"bom":{"both":[]},"nets":{}};
    </script>)HTML";

    IBomParser parser;
    auto r = parser.parseString(html);
    REQUIRE(r.has_value());
    REQUIRE(r->boardOutline.size() == 2);

    const auto& arc = r->boardOutline[1];
    CHECK(arc.type == DrawingSegment::Type::Arc);
    CHECK(arc.start.x == Catch::Approx(95.0));
    CHECK(arc.start.y == Catch::Approx(5.0));
    CHECK(arc.radius == Catch::Approx(5.0));
    CHECK(arc.startAngle == Catch::Approx(-90.0));
    CHECK(arc.endAngle == Catch::Approx(0.0));

    // Line segments keep the default sentinel (both 0 → "no arc data").
    CHECK(r->boardOutline[0].startAngle == Catch::Approx(0.0));
    CHECK(r->boardOutline[0].endAngle == Catch::Approx(0.0));
}
