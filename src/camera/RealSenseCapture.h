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

    /// Point the auto-exposure metering at a rectangle of the depth/stereo
    /// sensor (pixels in the current stream resolution), like the Viewer's AE
    /// ROI. Pass an empty/zero rect to meter the central 50%. Applied live on
    /// the device; returns false if the sensor doesn't support ROI.
    bool setAutoExposureRoi(int x, int y, int w, int h);

    /// Ask the capture thread to export the next computed point cloud to a
    /// binary PLY (vertices + color), like the Viewer's "Export". Fires
    /// plyExportFinished() when done.
    void requestPlyExport(const std::string& path);

    /// Ask the capture thread to run the D4xx on-chip self-calibration (no
    /// target needed). Blocks the capture loop for a few seconds; fires
    /// onChipCalibrationFinished() with the health score. Experimental.
    void requestOnChipCalibration() { m_calibPending.store(true); }

    /// Advanced-mode JSON preset I/O (rs2::serializable_device), like the
    /// Viewer's load/save. Applied/queried live on the device. Return false on
    /// failure (no device, unsupported, or file error).
    bool saveJsonPreset(const std::string& path) const;
    bool loadJsonPreset(const std::string& path);

    /// Set the advanced-mode disparity shift (depth table). Higher values move
    /// the measurable Z window closer — useful for close-range PCB inspection
    /// (Intel "tuning depth cameras" guidance). Returns false if advanced mode
    /// is unavailable/disabled. Typical range 0–256.
    bool setDisparityShift(int shift);
    /// Current disparity shift, or -1 if unavailable.
    int  disparityShift() const;

    /// Set the advanced-mode "Second Peak Threshold" (depth control group).
    /// Lowering it toward 0 (default 325) significantly reduces depth
    /// fluctuation on a static scene — Intel/MartyG's recommendation for fixed
    /// precise measurement (issue #10682). Returns false if advanced mode is
    /// unavailable/disabled. Typical range 0–1023.
    bool setSecondPeakThreshold(int value);
    /// Current Second Peak Threshold, or -1 if unavailable.
    int  secondPeakThreshold() const;

    /// Record all streams to a rosbag (.bag) starting at the next (re)start,
    /// like the Viewer's record button. Empty string disables recording.
    /// Takes effect after stop()+start().
    void setRecordFile(const std::string& path);
    std::string recordFile() const;

    /// Enable/disable the depth stream (color is always on — it is the primary
    /// feed). Takes effect on the next (re)start. When off, no depth/3D/cloud.
    void setDepthStreamEnabled(bool on) { m_depthStreamEnabled.store(on); }
    bool depthStreamEnabled() const { return m_depthStreamEnabled.load(); }

    /// Measured per-stream frame rate (frames/s), refreshed ~1 Hz on the
    /// capture thread. 0 until the first second elapses. For a live health
    /// readout like the Viewer's stream stats.
    double colorFps() const { return m_colorFps.load(); }
    double depthFps() const { return m_depthFps.load(); }

    /// Enable/disable computing the 3D point cloud on the capture thread.
    /// Off by default (avoids the per-frame rs2::pointcloud cost when the 3D
    /// view is not shown). When on, pointCloudReady() fires each frame.
    void setEmitPointCloud(bool on) { m_emitCloud.store(on); }

    /// Decimation magnitude applied to the depth that feeds the 3D point cloud
    /// and PLY export ONLY (never the overlay/depth-view depth, whose 1:1 color
    /// alignment must stay intact). 0/1 = off (full resolution); 2–8 downsample
    /// the cloud by that factor — cleaner, less noisy scan, fewer points, per
    /// Intel's canonical 3D-scan filter order. Takes effect immediately.
    void setCloudDecimation(int magnitude) { m_cloudDecimation.store(magnitude); }
    int  cloudDecimation() const { return m_cloudDecimation.load(); }

    /// Enable/disable a histogram-equalized colorized depth image
    /// (rs2::colorizer, like the RealSense Viewer's Depth panel). Off by
    /// default. When on, colorizedDepthReady() fires each frame.
    void setEmitColorizedDepth(bool on) { m_emitColorDepth.store(on); }

    /// Enable/disable emitting the left infrared camera (Y8 grayscale, same
    /// resolution as depth). Off by default. When on, infraredReady() fires
    /// each frame. Useful for reflective surfaces (solder, bare metal) where
    /// the color camera clips — per Intel's "Tuning depth cameras" guide.
    /// Requires depth stream active; takes effect without restart.
    void setEmitInfrared(bool on) { m_emitIR.store(on); }
    bool emitInfrared() const { return m_emitIR.load(); }

signals:
    /// Emitted alongside frameReady when depth is available: a CV_16UC1 depth
    /// map in millimetres, aligned to the color frame. Shared (no pixel copy).
    void depthFrameReady(ibom::camera::DepthFrameRef depth);

    /// Emitted when point-cloud emission is enabled: a colored 3D cloud built
    /// via rs2::pointcloud (vertices in metres, camera frame). Shared.
    void pointCloudReady(ibom::camera::PointCloudRef cloud);

    /// Emitted when colorized-depth emission is enabled: an RGB image of the
    /// depth, histogram-equalized by rs2::colorizer (aligned to color). Shared.
    void colorizedDepthReady(ibom::camera::FrameRef rgb);

    /// Emitted when IR emission is enabled: left IR camera (Y8→BGR, grayscale).
    /// Not aligned to color (same sensor as depth — near-perfect overlap).
    void infraredReady(ibom::camera::FrameRef ir);

    /// Result of a requestPlyExport(): ok + the path written (or error text).
    void plyExportFinished(bool ok, const QString& pathOrError);

    /// Result of a requestOnChipCalibration(): ok, health score (closer to 0
    /// is better), and a human-readable message.
    void onChipCalibrationFinished(bool ok, float health, const QString& message);

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
    std::atomic<int>    m_cloudDecimation{0};     // decimation magnitude for cloud/PLY (0/1=off)
    std::atomic<bool>   m_emitColorDepth{false};  // colorize depth via rs2::colorizer
    std::atomic<bool>   m_emitIR{false};          // emit left IR camera (Y8) when true
    std::atomic<bool>   m_calibPending{false};    // run on-chip self-calibration
    std::atomic<bool>   m_depthStreamEnabled{true};
    std::atomic<double> m_colorFps{0.0};
    std::atomic<double> m_depthFps{0.0};

    mutable std::mutex  m_plyMutex;               // guards the pending PLY path
    std::string         m_pendingPly;             // export path; empty = none

    mutable std::mutex  m_recordMutex;            // guards the record path
    std::string         m_recordFile;             // .bag to record to; empty = off

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
