#pragma once

#include <QObject>
#include <QString>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>   // cv::CLAHE
#include <opencv2/opencv_modules.hpp>
#ifdef IBOM_HAVE_OPENCV_CUDA
#  include <opencv2/cudafeatures2d.hpp>
#  include <opencv2/cudaimgproc.hpp>
#endif

#include <chrono>
#include <memory>
#include <vector>

#include "camera/CameraCapture.h"  // for ibom::camera::FrameRef
#include "overlay/OneEuroFilter.h"

namespace ibom::overlay {

/**
 * @brief ORB-based live tracking computed on a dedicated worker thread.
 *
 * The GUI thread posts frames via processFrame() (queued connection).
 * The worker downscales, runs ORB + BFMatcher + RANSAC, and emits the
 * composed homography. Heavy CV work is off the UI thread so the camera
 * render loop stays smooth.
 *
 * All slots are invoked via queued connections — the object is moved to
 * a QThread by its owner.
 */
class TrackingWorker : public QObject {
    Q_OBJECT

public:
    explicit TrackingWorker(QObject* parent = nullptr);
    ~TrackingWorker() override = default;

    /// Tracking lock state, reported by all modes (reference, optical-flow
    /// and microscope incremental; Drifting is incremental-only). Mirrored to
    /// the UI as a "Locked / Drifting / Lost — re-anchor" badge (§4 of
    /// docs/MICROSCOPE_PLACEMENT_PLAN.md). Reported as int over the signal so
    /// no Qt metatype registration is needed.
    enum class State { Locked = 0, Drifting = 1, Lost = 2 };

    /// Motion model fitted for the PCB→image estimate (Phase 2). A flat board
    /// seen ~perpendicular only needs a similarity (4 DOF); fitting the full
    /// 8-DOF homography then lets noise leak into spurious perspective, which
    /// reads as overlay wobble at the edges. `Auto` fits both and keeps the
    /// simpler one when it explains the data comparably well.
    enum class Model { Auto = 0, Similarity = 1, Affine = 2, Homography = 3 };

public slots:
    /// Configure detector + matching parameters. Recreates ORB detector
    /// with the requested keypoint count.
    /// @param loweRatio Lowe's ratio test threshold (typical 0.7–0.8).
    /// @param downscale Image downscale before ORB, clamped to [0.1, 1.0].
    void configure(int orbKeypoints,
                   int minMatchCount,
                   double loweRatio,
                   double ransacThreshold,
                   int intervalMs,
                   float downscale);

    /// Enable incremental (frame→frame) tracking instead of matching every
    /// frame against one fixed reference. Used at high magnification where the
    /// narrow field of view leaves too little overlap with a distant reference.
    /// @param driftThresholdPx accumulated reprojection error past which the
    ///        state flips to Drifting (invites a re-anchor).
    void setIncrementalMode(bool enabled, double driftThresholdPx);

    /// Configure Phase-2 stabilization (docs/LIVE_TRACKING_PLAN.md):
    /// @param model         motion model fitted (Model cast to int).
    /// @param oneEuroMinCutoff baseline cutoff (Hz) of the 1€ corner filter.
    /// @param oneEuroBeta   speed coupling of the 1€ corner filter.
    void setStabilization(int model, double oneEuroMinCutoff, double oneEuroBeta);

    /// Configure Phase-3 advanced tracking (docs/LIVE_TRACKING_PLAN.md):
    /// @param clahe        CLAHE photometric pre-equalization before ORB
    ///                     (steadier keypoints under glare / uneven light).
    /// @param opticalFlow  hybrid Lucas-Kanade tracking — between periodic ORB
    ///                     re-detections, sub-pixel-track the landmarks frame to
    ///                     frame (smoother + cheaper than ORB every frame).
    /// @param gpuMode      0=off, 1=auto (use GPU if available), 2=force.
    void setAdvanced(bool clahe, bool opticalFlow, int gpuMode);

    /// Enable hybrid drift correction (beta). Only affects incremental mode.
    /// When on, the worker keeps the original anchor keyframe captured at
    /// bootstrap and, whenever that view is recognizable in the current frame
    /// (enough confident inliers), snaps the cumulative homography back to the
    /// drift-free reference estimate (refH * base) and zeroes the accumulated
    /// drift. Between such corrections it falls back to frame→frame
    /// composition, so it stays responsive over large motion while shedding
    /// long-term drift the moment the anchor reappears.
    void setHybridCorrection(bool enabled);

