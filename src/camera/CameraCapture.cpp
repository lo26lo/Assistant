#include "CameraCapture.h"
#include "UnifiedAllocator.h"

#include <opencv2/videoio.hpp>
#include <opencv2/videoio/registry.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>
#include <algorithm>

#ifdef __linux__
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#endif

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

/// Build a CPU GStreamer pipeline that forces MJPG and decodes it on the CPU
/// (jpegdec — no NVDEC/nvvidconv, so NO EGL display is required, unlike
/// buildGstPipeline()). This is the reliable way to get MJPG out of UVC
/// cameras that ignore OpenCV's CAP_PROP_FOURCC hint (e.g. the HAYEAR
/// microscope, which stubbornly negotiates raw YUYV via cv::VideoCapture):
/// the explicit `image/jpeg` caps make GStreamer negotiate MJPG with the V4L2
/// driver. MJPG is compressed on-camera, so a USB-2.0 link carries full
/// resolution/fps where uncompressed YUYV would starve the bus.
std::string buildGstPipelineCpu(int deviceIndex, int width, int height, int fps)
{
    return "v4l2src device=/dev/video" + std::to_string(deviceIndex) +
           " ! image/jpeg,width=" + std::to_string(width) +
           ",height=" + std::to_string(height) +
           ",framerate=" + std::to_string(fps) + "/1"
           " ! jpegdec"
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

#ifdef __linux__
/// Human-readable USB link type for a /dev/video<N> node, read from sysfs.
///
/// Resolves /sys/class/video4linux/video<N>/device (a symlink to the USB
/// *interface*), walks up to the owning USB *device* directory and reads its
/// negotiated link "speed" (Mbit/s). Returns e.g. "USB 2.0 HS (480 Mb/s)" or
/// an empty string if the node isn't USB / sysfs can't be read.
///
/// This is the *negotiated* speed, so a UVC microscope that is a USB-2.0 device
/// (or a USB-3 camera that fell back to a 2.0 port) shows 480 Mb/s — which is
/// exactly what tells the user why an uncompressed YUYV stream starves the bus.
std::string usbLinkTag(int videoIndex)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dev = fs::canonical(
        "/sys/class/video4linux/video" + std::to_string(videoIndex) + "/device", ec);
    if (ec) return {};

    // Walk up the device tree until we hit the USB device node (it owns "speed").
    for (int hop = 0; hop < 6 && !dev.empty() && dev != dev.root_path(); ++hop) {
        std::ifstream f(dev / "speed");
        int mbps = 0;
        if (f && (f >> mbps) && mbps > 0) {
            const char* gen =
                  mbps >= 20000 ? "USB 3.2 Gen 2x2"
                : mbps >= 10000 ? "USB 3.2 Gen 2"
                : mbps >=  5000 ? "USB 3.2 Gen 1"
                : mbps >=   480 ? "USB 2.0 HS"
                : mbps >=    12 ? "USB 1.1 FS"
                :                 "USB 1.0 LS";
            return std::string(gen) + " (" + std::to_string(mbps) + " Mb/s)";
        }
        dev = dev.parent_path();
    }
    return {};
}
#endif

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

    // A previous captureLoop may have self-exited (e.g. failed open) leaving a
    // finished-but-joinable thread. Reassigning m_thread below would destroy a
    // joinable std::thread → std::terminate(). Join it first.
    if (m_thread) {
        if (m_thread->joinable())
            m_thread->join();
        m_thread.reset();
    }

    spdlog::info("Starting camera capture on device {}...", m_deviceIndex);

    m_capturing.store(true);
    m_thread = std::make_unique<std::thread>(&CameraCapture::captureLoop, this);

    emit captureStateChanged(true);
    return true;
}

