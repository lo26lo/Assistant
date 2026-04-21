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

    /// Set the PCB→reference_image homography. Emitted back combined with
    /// the per-frame delta after each successful tracking update.
    void setBaseHomography(cv::Mat h);

    /// Drop the current reference frame; the next processFrame() becomes
    /// the new reference.
    void resetReference();

    /// Process one incoming frame. Throttled by intervalMs — frames
    /// arriving too soon are dropped to keep the worker responsive.
    void processFrame(ibom::camera::FrameRef frame);

signals:
    /// Emitted with the composed homography (PCB → current image).
    void homographyUpdated(cv::Mat combinedHomography);

    /// Emitted once when a reference frame has been captured.
    void referenceCaptured(int keypointCount);

    /// Emitted on non-fatal tracking errors (e.g. cv::Exception).
    void trackingError(QString message);

private:
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

    std::chrono::steady_clock::time_point m_lastProcessTime;
    bool m_hasReference = false;
};

} // namespace ibom::overlay
