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

} // namespace

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
        const cv::Point2f pcb(static_cast<float>(c.position.x),
                              static_cast<float>(c.position.y));
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
    cv::Mat H = cv::findHomography(pcbPts, imgPts, cv::RANSAC,
                                   params.ransacThreshPx, mask);
    if (H.empty()) {
        r.message = "findHomography failed";
        return r;
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

} // namespace ibom::overlay
