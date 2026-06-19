#include "BoardLocator.h"
#include "Homography.h"

#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ibom::overlay {

namespace {

// Reference distance band (mm) around the center-ROI median depth that
// counts as "the board's plane". 15mm tolerates board thickness, tall
// components and sensor noise without merging into the table behind it.
constexpr double kDepthBandMm = 15.0;

// Candidate quad area must be within [1/kAreaTolerance, kAreaTolerance] of
// the iBOM-implied area to be accepted — generous because the user may not
// have the board perfectly centered/leveled.
constexpr double kAreaTolerance = 2.5;

// Candidate quad aspect ratio (long/short side) must be within this factor
// of the iBOM-implied aspect ratio.
constexpr double kAspectTolerance = 1.6;

// Minimum edge-agreement score (see scoreOrientation()) for a disambiguated
// orientation to be reported as found=true. Below this, every one of the 8
// candidates scored too low to trust (e.g. an iBOM with no boardOutline and
// no active-layer components — predictedPixels stays 0 and every candidate
// scores exactly 0.0, which would otherwise win "by default" since the
// search starts from bestScore = -1.0).
constexpr double kMinAcceptableScore = 0.10;

// Above this orientation score, the depth result is trusted outright and the
// (slower) contour method is skipped. Below it, BOTH methods are run and the
// higher-scoring one wins — this is what lets a high-contrast background
// (e.g. a white sheet under a green board) actually pay off: the depth method
// is colour-blind and, on a board lying coplanar with the table, merges board
// + surface into one oversized/offset quad that scores poorly; the contour
// method keys off luminance edges and can then beat it. See ERREUR #41/#44.
constexpr double kStrongScore = 0.30;

// Minimum fraction of valid (non-zero) pixels required in a depth frame
// before trusting plane segmentation on it — below this, specular
// reflections/glare have likely wiped out too much of the signal (see
// locateViaDepth()).
constexpr double kMinDepthFillRatio = 0.20;

} // namespace

cv::Mat BoardLocator::computeEdgeMap(const cv::Mat& colorBgr)
{
    cv::Mat gray;
    if (colorBgr.channels() == 3) {
        cv::cvtColor(colorBgr, gray, cv::COLOR_BGR2GRAY);
    } else if (colorBgr.channels() == 1) {
        gray = colorBgr;
    } else {
        cv::cvtColor(colorBgr, gray, cv::COLOR_BGRA2GRAY);
    }

    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);

    cv::Mat edges;
    cv::Canny(blurred, edges, 50, 150);

    // Dilate so a predicted edge landing a few px from a real one still
    // counts as agreement — the candidate quad is an approximation.
    cv::dilate(edges, edges, cv::Mat(), cv::Point(-1, -1), 2);
    return edges;
}

bool BoardLocator::validateSize(const cv::RotatedRect& rect,
                                 double expectedAreaPx,
                                 double expectedAspect,
                                 std::string& reason)
{
    const double area = static_cast<double>(rect.size.width) * rect.size.height;

    if (expectedAreaPx > 0.0) {
        const double ratio = area / expectedAreaPx;
        if (ratio > kAreaTolerance) {
            // Too BIG — the usual cause on a RealSense depth plane is the board
            // lying flush on a coplanar surface (table/mat): the ±band grabs the
            // board plus a margin of background at the same distance and fits one
            // rectangle around the merged blob. Loosening the tolerance would just
            // accept that oversized quad and misplace the overlay — the real fix is
            // to break the coplanarity (lift the board) or use manual alignment.
            reason = "detected region (" + std::to_string(static_cast<int>(area)) +
                      " px^2) is larger than the board at the known scale (" +
                      std::to_string(static_cast<int>(expectedAreaPx)) +
                      " px^2) — it is probably merged with a coplanar background. "
                      "Lift the board off the table/surface so it stands out in depth, "
                      "or use manual alignment";
            return false;
        }
        if (ratio < 1.0 / kAreaTolerance) {
            reason = "detected region (" + std::to_string(static_cast<int>(area)) +
                      " px^2) is smaller than the board at the known scale (" +
                      std::to_string(static_cast<int>(expectedAreaPx)) +
                      " px^2) — make sure the whole board is in frame";
            return false;
        }
    }

    if (expectedAspect > 0.0 && rect.size.width > 0.0f && rect.size.height > 0.0f) {
        double a = static_cast<double>(rect.size.width) / rect.size.height;
        if (a < 1.0) a = 1.0 / a;
        double expectedA = expectedAspect < 1.0 ? 1.0 / expectedAspect : expectedAspect;
        if (a / expectedA > kAspectTolerance || expectedA / a > kAspectTolerance) {
            reason = "candidate quad aspect ratio doesn't match the board outline";
            return false;
        }
    }

    reason.clear();
    return true;
}

