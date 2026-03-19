#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "overlay/Homography.h"
#include <opencv2/core.hpp>

using namespace ibom::overlay;
using Catch::Approx;

TEST_CASE("Homography — identity mapping", "[overlay][homography]")
{
    Homography h;

    // Set 4 matching point pairs (identity)
    std::vector<cv::Point2f> pcb  = {{0,0}, {100,0}, {100,100}, {0,100}};
    std::vector<cv::Point2f> img  = {{0,0}, {100,0}, {100,100}, {0,100}};

    bool ok = h.compute(pcb, img);
    REQUIRE(ok);
    REQUIRE(h.isValid());

    // Transform should be identity
    cv::Point2f result = h.pcbToImage({50, 50});
    REQUIRE(result.x == Approx(50).margin(0.5));
    REQUIRE(result.y == Approx(50).margin(0.5));
}

TEST_CASE("Homography — translation", "[overlay][homography]")
{
    Homography h;

    // PCB at origin, image shifted by (100, 200)
    std::vector<cv::Point2f> pcb = {{0,0}, {100,0}, {100,100}, {0,100}};
    std::vector<cv::Point2f> img = {{100,200}, {200,200}, {200,300}, {100,300}};

    bool ok = h.compute(pcb, img);
    REQUIRE(ok);

    cv::Point2f result = h.pcbToImage({50, 50});
    REQUIRE(result.x == Approx(150).margin(1));
    REQUIRE(result.y == Approx(250).margin(1));
}

TEST_CASE("Homography — inverse transform", "[overlay][homography]")
{
    Homography h;

    std::vector<cv::Point2f> pcb = {{0,0}, {100,0}, {100,100}, {0,100}};
    std::vector<cv::Point2f> img = {{50,50}, {250,50}, {250,250}, {50,250}};

    bool ok = h.compute(pcb, img);
    REQUIRE(ok);

    // Forward then inverse should return original point
    cv::Point2f fwd = h.pcbToImage({30, 40});
    cv::Point2f back = h.imageToPcb(fwd);

    REQUIRE(back.x == Approx(30).margin(1));
    REQUIRE(back.y == Approx(40).margin(1));
}

TEST_CASE("Homography — insufficient points", "[overlay][homography]")
{
    Homography h;

    // Only 3 points — should fail (need 4+)
    std::vector<cv::Point2f> pcb = {{0,0}, {100,0}, {50,100}};
    std::vector<cv::Point2f> img = {{0,0}, {100,0}, {50,100}};

    bool ok = h.compute(pcb, img);
    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(h.isValid());
}

TEST_CASE("Homography — reprojection error", "[overlay][homography]")
{
    Homography h;

    std::vector<cv::Point2f> pcb = {{0,0}, {100,0}, {100,100}, {0,100}};
    std::vector<cv::Point2f> img = {{0,0}, {100,0}, {100,100}, {0,100}};

    h.compute(pcb, img);

    double error = h.reprojectionError();
    REQUIRE(error < 1.0); // Should be very small for exact points
}
