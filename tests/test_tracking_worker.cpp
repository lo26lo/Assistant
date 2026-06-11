// Tests for TrackingWorker — the ORB + BFMatcher(knn) + Lowe ratio + RANSAC
// pipeline. This is the component most exposed to OpenCV behavioral differences
// between the Windows (4.12) and Jetson (4.10) builds, so we pin it down with a
// synthetic, deterministic homography-recovery test.
//
// The worker is exercised directly on the test thread (no QThread, no event
// loop): its signals are captured via direct-connection lambdas, which fire
// synchronously at emit time — no QCoreApplication required.

#include <catch2/catch_test_macros.hpp>

#include "overlay/TrackingWorker.h"
#include "camera/CameraCapture.h"  // ibom::camera::FrameRef

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>

#include <QObject>
#include <QString>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using ibom::overlay::TrackingWorker;
using ibom::camera::FrameRef;

namespace {

// Richly-textured grayscale-ish image so ORB finds plenty of distinctive
// corners. A plain checkerboard would yield ambiguous, repetitive features
// that defeat the ratio test — random blobs give unique, matchable structure.
cv::Mat makeTexture(int w = 800, int h = 600)
{
    cv::Mat img(h, w, CV_8UC3, cv::Scalar(40, 40, 40));
    cv::RNG rng(12345);  // fixed seed → deterministic test
    for (int i = 0; i < 400; ++i) {
        const cv::Point c(rng.uniform(0, w), rng.uniform(0, h));
        const int r = rng.uniform(3, 14);
        const cv::Scalar col(rng.uniform(60, 255), rng.uniform(60, 255), rng.uniform(60, 255));
        if (i % 2 == 0)
            cv::circle(img, c, r, col, cv::FILLED, cv::LINE_AA);
        else
            cv::rectangle(img, cv::Rect(c.x, c.y, r * 2, r * 2), col, cv::FILLED);
    }
    return img;
}

FrameRef wrap(const cv::Mat& m) { return std::make_shared<const cv::Mat>(m.clone()); }

} // namespace

TEST_CASE("TrackingWorker recovers a known homography", "[tracking]")
{
    TrackingWorker worker;

    // downscale=1.0 keeps keypoints in full-res coords; intervalMs=0 disables
    // throttling so the second frame is processed right after the reference.
    worker.configure(/*orbKeypoints*/ 1000, /*minMatchCount*/ 15,
                     /*loweRatio*/ 0.75, /*ransacThreshold*/ 3.0,
                     /*intervalMs*/ 0, /*downscale*/ 1.0f);
    worker.setBaseHomography(cv::Mat::eye(3, 3, CV_64F));

    int refKp = 0;
    QObject::connect(&worker, &TrackingWorker::referenceCaptured,
                     [&](int n) { refKp = n; });

    cv::Mat updated;
    int inliers = 0;
    double reprojErr = -1.0;
    QObject::connect(&worker, &TrackingWorker::homographyUpdated,
                     [&](cv::Mat h, int n, double e) {
                         updated = h.clone();
                         inliers = n;
                         reprojErr = e;
                     });

    QString err;
    QObject::connect(&worker, &TrackingWorker::trackingError,
                     [&](QString m) { err = m; });

    const cv::Mat base = makeTexture();

    // 1) First frame becomes the reference.
    worker.processFrame(wrap(base));
    REQUIRE(err.isEmpty());
    REQUIRE(refKp > 15);

    // 2) Known mild homography: small scale + translation + slight perspective.
    const cv::Mat H = (cv::Mat_<double>(3, 3) <<
        1.03, 0.01, 18.0,
        0.00, 1.02, 12.0,
        1e-5, 1e-5, 1.0);
    cv::Mat warped;
    cv::warpPerspective(base, warped, H, base.size());

    // 3) Track the warped frame; with base = identity, combined ≈ H.
    worker.processFrame(wrap(warped));
    REQUIRE(err.isEmpty());
    REQUIRE_FALSE(updated.empty());
    REQUIRE(updated.rows == 3);
    REQUIRE(updated.cols == 3);

    // Compare by reprojecting interior points (robust to the overall scale of
    // the homography matrix entries — direct element compare would be brittle).
    const std::vector<cv::Point2f> pts = {
        {200, 150}, {600, 150}, {200, 450}, {600, 450}, {400, 300}};
    std::vector<cv::Point2f> viaH, viaEst;
    cv::perspectiveTransform(pts, viaH, H);
    cv::perspectiveTransform(pts, viaEst, updated);

    double maxErr = 0.0;
    for (size_t i = 0; i < pts.size(); ++i) {
        const cv::Point2f d = viaH[i] - viaEst[i];
        maxErr = std::max(maxErr, std::hypot(static_cast<double>(d.x),
                                             static_cast<double>(d.y)));
    }

    INFO("max reprojection error = " << maxErr << " px");
    REQUIRE(maxErr < 5.0);

    // Quality metrics published with the homography: a clean synthetic warp
    // must yield a healthy inlier count and a sub-threshold median error.
    REQUIRE(inliers >= 15);
    REQUIRE(reprojErr >= 0.0);
    REQUIRE(reprojErr < 3.0);
}

TEST_CASE("TrackingWorker emits no homography without a base", "[tracking]")
{
    TrackingWorker worker;
    worker.configure(800, 15, 0.75, 3.0, 0, 1.0f);
    // Deliberately NOT calling setBaseHomography → m_baseHomography stays empty,
    // so processFrame must bail before emitting homographyUpdated.

    bool got = false;
    QObject::connect(&worker, &TrackingWorker::homographyUpdated,
                     [&](cv::Mat, int, double) { got = true; });

    const cv::Mat base = makeTexture();
    worker.processFrame(wrap(base));  // captures reference
    worker.processFrame(wrap(base));  // identical frame, matches, but no base
    REQUIRE_FALSE(got);
}