bool BoardLocator::locateViaDepth(const cv::Mat& depth16u,
                                   double expectedAreaPx,
                                   double expectedAspect,
                                   cv::RotatedRect& outRect,
                                   std::string& reason)
{
    if (depth16u.empty() || depth16u.type() != CV_16UC1) {
        reason = "no depth frame available";
        return false;
    }

    // Bail out early if most of the depth frame is invalid (0) — typically
    // caused by specular reflections off a glossy/shiny board surface or
    // direct glare confusing the D405's IR stereo matching. Running the
    // plane-segmentation below on mostly-noise depth produces a tiny,
    // wrong-shaped "plane" (e.g. just the glare spot) rather than the real
    // board, and also poisons the median-distance estimate used elsewhere
    // (StatsPanel "Distance", Auto-Align's pinhole px/mm). Fail clearly
    // instead of silently locating garbage.
    const double fillRatio = static_cast<double>(cv::countNonZero(depth16u))
                              / (static_cast<double>(depth16u.rows) * depth16u.cols);
    if (fillRatio < kMinDepthFillRatio) {
        reason = "depth data too sparse (" + std::to_string(static_cast<int>(fillRatio * 100)) +
                  "% valid) — likely glare/reflection off the board surface; reduce lighting "
                  "glare or try the contour method";
        return false;
    }

    // Reference distance: median of the central 20% ROI — same heuristic
    // Application.cpp uses for the live "Distance" stat. Assumes the user
    // points the camera roughly at the board before triggering Auto-Align.
    const int rw = std::max(1, depth16u.cols / 5);
    const int rh = std::max(1, depth16u.rows / 5);
    const cv::Rect roi((depth16u.cols - rw) / 2, (depth16u.rows - rh) / 2, rw, rh);

    std::vector<uint16_t> vals;
    vals.reserve(static_cast<size_t>(rw) * rh);
    for (int y = roi.y; y < roi.y + roi.height; ++y) {
        const auto* row = depth16u.ptr<uint16_t>(y);
        for (int x = roi.x; x < roi.x + roi.width; ++x)
            if (row[x] > 0) vals.push_back(row[x]);
    }
    if (vals.size() < 16) {
        reason = "too few valid depth samples in the center of frame";
        return false;
    }
    std::nth_element(vals.begin(), vals.begin() + vals.size() / 2, vals.end());
    const double refMm = vals[vals.size() / 2];

    cv::Mat mask = cv::Mat::zeros(depth16u.size(), CV_8UC1);
    for (int y = 0; y < depth16u.rows; ++y) {
        const auto* row = depth16u.ptr<uint16_t>(y);
        auto* mrow = mask.ptr<uchar>(y);
        for (int x = 0; x < depth16u.cols; ++x) {
            if (row[x] > 0 && std::abs(static_cast<double>(row[x]) - refMm) <= kDepthBandMm)
                mrow[x] = 255;
        }
    }

    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, cv::Mat(), cv::Point(-1, -1), 3);
    cv::morphologyEx(mask, mask, cv::MORPH_OPEN, cv::Mat(), cv::Point(-1, -1), 2);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty()) {
        reason = "no coherent plane found near the camera's center distance";
        return false;
    }

    const auto biggest = std::max_element(contours.begin(), contours.end(),
        [](const std::vector<cv::Point>& a, const std::vector<cv::Point>& b) {
            return cv::contourArea(a) < cv::contourArea(b);
        });

    const cv::RotatedRect rect = cv::minAreaRect(*biggest);
    if (!validateSize(rect, expectedAreaPx, expectedAspect, reason)) return false;

    outRect = rect;
    return true;
}

