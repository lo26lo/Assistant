#include "TrackingWorker.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

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
    resetReference();

    spdlog::info("TrackingWorker configured: ORB={}, minMatch={}, loweRatio={:.2f}, "
                 "RANSAC={:.1f}, interval={}ms, downscale={:.2f}",
                 orbKeypoints, minMatchCount, m_loweRatio,
                 ransacThreshold, intervalMs, m_downscale);
}

void TrackingWorker::setIncrementalMode(bool enabled, double driftThresholdPx)
{
    m_incremental     = enabled;
    m_reanchorDriftPx = std::max(1.0, driftThresholdPx);
    resetReference();
    spdlog::info("TrackingWorker: incremental={}, reanchorDrift={:.1f}px",
                 enabled, m_reanchorDriftPx);
}

void TrackingWorker::setBaseHomography(cv::Mat h)
{
    m_baseHomography = h.clone();
    resetReference();  // reference must be recaptured against new base
}

void TrackingWorker::resetReference()
{
    m_hasReference = false;
    m_refKeypoints.clear();
    m_refDescriptors = cv::Mat();

    m_prevKeypoints.clear();
    m_prevDescriptors  = cv::Mat();
    m_cumulativeH      = cv::Mat();
    m_accumulatedDrift = 0.0;
    m_lostFrames       = 0;
    setState(State::Lost);
}

void TrackingWorker::setState(State s)
{
    if (s == m_state) return;
    m_state = s;
    emit trackingStateChanged(static_cast<int>(s));
}

void TrackingWorker::matchPoints(const cv::Mat& srcDesc,
                                 const std::vector<cv::KeyPoint>& srcKp,
                                 const cv::Mat& dstDesc,
                                 const std::vector<cv::KeyPoint>& dstKp,
                                 std::vector<cv::Point2f>& srcPts,
                                 std::vector<cv::Point2f>& dstPts)
{
    srcPts.clear();
    dstPts.clear();
    if (srcDesc.empty() || dstDesc.empty()) return;

    std::vector<std::vector<cv::DMatch>> knn;
    m_matcher->knnMatch(srcDesc, dstDesc, knn, 2);
    srcPts.reserve(knn.size());
    dstPts.reserve(knn.size());
    for (const auto& pair : knn) {
        if (pair.size() < 2) continue;
        if (pair[0].distance < m_loweRatio * pair[1].distance) {
            srcPts.push_back(srcKp[pair[0].queryIdx].pt);
            dstPts.push_back(dstKp[pair[0].trainIdx].pt);
        }
    }
}

double TrackingWorker::medianReprojError(const std::vector<cv::Point2f>& src,
                                         const std::vector<cv::Point2f>& dst,
                                         const cv::Mat& H, const cv::Mat& inlierMask,
                                         int& inlierCount)
{
    std::vector<cv::Point2f> projected;
    cv::perspectiveTransform(src, projected, H);
    std::vector<double> errs;
    errs.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        if (inlierMask.empty() || inlierMask.at<uchar>(static_cast<int>(i))) {
            const cv::Point2f d = projected[i] - dst[i];
            errs.push_back(std::hypot(static_cast<double>(d.x),
                                      static_cast<double>(d.y)));
        }
    }
    inlierCount = static_cast<int>(errs.size());
    if (errs.empty()) return 0.0;
    const auto mid = errs.begin() + errs.size() / 2;
    std::nth_element(errs.begin(), mid, errs.end());
    return *mid;
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

        if (m_incremental)
            processIncremental(kp, desc);
        else
            processReference(kp, desc);

    } catch (const cv::Exception& e) {
        emit trackingError(QString::fromStdString(e.what()));
        spdlog::warn("TrackingWorker: cv::Exception: {}", e.what());
    }
}

