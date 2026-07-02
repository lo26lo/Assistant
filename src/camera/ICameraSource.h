#pragma once

#include <QObject>
#include <QMetaType>
#include <QSize>
#include <opencv2/core.hpp>
#include <memory>
#include <vector>
#include <string>

namespace ibom::camera {

/// Shared, immutable frame view. Pixel buffer is reference-counted — copying
/// the shared_ptr does not copy pixels. Multiple consumers (GUI, tracking
/// worker, calibration) can hold a reference concurrently without clones.
using FrameRef = std::shared_ptr<const cv::Mat>;

/// Shared, immutable depth frame. CV_16UC1, one millimetre per unit, aligned
/// to the color stream so depth[y,x] corresponds to the color pixel [y,x].
/// 0 = no/invalid depth at that pixel. Only RealSense-class sources emit it.
using DepthFrameRef = std::shared_ptr<const cv::Mat>;

/**
 * @brief Abstract camera source.
 *
 * Common contract shared by every capture backend so the rest of the app
 * (CameraView, TrackingWorker, OverlayRenderer, DatasetCreator) is agnostic
 * to whether frames come from a V4L2/UVC microscope or an Intel RealSense.
 *
 * Implementations run capture on their own thread and publish each frame as a
 * FrameRef via frameReady(), stamped with a monotonic capture timestamp
 * (steady_clock ns) taken on the capture thread. The timestamp is the
 * foundation for latency accounting downstream (stale-frame dropping in the
 * tracking worker, real capture-dt for the 1€ filter — F12 of
 * docs/LIVE_TRACKING_ANALYSE_2026-07.md).
 */
class ICameraSource : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~ICameraSource() override = default;

    /// Start capturing frames on a dedicated thread.
    virtual bool start() = 0;

    /// Stop capture and release the device.
    virtual void stop() = 0;

    /// Whether the source is currently capturing.
    virtual bool isCapturing() const = 0;

    /// Latest frame (thread-safe, zero-copy shared view). Null until the first
    /// frame has been captured.
    virtual FrameRef latestFrame() const = 0;

    // --- Settings (best-effort; backends clamp to supported modes) ---
    virtual void  setDeviceIndex(int index) = 0;
    virtual void  setResolution(int width, int height) = 0;
    virtual void  setFps(int fps) = 0;
    virtual QSize resolution() const = 0;

    /// Hardware-decode hint (only meaningful for the V4L2/MJPG backend).
    /// Default no-op so backends that do not decode compressed streams
    /// (e.g. RealSense, which delivers raw frames) can ignore it.
    virtual void setHardwareDecode(bool /*enabled*/) {}

    /// Whether this source can also produce a depth stream. Webcams return
    /// false; RealSense returns true. Lets Application opt into depth wiring
    /// without a hard dependency on a concrete type.
    virtual bool hasDepth() const { return false; }

signals:
    /// Emitted when a new frame is ready. The FrameRef is shared — consumers
    /// may hold it but MUST NOT mutate the underlying cv::Mat.
    /// @param captureNs Monotonic capture time (std::chrono::steady_clock,
    ///        nanoseconds since epoch), stamped on the capture thread right
    ///        after the frame was grabbed. Same clock as the consumers use, so
    ///        `now - captureNs` measures end-to-end pipeline latency.
    void frameReady(ibom::camera::FrameRef frame, qint64 captureNs);

    /// Emitted on a capture error (device gone, open failed, …).
    void captureError(const QString& message);

    /// Emitted when capture starts/stops.
    void captureStateChanged(bool capturing);
};

} // namespace ibom::camera

Q_DECLARE_METATYPE(ibom::camera::FrameRef)
// DepthFrameRef aliases the same underlying type as FrameRef; declaring the
// metatype again would redefine it, so it is intentionally NOT redeclared.