bool BoardLocator::locateViaContour(const cv::Mat& colorBgr,
                                     double expectedAreaPx,
                                     double expectedAspect,
                                     cv::RotatedRect& outRect,
                                     std::string& reason)
{
    cv::Mat gray;
    cv::cvtColor(colorBgr, gray, cv::COLOR_BGR2GRAY);
    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);

    cv::Mat edges;
    cv::Canny(blurred, edges, 40, 120);
    cv::dilate(edges, edges, cv::Mat(), cv::Point(-1, -1), 2);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(edges, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty()) {
        reason = "no contours found in the frame";
        return false;
    }

    const double frameArea = static_cast<double>(colorBgr.cols) * colorBgr.rows;
    bool any = false;
    double bestDelta = std::numeric_limits<double>::max();
    cv::RotatedRect best;

    for (const auto& c : contours) {
        const double area = cv::contourArea(c);
        if (area < frameArea * 0.02) continue; // ignore small noise/clutter

        const cv::RotatedRect rect = cv::minAreaRect(c);
        const double rectArea = static_cast<double>(rect.size.width) * rect.size.height;
        if (rectArea <= 0.0) continue;

        // Quad-likeness: a clean rectangular board outline fills most of its
        // own rotated bounding box; jagged clutter/silkscreen text doesn't.
        if (area / rectArea < 0.55) continue;

        std::string ignored;
        if ((expectedAreaPx > 0.0 || expectedAspect > 0.0) &&
            !validateSize(rect, expectedAreaPx, expectedAspect, ignored)) {
            continue;
        }

        const double delta = expectedAreaPx > 0.0 ? std::abs(rectArea - expectedAreaPx) : -area;
        if (delta < bestDelta) {
            bestDelta = delta;
            best = rect;
            any = true;
        }
    }

    if (!any) {
        reason = "no board-shaped quad found in the frame — try the depth-based "
                  "method (RealSense) or a less cluttered background, or fall "
                  "back to manual alignment";
        return false;
    }

    outRect = best;
    return true;
}

double BoardLocator::scoreOrientation(const cv::Mat& dilatedEdges,
                                       const ibom::IBomProject& project,
                                       const std::vector<cv::Point2f>& pcbCorners,
                                       const std::vector<cv::Point2f>& imgCorners,
                                       ibom::Layer activeLayer)
{
    Homography h;
    if (!h.compute(pcbCorners, imgCorners)) return 0.0;

    cv::Mat predicted = cv::Mat::zeros(dilatedEdges.size(), CV_8UC1);

    for (const auto& seg : project.boardOutline) {
        if (seg.type != DrawingSegment::Type::Line) continue;
        const cv::Point2f p1 = h.pcbToImage(cv::Point2f(static_cast<float>(seg.start.x),
                                                          static_cast<float>(seg.start.y)));
        const cv::Point2f p2 = h.pcbToImage(cv::Point2f(static_cast<float>(seg.end.x),
                                                          static_cast<float>(seg.end.y)));
        cv::line(predicted, p1, p2, cv::Scalar(255), 2, cv::LINE_AA);
    }

    for (const auto& comp : project.components) {
        // Only render the side of the board the camera is actually looking
        // at — mirrors OverlayRenderer's own active-layer filter. Mixing in
        // the opposite layer's (mirrored) footprints would dilute the score
        // of the correct orientation on a double-sided board.
        if (comp.layer != activeLayer) continue;
        const auto corners = h.transformRect(static_cast<float>(comp.bbox.minX),
                                              static_cast<float>(comp.bbox.minY),
                                              static_cast<float>(comp.bbox.width()),
                                              static_cast<float>(comp.bbox.height()));
        if (corners.size() != 4) continue;
        std::vector<cv::Point> poly;
        poly.reserve(4);
        for (const auto& p : corners) poly.emplace_back(static_cast<int>(p.x), static_cast<int>(p.y));
        cv::polylines(predicted, poly, true, cv::Scalar(255), 1, cv::LINE_AA);
    }

    const int predictedPixels = cv::countNonZero(predicted);
    if (predictedPixels == 0) return 0.0;

    cv::Mat overlap;
    cv::bitwise_and(predicted, dilatedEdges, overlap);
    return static_cast<double>(cv::countNonZero(overlap)) / predictedPixels;
}

