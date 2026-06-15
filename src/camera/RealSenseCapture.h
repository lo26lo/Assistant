#pragma once

#include "ICameraSource.h"
#include "PointCloudData.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
#include <utility>

namespace rs2 { class device; }

namespace ibom::camera {

/// Depth post-processing filter chain (spatial, temporal, threshold, hole
/// filling). Defined in the .cpp to keep librealsense out of this header.
struct FilterChain;

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
    /// Discrete named values (value → label) when the option is an enum
    /// (e.g. Visual Preset). Empty for plain numeric options → render as combo.
    std::vector<std::pair<float, std::string>> enumValues;
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
    /// Remaining factory intrinsics of the color stream (0 until started):
    /// vertical focal length and principal point, both in pixels.
    double colorFy()  const { return m_colorFy.load(); }
    double colorPpx() const { return m_colorPpx.load(); }
    double colorPpy() const { return m_colorPpy.load(); }

    /// List connected RealSense devices ("Intel RealSense D405 <serial>").
    /// Returns an empty vector when none are present or librealsense fails.
    static std::vector<std::string> listDevices();

    /// Enumerate every supported option of every sensor on the live device,
    /// PLUS the depth post-processing filters, with range/current value and SDK
    /// help text. Empty until the pipeline has started. Thread-safe.
    /// Filter groups use ownerId >= kFilterBase; each filter exposes a synthetic
    /// "Enabled" toggle at optionId == kEnableOption.
    std::vector<RsControl> listControls() const;

    /// Set one option. `ownerId` is a sensor index, or >= kFilterBase for a
    /// filter. Returns false on invalid target or if librealsense rejects it.
    bool setControl(int ownerId, int optionId, float value);

    static constexpr int kFilterBase   = 1000;  // ownerId offset for filters
    static constexpr int kEnableOption = -1;     // synthetic per-filter on/off

    /// Request a Visual Preset value to be applied by the capture thread right
    /// after the next (re)start — the reliable moment, once the device is live.
    /// < 0 = leave the preset untouched. Used when applying resolution profiles.
    void setPendingVisualPreset(float value) { m_pendingPreset.store(value); }

    /// Enable/disable computing the 3D point cloud on the capture thread.
    /// Off by default (avoids the per-frame rs2::pointcloud cost when the 3D
    /// view is not shown). When on, pointCloudReady() fires each frame.
    void setEmitPointCloud(bool on) { m_emitCloud.store(on); }

signals:
    /// Emitted alongside frameReady when depth is available: a CV_16UC1 depth
    /// map in millimetres, aligned to the color frame. Shared (no pixel copy).
    void depthFrameReady(ibom::camera::DepthFrameRef depth);

    /// Emitted when point-cloud emission is enabled: a colored 3D cloud built
    /// via rs2::pointcloud (vertices in metres, camera frame). Shared.
    void pointCloudReady(ibom::camera::PointCloudRef cloud);

private:
    void captureLoop();

    int m_deviceIndex = 0;
    int m_width  = 848;    // 848x480 = optimal depth precision on D4xx (per Intel)
    int m_height = 480;
    int m_fps    = 30;
    std::atomic<double> m_colorFx{0.0};
    std::atomic<double> m_colorFy{0.0};
    std::atomic<double> m_colorPpx{0.0};
    std::atomic<double> m_colorPpy{0.0};
    std::atomic<float>  m_pendingPreset{-1.0f};  // Visual Preset to apply on start
    std::atomic<bool>   m_emitCloud{false};       // compute rs2::pointcloud when true

    std::atomic<bool>            m_capturing{false};
    std::unique_ptr<std::thread> m_thread;

    mutable std::mutex m_frameMutex;
    FrameRef           m_latestFrame;

    // Live device handle for option get/set, published once the pipeline
    // starts. rs2::device is forward-declared; the dtor lives in the .cpp.
    mutable std::mutex            m_deviceMutex;
    std::unique_ptr<rs2::device>  m_device;

    // Depth post-processing filters (pimpl). Created in the ctor, applied on
    // the capture thread, options get/set from the GUI thread (own mutex).
    std::unique_ptr<FilterChain>  m_filters;
};

} // namespace ibom::camera
