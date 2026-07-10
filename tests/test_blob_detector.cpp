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
using ibom::overlay::detectPadBlobs;
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
        comp.bbox      = { c.x - 1.25, c.y - 1.25, c.x + 1.25, c.y + 1.25 };
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

    // Production (Application) fits blob poses as a 4-DOF similarity: with
    // noisy blob centers the homography's perspective terms are noise-fit and
    // wobble the board corners (docs/BLOB_REANCHOR_JITTER_ANALYSE.md).
    ComponentReanchor::Params rp;
    rp.fitSimilarity = true;
    const auto boot = ComponentReanchor::bootstrap(dets, project, ibom::Layer::Front, s, rp);
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

TEST_CASE("blob pose is repeatable at the board corners across noisy frames", "[blob][reanchor]")
{
    // Regression test for the 2026-07-03 field jitter: the periodic re-anchor
    // shook the overlay 13-63 px every tick because each blob detection pass
    // found a slightly different region subset and the 8-DOF homography fit
    // noise-fitted its perspective terms — precise inside the component cloud
    // (median ~3 px) yet swinging the BOARD CORNERS, which is where the 12 px
    // drift gate (Application::componentReanchor) measures. The production fix
    // (docs/BLOB_REANCHOR_JITTER_ANALYSE.md): similarity fit + region-centroid
    // centers. This test re-detects on noisy renderings of the same scene and
    // requires consecutive poses to stay UNDER the drift gate at the corners —
    // i.e. the periodic tick would skip instead of yanking the overlay.
    std::vector<cv::Point2f> centers;
    const ibom::IBomProject project = makeProject(centers);

    const double s = 6.0, th = 15.0 * CV_PI / 180.0;
    const auto gt = [&](cv::Point2f p) {
        return cv::Point2f(
            static_cast<float>(s * std::cos(th) * p.x - s * std::sin(th) * p.y + 120.0),
            static_cast<float>(s * std::sin(th) * p.x + s * std::cos(th) * p.y + 90.0));
    };
    cv::Mat clean(720, 900, CV_8UC3, cv::Scalar(30, 30, 30));
    {
        std::vector<cv::Point> boardQuad;
        for (const cv::Point2f& corner : { cv::Point2f(0, 0), cv::Point2f(100, 0),
                                           cv::Point2f(100, 80), cv::Point2f(0, 80) }) {
            const cv::Point2f q = gt(corner);
            boardQuad.emplace_back(cvRound(q.x), cvRound(q.y));
        }
        cv::fillConvexPoly(clean, boardQuad, cv::Scalar(200, 200, 200));
    }
    const int half = static_cast<int>(std::round(1.25 * s));
    for (const auto& c : centers) {
        const cv::Point2f q = gt(c);
        cv::rectangle(clean,
                      cv::Rect(cvRound(q.x) - half, cvRound(q.y) - half, 2 * half, 2 * half),
                      cv::Scalar(35, 35, 40), cv::FILLED);
    }

    const std::vector<cv::Point2f> pcbCorners = {
        { 0.f, 0.f }, { 100.f, 0.f }, { 100.f, 80.f }, { 0.f, 80.f } };
    ComponentReanchor::Params rp;
    rp.fitSimilarity = true;  // production blob path

    cv::theRNG().state = 12345;  // deterministic frame noise
    std::vector<cv::Point2f> prev;
    int poses = 0;
    for (int frame = 0; frame < 6; ++frame) {
        // Per-frame sensor noise + illumination change: shifts which MSER
        // regions survive from frame to frame (the field failure mode — 26-41
        // matched out of 103-124 detections) without drowning the image in
        // noise-born speckle regions.
        cv::Mat noise(clean.size(), CV_8UC3);
        cv::randn(noise, 0, 4);
        cv::Mat img = clean + noise + cv::Scalar::all(3 * frame);

        const auto dets = detectComponentBlobs(img, s);
        const auto boot = ComponentReanchor::bootstrap(dets, project,
                                                       ibom::Layer::Front, s, rp);
        INFO("frame " << frame << ": " << dets.size() << " dets, " << boot.message);
        // A tick that doesn't lock is harmless in production (the silent
        // re-anchor just skips); what must never happen is two LOCKED poses
        // far apart at the corners — that's the overlay yank.
        if (!boot.found) continue;
        ++poses;

        std::vector<cv::Point2f> cur;
        cv::perspectiveTransform(pcbCorners, cur, boot.homography);
        if (!prev.empty()) {
            double maxShift = 0.0;
            for (size_t i = 0; i < 4; ++i)
                maxShift = std::max(maxShift, static_cast<double>(cv::norm(cur[i] - prev[i])));
            INFO("corner shift vs previous locked pose = " << maxShift << " px");
            // Same threshold as Application's kReanchorMinShiftPx drift gate.
            REQUIRE(maxShift < 12.0);
        }
        prev = std::move(cur);
    }
    // The scene is easy — locking must be the rule, not the exception.
    REQUIRE(poses >= 4);
}

