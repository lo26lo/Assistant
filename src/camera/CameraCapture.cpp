#include "CameraCapture.h"
#include "FrameBuffer.h"

#include <opencv2/videoio.hpp>
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

    // Probe first 10 camera indices
    for (int i = 0; i < 10; ++i) {
        cv::VideoCapture cap;
#ifdef IBOM_PLATFORM_WINDOWS
        cap.open(i, cv::CAP_DSHOW);
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

    // Open camera with platform-specific backend
#ifdef IBOM_PLATFORM_WINDOWS
    int backend = cv::CAP_DSHOW; // DirectShow for lowest latency on Windows
#else
    int backend = cv::CAP_V4L2;  // V4L2 for Linux
#endif

    if (!cap.open(m_deviceIndex, backend)) {
        spdlog::error("Failed to open camera device {}", m_deviceIndex);
        emit captureError(QString("Failed to open camera device %1").arg(m_deviceIndex));
        m_capturing.store(false);
        emit captureStateChanged(false);
        return;
    }

    // Configure capture properties
    cap.set(cv::CAP_PROP_FRAME_WIDTH, m_width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, m_height);
    cap.set(cv::CAP_PROP_FPS, m_fps);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1); // Minimize internal buffering for low latency

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

        // Notify listeners
        emit frameReady(frame);
    }

    cap.release();
}

} // namespace ibom::camera
