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

std::vector<ai::Detection> detectPadBlobs(const cv::Mat& image,
                                          double scalePxPerMm,
                                          int maxDetections)
{
    std::vector<ai::Detection> out;
    if (image.empty()) return out;

    cv::Mat gray;
    if (image.channels() == 3)      cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    else if (image.channels() == 4) cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
    else                            gray = image;

    // Pad size window (px): ~0.3 mm (an 0201 pad) to ~6 mm (QFN thermal,
    // connector tab). No scale → permissive pixel defaults.
    double minSide, maxSide;
    if (scalePxPerMm > 0.0) {
        minSide = std::max(2.0, 0.3 * scalePxPerMm);
        maxSide = 6.0 * scalePxPerMm;
    } else {
        minSide = 2.0;
        maxSide = 40.0;
    }

    // White top-hat: keeps bright features SMALLER than the structuring
    // element, flattens everything slower-varying (soldermask, table, light
    // gradient). Response is relative to the local background — that's what
    // makes it hold up in the dim scenes where MSER drowned (ERREUR #59).
    const int k = (static_cast<int>(maxSide) | 1) + 2;  // odd, > biggest pad
    cv::Mat tophat;
    cv::morphologyEx(gray, tophat, cv::MORPH_TOPHAT,
                     cv::getStructuringElement(cv::MORPH_ELLIPSE, { k, k }));

    // Otsu adapts the cut to the scene; the absolute floor keeps a blank or
    // noise-only frame (Otsu happily splits pure noise) from yielding speckle.
    constexpr double kContrastFloor = 18.0;
    cv::Mat bin;
    const double otsu = cv::threshold(tophat, bin, 0, 255,
                                      cv::THRESH_BINARY | cv::THRESH_OTSU);
    if (otsu < kContrastFloor)
        cv::threshold(tophat, bin, kContrastFloor, 255, cv::THRESH_BINARY);

    // Solid connected components in the pad-size window. No dedup needed —
    // components are disjoint by construction.
    cv::Mat labels, stats, centroids;
    const int n = cv::connectedComponentsWithStats(bin, labels, stats,
                                                   centroids, 8, CV_32S);
    struct PadBlob { double area; cv::Rect box; cv::Point2f c; };
    std::vector<PadBlob> pads;
    pads.reserve(static_cast<size_t>(std::max(0, n - 1)));
    for (int i = 1; i < n; ++i) {
        const int w    = stats.at<int>(i, cv::CC_STAT_WIDTH);
        const int h    = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        const int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (w < minSide || h < minSide) continue;
        if (w > maxSide && h > maxSide) continue;  // both huge = background band
        const double aspect = static_cast<double>(std::max(w, h))
                            / std::max(1, std::min(w, h));
        if (aspect > 5.0) continue;                // streaks, silk lines
        if (area < 0.35 * w * h) continue;         // hollow/ragged = not a pad
        pads.push_back({ static_cast<double>(area),
                         cv::Rect(stats.at<int>(i, cv::CC_STAT_LEFT),
                                  stats.at<int>(i, cv::CC_STAT_TOP), w, h),
                         cv::Point2f(static_cast<float>(centroids.at<double>(i, 0)),
                                     static_cast<float>(centroids.at<double>(i, 1))) });
    }
    std::sort(pads.begin(), pads.end(),
              [](const PadBlob& a, const PadBlob& b) { return a.area > b.area; });
    if (static_cast<int>(pads.size()) > maxDetections)
        pads.resize(maxDetections);

    out.reserve(pads.size());
    for (const auto& p : pads) {
        ai::Detection d;
        d.classId    = 0;
        d.className  = "pad";
        d.confidence = 1.0f;
        // Center the bbox on the centroid: consumers take its center
        // (ComponentReanchor::detectionCenter).
        d.bbox = cv::Rect2f(p.c.x - p.box.width * 0.5f,
                            p.c.y - p.box.height * 0.5f,
                            static_cast<float>(p.box.width),
                            static_cast<float>(p.box.height));
        out.push_back(std::move(d));
    }
    spdlog::debug("[pad-detect] {} pad-like blobs (otsu {:.0f}, scale={:.2f} px/mm)",
                  out.size(), otsu, scalePxPerMm);
    return out;
}

} // namespace ibom::overlay
