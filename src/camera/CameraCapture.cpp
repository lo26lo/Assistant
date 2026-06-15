#include "CameraCapture.h"
#include "UnifiedAllocator.h"

#include <opencv2/videoio.hpp>
#include <opencv2/videoio/registry.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>
#include <algorithm>

namespace ibom::camera {

namespace {

/// Build a GStreamer pipeline that decodes the camera's MJPG stream on the
/// Jetson hardware blocks (nvv4l2decoder → NVDEC/VIC) and converts to BGR on
/// the GPU (nvvidconv). This keeps the MJPG decode off the CPU entirely.
/// MJPG (not H.264) is used deliberately: each frame is independent, so there
/// are no inter-frame compression artifacts that would corrupt pad edges,
/// silkscreen text or ORB keypoints — critical for inspection/measurement.
std::string buildGstPipeline(int deviceIndex, int width, int height, int fps)
{
    return "v4l2src device=/dev/video" + std::to_string(deviceIndex) +
           " ! image/jpeg,width=" + std::to_string(width) +
           ",height=" + std::to_string(height) +
           ",framerate=" + std::to_string(fps) + "/1"
           " ! nvv4l2decoder mjpeg=1"
           " ! nvvidconv"
           " ! video/x-raw,format=BGRx"
           " ! videoconvert ! video/x-raw,format=BGR"
           " ! appsink drop=1 max-buffers=1 sync=false";
}

/// True if OpenCV was built with the GStreamer backend available.
bool gstreamerAvailable()
{
    for (auto b : cv::videoio_registry::getBackends()) {
        if (b == cv::CAP_GSTREAMER) return true;
    }
    return false;
}

} // namespace

CameraCapture::CameraCapture(int deviceIndex, QObject* parent)
    : ICameraSource(parent)
    , m_deviceIndex(deviceIndex)
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

FrameRef CameraCapture::latestFrame() const
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    return m_latestFrame;
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
        cap.open(i, cv::CAP_V4L2);
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

    // V4L2 first, generic fallback if the V4L2 open fails (e.g. GStreamer source)
    std::vector<std::pair<int, const char*>> backends = {
        {cv::CAP_V4L2, "V4L2"},
        {cv::CAP_ANY,  "Auto"}
    };

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
    bool openedViaGst = false;

    // Preferred path on Jetson: hardware MJPG decode through GStreamer
    // (nvv4l2decoder). The pipeline already outputs BGR frames, so the
    // FOURCC/resolution properties below must be skipped when this succeeds.
    if (m_hwDecode) {
        if (gstreamerAvailable()) {
            const std::string pipeline =
                buildGstPipeline(m_deviceIndex, m_width, m_height, m_fps);
            spdlog::info("Trying NVIDIA HW decode via GStreamer:\n  {}", pipeline);
            if (cap.open(pipeline, cv::CAP_GSTREAMER) && cap.isOpened()) {
                spdlog::info("Camera opened with GStreamer (nvv4l2decoder, HW MJPG decode)");
                opened = true;
                openedViaGst = true;
            } else {
                spdlog::warn("GStreamer HW pipeline failed to open — "
                             "falling back to CPU V4L2 decode");
            }
        } else {
            spdlog::warn("HW decode requested but OpenCV has no GStreamer backend "
                         "— falling back to CPU V4L2 decode");
        }
    }

    if (!opened) {
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
    }

    if (!opened) {
        spdlog::error("Failed to open camera device {} with any backend", m_deviceIndex);
        emit captureError(QString("Failed to open camera device %1").arg(m_deviceIndex));
        m_capturing.store(false);
        emit captureStateChanged(false);
        return;
    }

    // These V4L2 properties are meaningless on the GStreamer path — the format,
    // resolution and fps are baked into the pipeline caps, and the appsink
    // already delivers decoded BGR frames.
    if (!openedViaGst) {
        // Request MJPG BEFORE the resolution/fps. UVC cameras default to raw YUYV,
        // whose bandwidth caps USB 2.0 well below 1080p@30 (the driver then silently
        // falls back to ~5-10 fps). MJPG is compressed on-camera, so full res/fps fit.
        // Order matters: FOURCC must be set before width/height for most V4L2 drivers.
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));

        // Configure capture properties (best-effort, camera may not support requested res)
        cap.set(cv::CAP_PROP_FRAME_WIDTH, m_width);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, m_height);
        cap.set(cv::CAP_PROP_FPS, m_fps);
        cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    }

    // Decode and log the FOURCC the camera actually settled on — if it isn't
    // MJPG, expect reduced fps at high resolution (the camera ignored the hint).
    const int fourcc = static_cast<int>(cap.get(cv::CAP_PROP_FOURCC));
    const char fourccStr[5] = {
        static_cast<char>(fourcc & 0xFF),
        static_cast<char>((fourcc >> 8) & 0xFF),
        static_cast<char>((fourcc >> 16) & 0xFF),
        static_cast<char>((fourcc >> 24) & 0xFF),
        '\0'
    };

    spdlog::info("Camera opened: {}x{} @ {} fps, FOURCC={} (unified memory: {})",
        static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH)),
        static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT)),
        static_cast<int>(cap.get(cv::CAP_PROP_FPS)),
        fourccStr,
        unifiedMemoryAvailable() ? "yes" : "no");

    cv::MatAllocator* alloc = unifiedAllocator();

    // Warmup: some UVC cameras need a few frames before the pipeline settles
    constexpr int kWarmupAttempts = 60;  // up to 3 seconds at 50ms each
    bool warmupOk = false;
    for (int i = 0; i < kWarmupAttempts && m_capturing.load(); ++i) {
        cv::Mat warmupFrame;
        if (cap.read(warmupFrame) && !warmupFrame.empty()) {
            spdlog::info("Camera warmup OK after {} attempt(s)", i + 1);
            warmupOk = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!warmupOk) {
        spdlog::error("Camera warmup failed: no frame after {} attempts", kWarmupAttempts);
        emit captureError(QString("Camera failed to provide frames (device %1, %2x%3)")
            .arg(m_deviceIndex).arg(m_width).arg(m_height));
        m_capturing.store(false);
        emit captureStateChanged(false);
        cap.release();
        return;
    }

    int consecutiveFailures = 0;
    while (m_capturing.load()) {
        // Fresh cv::Mat each iteration — prevents cap.read() from writing
        // into a buffer that is still shared with downstream consumers.
        // The custom allocator backs the pixel buffer with CUDA Unified
        // Memory on Jetson (no-op fallback to malloc on Windows/desktop).
        cv::Mat frame;
        frame.allocator = alloc;
        if (!cap.read(frame) || frame.empty()) {
            ++consecutiveFailures;
            if (consecutiveFailures >= 30) {
                spdlog::error("Camera: 30 consecutive read failures, stopping capture");
                emit captureError(QString("Camera stopped: sustained read failures on device %1")
                    .arg(m_deviceIndex));
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            continue;
        }
        consecutiveFailures = 0;

        // Wrap in shared_ptr<const cv::Mat> — pixel buffer is NOT copied;
        // cv::Mat's internal refcount + shared_ptr give us zero-copy fan-out.
        auto shared = std::make_shared<const cv::Mat>(std::move(frame));

        // Publish latest (thread-safe pointer swap, no pixel copy)
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            m_latestFrame = shared;
        }

        // Notify listeners — Qt copies the shared_ptr (refcount bump only).
        emit frameReady(shared);
    }

    cap.release();
}

} // namespace ibom::camera
