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
    m_lastEmittedH       = cv::Mat();
    m_staticFrames       = 0;
    m_lowQualityFrames   = 0;
    m_cornerFilters.clear();
    m_prevGray           = cv::Mat();
    m_flowImg.clear();
    m_flowPcb.clear();
    m_flowFramesSinceDetect = 0;
    setState(State::Lost);
}

cv::Mat TrackingWorker::estimateModel(const std::vector<cv::Point2f>& src,
                                      const std::vector<cv::Point2f>& dst,
                                      int& inliers, double& reprojErr)
{
    inliers = 0;
    reprojErr = 1e9;
    if (src.size() < 4 || src.size() != dst.size())
        return {};

    auto fitHomog = [&](int& inl, double& er) -> cv::Mat {
        cv::Mat mask;
        cv::Mat H = cv::findHomography(src, dst, cv::USAC_MAGSAC,
                                       m_ransacThreshold, mask);
        if (H.empty() || H.rows != 3 || H.cols != 3) { inl = 0; er = 1e9; return {}; }
        er = medianReprojError(src, dst, H, mask, inl);
        return H;
    };
    auto fitAffine = [&](bool partial, int& inl, double& er) -> cv::Mat {
        cv::Mat mask;
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

    switch (m_model) {
    case Model::Homography: return fitHomog(inliers, reprojErr);
    case Model::Affine:     return fitAffine(false, inliers, reprojErr);
    case Model::Similarity: return fitAffine(true,  inliers, reprojErr);
    case Model::Auto:
    default: {
        int si = 0, hi = 0; double se = 1e9, he = 1e9;
        cv::Mat S = fitAffine(true, si, se);
        cv::Mat H = fitHomog(hi, he);
        // Keep the simpler (steadier) similarity unless the homography is
        // clearly better — notably lower error and at least as many inliers.
        const bool homogBetter = !H.empty() && he < se * 0.7 && hi >= si;
        if (!S.empty() && !homogBetter) { inliers = si; reprojErr = se; return S; }
        if (!H.empty())                 { inliers = hi; reprojErr = he; return H; }
        inliers = si; reprojErr = se; return S;
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

bool TrackingWorker::emitHomography(const cv::Mat& rawH, int inliers, double reprojErr)
{
    // Quality gate: too few inliers ⇒ don't trust this fit, hold the last good
    // pose rather than letting the overlay jump. A short hysteresis avoids
    // flickering the state on a single bad frame.
    if (inliers < m_minMatchCount) {
        if (++m_lowQualityFrames >= 3) setState(State::Lost);
        spdlog::debug("[track] HELD low-quality: inliers={}/{} reproj={:.2f}px lowQ={}",
                      inliers, m_minMatchCount, reprojErr, m_lowQualityFrames);
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
        spdlog::debug("[track] HELD static-scene: cornerDisp={:.2f}px < {:.2f} (staticFrames={})",
                      disp, m_staticThreshPx, m_staticFrames);
        return false;
    }
    m_staticFrames = 0;

    const cv::Mat smoothed = smoothHomography(rawH);
    m_lastEmittedH = smoothed.clone();
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
        m_prevGray.empty() || m_prevGray.size() != fullGray.size())
        return false;

    std::vector<cv::Point2f> next;
    std::vector<uchar> status;
    std::vector<float> err;
    try {
        cv::calcOpticalFlowPyrLK(
            m_prevGray, fullGray, m_flowImg, next, status, err,
            cv::Size(21, 21), 3,
            cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::MAX_ITER, 20, 0.03));
    } catch (const cv::Exception&) {
        return false;
    }

    std::vector<cv::Point2f> keptPcb, keptImg;
    keptPcb.reserve(next.size());
    keptImg.reserve(next.size());
    const float w = static_cast<float>(fullGray.cols);
    const float h = static_cast<float>(fullGray.rows);
    for (size_t i = 0; i < next.size(); ++i) {
        if (!status[i] || err[i] > 20.0f) continue;
        if (next[i].x < 0 || next[i].y < 0 || next[i].x >= w || next[i].y >= h) continue;
        keptPcb.push_back(m_flowPcb[i]);
        keptImg.push_back(next[i]);
    }
    if (keptImg.size() < static_cast<size_t>(m_minMatchCount))
        return false;  // lost too many → let ORB re-acquire

    int inliers = 0;
    double reprojErr = 0.0;
    cv::Mat H = estimateModel(keptPcb, keptImg, inliers, reprojErr);
    if (H.empty() || inliers < m_minMatchCount)
        return false;

    // Keep only the inlier-consistent landmarks alive (the fit already
    // rejected fliers; we keep the tracked set for the next frame).
    m_flowPcb = std::move(keptPcb);
    m_flowImg = std::move(keptImg);
    m_prevGray = fullGray.clone();
    m_flowFramesSinceDetect++;

    m_lastHomography = H.clone();
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
    const double t = std::chrono::duration<double>(
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

    try {
        // Convert + downscale for speed. Keypoints are rescaled back to full
        // resolution so the emitted homography is in original image coords.
        cv::Mat gray;
        if (frame->channels() == 3)
            cv::cvtColor(*frame, gray, cv::COLOR_BGR2GRAY);
        else
            gray = *frame;

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
        // keypoints in RANSAC — see setBoardPolygon().
        cv::Mat mask = buildBoardMask(small.size(), fwdScale);

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

    if (desc.empty() || kp.size() < static_cast<size_t>(m_minMatchCount))
        return;

    std::vector<cv::Point2f> srcPts, dstPts;
    matchPoints(m_refDescriptors, m_refKeypoints, desc, kp, srcPts, dstPts);
    if (srcPts.size() < static_cast<size_t>(m_minMatchCount))
        return;

    if (m_baseHomography.empty())
        return;

    int inliers = 0;
    double reprojErr = 0.0;
    cv::Mat frameH = estimateModel(srcPts, dstPts, inliers, reprojErr);
    if (frameH.empty() || frameH.rows != 3 || frameH.cols != 3)
        return;

    cv::Mat combined = frameH * m_baseHomography;
    m_lastHomography = combined.clone();

    // Seed optical-flow landmarks from this fresh ORB fit so the LK fast path
    // can take over for the next m_flowRedetectInterval frames.
    if (m_opticalFlow)
        seedFlowLandmarks(kp, combined, fullGray);

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
