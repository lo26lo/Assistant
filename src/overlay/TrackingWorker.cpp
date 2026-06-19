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
    // Determinism: USAC is deterministic but fix the global RNG too so any
    // residual randomness can't add frame-to-frame wobble.
    cv::setRNGSeed(12345);
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

void TrackingWorker::setHybridCorrection(bool enabled)
{
    m_hybrid = enabled;
    spdlog::info("TrackingWorker: hybrid drift correction={}", enabled);
}

void TrackingWorker::setBaseHomography(cv::Mat h)
{
    m_baseHomography = h.clone();
    m_lastHomography = h.clone();  // best estimate available until tracking emits one
    resetReference();  // reference must be recaptured against new base
}

void TrackingWorker::setBoardPolygon(std::vector<cv::Point2f> pcbPoints)
{
    m_pcbPolygon = std::move(pcbPoints);
}

cv::Mat TrackingWorker::buildBoardMask(const cv::Size& smallSize, float downscale) const
{
    if (m_pcbPolygon.empty() || m_lastHomography.empty())
        return {};

    std::vector<cv::Point2f> imgPoly;
    try {
        cv::perspectiveTransform(m_pcbPolygon, imgPoly, m_lastHomography);
    } catch (const cv::Exception&) {
        return {};
    }
    if (imgPoly.empty())
        return {};

    // Grow the polygon outward from its centroid to tolerate motion since
    // the homography estimate this mask is built from — the board may have
    // moved between the last update and this frame.
    cv::Point2f centroid(0.f, 0.f);
    for (const auto& p : imgPoly) centroid += p;
    centroid *= (1.0f / static_cast<float>(imgPoly.size()));
    constexpr float kMarginScale = 1.6f;

    std::vector<cv::Point> maskPoly;
    maskPoly.reserve(imgPoly.size());
    for (const auto& p : imgPoly) {
        cv::Point2f grown = centroid + (p - centroid) * kMarginScale;
        // Scale into the downscaled detection frame's coordinate space.
        maskPoly.emplace_back(cv::saturate_cast<int>(grown.x * downscale),
                               cv::saturate_cast<int>(grown.y * downscale));
    }

    cv::Mat mask = cv::Mat::zeros(smallSize, CV_8U);
    cv::fillConvexPoly(mask, maskPoly, cv::Scalar(255));
    return mask;
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
    m_smoothedHomography = cv::Mat();
    m_lastEmittedH       = cv::Mat();
    m_staticFrames       = 0;
    m_lowQualityFrames   = 0;
    setState(State::Lost);
}

double TrackingWorker::cornerDisp(const cv::Mat& a, const cv::Mat& b) const
{
    if (a.empty() || b.empty() || m_pcbPolygon.size() < 4)
        return -1.0;
    std::vector<cv::Point2f> pa, pb;
    try {
        cv::perspectiveTransform(m_pcbPolygon, pa, a);
        cv::perspectiveTransform(m_pcbPolygon, pb, b);
    } catch (const cv::Exception&) {
        return -1.0;
    }
    double maxDisp = 0.0;
    for (size_t i = 0; i < pa.size(); ++i)
        maxDisp = std::max(maxDisp, cv::norm(pb[i] - pa[i]));
    return maxDisp;
}

bool TrackingWorker::emitHomography(const cv::Mat& rawH, int inliers, double reprojErr)
{
    // Quality gate: too few inliers ⇒ don't trust this fit, hold the last good
    // pose rather than letting the overlay jump. A short hysteresis avoids
    // flickering the state on a single bad frame.
    if (inliers < m_minMatchCount) {
        if (++m_lowQualityFrames >= 3) setState(State::Lost);
        return false;
    }
    m_lowQualityFrames = 0;

    // Static-scene gate: if the new estimate barely differs from the last one
    // we emitted, the scene isn't really moving — emit nothing so the overlay
    // freezes instead of shimmering on keypoint noise. cornerDisp() returns -1
    // when it can't measure (no polygon yet) → fall through and emit.
    const double disp = cornerDisp(rawH, m_lastEmittedH);
    if (disp >= 0.0 && disp < m_staticThreshPx) {
        ++m_staticFrames;
        return false;
    }
    m_staticFrames = 0;

    const cv::Mat smoothed = smoothHomography(rawH);
    m_lastEmittedH = smoothed.clone();
    emit homographyUpdated(smoothed, inliers, reprojErr);
    return true;
}

