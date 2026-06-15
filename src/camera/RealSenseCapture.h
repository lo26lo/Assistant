#pragma once

#include "ICameraSource.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <memory>

namespace rs2 { class device; }

namespace ibom::camera {

/// Plain descriptor of one librealsense sensor option, queried live from the
/// device. Mirrors what the RealSense Viewer shows — `description` comes from
/// the SDK (rs2::sensor::get_option_description) and is meant for a tooltip.
struct RsControl {
    int         sensorIndex = 0;     // index into device.query_sensors()
    std::string sensorName;          // e.g. "Stereo Module", "RGB Camera"
    int         optionId = 0;        // rs2_option value
    std::string name;                // rs2_option_to_string
    std::string description;         // SDK help text → UI tooltip
    float       min = 0, max = 0, step = 0, def = 0, current = 0;
    bool        isBool = false;      // range [0,1] step 1 → checkbox
    bool        readOnly = false;
};

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

    /// Color stream horizontal focal length in pixels (from factory
    /// intrinsics). 0 until the pipeline has started. Used to derive px/mm
    /// from depth: pixelsPerMm = colorFx / distance_mm.
    double colorFx() const { return m_colorFx.load(); }

    /// List connected RealSense devices ("Intel RealSense D405 <serial>").
    /// Returns an empty vector when none are present or librealsense fails.
    static std::vector<std::string> listDevices();

    /// Enumerate every supported option of every sensor on the live device,
    /// with its range/current value and SDK help text. Empty until the
    /// pipeline has started. Thread-safe (GUI thread may call while streaming).
    std::vector<RsControl> listControls() const;

    /// Set one sensor option. Returns false if the device isn't live, the
    /// sensor index is out of range, or librealsense rejects the value.
    bool setControl(int sensorIndex, int optionId, float value);

signals:
    /// Emitted alongside frameReady when depth is available: a CV_16UC1 depth
    /// map in millimetres, aligned to the color frame. Shared (no pixel copy).
    void depthFrameReady(ibom::camera::DepthFrameRef depth);

private:
    void captureLoop();

    int m_deviceIndex = 0;
    int m_width  = 1280;   // D405 native color/depth width
    int m_height = 720;
    int m_fps    = 30;
    std::atomic<double> m_colorFx{0.0};

    std::atomic<bool>            m_capturing{false};
    std::unique_ptr<std::thread> m_thread;

    mutable std::mutex m_frameMutex;
    FrameRef           m_latestFrame;

    // Live device handle for option get/set, published once the pipeline
    // starts. rs2::device is forward-declared; the dtor lives in the .cpp.
    mutable std::mutex            m_deviceMutex;
    std::unique_ptr<rs2::device>  m_device;
};

} // namespace ibom::camera
