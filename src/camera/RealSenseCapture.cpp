#include "RealSenseCapture.h"

#include <librealsense2/rs.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>

namespace ibom::camera {

/// Resolution-preserving depth filters (decimation excluded on purpose so the
/// color↔depth alignment stays 1:1). Each has a runtime on/off flag. The mutex
/// guards process() on the capture thread vs option get/set from the GUI.
struct FilterChain {
    rs2::spatial_filter      spatial;
    rs2::temporal_filter     temporal;
    rs2::threshold_filter    threshold;
    rs2::hole_filling_filter holeFill;
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
            devices.push_back(serial.empty() ? name : (name + " " + serial));
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
    cfg.enable_stream(RS2_STREAM_COLOR, m_width, m_height, RS2_FORMAT_BGR8, m_fps);
    cfg.enable_stream(RS2_STREAM_DEPTH, m_width, m_height, RS2_FORMAT_Z16, m_fps);

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
            fallback.enable_stream(RS2_STREAM_DEPTH, RS2_FORMAT_Z16);
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

            // ── 3D point cloud (SDK path) — canonical librealsense method ──
            // pc.map_to(color) + pc.calculate(depth). Reuses the SAME filtered
            // (post-processed) depth so the cloud benefits from spatial/temporal
            // smoothing without running the temporal filter on a second stream.
            // Built here (capture thread) to keep the GUI free; published as an
            // owned, downsampled buffer.
            if (m_emitCloud.load()) {
                const auto now = std::chrono::steady_clock::now();
                const bool due = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     now - lastCloud).count() >= 66;  // ~15 Hz
                if (due) {
                    lastCloud = now;
                    try {
                        pc.map_to(color);
                        rs2::points pts = pc.calculate(fdepth);
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
                    } catch (const rs2::error& e) {
                        spdlog::debug("RealSense pointcloud calc failed: {}", e.what());
                    }
                }
            }  // m_emitCloud
        }
    }

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
