#pragma once

#include <QApplication>
#include <QTimer>
#include <QThread>
#include <opencv2/core.hpp>
#include <memory>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_set>

namespace ibom {

class Config;
enum class CameraBackend;
struct IBomProject;
class IBomParser;

namespace gui {
class MainWindow;
}

namespace camera {
class ICameraSource;
class CameraCapture;
class CameraCalibration;
}

namespace ai {
class InferenceEngine;
class ModelManager;
class ComponentDetector;
}

namespace overlay {
class OverlayRenderer;
class Homography;
class HeatmapRenderer;
class TrackingWorker;
}

namespace features {
class PickAndPlace;
class Measurement;
class SnapshotHistory;
class DatasetCreator;
class RemoteView;
}

namespace exports {
class DataExporter;
}

/**
 * @brief Main application controller.
 *
 * Owns all subsystems and manages their lifecycle:
 * camera capture, AI inference, iBOM data, overlay, GUI.
 */
class Application : public QObject {
    Q_OBJECT

public:
    explicit Application(QApplication& qapp);
    ~Application() override;

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// Initialize all subsystems, returns false on failure.
    bool initialize();

    /// Access the global config.
    Config& config();
    const Config& config() const;

    /// Component detector, or nullptr while AI is disabled / still
    /// initializing / no model found. Only valid once aiStatusChanged(true)
    /// has been emitted; call from the GUI thread only.
    ai::ComponentDetector* componentDetector() const
    {
        return m_aiReady.load() ? m_componentDetector.get() : nullptr;
    }

signals:
    void shutdownRequested();
    void ibomLoaded(int componentCount);

    /// AI pipeline state. ready=true once the ONNX session is created and the
    /// detector model is loaded (first launch with TensorRT compiles the
    /// engine — can take minutes; runs off the GUI thread).
    void aiStatusChanged(bool ready, QString message);

    /// Emitted after a successful profile switch with the new active index.
    void cameraProfileChanged(int idx);

private:
    void setupLogging();
    void parseCommandLine();
    void createSubsystems();
    void connectSignals();
    /// Continuation of connectSignals() (control panel, BOM, inspection, …).
    /// Split out only so the large camera frameReady lambda can live in its
    /// own re-callable wireCameraSignals() unit.
    void connectControlSignals();
    void initializeAI();

    /// Instantiate the camera backend selected in Config (V4L2 microscope or
    /// RealSense) and apply the current resolution/fps/device settings. Does
    /// NOT wire signals — call wireCameraSignals() after.
    void createCamera();
    /// Connect the current m_camera's frameReady/captureError to the app.
    /// Re-callable: a freshly created backend just needs this run again.
    void wireCameraSignals();
    /// Stop, destroy and recreate the camera for a new backend, then re-wire
    /// and restart if it was capturing. Used by the Settings backend selector.
    void switchCameraBackend(CameraBackend backend);
    /// Switch to a named camera profile, saving/restoring per-profile tracking state.
    void switchProfile(int profileIndex);
    /// Arm microscope 1-point anchoring on the currently selected component.
    /// The next image click sets the homography from that single correspondence,
    /// using the live scale (or the configured microscope fallback) and rotation.
    void startComponentAnchor();
    /// Enumerate devices for the active backend and refresh the ControlPanel.
    void refreshCameraDeviceList();
    /// Open the dynamic RealSense sensor-controls panel (from ControlPanel or
    /// the Settings dialog). Shows a hint if the backend isn't RealSense / the
    /// camera isn't running.
    void openRealSenseControls();
    void updateCalibrationUI();

    void loadIBomFile(const QString& path);
    void refreshRecentFilesMenu();
    void applyRemoteViewConfig();
    /// Persist m_placedRefs for the current iBOM (session_state.json,
    /// keyed by iBOM path). An empty set removes the entry.
    void saveInspectionState();
    /// Read back the placed refs saved for the current iBOM, if any.
    std::unordered_set<std::string> loadSavedPlacedRefs() const;
    void runCalibration();
    void takeScreenshot();
    void updateDynamicScale();
    void startInspection();
    void onExport(const QString& format);
    void onSnapshot();

    QApplication&                               m_qapp;
    std::unique_ptr<Config>                    m_config;
    std::unique_ptr<gui::MainWindow>           m_mainWindow;
    std::unique_ptr<camera::ICameraSource>     m_camera;
    CameraBackend                              m_activeBackend{};  // backend of the live m_camera
    std::unique_ptr<ai::InferenceEngine>       m_inferenceEngine;
    std::unique_ptr<ai::ModelManager>          m_modelManager;
    std::unique_ptr<ai::ComponentDetector>     m_componentDetector;
    std::thread                                m_aiInitThread;   // joined in dtor
    std::atomic<bool>                          m_aiReady{false};

