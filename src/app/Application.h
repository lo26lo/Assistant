#pragma once

#include <QApplication>
#include <QTimer>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <memory>
#include <string>
#include <atomic>
#include <chrono>

namespace ibom {

class Config;
struct IBomProject;
class IBomParser;

namespace gui {
class MainWindow;
}

namespace camera {
class CameraCapture;
class CameraCalibration;
}

namespace ai {
class InferenceEngine;
class ModelManager;
}

namespace overlay {
class OverlayRenderer;
class Homography;
class HeatmapRenderer;
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

signals:
    void shutdownRequested();

private:
    void setupLogging();
    void parseCommandLine();
    void createSubsystems();
    void connectSignals();

    void loadIBomFile(const QString& path);
    void runCalibration();
    void takeScreenshot();

    QApplication&                               m_qapp;
    std::unique_ptr<Config>                    m_config;
    std::unique_ptr<gui::MainWindow>           m_mainWindow;
    std::unique_ptr<camera::CameraCapture>     m_camera;
    std::unique_ptr<ai::InferenceEngine>       m_inferenceEngine;
    std::unique_ptr<ai::ModelManager>          m_modelManager;

    // iBOM data pipeline
    std::unique_ptr<IBomParser>                m_ibomParser;
    std::shared_ptr<IBomProject>               m_ibomProject;

    // Overlay rendering
    std::unique_ptr<overlay::OverlayRenderer>  m_overlayRenderer;
    std::unique_ptr<overlay::Homography>       m_homography;
    std::unique_ptr<overlay::HeatmapRenderer>  m_heatmapRenderer;

    // Camera calibration
    std::unique_ptr<camera::CameraCalibration> m_calibration;

    // FPS tracking
    QTimer* m_fpsTimer = nullptr;
    std::atomic<int> m_frameCount{0};

    // Calibration image collection
    std::vector<cv::Mat> m_calibImages;
    bool m_collectingCalibImages = false;

    // Selected component ref for overlay highlight
    std::string m_selectedRef;

    // Heatmap visibility
    bool m_showHeatmap = false;

    // Manual homography point picking
    bool m_pickingHomographyPoints = false;
    std::vector<cv::Point2f> m_homographyImagePoints;

    // Live tracking mode
    bool    m_liveMode = false;
    cv::Mat m_referenceFrame;
    std::vector<cv::KeyPoint> m_refKeypoints;
    cv::Mat m_refDescriptors;
    cv::Ptr<cv::Feature2D> m_featureDetector;
    cv::Ptr<cv::DescriptorMatcher> m_featureMatcher;
    cv::Mat m_baseHomography;  // Original homography before live tracking
    std::chrono::steady_clock::time_point m_lastTrackingTime;
};

} // namespace ibom
