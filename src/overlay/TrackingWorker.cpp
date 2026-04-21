#include "TrackingWorker.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

namespace ibom::overlay {

TrackingWorker::TrackingWorker(QObject* parent)
    : QObject(parent)
{
    m_detector = cv::ORB::create(200);
    // crossCheck must be false: knnMatch(k=2) is incompatible with crossCheck.
    m_matcher  = cv::BFMatcher::create(cv::NORM_HAMMING, false);
}

void TrackingWorker::configure(int orbKeypoints,
                               int minMatchCount,
                               double loweRatio,
                               double ransacThreshold,
                               int intervalMs,
                               float downscale)
{
    m_detector        = cv::ORB::create(std::max(50, orbKeypoints));
    m_minMatchCount   = std::max(4, minMatchCount);
    m_loweRatio       = std::clamp(loweRatio, 0.1, 0.99);
    m_ransacThreshold = ransacThreshold;
    m_intervalMs      = std::max(0, intervalMs);
    m_downscale       = std::clamp(downscale, 0.1f, 1.0f);

    // Drop reference so the next frame re-captures it with the new detector.
    m_hasReference = false;
    m_refKeypoints.clear();
    m_refDescriptors = cv::Mat();

    spdlog::info("TrackingWorker configured: ORB={}, minMatch={}, loweRatio={:.2f}, "
                 "RANSAC={:.1f}, interval={}ms, downscale={:.2f}",
                 orbKeypoints, minMatchCount, m_loweRatio,
                 ransacThreshold, intervalMs, m_downscale);
}

void TrackingWorker::setBaseHomography(cv::Mat h)
{
    m_baseHomography = h.clone();
    m_hasReference = false;  // reference must be recaptured against new base
    m_refKeypoints.clear();
    m_refDescriptors = cv::Mat();
}

void TrackingWorker::resetReference()
{
    m_hasReference = false;
    m_refKeypoints.clear();
    m_refDescriptors = cv::Mat();
}

void TrackingWorker::processFrame(ibom::camera::FrameRef frame)
{
    if (!frame || frame->empty())
        return;

    // Throttle — if the GUI thread posts faster than m_intervalMs, drop.
    const auto now = std::chrono::steady_clock::now();
    if (m_hasReference) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastProcessTime).count();
        if (elapsed < m_intervalMs) return;
    }
    m_lastProcessTime = now;

    const auto t0 = std::chrono::steady_clock::now();

    try {
        // Convert + downscale for speed. Keypoints are rescaled back to full
        // resolution so the emitted homography is in original image coords.
        cv::Mat gray;
        if (frame->channels() == 3)
            cv::cvtColor(*frame, gray, cv::COLOR_BGR2GRAY);
        else
            gray = *frame;

        cv::Mat small;
        if (m_downscale > 0.0f && m_downscale < 1.0f)
            cv::resize(gray, small, cv::Size(), m_downscale, m_downscale, cv::INTER_AREA);
        else
            small = gray;

        const float invScale = (m_downscale > 0.0f) ? (1.0f / m_downscale) : 1.0f;
        const auto tPrep = std::chrono::steady_clock::now();

        std::vector<cv::KeyPoint> kp;
        cv::Mat desc;
        m_detector->detectAndCompute(small, cv::noArray(), kp, desc);
        const auto tDetect = std::chrono::steady_clock::now();

        // Rescale keypoint positions back to original-resolution coords.
        for (auto& k : kp) {
            k.pt.x *= invScale;
            k.pt.y *= invScale;
        }

        if (!m_hasReference) {
            if (kp.size() < static_cast<size_t>(m_minMatchCount) || desc.empty()) {
                return;  // wait for a better reference frame
            }
            m_refKeypoints   = std::move(kp);
            m_refDescriptors = desc.clone();
            m_hasReference   = true;
            emit referenceCaptured(static_cast<int>(m_refKeypoints.size()));
            spdlog::info("TrackingWorker: reference captured ({} keypoints)",
                         m_refKeypoints.size());
            return;
        }

        if (desc.empty() || kp.size() < static_cast<size_t>(m_minMatchCount))
            return;

        // Lowe's ratio test — only keep matches where the nearest neighbor is
        // clearly better than the second-nearest. Standard practice for ORB/SIFT.
        std::vector<std::vector<cv::DMatch>> knn;
        m_matcher->knnMatch(m_refDescriptors, desc, knn, 2);
        const auto tMatch = std::chrono::steady_clock::now();

        std::vector<cv::Point2f> srcPts, dstPts;
        srcPts.reserve(knn.size());
        dstPts.reserve(knn.size());
        for (const auto& pair : knn) {
            if (pair.size() < 2) continue;
            if (pair[0].distance < m_loweRatio * pair[1].distance) {
                srcPts.push_back(m_refKeypoints[pair[0].queryIdx].pt);
                dstPts.push_back(kp[pair[0].trainIdx].pt);
            }
        }

        if (srcPts.size() < static_cast<size_t>(m_minMatchCount))
            return;

        cv::Mat frameH = cv::findHomography(srcPts, dstPts, cv::RANSAC, m_ransacThreshold);
        if (frameH.empty() || frameH.rows != 3 || frameH.cols != 3)
            return;

        if (m_baseHomography.empty())
            return;

        cv::Mat combined = frameH * m_baseHomography;
        emit homographyUpdated(combined);

        const auto tEnd = std::chrono::steady_clock::now();
        using ms = std::chrono::duration<double, std::milli>;
        spdlog::debug("TrackingWorker: {}×{} → kp={} inliers={} | prep={:.1f} detect={:.1f} "
                      "match={:.1f} homog={:.1f} total={:.1f} ms",
                      small.cols, small.rows, kp.size(), srcPts.size(),
                      ms(tPrep   - t0).count(),
                      ms(tDetect - tPrep).count(),
                      ms(tMatch  - tDetect).count(),
                      ms(tEnd    - tMatch).count(),
                      ms(tEnd    - t0).count());

    } catch (const cv::Exception& e) {
        emit trackingError(QString::fromStdString(e.what()));
        spdlog::warn("TrackingWorker: cv::Exception: {}", e.what());
    }
}

} // namespace ibom::overlay
