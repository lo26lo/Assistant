#pragma once

#include "ICameraSource.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include <string>

namespace ibom::camera {

/**
 * @brief Captures frames from a USB camera (microscope) via V4L2/UVC.
 *
 * Runs capture in a dedicated thread for minimal latency.
 * Each frame is published as a FrameRef (shared_ptr<const cv::Mat>) — the
 * pixel buffer is shared zero-copy with all downstream consumers via
 * Qt signals and latestFrame().
 */
class CameraCapture : public ICameraSource {
    Q_OBJECT

public:
    explicit CameraCapture(int deviceIndex = 0, QObject* parent = nullptr);
    ~CameraCapture() override;

    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    bool start() override;
    void stop() override;
    bool isCapturing() const override { return m_capturing.load(); }
    FrameRef latestFrame() const override;

    // --- Settings ---
    void setDeviceIndex(int index) override;
    void setResolution(int width, int height) override;
    void setFps(int fps) override;
    QSize resolution() const override;

    /// Enable NVIDIA hardware MJPG decode via GStreamer (nvv4l2decoder) on
    /// Jetson. When enabled, captureLoop() tries the GStreamer pipeline first
    /// and falls back to CPU V4L2 automatically if it cannot open.
    void setHardwareDecode(bool enabled) override { m_hwDecode = enabled; }
    bool hardwareDecode() const { return m_hwDecode; }

    /// List available V4L2 capture devices as (index, friendly name) pairs.
    /// The index is the real /dev/video<N> number (NOT a list position) — it
    /// can have gaps, so callers must use it directly for setDeviceIndex().
    /// Only true video-capture nodes are returned (metadata / output-only
    /// nodes are filtered out via VIDIOC_QUERYCAP on Linux).
    static std::vector<std::pair<int, std::string>> listDevices();

private:
    void captureLoop();

    int m_deviceIndex;
    int m_width = 1920;
    int m_height = 1080;
    int m_fps = 30;
    bool m_hwDecode = true;

    std::atomic<bool>           m_capturing{false};
    std::unique_ptr<std::thread> m_thread;

    mutable std::mutex m_frameMutex;
    FrameRef           m_latestFrame;
};

} // namespace ibom::camera