void TrackingWorker::processReference(const std::vector<cv::KeyPoint>& kp,
                                      const cv::Mat& desc)
{
    if (!m_hasReference) {
        if (kp.size() < static_cast<size_t>(m_minMatchCount) || desc.empty())
            return;  // wait for a better reference frame
        m_refKeypoints   = kp;
        m_refDescriptors = desc.clone();
        m_hasReference   = true;
        emit referenceCaptured(static_cast<int>(m_refKeypoints.size()));
        spdlog::info("TrackingWorker: reference captured ({} keypoints)",
                     m_refKeypoints.size());
        return;
    }

    if (desc.empty() || kp.size() < static_cast<size_t>(m_minMatchCount))
        return;

    std::vector<cv::Point2f> srcPts, dstPts;
    matchPoints(m_refDescriptors, m_refKeypoints, desc, kp, srcPts, dstPts);
    if (srcPts.size() < static_cast<size_t>(m_minMatchCount))
        return;

    cv::Mat inlierMask;
    cv::Mat frameH = cv::findHomography(srcPts, dstPts, cv::RANSAC,
                                        m_ransacThreshold, inlierMask);
    if (frameH.empty() || frameH.rows != 3 || frameH.cols != 3)
        return;
    if (m_baseHomography.empty())
        return;

    int inliers = 0;
    double reprojErr = medianReprojError(srcPts, dstPts, frameH, inlierMask, inliers);

    cv::Mat combined = frameH * m_baseHomography;
    emit homographyUpdated(combined, inliers, reprojErr);
}

void TrackingWorker::processIncremental(const std::vector<cv::KeyPoint>& kp,
                                        const cv::Mat& desc)
{
    // Bootstrap: first frame after a (re-)anchor becomes the previous frame,
    // cumulative homography starts at the anchor (base) homography.
    if (!m_hasReference) {
        if (kp.size() < static_cast<size_t>(m_minMatchCount) || desc.empty())
            return;
        if (m_baseHomography.empty())
            return;
        m_prevKeypoints    = kp;
        m_prevDescriptors  = desc.clone();
        m_cumulativeH      = m_baseHomography.clone();
        m_accumulatedDrift = 0.0;
        m_lostFrames       = 0;
        m_hasReference     = true;
        setState(State::Locked);
        emit referenceCaptured(static_cast<int>(m_prevKeypoints.size()));
        // Don't emit homographyUpdated here: no match/RANSAC has happened yet,
        // so there is no real inlier count. Consumers (e.g. DatasetCreator)
        // gate on `inliers`, and a fabricated value (keypoint count) could pass
        // quality gates incorrectly. The next processed frame emits a real one.
        spdlog::info("TrackingWorker: incremental anchor bootstrapped ({} keypoints)",
                     m_prevKeypoints.size());
        return;
    }

    // Match against the previous frame (small motion → high overlap).
    std::vector<cv::Point2f> srcPts, dstPts;
    if (!desc.empty() && kp.size() >= static_cast<size_t>(m_minMatchCount))
        matchPoints(m_prevDescriptors, m_prevKeypoints, desc, kp, srcPts, dstPts);

    if (srcPts.size() < static_cast<size_t>(m_minMatchCount)) {
        // Lost this frame — keep the last good prev so a small re-overlap can
        // recover, but flag Lost after a few consecutive failures.
        if (++m_lostFrames >= 4)
            setState(State::Lost);
        return;
    }

    cv::Mat inlierMask;
    cv::Mat deltaH = cv::findHomography(srcPts, dstPts, cv::RANSAC,
                                        m_ransacThreshold, inlierMask);
    if (deltaH.empty() || deltaH.rows != 3 || deltaH.cols != 3) {
        if (++m_lostFrames >= 4)
            setState(State::Lost);
        return;
    }

    int inliers = 0;
    double reprojErr = medianReprojError(srcPts, dstPts, deltaH, inlierMask, inliers);

    // Compose: PCB → prev_image → current_image.
    m_cumulativeH = deltaH * m_cumulativeH;

    // Advance the reference to the current frame.
    m_prevKeypoints   = kp;
    m_prevDescriptors = desc.clone();
    m_lostFrames      = 0;

    // Drift grows with each composition; per-frame fit residual is a rough
    // proxy for the positional error injected this frame. Conservative (flags
    // early) — re-anchoring resets it.
    m_accumulatedDrift += reprojErr;
    setState(m_accumulatedDrift > m_reanchorDriftPx ? State::Drifting : State::Locked);

    emit homographyUpdated(m_cumulativeH.clone(), inliers, reprojErr);

    spdlog::debug("TrackingWorker[incr]: inliers={}/{} err={:.2f}px drift={:.1f}px state={}",
                  inliers, srcPts.size(), reprojErr, m_accumulatedDrift,
                  static_cast<int>(m_state));
}

} // namespace ibom::overlay
