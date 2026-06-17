#include "RealSenseCapture.h"

#include <librealsense2/rs.hpp>
#include <librealsense2/rs_advanced_mode.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iterator>

namespace ibom::camera {

/// Resolution-preserving depth filters for the overlay/depth-view path: the
/// four toggleable filters never change the depth size, so the color↔depth
/// alignment stays 1:1. (Decimation is also held here but is applied ONLY in
/// the point-cloud/PLY path — see `decimation` below.) Each toggleable filter
/// has a runtime on/off flag. The mutex guards process() on the capture thread
/// vs option get/set from the GUI.
struct FilterChain {
    rs2::spatial_filter      spatial;
    rs2::temporal_filter     temporal;
    rs2::threshold_filter    threshold;
    rs2::hole_filling_filter holeFill;
    // Decimation: downsamples depth (reduces noise + point count). NOT part of
    // the user-toggleable chain above and NOT applied to the depth that feeds
    // the iBOM overlay (would break the 1:1 color↔depth alignment). Used only
    // in the point-cloud path, where the SDK re-maps texture via UV regardless
    // of depth resolution — Intel's canonical 3D-scan order puts it first.
    rs2::decimation_filter   decimation;
    // Disparity-domain transforms: spatial+temporal are most effective applied
    // in the disparity domain (Intel post-processing whitepaper / rs-post-
    // processing example). depth→disparity (true) before, disparity→depth
    // (false) after. Not user-exposed; bracket spatial/temporal automatically.
    rs2::disparity_transform depthToDisparity{true};
    rs2::disparity_transform disparityToDepth{false};

    std::mutex mutex;
    // Defaults tuned for a static PCB rig: spatial+temporal on (cf. #10682).
    std::array<bool, 4> enabled{ true, true, false, false };  // spat, temp, thr, hole

    enum Idx { Spatial = 0, Temporal = 1, Threshold = 2, HoleFill = 3, Count = 4 };

    rs2::filter& at(int i) {
        switch (i) {
            case Temporal:  return temporal;
            case Threshold: return threshold;
            case HoleFill:  return holeFill;
            default:        return spatial;
        }
    }
    static const char* name(int i) {
        switch (i) {
            case Temporal:  return "Temporal Filter";
            case Threshold: return "Threshold Filter";
            case HoleFill:  return "Hole Filling Filter";
            default:        return "Spatial Filter";
        }
    }

    /// Apply the enabled filters in the canonical librealsense order:
    /// Threshold → [depth→disparity → Spatial → Temporal → disparity→depth]
    /// → Hole Filling. The caller must hold `mutex`.
    rs2::frame processOrdered(rs2::frame f) {
        if (enabled[Threshold]) f = threshold.process(f);
        const bool useDisparity = enabled[Spatial] || enabled[Temporal];
        if (useDisparity)      f = depthToDisparity.process(f);
        if (enabled[Spatial])  f = spatial.process(f);
        if (enabled[Temporal]) f = temporal.process(f);
        if (useDisparity)      f = disparityToDepth.process(f);
        if (enabled[HoleFill]) f = holeFill.process(f);
        return f;
    }
};

RealSenseCapture::RealSenseCapture(QObject* parent)
    : ICameraSource(parent)
    , m_filters(std::make_unique<FilterChain>())
{
    // Static PCB rig: lower the temporal filter's smooth alpha to 0.1 (vs the
    // SDK default 0.4). Intel/MartyG's explicit recommendation for precise
    // measurement on a *static* object — it significantly stabilizes depth
    // fluctuation (issue #10682). The trade-off (depth updates more slowly
    // after motion) is irrelevant for a fixed board, and the user can still
    // override the value live in the Post-Processing options panel.
    try {
        m_filters->temporal.set_option(RS2_OPTION_FILTER_SMOOTH_ALPHA, 0.1f);
    } catch (const rs2::error&) { /* keep SDK default */ }
}

RealSenseCapture::~RealSenseCapture()
{
    stop();
}

bool RealSenseCapture::start()
{
    if (m_capturing.load()) {
        spdlog::warn("RealSense already capturing.");
        return true;
    }
    // Join any finished-but-joinable thread from a prior self-exit before
    // reassigning m_thread (destroying a joinable std::thread → terminate).
    if (m_thread) {
        if (m_thread->joinable())
            m_thread->join();
        m_thread.reset();
    }
    spdlog::info("Starting RealSense capture (device {})...", m_deviceIndex);
    m_capturing.store(true);
    m_thread = std::make_unique<std::thread>(&RealSenseCapture::captureLoop, this);
    emit captureStateChanged(true);
    return true;
}

void RealSenseCapture::stop()
{
    // Always join/reset the thread if present — even if the capture loop already
    // cleared m_capturing on its own (error exit). Leaving a joinable thread
    // would std::terminate on destruction or on the next start().
    const bool wasCapturing = m_capturing.exchange(false);
    if (wasCapturing)
        spdlog::info("Stopping RealSense capture...");
    if (m_thread && m_thread->joinable())
        m_thread->join();
    m_thread.reset();
    // Only signal the transition if we were the ones stopping it; if the loop
    // exited by itself it already emitted captureStateChanged(false).
    if (wasCapturing) {
        emit captureStateChanged(false);
        spdlog::info("RealSense capture stopped.");
    }
}

FrameRef RealSenseCapture::latestFrame() const
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    return m_latestFrame;
}

