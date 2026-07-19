#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "overlay/AlignmentMath.h"

#include <opencv2/core.hpp>
#include <cmath>
#include <vector>

using Catch::Approx;
using namespace ibom::overlay;

namespace {

cv::Point2f apply(const cv::Mat& H, cv::Point2f p)
{
    std::vector<cv::Point2f> in{p}, out;
    cv::perspectiveTransform(in, out, H);
    return out[0];
}

} // namespace

TEST_CASE("alignmath — anchor similarity maps the anchor exactly", "[alignmath]")
{
    const cv::Point2f pcb(12.5f, 30.0f);
    const cv::Point2f img(640.0f, 360.0f);
    const cv::Mat H = alignmath::similarityFromAnchor(8.0, 0.3, +1.0, pcb, img);

    REQUIRE(!H.empty());
    const cv::Point2f m = apply(H, pcb);
    CHECK(m.x == Approx(img.x).margin(1e-3));
    CHECK(m.y == Approx(img.y).margin(1e-3));

    // Uniform scale: any unit step in PCB space moves `scale` px in the image.
    const cv::Point2f step = apply(H, {pcb.x + 1.0f, pcb.y});
    CHECK(std::hypot(step.x - img.x, step.y - img.y) == Approx(8.0).margin(1e-3));
}

TEST_CASE("alignmath — identity-ish parameters give a pure scale", "[alignmath]")
{
    const cv::Mat H = alignmath::similarityFromAnchor(2.0, 0.0, +1.0,
                                                      {0.f, 0.f}, {0.f, 0.f});
    const cv::Point2f m = apply(H, {10.0f, -4.0f});
    CHECK(m.x == Approx(20.0).margin(1e-3));
    CHECK(m.y == Approx(-8.0).margin(1e-3));
    // Front view: orientation-preserving (positive determinant).
    CHECK(cv::determinant(H) > 0.0);
}

TEST_CASE("alignmath — back-view similarity carries the mirror", "[alignmath]")
{
    const cv::Mat H = alignmath::similarityFromAnchor(5.0, 0.0, -1.0,
                                                      {10.f, 10.f}, {100.f, 100.f});
    REQUIRE(!H.empty());
    // Mirrored view: negative determinant, but the anchor still lands exactly.
    CHECK(cv::determinant(H) < 0.0);
    const cv::Point2f m = apply(H, {10.f, 10.f});
    CHECK(m.x == Approx(100.0).margin(1e-3));
    CHECK(m.y == Approx(100.0).margin(1e-3));
    // +x in PCB space moves −x on screen (the mirror), +y stays +y.
    const cv::Point2f px = apply(H, {11.f, 10.f});
    CHECK(px.x == Approx(95.0).margin(1e-3));
    const cv::Point2f py = apply(H, {10.f, 11.f});
    CHECK(py.y == Approx(105.0).margin(1e-3));
}

TEST_CASE("alignmath — two-point fit recovers scale and rotation", "[alignmath]")
{
    // Ground truth: scale 4 px/mm, rotation 90° CCW-in-pcb-frame, translation.
    const double s = 4.0, rot = M_PI / 2.0;
    auto gt = [&](cv::Point2f p) {
        const double c = std::cos(rot) * s, sn = std::sin(rot) * s;
        return cv::Point2f(static_cast<float>(c * p.x - sn * p.y + 50.0),
                           static_cast<float>(sn * p.x + c * p.y + 20.0));
    };
    const cv::Point2f a(5.f, 5.f), b(45.f, 25.f);

    double outScale = 0.0, outRot = 0.0;
    const cv::Mat H = alignmath::similarityFromTwoPoints(
        a, b, gt(a), gt(b), +1.0, &outScale, &outRot);
    REQUIRE(!H.empty());
    CHECK(outScale == Approx(4.0).margin(1e-6));
    CHECK(outRot == Approx(rot).margin(1e-6));

    // A third, uninvolved point must land where the ground truth puts it —
    // that's what makes the two-point similarity usable as an alignment.
    const cv::Point2f probe(30.f, 12.f);
    const cv::Point2f m = apply(H, probe);
    const cv::Point2f e = gt(probe);
    CHECK(m.x == Approx(e.x).margin(1e-3));
    CHECK(m.y == Approx(e.y).margin(1e-3));
}

TEST_CASE("alignmath — degenerate two-point inputs are refused", "[alignmath]")
{
    // PCB points closer than 0.1 mm.
    CHECK(alignmath::similarityFromTwoPoints({0.f, 0.f}, {0.05f, 0.f},
                                             {0.f, 0.f}, {100.f, 0.f},
                                             +1.0).empty());
    // Image points closer than 1 px.
    CHECK(alignmath::similarityFromTwoPoints({0.f, 0.f}, {50.f, 0.f},
                                             {10.f, 10.f}, {10.5f, 10.f},
                                             +1.0).empty());
}

TEST_CASE("alignmath — two-point back-view fit mirrors and still hits both points",
          "[alignmath]")
{
    // Synthesize a mirrored ground truth (vx = −1) and check the fit
    // reproduces BOTH correspondences and the negative determinant.
    const double s = 3.0;
    auto gt = [&](cv::Point2f p) {
        return cv::Point2f(static_cast<float>(-s * p.x + 300.0),
                           static_cast<float>(s * p.y + 10.0));
    };
    const cv::Point2f a(10.f, 5.f), b(60.f, 35.f);
    const cv::Mat H = alignmath::similarityFromTwoPoints(a, b, gt(a), gt(b), -1.0);
    REQUIRE(!H.empty());
    CHECK(cv::determinant(H) < 0.0);
    for (const auto& p : {a, b}) {
        const cv::Point2f m = apply(H, p);
        const cv::Point2f e = gt(p);
        CHECK(m.x == Approx(e.x).margin(1e-3));
        CHECK(m.y == Approx(e.y).margin(1e-3));
    }
}
