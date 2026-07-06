#pragma once

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace ibom::ai {

/// Aspect-preserving resize-with-padding (ultralytics-style "letterbox").
///
/// YOLO models are trained on letterboxed inputs: the image is scaled by a
/// single factor to fit the model canvas and the leftover margins are filled
/// with neutral gray (114). Feeding a plain anisotropic cv::resize instead —
/// as this codebase did before 2026-07 — shows the network components
/// squeezed 16:9→1:1, a distortion it never saw in training, costing accuracy
/// for free (docs/INVESTIGATION_360_2026-07.md §4.1).
///
/// Pure functions, no ONNX dependency — unit-tested in tests/test_letterbox.cpp.
struct Letterbox {
    float scale = 1.f;  ///< source px → model px (single isotropic factor)
    float padX  = 0.f;  ///< left margin in model px
    float padY  = 0.f;  ///< top margin in model px
};

/// Mapping of a `src`-sized image into a `dst`-sized model canvas.
inline Letterbox letterboxInfo(const cv::Size& src, const cv::Size& dst)
{
    Letterbox lb;
    if (src.width <= 0 || src.height <= 0 || dst.width <= 0 || dst.height <= 0)
        return lb;
    lb.scale = std::min(dst.width  / static_cast<float>(src.width),
                        dst.height / static_cast<float>(src.height));
    lb.padX = (dst.width  - src.width  * lb.scale) * 0.5f;
    lb.padY = (dst.height - src.height * lb.scale) * 0.5f;
    return lb;
}

/// Resize `src` by lb.scale and center it on a gray canvas of size `dst`.
inline cv::Mat letterboxImage(const cv::Mat& src, const cv::Size& dst,
                              const Letterbox& lb, uchar padValue = 114)
{
    cv::Mat canvas(dst, src.type(), cv::Scalar::all(padValue));
    const int w = std::max(1, static_cast<int>(std::lround(src.cols * lb.scale)));
    const int h = std::max(1, static_cast<int>(std::lround(src.rows * lb.scale)));
    const int x = std::clamp(static_cast<int>(std::lround(lb.padX)), 0, dst.width  - 1);
    const int y = std::clamp(static_cast<int>(std::lround(lb.padY)), 0, dst.height - 1);
    const cv::Rect roi(x, y,
                       std::min(w, dst.width  - x),
                       std::min(h, dst.height - y));
    cv::Mat view = canvas(roi);
    cv::resize(src, view, roi.size(), 0, 0, cv::INTER_LINEAR);
    return canvas;
}

/// Map a bbox from model space back to source-image space.
inline cv::Rect2f unletterboxRect(const cv::Rect2f& r, const Letterbox& lb)
{
    const float inv = lb.scale > 0.f ? 1.f / lb.scale : 1.f;
    return { (r.x - lb.padX) * inv, (r.y - lb.padY) * inv,
             r.width * inv, r.height * inv };
}

/// Clip a rect to an image of size `img` (letterbox margins can put part of a
/// raw model box inside the gray padding, i.e. outside the real image).
inline cv::Rect2f clipRect(const cv::Rect2f& r, const cv::Size& img)
{
    const float x0 = std::clamp(r.x, 0.f, static_cast<float>(img.width));
    const float y0 = std::clamp(r.y, 0.f, static_cast<float>(img.height));
    const float x1 = std::clamp(r.x + r.width,  0.f, static_cast<float>(img.width));
    const float y1 = std::clamp(r.y + r.height, 0.f, static_cast<float>(img.height));
    return { x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0) };
}

} // namespace ibom::ai