void RealSenseCapture::setDeviceIndex(int index) { m_deviceIndex = index; }

void RealSenseCapture::setResolution(int width, int height)
{
    m_width = width;
    m_height = height;
}

void RealSenseCapture::setFps(int fps) { m_fps = fps; }

QSize RealSenseCapture::resolution() const { return QSize(m_width, m_height); }

std::vector<std::string> RealSenseCapture::listDevices()
{
    std::vector<std::string> devices;
    try {
        rs2::context ctx;
        for (auto&& dev : ctx.query_devices()) {
            std::string name = dev.supports(RS2_CAMERA_INFO_NAME)
                ? dev.get_info(RS2_CAMERA_INFO_NAME) : "RealSense";
            std::string serial = dev.supports(RS2_CAMERA_INFO_SERIAL_NUMBER)
                ? dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER) : "";
            // USB type descriptor ("2.1", "3.2", …) — the link the D405 actually
            // negotiated. A D405 that enumerated at 2.1 has far less depth
            // bandwidth, so surfacing this in the UI is a real diagnostic.
            std::string usb = dev.supports(RS2_CAMERA_INFO_USB_TYPE_DESCRIPTOR)
                ? dev.get_info(RS2_CAMERA_INFO_USB_TYPE_DESCRIPTOR) : "";
            std::string label = serial.empty() ? name : (name + " " + serial);
            if (!usb.empty())
                label += " — USB " + usb;
            devices.push_back(std::move(label));
        }
    } catch (const rs2::error& e) {
        // "failed to set power state" and similar happen when the device is
        // busy streaming (e.g. Settings re-enumerates while the camera runs).
        // Benign and transient — the existing device list stays valid.
        spdlog::debug("RealSense enumeration skipped (device busy?): {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("RealSense enumeration error: {}", e.what());
    }
    return devices;
}

bool RealSenseCapture::setAutoExposureRoi(int x, int y, int w, int h)
{
    std::lock_guard<std::mutex> lk(m_deviceMutex);
    if (!m_device) return false;
    try {
        // Default to the central 50% when no explicit rectangle is given.
        if (w <= 0 || h <= 0) {
            x = m_width / 4;  y = m_height / 4;
            w = m_width / 2;  h = m_height / 2;
        }
        for (auto&& s : m_device->query_sensors()) {
            if (auto roi = s.as<rs2::roi_sensor>()) {
                rs2::region_of_interest r{ x, y, x + w, y + h };
                roi.set_region_of_interest(r);
                spdlog::info("RealSense AE ROI set to [{},{} {}x{}]", x, y, w, h);
                return true;
            }
        }
    } catch (const rs2::error& e) {
        spdlog::warn("RealSense set AE ROI failed: {}", e.what());
    }
    return false;
}

void RealSenseCapture::requestPlyExport(const std::string& path)
{
    std::lock_guard<std::mutex> lk(m_plyMutex);
    m_pendingPly = path;
}

bool RealSenseCapture::saveJsonPreset(const std::string& path) const
{
    std::lock_guard<std::mutex> lk(m_deviceMutex);
    if (!m_device) return false;
    try {
        auto ser = m_device->as<rs2::serializable_device>();
        if (!ser) return false;
        const std::string json = ser.serialize_json();
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        f << json;
        spdlog::info("RealSense preset saved: {}", path);
        return true;
    } catch (const rs2::error& e) {
        spdlog::warn("RealSense save preset failed: {}", e.what());
        return false;
    }
}

