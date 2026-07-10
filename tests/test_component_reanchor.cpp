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
// The production code refers to ai::Detection from inside namespace
// ibom::overlay, where `ai` resolves relative to `ibom`. This file is at
// global scope — alias the nested namespace so the same spelling works.
namespace ai = ibom::ai;

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
        // Small bbox around the center: ComponentReanchor uses the bbox center
        // (position is often unpopulated in real iBOMs), so exercise that path.
        c.bbox = { c.position.x - 1.0, c.position.y - 1.0,
                   c.position.x + 1.0, c.position.y + 1.0 };
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
        INFO(r.message);
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
    SECTION("similarity fit (production blob path)") {
        // The ground truth IS a similarity, so the 4-DOF fit must lock just as
        // well — this exercises the estimateAffinePartial2D branch end to end
        // (bootstrap propagates fitSimilarity into the estimate() refinement).
        ComponentReanchor::Params p;
        p.fitSimilarity = true;
        verify(ComponentReanchor::bootstrap(dets, project, ibom::Layer::Front, 8.0, p));
    }
}

TEST_CASE("bootstrap registers a BACK-side view (mirrored constellation)", "[reanchor]")
{
    std::vector<cv::Point2f> centers;
    ibom::IBomProject project = makeProject(centers);
    for (auto& c : project.components) c.layer = ibom::Layer::Back;

    // Ground truth back view: mirror (x → −x) then a similarity — what the
    // camera sees once the board is physically flipped. A similarity alone
    // cannot represent this; ComponentReanchor must compose the mirror in.
    const double s = 7.0, th = -12.0 * CV_PI / 180.0;
    const auto gtB = [&](cv::Point2f p) {
        const double x = -p.x;
        return cv::Point2f(
            static_cast<float>(s * std::cos(th) * x - s * std::sin(th) * p.y + 700.0),
            static_cast<float>(s * std::sin(th) * x + s * std::cos(th) * p.y + 150.0));
    };
    std::mt19937 rng(5);
    std::normal_distribution<float> noise(0.f, 1.0f);
    std::vector<ai::Detection> dets;
    for (size_t i = 0; i < centers.size(); ++i) {
        if (i % 5 == 4) continue;  // 20% dropouts
        cv::Point2f q = gtB(centers[i]);
        q.x += noise(rng);
        q.y += noise(rng);
        dets.push_back(detectionAt(q));
    }

    ComponentReanchor::Params p;
    p.fitSimilarity = true;  // production blob path
    const auto r = ComponentReanchor::bootstrap(dets, project,
                                                ibom::Layer::Back, s, p);
    INFO(r.message);
    REQUIRE(r.found);

    // The returned homography maps RAW pcb coords with the mirror inside:
    // negative determinant of the linear part…
    const double det =
        r.homography.at<double>(0, 0) * r.homography.at<double>(1, 1)
        - r.homography.at<double>(0, 1) * r.homography.at<double>(1, 0);
    REQUIRE(det < 0.0);
    // …and it reprojects raw centers onto the mirrored view positions.
    std::vector<cv::Point2f> proj;
    cv::perspectiveTransform(centers, proj, r.homography);
    std::vector<double> errs;
    for (size_t i = 0; i < centers.size(); ++i)
        errs.push_back(cv::norm(proj[i] - gtB(centers[i])));
    std::nth_element(errs.begin(), errs.begin() + errs.size() / 2, errs.end());
    INFO("median reprojection vs ground truth = " << errs[errs.size() / 2] << " px");
    REQUIRE(errs[errs.size() / 2] < 4.0);
}

