#pragma once

#include <QObject>
#include <QMetaType>
#include <QSize>
#include <opencv2/core.hpp>
#include <memory>
#include <atomic>
#include <thread>

namespace ibom::camera {

class FrameBuffer;

/// Shared, immutable frame view. Pixel buffer is reference-counted — copying
/// the shared_ptr does not copy pixels. Multiple consumers (GUI, tracking
/// worker, calibration) can hold a reference concurrently without clones.
using FrameRef = std::shared_ptr<const cv::Mat>;

/**
 * @brief Captures frames from a USB camera (microscope).
 *
 * Runs capture in a dedicated thread for minimal latency.
 * Frames are pushed into a FrameBuffer for consumption by
 * the rendering / AI pipeline.
 */
class CameraCapture : public QObject {
    Q_OBJECT

public:
    explicit CameraCapture(int deviceIndex = 0, QObject* parent = nullptr);
    ~CameraCapture() override;

    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    /// Start capturing frames.
    bool start();

    /// Stop capture and release device.
    void stop();

    /// Whether the camera is currently capturing.
    bool isCapturing() const { return m_capturing.load(); }

    /// Get the latest frame (thread-safe, zero-copy shared view).
    /// Returns an empty FrameRef (null) if no frame has been captured yet.
    FrameRef latestFrame() const;

    /// Get the shared frame buffer.
    FrameBuffer& frameBuffer();

    // --- Settings ---
    void setDeviceIndex(int index);
    void setResolution(int width, int height);
    void setFps(int fps);
    QSize resolution() const;

    /// List available camera devices.
    static std::vector<std::string> listDevices();

signals:
    /// Emitted when a new frame is ready. The FrameRef is shared — consumers
    /// may hold it but MUST NOT mutate the underlying cv::Mat.
    void frameReady(ibom::camera::FrameRef frame);

    /// Emitted on capture error.
    void captureError(const QString& message);

    /// Emitted when capture starts/stops.
    void captureStateChanged(bool capturing);

private:
    void captureLoop();

    int m_deviceIndex;
    int m_width = 1920;
    int m_height = 1080;
    int m_fps = 30;

    std::atomic<bool>           m_capturing{false};
    std::unique_ptr<std::thread> m_thread;
    std::unique_ptr<FrameBuffer> m_frameBuffer;

    mutable std::mutex m_frameMutex;
    FrameRef           m_latestFrame;
};

} // namespace ibom::camera

Q_DECLARE_METATYPE(ibom::camera::FrameRef)