bool RealSenseCapture::loadJsonPreset(const std::string& path)
{
    std::lock_guard<std::mutex> lk(m_deviceMutex);
    if (!m_device) return false;
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        const std::string json((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
        auto ser = m_device->as<rs2::serializable_device>();
        if (!ser) return false;
        ser.load_json(json);
        spdlog::info("RealSense preset loaded: {}", path);
        return true;
    } catch (const rs2::error& e) {
        spdlog::warn("RealSense load preset failed: {}", e.what());
        return false;
    }
}

bool RealSenseCapture::setDisparityShift(int shift)
{
    std::lock_guard<std::mutex> lk(m_deviceMutex);
    if (!m_device) return false;
    try {
        auto adv = m_device->as<rs400::advanced_mode>();
        if (!adv) return false;
        if (!adv.is_enabled()) {
            // Enabling advanced mode resets the device (it reconnects), which
            // would disrupt streaming. Require it to be pre-enabled and report.
            spdlog::warn("RealSense advanced mode disabled — cannot set "
                         "disparity shift. Enable it once with rs-enumerate or "
                         "the Viewer, then retry.");
            return false;
        }
        STDepthTableControl table = adv.get_depth_table();
        table.disparityShift = shift;
        adv.set_depth_table(table);
        spdlog::info("RealSense disparity shift set to {}", shift);
        return true;
    } catch (const rs2::error& e) {
        spdlog::warn("RealSense set disparity shift failed: {}", e.what());
        return false;
    }
}

int RealSenseCapture::disparityShift() const
{
    std::lock_guard<std::mutex> lk(m_deviceMutex);
    if (!m_device) return -1;
    try {
        auto adv = m_device->as<rs400::advanced_mode>();
        if (!adv || !adv.is_enabled()) return -1;
        return adv.get_depth_table().disparityShift;
    } catch (const rs2::error&) {
        return -1;
    }
}

bool RealSenseCapture::setSecondPeakThreshold(int value)
{
    std::lock_guard<std::mutex> lk(m_deviceMutex);
    if (!m_device) return false;
    try {
        auto adv = m_device->as<rs400::advanced_mode>();
        if (!adv) return false;
        if (!adv.is_enabled()) {
            // Enabling advanced mode resets the device — require it pre-enabled
            // (same guard as setDisparityShift).
            spdlog::warn("RealSense advanced mode disabled — cannot set "
                         "second peak threshold. Enable it once with rs-enumerate "
                         "or the Viewer, then retry.");
            return false;
        }
        STDepthControlGroup dc = adv.get_depth_control();
        dc.deepSeaSecondPeakThreshold = static_cast<float>(value);
        adv.set_depth_control(dc);
        spdlog::info("RealSense second peak threshold set to {}", value);
        return true;
    } catch (const rs2::error& e) {
        spdlog::warn("RealSense set second peak threshold failed: {}", e.what());
        return false;
    }
}

int RealSenseCapture::secondPeakThreshold() const
{
    std::lock_guard<std::mutex> lk(m_deviceMutex);
    if (!m_device) return -1;
    try {
        auto adv = m_device->as<rs400::advanced_mode>();
        if (!adv || !adv.is_enabled()) return -1;
        return static_cast<int>(adv.get_depth_control().deepSeaSecondPeakThreshold);
    } catch (const rs2::error&) {
        return -1;
    }
}

void RealSenseCapture::setRecordFile(const std::string& path)
{
    std::lock_guard<std::mutex> lk(m_recordMutex);
    m_recordFile = path;
}

std::string RealSenseCapture::recordFile() const
{
    std::lock_guard<std::mutex> lk(m_recordMutex);
    return m_recordFile;
}

void RealSenseCapture::captureLoop()
{
    rs2::pipeline pipe;
    rs2::config cfg;

    // Bind to the selected device by serial when several are connected, so the
    // index in the UI maps to a stable physical camera.
    try {
        rs2::context ctx;
        auto devs = ctx.query_devices();
        if (static_cast<int>(devs.size()) > m_deviceIndex && m_deviceIndex >= 0) {
            auto dev = devs[m_deviceIndex];
            if (dev.supports(RS2_CAMERA_INFO_SERIAL_NUMBER))
                cfg.enable_device(dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER));
        }
    } catch (const rs2::error& e) {
        spdlog::warn("RealSense device select failed: {} — using default", e.what());
    }

    // Color (BGR8 to match the app) + depth (Z16). Depth is later aligned to
    // the color frame so depth[y,x] maps to color pixel [y,x].
    const bool depthOn = m_depthStreamEnabled.load();
    cfg.enable_stream(RS2_STREAM_COLOR, m_width, m_height, RS2_FORMAT_BGR8, m_fps);
    if (depthOn) {
        cfg.enable_stream(RS2_STREAM_DEPTH, m_width, m_height, RS2_FORMAT_Z16, m_fps);
        // Also enable left IR (Y8) — same stereo sensor as depth, negligible overhead.
        // Always configured so the emitInfrared() toggle works without a pipeline restart.
        cfg.enable_stream(RS2_STREAM_INFRARED, 1, m_width, m_height, RS2_FORMAT_Y8, m_fps);
    }

    // Optional rosbag recording (Viewer-style): record all streams to a .bag.
    {
        std::lock_guard<std::mutex> lk(m_recordMutex);
        if (!m_recordFile.empty()) {
            try {
                cfg.enable_record_to_file(m_recordFile);
                spdlog::info("RealSense recording to {}", m_recordFile);
            } catch (const rs2::error& e) {
                spdlog::warn("RealSense record-to-file failed: {}", e.what());
            }
        }
    }

    rs2::pipeline_profile profile;
    bool started = false;
    try {
        profile = pipe.start(cfg);
        started = true;
    } catch (const rs2::error& e) {
        // Requested mode unsupported (e.g. the global 1920x1080 default exceeds
        // the D405's 1280x720 sensors). Retry letting librealsense pick valid
        // default color+depth modes rather than failing outright.
        spdlog::warn("RealSense {}x{}@{} unsupported ({}). Retrying with default modes.",
                     m_width, m_height, m_fps, e.what());
        try {
            rs2::config fallback;
            fallback.enable_stream(RS2_STREAM_COLOR, RS2_FORMAT_BGR8);
            if (depthOn) {
                fallback.enable_stream(RS2_STREAM_DEPTH, RS2_FORMAT_Z16);
                fallback.enable_stream(RS2_STREAM_INFRARED, 1, RS2_FORMAT_Y8);
            }
            profile = pipe.start(fallback);
            started = true;
        } catch (const rs2::error& e2) {
            spdlog::error("RealSense pipeline start failed: {}", e2.what());
            emit captureError(QString("RealSense start failed: %1").arg(e2.what()));
        }
    }
    if (!started) {
        m_capturing.store(false);
        emit captureStateChanged(false);
        return;
    }

    // Depth → metres scale (depth units), and align depth onto the color frame.
    float depthUnits = 0.001f;  // default 1 mm/unit; refined from the sensor
    try {
        depthUnits = profile.get_device().first<rs2::depth_sensor>().get_depth_scale();
    } catch (const rs2::error&) { /* keep default */ }
    rs2::align alignToColor(RS2_STREAM_COLOR);

    // Publish the device handle so the GUI can query/set sensor options live.
    try {
        std::lock_guard<std::mutex> lk(m_deviceMutex);
        m_device = std::make_unique<rs2::device>(profile.get_device());
    } catch (const rs2::error&) { /* options panel will be empty */ }

    // Apply a pending Visual Preset (from a resolution profile) now that the
    // device is live — the reliable point to set it.
    const float pendingPreset = m_pendingPreset.exchange(-1.0f);
    if (pendingPreset >= 0.0f) {
        try {
            for (auto&& s : profile.get_device().query_sensors()) {
                if (s.supports(RS2_OPTION_VISUAL_PRESET)) {
                    s.set_option(RS2_OPTION_VISUAL_PRESET, pendingPreset);
                    break;
                }
            }
        } catch (const rs2::error& e) {
            spdlog::warn("RealSense visual preset apply failed: {}", e.what());
        }
    }

    // Report the mode the camera actually settled on + cache color intrinsics.
    try {
        auto vsp = profile.get_stream(RS2_STREAM_COLOR).as<rs2::video_stream_profile>();
        m_width  = vsp.width();
        m_height = vsp.height();
        m_fps    = vsp.fps();
        const rs2_intrinsics in = vsp.get_intrinsics();
        m_colorFx.store(static_cast<double>(in.fx));
        m_colorFy.store(static_cast<double>(in.fy));
        m_colorPpx.store(static_cast<double>(in.ppx));
        m_colorPpy.store(static_cast<double>(in.ppy));
        spdlog::info("RealSense color opened: {}x{} @ {} fps (BGR8), fx={:.1f}px, "
                     "depth_units={} m",
                     m_width, m_height, m_fps, in.fx, depthUnits);
    } catch (const rs2::error&) { /* keep requested values */ }

    // Point cloud helper (SDK path, like the RealSense Viewer): map_to(color)
    // + calculate(depth). Reused across frames; only run when 3D view is on.
    rs2::pointcloud pc;
    auto lastCloud = std::chrono::steady_clock::now() - std::chrono::seconds(1);

    // Depth colorizer (histogram-equalized, like the Viewer's Depth panel).
    // Only run when the colorized-depth view is active.
    rs2::colorizer colorizer;
    colorizer.set_option(RS2_OPTION_COLOR_SCHEME, 0.f);  // 0 = Jet (Viewer default)
    // Histogram equalization is on by default — that's what makes near/far
    // detail readable regardless of the absolute depth range.

    // Per-stream FPS measurement (refreshed ~1 Hz).
    auto fpsWindow = std::chrono::steady_clock::now();
    int colorFrames = 0, depthFrames = 0;

    int consecutiveFailures = 0;
    while (m_capturing.load()) {
        rs2::frameset fs;
        try {
            // Bounded wait so stop() is responsive even if frames stall.
            if (!pipe.try_wait_for_frames(&fs, 1000)) {
                if (++consecutiveFailures >= 30) {
                    spdlog::error("RealSense: 30 frame timeouts, stopping capture");
                    emit captureError("RealSense stopped: sustained frame timeouts");
                    break;
                }
                continue;
            }
        } catch (const rs2::error& e) {
            spdlog::error("RealSense wait_for_frames error: {}", e.what());
            emit captureError(QString("RealSense error: %1").arg(e.what()));
            break;
        }
        consecutiveFailures = 0;

        // Align depth onto color so the two frames share a pixel grid.
        rs2::frameset aligned;
        try {
            aligned = alignToColor.process(fs);
        } catch (const rs2::error&) {
            aligned = fs;  // fall back to raw set
        }

        rs2::video_frame color = aligned.get_color_frame();
        if (!color) continue;

        // The rs2 buffer is recycled by the pipeline, so clone into an owned
        // cv::Mat before publishing. (Phase 2 can route this through the
        // UnifiedAllocator to land in CUDA UMA.)
        const cv::Mat view(cv::Size(color.get_width(), color.get_height()),
                           CV_8UC3, const_cast<void*>(color.get_data()),
                           cv::Mat::AUTO_STEP);
        auto shared = std::make_shared<const cv::Mat>(view.clone());

        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            m_latestFrame = shared;
        }
        emit frameReady(shared);
        ++colorFrames;

        // Publish depth as CV_16UC1 in millimetres (aligned to color), after
        // the post-processing chain to cut stereo noise (D405 has no projector).
        if (rs2::depth_frame depth = aligned.get_depth_frame()) {
            rs2::frame filtered = depth;
            {
                std::lock_guard<std::mutex> lk(m_filters->mutex);
                filtered = m_filters->processOrdered(filtered);
            }
            const rs2::depth_frame fdepth = filtered.as<rs2::depth_frame>();
            const cv::Mat raw(cv::Size(fdepth.get_width(), fdepth.get_height()),
                              CV_16UC1, const_cast<void*>(fdepth.get_data()),
                              cv::Mat::AUTO_STEP);
            // raw units → mm. depthUnits is metres/unit, so mm = raw * units * 1000.
            cv::Mat depthMm;
            raw.convertTo(depthMm, CV_16UC1, depthUnits * 1000.0);
            emit depthFrameReady(std::make_shared<const cv::Mat>(std::move(depthMm)));
            ++depthFrames;

            // ── Colorized depth (rs2::colorizer) for the 2D depth view ──
            // Histogram-equalized RGB, like the Viewer's Depth panel. Only when
            // that view is active.
            if (m_emitColorDepth.load()) {
                try {
                    rs2::video_frame cd = colorizer.colorize(fdepth);
                    const cv::Mat bgr(cv::Size(cd.get_width(), cd.get_height()),
                                      CV_8UC3, const_cast<void*>(cd.get_data()),
                                      cv::Mat::AUTO_STEP);
                    // colorizer outputs RGB8; emit as an owned BGR Mat to match
                    // the app's color convention (Application converts BGR→RGB).
                    cv::Mat bgrOwned;
                    cv::cvtColor(bgr, bgrOwned, cv::COLOR_RGB2BGR);
                    emit colorizedDepthReady(
                        std::make_shared<const cv::Mat>(std::move(bgrOwned)));
                } catch (const rs2::error& e) {
                    spdlog::debug("RealSense colorize failed: {}", e.what());
                }
            }

            // ── 3D point cloud (SDK path) — canonical librealsense method ──
            // pc.map_to(color) + pc.calculate(depth). Reuses the SAME filtered
            // (post-processed) depth so the cloud benefits from spatial/temporal
            // smoothing without running the temporal filter on a second stream.
            // Built here (capture thread) to keep the GUI free; published as an
            // owned, downsampled buffer.
            std::string plyPath;
            { std::lock_guard<std::mutex> lk(m_plyMutex); plyPath.swap(m_pendingPly); }

            const auto now = std::chrono::steady_clock::now();
            const bool due = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - lastCloud).count() >= 66;  // ~15 Hz
            const bool wantCloud = m_emitCloud.load() && due;
            if (wantCloud || !plyPath.empty()) {
                if (wantCloud) lastCloud = now;
                try {
                    // Optional decimation for the cloud/PLY only (Intel's
                    // canonical 3D-scan order, issue #10682). Cleaner, less
                    // noisy scan with fewer points. The overlay/depth-view
                    // depth (fdepth) is left untouched so its 1:1 color
                    // alignment stays exact. The SDK re-maps texture via UV,
                    // so a coarser depth still colors correctly from `color`.
                    rs2::depth_frame cloudDepth = fdepth;
                    const int dec = m_cloudDecimation.load();
                    if (dec >= 2) {
                        std::lock_guard<std::mutex> lk(m_filters->mutex);
                        m_filters->decimation.set_option(
                            RS2_OPTION_FILTER_MAGNITUDE, static_cast<float>(dec));
                        cloudDepth = m_filters->decimation.process(fdepth)
                                         .as<rs2::depth_frame>();
                    }
                    pc.map_to(color);
                    rs2::points pts = pc.calculate(cloudDepth);

                    // PLY export (Viewer "Export"): SDK writes vertices+color.
                    if (!plyPath.empty()) {
                        try {
                            pts.export_to_ply(plyPath, color);
                            spdlog::info("RealSense point cloud exported: {}", plyPath);
                            emit plyExportFinished(true, QString::fromStdString(plyPath));
                        } catch (const rs2::error& e) {
                            spdlog::warn("RealSense PLY export failed: {}", e.what());
                            emit plyExportFinished(false, QString::fromUtf8(e.what()));
                        }
                    }

                    if (wantCloud) {
                        const auto* verts = pts.get_vertices();
                        const auto* uvs   = pts.get_texture_coordinates();
                        const size_t total = pts.size();

                        // Downsample so the published cloud stays under ~200k points.
                        size_t stride = 1;
                        while (total / stride > 200000) ++stride;

                        const int cw = color.get_width();
                        const int ch = color.get_height();
                        const auto* cdata = static_cast<const uint8_t*>(color.get_data());

                        auto cloud = std::make_shared<PointCloudData>();
                        cloud->xyz.reserve((total / stride) * 3);
                        cloud->rgb.reserve((total / stride) * 3);
                        for (size_t i = 0; i < total; i += stride) {
                            if (verts[i].z <= 0.f) continue;   // invalid sample
                            cloud->xyz.push_back(verts[i].x);
                            cloud->xyz.push_back(verts[i].y);
                            cloud->xyz.push_back(verts[i].z);
                            // Sample BGR8 color via the point's texture coordinate.
                            int u = std::min(std::max(int(uvs[i].u * cw + 0.5f), 0), cw - 1);
                            int v = std::min(std::max(int(uvs[i].v * ch + 0.5f), 0), ch - 1);
                            const uint8_t* px = cdata + (size_t(v) * cw + u) * 3;
                            cloud->rgb.push_back(px[2]);  // R
                            cloud->rgb.push_back(px[1]);  // G
                            cloud->rgb.push_back(px[0]);  // B
                        }
                        cloud->count = static_cast<int>(cloud->xyz.size() / 3);
                        emit pointCloudReady(PointCloudRef(std::move(cloud)));
                    }
                } catch (const rs2::error& e) {
                    spdlog::debug("RealSense pointcloud calc failed: {}", e.what());
                }
            }
        }

        // ── Left IR camera (Intel tuning guide: use IR for reflective surfaces) ──
        // The stereo left camera (Y8 grayscale) avoids color-channel saturation
        // on shiny solder joints and bare PCB metal. Emitted only when requested.
        if (m_emitIR.load()) {
            if (rs2::video_frame ir = fs.get_infrared_frame(1)) {
                const cv::Mat y8(cv::Size(ir.get_width(), ir.get_height()),
                                 CV_8UC1, const_cast<void*>(ir.get_data()),
                                 cv::Mat::AUTO_STEP);
                cv::Mat bgr;
                cv::cvtColor(y8, bgr, cv::COLOR_GRAY2BGR);
                emit infraredReady(std::make_shared<const cv::Mat>(std::move(bgr)));
            }
        }

        // ── On-chip self-calibration (D4xx, no target) ──────────────
        // Runs on the capture thread between frames; blocks a few seconds.
        if (m_calibPending.exchange(false)) {
            try {
                std::lock_guard<std::mutex> lk(m_deviceMutex);
                if (!m_device) {
                    emit onChipCalibrationFinished(false, 0.f, "No device handle");
                } else if (!m_depthStreamEnabled.load()) {
                    // The on-chip routine drives the stereo/depth module — it
                    // cannot run with the depth stream disabled.
                    emit onChipCalibrationFinished(false, 0.f,
                        QString("Self-calibration needs the depth stream enabled. "
                                "Re-enable depth, then retry."));
                } else if (m_colorFps.load() <= 0.0 && m_depthFps.load() <= 0.0) {
                    // No frames flowing yet — the firmware rejects hwmon commands
                    // until the streams have settled ("HW not ready", -7). This is
                    // the usual cause right after a backend switch.
                    emit onChipCalibrationFinished(false, 0.f,
                        QString("Camera is not streaming yet. Wait a couple of "
                                "seconds after starting, then retry."));
                } else {
                    // Guard against USB2: the D4xx self-calibration is known to
                    // fail (hwmon "HW not ready") on a degraded USB 2.1 link. Warn
                    // up front so the user can re-seat the cable / use a USB3 port.
                    std::string usb = m_device->supports(RS2_CAMERA_INFO_USB_TYPE_DESCRIPTOR)
                        ? m_device->get_info(RS2_CAMERA_INFO_USB_TYPE_DESCRIPTOR) : "";
                    if (!usb.empty() && usb[0] == '2') {
                        spdlog::warn("RealSense self-calibration on USB {} link — "
                                     "the D4xx firmware often rejects it below USB3.", usb);
                    }

                    auto cal = m_device->as<rs2::auto_calibrated_device>();
                    // Default config: speed=slow for best accuracy on a static rig.
                    const std::string cfg =
                        "{\n \"speed\": 3,\n \"scan parameter\": 0\n}";

                    // "HW not ready" (-7) is a transient firmware state — the
                    // device frequently accepts the command on a second attempt
                    // once it has settled. Retry a few times before giving up.
                    constexpr int kMaxAttempts = 3;
                    float health = 0.f;
                    bool  done   = false;
                    std::string lastErr;
                    for (int attempt = 1; attempt <= kMaxAttempts && !done; ++attempt) {
                        try {
                            rs2::calibration_table table =
                                cal.run_on_chip_calibration(cfg, &health, 10000);
                            cal.set_calibration_table(table);
                            cal.write_calibration();
                            done = true;
                        } catch (const rs2::error& e) {
                            lastErr = e.what();
                            spdlog::warn("RealSense on-chip calibration attempt {}/{} "
                                         "failed: {}", attempt, kMaxAttempts, lastErr);
                            if (attempt < kMaxAttempts)
                                std::this_thread::sleep_for(std::chrono::milliseconds(800));
                        }
                    }

                    if (done) {
                        spdlog::info("RealSense on-chip calibration done, health={:.4f}", health);
                        emit onChipCalibrationFinished(
                            true, health,
                            QString("Self-calibration OK (health %1)").arg(health, 0, 'f', 4));
                    } else {
                        QString hint = QString::fromUtf8(lastErr.c_str());
                        if (lastErr.find("HW not ready") != std::string::npos) {
                            hint += QString("\n\nThe camera firmware was not ready. "
                                            "Make sure it is on a USB3 port%1, hold it "
                                            "still against a flat textured surface, let "
                                            "it stream for a few seconds, then retry.")
                                .arg(usb.empty() ? QString() : QString(" (currently USB %1)").arg(QString::fromStdString(usb)));
                        }
                        emit onChipCalibrationFinished(false, 0.f, hint);
                    }
                }
            } catch (const rs2::error& e) {
                spdlog::warn("RealSense on-chip calibration failed: {}", e.what());
                emit onChipCalibrationFinished(false, 0.f, QString::fromUtf8(e.what()));
            }
        }

        // Refresh per-stream FPS roughly once a second.
        const auto fnow = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(fnow - fpsWindow).count();
        if (secs >= 1.0) {
            m_colorFps.store(colorFrames / secs);
            m_depthFps.store(depthFrames / secs);
            colorFrames = depthFrames = 0;
            fpsWindow = fnow;
        }
    }

    m_colorFps.store(0.0);
    m_depthFps.store(0.0);

    {
        std::lock_guard<std::mutex> lk(m_deviceMutex);
        m_device.reset();
    }

    try {
        pipe.stop();
    } catch (const rs2::error&) { /* already stopped */ }

    // If the loop ended on its own (error/timeout break) rather than via stop(),
    // transition to the stopped state and notify listeners — otherwise
    // isCapturing() would stay true and start() would refuse to restart.
    if (m_capturing.exchange(false))
        emit captureStateChanged(false);
}

