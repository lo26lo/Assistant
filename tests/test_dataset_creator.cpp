// Unit tests for the dataset auto-annotation pipeline (Phase A):
// class mapping rules and YOLO label projection. The DatasetCreator QObject
// itself (threading, session I/O) is exercised on-device; here we pin down
// the pure logic that determines label correctness.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "features/DatasetCreator.h"

#include <opencv2/core.hpp>

using namespace ibom;
using namespace ibom::features;
using Catch::Approx;

namespace {

nlohmann::json testRules()
{
    return nlohmann::json::parse(R"({
        "classes": ["resistor", "capacitor", "led", "diode", "connector", "other"],
        "rules": [
            { "class": "led",       "footprint": "LED" },
            { "class": "led",       "ref": "^D\\d+", "value": "LED" },
            { "class": "resistor",  "footprint": "^R_|:R_" },
            { "class": "resistor",  "ref": "^R\\d+" },
            { "class": "capacitor", "ref": "^C\\d+" },
            { "class": "diode",     "footprint": "SOD" },
            { "class": "diode",     "ref": "^D\\d+" },
            { "class": "connector", "ref": "^J\\d+" }
        ]
    })");
}

Component makeComponent(const std::string& ref, const std::string& value,
                        const std::string& footprint,
                        double minX, double minY, double maxX, double maxY,
                        Layer layer = Layer::Front)
{
    Component c;
    c.reference = ref;
    c.value     = value;
    c.footprint = footprint;
    c.layer     = layer;
    c.bbox      = {minX, minY, maxX, maxY};
    return c;
}

} // namespace

TEST_CASE("ClassMapper applies rules in order", "[dataset]")
{
    ClassMapper mapper;
    REQUIRE(mapper.loadJson(testRules()));
    REQUIRE(mapper.classNames().size() == 6);

    // Footprint match
    auto r1 = makeComponent("R1", "10k", "Resistor_SMD:R_0402_1005Metric", 0, 0, 1, 1);
    CHECK(mapper.classId(r1) == 0);

    // Ref fallback when footprint doesn't match
    auto r2 = makeComponent("R7", "1k", "Custom:Weird_0402", 0, 0, 1, 1);
    CHECK(mapper.classId(r2) == 0);

    // LED before diode: ref D + value LED → led, not diode
    auto led = makeComponent("D3", "LED_RED", "Custom:Something", 0, 0, 1, 1);
    CHECK(mapper.classId(led) == 2);

    // Plain diode: SOD footprint
    auto diode = makeComponent("D4", "1N4148", "Diode_SMD:SOD-123", 0, 0, 1, 1);
    CHECK(mapper.classId(diode) == 3);

    // Case-insensitive footprint match
    auto led2 = makeComponent("D9", "x", "led_smd:led_0603", 0, 0, 1, 1);
    CHECK(mapper.classId(led2) == 2);

    // Unmapped → "other" + recorded for curation
    auto mystery = makeComponent("U1", "STM32", "Package_QFP:LQFP-48", 0, 0, 1, 1);
    CHECK(mapper.classId(mystery) == 5);
    CHECK(mapper.unmappedFootprints().count("Package_QFP:LQFP-48") == 1);
}

TEST_CASE("ClassMapper rejects rule files without 'other'", "[dataset]")
{
    ClassMapper mapper;
    auto bad = nlohmann::json::parse(
        R"({"classes": ["resistor"], "rules": []})");
    CHECK_FALSE(mapper.loadJson(bad));
    CHECK_FALSE(mapper.isLoaded());
}

TEST_CASE("projectLabels: identity-scaled homography produces exact YOLO boxes",
          "[dataset]")
{
    ClassMapper mapper;
    REQUIRE(mapper.loadJson(testRules()));

    // PCB mm → image px: pure scale ×10 (board 192×108 mm → 1920×1080 px).
    cv::Mat H = (cv::Mat_<double>(3, 3) << 10, 0, 0,  0, 10, 0,  0, 0, 1);
    const cv::Size img(1920, 1080);

    LabelParams p;
    p.shrink = 1.0;  // no shrink → exact geometry check
    p.minBoxPx = 12;
    p.minVisibleFrac = 0.6;

    // 4×2 mm resistor centered at (96, 54) mm → 40×20 px at (960, 540) px.
    std::vector<Component> comps = {
        makeComponent("R1", "10k", "Resistor_SMD:R_0402_1005Metric",
                      94, 53, 98, 55)};

    auto labels = projectLabels(comps, Layer::Front, H, img, p, mapper);
    REQUIRE(labels.size() == 1);
    CHECK(labels[0].classId == 0);
    CHECK(labels[0].cx == Approx(960.0 / 1920.0));
    CHECK(labels[0].cy == Approx(540.0 / 1080.0));
    CHECK(labels[0].w  == Approx(40.0 / 1920.0));
    CHECK(labels[0].h  == Approx(20.0 / 1080.0));
}