void TrackingWorker::refineKeypointsSubPix(const cv::Mat& fullGray,
                                           std::vector<cv::KeyPoint>& kp)
{
    if (kp.empty() || fullGray.empty() || fullGray.type() != CV_8UC1)
        return;
    std::vector<cv::Point2f> pts;
    pts.reserve(kp.size());
    for (const auto& k : kp) pts.push_back(k.pt);
    try {
        cv::cornerSubPix(
            fullGray, pts, cv::Size(5, 5), cv::Size(-1, -1),
            cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::MAX_ITER,
                             20, 0.03));
        for (size_t i = 0; i < kp.size(); ++i) kp[i].pt = pts[i];
    } catch (const cv::Exception&) {
        // Leave keypoints unrefined on any failure (e.g. point near border).
    }
}

cv::Mat TrackingWorker::smoothHomography(const cv::Mat& rawH)
{
    if (rawH.empty() || m_pcbPolygon.size() < 4)
        return rawH;

    if (m_smoothedHomography.empty()) {
        m_smoothedHomography = rawH.clone();
        return rawH;
    }

    std::vector<cv::Point2f> prevPts, newPts;
    try {
        cv::perspectiveTransform(m_pcbPolygon, prevPts, m_smoothedHomography);
        cv::perspectiveTransform(m_pcbPolygon, newPts, rawH);
    } catch (const cv::Exception&) {
        m_smoothedHomography = rawH.clone();
        return rawH;
    }

    // Max corner displacement between the previous smoothed estimate and the
    // fresh raw one. Small values are sensor/keypoint noise on an otherwise
    // static scene; large values are real motion.
    double maxDisp = 0.0;
    for (size_t i = 0; i < prevPts.size(); ++i)
        maxDisp = std::max(maxDisp, cv::norm(newPts[i] - prevPts[i]));

    // Below kNoiseFloorPx: heavily damp (mostly trust the previous estimate).
    // Above kSnapPx: trust the new estimate fully (don't lag real motion).
    // In between: linearly ramp the blend weight given to the new estimate.
    constexpr double kNoiseFloorPx = 1.5;
    constexpr double kSnapPx       = 12.0;
    constexpr double kMinAlpha     = 0.15;  // weight on new pts at/below the noise floor

    double alpha;
    if (maxDisp <= kNoiseFloorPx)
        alpha = kMinAlpha;
    else if (maxDisp >= kSnapPx)
        alpha = 1.0;
    else
        alpha = kMinAlpha + (1.0 - kMinAlpha) * (maxDisp - kNoiseFloorPx) / (kSnapPx - kNoiseFloorPx);

    std::vector<cv::Point2f> blended(prevPts.size());
    for (size_t i = 0; i < prevPts.size(); ++i)
        blended[i] = prevPts[i] * (1.0 - alpha) + newPts[i] * alpha;

    cv::Mat smoothed;
    try {
        smoothed = cv::findHomography(m_pcbPolygon, blended, 0 /* least-squares, no RANSAC */);
    } catch (const cv::Exception&) {
        smoothed = cv::Mat();
    }
    if (smoothed.empty() || smoothed.rows != 3 || smoothed.cols != 3) {
        m_smoothedHomography = rawH.clone();
        return rawH;
    }

    m_smoothedHomography = smoothed;
    return smoothed;
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
        const float fwdScale = (m_downscale > 0.0f && m_downscale < 1.0f) ? m_downscale : 1.0f;

        // Restrict detection to the board's projected area (grown by a
        // margin) so a static background can't outvote the board's own
        // keypoints in RANSAC — see setBoardPolygon().
        cv::Mat mask = buildBoardMask(small.size(), fwdScale);

        std::vector<cv::KeyPoint> kp;
        cv::Mat desc;
        m_detector->detectAndCompute(small, mask, kp, desc);

        // Rescale keypoint positions back to original-resolution coords.
        for (auto& k : kp) {
            k.pt.x *= invScale;
            k.pt.y *= invScale;
        }

        // Sub-pixel refinement on the full-res gray image — cuts the
        // quantization jitter ORB carries (worse after the ×downscale
        // upscaling). Done before matching so descriptors keep their indices.
        refineKeypointsSubPix(gray, kp);

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
    cv::Mat frameH = cv::findHomography(srcPts, dstPts, cv::USAC_MAGSAC,
                                        m_ransacThreshold, inlierMask);
    if (frameH.empty() || frameH.rows != 3 || frameH.cols != 3)
        return;
    if (m_baseHomography.empty())
        return;

    int inliers = 0;
    double reprojErr = medianReprojError(srcPts, dstPts, frameH, inlierMask, inliers);

    cv::Mat combined = frameH * m_baseHomography;
    m_lastHomography = combined.clone();
    emitHomography(combined, inliers, reprojErr);
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
        // Keep this first frame as the drift-free anchor keyframe so hybrid
        // correction can later snap back to it (see processIncremental's
        // hybrid path). The base homography maps PCB→this frame's image, so
        // matching a future frame against this keyframe yields a drift-free
        // PCB→current estimate.
        m_refKeypoints   = kp;
        m_refDescriptors = desc.clone();
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

    // ── Hybrid drift correction (beta) ────────────────────────────────
    // Before the (drift-prone) frame→frame step, try to recognize the
    // original anchor keyframe directly in the current frame. When it matches
    // confidently, the resulting PCB→current estimate carries NO accumulated
    // drift, so we snap to it and reset the drift counter. When the board has
    // moved too far for the anchor to be recognizable, this silently fails and
    // we fall through to incremental composition.
    if (m_hybrid && !m_refDescriptors.empty() && !m_baseHomography.empty() &&
        !desc.empty() && kp.size() >= static_cast<size_t>(m_minMatchCount)) {
        std::vector<cv::Point2f> rSrc, rDst;
        matchPoints(m_refDescriptors, m_refKeypoints, desc, kp, rSrc, rDst);
        if (rSrc.size() >= static_cast<size_t>(m_minMatchCount)) {
            cv::Mat rMask;
            cv::Mat refH = cv::findHomography(rSrc, rDst, cv::USAC_MAGSAC,
                                              m_ransacThreshold, rMask);
            if (!refH.empty() && refH.rows == 3 && refH.cols == 3) {
                int rInliers = 0;
                const double rErr = medianReprojError(rSrc, rDst, refH, rMask, rInliers);
                // Trust it as drift-free only when the fit is both well-
                // supported and tight — a weak/loose anchor match would inject
                // a worse estimate than the incremental one it replaces.
                if (rInliers >= m_minMatchCount && rErr <= m_ransacThreshold) {
                    cv::Mat combined = refH * m_baseHomography;
                    m_cumulativeH      = combined.clone();
                    m_lastHomography   = combined.clone();
                    m_prevKeypoints    = kp;
                    m_prevDescriptors  = desc.clone();
                    m_accumulatedDrift = 0.0;
                    m_lostFrames       = 0;
                    setState(State::Locked);
                    emitHomography(combined, rInliers, rErr);
                    spdlog::debug("TrackingWorker[hybrid]: anchor re-locked, "
                                  "inliers={}/{} err={:.2f}px (drift reset)",
                                  rInliers, rSrc.size(), rErr);
                    return;
                }
            }
        }
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
    cv::Mat deltaH = cv::findHomography(srcPts, dstPts, cv::USAC_MAGSAC,
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
    m_lastHomography = m_cumulativeH.clone();

    // Advance the reference to the current frame.
    m_prevKeypoints   = kp;
    m_prevDescriptors = desc.clone();
    m_lostFrames      = 0;

    // Drift grows with each composition; per-frame fit residual is a rough
    // proxy for the positional error injected this frame. Conservative (flags
    // early) — re-anchoring resets it.
    m_accumulatedDrift += reprojErr;
    setState(m_accumulatedDrift > m_reanchorDriftPx ? State::Drifting : State::Locked);

    emitHomography(m_cumulativeH, inliers, reprojErr);

    spdlog::debug("TrackingWorker[incr]: inliers={}/{} err={:.2f}px drift={:.1f}px state={}",
                  inliers, srcPts.size(), reprojErr, m_accumulatedDrift,
                  static_cast<int>(m_state));
}

} // namespace ibom::overlay
