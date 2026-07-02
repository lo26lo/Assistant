// Tests for ComponentReanchor::bootstrap — the prior-free global registration
// of an AI-detection constellation against the iBOM component layout
// (docs/AUTO_ALIGN_V2_PLAN.md). Pure geometry: no detector, no Qt event loop —
// detections are synthesized from a known ground-truth similarity transform
// with dropouts, localization noise and spurious detections, and the recovered
// homography must reproject the layout back onto them.

#include <catch2/catch_test_macros.hpp>

#include "overlay/ComponentReanchor.h"
#include "ibom/IBomData.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

using ibom::overlay::ComponentReanchor;

namespace {

// Pseudo-random but deterministic component constellation, irregular enough
// that the registration has a unique answer (a regular grid would alias).
ibom::IBomProject makeProject(std::vector<cv::Point2f>& centersOut)
{
    ibom::IBomProject p;
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> ux(0.f, 100.f);
    std::uniform_real_distribution<float> uy(0.f, 80.f);
    for (int i = 0; i < 40; ++i) {
        ibom::Component c;
        c.reference = "R" + std::to_string(i);
        c.layer     = ibom::Layer::Front;
        c.position  = { ux(rng), uy(rng) };
        centersOut.push_back({ static_cast<float>(c.position.x),
                               static_cast<float>(c.position.y) });
        p.components.push_back(std::move(c));
    }
    p.boardInfo.boardBBox = { 0.0, 0.0, 100.0, 80.0 };
    return p;
}

ai::Detection detectionAt(cv::Point2f c)
{
    ai::Detection d;
    d.classId    = 0;
    d.confidence = 0.9f;
    d.bbox       = cv::Rect2f(c.x - 6.f, c.y - 6.f, 12.f, 12.f);
    return d;
}

} // namespace

TEST_CASE("bootstrap recovers a pose from detections without any prior", "[reanchor]")
{
    std::vector<cv::Point2f> centers;
    const ibom::IBomProject project = makeProject(centers);

    // Ground truth: 8 px/mm, 25° rotation, translation (300, 200) — a board
    // seen by a D405 at typical distance, rotated on the bench.
    const double s = 8.0, th = 25.0 * CV_PI / 180.0;
    const auto gt = [&](cv::Point2f p) {
        return cv::Point2f(
            static_cast<float>(s * std::cos(th) * p.x - s * std::sin(th) * p.y + 300.0),
            static_cast<float>(s * std::sin(th) * p.x + s * std::cos(th) * p.y + 200.0));
    };

    // Detections: 3 components out of 4 detected (25% dropouts), ±1 px
    // localization noise, plus 6 spurious detections (false positives).
    std::mt19937 rng(21);
    std::normal_distribution<float> noise(0.f, 1.0f);
    std::uniform_real_distribution<float> fx(0.f, 1200.f), fy(0.f, 900.f);
    std::vector<ai::Detection> dets;
    for (size_t i = 0; i < centers.size(); ++i) {
        if (i % 4 == 3) continue;
        cv::Point2f q = gt(centers[i]);
        q.x += noise(rng);
        q.y += noise(rng);
        dets.push_back(detectionAt(q));
    }
    for (int i = 0; i < 6; ++i)
        dets.push_back(detectionAt({ fx(rng), fy(rng) }));

    const auto verify = [&](const ibom::overlay::ComponentReanchorResult& r) {
        REQUIRE(r.found);
        REQUIRE(r.inliers >= 8);
        // The recovered homography must map component centers onto their
        // ground-truth image positions (median error well under the pad size).
        std::vector<cv::Point2f> proj;
        cv::perspectiveTransform(centers, proj, r.homography);
        std::vector<double> errs;
        for (size_t i = 0; i < centers.size(); ++i)
            errs.push_back(cv::norm(proj[i] - gt(centers[i])));
        std::nth_element(errs.begin(), errs.begin() + errs.size() / 2, errs.end());
        INFO("median reprojection vs ground truth = " << errs[errs.size() / 2] << " px");
        REQUIRE(errs[errs.size() / 2] < 4.0);
    };

    SECTION("no scale prior (full hypothesis space)") {
        verify(ComponentReanchor::bootstrap(dets, project, ibom::Layer::Front, 0.0));
    }
    SECTION("with a physical scale prior (D405 pinhole path)") {
        verify(ComponentReanchor::bootstrap(dets, project, ibom::Layer::Front, 8.0));
    }
}

TEST_CASE("bootstrap rejects an unrelated constellation", "[reanchor]")
{
    std::vector<cv::Point2f> centers;
    const ibom::IBomProject project = makeProject(centers);

    // 10 detections at fresh random positions, unrelated to the layout: the
    // consensus threshold (max(minMatches, 25% of the smaller set)) plus the
    // final inlier/reprojection validation must both starve. Deterministic:
    // bootstrap's internal RNG is fixed-seeded.
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> fx(0.f, 1200.f), fy(0.f, 900.f);
    std::vector<ai::Detection> dets;
    for (int i = 0; i < 10; ++i)
        dets.push_back(detectionAt({ fx(rng), fy(rng) }));

    const auto r = ComponentReanchor::bootstrap(dets, project, ibom::Layer::Front, 0.0);
    INFO(r.message);
    REQUIRE_FALSE(r.found);
}