TEST_CASE("blob detector returns nothing on a blank image", "[blob]")
{
    cv::Mat blank(480, 640, CV_8UC3, cv::Scalar(128, 128, 128));
    const auto dets = detectComponentBlobs(blank, 6.0);
    // A flat field has no stable component-sized regions.
    REQUIRE(dets.size() <= 2);
}

// ── detectPadBlobs — the bare-board path (ERREUR #59) ──────────────────────

namespace {

/// Bare-board scene generator: dark background (mat), slightly lighter
/// soldermask board, BRIGHT pad rectangles — 2-pad passives at ground-truth
/// positions. Returns the ground-truth pad centers (image px).
cv::Mat makeBareBoardImage(double s, int brightness,
                           std::vector<cv::Point2f>& padCentersOut)
{
    // Board 100×80 mm axis-aligned at (60, 40) px for simplicity.
    cv::Mat img(560, 760, CV_8UC3, cv::Scalar(25, 25, 25));           // mat
    const cv::Rect board(60, 40, static_cast<int>(100 * s),
                         static_cast<int>(80 * s));
    cv::rectangle(img, board, cv::Scalar(45, 62, 40), cv::FILLED);    // mask

    std::mt19937 rng(41);
    std::uniform_real_distribution<float> ux(12.f, 88.f);
    std::uniform_real_distribution<float> uy(12.f, 68.f);
    std::vector<cv::Point2f> centersMm;
    while (centersMm.size() < 30) {
        cv::Point2f c(ux(rng), uy(rng));
        bool ok = true;
        for (const auto& q : centersMm)
            if (std::hypot(c.x - q.x, c.y - q.y) < 6.f) { ok = false; break; }
        if (!ok) continue;
        centersMm.push_back(c);
        // Two 1.2×1.4 mm pads at ±1.1 mm around the component center.
        for (int k = -1; k <= 1; k += 2) {
            const cv::Point2f pmm(c.x + 1.1f * k, c.y);
            const cv::Point2f ppx(board.x + pmm.x * static_cast<float>(s),
                                  board.y + pmm.y * static_cast<float>(s));
            const int hw = std::max(2, static_cast<int>(0.6 * s));
            const int hh = std::max(2, static_cast<int>(0.7 * s));
            cv::rectangle(img,
                cv::Rect(cvRound(ppx.x) - hw, cvRound(ppx.y) - hh, 2 * hw, 2 * hh),
                cv::Scalar(brightness, brightness, brightness), cv::FILLED);
            padCentersOut.push_back(ppx);
        }
    }
    // Sensor noise (deterministic).
    cv::theRNG().state = 777;
    cv::Mat noise(img.size(), CV_8UC3);
    cv::randn(noise, 0, 3);
    img += noise;
    return img;
}

} // namespace

TEST_CASE("pad detector finds bright pads on a DIM bare board", "[blob][pads]")
{
    // Field failure ERREUR #59: dim scene → MSER yields pad-sized noise blobs
    // while real pads go undetected. The top-hat detector is relative to the
    // LOCAL background — pads at brightness 110 on a 50-ish mask must all be
    // found even though the whole scene is dark.
    const double s = 6.0;
    std::vector<cv::Point2f> padCenters;
    const cv::Mat img = makeBareBoardImage(s, /*brightness=*/110, padCenters);

    const auto dets = detectPadBlobs(img, s);
    INFO("pads drawn = " << padCenters.size() << ", detected = " << dets.size());
    REQUIRE(dets.size() >= padCenters.size() * 8 / 10);

    // Each detection's center must sit on a drawn pad (≤ 2.5 px).
    int onPad = 0;
    for (const auto& d : dets) {
        const cv::Point2f c(d.bbox.x + d.bbox.width * 0.5f,
                            d.bbox.y + d.bbox.height * 0.5f);
        for (const auto& gt : padCenters)
            if (cv::norm(c - gt) <= 2.5) { ++onPad; break; }
    }
    INFO("detections on a real pad = " << onPad << "/" << dets.size());
    REQUIRE(onPad >= static_cast<int>(dets.size()) - 3);  // ≤3 spurious
}

