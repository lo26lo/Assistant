#include "Application.h"
#include "Config.h"
#include "gui/MainWindow.h"
#include "camera/CameraCapture.h"
#include "ai/InferenceEngine.h"
#include "ai/ModelManager.h"
#include "utils/Logger.h"

#include <spdlog/spdlog.h>
#include <QCommandLineParser>

namespace ibom {

Application::Application(int& argc, char** argv)
    : m_argc(argc)
    , m_argv(argv)
{
}

Application::~Application() = default;

bool Application::initialize()
{
    // Qt application must be created first
    m_qapp = std::make_unique<QApplication>(m_argc, m_argv);
    m_qapp->setApplicationName("MicroscopeIBOM");
    m_qapp->setApplicationVersion("0.1.0");
    m_qapp->setOrganizationName("MicroscopeIBOM");

    // Initialize logging
    setupLogging();
    spdlog::info("MicroscopeIBOM v{} starting...", "0.1.0");

    // Load configuration
    m_config = std::make_unique<Config>();
    m_config->load();

    // Parse command line (may override config)
    parseCommandLine();

    // Create all subsystems
    createSubsystems();

    // Wire signals between subsystems
    connectSignals();

    // Show main window
    m_mainWindow->show();

    spdlog::info("Application initialized successfully.");
    return true;
}

int Application::exec()
{
    return m_qapp->exec();
}

Config& Application::config()
{
    return *m_config;
}

const Config& Application::config() const
{
    return *m_config;
}

void Application::setupLogging()
{
    utils::Logger::initialize("MicroscopeIBOM");
}

void Application::parseCommandLine()
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Microscope iBOM AI Overlay");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption ibomFileOption("ibom", "Load iBOM HTML file", "file");
    QCommandLineOption cameraOption("camera", "Camera device index", "index", "0");
    QCommandLineOption darkModeOption("dark", "Enable dark mode");

    parser.addOption(ibomFileOption);
    parser.addOption(cameraOption);
    parser.addOption(darkModeOption);

    parser.process(*m_qapp);

    if (parser.isSet(ibomFileOption)) {
        m_config->setIBomFilePath(parser.value(ibomFileOption).toStdString());
    }
    if (parser.isSet(cameraOption)) {
        m_config->setCameraIndex(parser.value(cameraOption).toInt());
    }
    if (parser.isSet(darkModeOption)) {
        m_config->setDarkMode(true);
    }
}

void Application::createSubsystems()
{
    spdlog::debug("Creating subsystems...");

    // Model manager (loads/caches AI models)
    m_modelManager = std::make_unique<ai::ModelManager>(m_config->modelsPath());

    // AI inference engine
    m_inferenceEngine = std::make_unique<ai::InferenceEngine>(*m_modelManager);

    // Camera capture
    m_camera = std::make_unique<camera::CameraCapture>(m_config->cameraIndex());

    // Main window (owns GUI widgets, overlay renderer, etc.)
    m_mainWindow = std::make_unique<gui::MainWindow>(
        *m_config, *m_camera, *m_inferenceEngine
    );
}

void Application::connectSignals()
{
    connect(this, &Application::shutdownRequested, m_qapp.get(), &QApplication::quit);
}

} // namespace ibom