    /// Set the PCB→reference_image homography. Emitted back combined with
    /// the per-frame delta after each successful tracking update.
    void setBaseHomography(cv::Mat h);

    /// Set the board outline in PCB coordinates (board bbox corners are
    /// enough). Used to mask ORB detection to the board area only — without
    /// this, keypoints from a static background (table, cables) can
    /// outnumber the board's own keypoints when the board is moved by hand
    /// under a fixed camera, and RANSAC locks onto the *background* (near-
    /// identity transform) instead of the board's actual motion. Call once
    /// after an iBOM is loaded; persists across resetReference()/re-anchors.
    void setBoardPolygon(std::vector<cv::Point2f> pcbPoints);

    /// Drop the current reference frame; the next processFrame() becomes
    /// the new reference. Also resets incremental drift accumulation.
    void resetReference();

    /// Process one incoming frame. Throttled by intervalMs — frames
    /// arriving too soon are dropped to keep the worker responsive.
    void processFrame(ibom::camera::FrameRef frame);

signals:
    /// Emitted with the composed homography (PCB → current image) and the
    /// quality of the estimate: RANSAC inlier count and the median
    /// reprojection error (px) of those inliers. Consumers gate on these
    /// to decide whether the homography is trustworthy (e.g. DatasetCreator).
    void homographyUpdated(cv::Mat combinedHomography, int inliers, double reprojErrPx);

    /// Emitted once when a reference frame has been captured.
    void referenceCaptured(int keypointCount);

    /// Emitted when the tracking state changes, in every mode: Locked on a
    /// healthy fit (reference, flow or incremental path), Lost after a few
    /// consecutive detect/match misses, Drifting from incremental drift
    /// accumulation. Value is a TrackingWorker::State cast to int.
    void trackingStateChanged(int state);

    /// Emitted on non-fatal tracking errors (e.g. cv::Exception).
    void trackingError(QString message);

private:
    /// Reference-frame matching (original behaviour): match current vs one
    /// fixed reference, emit frameH * base. `fullGray` is the full-resolution
    /// gray image, used to seed optical-flow landmarks on a successful fit.
    void processReference(const std::vector<cv::KeyPoint>& kp, const cv::Mat& desc,
                          const cv::Mat& fullGray);

    /// Detect ORB keypoints + descriptors on the (downscaled) image, on GPU
    /// when active else CPU. Keypoints are returned in `smallImg` coordinates
    /// (caller rescales). `mask` is the board-area mask in `smallImg` coords.
    void detectFeatures(const cv::Mat& smallImg, const cv::Mat& mask,
                        std::vector<cv::KeyPoint>& kp, cv::Mat& desc);

    /// Hybrid optical-flow fast path: Lucas-Kanade-track the current landmark
    /// set (m_flowImg) from the previous frame into `fullGray`, refit PCB→image
    /// from the surviving (m_flowPcb ↔ tracked) pairs and emit. Returns false
    /// when it can't run / lost too many points (caller falls back to ORB).
    bool runOpticalFlow(const cv::Mat& fullGray);

    /// (Re)seed optical-flow landmarks from a fresh ORB homography H (PCB→image)
    /// and the current board keypoints: back-projects them through H⁻¹ to PCB.
    void seedFlowLandmarks(const std::vector<cv::KeyPoint>& kp, const cv::Mat& H,
                           const cv::Mat& fullGray);

    /// Incremental matching: match current vs previous frame, compose deltas.
    void processIncremental(const std::vector<cv::KeyPoint>& kp, const cv::Mat& desc);
    /// Lowe-ratio match srcDesc→dstDesc into corresponding point lists.
    void matchPoints(const cv::Mat& srcDesc, const std::vector<cv::KeyPoint>& srcKp,
                     const cv::Mat& dstDesc, const std::vector<cv::KeyPoint>& dstKp,
                     std::vector<cv::Point2f>& srcPts,
                     std::vector<cv::Point2f>& dstPts);
    /// Median reprojection error (px) of the inliers of H mapping src→dst.
    static double medianReprojError(const std::vector<cv::Point2f>& src,
                                    const std::vector<cv::Point2f>& dst,
                                    const cv::Mat& H, const cv::Mat& inlierMask,
                                    int& inlierCount);
    void setState(State s);

