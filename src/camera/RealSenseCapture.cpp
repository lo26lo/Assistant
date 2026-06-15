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

    // Phase 1: color only. BGR8 so the cv::Mat matches the rest of the app.
    cfg.enable_stream(RS2_STREAM_COLOR, m_width, m_height, RS2_FORMAT_BGR8, m_fps);

    rs2::pipeline_profile profile;
    bool started = false;
    try {
        profile = pipe.start(cfg);
        started = true;
    } catch (const rs2::error& e) {
        // Requested mode unsupported (e.g. the global 1920x1080 default exceeds
        // the D405's 1280x720 color sensor). Retry letting librealsense pick a
        // valid default color mode rather than failing outright.
        spdlog::warn("RealSense {}x{}@{} unsupported ({}). Retrying with default mode.",
                     m_width, m_height, m_fps, e.what());
        try {
            rs2::config fallback;
            fallback.enable_stream(RS2_STREAM_COLOR, RS2_FORMAT_BGR8);
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

    // Report the mode the camera actually settled on.
    try {
        auto vsp = profile.get_stream(RS2_STREAM_COLOR).as<rs2::video_stream_profile>();
        m_width  = vsp.width();
        m_height = vsp.height();
        m_fps    = vsp.fps();
        spdlog::info("RealSense color opened: {}x{} @ {} fps (BGR8)",
                     m_width, m_height, m_fps);
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

        rs2::video_frame color = fs.get_color_frame();
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
    }

    try {
        pipe.stop();
    } catch (const rs2::error&) { /* already stopped */ }
}

} // namespace ibom::camera
