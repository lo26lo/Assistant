#pragma once

#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QImage>
#include <QSize>
#include <QTransform>
#include <QFutureWatcher>
#include <opencv2/core.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_set>

#include "camera/ICameraSource.h"  // for ibom::camera::FrameRef/DepthFrameRef

namespace ibom {

class Config;
enum class CameraBackend;
struct IBomProject;
struct Component;
class IBomParser;

namespace gui {
class MainWindow;
class CalibrationMonitorDialog;
class AlignmentWizard;
class MultiAlignDialog;
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
    /// Locate the board outline in the most recent camera frame (BoardLocator)
    /// and, on success, set the homography from it directly — no clicking
    /// required. Runs on a worker thread (QtConcurrent) since edge detection +
    /// orientation scoring can take tens of ms. See docs/AUTO_ALIGN_PLAN.md.
    /// @param silent  When true (periodic auto re-anchor), suppresses all UI
    ///                (no status messages / dialogs) and only applies the result
    ///                if it is confident (score ≥ reanchorMinScore) AND disagrees
    ///                enough with the current pose to be worth correcting drift,
    ///                so healthy live tracking is left undisturbed.
    /// @param isRetry Internal: this call is an automatic retry of a failed
    ///                interactive attempt — don't re-arm the retry budget.
    ///                Interactive not-found results retry up to 2 more times on
    ///                fresh frames (~300 ms apart) before reporting failure, so
    ///                one badly-timed frame (blur, glare, hand) doesn't fail
    ///                the whole click.
    void autoAlignBoard(bool silent = false, bool isRetry = false);

    /// Flip the ControlPanel's Live Tracking checkbox on after a successful
    /// alignment (any path: 4-corner, 2-comp, multi-comp, anchor, Auto-Align,
    /// component re-anchor) so alignment flows straight into tracking. Goes
    /// through the checkbox → liveModeChanged → the normal enable handler, so
    /// UI and state stay in sync. No-op when already live (the alignment
    /// paths rebase the running tracker themselves) or when the homography
    /// isn't valid.
    void autoStartLiveTracking();

    /// Self-re-arming recovery poll started when live tracking reports LOST:
    /// as long as the state stays Lost (and live mode is on), periodically
    /// re-locate the board outright via component re-anchor (trained model or
    /// model-free blobs) — independent of the reanchor_enabled
    /// periodic-correction setting (this is loss RECOVERY, not drift
    /// correction). Stops as soon as tracking recovers.
    void attemptLostRecovery();
    /// Component-level re-anchor (docs/AI_MODEL_DATASETS_PLAN.md, "Piste B"):
    /// detects components on the current frame (trained model when loaded,
    /// else model-free blobs), matches detections to expected iBOM positions
    /// (current pose as prior, global bootstrap otherwise) and re-fits the
    /// pose via RANSAC — a similarity fit for blob detections, so the pose is
    /// repeatable enough for the periodic drift gate
    /// (docs/BLOB_REANCHOR_JITTER_ANALYSE.md). Works when the board fills the
    /// frame — exactly where the geometric BoardLocator path (autoAlignBoard)
    /// fails. @param silent as in autoAlignBoard().
    void componentReanchor(bool silent = false);
    /// Start/stop/retune the periodic geometric re-anchor timer from Config
    /// (features: reanchor_enabled / reanchor_interval_s).
    void updateReanchorTimer();
    /// Finish multi-component alignment: fit a transform from the collected
    /// PCB↔image landmark pairs (≥4 → homography, 3 → affine, 2 → similarity)
    /// and apply it like the other alignment paths. No-op (with a message) if
    /// fewer than 2 landmarks were marked.
    void applyMultiAlignment();
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
    /// Push the current camera/calibration state into the live calibration
    /// monitor (no-op if the monitor was never opened).
    void pushCalibrationMonitorState();
    /// Write a full snapshot of the configuration + runtime state to the log
    /// (every tunable: camera, tracking, optical flow, re-anchor, overlay,
    /// scale, calibration, AI, alignment). Logged at info so it is always
    /// recorded; toggled verbose logging adds the per-frame debug stream.
    void logFullState();
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

    // Overlay rendering (the draw path is the stateless
    // overlay::OverlayRenderer::renderBoardSpace(), re-run from frameReady only
    // when its inputs change; CameraView warps the buffer per paint — there is
    // no renderer instance to own).
    std::unique_ptr<overlay::Homography>       m_homography;
    std::unique_ptr<overlay::HeatmapRenderer>  m_heatmapRenderer;

