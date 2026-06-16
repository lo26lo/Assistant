#pragma once

#include <QObject>
#include <QString>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>

#include <chrono>
#include <memory>
#include <vector>

#include "camera/CameraCapture.h"  // for ibom::camera::FrameRef

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

    /// Tracking lock state for the microscope incremental mode. Mirrored to
    /// the UI as a "Locked / Drifting / Lost — re-anchor" badge (§4 of
    /// docs/MICROSCOPE_PLACEMENT_PLAN.md). Reported as int over the signal so
    /// no Qt metatype registration is needed.
    enum class State { Locked = 0, Drifting = 1, Lost = 2 };

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

    /// Set the PCB→reference_image homography. Emitted back combined with
    /// the per-frame delta after each successful tracking update.
    void setBaseHomography(cv::Mat h);

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

    /// Emitted when the incremental tracking state changes (only fires in
    /// incremental mode). Value is a TrackingWorker::State cast to int.
    void trackingStateChanged(int state);

    /// Emitted on non-fatal tracking errors (e.g. cv::Exception).
    void trackingError(QString message);

private:
    /// Reference-frame matching (original behaviour): match current vs one
    /// fixed reference, emit frameH * base.
    void processReference(const std::vector<cv::KeyPoint>& kp, const cv::Mat& desc);
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

    cv::Ptr<cv::Feature2D>         m_detector;
    cv::Ptr<cv::DescriptorMatcher> m_matcher;

    cv::Mat                   m_refDescriptors;
    std::vector<cv::KeyPoint> m_refKeypoints;
    cv::Mat                   m_baseHomography;

    int    m_minMatchCount      = 10;
    double m_loweRatio          = 0.75;
    double m_ransacThreshold    = 5.0;
    int    m_intervalMs         = 200;
    float  m_downscale          = 0.5f;

    // Incremental (frame→frame) tracking state.
    bool                      m_incremental      = false;
    double                    m_reanchorDriftPx  = 40.0;
    cv::Mat                   m_prevDescriptors;          // previous frame
    std::vector<cv::KeyPoint> m_prevKeypoints;
    cv::Mat                   m_cumulativeH;              // PCB → current image
    double                    m_accumulatedDrift = 0.0;   // Σ per-frame reproj err
    int                       m_lostFrames       = 0;     // consecutive match failures
    State                     m_state            = State::Lost;

    std::chrono::steady_clock::time_point m_lastProcessTime;
    bool m_hasReference = false;
};

} // namespace ibom::overlay
