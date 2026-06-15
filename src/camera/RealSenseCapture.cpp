#include "RealSenseCapture.h"

#include <librealsense2/rs.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <chrono>

namespace ibom::camera {

RealSenseCapture::RealSenseCapture(QObject* parent)
    : ICameraSource(parent)
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
    if (!m_capturing.load()) return;
    spdlog::info("Stopping RealSense capture...");
    m_capturing.store(false);
    if (m_thread && m_thread->joinable())
        m_thread->join();
    m_thread.reset();
    emit captureStateChanged(false);
    spdlog::info("RealSense capture stopped.");
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
        spdlog::warn("RealSense enumeration failed: {}", e.what());
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

    // Report the mode the camera actually settled on + cache color intrinsics.
    try {
        auto vsp = profile.get_stream(RS2_STREAM_COLOR).as<rs2::video_stream_profile>();
        m_width  = vsp.width();
        m_height = vsp.height();
        m_fps    = vsp.fps();
        const rs2_intrinsics in = vsp.get_intrinsics();
        m_colorFx.store(static_cast<double>(in.fx));
        spdlog::info("RealSense color opened: {}x{} @ {} fps (BGR8), fx={:.1f}px, "
                     "depth_units={} m",
                     m_width, m_height, m_fps, in.fx, depthUnits);
    } catch (const rs2::error&) { /* keep requested values */ }

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

        // Publish depth as CV_16UC1 in millimetres (aligned to color).
        if (rs2::depth_frame depth = aligned.get_depth_frame()) {
            const cv::Mat raw(cv::Size(depth.get_width(), depth.get_height()),
                              CV_16UC1, const_cast<void*>(depth.get_data()),
                              cv::Mat::AUTO_STEP);
            // raw units → mm. depthUnits is metres/unit, so mm = raw * units * 1000.
            cv::Mat depthMm;
            raw.convertTo(depthMm, CV_16UC1, depthUnits * 1000.0);
            emit depthFrameReady(std::make_shared<const cv::Mat>(std::move(depthMm)));
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_deviceMutex);
        m_device.reset();
    }

    try {
        pipe.stop();
    } catch (const rs2::error&) { /* already stopped */ }
}

std::vector<RsControl> RealSenseCapture::listControls() const
{
    std::vector<RsControl> out;
    std::lock_guard<std::mutex> lk(m_deviceMutex);
    if (!m_device) return out;
    try {
        const auto sensors = m_device->query_sensors();
        for (int si = 0; si < static_cast<int>(sensors.size()); ++si) {
            const rs2::sensor& s = sensors[si];
            const std::string sname = s.supports(RS2_CAMERA_INFO_NAME)
                ? s.get_info(RS2_CAMERA_INFO_NAME) : ("Sensor " + std::to_string(si));
            for (int o = 0; o < static_cast<int>(RS2_OPTION_COUNT); ++o) {
                const auto opt = static_cast<rs2_option>(o);
                if (!s.supports(opt)) continue;
                rs2::option_range r;
                try { r = s.get_option_range(opt); }
                catch (const rs2::error&) { continue; }

                RsControl c;
                c.sensorIndex = si;
                c.sensorName  = sname;
                c.optionId    = o;
                c.name        = rs2_option_to_string(opt);
                try { c.description = s.get_option_description(opt); }
                catch (const rs2::error&) {}
                c.min = r.min; c.max = r.max; c.step = r.step; c.def = r.def;
                try { c.current = s.get_option(opt); } catch (const rs2::error&) {}
                c.isBool   = (r.min == 0.f && r.max == 1.f && r.step == 1.f);
                c.readOnly = s.is_option_read_only(opt);

                // Discrete enum (e.g. Visual Preset): integer steps over a small
                // range where the SDK provides a label per value. Skip booleans.
                const bool integralStep = (r.step == 1.f);
                const int  count = static_cast<int>((r.max - r.min) / 1.f) + 1;
                if (!c.isBool && integralStep && count >= 2 && count <= 32) {
                    bool allNamed = true;
                    std::vector<std::pair<float, std::string>> values;
                    for (int v = static_cast<int>(r.min); v <= static_cast<int>(r.max); ++v) {
                        const char* desc = nullptr;
                        try { desc = s.get_option_value_description(opt, static_cast<float>(v)); }
                        catch (const rs2::error&) { desc = nullptr; }
                        if (!desc || !*desc) { allNamed = false; break; }
                        values.emplace_back(static_cast<float>(v), desc);
                    }
                    if (allNamed) c.enumValues = std::move(values);
                }

                out.push_back(std::move(c));
            }
        }
    } catch (const rs2::error& e) {
        spdlog::warn("RealSense listControls failed: {}", e.what());
    }
    return out;
}

bool RealSenseCapture::setControl(int sensorIndex, int optionId, float value)
{
    std::lock_guard<std::mutex> lk(m_deviceMutex);
    if (!m_device) return false;
    try {
        const auto sensors = m_device->query_sensors();
        if (sensorIndex < 0 || sensorIndex >= static_cast<int>(sensors.size()))
            return false;
        sensors[sensorIndex].set_option(static_cast<rs2_option>(optionId), value);
        return true;
    } catch (const rs2::error& e) {
        spdlog::warn("RealSense setControl(sensor {}, opt {}, {}) failed: {}",
                     sensorIndex, optionId, value, e.what());
        return false;
    }
}

} // namespace ibom::camera