namespace {

// Bare-board project (ERREUR #57): the pads carry the observable geometry —
// varied 2-pad footprints (horizontal and vertical) at absolute board coords,
// exactly how IBomParser fills Pad::position. Component centers are what the
// recovered pose is validated against.
ibom::IBomProject makeBareProject(std::vector<cv::Point2f>& centersOut,
                                  std::vector<cv::Point2f>& padsOut)
{
    ibom::IBomProject p;
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> ux(5.f, 95.f);
    std::uniform_real_distribution<float> uy(5.f, 75.f);
    for (int i = 0; i < 30; ++i) {
        ibom::Component c;
        c.reference = "R" + std::to_string(i);
        c.layer     = ibom::Layer::Front;
        c.position  = { ux(rng), uy(rng) };
        c.bbox = { c.position.x - 1.5, c.position.y - 1.0,
                   c.position.x + 1.5, c.position.y + 1.0 };
        const bool vertical = (i % 3 == 0);
        for (int k = -1; k <= 1; k += 2) {
            ibom::Pad pad;
            pad.position = vertical
                ? ibom::Point2D{ c.position.x, c.position.y + 1.1 * k }
                : ibom::Point2D{ c.position.x + 1.1 * k, c.position.y };
            pad.sizeX = 1.0;
            pad.sizeY = 1.2;
            pad.isSMD = true;
            padsOut.push_back({ static_cast<float>(pad.position.x),
                                static_cast<float>(pad.position.y) });
            c.pads.push_back(std::move(pad));
        }
        centersOut.push_back({ static_cast<float>(c.position.x),
                               static_cast<float>(c.position.y) });
        p.components.push_back(std::move(c));
    }
    p.boardInfo.boardBBox = { 0.0, 0.0, 100.0, 80.0 };
    return p;
}

// A similarity prior as ComponentReanchor consumes it (raw PCB mm → image px).
ibom::overlay::Homography makeSimilarityPose(double s, double tx, double ty)
{
    cv::Mat H = (cv::Mat_<double>(3, 3) << s, 0, tx,  0, s, ty,  0, 0, 1);
    ibom::overlay::Homography pose;
    pose.setMatrix(H);
    return pose;
}

} // namespace

TEST_CASE("pads constellation registers a BARE board", "[reanchor]")
{
    // Field scenario ERREUR #57: bare PCB under a D405 at ~100 mm — the blob
    // detector sees the shiny tinned PADS, not component bodies. The pad
    // constellation makes that a well-posed registration.
    std::vector<cv::Point2f> centers, padPts;
    const ibom::IBomProject project = makeBareProject(centers, padPts);

    // Ground truth at the field scale: 4.4 px/mm, 15° rotation.
    const double s = 4.4, th = 15.0 * CV_PI / 180.0;
    const auto gt = [&](cv::Point2f p) {
        return cv::Point2f(
            static_cast<float>(s * std::cos(th) * p.x - s * std::sin(th) * p.y + 250.0),
            static_cast<float>(s * std::sin(th) * p.x + s * std::cos(th) * p.y + 120.0));
    };

    // Detections = pads (that is the point), ±0.8 px noise, 20% dropouts,
    // 8 spurious blobs (vias, silkscreen).
    std::mt19937 rng(31);
    std::normal_distribution<float> noise(0.f, 0.8f);
    std::uniform_real_distribution<float> fx(0.f, 848.f), fy(0.f, 480.f);
    std::vector<ai::Detection> dets;
    for (size_t i = 0; i < padPts.size(); ++i) {
        if (i % 5 == 4) continue;
        cv::Point2f q = gt(padPts[i]);
        q.x += noise(rng);
        q.y += noise(rng);
        dets.push_back(detectionAt(q));
    }
    for (int i = 0; i < 8; ++i)
        dets.push_back(detectionAt({ fx(rng), fy(rng) }));

    ComponentReanchor::Params p;
    p.fitSimilarity = true;  // production blob path
    p.constellation = ComponentReanchor::Constellation::Pads;
    const auto r = ComponentReanchor::bootstrap(dets, project,
                                                ibom::Layer::Front, s, p);
    INFO(r.message);
    REQUIRE(r.found);
    REQUIRE(r.message.find("pads") != std::string::npos);

    // The recovered pose must map COMPONENT CENTERS (what the overlay draws)
    // onto their ground-truth positions.
    std::vector<cv::Point2f> proj;
    cv::perspectiveTransform(centers, proj, r.homography);
    std::vector<double> errs;
    for (size_t i = 0; i < centers.size(); ++i)
        errs.push_back(cv::norm(proj[i] - gt(centers[i])));
    std::nth_element(errs.begin(), errs.begin() + errs.size() / 2, errs.end());
    INFO("median reprojection vs ground truth = " << errs[errs.size() / 2] << " px");
    REQUIRE(errs[errs.size() / 2] < 4.0);
}

