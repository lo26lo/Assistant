#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace ibom {

/// Capture backend selecting which camera implementation to instantiate.
enum class CameraBackend {
    V4L2      = 0,  // USB/UVC microscope via OpenCV V4L2 (default)
    RealSense = 1   // Intel RealSense (e.g. D405) via librealsense2
};

/// Method for dynamic pixels/mm scaling when zoom changes.
enum class ScaleMethod {
    None        = 0,  // Use calibration value only (fixed)
    Homography  = 1,  // Extract scale factor from live tracking homography
    IBomPads    = 2   // Compute from known iBOM pad distances
};

/// Order in which components are presented during inspection.
enum class SortMethod {
    ValueCount       = 0,  // Most numerous value group first — minimizes SMD reel changes
    ValueAlphabetic  = 1,  // Alphabetic by value, group together
    Position         = 2,  // Original iBOM load order (often matches PCB layout)
    FootprintSize    = 3   // Smallest footprint first
};

/**
 * @brief Persistent application configuration.
 *
 * Loaded from/saved to a JSON config file.
 * Stores camera settings, AI model paths, UI preferences, etc.
 */
class Config {
public:
    Config();
    ~Config() = default;

    /// Load config from default file (or create defaults).
    bool load(const std::string& path = "");

    /// Save current config to file.
    bool save(const std::string& path = "") const;

    // --- Camera ---
    CameraBackend cameraBackend() const { return m_cameraBackend; }
    void setCameraBackend(CameraBackend b) { m_cameraBackend = b; }

    int  cameraIndex() const { return m_cameraIndex; }
    void setCameraIndex(int index) { m_cameraIndex = index; }

    int  cameraWidth() const { return m_cameraWidth; }
    void setCameraWidth(int w) { m_cameraWidth = w; }

    int  cameraHeight() const { return m_cameraHeight; }
    void setCameraHeight(int h) { m_cameraHeight = h; }

    int  cameraFps() const { return m_cameraFps; }
    void setCameraFps(int fps) { m_cameraFps = fps; }

    /// Use NVIDIA hardware MJPG decode (GStreamer nvv4l2decoder) on Jetson.
    /// Falls back to CPU V4L2 automatically if the pipeline fails to open.
    bool cameraHwDecode() const { return m_cameraHwDecode; }
    void setCameraHwDecode(bool enabled) { m_cameraHwDecode = enabled; }

    // --- iBOM ---
    const std::string& ibomFilePath() const { return m_ibomFilePath; }
    void setIBomFilePath(const std::string& path) { m_ibomFilePath = path; }

    /// Recently opened iBOM files, most recent first (capped at 5).
    const std::vector<std::string>& recentIbomFiles() const { return m_recentIbomFiles; }
    /// Move (or insert) a path to the front of the recent list.
    void addRecentIbomFile(const std::string& path);

    /// Reload the last opened iBOM automatically at startup.
    bool autoReloadIbom() const { return m_autoReloadIbom; }
    void setAutoReloadIbom(bool e) { m_autoReloadIbom = e; }

    // --- AI Models ---
    const std::string& modelsPath() const { return m_modelsPath; }
    void setModelsPath(const std::string& path) { m_modelsPath = path; }

    /// Master switch: when false, the inference engine is never initialized
    /// (no ONNX session, no TensorRT engine compilation at startup).
    bool aiEnabled() const { return m_aiEnabled; }
    void setAiEnabled(bool enable) { m_aiEnabled = enable; }

    /// Preferred detector model name (stem of the .onnx in modelsPath).
    /// If absent from the scan, the first available model is used.
    const std::string& detectorModel() const { return m_detectorModel; }
    void setDetectorModel(const std::string& name) { m_detectorModel = name; }

    bool useTensorRT() const { return m_useTensorRT; }
    void setUseTensorRT(bool enable) { m_useTensorRT = enable; }

    float detectionConfidence() const { return m_detectionConfidence; }
    void setDetectionConfidence(float conf) { m_detectionConfidence = conf; }