void CameraCapture::stop()
{
    // Always join/reset the thread if present — even if the capture loop already
    // cleared m_capturing on its own (error exit). An early return here would
    // leave a joinable thread that std::terminate()s on destruction or on the
    // next start(). (Mirrors RealSenseCapture::stop().)
    const bool wasCapturing = m_capturing.exchange(false);
    if (wasCapturing)
        spdlog::info("Stopping camera capture...");

    if (m_thread && m_thread->joinable())
        m_thread->join();
    m_thread.reset();

    // Only signal the transition if we were the ones stopping it; if the loop
    // exited by itself it already emitted captureStateChanged(false).
    if (wasCapturing) {
        emit captureStateChanged(false);
        spdlog::info("Camera capture stopped.");
    }
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

std::vector<std::pair<int, std::string>> CameraCapture::listDevices()
{
    std::vector<std::pair<int, std::string>> devices;

#ifdef __linux__
    // Probe each /dev/video<N> node directly with VIDIOC_QUERYCAP. This:
    //  - returns the REAL index N (not a list position), so gaps are preserved;
    //  - reads the card name so the UI can tell the microscope from the D405;
    //  - skips non-capture nodes (metadata / output) that would fail to open;
    //  - avoids the cv::VideoCapture::open() probe, which floods the log with
    //    GStreamer/obsensor warnings on every missing index.
    for (int i = 0; i < 16; ++i) {
        const std::string path = "/dev/video" + std::to_string(i);
        // O_RDONLY: VIDIOC_QUERYCAP is a read-only query, and this is less likely
        // to clash with a device already held open by the capture thread.
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        v4l2_capability cap{};
        if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                ? cap.device_caps : cap.capabilities;
            if (caps & V4L2_CAP_VIDEO_CAPTURE) {
                std::string name = (cap.card[0] != 0)
                    ? std::string(reinterpret_cast<const char*>(cap.card))
                    : ("Camera " + std::to_string(i));
                // The D405 exposes its color/IR/depth streams as plain UVC
                // /dev/video* nodes too, so VIDIOC_QUERYCAP reports them as
                // valid capture devices here. They must only ever be opened
                // through the RealSense SDK (depth-aligned, factory-rectified),
                // never as a raw V4L2 stream, so exclude them from this list.
                if (name.find("RealSense") != std::string::npos) {
                    ::close(fd);
                    continue;
                }
                // Append the negotiated USB link type so the UI shows whether a
                // camera is on USB 2.0 (480 Mb/s ceiling → uncompressed HD
                // starves) vs USB 3.x. Best-effort; omitted if unreadable.
                const std::string usb = usbLinkTag(i);
                if (!usb.empty())
                    name += " — " + usb;
                devices.emplace_back(i, std::move(name));
            }
        }
        ::close(fd);
    }
#else
    for (int i = 0; i < 10; ++i) {
        cv::VideoCapture cap;
        cap.open(i, cv::CAP_V4L2);
        if (cap.isOpened()) {
            devices.emplace_back(i, "Camera " + std::to_string(i));
            cap.release();
        }
    }
#endif

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
                // isOpened() lies for this pipeline: nvvidconv needs an EGL
                // display, and in a headless container it fails with "No EGL
                // Display" / "Could not get EGL display connection". The caps
                // negotiation then collapses (width/height come back as -1) and
                // NO frames ever flow — yet cap.isOpened() still reports true.
                // Validate that the pipeline actually produces a frame before
                // trusting it; otherwise release it and fall back to CPU V4L2.
                bool alive = false;
                for (int i = 0; i < 10; ++i) {
                    cv::Mat probe;
                    if (cap.read(probe) && !probe.empty()) { alive = true; break; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (alive) {
                    spdlog::info("Camera opened with GStreamer (nvv4l2decoder, HW MJPG decode)");
                    opened = true;
                    openedViaGst = true;
                } else {
                    spdlog::warn("GStreamer HW pipeline opened but delivered no frames "
                                 "(likely 'No EGL Display' in a headless container) — "
                                 "falling back to CPU V4L2 decode");
                    cap.release();
                }
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

#ifdef __linux__
    // The /dev/video<N> index is NOT stable: after a USB re-enumeration (e.g.
    // the bus collapse on a backend switch), the microscope can move from
    // video6 to video0, leaving the configured index pointing at nothing. If
    // the configured index failed, fall back to the first real capture device
    // that is not a RealSense node (those must go through the RS SDK, not V4L2).
    if (!opened) {
        const auto devs = listDevices();
        for (const auto& [idx, label] : devs) {
            if (idx == m_deviceIndex) continue;                 // already tried
            if (label.find("RealSense") != std::string::npos) continue;
            spdlog::warn("Configured device {} unavailable — falling back to "
                         "detected camera {}: {}", m_deviceIndex, idx, label);
            bool result = cap.open(idx, cv::CAP_V4L2);
            if (result && cap.isOpened()) {
                spdlog::info("Camera opened on fallback device {} (V4L2)", idx);
                m_deviceIndex = idx;                            // adopt the live index
                opened = true;
                break;
            }
        }
    }
#endif

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

        // Re-apply MJPG AFTER the resolution. Several UVC drivers (and OpenCV's
        // V4L2 backend) reset the pixel format back to raw YUYV when the frame
        // size changes, silently undoing the FOURCC set above. Setting it a
        // second time here is what actually makes MJPG stick on those cameras —
        // without it the microscope streams uncompressed YUYV, which saturates
        // the USB bus (select() timeouts, ~5-10 fps, and bus instability that
        // can collapse the whole controller on a backend switch).
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
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

    // If the camera ignored OpenCV's MJPG hint and stayed on a raw format, the
    // uncompressed stream starves the USB bus at 720p+ (select() timeouts, ~5-10
    // fps, controller instability). cv::VideoCapture's CAP_PROP_FOURCC is simply
    // unreliable on some UVC drivers (the HAYEAR microscope is one). The robust
    // fix is to re-open through a CPU GStreamer pipeline whose explicit
    // `image/jpeg` caps force MJPG negotiation, decoded by jpegdec (no EGL/NVDEC,
    // so it works in a headless container where the HW pipeline can't).
    if (!openedViaGst && std::string(fourccStr) != "MJPG" && gstreamerAvailable()) {
        const std::string pipeline =
            buildGstPipelineCpu(m_deviceIndex, m_width, m_height, m_fps);
        spdlog::warn("Camera negotiated '{}' (not MJPG) via V4L2 — retrying with a "
                     "CPU MJPG GStreamer pipeline to force compression:\n  {}",
                     fourccStr, pipeline);
        cv::VideoCapture gst;
        if (gst.open(pipeline, cv::CAP_GSTREAMER) && gst.isOpened()) {
            bool alive = false;
            for (int i = 0; i < 20; ++i) {
                cv::Mat probe;
                if (gst.read(probe) && !probe.empty()) { alive = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (alive) {
                spdlog::info("Camera re-opened with CPU MJPG GStreamer pipeline "
                             "(forced image/jpeg, jpegdec)");
                cap.release();
                cap = std::move(gst);
                openedViaGst = true;   // appsink delivers decoded BGR already
            } else {
                spdlog::warn("CPU MJPG GStreamer pipeline produced no frames — "
                             "keeping the raw V4L2 stream (expect bandwidth issues)");
                gst.release();
            }
        } else {
            spdlog::warn("CPU MJPG GStreamer pipeline failed to open — keeping the "
                         "raw V4L2 stream (expect bandwidth issues)");
        }
    }

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