TEST_CASE("inlier-ratio gate rejects a constellation coincidence", "[reanchor]")
{
    // The exact ERREUR #57 signature: enough detections agree with SOME pose
    // to clear the absolute gates (12 ≥ minInliers, tiny median error), but
    // they are a minority of the gated matches (12/40 = 30 % — the field lock
    // was 40/117 = 34 %, applied with a perfect synthetic score).
    std::vector<cv::Point2f> centers;
    const ibom::IBomProject project = makeProject(centers);
    const auto prior = makeSimilarityPose(5.0, 100.0, 60.0);

    std::vector<ai::Detection> dets;
    for (size_t i = 0; i < centers.size(); ++i) {
        cv::Point2f q(static_cast<float>(5.0 * centers[i].x + 100.0),
                      static_cast<float>(5.0 * centers[i].y + 60.0));
        if (i >= 12) {
            // Structureless disagreement: 18-28 px away from the predicted
            // position — inside the legacy 60 px gate, far outside the 6 px
            // RANSAC band, and with no similarity consistent across them.
            const double a = 0.7 * static_cast<double>(i);
            const double m = 18.0 + static_cast<double>(i % 6) * 2.0;
            q.x += static_cast<float>(m * std::cos(a));
            q.y += static_cast<float>(m * std::sin(a));
        }
        dets.push_back(detectionAt(q));
    }

    ComponentReanchor::Params p;
    p.fitSimilarity = true;

    SECTION("default ratio gate rejects it") {
        const auto r = ComponentReanchor::estimate(dets, project, prior,
                                                   ibom::Layer::Front, {}, p);
        INFO(r.message);
        REQUIRE_FALSE(r.found);
        REQUIRE(r.message.find("inlier ratio") != std::string::npos);
    }
    SECTION("disabling the gate restores the old (unsafe) acceptance") {
        p.minInlierRatio = 0.0;
        const auto r = ComponentReanchor::estimate(dets, project, prior,
                                                   ibom::Layer::Front, {}, p);
        INFO(r.message);
        REQUIRE(r.found);  // proves the ratio gate is what rejected it above
    }
}

TEST_CASE("physical matching gate scales with px/mm", "[reanchor]")
{
    // 60 px is 13.6 mm at the D405 wide view (4.4 px/mm) — wide enough to
    // gate anything onto something. A 5 mm physical gate is 22 px there.
    std::vector<cv::Point2f> centers;
    const ibom::IBomProject project = makeProject(centers);
    const double s = 4.4;
    const auto prior = makeSimilarityPose(s, 200.0, 100.0);

    // Every detection sits 30 px from its predicted position: inside the
    // legacy 60 px gate, outside the 22 px physical gate.
    std::vector<ai::Detection> dets;
    for (size_t i = 0; i < centers.size(); ++i) {
        cv::Point2f q(static_cast<float>(s * centers[i].x + 200.0),
                      static_cast<float>(s * centers[i].y + 100.0));
        const double a = 1.3 * static_cast<double>(i);
        q.x += static_cast<float>(30.0 * std::cos(a));
        q.y += static_cast<float>(30.0 * std::sin(a));
        dets.push_back(detectionAt(q));
    }

    ComponentReanchor::Params p;
    p.fitSimilarity = true;

    // Legacy 60 px gate: essentially everything pairs up (each detection is
    // 30 px from its own component's prediction).
    const auto legacy = ComponentReanchor::estimate(dets, project, prior,
                                                    ibom::Layer::Front, {}, p);
    INFO("legacy: " << legacy.message << " (matches " << legacy.matches << ")");
    REQUIRE(legacy.matches >= 30);
    REQUIRE_FALSE(legacy.found);  // structureless offsets — rejected downstream

    // 5 mm physical gate = 22 px at this scale: the 30 px own-matches are
    // excluded. A few detections may still land within 22 px of a NEIGHBOR's
    // prediction on a dense board — the gate cannot prevent cross-matches,
    // only starve them — so assert a massive reduction, not zero.
    p.matchGateMm  = 5.0;
    p.scalePxPerMm = s;
    const auto phys = ComponentReanchor::estimate(dets, project, prior,
                                                  ibom::Layer::Front, {}, p);
    INFO("physical: " << phys.message << " (matches " << phys.matches << ")");
    REQUIRE_FALSE(phys.found);
    REQUIRE(phys.matches <= legacy.matches / 3);
}

