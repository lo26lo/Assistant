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
    // connector slots read as one region). Keep the pixel-mass centroid of
    // each region, not the bbox center: the bbox is set by the region's
    // extremal pixels (a shadow lobe, a glare streak, attached silk), so its
    // center carries a per-component bias that changes with the matched
    // subset frame to frame — a measured driver of the re-anchor pose jitter
    // (docs/BLOB_REANCHOR_JITTER_ANALYSE.md, cause n°2).
    struct Blob { cv::Rect box; cv::Point2f centroid; };
    std::vector<Blob> kept;
    kept.reserve(boxes.size());
    for (size_t i = 0; i < boxes.size(); ++i) {
        const cv::Rect& b = boxes[i];
        const double w = b.width, h = b.height;
        if (w < minSide || h < minSide) continue;
        if (w > maxSide && h > maxSide) continue;
        const double aspect = std::max(w, h) / std::max(1.0, std::min(w, h));
        if (aspect > 8.0) continue;
        double sx = 0.0, sy = 0.0;
        for (const cv::Point& p : regions[i]) { sx += p.x; sy += p.y; }
        const double n = std::max<size_t>(1, regions[i].size());
        kept.push_back({ b, cv::Point2f(static_cast<float>(sx / n),
                                        static_cast<float>(sy / n)) });
    }

    // MSER nests regions → dedup by centroid proximity so one component
    // yields one detection (min pitch ≈ the smallest accepted side). Largest
    // area first, so each cluster keeps its most complete region — the whole
    // component body rather than an inner sub-region.
    std::sort(kept.begin(), kept.end(),
              [](const Blob& a, const Blob& b) { return a.box.area() > b.box.area(); });
    const double mergeR  = minSide * 0.8;
    const double mergeR2 = mergeR * mergeR;
    std::vector<Blob> dedup;
    dedup.reserve(kept.size());
    for (const auto& b : kept) {
        bool dup = false;
        for (const auto& d : dedup) {
            const double dx = b.centroid.x - d.centroid.x;
            const double dy = b.centroid.y - d.centroid.y;
            if (dx * dx + dy * dy < mergeR2) { dup = true; break; }
        }
        if (!dup) dedup.push_back(b);
    }

    // Cap to the strongest (largest) N — bootstrap tolerates false positives
    // but its consensus loop is O(nComp × nDet) per RANSAC iteration. Already
    // sorted by area above.
    if (static_cast<int>(dedup.size()) > maxDetections)
        dedup.resize(maxDetections);

    out.reserve(dedup.size());
    for (const auto& b : dedup) {
        ai::Detection d;
        d.classId    = 0;
        d.className  = "component";
        d.confidence = 1.0f;
        // ai::Detection only carries a bbox and consumers take its center
        // (ComponentReanchor::detectionCenter) — re-center the box on the
        // region centroid so that center IS the stable centroid.
        d.bbox = cv::Rect2f(b.centroid.x - b.box.width * 0.5f,
                            b.centroid.y - b.box.height * 0.5f,
                            static_cast<float>(b.box.width),
                            static_cast<float>(b.box.height));
        out.push_back(std::move(d));
    }
    spdlog::debug("[blob-detect] {} component-like blobs (scale={:.2f} px/mm)",
                  out.size(), scalePxPerMm);
    return out;
}

} // namespace ibom::overlay