    // iBOM data pipeline
    std::unique_ptr<IBomParser>                m_ibomParser;
    std::shared_ptr<IBomProject>               m_ibomProject;

    // Overlay rendering
    std::unique_ptr<overlay::OverlayRenderer>  m_overlayRenderer;
    std::unique_ptr<overlay::Homography>       m_homography;
    std::unique_ptr<overlay::HeatmapRenderer>  m_heatmapRenderer;

    // Camera calibration
    std::unique_ptr<camera::CameraCalibration> m_calibration;

    // Inspection workflow features
    std::unique_ptr<features::PickAndPlace>     m_pickAndPlace;
    std::unique_ptr<features::Measurement>      m_measurement;
    std::unique_ptr<features::SnapshotHistory>  m_snapshotHistory;
    std::unique_ptr<exports::DataExporter>      m_dataExporter;

    // Remote browser view (WebSocket MJPEG) — created on demand when
    // features.remote_view is enabled (config or Settings dialog).
    std::unique_ptr<features::RemoteView>       m_remoteView;

    // FPS tracking
    QTimer* m_fpsTimer = nullptr;
    std::atomic<int> m_frameCount{0};

    // Focus assist — last time the sharpness metric was computed (throttle).
    qint64 m_lastSharpnessMs = 0;
    // Depth (RealSense) — last time distance/scale was computed (throttle).
    qint64 m_lastDepthMs = 0;
    // Live view mode: false = color image, true = colorized depth map.
    // Only meaningful for the RealSense backend (depth stream).
    bool m_depthViewMode = false;
    // 3D point cloud mode: central view shows the orbitable cloud (RealSense).
    // The cloud itself is built on the capture thread (rs2::pointcloud).
    bool m_pointCloudMode = false;

    // Calibration image collection
    std::vector<cv::Mat> m_calibImages;
    bool m_collectingCalibImages = false;

    // Selected component ref for overlay highlight
    std::string m_selectedRef;

    // Components already placed during the current inspection — used to render
    // them in a faded "done" color in the overlay.
    std::unordered_set<std::string> m_placedRefs;

    // Heatmap visibility
    bool m_showHeatmap = false;

    // Manual homography point picking
    bool m_pickingHomographyPoints = false;
    std::vector<cv::Point2f> m_homographyImagePoints;

    // Microscope 1-point anchor mode (see docs/MICROSCOPE_PLACEMENT_PLAN.md §2).
    // Armed by startComponentAnchor() using the currently selected ref; the next
    // image click builds a similarity homography (known scale + rotation) so the
    // target component lands at the clicked point.
    bool        m_anchorMode = false;
    std::string m_anchorRef;
    cv::Point2f m_anchorPcb;

    // 2-component alignment mode
    bool m_alignOnComponents = false;
    int  m_alignCompStep = 0;  // 0=select comp1, 1=click comp1, 2=select comp2, 3=click comp2
    std::string m_alignRef1;
    std::string m_alignRef2;
    cv::Point2f m_alignPcb1;
    cv::Point2f m_alignPcb2;
    cv::Point2f m_alignImg1;
    cv::Point2f m_alignImg2;

    // Live tracking mode — ORB work happens on m_trackingThread via m_trackingWorker.
    bool     m_liveMode = false;
    cv::Mat  m_baseHomography;  // Original homography before live tracking
    QThread* m_trackingThread = nullptr;            // owned by Application (QObject parent)
    overlay::TrackingWorker* m_trackingWorker = nullptr;  // lives on m_trackingThread

    // Dataset capture (Phase A) — JPEG encoding + label projection run on a
    // dedicated thread, same ownership pattern as the tracking worker.
    QThread* m_datasetThread = nullptr;
    features::DatasetCreator* m_datasetCreator = nullptr;  // lives on m_datasetThread

    // Dynamic scale tracking
    double m_basePixelsPerMm = 0.0;  // pixelsPerMm at initial homography
    double m_currentPixelsPerMm = 0.0;

    // Per-profile tracking state (saved/restored on profile switch)
    struct ProfileTrackingState {
        cv::Mat  baseHomography;   // m_baseHomography snapshot
        cv::Mat  liveHomography;   // m_homography->matrix() snapshot
        double   pixelsPerMm = 0.0;
        bool     liveMode    = false;
    };
    std::vector<ProfileTrackingState> m_profileStates{2};
};

} // namespace ibom