namespace {

// Repetitive pad lattice (the ERREUR #58 board): a regular grid aliases under
// shifts and 180° rotation; a handful of distinctive off-grid pads break the
// symmetry — barely. nx × ny grid at `pitch` mm plus `extras` markers.
ibom::IBomProject makeLatticeProject(int nx, int ny, float pitch,
                                     const std::vector<cv::Point2f>& extras,
                                     std::vector<cv::Point2f>& padsOut)
{
    ibom::IBomProject p;
    auto addPad = [&](ibom::Component& c, float x, float y) {
        ibom::Pad pad;
        pad.position = { x, y };
        pad.sizeX = 1.0;
        pad.sizeY = 1.0;
        pad.isSMD = true;
        padsOut.push_back({ x, y });
        c.pads.push_back(std::move(pad));
    };
    int idx = 0;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            ibom::Component c;
            c.reference = "G" + std::to_string(idx++);
            c.layer     = ibom::Layer::Front;
            const float x = 10.f + i * pitch, y = 10.f + j * pitch;
            c.bbox = { x - 0.5, y - 0.5, x + 0.5, y + 0.5 };
            addPad(c, x, y);
            p.components.push_back(std::move(c));
        }
    for (const auto& e : extras) {
        ibom::Component c;
        c.reference = "M" + std::to_string(idx++);
        c.layer     = ibom::Layer::Front;
        c.bbox = { e.x - 0.5, e.y - 0.5, e.x + 0.5, e.y + 0.5 };
        addPad(c, e.x, e.y);
        p.components.push_back(std::move(c));
    }
    p.boardInfo.boardBBox = { 0.0, 0.0, 20.0 + (nx - 1) * pitch,
                              20.0 + (ny - 1) * pitch };
    return p;
}

} // namespace

TEST_CASE("scale-compatible sampling resolves a repetitive lattice", "[reanchor]")
{
    // 12×10 grid (2 mm pitch) + 8 distinctive markers: shift/rotation aliases
    // score high (the lattice mostly lands on itself) but the true pose is
    // strictly better — provided the sampler actually DRAWS it. Blind
    // pair→pair sampling drowned it among the aliases (ERREUR #58).
    const std::vector<cv::Point2f> extras = {
        { 3.f, 4.f }, { 36.f, 5.5f }, { 2.5f, 25.f }, { 34.f, 27.f },
        { 5.f, 15.5f }, { 33.f, 16.f }, { 19.f, 2.5f }, { 17.f, 30.f }
    };
    std::vector<cv::Point2f> pads;
    const ibom::IBomProject project = makeLatticeProject(12, 10, 2.0f, extras, pads);

    // Ground truth: the field scale, board rotated 90° on the bench — the
    // user's exact scenario (rotating the board to match made it lock).
    const double s = 4.4, th = 90.0 * CV_PI / 180.0;
    const auto gt = [&](cv::Point2f p) {
        return cv::Point2f(
            static_cast<float>(s * std::cos(th) * p.x - s * std::sin(th) * p.y + 500.0),
            static_cast<float>(s * std::sin(th) * p.x + s * std::cos(th) * p.y + 100.0));
    };
    std::mt19937 rng(17);
    std::normal_distribution<float> noise(0.f, 0.5f);
    std::vector<ai::Detection> dets;
    for (const auto& p : pads) {
        cv::Point2f q = gt(p);
        q.x += noise(rng);
        q.y += noise(rng);
        dets.push_back(detectionAt(q));
    }

    ComponentReanchor::Params rp;
    rp.fitSimilarity = true;
    rp.constellation = ComponentReanchor::Constellation::Pads;
    const auto r = ComponentReanchor::bootstrap(dets, project,
                                                ibom::Layer::Front, s, rp);
    INFO(r.message);
    REQUIRE(r.found);

    std::vector<cv::Point2f> proj;
    cv::perspectiveTransform(pads, proj, r.homography);
    std::vector<double> errs;
    for (size_t i = 0; i < pads.size(); ++i)
        errs.push_back(cv::norm(proj[i] - gt(pads[i])));
    std::nth_element(errs.begin(), errs.begin() + errs.size() / 2, errs.end());
    INFO("median reprojection vs ground truth = " << errs[errs.size() / 2] << " px");
    REQUIRE(errs[errs.size() / 2] < 3.0);
}