    /// Build an ORB detection mask covering the board, projected through the
    /// last known homography into `smallSize` (the downscaled detection
    /// frame), grown by `marginScale` around its centroid to tolerate the
    /// motion since that estimate. Returns an empty Mat if the board polygon
    /// or a homography estimate isn't available yet (caller falls back to
    /// unmasked detection).
    cv::Mat buildBoardMask(const cv::Size& smallSize, float downscale,
                           float marginScale) const;

    /// Record one detect/match failure (m_lostFrames++). Drives the
    /// escalating mask fallback in processFrame() — the mask is built from
    /// m_lastHomography, which only refreshes on success, so consecutive
    /// misses mean the mask itself is likely stale (ERREUR #51): widen it,
    /// then drop it, or the loss is permanent. Flips the state to Lost after
    /// a few consecutive misses.
    void noteDetectMiss();

    /// Temporally smooth a freshly-fit homography to suppress visible overlay
    /// "vibration" on a static scene. Raw per-frame ORB/RANSAC fits wobble by
    /// a few pixels even with zero physical motion (keypoint localization
    /// noise); without damping that wobble is fully visible in the overlay.
    /// Projects m_pcbPolygon through the previous smoothed estimate and the
    /// new raw one, exponentially blends the two point sets, then refits a
    /// homography from the blend — so small noise is damped while a genuine
    /// large displacement (real motion) still snaps through immediately
    /// (the blend factor saturates toward the new points past a px threshold).
    /// Falls back to returning `rawH` unchanged if no board polygon is set.
    cv::Mat smoothHomography(const cv::Mat& rawH);

    /// Max displacement (px) of the board-polygon corners projected through
    /// `a` vs `b`. Returns -1 when it can't be computed (no polygon / empty
    /// matrix / cv error). Shared by the static-scene gate and smoothing.
    double cornerDisp(const cv::Mat& a, const cv::Mat& b) const;

    /// Single exit point for a freshly-fit PCB→image homography: applies the
    /// quality gate (too few inliers → hold the last good pose), the static-
    /// scene gate (estimate barely moved → emit nothing so the overlay stops
    /// shimmering), then temporal smoothing, and finally emits. Returns true
    /// when an update was actually emitted.
    bool emitHomography(const cv::Mat& rawH, int inliers, double reprojErr);

    /// Fit the PCB→image (or ref→cur) point correspondence with the configured
    /// motion model and return it as a 3×3 matrix. Fills inliers + median
    /// reprojection error. `Auto` fits a similarity and a homography and keeps
    /// the similarity unless the homography is clearly better. Returns an empty
    /// Mat on failure. When `inlierMask` is non-null it receives the chosen
    /// fit's Nx1 uchar inlier mask (empty on failure) — used by the optical-flow
    /// path to prune outlier landmarks from its tracked set.
    cv::Mat estimateModel(const std::vector<cv::Point2f>& src,
                          const std::vector<cv::Point2f>& dst,
                          int& inliers, double& reprojErr,
                          cv::Mat* inlierMask = nullptr);

    /// Signed double-area (shoelace ×2, px²) of the board polygon projected
    /// through H. NaN when it can't be computed or any corner is non-finite.
    /// Feeds the geometric sanity gate in emitHomography(): a degenerate fit
    /// can pass the inlier-count gate yet project the board to NaN/collapsed/
    /// mirrored corners — and NaN slips through every numeric comparison.
    double projectedArea2(const cv::Mat& H) const;

    /// Spatially distribute keypoints: keep the strongest per grid cell so the
    /// fit is well-conditioned across the whole board instead of dominated by a
    /// dense cluster. Subsets `kp` and the matching `desc` rows in place. No-op
    /// when there are fewer keypoints than the target.
    void bucketKeypoints(std::vector<cv::KeyPoint>& kp, cv::Mat& desc,
                         const cv::Size& imgSize) const;