    // --- UI ---
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool dark) { m_darkMode = dark; }

    float overlayOpacity() const { return m_overlayOpacity; }
    void setOverlayOpacity(float opacity) { m_overlayOpacity = opacity; }

    bool showPads() const { return m_showPads; }
    void setShowPads(bool show) { m_showPads = show; }

    bool showSilkscreen() const { return m_showSilkscreen; }
    void setShowSilkscreen(bool show) { m_showSilkscreen = show; }

    bool showFabrication() const { return m_showFabrication; }
    void setShowFabrication(bool show) { m_showFabrication = show; }

    // --- Features ---
    bool voiceControlEnabled() const { return m_voiceControl; }
    void setVoiceControlEnabled(bool e) { m_voiceControl = e; }

    bool remoteViewEnabled() const { return m_remoteView; }
    void setRemoteViewEnabled(bool e) { m_remoteView = e; }

    int remoteViewPort() const { return m_remoteViewPort; }
    void setRemoteViewPort(int port) { m_remoteViewPort = port; }

    // --- Live Tracking ---
    int  trackingIntervalMs() const { return m_trackingIntervalMs; }
    void setTrackingIntervalMs(int ms) { m_trackingIntervalMs = ms; }

    int  orbKeypoints() const { return m_orbKeypoints; }
    void setOrbKeypoints(int n) { m_orbKeypoints = n; }

    int  minMatchCount() const { return m_minMatchCount; }
    void setMinMatchCount(int n) { m_minMatchCount = n; }

    /// Lowe's ratio test threshold: keep match[0] iff its distance < ratio × match[1].
    /// Typical values 0.7–0.8; smaller = stricter.
    double matchDistanceRatio() const { return m_matchDistanceRatio; }
    void setMatchDistanceRatio(double r) { m_matchDistanceRatio = r; }

    double ransacThreshold() const { return m_ransacThreshold; }
    void setRansacThreshold(double t) { m_ransacThreshold = t; }

    /// Image downscale factor before ORB (0.1–1.0). Smaller = faster, less robust.
    float trackingDownscale() const { return m_trackingDownscale; }
    void  setTrackingDownscale(float d) { m_trackingDownscale = d; }

    // --- Calibration ---
    int  calibBoardCols() const { return m_calibBoardCols; }
    void setCalibBoardCols(int n) { m_calibBoardCols = n; }

    int  calibBoardRows() const { return m_calibBoardRows; }
    void setCalibBoardRows(int n) { m_calibBoardRows = n; }

    float calibSquareSize() const { return m_calibSquareSize; }
    void setCalibSquareSize(float mm) { m_calibSquareSize = mm; }

    ScaleMethod scaleMethod() const { return m_scaleMethod; }
    void setScaleMethod(ScaleMethod m) { m_scaleMethod = m; }

    float opticalMultiplier() const { return m_opticalMultiplier; }
    void setOpticalMultiplier(float m) { m_opticalMultiplier = m; }

    // --- Checkboxes (BOM tracking) ---
    const std::vector<std::string>& checkboxColumns() const { return m_checkboxColumns; }
    void setCheckboxColumns(const std::vector<std::string>& cols) { m_checkboxColumns = cols; }

    // --- Inspection ---
    SortMethod sortMethod() const { return m_sortMethod; }
    void setSortMethod(SortMethod m) { m_sortMethod = m; }

    /// Hex color strings ("#RRGGBB" or "#AARRGGBB") for component overlay states.
    const std::string& selectedColorHex() const { return m_selectedColorHex; }
    void setSelectedColorHex(const std::string& s) { m_selectedColorHex = s; }

    const std::string& placedColorHex() const { return m_placedColorHex; }
    void setPlacedColorHex(const std::string& s) { m_placedColorHex = s; }

    const std::string& normalColorHex() const { return m_normalColorHex; }
    void setNormalColorHex(const std::string& s) { m_normalColorHex = s; }

    /// Multiplier (0..1) applied to placed components' overlay alpha.
    float placedOpacity() const { return m_placedOpacity; }
    void setPlacedOpacity(float o) { m_placedOpacity = o; }

    /// Width (px) of the silkscreen outline for the currently-selected component.
    float selectedOutlineWidth() const { return m_selectedOutlineWidth; }
    void setSelectedOutlineWidth(float w) { m_selectedOutlineWidth = w; }

    // --- Dataset capture (Phase A — see docs/DATASET_CREATOR_PLAN.md) ---
    int    datasetMinInliers() const { return m_datasetMinInliers; }
    double datasetMaxReprojErrPx() const { return m_datasetMaxReprojErrPx; }
    /// Laplacian variance threshold below which a frame counts as blurred.
    double datasetMinSharpness() const { return m_datasetMinSharpness; }
    /// Max fraction of saturated (white) + crushed (black) pixels.
    double datasetMaxBadExposureFrac() const { return m_datasetMaxBadExposureFrac; }
    int    datasetMaxHomographyAgeMs() const { return m_datasetMaxHomographyAgeMs; }
    /// Min interval between two saved frames (≈ 2 img/s at 500 ms).
    int    datasetSaveIntervalMs() const { return m_datasetSaveIntervalMs; }
    /// Min mean displacement (px) of the projected board corners between
    /// two saved frames — enforces viewpoint variety.
    double datasetMinPoseDeltaPx() const { return m_datasetMinPoseDeltaPx; }
    /// Shrink factor applied to projected iBOM bboxes (courtyard > package).
    double datasetBboxShrink() const { return m_datasetBboxShrink; }
    int    datasetMinBoxPx() const { return m_datasetMinBoxPx; }
    double datasetMinVisibleFrac() const { return m_datasetMinVisibleFrac; }