TEST_CASE("bootstrap refuses a perfectly symmetric lattice as ambiguous", "[reanchor]")
{
    // Pure 8×8 grid, no distinctive feature: the 180°-rotated pose scores
    // EXACTLY the same consensus as the true one. Picking one is a coin flip
    // that lands the overlay upside down half the time — the ambiguity margin
    // must refuse instead (ERREUR #58).
    std::vector<cv::Point2f> pads;
    const ibom::IBomProject project =
        makeLatticeProject(8, 8, 2.0f, {}, pads);

    const double s = 5.0;
    std::vector<ai::Detection> dets;
    for (const auto& p : pads)
        dets.push_back(detectionAt({ static_cast<float>(s * p.x + 300.0),
                                     static_cast<float>(s * p.y + 150.0) }));

    ComponentReanchor::Params rp;
    rp.fitSimilarity = true;
    rp.constellation = ComponentReanchor::Constellation::Pads;
    const auto r = ComponentReanchor::bootstrap(dets, project,
                                                ibom::Layer::Front, s, rp);
    INFO(r.message);
    REQUIRE_FALSE(r.found);
    REQUIRE(r.message.find("ambiguous") != std::string::npos);
}

TEST_CASE("orientation vote fixes the corner assignment the outline can't", "[reanchor]")
{
    // Field pipeline "contour + pads" (suite 142): BoardLocator reliably finds
    // the board QUAD but its edge-agreement pick of the orientation is weakly
    // discriminative — simulate it handing over the right quad with the WRONG
    // corner assignment (off by one = 90°). The pad vote must recover the true
    // one and lock with strong support.
    std::vector<cv::Point2f> centers, padPts;
    const ibom::IBomProject project = makeBareProject(centers, padPts);

    // Physical scene: board rotated 90° on the bench, field scale.
    const double s = 4.4, th = 90.0 * CV_PI / 180.0;
    const auto gt = [&](cv::Point2f p) {
        return cv::Point2f(
            static_cast<float>(s * std::cos(th) * p.x - s * std::sin(th) * p.y + 480.0),
            static_cast<float>(s * std::sin(th) * p.x + s * std::cos(th) * p.y + 60.0));
    };
    std::mt19937 rng(53);
    std::normal_distribution<float> noise(0.f, 0.5f);
    std::vector<ai::Detection> dets;
    for (size_t i = 0; i < padPts.size(); ++i) {
        if (i % 10 == 9) continue;  // 10% dropouts
        cv::Point2f q = gt(padPts[i]);
        q.x += noise(rng);
        q.y += noise(rng);
        dets.push_back(detectionAt(q));
    }

    // The located quad: exact corner positions, assignment shifted by one —
    // i.e. the outline said "rot 0" while the truth is one step away.
    const auto& bb = project.boardInfo.boardBBox;
    const std::vector<cv::Point2f> pcb = {
        { static_cast<float>(bb.minX), static_cast<float>(bb.minY) },
        { static_cast<float>(bb.maxX), static_cast<float>(bb.minY) },
        { static_cast<float>(bb.maxX), static_cast<float>(bb.maxY) },
        { static_cast<float>(bb.minX), static_cast<float>(bb.maxY) }
    };
    std::vector<cv::Point2f> located(4);
    for (int i = 0; i < 4; ++i) located[i] = gt(pcb[(i + 1) % 4]);

    ComponentReanchor::Params p;
    p.fitSimilarity = true;
    p.constellation = ComponentReanchor::Constellation::Pads;
    p.matchGateMm   = 5.0;
    p.scalePxPerMm  = s;

    const auto r = ComponentReanchor::estimateOrientations(
        dets, project, located, ibom::Layer::Front, p);
    INFO(r.message);
    REQUIRE(r.found);
    REQUIRE(r.message.find("orientation vote") != std::string::npos);
    REQUIRE(r.inliers >= static_cast<int>(0.7 * r.matches));

    std::vector<cv::Point2f> proj;
    cv::perspectiveTransform(centers, proj, r.homography);
    std::vector<double> errs;
    for (size_t i = 0; i < centers.size(); ++i)
        errs.push_back(cv::norm(proj[i] - gt(centers[i])));
    std::nth_element(errs.begin(), errs.begin() + errs.size() / 2, errs.end());
    INFO("median reprojection vs ground truth = " << errs[errs.size() / 2] << " px");
    REQUIRE(errs[errs.size() / 2] < 3.0);
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
