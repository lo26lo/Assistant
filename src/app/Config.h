#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace ibom {

/// Method for dynamic pixels/mm scaling when zoom changes.
enum class ScaleMethod {
    None        = 0,  // Use calibration value only (fixed)
    Homography  = 1,  // Extract scale factor from live tracking homography
    IBomPads    = 2   // Compute from known iBOM pad distances
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
    int  cameraIndex() const { return m_cameraIndex; }
    void setCameraIndex(int index) { m_cameraIndex = index; }

    int  cameraWidth() const { return m_cameraWidth; }
    void setCameraWidth(int w) { m_cameraWidth = w; }

    int  cameraHeight() const { return m_cameraHeight; }
    void setCameraHeight(int h) { m_cameraHeight = h; }

    int  cameraFps() const { return m_cameraFps; }
    void setCameraFps(int fps) { m_cameraFps = fps; }

    // --- iBOM ---
    const std::string& ibomFilePath() const { return m_ibomFilePath; }
    void setIBomFilePath(const std::string& path) { m_ibomFilePath = path; }

    // --- AI Models ---
    const std::string& modelsPath() const { return m_modelsPath; }
    void setModelsPath(const std::string& path) { m_modelsPath = path; }

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

private:
    std::string defaultConfigPath() const;

    // Camera
    int m_cameraIndex    = 0;
    int m_cameraWidth    = 1920;
    int m_cameraHeight   = 1080;
    int m_cameraFps      = 30;

    // iBOM
    std::string m_ibomFilePath;

    // AI
    std::string m_modelsPath = "models";
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
};

} // namespace ibom
