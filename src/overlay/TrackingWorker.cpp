#include "TrackingWorker.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>   // calcOpticalFlowPyrLK
#ifdef IBOM_HAVE_OPENCV_CUDA
#  include <opencv2/core/cuda.hpp>
#endif
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>

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

    m_clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
}

void TrackingWorker::setAdvanced(bool clahe, bool opticalFlow, int gpuMode)
{
    m_useClahe    = clahe;
    m_opticalFlow = opticalFlow;
    m_gpuMode     = std::clamp(gpuMode, 0, 2);

    bool gpuAvail = false;
#ifdef IBOM_HAVE_OPENCV_CUDA
    try {
        gpuAvail = cv::cuda::getCudaEnabledDeviceCount() > 0;
    } catch (const cv::Exception&) {
        gpuAvail = false;
    }
#endif
    m_gpuActive = (m_gpuMode != 0) && gpuAvail;

#ifdef IBOM_HAVE_OPENCV_CUDA
    if (m_gpuActive) {
        try {
            const int cap = (m_targetKeypoints > 0 ? m_targetKeypoints : 200) * 2;
            m_gpuOrb = cv::cuda::ORB::create(cap);
        } catch (const cv::Exception& e) {
            spdlog::warn("TrackingWorker: GPU ORB init failed ({}), falling back to CPU",
                         e.what());
            m_gpuActive = false;
        }
    }
#endif

    // Optical-flow / detector switch invalidates the current reference.
    m_flowImg.clear();
    m_flowPcb.clear();
    m_prevGray = cv::Mat();
    spdlog::info("TrackingWorker advanced: clahe={}, opticalFlow={}, gpuMode={} (active={})",
                 m_useClahe, m_opticalFlow, m_gpuMode, m_gpuActive);
}