namespace {

/// Append every supported rs2 option of `opts` as RsControl rows. Shared by
/// sensors and filters (both derive from rs2::options).
void enumerateOptions(const rs2::options& opts, int ownerId,
                      const std::string& group, std::vector<RsControl>& out)
{
    for (int o = 0; o < static_cast<int>(RS2_OPTION_COUNT); ++o) {
        const auto opt = static_cast<rs2_option>(o);
        if (!opts.supports(opt)) continue;
        // Skip internal stream-mux options (not meaningful as user controls).
        { const char* _n = rs2_option_to_string(opt); if (_n) { std::string_view _sv(_n); if (_sv == "Stream Filter" || _sv == "Stream Format Filter" || _sv == "Stream Index Filter") continue; } }
        rs2::option_range r;
        try { r = opts.get_option_range(opt); }
        catch (const rs2::error&) { continue; }

        RsControl c;
        c.sensorIndex = ownerId;
        c.sensorName  = group;
        c.optionId    = o;
        c.name        = rs2_option_to_string(opt);
        try { c.description = opts.get_option_description(opt); }
        catch (const rs2::error&) {}
        c.min = r.min; c.max = r.max; c.step = r.step; c.def = r.def;
        try { c.current = opts.get_option(opt); } catch (const rs2::error&) {}
        c.isBool   = (r.min == 0.f && r.max == 1.f && r.step == 1.f);
        c.readOnly = opts.is_option_read_only(opt);

        // Discrete enum (e.g. Visual Preset) → labelled values for a combo box.
        const bool integralStep = (r.step == 1.f);
        const int  count = static_cast<int>((r.max - r.min) / 1.f) + 1;
        if (!c.isBool && integralStep && count >= 2 && count <= 32) {
            bool allNamed = true;
            std::vector<std::pair<float, std::string>> values;
            for (int v = static_cast<int>(r.min); v <= static_cast<int>(r.max); ++v) {
                const char* d = nullptr;
                try { d = opts.get_option_value_description(opt, static_cast<float>(v)); }
                catch (const rs2::error&) { d = nullptr; }
                if (!d || !*d) { allNamed = false; break; }
                values.emplace_back(static_cast<float>(v), d);
            }
            if (allNamed) c.enumValues = std::move(values);
        }
        out.push_back(std::move(c));
    }
}

} // namespace