    // Camera calibration
    std::unique_ptr<camera::CameraCalibration> m_calibration;

    // Dev tool: live calibration monitor pop-up (created lazily on first open,
    // then kept alive so it keeps buffering warnings).
    std::unique_ptr<gui::CalibrationMonitorDialog> m_calibMonitor;

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

    // Periodic re-anchor (plan B): when enabled + live tracking, a timer runs
    // a silent componentReanchor() to correct accumulated drift (trained
    // model or model-free blobs — blob poses use a similarity fit to stay
    // under the drift gate, docs/BLOB_REANCHOR_JITTER_ANALYSE.md). Off by
    // default. See docs/JETSON_SESSION_LOG.md.
    QTimer* m_reanchorTimer = nullptr;
    int     m_reanchorFailStreak = 0;   // consecutive BoardLocator misses → back off
    int     m_reanchorTickCount  = 0;   // for skipping ticks while backing off

    // Interactive Auto-Align retry budget (see autoAlignBoard's isRetry doc).
    int  m_autoAlignRetriesLeft = 0;
    // Loss-recovery poll state (see attemptLostRecovery): last state reported
    // by trackingStateChanged, and whether a recovery chain is already armed
    // (Lost can be signalled repeatedly — only one poll chain must run).
    int  m_lastTrackingState  = -1;
    bool m_lostRecoveryArmed  = false;
    // Consecutive loss-recovery ticks (attemptLostRecovery): drives the
    // back-off from 3 s → 15 s once geometric re-anchor proves hopeless on a
    // detector-less, board-fills-the-frame scene.
    int  m_lostRecoveryAttempts = 0;

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

    // Most recent color/depth frames, cached for on-demand use (Auto-Align —
    // see autoAlignBoard()). Both are immutable shared views (zero-copy), so
    // simply holding the shared_ptr is safe across the throttled frameReady
    // lambda's lifetime; no clone needed.
    ibom::camera::FrameRef      m_lastColorFrame;
    ibom::camera::DepthFrameRef m_lastDepthFrame;
    bool m_autoAligning = false;  // guards against re-entrant Auto-Align clicks

    // Median depth (mm) over the central ROI, refreshed by the RealSense
    // depthFrameReady handler regardless of Config::scaleMethod() — lets
    // autoAlignBoard() derive a fresh pixels-per-mm estimate from pinhole
    // geometry (fx / distance) instead of relying on a possibly stale
    // checkerboard calibration done at a different working distance/camera.
    double m_lastDepthDistanceMm = 0.0;

    // Bumped at the start of every alignment-applying action (manual 4-point,
    // 2-component, anchor, and Auto-Align). Auto-Align's worker-thread result
    // captures the value in flight and skips applying itself if a newer
    // alignment already landed by the time it finishes.
    uint64_t m_alignmentEpoch = 0;

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

    // Multi-component alignment (≥2 landmarks). Works on non-rectangular boards
    // since it uses component positions, not board corners. Each landmark is
    // marked either by clicking 2 opposite body corners (the midpoint is used,
    // matched to the component bbox center) or by a single click on pin 1
    // (matched to the iBOM pin-1 pad position). ≥4 landmarks → full homography
    // (corrects perspective/tilt); 3 → affine; 2 → similarity.
    bool m_alignMulti = false;
    bool m_alignMultiAwaitClick = false;  // a component is chosen, waiting image click(s)
    int  m_alignMultiMethod = 0;          // 0 = 2 body corners (midpoint), 1 = pin 1, 2 = 2 farthest pads (midpoint)
    std::string m_alignMultiRef;          // component currently being marked
    cv::Point2f m_alignMultiPcb;          // its known PCB point (center or pin 1)
    bool m_alignMultiHaveCorner1 = false; // corners method: first corner captured
    cv::Point2f m_alignMultiCorner1;
    std::vector<cv::Point2f> m_alignMultiPcbPts;
    std::vector<cv::Point2f> m_alignMultiImgPts;
    std::vector<std::string> m_alignMultiRefs;

    // Guided alignment assistant (created lazily, parented to the main window).
    // When non-null and visible, alignment-completion sites forward their
    // human-readable summary to it via reportAlignmentResult().
    gui::AlignmentWizard* m_alignWizard = nullptr;
    // Persistent non-modal panel driving the multi-component alignment flow
    // (method choice + live progress). Created lazily on first use.
    gui::MultiAlignDialog* m_multiAlignDialog = nullptr;
    /// Forward an alignment summary to the wizard (if any) and the status bar.
    void reportAlignmentResult(const QString& summary);
    /// Keep the ControlPanel button and (if open) the wizard's run page in sync
    /// with whether multi-component landmark collection is in progress.
    void setMultiAlignUIState(bool collecting);

