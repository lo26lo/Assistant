#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "ibom/ComponentMap.h"
#include "ibom/IBomData.h"

using namespace ibom;
using Catch::Approx;

static std::vector<Component> createTestComponents()
{
    std::vector<Component> comps;

    auto makeComp = [](const std::string& ref, const std::string& val,
                       double x, double y, Layer layer) {
        Component c;
        c.reference = ref;
        c.value = val;
        c.footprint = "0805";
        c.layer = layer;
        c.position = {x, y};
        c.bbox.minX = x - 1; c.bbox.minY = y - 0.5;
        c.bbox.maxX = x + 1; c.bbox.maxY = y + 0.5;
        return c;
    };

    comps.push_back(makeComp("R1",  "10K",  10, 20, Layer::Front));
    comps.push_back(makeComp("R2",  "10K",  30, 20, Layer::Front));
    comps.push_back(makeComp("C1",  "100nF", 10, 40, Layer::Front));
    comps.push_back(makeComp("C2",  "100nF", 50, 40, Layer::Front));
    comps.push_back(makeComp("U1",  "ATmega328", 25, 30, Layer::Front));
    comps.push_back(makeComp("R3",  "4.7K",  10, 20, Layer::Back)); // Back layer

    return comps;
}

TEST_CASE("ComponentMap — build and query count", "[ibom][component_map]")
{
    auto comps = createTestComponents();
    ComponentMap map;
    map.build(comps);

    REQUIRE(map.count() == 6);
}

TEST_CASE("ComponentMap — find by reference", "[ibom][component_map]")
{
    auto comps = createTestComponents();
    ComponentMap map;
    map.build(comps);

    auto result = map.findByReference("R1");
    REQUIRE(result != nullptr);
    REQUIRE(result->reference == "R1");
    REQUIRE(result->value == "10K");

    auto missing = map.findByReference("R99");
    REQUIRE(missing == nullptr);
}

TEST_CASE("ComponentMap — find by value", "[ibom][component_map]")
{
    auto comps = createTestComponents();
    ComponentMap map;
    map.build(comps);

    auto results = map.findByValue("10K");
    REQUIRE(results.size() == 2); // R1 and R2

    auto results2 = map.findByValue("100nF");
    REQUIRE(results2.size() == 2); // C1 and C2
}

TEST_CASE("ComponentMap — find nearest", "[ibom][component_map]")
{
    auto comps = createTestComponents();
    ComponentMap map;
    map.build(comps);

    // Search near R1 (at 10, 20)
    auto nearest = map.findNearest(Point2D{11.0, 21.0}, 5.0);
    REQUIRE(nearest != nullptr);
    REQUIRE(nearest->reference == "R1");
}

TEST_CASE("ComponentMap — find in rect", "[ibom][component_map]")
{
    auto comps = createTestComponents();
    ComponentMap map;
    map.build(comps);

    // Rect covering R1, R2, U1 area
    BBox rect;
    rect.minX = 5; rect.minY = 15;
    rect.maxX = 35; rect.maxY = 35;
    auto found = map.findInRect(rect);
    REQUIRE(found.size() >= 2); // At least R1, R2, U1
}

TEST_CASE("ComponentMap — find by layer", "[ibom][component_map]")
{
    auto comps = createTestComponents();
    ComponentMap map;
    map.build(comps);

    auto front = map.findByLayer(Layer::Front);
    REQUIRE(front.size() == 5);

    auto back = map.findByLayer(Layer::Back);
    REQUIRE(back.size() == 1);
    REQUIRE(back[0]->reference == "R3");
}

TEST_CASE("ComponentMap — empty map queries", "[ibom][component_map]")
{
    ComponentMap map;

    REQUIRE(map.count() == 0);
    REQUIRE(map.findByReference("R1") == nullptr);
    REQUIRE(map.findByValue("10K").empty());
    REQUIRE(map.findNearest(Point2D{0, 0}, 10) == nullptr);
}
