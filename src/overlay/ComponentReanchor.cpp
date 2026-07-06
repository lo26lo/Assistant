#include "ComponentReanchor.h"

#include <opencv2/calib3d.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

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

    // Predicted image position of every candidate component on the active layer.
    struct Cand {
        size_t compIdx;
        cv::Point2f pcb;
        cv::Point2f predImg;
        int cls;
    };
    std::vector<Cand> cands;
    cands.reserve(project.components.size());
    for (size_t i = 0; i < project.components.size(); ++i) {
        const auto& c = project.components[i];
        if (c.layer != activeLayer) continue;
        const cv::Point2f pcb = componentCenter(c);
        cands.push_back({ i, pcb, currentPose.pcbToImage(pcb),
                          useClass ? classOfComponent[i] : -1 });
    }
    if (cands.size() < static_cast<size_t>(params.minMatches)) {
        r.message = "too few components on active layer to re-anchor";
        return r;
    }

    // Candidate (detection, component) pairs within the gating radius, sorted
    // by distance, then assigned greedily so each is used at most once.
    struct Pair { double dist; size_t det; size_t cand; };
    std::vector<Pair> pairs;
    const double maxD2 = params.maxMatchDistPx * params.maxMatchDistPx;
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
    if (r.medianReprojPx > params.maxMedianReprojPx) {
        r.message = "median reprojection error too high (" +
                    std::to_string(r.medianReprojPx) + "px)";
        return r;
    }

    r.found = true;
    r.homography = H;
    r.message = "re-anchored on " + std::to_string(r.inliers) + "/" +
                std::to_string(r.matches) + " components, median " +
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

    // Component centers on the active layer (PCB mm).
    std::vector<cv::Point2f> comp;
    comp.reserve(project.components.size());
    for (const auto& c : project.components) {
        if (c.layer != activeLayer) continue;
        comp.push_back(componentCenter(c));
    }
    if (comp.size() < static_cast<size_t>(params.minMatches) ||
        detections.size() < static_cast<size_t>(params.minMatches)) {
        r.message = "bootstrap: too few components (" + std::to_string(comp.size()) +
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
    spdlog::debug("[comp-reanchor] bootstrap: {} components (diag {:.1f} mm), {} detections",
                  comp.size(), layoutDiag, det.size());
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
    int    bestScore = 0;
    double bestCosS = 0.0, bestSinS = 0.0, bestTx = 0.0, bestTy = 0.0;
    std::vector<char> used(det.size(), 0);

    for (int it = 0; it < params.bootstrapIterations; ++it) {
        const int di = rng.uniform(0, nDet), dj = rng.uniform(0, nDet);
        const int ca = rng.uniform(0, nComp), cb = rng.uniform(0, nComp);
        if (di == dj || ca == cb) continue;

        const cv::Point2f dd = det[dj] - det[di];
        const double ddN = std::hypot(static_cast<double>(dd.x), static_cast<double>(dd.y));
        if (ddN < minDetSep) continue;
        const cv::Point2f cc = comp[cb] - comp[ca];
        const double ccN = std::hypot(static_cast<double>(cc.x), static_cast<double>(cc.y));
        if (ccN < minCompSep) continue;

        const double s = ddN / ccN;
        if (s < sMin || s > sMax) continue;

        // Similarity fully determined by the pair→pair correspondence.
        const double theta = std::atan2(static_cast<double>(dd.y), static_cast<double>(dd.x))
                           - std::atan2(static_cast<double>(cc.y), static_cast<double>(cc.x));
        const double cosS = s * std::cos(theta);
        const double sinS = s * std::sin(theta);
        const double tx = det[di].x - (cosS * comp[ca].x - sinS * comp[ca].y);
        const double ty = det[di].y - (sinS * comp[ca].x + cosS * comp[ca].y);

        // Consensus: components landing on a still-unclaimed detection within
        // a PHYSICAL tolerance (mm scaled to px) — meaningful at any zoom.
        const double tol  = std::clamp(params.bootstrapTolMm * s, 8.0, 80.0);
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

        if (score > bestScore) {
            bestScore = score;
            bestCosS = cosS; bestSinS = sinS; bestTx = tx; bestTy = ty;
            // Strong consensus → stop early; more iterations only cost time.
            if (bestScore >= 2 * params.minMatches &&
                bestScore >= static_cast<int>(0.6 * std::min(nDet, nComp)))
                break;
        }
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

    // Hand the winning similarity to the prior-based path: it does the exact
    // greedy matching, the RANSAC homography fit and the inlier/reprojection
    // validation. Widen its gating radius to the bootstrap tolerance so the
    // coarse prior doesn't starve it.
    cv::Mat S = (cv::Mat_<double>(3, 3) <<
        bestCosS, -bestSinS, bestTx,
        bestSinS,  bestCosS, bestTy,
        0.0,       0.0,      1.0);
    Homography prior;
    prior.setMatrix(S);

    Params refine = params;
    const double tolPx = std::clamp(params.bootstrapTolMm * std::hypot(bestCosS, bestSinS),
                                    8.0, 80.0);
    refine.maxMatchDistPx = std::max(params.maxMatchDistPx, 1.5 * tolPx);

    r = estimate(detections, project, prior, activeLayer, {}, refine);
    r.message = "bootstrap(" + std::to_string(bestScore) + " consensus): " + r.message;
    if (r.found)
        spdlog::info("[comp-reanchor] {}", r.message);
    return r;
}

} // namespace ibom::overlay
