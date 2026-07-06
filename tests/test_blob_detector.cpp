// End-to-end test of the MODEL-FREE alignment path: a synthetic PCB image
// (dark component rectangles on a light board) → detectComponentBlobs (classic
// CV) → ComponentReanchor::bootstrap → a pose that maps the iBOM layout back
// onto the drawn components. This is what lets Auto-Align / re-anchor work with
// an empty models/ (no trained detector).

#include <catch2/catch_test_macros.hpp>

#include "overlay/BlobComponentDetector.h"
#include "overlay/ComponentReanchor.h"
#include "ibom/IBomData.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace ai = ibom::ai;
using ibom::overlay::detectComponentBlobs;
using ibom::overlay::ComponentReanchor;

namespace {

ibom::IBomProject makeProject(std::vector<cv::Point2f>& centersOut)
{
    ibom::IBomProject p;
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> ux(5.f, 95.f);
    std::uniform_real_distribution<float> uy(5.f, 75.f);
    // Rejection-sample so components don't overlap once drawn (min 6 mm pitch),
    // otherwise adjacent dark rects merge into one MSER blob.
    std::vector<cv::Point2f> pts;
    while (pts.size() < 30) {
        cv::Point2f c(ux(rng), uy(rng));
        bool ok = true;
        for (const auto& q : pts)
            if (std::hypot(c.x - q.x, c.y - q.y) < 6.f) { ok = false; break; }
        if (!ok) continue;
        pts.push_back(c);
        ibom::Component comp;
        comp.reference = "U" + std::to_string(pts.size());
        comp.layer     = ibom::Layer::Front;
        comp.position  = { c.x, c.y };
        p.components.push_back(std::move(comp));
    }
    centersOut = pts;
    p.boardInfo.boardBBox = { 0.0, 0.0, 100.0, 80.0 };
    return p;
}

} // namespace

TEST_CASE("blob detector + bootstrap aligns a synthetic PCB with no model", "[blob][reanchor]")
{
    std::vector<cv::Point2f> centers;
    const ibom::IBomProject project = makeProject(centers);

    // Ground truth: 6 px/mm, 15° rotation, translation (120, 90).
    const double s = 6.0, th = 15.0 * CV_PI / 180.0;
    const auto gt = [&](cv::Point2f p) {
        return cv::Point2f(
            static_cast<float>(s * std::cos(th) * p.x - s * std::sin(th) * p.y + 120.0),
            static_cast<float>(s * std::sin(th) * p.x + s * std::cos(th) * p.y + 90.0));
    };

    // Light board, dark component bodies (~2.5 mm → 15 px) at gt positions.
    cv::Mat img(720, 900, CV_8UC3, cv::Scalar(30, 30, 30));
    {
        std::vector<cv::Point> boardQuad;
        for (const cv::Point2f& corner : { cv::Point2f(0, 0), cv::Point2f(100, 0),
                                           cv::Point2f(100, 80), cv::Point2f(0, 80) }) {
            const cv::Point2f q = gt(corner);
            boardQuad.emplace_back(cvRound(q.x), cvRound(q.y));
        }
        cv::fillConvexPoly(img, boardQuad, cv::Scalar(200, 200, 200));  // board
    }
    const int half = static_cast<int>(std::round(1.25 * s));  // 2.5 mm body / 2
    for (const auto& c : centers) {
        const cv::Point2f q = gt(c);
        cv::rectangle(img,
                      cv::Rect(cvRound(q.x) - half, cvRound(q.y) - half, 2 * half, 2 * half),
                      cv::Scalar(35, 35, 40), cv::FILLED);
    }

    // Detect blobs with the physical scale prior (as the D405 pinhole path would).
    const std::vector<ai::Detection> dets = detectComponentBlobs(img, s);
    INFO("blobs detected = " << dets.size());
    REQUIRE(dets.size() >= 12);  // most components, tolerating some misses/merges

    const auto boot = ComponentReanchor::bootstrap(dets, project, ibom::Layer::Front, s);
    REQUIRE(boot.found);
    REQUIRE(boot.inliers >= 8);

    // Recovered pose maps component centers back onto their drawn positions.
    std::vector<cv::Point2f> proj;
    cv::perspectiveTransform(centers, proj, boot.homography);
    std::vector<double> errs;
    for (size_t i = 0; i < centers.size(); ++i)
        errs.push_back(cv::norm(proj[i] - gt(centers[i])));
    std::nth_element(errs.begin(), errs.begin() + errs.size() / 2, errs.end());
    INFO("median reprojection = " << errs[errs.size() / 2] << " px");
    REQUIRE(errs[errs.size() / 2] < 6.0);
}

TEST_CASE("blob detector returns nothing on a blank image", "[blob]")
{
    cv::Mat blank(480, 640, CV_8UC3, cv::Scalar(128, 128, 128));
    const auto dets = detectComponentBlobs(blank, 6.0);
    // A flat field has no stable component-sized regions.
    REQUIRE(dets.size() <= 2);
}
