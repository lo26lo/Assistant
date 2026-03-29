#include "CameraCapture.h"
#include "FrameBuffer.h"

#include <opencv2/videoio.hpp>
#include <opencv2/videoio/registry.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

namespace ibom::camera {

CameraCapture::CameraCapture(int deviceIndex, QObject* parent)
    : QObject(parent)
    , m_deviceIndex(deviceIndex)
    , m_frameBuffer(std::make_unique<FrameBuffer>(3)) // Triple buffering
{
}

CameraCapture::~CameraCapture()
{
    stop();
}

bool CameraCapture::start()
{
    if (m_capturing.load()) {
        spdlog::warn("Camera already capturing.");
        return true;
    }

    spdlog::info("Starting camera capture on device {}...", m_deviceIndex);

    m_capturing.store(true);
    m_thread = std::make_unique<std::thread>(&CameraCapture::captureLoop, this);

    emit captureStateChanged(true);
    return true;
}

void CameraCapture::stop()
{
    if (!m_capturing.load()) return;

    spdlog::info("Stopping camera capture...");
    m_capturing.store(false);

    if (m_thread && m_thread->joinable()) {
        m_thread->join();
    }
    m_thread.reset();

    emit captureStateChanged(false);
    spdlog::info("Camera capture stopped.");
}

cv::Mat CameraCapture::latestFrame() const
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    return m_latestFrame.clone();
}

FrameBuffer& CameraCapture::frameBuffer()
{
    return *m_frameBuffer;
}

void CameraCapture::setDeviceIndex(int index)
{
    m_deviceIndex = index;
}

void CameraCapture::setResolution(int width, int height)
{
    m_width = width;
    m_height = height;
}

void CameraCapture::setFps(int fps)
{
    m_fps = fps;
}

QSize CameraCapture::resolution() const
{
    return QSize(m_width, m_height);
}

std::vector<std::string> CameraCapture::listDevices()
{
    std::vector<std::string> devices;

    for (int i = 0; i < 10; ++i) {
        cv::VideoCapture cap;
#ifdef IBOM_PLATFORM_WINDOWS
        cap.open(i, cv::CAP_MSMF);
#else
        cap.open(i, cv::CAP_V4L2);
#endif
        if (cap.isOpened()) {
            devices.push_back("Camera " + std::to_string(i));
            cap.release();
        }
    }

    return devices;
}

void CameraCapture::captureLoop()
{
    cv::VideoCapture cap;

    // Try multiple backends on Windows for maximum compatibility
#ifdef IBOM_PLATFORM_WINDOWS
    std::vector<std::pair<int, const char*>> backends = {
        {cv::CAP_MSMF,  "Media Foundation"},
        {cv::CAP_DSHOW, "DirectShow"},
        {cv::CAP_ANY,   "Auto"}
    };
#else
    std::vector<std::pair<int, const char*>> backends = {
        {cv::CAP_V4L2, "V4L2"},
        {cv::CAP_ANY,  "Auto"}
    };
#endif

    // Log available OpenCV videoio backends
    auto availBackends = cv::videoio_registry::getBackends();
    spdlog::info("OpenCV videoio backends available ({}):", availBackends.size());
    for (auto b : availBackends) {
        spdlog::info("  - {} (id={})", cv::videoio_registry::getBackendName(b), static_cast<int>(b));
    }
    auto camBackends = cv::videoio_registry::getCameraBackends();
    spdlog::info("OpenCV camera backends ({}):", camBackends.size());
    for (auto b : camBackends) {
        spdlog::info("  - {} (id={})", cv::videoio_registry::getBackendName(b), static_cast<int>(b));
    }

    bool opened = false;
    for (auto& [backend, name] : backends) {
        spdlog::info("Trying camera {} with {} backend (id={})...", m_deviceIndex, name, backend);
        bool result = cap.open(m_deviceIndex, backend);
        spdlog::info("  cap.open result: {}, isOpened: {}", result, cap.isOpened());
        if (result && cap.isOpened()) {
            spdlog::info("Camera opened with {} backend", name);
            opened = true;
            break;
        }
    }

    if (!opened) {
        spdlog::error("Failed to open camera device {} with any backend", m_deviceIndex);
        emit captureError(QString("Failed to open camera device %1").arg(m_deviceIndex));
        m_capturing.store(false);
        emit captureStateChanged(false);
        return;
    }

    // Configure capture properties (best-effort, camera may not support requested res)
    cap.set(cv::CAP_PROP_FRAME_WIDTH, m_width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, m_height);
    cap.set(cv::CAP_PROP_FPS, m_fps);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    spdlog::info("Camera opened: {}x{} @ {} fps",
        static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH)),
        static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT)),
        static_cast<int>(cap.get(cv::CAP_PROP_FPS)));

    cv::Mat frame;

    while (m_capturing.load()) {
        if (!cap.read(frame)) {
            spdlog::warn("Failed to read frame from camera.");
            continue;
        }

        if (frame.empty()) continue;

        // Update latest frame (thread-safe)
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            frame.copyTo(m_latestFrame);
        }

        // Push to ring buffer for AI pipeline
        m_frameBuffer->push(frame);

        // Notify listeners (clone to decouple from capture loop's buffer)
        emit frameReady(frame.clone());
    }

    cap.release();
}

} // namespace ibom::camera
