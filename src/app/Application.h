#pragma once

#include <QApplication>
#include <QTimer>
#include <QThread>
#include <opencv2/core.hpp>
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
class TrackingWorker;
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
    void updateDynamicScale();

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

    // Dynamic scale tracking
    double m_basePixelsPerMm = 0.0;  // pixelsPerMm at initial homography
    double m_currentPixelsPerMm = 0.0;
};

} // namespace ibom