void TrackingWorker::configure(int orbKeypoints,
                               int minMatchCount,
                               double loweRatio,
                               double ransacThreshold,
                               int intervalMs,
                               float downscale)
{
    // Detect more candidates than we keep so bucketKeypoints() has something
    // to distribute spatially (ORB's own top-N is response-ranked and tends to
    // cluster). Target = requested count; detector cap = 2× target.
    m_targetKeypoints = std::max(50, orbKeypoints);
    m_detector        = cv::ORB::create(m_targetKeypoints * 2);
#ifdef IBOM_HAVE_OPENCV_CUDA
    if (m_gpuActive) {
        try { m_gpuOrb = cv::cuda::ORB::create(m_targetKeypoints * 2); }
        catch (const cv::Exception&) { m_gpuActive = false; }
    }
#endif
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

void TrackingWorker::setStabilization(int model, double oneEuroMinCutoff,
                                      double oneEuroBeta)
{
    m_model            = static_cast<Model>(std::clamp(model, 0, 3));
    m_oneEuroMinCutoff = std::max(0.001, oneEuroMinCutoff);
    m_oneEuroBeta      = std::max(0.0, oneEuroBeta);
    m_cornerFilters.clear();  // recreated lazily with the new parameters
    spdlog::info("TrackingWorker stabilization: model={}, 1€ minCutoff={:.3f}, beta={:.3f}",
                 static_cast<int>(m_model), m_oneEuroMinCutoff, m_oneEuroBeta);
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

bool TrackingWorker::tryReserveFrameSlot()
{
    int cur = m_framesInFlight.load(std::memory_order_relaxed);
    while (cur < 2) {
        if (m_framesInFlight.compare_exchange_weak(cur, cur + 1,
                                                   std::memory_order_relaxed))
            return true;
    }
    return false;
}

cv::Mat TrackingWorker::buildBoardMask(const cv::Size& smallSize, float downscale,
                                       float marginScale) const
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

    std::vector<cv::Point> maskPoly;
    maskPoly.reserve(imgPoly.size());
    for (const auto& p : imgPoly) {
        cv::Point2f grown = centroid + (p - centroid) * marginScale;
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
    m_lastEmittedH       = cv::Mat();
    m_staticFrames       = 0;
    m_lowQualityFrames   = 0;
    m_pendingJumpH       = cv::Mat();
    m_cornerFilters.clear();
    m_prevGray           = cv::Mat();
    m_flowImg.clear();
    m_flowPcb.clear();
    m_flowFramesSinceDetect = 0;
    m_flowHealthy           = false;
    m_lastHealthyPoseMs     = 0;
    setState(State::Lost);
}

cv::Mat TrackingWorker::estimateModel(const std::vector<cv::Point2f>& src,
                                      const std::vector<cv::Point2f>& dst,
                                      int& inliers, double& reprojErr,
                                      cv::Mat* inlierMask)
{
    inliers = 0;
    reprojErr = 1e9;
    if (inlierMask) *inlierMask = cv::Mat();
    if (src.size() < 4 || src.size() != dst.size())
        return {};

    auto fitHomog = [&](int& inl, double& er, cv::Mat& mask) -> cv::Mat {
        cv::Mat H = cv::findHomography(src, dst, cv::USAC_MAGSAC,
                                       m_ransacThreshold, mask);
        if (H.empty() || H.rows != 3 || H.cols != 3) { inl = 0; er = 1e9; return {}; }
        er = medianReprojError(src, dst, H, mask, inl);
        return H;
    };
    auto fitAffine = [&](bool partial, int& inl, double& er, cv::Mat& mask) -> cv::Mat {
        cv::Mat A = partial
            ? cv::estimateAffinePartial2D(src, dst, mask, cv::RANSAC, m_ransacThreshold)
            : cv::estimateAffine2D(src, dst, mask, cv::RANSAC, m_ransacThreshold);
        if (A.empty() || A.rows != 2 || A.cols != 3) { inl = 0; er = 1e9; return {}; }
        cv::Mat A64; A.convertTo(A64, CV_64F);
        cv::Mat H = cv::Mat::eye(3, 3, CV_64F);
        A64.copyTo(H(cv::Rect(0, 0, 3, 2)));
        er = medianReprojError(src, dst, H, mask, inl);
        return H;
    };
    auto deliver = [&](const cv::Mat& H, int inl, double er, cv::Mat& mask) -> cv::Mat {
        inliers = inl;
        reprojErr = er;
        if (inlierMask) *inlierMask = std::move(mask);
        return H;
    };

    cv::Mat mask;
    switch (m_model) {
    case Model::Homography: {
        int inl = 0; double er = 1e9;
        cv::Mat H = fitHomog(inl, er, mask);
        return deliver(H, inl, er, mask);
    }
    case Model::Affine: {
        int inl = 0; double er = 1e9;
        cv::Mat A = fitAffine(false, inl, er, mask);
        return deliver(A, inl, er, mask);
    }
    case Model::Similarity: {
        int inl = 0; double er = 1e9;
        cv::Mat S = fitAffine(true, inl, er, mask);
        return deliver(S, inl, er, mask);
    }
    case Model::Auto:
    default: {
        int si = 0, hi = 0; double se = 1e9, he = 1e9;
        cv::Mat sMask, hMask;
        cv::Mat S = fitAffine(true, si, se, sMask);
        cv::Mat H = fitHomog(hi, he, hMask);
        // Keep the simpler (steadier) similarity unless the homography is
        // clearly better — notably lower error and at least as many inliers.
        const bool homogBetter = !H.empty() && he < se * 0.7 && hi >= si;
        if (!S.empty() && !homogBetter) return deliver(S, si, se, sMask);
        if (!H.empty())                 return deliver(H, hi, he, hMask);
        return deliver(S, si, se, sMask);
    }
    }
}

void TrackingWorker::bucketKeypoints(std::vector<cv::KeyPoint>& kp, cv::Mat& desc,
                                     const cv::Size& imgSize) const
{
    if (m_targetKeypoints <= 0 || static_cast<int>(kp.size()) <= m_targetKeypoints)
        return;
    if (imgSize.width <= 0 || imgSize.height <= 0) return;

    constexpr int kCols = 8, kRows = 6;
    const int cells = kCols * kRows;
    const int perCell = std::max(1, m_targetKeypoints / cells);

    // Strongest first.
    std::vector<int> order(kp.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return kp[a].response > kp[b].response; });

    std::vector<int> cellCount(cells, 0);
    std::vector<int> kept;
    kept.reserve(m_targetKeypoints);
    const double cw = static_cast<double>(imgSize.width)  / kCols;
    const double ch = static_cast<double>(imgSize.height) / kRows;

    for (int idx : order) {
        if (static_cast<int>(kept.size()) >= m_targetKeypoints) break;
        int cx = std::clamp(static_cast<int>(kp[idx].pt.x / cw), 0, kCols - 1);
        int cy = std::clamp(static_cast<int>(kp[idx].pt.y / ch), 0, kRows - 1);
        int c = cy * kCols + cx;
        if (cellCount[c] >= perCell) continue;  // cell full — spread out
        cellCount[c]++;
        kept.push_back(idx);
    }
    // Top up with the next strongest globally if cells didn't fill the target.
    if (static_cast<int>(kept.size()) < m_targetKeypoints) {
        std::vector<char> taken(kp.size(), 0);
        for (int i : kept) taken[i] = 1;
        for (int idx : order) {
            if (static_cast<int>(kept.size()) >= m_targetKeypoints) break;
            if (!taken[idx]) kept.push_back(idx);
        }
    }

    std::vector<cv::KeyPoint> newKp;
    newKp.reserve(kept.size());
    cv::Mat newDesc(static_cast<int>(kept.size()), desc.cols, desc.type());
    for (size_t i = 0; i < kept.size(); ++i) {
        newKp.push_back(kp[kept[i]]);
        desc.row(kept[i]).copyTo(newDesc.row(static_cast<int>(i)));
    }
    kp = std::move(newKp);
    desc = newDesc;
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

double TrackingWorker::projectedArea2(const cv::Mat& H) const
{
    if (H.empty() || m_pcbPolygon.size() < 3)
        return std::numeric_limits<double>::quiet_NaN();
    std::vector<cv::Point2f> pts;
    try {
        cv::perspectiveTransform(m_pcbPolygon, pts, H);
    } catch (const cv::Exception&) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double area2 = 0.0;
    for (size_t i = 0; i < pts.size(); ++i) {
        const cv::Point2f& a = pts[i];
        const cv::Point2f& b = pts[(i + 1) % pts.size()];
        if (!std::isfinite(a.x) || !std::isfinite(a.y))
            return std::numeric_limits<double>::quiet_NaN();
        area2 += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
    }
    return area2;
}

bool TrackingWorker::emitHomography(const cv::Mat& rawH, int inliers, double reprojErr)
{
    // Quality gate: too few inliers, or a median inlier error looser than the
    // RANSAC threshold (mushy fit), ⇒ don't trust this fit; hold the last good
    // pose rather than letting the overlay jump. A short hysteresis avoids
    // flickering the state on a single bad frame.
    if (inliers < m_minMatchCount || reprojErr > m_ransacThreshold) {
        if (++m_lowQualityFrames >= 3) setState(State::Lost);
        spdlog::debug("[track] HELD low-quality: inliers={}/{} reproj={:.2f}px lowQ={}",
                      inliers, m_minMatchCount, reprojErr, m_lowQualityFrames);
        return false;
    }
    m_lowQualityFrames = 0;

    // Geometric sanity gate: the projected board corners must be finite and
    // span a non-collapsed area with the same orientation as the reference
    // pose — a tracking update can never mirror the board. A degenerate fit
    // (quasi-collinear inlier set, glare) can pass the inlier gate yet project
    // to NaN/flipped corners, and NaN in particular slips through both the
    // static and jump gates below (every NaN comparison is false).
    if (m_pcbPolygon.size() >= 4) {
        const double newArea2 = projectedArea2(rawH);
        if (!std::isfinite(newArea2) || std::abs(newArea2) < 1.0) {
            spdlog::debug("[track] HELD insane pose: non-finite or collapsed corners");
            return false;
        }
        const cv::Mat& refPose = !m_lastEmittedH.empty() ? m_lastEmittedH
                                                         : m_baseHomography;
        const double refArea2 = projectedArea2(refPose);
        if (std::isfinite(refArea2) && refArea2 != 0.0 &&
            (newArea2 > 0.0) != (refArea2 > 0.0)) {
            spdlog::debug("[track] HELD insane pose: orientation flip");
            return false;
        }
    }

    const qint64 nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // Static-scene gate: if the new estimate barely differs from the last one
    // we emitted, the scene isn't really moving — emit nothing so the overlay
    // freezes instead of shimmering on keypoint noise. cornerDisp() returns -1
    // when it can't measure (no polygon yet) → fall through and emit.
    // A static hold is still a HEALTHY pose decision: refresh the recovery
    // clock so long static viewing keeps the jump gate fully armed.
    const double disp = cornerDisp(rawH, m_lastEmittedH);
    if (disp >= 0.0 && disp < m_staticThreshPx) {
        ++m_staticFrames;
        m_lastHealthyPoseMs = nowMs;
        spdlog::debug("[track] HELD static-scene: cornerDisp={:.2f}px < {:.2f} (staticFrames={})",
                      disp, m_staticThreshPx, m_staticFrames);
        return false;
    }
    m_staticFrames = 0;

    // Jump gate: a single-step displacement of a large fraction of the frame
    // is more often a degenerate fit than real motion (real motion at camera
    // rate moves a few dozen px per step). Require two consecutive concordant
    // estimates: genuine motion keeps producing nearby poses so it confirms on
    // the very next estimate; a one-off wild fit never gets confirmed.
    // Recovery bypass: after a long spell without any healthy pose (board
    // picked up / blur / occlusion), the last emitted pose is no longer a
    // meaningful prior — demanding continuity with it would keep holding
    // every sane fit forever once the board is put back down. Accept the
    // first sane fit directly and snap.
    const double jumpThresh = 0.15 * m_frameDiag;
    if (jumpThresh > 0.0 && disp > jumpThresh) {
        constexpr qint64 kRecoveryDroughtMs = 2000;
        const bool recovering = m_lastHealthyPoseMs > 0 &&
                                (nowMs - m_lastHealthyPoseMs) > kRecoveryDroughtMs;
        if (recovering) {
            m_pendingJumpH = cv::Mat();
            m_cornerFilters.clear();
            spdlog::info("[track] jump accepted (recovery after {} ms without a healthy pose)",
                         nowMs - m_lastHealthyPoseMs);
        } else {
            const double agree = cornerDisp(rawH, m_pendingJumpH);
            if (agree < 0.0 || agree > jumpThresh * 0.5) {
                m_pendingJumpH = rawH.clone();
                spdlog::debug("[track] HELD jump: cornerDisp={:.0f}px > {:.0f}px, awaiting confirmation",
                              disp, jumpThresh);
                return false;
            }
            // Confirmed: clear the corner filters so the overlay snaps to the
            // new pose instead of trailing across the jump.
            m_pendingJumpH = cv::Mat();
            m_cornerFilters.clear();
            spdlog::debug("[track] jump confirmed: cornerDisp={:.0f}px", disp);
        }
    } else if (!m_pendingJumpH.empty()) {
        m_pendingJumpH = cv::Mat();
    }

    const cv::Mat smoothed = smoothHomography(rawH);
    m_lastEmittedH = smoothed.clone();
    m_lastHealthyPoseMs = nowMs;
    emit homographyUpdated(smoothed, inliers, reprojErr);
    spdlog::debug("[track] EMIT: inliers={} reproj={:.2f}px cornerDisp={:.2f}px",
                  inliers, reprojErr, disp);
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

void TrackingWorker::detectFeatures(const cv::Mat& smallImg, const cv::Mat& mask,
                                    std::vector<cv::KeyPoint>& kp, cv::Mat& desc)
{
#ifdef IBOM_HAVE_OPENCV_CUDA
    if (m_gpuActive && m_gpuOrb) {
        try {
            cv::cuda::GpuMat gImg, gMask, gDesc;
            gImg.upload(smallImg);
            if (!mask.empty()) gMask.upload(mask);
            m_gpuOrb->detectAndCompute(gImg, gMask, kp, gDesc);
            gDesc.download(desc);  // match on CPU (descriptors are small)
            return;
        } catch (const cv::Exception& e) {
            spdlog::warn("TrackingWorker: GPU ORB failed ({}), CPU fallback", e.what());
            m_gpuActive = false;  // stop trying this session
        }
    }
#endif
    m_detector->detectAndCompute(smallImg, mask, kp, desc);
}

void TrackingWorker::seedFlowLandmarks(const std::vector<cv::KeyPoint>& kp,
                                       const cv::Mat& H, const cv::Mat& fullGray)
{
    m_flowImg.clear();
    m_flowPcb.clear();
    m_flowFramesSinceDetect = 0;
    m_flowHealthy = false;  // fresh seed — unproven until the first flow fit
    if (!m_opticalFlow || kp.empty() || H.empty() || fullGray.empty())
        return;

    cv::Mat Hinv;
    try { Hinv = H.inv(); } catch (const cv::Exception&) { return; }
    if (Hinv.empty()) return;

    std::vector<cv::Point2f> img;
    img.reserve(kp.size());
    for (const auto& k : kp) img.push_back(k.pt);  // already board-masked
    try {
        cv::perspectiveTransform(img, m_flowPcb, Hinv);  // image → PCB
    } catch (const cv::Exception&) {
        m_flowPcb.clear();
        return;
    }
    m_flowImg = std::move(img);
    m_prevGray = fullGray.clone();
}

bool TrackingWorker::runOpticalFlow(const cv::Mat& fullGray)
{
    if (m_flowImg.size() < static_cast<size_t>(m_minMatchCount) ||
        m_prevGray.empty() || m_prevGray.size() != fullGray.size()) {
        m_flowHealthy = false;
        return false;
    }

    const cv::Size lkWin(21, 21);
    const cv::TermCriteria lkCrit(
        cv::TermCriteria::EPS + cv::TermCriteria::MAX_ITER, 20, 0.03);

    std::vector<cv::Point2f> next, back;
    std::vector<uchar> status, statusB;
    std::vector<float> err, errB;
    try {
        cv::calcOpticalFlowPyrLK(m_prevGray, fullGray, m_flowImg, next,
                                 status, err, lkWin, 3, lkCrit);
        // Forward-backward consistency (MedianFlow-style): re-track the moved
        // points back into the previous frame. A landmark that silently slid
        // onto a neighboring feature — LK's classic failure mode, invisible to
        // the status/err outputs — won't land back on its start point.
        cv::calcOpticalFlowPyrLK(fullGray, m_prevGray, next, back,
                                 statusB, errB, lkWin, 3, lkCrit);
    } catch (const cv::Exception&) {
        m_flowHealthy = false;
        return false;
    }

    constexpr float kMaxFbErrPx = 0.5f;  // round-trip tolerance (px)
    std::vector<cv::Point2f> keptPcb, keptImg;
    keptPcb.reserve(next.size());
    keptImg.reserve(next.size());
    const float w = static_cast<float>(fullGray.cols);
    const float h = static_cast<float>(fullGray.rows);
    for (size_t i = 0; i < next.size(); ++i) {
        if (!status[i] || err[i] > 20.0f) continue;
        if (next[i].x < 0 || next[i].y < 0 || next[i].x >= w || next[i].y >= h) continue;
        if (i < statusB.size()) {
            if (!statusB[i]) continue;
            const cv::Point2f rt = back[i] - m_flowImg[i];
            if (rt.x * rt.x + rt.y * rt.y > kMaxFbErrPx * kMaxFbErrPx) continue;
        }
        keptPcb.push_back(m_flowPcb[i]);
        keptImg.push_back(next[i]);
    }
    if (keptImg.size() < static_cast<size_t>(m_minMatchCount)) {
        m_flowHealthy = false;
        return false;  // lost too many → let ORB re-acquire
    }

    int inliers = 0;
    double reprojErr = 0.0;
    cv::Mat inlierMask;
    cv::Mat H = estimateModel(keptPcb, keptImg, inliers, reprojErr, &inlierMask);
    if (H.empty() || inliers < m_minMatchCount) {
        m_flowHealthy = false;
        return false;
    }

    // Prune the RANSAC outliers from the landmark set. They used to be kept
    // ("the fit already rejected fliers") — but rejected-from-the-fit is not
    // removed-from-the-set: a drifted landmark survived until the next ORB
    // re-seed (up to m_flowRedetectInterval frames) and voted against the
    // right model on every one of them.
    if (inlierMask.rows == static_cast<int>(keptImg.size())) {
        std::vector<cv::Point2f> inPcb, inImg;
        inPcb.reserve(static_cast<size_t>(inliers));
        inImg.reserve(static_cast<size_t>(inliers));
        for (size_t i = 0; i < keptImg.size(); ++i) {
            if (inlierMask.at<uchar>(static_cast<int>(i))) {
                inPcb.push_back(keptPcb[i]);
                inImg.push_back(keptImg[i]);
            }
        }
        keptPcb = std::move(inPcb);
        keptImg = std::move(inImg);
    }

    m_flowPcb = std::move(keptPcb);
    m_flowImg = std::move(keptImg);
    m_prevGray = fullGray.clone();
    m_flowFramesSinceDetect++;

    // Attrition-triggered early re-seed: when FB-check + pruning have thinned
    // the set, route the next frame to ORB now instead of riding out the fixed
    // re-detect interval on a starving landmark set.
    if (m_flowImg.size() < static_cast<size_t>(2 * m_minMatchCount))
        m_flowFramesSinceDetect = m_flowRedetectInterval;

    m_lastHomography = H.clone();
    m_lostFrames  = 0;     // healthy fit — reset the mask-fallback miss streak
    m_flowHealthy = true;  // scheduled ORB refreshes may re-seed seamlessly (F8)
    setState(State::Locked);
    emitHomography(H, inliers, reprojErr);
    return true;
}

cv::Mat TrackingWorker::smoothHomography(const cv::Mat& rawH)
{
    if (rawH.empty() || m_pcbPolygon.size() < 4)
        return rawH;

    std::vector<cv::Point2f> newPts;
    try {
        cv::perspectiveTransform(m_pcbPolygon, newPts, rawH);
    } catch (const cv::Exception&) {
        return rawH;
    }
    const size_t n = newPts.size();

    // One 1€ filter per corner coordinate (x and y). The speed-adaptive cutoff
    // crushes jitter when the corners are nearly still and opens up instantly
    // on real motion — better jitter/lag trade-off than the old fixed EMA.
    if (m_cornerFilters.size() != n * 2) {
        m_cornerFilters.assign(
            n * 2, OneEuroFilter(m_oneEuroMinCutoff, m_oneEuroBeta));
    }
    // Feed the filters with the frame's CAPTURE time (F12) — using processing
    // time injected the worker's scheduling jitter into the filter's dt.
    // Falls back to "now" when the caller provided no timestamp.
    const double t = (m_frameTimeSec > 0.0)
        ? m_frameTimeSec
        : std::chrono::duration<double>(
              std::chrono::steady_clock::now().time_since_epoch()).count();

    std::vector<cv::Point2f> filt(n);
    for (size_t i = 0; i < n; ++i) {
        filt[i].x = static_cast<float>(m_cornerFilters[2 * i].filter(newPts[i].x, t));
        filt[i].y = static_cast<float>(m_cornerFilters[2 * i + 1].filter(newPts[i].y, t));
    }

    cv::Mat smoothed;
    try {
        smoothed = cv::findHomography(m_pcbPolygon, filt, 0 /* least-squares */);
    } catch (const cv::Exception&) {
        return rawH;
    }
    if (smoothed.empty() || smoothed.rows != 3 || smoothed.cols != 3)
        return rawH;
    return smoothed;
}

void TrackingWorker::setState(State s)
{
    if (s == m_state) return;
    m_state = s;
    emit trackingStateChanged(static_cast<int>(s));
}

void TrackingWorker::noteDetectMiss()
{
    ++m_lostFrames;
    spdlog::debug("[track] detect/match miss #{} (mask fallback widens at 3, drops at 6)",
                  m_lostFrames);
    if (m_lostFrames >= 4)
        setState(State::Lost);
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

void TrackingWorker::processFrame(ibom::camera::FrameRef frame, qint64 captureNs)
{
    // Release the backpressure slot reserved by the poster. Direct callers
    // (tests) never reserve — clamp at zero instead of drifting negative
    // (production posts always reserve first, so the clamp never races).
    if (m_framesInFlight.fetch_sub(1, std::memory_order_relaxed) <= 0)
        m_framesInFlight.store(0, std::memory_order_relaxed);

    if (!frame || frame->empty())
        return;

    // Freshness gate (F12): a frame captured long ago means the worker fell
    // behind — processing it would compute an already-outdated pose AND delay
    // fresher queued frames further. Drop it cheaply; the backlog drains in
    // microseconds and tracking resumes on the freshest frame. Belt to the
    // backpressure valve's suspenders (a stall between reserve and dispatch
    // can still age a queued frame). captureNs == 0 → no timestamp: keep.
    const qint64 nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (captureNs > 0 && nowNs - captureNs > 150'000'000LL) {
        spdlog::debug("[track] dropped stale frame ({} ms old)",
                      (nowNs - captureNs) / 1'000'000LL);
        return;
    }
    m_frameTimeSec = (captureNs > 0 ? captureNs : nowNs) * 1e-9;

    try {
        // Convert + downscale for speed. Keypoints are rescaled back to full
        // resolution so the emitted homography is in original image coords.
        cv::Mat gray;
        if (frame->channels() == 3)
            cv::cvtColor(*frame, gray, cv::COLOR_BGR2GRAY);
        else
            gray = *frame;

        // Frame diagonal feeds the jump gate's threshold (fraction of frame).
        m_frameDiag = std::hypot(static_cast<double>(gray.cols),
                                 static_cast<double>(gray.rows));

        // CLAHE photometric equalization → steadier keypoints under glare /
        // uneven lighting (D405). Applied on full-res gray so optical flow and
        // ORB both see the equalized image.
        if (m_useClahe && m_clahe) {
            try { m_clahe->apply(gray, gray); }
            catch (const cv::Exception&) { /* keep raw gray */ }
        }

        // Phase-3 optical-flow fast path: between periodic ORB re-detections,
        // Lucas-Kanade-track the landmarks frame to frame (sub-pixel, cheap,
        // smooth). Skipped in incremental mode and when it's time to refresh.
        if (m_opticalFlow && !m_incremental && m_hasReference &&
            m_flowFramesSinceDetect < m_flowRedetectInterval &&
            runOpticalFlow(gray)) {
            return;
        }

        // Throttle ONLY the expensive ORB detect/match path. The optical-flow
        // fast path above runs every frame, unthrottled, so live tracking stays
        // smooth at camera rate between periodic ORB re-detections; ORB itself
        // is paced by m_intervalMs. Previously the throttle sat at the top of
        // processFrame and capped optical flow to the same ~5 Hz, defeating it.
        const auto now = std::chrono::steady_clock::now();
        if (m_hasReference) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_lastProcessTime).count();
            if (elapsed < m_intervalMs) return;
        }
        m_lastProcessTime = now;

        cv::Mat small;
        if (m_downscale > 0.0f && m_downscale < 1.0f)
            cv::resize(gray, small, cv::Size(), m_downscale, m_downscale, cv::INTER_AREA);
        else
            small = gray;

        const float invScale = (m_downscale > 0.0f) ? (1.0f / m_downscale) : 1.0f;
        const float fwdScale = (m_downscale > 0.0f && m_downscale < 1.0f) ? m_downscale : 1.0f;

        // Restrict detection to the board's projected area (grown by a
        // margin) so a static background can't outvote the board's own
        // keypoints in RANSAC — see setBoardPolygon(). The mask is projected
        // through m_lastHomography, which only refreshes on success — so after
        // consecutive misses the mask itself is the prime suspect (board moved
        // out of it → permanent silent loss, ERREUR #51). Escalate: widen the
        // margin, then drop the mask entirely so the board can be re-acquired
        // anywhere in the frame. Escalation only applies once a reference
        // exists: the reference capture must stay board-masked, otherwise
        // background keypoints contaminate the reference set (ERREUR #35).
        constexpr int kWidenMaskAfter = 3;  // misses before growing the margin
        constexpr int kDropMaskAfter  = 6;  // misses before full-frame detection
        cv::Mat mask;
        if (!m_hasReference || m_lostFrames < kWidenMaskAfter)
            mask = buildBoardMask(small.size(), fwdScale, 1.6f);
        else if (m_lostFrames < kDropMaskAfter)
            mask = buildBoardMask(small.size(), fwdScale, 2.5f);
        // else: empty mask → detect over the whole frame to re-acquire

        std::vector<cv::KeyPoint> kp;
        cv::Mat desc;
        detectFeatures(small, mask, kp, desc);

        // Rescale keypoint positions back to original-resolution coords.
        for (auto& k : kp) {
            k.pt.x *= invScale;
            k.pt.y *= invScale;
        }

        // Spatially distribute keypoints (full-res coords) so the fit is
        // well-conditioned across the whole board, not dominated by a cluster.
        bucketKeypoints(kp, desc, gray.size());

        // Sub-pixel refinement on the full-res gray image — cuts the
        // quantization jitter ORB carries (worse after the ×downscale
        // upscaling). Done before matching so descriptors keep their indices.
        refineKeypointsSubPix(gray, kp);

        if (m_incremental)
            processIncremental(kp, desc);
        else
            processReference(kp, desc, gray);

    } catch (const cv::Exception& e) {
        emit trackingError(QString::fromStdString(e.what()));
        spdlog::warn("TrackingWorker: cv::Exception: {}", e.what());
    }
}

void TrackingWorker::processReference(const std::vector<cv::KeyPoint>& kp,
                                      const cv::Mat& desc, const cv::Mat& fullGray)
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

    if (desc.empty() || kp.size() < static_cast<size_t>(m_minMatchCount)) {
        noteDetectMiss();
        return;
    }

    std::vector<cv::Point2f> srcPts, dstPts;
    matchPoints(m_refDescriptors, m_refKeypoints, desc, kp, srcPts, dstPts);
    if (srcPts.size() < static_cast<size_t>(m_minMatchCount)) {
        noteDetectMiss();
        return;
    }

    if (m_baseHomography.empty())
        return;  // configuration gap, not a tracking miss

    int inliers = 0;
    double reprojErr = 0.0;
    cv::Mat frameH = estimateModel(srcPts, dstPts, inliers, reprojErr);
    if (frameH.empty() || frameH.rows != 3 || frameH.cols != 3) {
        noteDetectMiss();
        return;
    }

    cv::Mat combined = frameH * m_baseHomography;

    const bool healthy = inliers >= m_minMatchCount;
    if (healthy) {
        m_lostFrames = 0;
        // Only a healthy fit may steer the detection mask (m_lastHomography)
        // or seed optical-flow landmarks — a degenerate low-inlier estimate
        // would point both at a wrong area and cement the loss.
        m_lastHomography = combined.clone();
        // Seamless re-seed (F8): when this ORB pass is just the periodic
        // refresh of a HEALTHY flow track, re-seed the landmarks but do not
        // emit the ORB pose — the flow re-emits from the new seed on the very
        // next frame. Emitting both back to back injected a micro-jump at
        // every refresh (ORB and flow disagree by a sub-pixel step with a
        // different noise character). On recovery (flow was not healthy) the
        // ORB pose is emitted immediately as before.
        const bool seamlessReseed = m_opticalFlow && m_flowHealthy;
        if (m_opticalFlow)
            seedFlowLandmarks(kp, combined, fullGray);
        // The reference path never reported Locked before (only the flow and
        // incremental paths did), leaving the UI badge stuck on "LOST" while
        // pure-ORB tracking worked fine.
        setState(State::Locked);
        if (seamlessReseed) {
            spdlog::debug("[track] seamless ORB re-seed ({} landmarks), emit deferred to flow",
                          m_flowImg.size());
            return;
        }
    } else {
        noteDetectMiss();
    }

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
            int rInliers = 0;
            double rErr = 0.0;
            cv::Mat refH = estimateModel(rSrc, rDst, rInliers, rErr);
            if (!refH.empty() && refH.rows == 3 && refH.cols == 3) {
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
        // recover; noteDetectMiss flags Lost after a few consecutive failures
        // and drives the mask fallback.
        noteDetectMiss();
        return;
    }

    // Fit the frame→frame delta with the configured motion model (F10). This
    // used to be a direct 8-DOF findHomography regardless of m_model — but
    // deltas get COMPOSED into m_cumulativeH every frame, so their spurious
    // perspective noise compounds and inflates microscope drift. With
    // Auto/Similarity the composed noise stays rigid.
    int inliers = 0;
    double reprojErr = 0.0;
    cv::Mat deltaH = estimateModel(srcPts, dstPts, inliers, reprojErr);
    if (deltaH.empty() || deltaH.rows != 3 || deltaH.cols != 3) {
        noteDetectMiss();
        return;
    }

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