private:
    std::string defaultConfigPath() const;

    // Camera
    CameraBackend m_cameraBackend = CameraBackend::V4L2;
    int m_cameraIndex    = 0;
    int m_cameraWidth    = 1920;
    int m_cameraHeight   = 1080;
    int m_cameraFps      = 30;
    bool m_cameraHwDecode = true;   // NVIDIA HW MJPG decode (Jetson), auto-fallback to V4L2

    // iBOM
    std::string m_ibomFilePath;
    std::vector<std::string> m_recentIbomFiles;
    bool m_autoReloadIbom = true;

    // AI
    std::string m_modelsPath = "models";
    bool  m_aiEnabled          = true;
    std::string m_detectorModel = "component_detector";
    bool  m_useTensorRT        = true;
    float m_detectionConfidence = 0.5f;

    // UI
    bool  m_darkMode       = false;
    float m_overlayOpacity = 0.7f;
    bool  m_showPads       = true;
    bool  m_showSilkscreen = true;
    bool  m_showFabrication = false;

    // Features
    bool m_voiceControl    = false;
    bool m_remoteView      = false;
    int  m_remoteViewPort  = 8080;

    // Live Tracking
    int    m_trackingIntervalMs = 200;
    int    m_orbKeypoints       = 200;    // ORB is called every intervalMs; 200 is enough @ 0.5× downscale
    int    m_minMatchCount      = 8;
    double m_matchDistanceRatio = 0.75;   // Lowe's ratio (lower = stricter)
    double m_ransacThreshold    = 3.0;
    float  m_trackingDownscale  = 0.5f;   // 1.0 = full res, 0.5 = half (default)

    // Calibration (microscope-friendly defaults: small 5cm card)
    int   m_calibBoardCols  = 7;    // inner corners cols
    int   m_calibBoardRows  = 5;    // inner corners rows
    float m_calibSquareSize = 5.0f; // mm per square
    ScaleMethod m_scaleMethod = ScaleMethod::Homography;
    float m_opticalMultiplier = 1.0f; // Lens adapter: 0.5x, 1x, 2x etc.

    // BOM
    std::vector<std::string> m_checkboxColumns = {"Sourced", "Placed"};

    // Inspection
    SortMethod  m_sortMethod          = SortMethod::ValueCount;
    std::string m_selectedColorHex    = "#00E5FF";  // bright cyan — high contrast vs PCB
    std::string m_placedColorHex      = "#48C848";  // green — matches placed status
    std::string m_normalColorHex      = "#AAAA44";  // muted gold (current default)
    float       m_placedOpacity       = 0.45f;      // dim placed comps to background
    float       m_selectedOutlineWidth = 3.0f;      // thicker silk border for selected

    // Dataset capture (defaults from docs/DATASET_CREATOR_PLAN.md §A2/§A3)
    int    m_datasetMinInliers         = 25;
    double m_datasetMaxReprojErrPx     = 3.0;
    double m_datasetMinSharpness       = 100.0;
    double m_datasetMaxBadExposureFrac = 0.05;
    int    m_datasetMaxHomographyAgeMs = 300;
    int    m_datasetSaveIntervalMs     = 500;
    double m_datasetMinPoseDeltaPx     = 15.0;
    double m_datasetBboxShrink         = 0.85;
    int    m_datasetMinBoxPx           = 12;
    double m_datasetMinVisibleFrac     = 0.6;
};

} // namespace ibom