TEST_CASE("pad detector stays quiet on a blank noisy frame", "[blob][pads]")
{
    // Otsu on pure noise splits it — the absolute contrast floor must not.
    cv::Mat img(480, 640, CV_8UC3, cv::Scalar(40, 40, 40));
    cv::theRNG().state = 99;
    cv::Mat noise(img.size(), CV_8UC3);
    cv::randn(noise, 0, 6);
    img += noise;
    const auto dets = detectPadBlobs(img, 6.0);
    INFO("detections on noise = " << dets.size());
    REQUIRE(dets.size() <= 8);
}

TEST_CASE("pad detector + pads bootstrap align a bare board end to end", "[blob][pads][reanchor]")
{
    // The full ERREUR #57/#59 chain, model-free: bare board → bright-pad
    // detection → pad-constellation bootstrap → pose. The iBOM project carries
    // the same pads the image draws (absolute mm, like the parser fills them).
    const double s = 6.0;
    std::vector<cv::Point2f> padCentersPx;
    const cv::Mat img = makeBareBoardImage(s, /*brightness=*/120, padCentersPx);

    // Rebuild the matching project: same generator constants as the image.
    ibom::IBomProject project;
    {
        std::mt19937 rng(41);
        std::uniform_real_distribution<float> ux(12.f, 88.f);
        std::uniform_real_distribution<float> uy(12.f, 68.f);
        std::vector<cv::Point2f> centersMm;
        while (centersMm.size() < 30) {
            cv::Point2f c(ux(rng), uy(rng));
            bool ok = true;
            for (const auto& q : centersMm)
                if (std::hypot(c.x - q.x, c.y - q.y) < 6.f) { ok = false; break; }
            if (!ok) continue;
            centersMm.push_back(c);
            ibom::Component comp;
            comp.reference = "R" + std::to_string(centersMm.size());
            comp.layer     = ibom::Layer::Front;
            comp.bbox      = { c.x - 1.7, c.y - 0.7, c.x + 1.7, c.y + 0.7 };
            for (int k = -1; k <= 1; k += 2) {
                ibom::Pad pad;
                pad.position = { c.x + 1.1 * k, c.y };
                pad.sizeX = 1.2;
                pad.sizeY = 1.4;
                pad.isSMD = true;
                comp.pads.push_back(std::move(pad));
            }
            project.components.push_back(std::move(comp));
        }
        project.boardInfo.boardBBox = { 0.0, 0.0, 100.0, 80.0 };
    }

    const auto dets = detectPadBlobs(img, s);
    INFO("detections = " << dets.size());

    ComponentReanchor::Params rp;
    rp.fitSimilarity = true;
    rp.constellation = ComponentReanchor::Constellation::Pads;
    const auto boot = ComponentReanchor::bootstrap(dets, project,
                                                   ibom::Layer::Front, s, rp);
    INFO(boot.message);
    REQUIRE(boot.found);
    // A clean bare-board lock must be strongly supported — this is the number
    // that separates a real lock from the field aliases (44-65 %).
    REQUIRE(boot.inliers >= static_cast<int>(0.75 * boot.matches));

    // The image draws the board at (60, 40) px with scale s: the recovered
    // pose must map pad mm-positions onto the drawn pads.
    std::vector<cv::Point2f> pcb, proj;
    for (const auto& comp : project.components)
        for (const auto& pad : comp.pads)
            pcb.push_back({ static_cast<float>(pad.position.x),
                            static_cast<float>(pad.position.y) });
    cv::perspectiveTransform(pcb, proj, boot.homography);
    std::vector<double> errs;
    for (size_t i = 0; i < pcb.size(); ++i) {
        const cv::Point2f gt(60.f + pcb[i].x * static_cast<float>(s),
                             40.f + pcb[i].y * static_cast<float>(s));
        errs.push_back(cv::norm(proj[i] - gt));
    }
    std::nth_element(errs.begin(), errs.begin() + errs.size() / 2, errs.end());
    INFO("median reprojection vs drawn pads = " << errs[errs.size() / 2] << " px");
    REQUIRE(errs[errs.size() / 2] < 3.0);
}