std::vector<RsControl> RealSenseCapture::listControls() const
{
    std::vector<RsControl> out;

    // Sensor options.
    {
        std::lock_guard<std::mutex> lk(m_deviceMutex);
        if (m_device) {
            try {
                const auto sensors = m_device->query_sensors();
                for (int si = 0; si < static_cast<int>(sensors.size()); ++si) {
                    const rs2::sensor& s = sensors[si];
                    const std::string sname = s.supports(RS2_CAMERA_INFO_NAME)
                        ? s.get_info(RS2_CAMERA_INFO_NAME) : ("Sensor " + std::to_string(si));
                    enumerateOptions(s, si, sname, out);
                }
            } catch (const rs2::error& e) {
                spdlog::warn("RealSense listControls (sensors) failed: {}", e.what());
            }
        }
    }

    // Depth post-processing filters: a synthetic "Enabled" toggle + the
    // filter's own options. Grouped under each filter's name.
    if (m_filters) {
        std::lock_guard<std::mutex> lk(m_filters->mutex);
        for (int fi = 0; fi < FilterChain::Count; ++fi) {
            const int ownerId = kFilterBase + fi;
            const std::string group = FilterChain::name(fi);

            RsControl en;
            en.sensorIndex = ownerId;
            en.sensorName  = group;
            en.optionId    = kEnableOption;
            en.name        = "Enabled";
            en.description = "Enable this depth post-processing filter.";
            en.min = 0; en.max = 1; en.step = 1; en.def = 1;
            en.current = m_filters->enabled[fi] ? 1.f : 0.f;
            en.isBool = true;
            out.push_back(std::move(en));

            try { enumerateOptions(m_filters->at(fi), ownerId, group, out); }
            catch (const rs2::error&) {}
        }
    }

    return out;
}

