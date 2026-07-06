#include "BlobComponentDetector.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/features2d.hpp>  // cv::MSER
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace ibom::overlay {

std::vector<ai::Detection> detectComponentBlobs(const cv::Mat& image,
                                                double scalePxPerMm,
                                                int maxDetections)
{
    std::vector<ai::Detection> out;
    if (image.empty()) return out;

    cv::Mat gray;
    if (image.channels() == 3)      cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    else if (image.channels() == 4) cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
    else                            gray = image;

    // Local contrast boost so component bodies pop from the substrate under
    // uneven lighting (D405 glare) before MSER.
    try {
        cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
        clahe->apply(gray, gray);
    } catch (const cv::Exception&) { /* keep raw gray */ }

    // Size gate from the scale prior. Component bodies span ~0.4 mm (0201) to
    // ~22 mm (big ICs / connectors). No scale → a permissive pixel range.
    double minSide, maxSide;
    if (scalePxPerMm > 0.0) {
        minSide = std::max(2.0, 0.4 * scalePxPerMm);
        maxSide = 22.0 * scalePxPerMm;
    } else {
        minSide = 4.0;
        maxSide = 0.5 * std::min(gray.cols, gray.rows);
    }
    const double minArea = minSide * minSide;
    const double maxArea = maxSide * maxSide;

    // MSER: maximally-stable regions catch component bodies regardless of
    // polarity (dark IC on light board, or shiny part on dark mask).
    std::vector<std::vector<cv::Point>> regions;
    std::vector<cv::Rect> boxes;
    try {
        cv::Ptr<cv::MSER> mser = cv::MSER::create(
            /*delta*/ 5,
            /*min_area*/ std::max(10, static_cast<int>(minArea)),
            /*max_area*/ std::max(static_cast<int>(minArea) + 1, static_cast<int>(maxArea)));
        mser->detectRegions(gray, regions, boxes);
    } catch (const cv::Exception& e) {
        spdlog::warn("[blob-detect] MSER failed: {}", e.what());
        return out;
    }

    // Filter by side length + aspect (drop silkscreen lines, traces, long
    // connector slots read as one region).
    std::vector<cv::Rect> kept;
    kept.reserve(boxes.size());
    for (const auto& b : boxes) {
        const double w = b.width, h = b.height;
        if (w < minSide || h < minSide) continue;
        if (w > maxSide && h > maxSide) continue;
        const double aspect = std::max(w, h) / std::max(1.0, std::min(w, h));
        if (aspect > 8.0) continue;
        kept.push_back(b);
    }

    // MSER nests regions → dedup by center proximity so one component yields
    // one detection (min pitch ≈ the smallest accepted side).
    const double mergeR  = minSide * 0.8;
    const double mergeR2 = mergeR * mergeR;
    std::vector<cv::Rect> dedup;
    dedup.reserve(kept.size());
    for (const auto& b : kept) {
        const cv::Point2f c(b.x + b.width * 0.5f, b.y + b.height * 0.5f);
        bool dup = false;
        for (const auto& d : dedup) {
            const cv::Point2f dc(d.x + d.width * 0.5f, d.y + d.height * 0.5f);
            const double dx = c.x - dc.x, dy = c.y - dc.y;
            if (dx * dx + dy * dy < mergeR2) { dup = true; break; }
        }
        if (!dup) dedup.push_back(b);
    }

    // Cap to the strongest (largest) N — bootstrap tolerates false positives
    // but its consensus loop is O(nComp × nDet) per RANSAC iteration.
    if (static_cast<int>(dedup.size()) > maxDetections) {
        std::nth_element(dedup.begin(), dedup.begin() + maxDetections, dedup.end(),
                         [](const cv::Rect& a, const cv::Rect& b) { return a.area() > b.area(); });
        dedup.resize(maxDetections);
    }

    out.reserve(dedup.size());
    for (const auto& b : dedup) {
        ai::Detection d;
        d.classId    = 0;
        d.className  = "component";
        d.confidence = 1.0f;
        d.bbox       = cv::Rect2f(static_cast<float>(b.x), static_cast<float>(b.y),
                                  static_cast<float>(b.width), static_cast<float>(b.height));
        out.push_back(std::move(d));
    }
    spdlog::debug("[blob-detect] {} component-like blobs (scale={:.2f} px/mm)",
                  out.size(), scalePxPerMm);
    return out;
}

} // namespace ibom::overlay