    /// Refine keypoint positions to sub-pixel on the full-resolution gray
    /// image (cv::cornerSubPix) — reduces the quantization jitter that ORB
    /// keypoints carry, especially after the ×downscale upscaling. No-op on
    /// failure (out-of-range points etc.).
    void refineKeypointsSubPix(const cv::Mat& fullGray, std::vector<cv::KeyPoint>& kp);

    cv::Ptr<cv::Feature2D>         m_detector;
    cv::Ptr<cv::DescriptorMatcher> m_matcher;

    cv::Mat                   m_refDescriptors;
    std::vector<cv::KeyPoint> m_refKeypoints;
    cv::Mat                   m_baseHomography;

    // Board outline in PCB coords (bbox corners) + best current estimate of
    // the PCB->image homography, used only to mask ORB detection (see
    // setBoardPolygon doc comment). Updated after every successful estimate
    // so the mask tracks the board as it moves.
    std::vector<cv::Point2f> m_pcbPolygon;
    cv::Mat                  m_lastHomography;

    // Phase-1 stabilization state (see docs/LIVE_TRACKING_PLAN.md).
    cv::Mat m_lastEmittedH;            // last pose actually sent to consumers
    int     m_staticFrames     = 0;    // consecutive frames judged "not moving"
    int     m_lowQualityFrames = 0;    // consecutive frames below the inlier gate
    double  m_staticThreshPx   = 0.8;  // corner motion below this ⇒ scene static
    // Anti-jump gate (lot B, F4): a pose displaced by a large fraction of the
    // frame in one step is held here until a second, concordant estimate
    // confirms it — one-off degenerate fits never get confirmed.
    cv::Mat m_pendingJumpH;
    double  m_frameDiag = 0.0;         // diagonal (px) of the last processed frame

    // Phase-2 stabilization (docs/LIVE_TRACKING_PLAN.md).
    Model   m_model            = Model::Homography;  // safe default = legacy
    double  m_oneEuroMinCutoff = 1.0;  // Hz
    double  m_oneEuroBeta      = 0.02;
    std::vector<OneEuroFilter> m_cornerFilters;  // 2 per board corner (x,y)
    int     m_targetKeypoints  = 0;    // bucketing target (0 = disabled)

    // Phase-3 advanced tracking (docs/LIVE_TRACKING_PLAN.md).
    bool    m_useClahe    = false;
    bool    m_opticalFlow = false;
    int     m_gpuMode     = 1;          // 0 off / 1 auto / 2 force
    bool    m_gpuActive   = false;      // resolved at configure time
    cv::Ptr<cv::CLAHE> m_clahe;
    // Optical-flow landmark set: PCB coords ↔ current image positions, tracked
    // frame-to-frame by Lucas-Kanade; refreshed by ORB every N frames.
    cv::Mat                  m_prevGray;          // previous full-res gray (LK)
    std::vector<cv::Point2f> m_flowPcb;           // landmark PCB coords
    std::vector<cv::Point2f> m_flowImg;           // landmark image positions
    int     m_flowFramesSinceDetect = 0;
    int     m_flowRedetectInterval  = 30;         // force an ORB refresh cadence
#ifdef IBOM_HAVE_OPENCV_CUDA
    cv::Ptr<cv::cuda::ORB>   m_gpuOrb;
#endif

    int    m_minMatchCount      = 10;
    double m_loweRatio          = 0.75;
    double m_ransacThreshold    = 5.0;
    int    m_intervalMs         = 200;
    float  m_downscale          = 0.5f;

    // Incremental (frame→frame) tracking state.
    bool                      m_incremental      = false;
    bool                      m_hybrid           = true;  // beta drift correction
    double                    m_reanchorDriftPx  = 40.0;
    cv::Mat                   m_prevDescriptors;          // previous frame
    std::vector<cv::KeyPoint> m_prevKeypoints;
    cv::Mat                   m_cumulativeH;              // PCB → current image
    double                    m_accumulatedDrift = 0.0;   // Σ per-frame reproj err
    // Consecutive detect/match failures, counted in BOTH tracking modes (not
    // just incremental): drives the Lost state and the escalating board-mask
    // fallback (see noteDetectMiss / buildBoardMask). Reset on any success.
    int                       m_lostFrames       = 0;
    State                     m_state            = State::Lost;

    std::chrono::steady_clock::time_point m_lastProcessTime;
    bool m_hasReference = false;
};

} // namespace ibom::overlay
