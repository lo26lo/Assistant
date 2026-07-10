#include "ComponentReanchor.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>  // getPerspectiveTransform (orientation vote)
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace ibom::overlay {

namespace {

cv::Point2f detectionCenter(const ai::Detection& d)
{
    return { d.bbox.x + d.bbox.width * 0.5f,
             d.bbox.y + d.bbox.height * 0.5f };
}

double medianOf(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/// Component reference point in PCB coords. Uses the bbox center — always
/// populated by the parser — NOT Component::position, which is only filled
/// when the iBOM footprint has a "center" field (many don't; it then stays
/// (0,0), collapsing every component to the origin → "degenerate layout").
cv::Point2f componentCenter(const ibom::Component& c)
{
    const double w = c.bbox.maxX - c.bbox.minX;
    const double h = c.bbox.maxY - c.bbox.minY;
    if (w > 1e-6 && h > 1e-6)
        return { static_cast<float>(0.5 * (c.bbox.minX + c.bbox.maxX)),
                 static_cast<float>(0.5 * (c.bbox.minY + c.bbox.maxY)) };
    return { static_cast<float>(c.position.x), static_cast<float>(c.position.y) };
}

/// Expected constellation in raw PCB mm. Components → one point per component
/// (bbox center); Pads → every pad position (absolute board coords, straight
/// from the parser — OverlayRenderer draws them unshifted), falling back to
/// the component center for a padless component. maxPoints caps the list for
/// bootstrap's O(nRef × nDet) consensus loop, keeping the LARGEST pads — the
/// ones the blob detector actually sees.
struct RefPoint { size_t compIdx; cv::Point2f raw; double sortKey; };

std::vector<RefPoint> buildConstellation(const ibom::IBomProject& project,
                                         ibom::Layer layer,
                                         ComponentReanchor::Constellation src,
                                         size_t maxPoints = 0)
{
    std::vector<RefPoint> pts;
    for (size_t i = 0; i < project.components.size(); ++i) {
        const auto& c = project.components[i];
        if (c.layer != layer) continue;
        if (src == ComponentReanchor::Constellation::Pads && !c.pads.empty()) {
            for (const auto& pad : c.pads)
                pts.push_back({ i,
                    { static_cast<float>(pad.position.x),
                      static_cast<float>(pad.position.y) },
                    pad.sizeX * pad.sizeY });
        } else {
            pts.push_back({ i, componentCenter(c), 0.0 });
        }
    }
    if (maxPoints > 0 && pts.size() > maxPoints) {
        std::partial_sort(pts.begin(), pts.begin() + maxPoints, pts.end(),
            [](const RefPoint& a, const RefPoint& b) { return a.sortKey > b.sortKey; });
        pts.resize(maxPoints);
    }
    return pts;
}

const char* constellationName(ComponentReanchor::Constellation c)
{
    return c == ComponentReanchor::Constellation::Pads ? "pads" : "components";
}

/// Back side: the camera sees the layout MIRRORED. A similarity (and the
/// bootstrap's pair→pair hypotheses) cannot represent a mirror, so the fits
/// run in a "view frame" — x negated for Layer::Back (the mirror axis choice
/// is irrelevant: the fitted translation absorbs any offset) — and the mirror
/// is composed back into the returned matrix, preserving the app-wide
/// convention that a homography always maps RAW PCB mm → image px (with a
/// negative determinant when looking at the back).
cv::Point2f viewPoint(cv::Point2f p, bool mirrored)
{
    return mirrored ? cv::Point2f(-p.x, p.y) : p;
}

cv::Mat mirrorX()
{
    return (cv::Mat_<double>(3, 3) << -1, 0, 0,  0, 1, 0,  0, 0, 1);
}

} // namespace

ComponentReanchorResult ComponentReanchor::estimate(
    const std::vector<ai::Detection>& detections,
    const ibom::IBomProject& project,
    const Homography& currentPose,
    ibom::Layer activeLayer,
    const std::vector<int>& classOfComponent)
{
    // Built here, not as a `= {}` default argument in the header: Params is a
    // nested aggregate, and ComponentReanchor is a complete type by the time
    // this translation unit compiles, so aggregate-initializing it from its
    // default member initializers is unproblematic (see the declaration
    // comment in ComponentReanchor.h for why the header can't do this).
    return estimate(detections, project, currentPose, activeLayer,
                    classOfComponent, Params{});
}

ComponentReanchorResult ComponentReanchor::estimate(
    const std::vector<ai::Detection>& detections,
    const ibom::IBomProject& project,
    const Homography& currentPose,
    ibom::Layer activeLayer,
    const std::vector<int>& classOfComponent,
    const Params& params)
{
    ComponentReanchorResult r;

    if (!currentPose.isValid()) {
        r.message = "no current pose to use as matching prior";
        return r;
    }
    if (detections.empty()) {
        r.message = "no detections in frame";
        return r;
    }

    const bool useClass = params.useClassPrior
        && classOfComponent.size() == project.components.size();
    const bool mirrored = (activeLayer == ibom::Layer::Back);

    // Predicted image position of every constellation point (component
    // centers, or pads on a bare board) on the active layer. Prediction goes
    // through the RAW coords (currentPose maps raw PCB → image, mirror
    // included); the fit runs on view-frame coords.
    struct Cand {
        size_t compIdx;
        cv::Point2f pcb;      // view-frame coords (x negated for Back)
        cv::Point2f predImg;
        int cls;
    };
    const auto refs = buildConstellation(project, activeLayer, params.constellation);
    std::vector<Cand> cands;
    cands.reserve(refs.size());
    for (const auto& rp : refs) {
        cands.push_back({ rp.compIdx, viewPoint(rp.raw, mirrored),
                          currentPose.pcbToImage(rp.raw),
                          useClass ? classOfComponent[rp.compIdx] : -1 });
    }
    if (cands.size() < static_cast<size_t>(params.minMatches)) {
        r.message = std::string("too few ") + constellationName(params.constellation)
                  + " on active layer to re-anchor";
        return r;
    }

    // Candidate (detection, constellation-point) pairs within the gating
    // radius, sorted by distance, then assigned greedily so each is used at
    // most once. The gate is physical when the caller provides a scale
    // (see Params::matchGateMm).
    struct Pair { double dist; size_t det; size_t cand; };
    std::vector<Pair> pairs;
    double gatePx = params.maxMatchDistPx;
    if (params.matchGateMm > 0.0 && params.scalePxPerMm > 0.0)
        gatePx = std::clamp(params.matchGateMm * params.scalePxPerMm, 15.0, 90.0);
    const double maxD2 = gatePx * gatePx;
    for (size_t di = 0; di < detections.size(); ++di) {
        const cv::Point2f dc = detectionCenter(detections[di]);
        for (size_t ci = 0; ci < cands.size(); ++ci) {
            if (useClass && cands[ci].cls >= 0
                && cands[ci].cls != detections[di].classId)
                continue;
            const double dx = dc.x - cands[ci].predImg.x;
            const double dy = dc.y - cands[ci].predImg.y;
            const double d2 = dx * dx + dy * dy;
            if (d2 <= maxD2)
                pairs.push_back({ std::sqrt(d2), di, ci });
        }
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const Pair& a, const Pair& b) { return a.dist < b.dist; });

    std::vector<char> detUsed(detections.size(), 0);
    std::vector<char> candUsed(cands.size(), 0);
    std::vector<cv::Point2f> pcbPts, imgPts;
    for (const auto& p : pairs) {
        if (detUsed[p.det] || candUsed[p.cand]) continue;
        detUsed[p.det] = candUsed[p.cand] = 1;
        pcbPts.push_back(cands[p.cand].pcb);
        imgPts.push_back(detectionCenter(detections[p.det]));
    }

    r.matches = static_cast<int>(pcbPts.size());
    if (r.matches < params.minMatches) {
        r.message = "too few component<->detection matches (" +
                    std::to_string(r.matches) + ")";
        return r;
    }

    cv::Mat mask;
    cv::Mat H;
    if (params.fitSimilarity) {
        // 4-DOF similarity: on a fronto-parallel scene with noisy centers the
        // homography's perspective terms are noise-fit and wobble the board
        // corners by tens of px (see Params::fitSimilarity).
        const cv::Mat A = cv::estimateAffinePartial2D(
            pcbPts, imgPts, mask, cv::RANSAC, params.ransacThreshPx);
        if (A.empty()) {
            r.message = "estimateAffinePartial2D failed";
            return r;
        }
        H = cv::Mat::eye(3, 3, CV_64F);
        A.copyTo(H(cv::Rect(0, 0, 3, 2)));
    } else {
        H = cv::findHomography(pcbPts, imgPts, cv::RANSAC,
                               params.ransacThreshPx, mask);
        if (H.empty()) {
            r.message = "findHomography failed";
            return r;
        }
    }

    // Inlier count + median reprojection error among inliers.
    std::vector<cv::Point2f> proj;
    cv::perspectiveTransform(pcbPts, proj, H);
    std::vector<double> inlierErr;
    for (int i = 0; i < static_cast<int>(pcbPts.size()); ++i) {
        if (!mask.empty() && !mask.at<uchar>(i)) continue;
        inlierErr.push_back(cv::norm(proj[i] - imgPts[i]));
    }
    r.inliers = static_cast<int>(inlierErr.size());
    r.medianReprojPx = medianOf(inlierErr);

    if (r.inliers < params.minInliers) {
        r.message = "too few RANSAC inliers (" + std::to_string(r.inliers) +
                    " < " + std::to_string(params.minInliers) + ")";
        return r;
    }
    // Relative support gate: an aliased pose on a repetitive/bare layout can
    // clear the ABSOLUTE gates with a minority of the matches agreeing
    // (field case ERREUR #57: 40/117 = 34 % accepted, wrong pose applied).
    if (params.minInlierRatio > 0.0 &&
        r.inliers < params.minInlierRatio * r.matches) {
        r.message = "inlier ratio too low (" + std::to_string(r.inliers) + "/" +
                    std::to_string(r.matches) +
                    ") — constellation coincidence, not a lock";
        return r;
    }
    if (r.medianReprojPx > params.maxMedianReprojPx) {
        r.message = "median reprojection error too high (" +
                    std::to_string(r.medianReprojPx) + "px)";
        return r;
    }

    r.found = true;
    // H was fitted in the view frame — compose the mirror back in so the
    // returned homography maps RAW PCB coords (see viewPoint()).
    r.homography = mirrored ? cv::Mat(H * mirrorX()) : H;
    r.message = "re-anchored on " + std::to_string(r.inliers) + "/" +
                std::to_string(r.matches) + " " +
                constellationName(params.constellation) + ", median " +
                std::to_string(r.medianReprojPx) + "px";
    spdlog::debug("[comp-reanchor] {}", r.message);
    return r;
}

ComponentReanchorResult ComponentReanchor::bootstrap(
    const std::vector<ai::Detection>& detections,
    const ibom::IBomProject& project,
    ibom::Layer activeLayer,
    double scalePriorPxPerMm)
{
    // Params{} built here, where ComponentReanchor is a complete type — see
    // the estimate() convenience overload above (ERREUR #53).
    return bootstrap(detections, project, activeLayer, scalePriorPxPerMm,
                     Params{});
}

ComponentReanchorResult ComponentReanchor::bootstrap(
    const std::vector<ai::Detection>& detections,
    const ibom::IBomProject& project,
    ibom::Layer activeLayer,
    double scalePriorPxPerMm,
    const Params& params)
{
    ComponentReanchorResult r;

    // Constellation points (component centers, or pads on a bare board) on
    // the active layer, in the VIEW frame (x negated for Layer::Back — the
    // pair→pair similarity hypotheses cannot represent the mirror a back view
    // carries; see viewPoint()). Pads are capped to the 250 largest so the
    // O(nRef × nDet) consensus loop stays bounded.
    const bool mirrored = (activeLayer == ibom::Layer::Back);
    const auto refs = buildConstellation(project, activeLayer,
                                         params.constellation, /*maxPoints=*/250);
    std::vector<cv::Point2f> comp;
    comp.reserve(refs.size());
    for (const auto& rp : refs)
        comp.push_back(viewPoint(rp.raw, mirrored));
    if (comp.size() < static_cast<size_t>(params.minMatches) ||
        detections.size() < static_cast<size_t>(params.minMatches)) {
        r.message = std::string("bootstrap: too few ")
                  + constellationName(params.constellation)
                  + " (" + std::to_string(comp.size()) +
                    ") or detections (" + std::to_string(detections.size()) + ")";
        spdlog::info("[comp-reanchor] {}", r.message);
        return r;
    }

    std::vector<cv::Point2f> det;
    det.reserve(detections.size());
    for (const auto& d : detections)
        det.push_back(detectionCenter(d));

    // Hypotheses from far-apart pairs only: a short baseline makes the angle
    // and scale estimates noise-dominated. Component threshold is a fraction
    // of the layout's own extent so it adapts to any board size.
    float cMinX = comp[0].x, cMaxX = comp[0].x, cMinY = comp[0].y, cMaxY = comp[0].y;
    for (const auto& p : comp) {
        cMinX = std::min(cMinX, p.x); cMaxX = std::max(cMaxX, p.x);
        cMinY = std::min(cMinY, p.y); cMaxY = std::max(cMaxY, p.y);
    }
    const double layoutDiag = std::hypot(cMaxX - cMinX, cMaxY - cMinY);
    if (layoutDiag < 1.0) {
        r.message = "bootstrap: degenerate component layout (span " +
                    std::to_string(layoutDiag) + " mm across " +
                    std::to_string(comp.size()) + " components)";
        spdlog::warn("[comp-reanchor] {}", r.message);
        return r;
    }
    spdlog::debug("[comp-reanchor] bootstrap: {} {} (diag {:.1f} mm), {} detections",
                  comp.size(), constellationName(params.constellation),
                  layoutDiag, det.size());
    const double minCompSep = std::max(2.0, 0.15 * layoutDiag);  // mm
    const double minDetSep  = 30.0;                              // px

    // Scale (px per mm) plausibility window.
    double sMin = 0.05, sMax = 2000.0;
    if (scalePriorPxPerMm > 0.0) {
        sMin = 0.55 * scalePriorPxPerMm;
        sMax = 1.80 * scalePriorPxPerMm;
    }

    // Deterministic RNG: reproducible tests, and no frame-to-frame lottery in
    // the field (same scene → same pose).
    cv::RNG rng(0x5EEDu);

    const int nDet  = static_cast<int>(det.size());
    const int nComp = static_cast<int>(comp.size());

    // ── Scale-compatible pair sampling (ERREUR #58) ──
    // Blind (detection pair, ref pair) sampling wastes nearly every iteration
    // on scale-incompatible hypotheses, and on a repetitive pad lattice it can
    // simply never draw the TRUE pose among the abundant aliases. Index the
    // constellation pairs by separation once; each iteration then draws a
    // detection pair and picks a ref pair whose separation fits the scale
    // window — every hypothesis is plausible by construction. Skipped above
    // 600 refs (pair table too big) → old blind sampling.
    struct CPair { float sep; int a, b; };
    std::vector<CPair> cpairs;
    std::vector<float> cseps;
    const bool indexed = nComp <= 600;
    if (indexed) {
        cpairs.reserve(static_cast<size_t>(nComp) * (nComp - 1) / 2);
        for (int a = 0; a < nComp; ++a)
            for (int b = a + 1; b < nComp; ++b) {
                const float sx = comp[b].x - comp[a].x, sy = comp[b].y - comp[a].y;
                const float sep = std::sqrt(sx * sx + sy * sy);
                if (sep >= minCompSep) cpairs.push_back({ sep, a, b });
            }
        std::sort(cpairs.begin(), cpairs.end(),
                  [](const CPair& x, const CPair& y) { return x.sep < y.sep; });
        cseps.reserve(cpairs.size());
        for (const auto& cp : cpairs) cseps.push_back(cp.sep);
        if (cpairs.empty()) {
            r.message = "bootstrap: no usable constellation pair (all under min separation)";
            spdlog::info("[comp-reanchor] {}", r.message);
            return r;
        }
    }

    int    bestScore = 0, secondScore = 0;
    double bestCosS = 0.0, bestSinS = 0.0, bestTx = 0.0, bestTy = 0.0;
    std::vector<char> used(det.size(), 0);

    // Layout centroid (view frame) — pose comparison anchor. Comparing raw
    // translations is wrong: tx/ty live at the PCB origin, where a tiny
    // rotation difference between two noisy variants of the SAME pose levers
    // a large translation delta (false "distinct pose" → false ambiguity).
    double cx0 = 0.0, cy0 = 0.0;
    for (const auto& p : comp) { cx0 += p.x; cy0 += p.y; }
    cx0 /= nComp; cy0 /= nComp;

    // Two poses are "the same" when their linear parts agree and they map the
    // layout centroid to nearly the same image point — re-finding the winner
    // must not count as a runner-up.
    const auto distinctPoses = [&](double c1, double s1, double x1, double y1,
                                   double c2, double s2, double x2, double y2) {
        const double lin = std::hypot(c1 - c2, s1 - s2)
                         / std::max(1e-9, std::hypot(c2, s2));
        if (lin > 0.08) return true;
        const double tolPx = std::clamp(
            params.bootstrapTolMm * std::hypot(c2, s2), 8.0, 80.0);
        const double u1 = c1 * cx0 - s1 * cy0 + x1, v1 = s1 * cx0 + c1 * cy0 + y1;
        const double u2 = c2 * cx0 - s2 * cy0 + x2, v2 = s2 * cx0 + c2 * cy0 + y2;
        return std::hypot(u1 - u2, v1 - v2) > 3.0 * tolPx;
    };

    // Full greedy consensus: constellation points landing on a still-unclaimed
    // detection within a PHYSICAL tolerance (mm scaled to px by the hypothesis
    // scale) — meaningful at any zoom.
    const auto fullConsensus = [&](double cosS, double sinS, double tx, double ty) {
        const double tol  = std::clamp(
            params.bootstrapTolMm * std::hypot(cosS, sinS), 8.0, 80.0);
        const double tol2 = tol * tol;
        std::fill(used.begin(), used.end(), 0);
        int score = 0;
        for (int c = 0; c < nComp; ++c) {
            const double px = cosS * comp[c].x - sinS * comp[c].y + tx;
            const double py = sinS * comp[c].x + cosS * comp[c].y + ty;
            int bestD = -1;
            double bestD2 = tol2;
            for (int d = 0; d < nDet; ++d) {
                if (used[d]) continue;
                const double dx = det[d].x - px, dy = det[d].y - py;
                const double d2 = dx * dx + dy * dy;
                if (d2 <= bestD2) { bestD2 = d2; bestD = d; }
            }
            if (bestD >= 0) { used[bestD] = 1; ++score; }
        }
        return score;
    };

    // Best/runner-up bookkeeping shared by both search phases. NOTE: no early
    // exit anywhere — the ambiguity margin below needs the runner-up to have
    // actually been visited (ERREUR #58).
    const auto submit = [&](double cosS, double sinS, double tx, double ty,
                            int score) {
        if (score > bestScore) {
            if (bestScore > 0 && distinctPoses(bestCosS, bestSinS, bestTx, bestTy,
                                               cosS, sinS, tx, ty))
                secondScore = std::max(secondScore, bestScore);
            bestScore = score;
            bestCosS = cosS; bestSinS = sinS; bestTx = tx; bestTy = ty;
        } else if (score > secondScore &&
                   distinctPoses(cosS, sinS, tx, ty,
                                 bestCosS, bestSinS, bestTx, bestTy)) {
            secondScore = score;
        }
    };

    struct Hyp { double cosS, sinS, tx, ty; };
    const auto hypFrom = [&](int di, int dj, int ca, int cb) {
        const cv::Point2f dd = det[dj] - det[di];
        const cv::Point2f cc = comp[cb] - comp[ca];
        const double s = std::hypot(static_cast<double>(dd.x), static_cast<double>(dd.y))
                       / std::max(1e-9, std::hypot(static_cast<double>(cc.x),
                                                   static_cast<double>(cc.y)));
        const double theta = std::atan2(static_cast<double>(dd.y), static_cast<double>(dd.x))
                           - std::atan2(static_cast<double>(cc.y), static_cast<double>(cc.x));
        const double cosS = s * std::cos(theta);
        const double sinS = s * std::sin(theta);
        return Hyp{ cosS, sinS,
                    det[di].x - (cosS * comp[ca].x - sinS * comp[ca].y),
                    det[di].y - (sinS * comp[ca].x + cosS * comp[ca].y) };
    };

    // ── Phase 1: deterministic anchor seeding (ERREUR #58) ──
    // Random pair→pair draws almost never hit the ONE correct correspondence
    // among tens of thousands of scale-compatible ref pairs, while a
    // repetitive lattice offers its aliases thousands of supporting pairs —
    // so random search finds aliases by default and the true pose only by
    // luck. Instead: take long-baseline pairs among the LARGEST detections
    // (the detector emits area-descending; large blobs are the most reliable
    // pads), enumerate ALL scale-compatible ref pairs for each, pre-score
    // each hypothesis cheaply against a spatial hash of the detections, and
    // run the full consensus only on the most promising. If the anchor
    // detections are real, the true correspondence is enumerated — found by
    // construction, not by chance.
    if (indexed) {
        // Spatial hash of detections (cell 64 px, 3×3 probe → reliable for
        // pre-score tolerances up to ~64 px).
        std::unordered_map<long long, std::vector<int>> detHash;
        const double kCell = 64.0;
        const auto cellOf = [&](double x, double y) {
            return static_cast<long long>(std::floor(x / kCell) + 100000) * 1000003LL
                 + static_cast<long long>(std::floor(y / kCell) + 100000);
        };
        for (int d = 0; d < nDet; ++d)
            detHash[cellOf(det[d].x, det[d].y)].push_back(d);
        const auto nearDetection = [&](double px, double py, double tol2) {
            const long long bx = static_cast<long long>(std::floor(px / kCell) + 100000);
            const long long by = static_cast<long long>(std::floor(py / kCell) + 100000);
            for (long long oy = -1; oy <= 1; ++oy)
                for (long long ox = -1; ox <= 1; ++ox) {
                    const auto it = detHash.find((bx + ox) * 1000003LL + (by + oy));
                    if (it == detHash.end()) continue;
                    for (const int d : it->second) {
                        const double dx = det[d].x - px, dy = det[d].y - py;
                        if (dx * dx + dy * dy <= tol2) return true;
                    }
                }
            return false;
        };

        // Anchor detection pairs: long baselines among the first (= largest)
        // 40 detections, with DISJOINT endpoints — the globally longest pairs
        // all share the extremal points, and extremal points are where the
        // spurious detections live (image corners). One bad endpoint poisons
        // every anchor it touches; disjointness guarantees that with 12
        // anchors and s spurious endpoints, at least 12−s anchors are clean.
        const int K = std::min(nDet, 40);
        struct DPair { double sep; int i, j; };
        std::vector<DPair> dpairs;
        dpairs.reserve(static_cast<size_t>(K) * (K - 1) / 2);
        for (int i = 0; i < K; ++i)
            for (int j = i + 1; j < K; ++j) {
                const double sep = std::hypot(
                    static_cast<double>(det[j].x - det[i].x),
                    static_cast<double>(det[j].y - det[i].y));
                if (sep >= minDetSep) dpairs.push_back({ sep, i, j });
            }
        std::sort(dpairs.begin(), dpairs.end(),
                  [](const DPair& x, const DPair& y) { return x.sep > y.sep; });
        {
            std::vector<char> endpointUsed(nDet, 0);
            std::vector<DPair> picked;
            for (const auto& dp : dpairs) {
                if (endpointUsed[dp.i] || endpointUsed[dp.j]) continue;
                endpointUsed[dp.i] = endpointUsed[dp.j] = 1;
                picked.push_back(dp);
                if (picked.size() >= 12) break;
            }
            dpairs = std::move(picked);
        }

        // Pre-score refs: ~60 points in STRIDE over the whole constellation —
        // not the first N. On a repetitive layout the first N (one corner of
        // a pad grid) saturate identically under every lattice alias; the
        // discriminating pads (connectors, odd footprints) live elsewhere in
        // the list and are exactly what tells the true pose from an alias.
        std::vector<int> preIdx;
        {
            const int stride = std::max(1, nComp / 60);
            for (int c = 0; c < nComp; c += stride) preIdx.push_back(c);
        }
        const int nPre = static_cast<int>(preIdx.size());
        struct Seed { int pre; Hyp h; };
        std::vector<Seed> seeds;

        for (const auto& ap : dpairs) {
            const float lo = static_cast<float>(ap.sep / sMax);
            const float hi = static_cast<float>(ap.sep / sMin);
            const auto itLo = std::lower_bound(cseps.begin(), cseps.end(), lo);
            const auto itHi = std::upper_bound(cseps.begin(), cseps.end(), hi);
            const int first = static_cast<int>(itLo - cseps.begin());
            const int last  = static_cast<int>(itHi - cseps.begin());
            // Very large windows (weak/no scale prior) are stride-sampled so
            // one anchor cannot eat the whole budget.
            const int span = last - first;
            const int step = std::max(1, span / 20000);
            for (int p = first; p < last; p += step) {
                for (int orient = 0; orient < 2; ++orient) {
                    const int ca = orient ? cpairs[p].b : cpairs[p].a;
                    const int cb = orient ? cpairs[p].a : cpairs[p].b;
                    const Hyp h = hypFrom(ap.i, ap.j, ca, cb);
                    const double tol = std::min(64.0, std::clamp(
                        params.bootstrapTolMm * std::hypot(h.cosS, h.sinS), 8.0, 80.0));
                    const double tol2 = tol * tol;
                    int pre = 0;
                    for (const int c : preIdx) {
                        const double px = h.cosS * comp[c].x - h.sinS * comp[c].y + h.tx;
                        const double py = h.sinS * comp[c].x + h.cosS * comp[c].y + h.ty;
                        if (nearDetection(px, py, tol2)) ++pre;
                    }
                    if (pre * 2 >= nPre)  // under half the spread refs → noise
                        seeds.push_back({ pre, h });
                }
            }
        }
        // Full consensus on the most promising seeds. The cap is generous on
        // purpose: on a repetitive layout many aliases tie at high pre-scores
        // and a tight cap would drop the true pose on an arbitrary tie-break.
        std::sort(seeds.begin(), seeds.end(),
                  [](const Seed& x, const Seed& y) { return x.pre > y.pre; });
        if (seeds.size() > 256) seeds.resize(256);
        for (const auto& sd : seeds)
            submit(sd.h.cosS, sd.h.sinS, sd.h.tx, sd.h.ty,
                   fullConsensus(sd.h.cosS, sd.h.sinS, sd.h.tx, sd.h.ty));
    }

    // ── Phase 2: randomized pair→pair search (coverage backstop) ──
    // Catches what the anchors miss (all-anchor-junk frames, exotic poses);
    // scale-compatible draws when indexed, the historic blind draws otherwise.
    for (int it = 0; it < params.bootstrapIterations; ++it) {
        const int di = rng.uniform(0, nDet), dj = rng.uniform(0, nDet);
        if (di == dj) continue;
        const cv::Point2f dd = det[dj] - det[di];
        const double ddN = std::hypot(static_cast<double>(dd.x), static_cast<double>(dd.y));
        if (ddN < minDetSep) continue;

        int ca, cb;
        if (indexed) {
            const float lo = static_cast<float>(ddN / sMax);
            const float hi = static_cast<float>(ddN / sMin);
            const auto itLo = std::lower_bound(cseps.begin(), cseps.end(), lo);
            const auto itHi = std::upper_bound(cseps.begin(), cseps.end(), hi);
            if (itLo >= itHi) continue;
            const int pick = rng.uniform(static_cast<int>(itLo - cseps.begin()),
                                         static_cast<int>(itHi - cseps.begin()));
            ca = cpairs[pick].a;
            cb = cpairs[pick].b;
            if (rng.uniform(0, 2)) std::swap(ca, cb);  // both orientations
        } else {
            ca = rng.uniform(0, nComp);
            cb = rng.uniform(0, nComp);
            if (ca == cb) continue;
        }

        const cv::Point2f cc = comp[cb] - comp[ca];
        const double ccN = std::hypot(static_cast<double>(cc.x), static_cast<double>(cc.y));
        if (ccN < minCompSep) continue;
        const double s = ddN / ccN;
        if (s < sMin || s > sMax) continue;

        const Hyp h = hypFrom(di, dj, ca, cb);
        submit(h.cosS, h.sinS, h.tx, h.ty,
               fullConsensus(h.cosS, h.sinS, h.tx, h.ty));
    }

    const int need = std::max(params.minMatches,
                              static_cast<int>(std::ceil(0.25 * std::min(nDet, nComp))));
    if (bestScore < need) {
        r.matches = bestScore;
        r.message = "bootstrap: best consensus " + std::to_string(bestScore) +
                    " < " + std::to_string(need) + " required (" +
                    std::to_string(nComp) + " comps, " + std::to_string(nDet) + " dets)";
        spdlog::info("[comp-reanchor] {}", r.message);
        return r;
    }

    // Ambiguity margin (ERREUR #58): when a DISTINCT pose reaches nearly the
    // same consensus, the layout aliases (mirrored/rotated pad lattice) and
    // picking between the two is a coin flip — refuse and say so. A wrong
    // silent lock costs far more than an honest failure.
    if (params.bootstrapAmbiguityRatio > 0.0 &&
        secondScore >= params.bootstrapAmbiguityRatio * bestScore) {
        r.matches = bestScore;
        r.message = "bootstrap: ambiguous registration — two distinct poses reach "
                    + std::to_string(bestScore) + " vs " + std::to_string(secondScore) +
                    " consensus (repetitive layout); refusing to guess";
        spdlog::warn("[comp-reanchor] {}", r.message);
        return r;
    }

    // Hand the winning similarity to the prior-based path: it does the exact
    // greedy matching, the RANSAC homography fit and the inlier/reprojection
    // validation. Widen its gating radius to the bootstrap tolerance so the
    // coarse prior doesn't starve it.
    cv::Mat S = (cv::Mat_<double>(3, 3) <<
        bestCosS, -bestSinS, bestTx,
        bestSinS,  bestCosS, bestTy,
        0.0,       0.0,      1.0);
    // S maps view-frame → image; estimate() expects a RAW-PCB → image prior
    // (it re-derives the view frame internally for Back).
    Homography prior;
    prior.setMatrix(mirrored ? cv::Mat(S * mirrorX()) : S);

    Params refine = params;
    const double tolPx = std::clamp(params.bootstrapTolMm * std::hypot(bestCosS, bestSinS),
                                    8.0, 80.0);
    refine.maxMatchDistPx = std::max(params.maxMatchDistPx, 1.5 * tolPx);
    // The px widening above already encodes the right tolerance for a coarse
    // bootstrap prior — don't let a caller-set physical gate re-shrink it.
    refine.matchGateMm = 0.0;

    r = estimate(detections, project, prior, activeLayer, {}, refine);
    r.message = "bootstrap(" + std::to_string(bestScore) + " consensus): " + r.message;
    if (r.found)
        spdlog::info("[comp-reanchor] {}", r.message);
    return r;
}

ComponentReanchorResult ComponentReanchor::estimateOrientations(
    const std::vector<ai::Detection>& detections,
    const ibom::IBomProject& project,
    const std::vector<cv::Point2f>& imageCorners,
    ibom::Layer activeLayer,
    const Params& params)
{
    ComponentReanchorResult best;
    if (imageCorners.size() != 4) {
        best.message = "orientation vote: need 4 board corners";
        return best;
    }
    const auto& bb = project.boardInfo.boardBBox;
    const std::vector<cv::Point2f> pcb = {
        { static_cast<float>(bb.minX), static_cast<float>(bb.minY) },
        { static_cast<float>(bb.maxX), static_cast<float>(bb.minY) },
        { static_cast<float>(bb.maxX), static_cast<float>(bb.maxY) },
        { static_cast<float>(bb.minX), static_cast<float>(bb.maxY) }
    };
    const auto ratioOf = [](const ComponentReanchorResult& r) {
        return r.matches > 0 ? static_cast<double>(r.inliers) / r.matches : 0.0;
    };
    int bestRot = -1;
    for (int k = 0; k < 4; ++k) {
        // Rotate the corner ASSIGNMENT (which pcb corner maps to which image
        // corner) — the quad itself is fixed by the outline. On a non-square
        // board the two 90° assignments produce a heavily skewed prior whose
        // estimate simply fails to lock; harmless.
        std::vector<cv::Point2f> img(4);
        for (int i = 0; i < 4; ++i) img[i] = imageCorners[(i + k) % 4];
        Homography prior;
        prior.setMatrix(cv::getPerspectiveTransform(pcb, img));
        auto r = estimate(detections, project, prior, activeLayer, {}, params);
        if (r.found && (!best.found || ratioOf(r) > ratioOf(best))) {
            best = std::move(r);
            bestRot = k;
        }
    }
    if (best.found) {
        best.message = "orientation vote (rot " + std::to_string(bestRot * 90) +
                       "°): " + best.message;
        spdlog::info("[comp-reanchor] {}", best.message);
    } else {
        best.message = "orientation vote: no rotation locked on the "
                       + std::string(constellationName(params.constellation));
        spdlog::info("[comp-reanchor] {}", best.message);
    }
    return best;
}

} // namespace ibom::overlay