    /// Sub-pixel snap of a hand-clicked Multi-Comp landmark point (corner or
    /// pin-1 click) onto the nearest strong local feature, via
    /// cv::cornerSubPix in a small window around the click — improves
    /// precision over the raw click position, which is limited by mouse/
    /// screen resolution. Falls back to the original point if no frame is
    /// available or the window has no usable corner (e.g. a flat pad
    /// center); never moves the point further than searchRadiusPx.
    cv::Point2f refineClickPoint(cv::Point2f rawPoint, int searchRadiusPx = 8) const;

    /// Arm the given component as the next Multi-Comp landmark using the
    /// session-wide marking method (m_alignMultiMethod). Computes its PCB
    /// anchor (pin-1 pad / midpoint of the two farthest pads / bbox center),
    /// reflects the selection in every UI surface, and sets m_alignMultiAwaitClick.
    /// Callable repeatedly: selecting another component (from the BOM panel OR
    /// the minimap) before finishing the image click(s) simply switches the
    /// target — no need to cancel. No-op if the component is unusable for the
    /// chosen method (e.g. pin-1 method on a part with no pin-1 pad).
    void beginMarkComponent(const std::string& ref);

    /// Resolve a PCB-coordinate point (e.g. a PCB Map click) to a component:
    /// the smallest Front-layer component whose bbox contains the point, or
    /// the nearest component centre if the click lands on bare board. Returns
    /// nullptr when no iBOM is loaded. Used by the minimap selection paths.
    const Component* componentAtPcb(cv::Point2f pcbPt) const;

    /// Create (lazily) and show the persistent non-modal multi-align panel,
    /// wiring its method/finish/cancel signals to the alignment flow.
    void showMultiAlignDialog();

    // Live tracking mode — ORB work happens on m_trackingThread via m_trackingWorker.
    bool     m_liveMode = false;
    cv::Mat  m_baseHomography;  // Original homography before live tracking
    QThread* m_trackingThread = nullptr;            // owned by Application (QObject parent)
    overlay::TrackingWorker* m_trackingWorker = nullptr;  // lives on m_trackingThread

    // Dataset capture (Phase A) — JPEG encoding + label projection run on a
    // dedicated thread, same ownership pattern as the tracking worker.
    QThread* m_datasetThread = nullptr;
    features::DatasetCreator* m_datasetCreator = nullptr;  // lives on m_datasetThread

    // ── iBOM overlay render cache (change-gated) ───────────────────────────
    // The overlay is rendered in BOARD space (OverlayRenderer::renderBoardSpace)
    // and only rebuilt when one of its content inputs changes: selection, placed
    // set, toggles, colors, loaded project. The homography is NOT part of the
    // signature anymore — pose changes just update the warp transform pushed to
    // CameraView every frame. The m_ovSig* fields are the signature of the
    // inputs the cached buffer was last rendered from.
    bool        m_overlayValid    = false;   // false until first render
    std::string m_ovSigSelected;
    std::size_t m_ovSigPlacedHash = 0;
    bool        m_ovSigPads = false, m_ovSigSilk = false;
    std::string m_ovSigColorKey;
    float       m_ovSigPlacedOpacity = -1.0f;
    float       m_ovSigSelectedSilkW = -1.0f;
    const void* m_ovSigProject = nullptr;    // identity of the rendered IBomProject
    // buffer→PCB mapping of the current board buffer (inverse of the renderer's
    // pcbToBuffer), composed with the live homography into the per-frame warp.
    QTransform  m_boardBufferToPcb;
    // Whether the picking-feedback overlay is currently shown, so it can be
    // cleared exactly once when picking ends (it owns the full-frame overlay
    // channel now that the iBOM overlay lives in the board channel).
    bool        m_pickOverlayShown = false;

    // Dynamic scale tracking
    double m_basePixelsPerMm = 0.0;  // pixelsPerMm at initial homography
    double m_currentPixelsPerMm = 0.0;
    // IBomPads scale method: cached far-apart reference pad pair, recomputed
    // only when the loaded project changes (ERREUR #52). Positions are copied
    // by value; the project pointer is an identity tag, never dereferenced.
    const void* m_scaleRefProject = nullptr;
    cv::Point2f m_scaleRefA, m_scaleRefB;   // PCB coords (mm)
    double      m_scaleRefDistMm = 0.0;
    qint64      m_lastScaleUpdateMs = 0;    // throttle for the live-tracking path

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