BoardLocateResult BoardLocator::disambiguate(const cv::RotatedRect& rect,
                                              const cv::Mat& colorBgr,
                                              const ibom::IBomProject& project,
                                              const std::string& method,
                                              ibom::Layer activeLayer)
{
    BoardLocateResult result;
    result.method = method;

    cv::Point2f pts[4];
    rect.points(pts);

    const auto& bbox = project.boardInfo.boardBBox;
    // Clockwise PCB corners (Y-down), matching the convention used by the
    // manual 4-corner/anchor alignment paths in Application.cpp: TL, TR, BR, BL.
    const std::vector<cv::Point2f> pcbCorners = {
        {static_cast<float>(bbox.minX), static_cast<float>(bbox.minY)},
        {static_cast<float>(bbox.maxX), static_cast<float>(bbox.minY)},
        {static_cast<float>(bbox.maxX), static_cast<float>(bbox.maxY)},
        {static_cast<float>(bbox.minX), static_cast<float>(bbox.maxY)}
    };

    const cv::Mat edges = computeEdgeMap(colorBgr);

    double bestScore = -1.0;
    std::vector<cv::Point2f> bestImgCorners;

    // The quad's winding direction isn't known a priori (depends on contour
    // tracing direction / minAreaRect internals), so try both windings, each
    // with all 4 cyclic rotations — 8 candidates covering every way the
    // physical board could be rotated relative to the iBOM layout.
    for (int winding = 0; winding < 2; ++winding) {
        std::vector<cv::Point2f> base(pts, pts + 4);
        if (winding == 1) std::reverse(base.begin(), base.end());

        for (int shift = 0; shift < 4; ++shift) {
            std::vector<cv::Point2f> candidate(4);
            for (int i = 0; i < 4; ++i) candidate[i] = base[(i + shift) % 4];

            const double score = scoreOrientation(edges, project, pcbCorners, candidate, activeLayer);
            if (score > bestScore) {
                bestScore = score;
                bestImgCorners = candidate;
            }
        }
    }

    result.found = !bestImgCorners.empty() && bestScore >= kMinAcceptableScore;
    result.imageCorners = bestImgCorners;
    result.score = std::max(0.0, bestScore);

    char buf[160];
    if (result.found) {
        std::snprintf(buf, sizeof(buf), "Board located via %s, edge-agreement score %.2f",
                      method.c_str(), result.score);
    } else if (bestImgCorners.empty()) {
        std::snprintf(buf, sizeof(buf), "Board outline found via %s but orientation scoring failed",
                      method.c_str());
    } else {
        std::snprintf(buf, sizeof(buf),
                      "Board outline found via %s but best orientation score %.2f is below threshold %.2f",
                      method.c_str(), bestScore, kMinAcceptableScore);
    }
    result.message = buf;
    return result;
}

BoardLocateResult BoardLocator::locate(const cv::Mat& colorBgr,
                                        const cv::Mat& depth16u,
                                        const ibom::IBomProject& project,
                                        double expectedPixelsPerMm,
                                        ibom::Layer activeLayer)
{
    BoardLocateResult result;

    if (colorBgr.empty()) {
        result.message = "no camera frame available";
        return result;
    }

    const auto& bbox = project.boardInfo.boardBBox;
    const double boardWMm = bbox.width();
    const double boardHMm = bbox.height();
    if (boardWMm <= 0.0 || boardHMm <= 0.0) {
        result.message = "iBOM board outline has no usable bounding box";
        return result;
    }

    double expectedAreaPx = 0.0;
    const double expectedAspect = boardWMm / boardHMm;
    if (expectedPixelsPerMm > 0.0) {
        expectedAreaPx = (boardWMm * expectedPixelsPerMm) * (boardHMm * expectedPixelsPerMm);
    }

    // Try depth first. Disambiguate it to get a real orientation score.
    cv::RotatedRect depthRect;
    std::string depthReason;
    const bool depthOk =
        locateViaDepth(depth16u, expectedAreaPx, expectedAspect, depthRect, depthReason);
    if (depthOk)
        result = disambiguate(depthRect, colorBgr, project, "depth", activeLayer);

    // If depth is unavailable or only weakly agrees with the layout, also run
    // the contour method and keep whichever scores higher. The contour method
    // exploits luminance contrast (a white sheet under the board), which the
    // depth method cannot see — so it can rescue a coplanar-board case where
    // depth merged the board with the surface (see kStrongScore / ERREUR #44).
    if (!result.found || result.score < kStrongScore) {
        cv::RotatedRect contourRect;
        std::string contourReason;
        const bool contourOk =
            locateViaContour(colorBgr, expectedAreaPx, expectedAspect, contourRect, contourReason);
        if (contourOk) {
            BoardLocateResult contourRes =
                disambiguate(contourRect, colorBgr, project, "contour", activeLayer);
            if (contourRes.found && contourRes.score > result.score)
                result = contourRes;
        }
        // Nothing usable from either path — compose a combined diagnostic.
        if (!result.found) {
            const std::string dr = depthOk ? "orientation score too low" : depthReason;
            const std::string cr = contourOk ? "orientation score too low" : contourReason;
            result.message = "Depth: " + dr + ". Contour: " + cr;
        }
    }

    if (result.found)
        spdlog::info("BoardLocator: {}", result.message);
    else
        spdlog::warn("BoardLocator: {}", result.message);
    return result;
}

} // namespace ibom::overlay