bool RealSenseCapture::setControl(int ownerId, int optionId, float value)
{
    // Filter target.
    if (ownerId >= kFilterBase) {
        const int fi = ownerId - kFilterBase;
        if (!m_filters || fi < 0 || fi >= FilterChain::Count) return false;
        std::lock_guard<std::mutex> lk(m_filters->mutex);
        if (optionId == kEnableOption) {
            m_filters->enabled[fi] = (value >= 0.5f);
            return true;
        }
        try {
            m_filters->at(fi).set_option(static_cast<rs2_option>(optionId), value);
            return true;
        } catch (const rs2::error& e) {
            spdlog::warn("RealSense setControl(filter {}, opt {}) failed: {}",
                         fi, optionId, e.what());
            return false;
        }
    }

    // Sensor target.
    std::lock_guard<std::mutex> lk(m_deviceMutex);
    if (!m_device) return false;
    try {
        const auto sensors = m_device->query_sensors();
        if (ownerId < 0 || ownerId >= static_cast<int>(sensors.size())) return false;
        sensors[ownerId].set_option(static_cast<rs2_option>(optionId), value);
        return true;
    } catch (const rs2::error& e) {
        spdlog::warn("RealSense setControl(sensor {}, opt {}, {}) failed: {}",
                     ownerId, optionId, value, e.what());
        return false;
    }
}

} // namespace ibom::camera
