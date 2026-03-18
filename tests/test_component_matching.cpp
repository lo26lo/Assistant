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
                       float x, float y, int layer) {
        Component c;
        c.reference = ref;
        c.value = val;
        c.footprint = "0805";
        c.layer = layer;
        c.positionX = x;
        c.positionY = y;
        c.bbox.minX = x - 1; c.bbox.minY = y - 0.5;
        c.bbox.maxX = x + 1; c.bbox.maxY = y + 0.5;
        return c;
    };

    comps.push_back(makeComp("R1",  "10K",  10, 20, 0));
    comps.push_back(makeComp("R2",  "10K",  30, 20, 0));
    comps.push_back(makeComp("C1",  "100nF", 10, 40, 0));
    comps.push_back(makeComp("C2",  "100nF", 50, 40, 0));
    comps.push_back(makeComp("U1",  "ATmega328", 25, 30, 0));
    comps.push_back(makeComp("R3",  "4.7K",  10, 20, 1)); // Back layer

    return comps;
}

TEST_CASE("ComponentMap — build and query count", "[ibom][component_map]")
{
    auto comps = createTestComponents();
    ComponentMap map;
    map.build(comps);

    REQUIRE(map.totalComponents() == 6);
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
    auto nearest = map.findNearest(11.0f, 21.0f, 5.0f);
    REQUIRE(!nearest.empty());
    REQUIRE(nearest[0]->reference == "R1");
}

TEST_CASE("ComponentMap — find in rect", "[ibom][component_map]")
{
    auto comps = createTestComponents();
    ComponentMap map;
    map.build(comps);

    // Rect covering R1, R2, U1 area
    auto found = map.findInRect(5, 15, 35, 35);
    REQUIRE(found.size() >= 2); // At least R1, R2, U1
}

TEST_CASE("ComponentMap — find by layer", "[ibom][component_map]")
{
    auto comps = createTestComponents();
    ComponentMap map;
    map.build(comps);

    auto front = map.findByLayer(0);
    REQUIRE(front.size() == 5);

    auto back = map.findByLayer(1);
    REQUIRE(back.size() == 1);
    REQUIRE(back[0]->reference == "R3");
}

TEST_CASE("ComponentMap — empty map queries", "[ibom][component_map]")
{
    ComponentMap map;

    REQUIRE(map.totalComponents() == 0);
    REQUIRE(map.findByReference("R1") == nullptr);
    REQUIRE(map.findByValue("10K").empty());
    REQUIRE(map.findNearest(0, 0, 10).empty());
}
