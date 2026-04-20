#include "TrackingWorker.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

namespace ibom::overlay {

TrackingWorker::TrackingWorker(QObject* parent)
    : QObject(parent)
{
    m_detector = cv::ORB::create(500);
    m_matcher  = cv::BFMatcher::create(cv::NORM_HAMMING, true);
}

void TrackingWorker::configure(int orbKeypoints,
                               int minMatchCount,
                               double matchDistanceRatio,
                               double ransacThreshold,
                               int intervalMs)
{
    m_detector           = cv::ORB::create(std::max(50, orbKeypoints));
    m_minMatchCount      = std::max(4, minMatchCount);
    m_matchDistanceRatio = matchDistanceRatio;
    m_ransacThreshold    = ransacThreshold;
    m_intervalMs         = std::max(0, intervalMs);

    // Drop reference so the next frame re-captures it with the new detector.
    m_hasReference = false;
    m_refKeypoints.clear();
    m_refDescriptors = cv::Mat();

    spdlog::info("TrackingWorker configured: ORB={}, minMatch={}, ratio={:.2f}, "
                 "RANSAC={:.1f}, interval={}ms",
                 orbKeypoints, minMatchCount, matchDistanceRatio,
                 ransacThreshold, intervalMs);
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

        std::vector<cv::KeyPoint> kp;
        cv::Mat desc;
        m_detector->detectAndCompute(small, cv::noArray(), kp, desc);

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

        std::vector<cv::DMatch> matches;
        m_matcher->match(m_refDescriptors, desc, matches);
        if (matches.size() < static_cast<size_t>(m_minMatchCount))
            return;

        double minDist = 1e9;
        for (const auto& m : matches)
            if (m.distance < minDist) minDist = m.distance;
        const double threshold = std::max(m_matchDistanceRatio * minDist, 30.0);

        std::vector<cv::Point2f> srcPts, dstPts;
        srcPts.reserve(matches.size());
        dstPts.reserve(matches.size());
        for (const auto& m : matches) {
            if (m.distance <= threshold) {
                srcPts.push_back(m_refKeypoints[m.queryIdx].pt);
                dstPts.push_back(kp[m.trainIdx].pt);
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

    } catch (const cv::Exception& e) {
        emit trackingError(QString::fromStdString(e.what()));
        spdlog::warn("TrackingWorker: cv::Exception: {}", e.what());
    }
}

} // namespace ibom::overlay
