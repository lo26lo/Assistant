// Letterbox mapping used by InferenceEngine's YOLO pre/postprocess
// (src/ai/Letterbox.h): aspect-preserving resize + gray padding, and the
// inverse mapping of model-space boxes back into original image coords.
// Pure geometry — no ONNX Runtime needed, so this runs in CI too.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "ai/Letterbox.h"

using Catch::Approx;
using namespace ibom::ai;

TEST_CASE("letterboxInfo: 16:9 frame into a square canvas", "[letterbox]")
{
    // 1920x1080 → 640x640: limited by width, scale 1/3, vertical margins.
    const Letterbox lb = letterboxInfo({1920, 1080}, {640, 640});
    REQUIRE(lb.scale == Approx(640.0 / 1920.0));
    REQUIRE(lb.padX == Approx(0.f));
    REQUIRE(lb.padY == Approx((640.f - 1080.f * lb.scale) / 2.f));  // 140
    REQUIRE(lb.padY == Approx(140.f));
}

TEST_CASE("letterboxImage: content centered, margins gray", "[letterbox]")
{
    cv::Mat white(480, 848, CV_8UC3, cv::Scalar(255, 255, 255));  // D405 frame
    const cv::Size dst(640, 640);
    const Letterbox lb = letterboxInfo(white.size(), dst);
    const cv::Mat canvas = letterboxImage(white, dst, lb);

    REQUIRE(canvas.size() == dst);
    // Center pixel = image content (white).
    REQUIRE(canvas.at<cv::Vec3b>(320, 320) == cv::Vec3b(255, 255, 255));
    // Top and bottom margins = neutral gray 114.
    REQUIRE(canvas.at<cv::Vec3b>(2, 320) == cv::Vec3b(114, 114, 114));
    REQUIRE(canvas.at<cv::Vec3b>(637, 320) == cv::Vec3b(114, 114, 114));
}

TEST_CASE("unletterboxRect: model space → source space round trip", "[letterbox]")
{
    const cv::Size src(1920, 1080), dst(640, 640);
    const Letterbox lb = letterboxInfo(src, dst);

    // A component at a known source position, forward-mapped by hand…
    const cv::Rect2f srcBox(300.f, 500.f, 60.f, 30.f);
    const cv::Rect2f modelBox(srcBox.x * lb.scale + lb.padX,
                              srcBox.y * lb.scale + lb.padY,
                              srcBox.width * lb.scale,
                              srcBox.height * lb.scale);
    // …must come back exactly through the inverse mapping.
    const cv::Rect2f back = unletterboxRect(modelBox, lb);
    REQUIRE(back.x == Approx(srcBox.x));
    REQUIRE(back.y == Approx(srcBox.y));
    REQUIRE(back.width == Approx(srcBox.width));
    REQUIRE(back.height == Approx(srcBox.height));
}

TEST_CASE("clipRect: boxes leaking into the padding get clipped", "[letterbox]")
{
    const cv::Size src(1920, 1080), dst(640, 640);
    const Letterbox lb = letterboxInfo(src, dst);

    // A model box sitting partly in the top gray margin (y < padY) maps to a
    // negative source y — clipping must bring it back inside the image.
    const cv::Rect2f inPadding(10.f, lb.padY - 12.f, 30.f, 30.f);
    const cv::Rect2f raw = unletterboxRect(inPadding, lb);
    REQUIRE(raw.y < 0.f);
    const cv::Rect2f clipped = clipRect(raw, src);
    REQUIRE(clipped.y == Approx(0.f));
    REQUIRE(clipped.height < raw.height);
    REQUIRE(clipped.height > 0.f);

    // A box entirely inside the image is untouched.
    const cv::Rect2f inside(100.f, 100.f, 50.f, 50.f);
    const cv::Rect2f same = clipRect(inside, src);
    REQUIRE(same == inside);
}

TEST_CASE("letterboxInfo: degenerate sizes are safe", "[letterbox]")
{
    const Letterbox lb = letterboxInfo({0, 0}, {640, 640});
    REQUIRE(lb.scale == Approx(1.f));
    REQUIRE(lb.padX == Approx(0.f));
    REQUIRE(lb.padY == Approx(0.f));
}
