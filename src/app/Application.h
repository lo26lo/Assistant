#pragma once

#include <QApplication>
#include <memory>
#include <string>

namespace ibom {

class Config;

namespace gui {
class MainWindow;
}

namespace camera {
class CameraCapture;
}

namespace ai {
class InferenceEngine;
class ModelManager;
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
    explicit Application(int& argc, char** argv);
    ~Application() override;

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// Initialize all subsystems, returns false on failure.
    bool initialize();

    /// Run the application event loop.
    int exec();

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

    std::unique_ptr<QApplication>              m_qapp;
    std::unique_ptr<Config>                    m_config;
    std::unique_ptr<gui::MainWindow>           m_mainWindow;
    std::unique_ptr<camera::CameraCapture>     m_camera;
    std::unique_ptr<ai::InferenceEngine>       m_inferenceEngine;
    std::unique_ptr<ai::ModelManager>          m_modelManager;

    int    m_argc;
    char** m_argv;
};

} // namespace ibom
