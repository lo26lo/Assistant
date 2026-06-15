#pragma once

#include "ICameraSource.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace ibom::camera {

/**
 * @brief Captures frames from an Intel RealSense camera (e.g. D405) via
 *        librealsense2.
 *
 * Phase 1: color stream only — published as a FrameRef (BGR cv::Mat), exactly
 * like CameraCapture, so all downstream consumers work unchanged. The depth
 * stream (hasDepth() == true) is wired in Phase 2.
 *
 * Capture runs on a dedicated thread. The librealsense pipeline recycles its
 * frame buffers, so each published frame is cloned into an owned cv::Mat.
 */
class RealSenseCapture : public ICameraSource {
    Q_OBJECT

public:
    explicit RealSenseCapture(QObject* parent = nullptr);
    ~RealSenseCapture() override;

    RealSenseCapture(const RealSenseCapture&) = delete;
    RealSenseCapture& operator=(const RealSenseCapture&) = delete;

    bool start() override;
    void stop() override;
    bool isCapturing() const override { return m_capturing.load(); }
    FrameRef latestFrame() const override;

    void setDeviceIndex(int index) override;
    void setResolution(int width, int height) override;
    void setFps(int fps) override;
    QSize resolution() const override;

    bool hasDepth() const override { return true; }

    /// List connected RealSense devices ("Intel RealSense D405 <serial>").
    /// Returns an empty vector when none are present or librealsense fails.
    static std::vector<std::string> listDevices();

private:
    void captureLoop();

    int m_deviceIndex = 0;
    int m_width  = 1280;   // D405 native color/depth width
    int m_height = 720;
    int m_fps    = 30;

    std::atomic<bool>            m_capturing{false};
    std::unique_ptr<std::thread> m_thread;

    mutable std::mutex m_frameMutex;
    FrameRef           m_latestFrame;
};

} // namespace ibom::camera