TEST_CASE("projectLabels: shrink reduces the box around its center", "[dataset]")
{
    ClassMapper mapper;
    REQUIRE(mapper.loadJson(testRules()));

    cv::Mat H = (cv::Mat_<double>(3, 3) << 10, 0, 0,  0, 10, 0,  0, 0, 1);
    LabelParams p;
    p.shrink = 0.5;

    std::vector<Component> comps = {
        makeComponent("R1", "x", "R_0402", 94, 53, 98, 55)};
    auto labels = projectLabels(comps, Layer::Front, H, cv::Size(1920, 1080), p, mapper);
    REQUIRE(labels.size() == 1);
    CHECK(labels[0].cx == Approx(960.0 / 1920.0));   // center unchanged
    CHECK(labels[0].w  == Approx(20.0 / 1920.0));    // 40 px × 0.5
    CHECK(labels[0].h  == Approx(10.0 / 1080.0));    // 20 px × 0.5
}

TEST_CASE("projectLabels: gates reject wrong layer, tiny and clipped boxes",
          "[dataset]")
{
    ClassMapper mapper;
    REQUIRE(mapper.loadJson(testRules()));

    cv::Mat H = (cv::Mat_<double>(3, 3) << 10, 0, 0,  0, 10, 0,  0, 0, 1);
    const cv::Size img(1920, 1080);
    LabelParams p;
    p.shrink = 1.0;

    SECTION("back-layer component is skipped when capturing the front") {
        std::vector<Component> comps = {
            makeComponent("C1", "100n", "C_0402", 94, 53, 98, 55, Layer::Back)};
        CHECK(projectLabels(comps, Layer::Front, H, img, p, mapper).empty());
    }

    SECTION("box smaller than minBoxPx is rejected") {
        // 1×1 mm → 10×10 px < 12 px minimum
        std::vector<Component> comps = {
            makeComponent("R1", "x", "R_0402", 96, 54, 97, 55)};
        CHECK(projectLabels(comps, Layer::Front, H, img, p, mapper).empty());
    }

    SECTION("component mostly outside the frame is rejected") {
        // 40 mm wide, only 8 mm inside (x: -32..8 mm → -320..80 px): 20% visible
        std::vector<Component> comps = {
            makeComponent("J1", "conn", "Conn_Header", -32, 50, 8, 58)};
        CHECK(projectLabels(comps, Layer::Front, H, img, p, mapper).empty());
    }

    SECTION("component partially clipped but ≥60% visible is kept and clamped") {
        // x: -1..9 mm → -10..90 px, 90% visible after clip to [0, 90]
        std::vector<Component> comps = {
            makeComponent("J1", "conn", "Conn_Header", -1, 50, 9, 58)};
        auto labels = projectLabels(comps, Layer::Front, H, img, p, mapper);
        REQUIRE(labels.size() == 1);
        // Clipped box: x [0, 90] px → cx=45px, w=90px
        CHECK(labels[0].cx == Approx(45.0 / 1920.0));
        CHECK(labels[0].w  == Approx(90.0 / 1920.0));
        // All coordinates normalized and inside [0,1]
        CHECK(labels[0].cx - labels[0].w / 2 >= 0.0);
        CHECK(labels[0].cx + labels[0].w / 2 <= 1.0);
    }
}

TEST_CASE("projectLabels: empty homography yields no labels", "[dataset]")
{
    ClassMapper mapper;
    REQUIRE(mapper.loadJson(testRules()));
    std::vector<Component> comps = {
        makeComponent("R1", "x", "R_0402", 94, 53, 98, 55)};
    LabelParams p;
    CHECK(projectLabels(comps, Layer::Front, cv::Mat(), cv::Size(1920, 1080),
                        p, mapper).empty());
}
