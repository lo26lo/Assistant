#include "Application.h"
#include "Config.h"
#include "gui/MainWindow.h"
#include "gui/CameraView.h"
#include "gui/PointCloudView.h"
#include "gui/ControlPanel.h"
#include "gui/StatsPanel.h"
#include "gui/BomPanel.h"
#include "gui/InspectionWizard.h"
#include "gui/AlignmentWizard.h"
#include "gui/MultiAlignDialog.h"
#include "gui/InspectionPanel.h"
#include "camera/ICameraSource.h"
#include "camera/CameraCapture.h"
#ifdef IBOM_HAVE_REALSENSE
#include "camera/RealSenseCapture.h"
#include "gui/RealSenseControlsDialog.h"
#endif
#include "camera/CameraCalibration.h"
#include "ai/InferenceEngine.h"
#include "ai/ModelManager.h"
#include "ai/ComponentDetector.h"
#include "ibom/IBomParser.h"
#include "ibom/IBomData.h"
#include "overlay/OverlayRenderer.h"
#include "overlay/Homography.h"
#include "overlay/HeatmapRenderer.h"
#include "overlay/TrackingWorker.h"
#include "overlay/BoardLocator.h"
#include "overlay/ComponentReanchor.h"
#include "features/PickAndPlace.h"
#include "features/Measurement.h"
#include "features/SnapshotHistory.h"
#include "features/DatasetCreator.h"
#include "features/RemoteView.h"
#include "gui/DatasetPanel.h"
#include "gui/BoardMinimap.h"
#include "gui/FovMeasureDialog.h"
#include "gui/CalibrationMonitorDialog.h"
#include "export/DataExporter.h"
#include "export/ReportGenerator.h"
#include "gui/Theme.h"
#include "utils/Logger.h"
#include "utils/QtLogSink.h"
#include "utils/Paths.h"
#include "utils/ImageUtils.h"

#include <spdlog/spdlog.h>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QMetaType>
#include <QStandardPaths>
#include <QPushButton>
#include <QPainter>
#include <QColor>
#include <QMediaDevices>
#include <QCameraDevice>
#include <QDesktopServices>
#include <QUrl>
#include <QDateTime>
#include <QFile>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <nlohmann/json.hpp>
#include <fstream>
#include <cmath>
#include <functional>
#include <limits>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>
#include <filesystem>

Q_DECLARE_METATYPE(cv::Mat)
Q_DECLARE_METATYPE(std::shared_ptr<const ibom::IBomProject>)
Q_DECLARE_METATYPE(ibom::Layer)
Q_DECLARE_METATYPE(std::vector<cv::Point2f>)

namespace ibom {

Application::Application(QApplication& qapp)
    : QObject(&qapp)
    , m_qapp(qapp)
{
}

Application::~Application()
{
    // Persist runtime tweaks made outside the settings dialog (control panel
    // sliders update Config in memory only — SettingsDialog is the only other
    // call site of save()).
    if (m_config)
        m_config->save();

    // The AI init thread owns no Qt objects; joining is safe and bounded by
    // the ONNX session creation (cannot be cancelled mid-flight anyway).
    if (m_aiInitThread.joinable())
        m_aiInitThread.join();

    if (m_trackingThread) {
        m_trackingThread->quit();
        m_trackingThread->wait();
    }

    if (m_datasetThread) {
        m_datasetThread->quit();
        m_datasetThread->wait();
    }
}

bool Application::initialize()
{
    // Register types for cross-thread signal/slot marshalling.
    qRegisterMetaType<cv::Mat>("cv::Mat");
    qRegisterMetaType<ibom::camera::FrameRef>("ibom::camera::FrameRef");
    // DepthFrameRef is the same C++ type as FrameRef; register the alias name so
    // RealSenseCapture::depthFrameReady can be queued across threads.
    qRegisterMetaType<ibom::camera::FrameRef>("ibom::camera::DepthFrameRef");
    qRegisterMetaType<ibom::camera::PointCloudRef>("ibom::camera::PointCloudRef");
    qRegisterMetaType<ibom::features::DatasetStatus>("ibom::features::DatasetStatus");
    qRegisterMetaType<std::shared_ptr<const IBomProject>>(
        "std::shared_ptr<const ibom::IBomProject>");
    qRegisterMetaType<ibom::Layer>("ibom::Layer");
    qRegisterMetaType<std::vector<cv::Point2f>>("std::vector<cv::Point2f>");

    // Logging is already initialized in main(); just flush on info for diagnostics
    spdlog::flush_on(spdlog::level::info);
    spdlog::info("MicroscopeIBOM v{} starting...", "0.1.0");

    // Load configuration
    m_config = std::make_unique<Config>();
    m_config->load();

    // Command line parsing disabled — causing exit on Windows GUI app
    // parseCommandLine();

    spdlog::info("About to create subsystems...");
    // Create all subsystems
    try {
        createSubsystems();
    } catch (const std::exception& e) {
        spdlog::critical("Exception in createSubsystems: {}", e.what());
        return false;
    } catch (...) {
        spdlog::critical("Unknown exception in createSubsystems");
        return false;
    }

    // Wire signals between subsystems
    connectSignals();

    // Apply persisted verbose-logging setting (Dev menu) at startup and reflect
    // it in the menu checkmark.
    if (m_config->verboseLogging()) {
        utils::Logger::setLevel(spdlog::level::trace);
        spdlog::info("Verbose debug logging restored from config — log file: {}",
                     utils::Logger::logFilePath());
    }
    m_mainWindow->setVerboseLoggingChecked(m_config->verboseLogging());

    // Reflect persisted AI settings in the control panel (before initializeAI
    // so the spinner already shows the threshold the detector will use).
    m_mainWindow->controlPanel()->setConfidenceThreshold(m_config->detectionConfidence());
    m_mainWindow->controlPanel()->setHybridMode(m_config->hybridDriftCorrection());

    // AI pipeline — off the GUI thread: first launch with TensorRT compiles
    // the engine (minutes); the app is fully usable without it meanwhile.
    initializeAI();

    // Enumerate cameras and populate ControlPanel. The capture pipeline opens
    // devices by their real /dev/video index via V4L2 (VIDIOC_QUERYCAP), so the
    // combo stores that index as item data and refreshCameraDeviceList() already
    // re-selects the configured device by data — no positional fix-up here.
    refreshCameraDeviceList();

    // Show main window
    m_mainWindow->show();

    // Sync profile combo to the persisted active profile.
    m_mainWindow->setActiveProfile(m_config->activeProfileIndex());

    // Pass stable homography pointer to the minimap (it lives for the app's lifetime).
    m_mainWindow->boardMinimap()->setHomography(
        m_homography.get(),
        QSize(m_config->cameraWidth(), m_config->cameraHeight()));

    // Remote browser view, if enabled in config.
    applyRemoteViewConfig();

    // Recent iBOM files menu + optional auto-reload of the last board.
    refreshRecentFilesMenu();
    if (m_config->autoReloadIbom() && !m_config->ibomFilePath().empty()) {
        const QString last = QString::fromStdString(m_config->ibomFilePath());
        if (QFileInfo::exists(last)) {
            spdlog::info("Auto-reloading last iBOM: {}", last.toStdString());
            loadIBomFile(last);
        }
    }

    spdlog::info("Application initialized successfully.");
    return true;
}

void Application::refreshRecentFilesMenu()
{
    QStringList files;
    for (const auto& f : m_config->recentIbomFiles())
        files << QString::fromStdString(f);
    m_mainWindow->setRecentFiles(files);
}

void Application::saveInspectionState()
{
    const std::string key = m_config->ibomFilePath();
    if (key.empty()) return;

    const auto statePath = utils::dataDir() / "session_state.json";

    nlohmann::json root = nlohmann::json::object();
    {
        std::ifstream ifs(statePath);
        if (ifs.good()) {
            try { root = nlohmann::json::parse(ifs); } catch (...) {}
            if (!root.is_object()) root = nlohmann::json::object();
        }
    }

    if (m_placedRefs.empty()) {
        root.erase(key);
    } else {
        nlohmann::json entry;
        entry["placed"]   = std::vector<std::string>(m_placedRefs.begin(),
                                                     m_placedRefs.end());
        entry["saved_at"] = QDateTime::currentDateTime()
                                .toString(Qt::ISODate).toStdString();
        root[key] = entry;
    }

    std::ofstream ofs(statePath);
    if (!ofs.good()) {
        spdlog::warn("Could not write inspection state to {}", statePath.string());
        return;
    }
    ofs << root.dump(2);
}

std::unordered_set<std::string> Application::loadSavedPlacedRefs() const
{
    std::unordered_set<std::string> refs;
    const std::string key = m_config->ibomFilePath();
    if (key.empty()) return refs;

    std::ifstream ifs(utils::dataDir() / "session_state.json");
    if (!ifs.good()) return refs;

    nlohmann::json root;
    try { root = nlohmann::json::parse(ifs); } catch (...) { return refs; }

    auto it = root.find(key);
    if (it == root.end() || !it->is_object() || !it->contains("placed"))
        return refs;
    for (const auto& r : (*it)["placed"])
        if (r.is_string()) refs.insert(r.get<std::string>());
    return refs;
}

void Application::applyRemoteViewConfig()
{
    const bool    enabled = m_config->remoteViewEnabled();
    const quint16 port    = static_cast<quint16>(m_config->remoteViewPort());

    if (!enabled) {
        if (m_remoteView && m_remoteView->isRunning()) {
            m_remoteView->stop();
            spdlog::info("RemoteView: disabled");
        }
        return;
    }

    if (m_remoteView && m_remoteView->isRunning() && m_remoteView->port() == port)
        return;  // already running with the requested settings

    if (!m_remoteView)
        m_remoteView = std::make_unique<features::RemoteView>();
    else if (m_remoteView->isRunning())
        m_remoteView->stop();

    if (!m_remoteView->start(port))
        return;

    // The server speaks WebSocket only (no HTTP), so the viewer page is
    // written to disk; open it in any browser, with ?host=<jetson-ip> when
    // viewing from another machine.
    const auto viewerPath = utils::dataDir() / "remote_view.html";
    QFile f(QString::fromStdString(viewerPath.string()));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(m_remoteView->generateHTMLViewer().toUtf8());
        spdlog::info("RemoteView: streaming on ws://0.0.0.0:{} — viewer page: {} "
                     "(open with ?host=<jetson-ip> from another machine)",
                     port, viewerPath.string());
    } else {
        spdlog::warn("RemoteView: could not write viewer page to {}",
                     viewerPath.string());
    }
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

    parser.process(m_qapp);

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
    spdlog::info("Creating subsystems...");

    // Model manager (loads/caches AI models)
    spdlog::info("Creating ModelManager...");
    m_modelManager = std::make_unique<ai::ModelManager>(m_config->modelsPath());

    // AI inference engine
    spdlog::info("Creating InferenceEngine...");
    m_inferenceEngine = std::make_unique<ai::InferenceEngine>(*m_modelManager);

    // Camera capture — backend chosen from config (microscope USB by default,
    // RealSense D405 when selected). Both implementations satisfy ICameraSource
    // so everything downstream is backend-agnostic.
    createCamera();

    // Camera calibration
    spdlog::info("Creating CameraCalibration...");
    m_calibration = std::make_unique<camera::CameraCalibration>();
    // Try to load existing calibration. Unified data dir (honors
    // $IBOM_DATA_DIR) so config + calibration + snapshots share one Docker
    // volume — see src/utils/Paths.h.
    auto calibPath = QString::fromStdString((utils::dataDir() / "calibration.yml").string());
    if (m_calibration->load(calibPath.toStdString())) {
        spdlog::info("Loaded camera calibration from {}", calibPath.toStdString());
    }

    // iBOM parser
    spdlog::info("Creating IBomParser...");
    m_ibomParser = std::make_unique<IBomParser>();

    // Homography (the overlay renderer is a stateless free function,
    // overlay::OverlayRenderer::renderBoardSpace — no instance to create).
    m_homography = std::make_unique<overlay::Homography>();

    // Heatmap renderer
    m_heatmapRenderer = std::make_unique<overlay::HeatmapRenderer>();

    // Live-tracking worker on its own thread. ORB + BFMatcher + RANSAC run
    // there, off the GUI thread, so heavy CV work never blocks rendering.
    m_trackingThread = new QThread(this);
    m_trackingThread->setObjectName("ORB-Tracking");
    m_trackingWorker = new overlay::TrackingWorker();
    m_trackingWorker->moveToThread(m_trackingThread);
    connect(m_trackingThread, &QThread::finished,
            m_trackingWorker, &QObject::deleteLater);
    m_trackingThread->start();
    QMetaObject::invokeMethod(m_trackingWorker, "configure", Qt::QueuedConnection,
        Q_ARG(int,    m_config->orbKeypoints()),
        Q_ARG(int,    m_config->minMatchCount()),
        Q_ARG(double, m_config->matchDistanceRatio()),
        Q_ARG(double, m_config->ransacThreshold()),
        Q_ARG(int,    m_config->trackingIntervalMs()),
        Q_ARG(float,  m_config->trackingDownscale()));
    QMetaObject::invokeMethod(m_trackingWorker, "setStabilization", Qt::QueuedConnection,
        Q_ARG(int,    m_config->trackingModel()),
        Q_ARG(double, m_config->oneEuroMinCutoff()),
        Q_ARG(double, m_config->oneEuroBeta()));
    QMetaObject::invokeMethod(m_trackingWorker, "setAdvanced", Qt::QueuedConnection,
        Q_ARG(bool, m_config->trackingClahe()),
        Q_ARG(bool, m_config->trackingOpticalFlow()),
        Q_ARG(int,  m_config->trackingGpuMode()));
    QMetaObject::invokeMethod(m_trackingWorker, "setIncrementalMode", Qt::QueuedConnection,
        Q_ARG(bool,   (m_config->cameraBackend() == CameraBackend::V4L2)
                      && m_config->microscopeIncremental()),
        Q_ARG(double, m_config->microscopeReanchorDriftPx()));
    QMetaObject::invokeMethod(m_trackingWorker, "setHybridCorrection", Qt::QueuedConnection,
        Q_ARG(bool, m_config->hybridDriftCorrection()));

    // Dataset capture worker on its own thread — JPEG writes + label
    // projection must never block the GUI (same pattern as tracking).
    m_datasetThread = new QThread(this);
    m_datasetThread->setObjectName("DatasetCapture");
    m_datasetCreator = new features::DatasetCreator();
    m_datasetCreator->moveToThread(m_datasetThread);
    connect(m_datasetThread, &QThread::finished,
            m_datasetCreator, &QObject::deleteLater);
    m_datasetThread->start();
    QMetaObject::invokeMethod(m_datasetCreator, "configure", Qt::QueuedConnection,
        Q_ARG(int,    m_config->datasetMinInliers()),
        Q_ARG(double, m_config->datasetMaxReprojErrPx()),
        Q_ARG(double, m_config->datasetMinSharpness()),
        Q_ARG(double, m_config->datasetMaxBadExposureFrac()),
        Q_ARG(int,    m_config->datasetMaxHomographyAgeMs()),
        Q_ARG(int,    m_config->datasetSaveIntervalMs()),
        Q_ARG(double, m_config->datasetMinPoseDeltaPx()),
        Q_ARG(double, m_config->datasetBboxShrink()),
        Q_ARG(int,    m_config->datasetMinBoxPx()),
        Q_ARG(double, m_config->datasetMinVisibleFrac()));
    QMetaObject::invokeMethod(m_datasetCreator, "setOutputRoot", Qt::QueuedConnection,
        Q_ARG(QString, QString::fromStdString((utils::dataDir() / "dataset").string())));
    {
        // Class rules: user override in the data dir wins, else the file
        // shipped in the repo (cwd = repo root inside the container).
        namespace fs = std::filesystem;
        const fs::path overridePath = utils::dataDir() / "footprint_classes.json";
        const fs::path defaultPath  = "resources/footprint_classes.json";
        const fs::path rulesPath = fs::exists(overridePath) ? overridePath : defaultPath;
        QMetaObject::invokeMethod(m_datasetCreator, "setClassRulesPath", Qt::QueuedConnection,
            Q_ARG(QString, QString::fromStdString(rulesPath.string())));
    }

    // Periodic re-anchor timer (plan B): during live tracking, correct
    // accumulated drift. Two strategies, the better one chosen at each tick:
    //   • a component model is loaded  -> componentReanchor() (matches AI
    //     detections to iBOM positions; works when the board fills the frame);
    //   • otherwise                    -> autoAlignBoard() (geometric outline,
    //     only works when the whole board outline is visible).
    // The timeout re-checks all conditions; updateReanchorTimer() starts/stops
    // it and sets the interval from Config.
    m_reanchorTimer = new QTimer(this);
    m_reanchorTimer->setObjectName("ReanchorTimer");
    connect(m_reanchorTimer, &QTimer::timeout, this, [this]() {
        if (!m_config->reanchorEnabled() || !m_liveMode || !m_ibomProject || m_autoAligning)
            return;
        // Back off when the geometric path keeps missing (e.g. the board fills
        // the frame, so the outline can't be separated): skip ticks in
        // proportion to the failure streak instead of running Canny +
        // orientation scoring every interval forever. The component path does
        // not have this limitation, so it ignores the back-off.
        if (componentDetector()) {
            componentReanchor(/*silent=*/true);
            return;
        }
        if (m_reanchorFailStreak > 0
            && (++m_reanchorTickCount % (m_reanchorFailStreak + 1)) != 0)
            return;
        autoAlignBoard(/*silent=*/true);
    });
    updateReanchorTimer();

    // Inspection workflow features
    spdlog::info("Creating inspection workflow features...");
    m_pickAndPlace    = std::make_unique<features::PickAndPlace>(this);
    m_measurement     = std::make_unique<features::Measurement>(this);
    m_snapshotHistory = std::make_unique<features::SnapshotHistory>(this);
    m_dataExporter    = std::make_unique<exports::DataExporter>(this);

    // Configure storage for snapshots — silent saves under the unified data dir.
    QString snapDir = QString::fromStdString((utils::dataDir() / "snapshots").string());
    m_snapshotHistory->setStorageDir(snapDir);

    // Main window (owns GUI widgets)
    spdlog::info("Creating MainWindow...");
    m_mainWindow = std::make_unique<gui::MainWindow>(this);
    spdlog::info("All subsystems created.");
}

void Application::initializeAI()
{
    if (!m_config->aiEnabled()) {
        spdlog::info("AI pipeline disabled in config (ai.enabled=false)");
        emit aiStatusChanged(false, tr("AI: disabled"));
        return;
    }

    const auto models = m_modelManager->availableModels();
    if (models.empty()) {
        spdlog::info("AI pipeline idle: no .onnx model in '{}' — drop a model "
                     "there and restart to enable detection (see docs/AI_PIPELINE.md)",
                     m_modelManager->modelsDirectory());
        emit aiStatusChanged(false, tr("AI: no model"));
        return;
    }

    // Preferred model from config, else first model found by the scan.
    std::string name = m_config->detectorModel();
    if (m_modelManager->modelPath(name).empty()) {
        spdlog::warn("AI: detector model '{}' not found, falling back to '{}'",
                     name, models.front());
        name = models.front();
    }
    const std::string path   = m_modelManager->modelPath(name);
    const bool        useTrt = m_config->useTensorRT();
    const float       conf   = m_config->detectionConfidence();

    spdlog::info("AI pipeline: initializing in background (model '{}', TensorRT={})",
                 name, useTrt);
    emit aiStatusChanged(false, tr("Loading AI model %1…")
                                    .arg(QString::fromStdString(name)));

    // Session creation + first TensorRT engine compilation can take minutes on
    // first launch — run it off the GUI thread. Emitting signals from here is
    // safe: cross-thread connections are queued by Qt. m_componentDetector is
    // only published to the GUI through the m_aiReady flag.
    m_aiInitThread = std::thread([this, path, name, useTrt, conf]() {
        if (!m_inferenceEngine->initialize(useTrt)) {
            spdlog::error("AI pipeline: inference engine initialization failed");
            emit aiStatusChanged(false, tr("AI engine initialization failed"));
            return;
        }

        auto detector = std::make_unique<ai::ComponentDetector>(*m_inferenceEngine);
        if (!detector->loadModel(path)) {
            spdlog::error("AI pipeline: failed to load model '{}'", path);
            emit aiStatusChanged(false, tr("Failed to load AI model %1")
                                            .arg(QString::fromStdString(name)));
            return;
        }
        detector->setConfidenceThreshold(conf);

        m_componentDetector = std::move(detector);
        m_aiReady.store(true);
        spdlog::info("AI pipeline ready: model '{}' loaded (TensorRT={})", name, useTrt);
        emit aiStatusChanged(true, tr("AI ready: %1")
                                       .arg(QString::fromStdString(name)));
    });
}

void Application::createCamera()
{
    // Both backends implement ICameraSource, so everything downstream (overlay,
    // tracking, dataset) is agnostic to which one is active.
#ifdef IBOM_HAVE_REALSENSE
    if (m_config->cameraBackend() == CameraBackend::RealSense) {
        spdlog::info("Creating RealSenseCapture (D405)...");
        m_camera = std::make_unique<camera::RealSenseCapture>();
    } else
#endif
    {
        spdlog::info("Creating CameraCapture (V4L2/UVC microscope)...");
        m_camera = std::make_unique<camera::CameraCapture>(m_config->cameraIndex());
    }
    m_camera->setDeviceIndex(m_config->cameraIndex());
    // RealSense keeps its own 848x480 default (optimal depth precision) unless
    // the user set a non-generic resolution — don't push the V4L2-oriented
    // 1920x1080 default onto the D405 (it would just fall back).
    const bool genericRes = (m_config->cameraWidth() == 1920 && m_config->cameraHeight() == 1080);
    if (!(m_config->cameraBackend() == CameraBackend::RealSense && genericRes))
        m_camera->setResolution(m_config->cameraWidth(), m_config->cameraHeight());
    m_camera->setFps(m_config->cameraFps());
    m_camera->setHardwareDecode(m_config->cameraHwDecode());
    m_activeBackend = m_config->cameraBackend();
}

void Application::switchCameraBackend(CameraBackend backend)
{
    if (m_activeBackend == backend && m_camera)
        return;  // no change

    const bool wasCapturing = m_camera && m_camera->isCapturing();
    if (m_camera) m_camera->stop();

    m_config->setCameraBackend(backend);
    m_config->save();

    // Destroying the old source auto-removes its Qt connections; recreate and
    // re-wire against the new backend.
    m_camera.reset();
    createCamera();
    wireCameraSignals();

    spdlog::info("Camera backend switched to {}",
                 backend == CameraBackend::RealSense ? "RealSense" : "V4L2");

    if (wasCapturing) {
        if (!m_camera->start())
            m_mainWindow->updateStatusMessage(tr("Failed to start camera after backend switch"));
    }

    // Do NOT re-enumerate the USB bus inline here. refreshCameraDeviceList()
    // calls RealSenseCapture::listDevices() → rs2::context::query_devices(),
    // which blocks the GUI thread AND probes the whole USB tree at the most
    // fragile instant — right after releasing the old camera and while the new
    // capture thread is itself bringing the device up (it enumerates too).
    // On the Jetson that double hit froze the UI and could trip the Tegra xHCI
    // controller, dropping every USB device. Defer it: by the time this fires
    // the bus has settled and the new pipeline is already streaming, so the
    // (single) enumeration is quick and safe.
    QTimer::singleShot(1500, this, [this]() { refreshCameraDeviceList(); });
}

void Application::switchProfile(int profileIndex)
{
    if (profileIndex < 0 || profileIndex >= static_cast<int>(m_config->profiles().size()))
        return;
    if (profileIndex == m_config->activeProfileIndex() && m_camera)
        return;

    // Save current tracking state for the outgoing profile
    const int outIdx = m_config->activeProfileIndex();
    if (outIdx < static_cast<int>(m_profileStates.size())) {
        auto& st = m_profileStates[outIdx];
        st.liveMode    = m_liveMode;
        st.pixelsPerMm = m_currentPixelsPerMm;
        if (m_homography && m_homography->isValid())
            st.liveHomography  = m_homography->matrix().clone();
        st.baseHomography = m_baseHomography.clone();
    }

    // Save current flat camera settings back to the outgoing profile
    m_config->saveCurrentCameraToProfile();

    // Switch to the new profile
    m_config->setActiveProfileIndex(profileIndex);
    m_config->applyActiveProfile();
    m_config->save();

    // Switch the camera backend (stops/creates/rewires/starts)
    switchCameraBackend(m_config->cameraBackend());

    // Restore tracking state for the incoming profile
    if (profileIndex < static_cast<int>(m_profileStates.size())) {
        const auto& st = m_profileStates[profileIndex];
        m_liveMode           = false;  // always start with live tracking off after profile switch
        m_currentPixelsPerMm = st.pixelsPerMm;
        m_basePixelsPerMm    = st.pixelsPerMm;
        if (!st.liveHomography.empty() && m_homography)
            m_homography->setMatrix(st.liveHomography);
        m_baseHomography = st.baseHomography.clone();
    }

    // Push the restored state into the rendering/UI components. With
    // m_liveMode forced false above, the normal homographyUpdated handler
    // won't run for this switch, so OverlayRenderer / StatsPanel scale /
    // BoardMinimap would otherwise keep showing the outgoing profile's values.
    if (auto* sp = m_mainWindow->statsPanel())
        sp->setScale(m_currentPixelsPerMm);
    if (m_mainWindow->boardMinimap())
        m_mainWindow->boardMinimap()->update();

    // Tracking mode follows the profile: the microscope (V4L2, narrow FOV) uses
    // incremental frame→frame tracking when enabled; the D405 (RealSense, wide
    // field) keeps global reference matching. See §0bis of the placement plan.
    if (m_trackingWorker) {
        const bool incremental = (m_config->cameraBackend() == CameraBackend::V4L2)
                                 && m_config->microscopeIncremental();
        QMetaObject::invokeMethod(m_trackingWorker, "setIncrementalMode",
            Qt::QueuedConnection,
            Q_ARG(bool,   incremental),
            Q_ARG(double, m_config->microscopeReanchorDriftPx()));
        QMetaObject::invokeMethod(m_trackingWorker, "setHybridCorrection",
            Qt::QueuedConnection,
            Q_ARG(bool, m_config->hybridDriftCorrection()));
    }

    emit cameraProfileChanged(profileIndex);
    spdlog::info("Switched to camera profile {}: {}", profileIndex,
                 m_config->profiles()[profileIndex].name);
}

void Application::startComponentAnchor()
{
    if (!m_ibomProject || m_ibomProject->components.empty()) {
        m_mainWindow->updateStatusMessage(tr("Load an iBOM first"));
        return;
    }
    if (m_selectedRef.empty()) {
        m_mainWindow->updateStatusMessage(
            tr("Select a component in the BOM panel, then anchor on it"));
        return;
    }

    const Component* comp = nullptr;
    for (const auto& c : m_ibomProject->components)
        if (c.reference == m_selectedRef) { comp = &c; break; }
    if (!comp) {
        m_mainWindow->updateStatusMessage(tr("Selected component not found"));
        return;
    }

    // Cancel any other interactive picking mode.
    m_alignOnComponents = false;
    m_alignCompStep = 0;
    m_pickingHomographyPoints = false;

    m_anchorRef = m_selectedRef;
    m_anchorPcb = cv::Point2f(static_cast<float>(comp->bbox.center().x),
                              static_cast<float>(comp->bbox.center().y));
    m_anchorMode = true;
    m_mainWindow->updateStatusMessage(
        tr("Anchor: CLICK on %1 in the camera image")
            .arg(QString::fromStdString(m_anchorRef)));
    spdlog::info("Anchor armed on {} at PCB ({:.2f}, {:.2f})",
                 m_anchorRef, m_anchorPcb.x, m_anchorPcb.y);
}

void Application::reportAlignmentResult(const QString& summary)
{
    m_mainWindow->updateStatusMessage(summary);
    if (m_alignWizard && m_alignWizard->isVisible())
        m_alignWizard->reportResult(summary);

    // Persist the homography so it can be offered back on the next launch if
    // the same iBOM is reloaded (see loadIBomFile()). Best-effort: there is no
    // way to know whether the camera/board actually moved since, so this is
    // just "what we had last time", not a correctness guarantee.
    if (m_homography && m_homography->isValid() && m_config) {
        Config::SavedAlignment sa;
        sa.valid         = true;
        sa.ibomFilePath  = m_config->ibomFilePath();
        sa.pixelsPerMm   = m_currentPixelsPerMm;
        sa.timestamp     = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
        const cv::Mat& m = m_homography->matrix();
        if (m.rows == 3 && m.cols == 3) {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    sa.matrix[r * 3 + c] = m.at<double>(r, c);
            m_config->setSavedAlignment(sa);
            m_config->save();
        }
    }
}

cv::Point2f Application::refineClickPoint(cv::Point2f rawPoint, int searchRadiusPx) const
{
    if (!m_lastColorFrame || m_lastColorFrame->empty())
        return rawPoint;

    const cv::Mat& frame = *m_lastColorFrame;
    if (rawPoint.x < 0 || rawPoint.y < 0 ||
        rawPoint.x >= frame.cols || rawPoint.y >= frame.rows)
        return rawPoint;

    cv::Mat gray;
    if (frame.channels() == 1) gray = frame;
    else cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

    std::vector<cv::Point2f> pts = {rawPoint};
    const int winRadius = std::max(3, searchRadiusPx / 2);
    cv::cornerSubPix(gray, pts, cv::Size(winRadius, winRadius), cv::Size(-1, -1),
                      cv::TermCriteria(cv::TermCriteria::EPS | cv::TermCriteria::COUNT, 20, 0.01));

    cv::Point2f refined = pts[0];
    const float dist = cv::norm(refined - rawPoint);
    if (!std::isfinite(refined.x) || !std::isfinite(refined.y) || dist > searchRadiusPx) {
        spdlog::debug("refineClickPoint: rejecting subpix result (dist={:.1f}px > {}px radius)",
                      dist, searchRadiusPx);
        return rawPoint;
    }
    spdlog::debug("refineClickPoint: ({:.1f},{:.1f}) -> ({:.1f},{:.1f}) [{:.2f}px]",
                  rawPoint.x, rawPoint.y, refined.x, refined.y, dist);
    return refined;
}

void Application::setMultiAlignUIState(bool collecting)
{
    m_mainWindow->controlPanel()->setAlignMultiActive(collecting);
    if (m_alignWizard) m_alignWizard->setMultiAlignCollecting(collecting);
    // Leaving collection (finish or cross-cancel) clears any pending click
    // targets drawn on the PCB Map and closes the multi-align panel.
    if (!collecting) {
        m_mainWindow->boardMinimap()->setClickTargets({});
        if (m_multiAlignDialog) m_multiAlignDialog->hide();
    }
}

void Application::showMultiAlignDialog()
{
    if (!m_multiAlignDialog) {
        m_multiAlignDialog = new gui::MultiAlignDialog(m_mainWindow.get());

        connect(m_multiAlignDialog, &gui::MultiAlignDialog::methodChanged,
                this, [this](int method) {
            m_alignMultiMethod = method;
            // Re-arm the currently selected component with the new method so the
            // change takes effect immediately (and updates the PCB Map targets).
            if (m_alignMulti && !m_selectedRef.empty())
                beginMarkComponent(m_selectedRef);
        });
        connect(m_multiAlignDialog, &gui::MultiAlignDialog::finishRequested,
                this, [this]() { applyMultiAlignment(); });
        connect(m_multiAlignDialog, &gui::MultiAlignDialog::cancelRequested,
                this, [this]() {
            if (!m_alignMulti) return;
            m_alignMulti = false;
            m_alignMultiAwaitClick = false;
            m_alignMultiHaveCorner1 = false;
            setMultiAlignUIState(false);   // clears PCB Map targets + button text
            if (m_multiAlignDialog) m_multiAlignDialog->hide();
            m_mainWindow->updateStatusMessage(tr("Multi-align cancelled"));
        });
    }
    m_multiAlignDialog->setMethod(m_alignMultiMethod);
    m_multiAlignDialog->setSelectedComponent(QString());
    m_multiAlignDialog->setStatus(tr("Pick a component to begin."));
    m_multiAlignDialog->setMarkedCount(0);
    m_multiAlignDialog->show();
    m_multiAlignDialog->raise();
    m_multiAlignDialog->activateWindow();
}

const Component* Application::componentAtPcb(cv::Point2f pcbPt) const
{
    if (!m_ibomProject) return nullptr;

    // Prefer a component whose bbox actually contains the click — this makes
    // "click the part on the PCB Map to select it" reliable even on a dense
    // board, where the nearest *center* can belong to a bigger neighbour. When
    // several overlap the point, take the smallest (most specific) one. Only
    // when the click lands on bare board do we fall back to nearest-center.
    const Component* hit = nullptr;
    double hitArea = std::numeric_limits<double>::max();
    const Component* nearest = nullptr;
    double bestDist = std::numeric_limits<double>::max();

    for (const auto& c : m_ibomProject->components) {
        if (c.layer != ibom::Layer::Front) continue;  // matches the rendered overlay
        const auto& bb = c.bbox;
        if (pcbPt.x >= bb.minX && pcbPt.x <= bb.maxX &&
            pcbPt.y >= bb.minY && pcbPt.y <= bb.maxY) {
            const double area = (bb.maxX - bb.minX) * (bb.maxY - bb.minY);
            if (area < hitArea) { hitArea = area; hit = &c; }
        }
        const double d = std::hypot(c.position.x - pcbPt.x, c.position.y - pcbPt.y);
        if (d < bestDist) { bestDist = d; nearest = &c; }
    }
    return hit ? hit : nearest;
}

void Application::beginMarkComponent(const std::string& ref)
{
    if (!m_ibomProject) return;
    const Component* comp = nullptr;
    for (const auto& c : m_ibomProject->components)
        if (c.reference == ref) { comp = &c; break; }
    if (!comp) return;

    // Reflect the selection everywhere (overlay emphasis, minimap, BOM row).
    m_selectedRef = ref;
    m_mainWindow->boardMinimap()->setSelectedRef(ref);
    if (auto* bom = m_mainWindow->bomPanel()) bom->highlightComponent(ref);

    const QString qref = QString::fromStdString(ref);
    if (m_multiAlignDialog) {
        m_multiAlignDialog->setSelectedComponent(qref);
        m_multiAlignDialog->setMethod(m_alignMultiMethod);
    }
    // Push a message to both the status bar and (if open) the multi-align panel.
    auto setStatus = [this](const QString& s) {
        m_mainWindow->updateStatusMessage(s);
        if (m_multiAlignDialog) m_multiAlignDialog->setStatus(s);
    };

    switch (m_alignMultiMethod) {
    case 1: {  // Pin 1 — single click, anchored to the iBOM pin-1 pad.
        const Pad* pin1 = nullptr;
        for (const auto& p : comp->pads) if (p.isPin1) { pin1 = &p; break; }
        if (!pin1) {
            // Don't arm — let the user pick another component without cancelling.
            m_alignMultiAwaitClick = false;
            m_mainWindow->boardMinimap()->setClickTargets({});
            setStatus(
                tr("%1 has no pin-1 pad in the iBOM — pick another component, or "
                   "switch to the 'opposite pads' / 'body corners' method.")
                .arg(qref));
            return;
        }
        m_alignMultiPcb = cv::Point2f(static_cast<float>(pin1->position.x),
                                      static_cast<float>(pin1->position.y));
        m_alignMultiHaveCorner1 = false;
        m_alignMultiAwaitClick  = true;
        m_alignMultiRef = ref;
        // Pin 1 is already drawn as the prominent RED marker on the PCB Map
        // (the selected component's pin-1 pad). The user asked to rely on that
        // red pin rather than a separate green target ring for the pin method,
        // so don't add a green click target here.
        m_mainWindow->boardMinimap()->setClickTargets({});
        setStatus(
            tr("Click PIN 1 of %1 in the image (the RED pin on the PCB Map). "
               "Or pick another component to switch.").arg(qref));
        break;
    }
    case 2: {  // Two farthest-apart pads → midpoint of the two (precise anchor).
        if (comp->pads.size() < 2) {
            m_alignMultiAwaitClick = false;
            m_mainWindow->boardMinimap()->setClickTargets({});
            setStatus(
                tr("%1 has fewer than 2 pads — pick another component, or use the "
                   "'body corners' / 'pin 1' method.").arg(qref));
            return;
        }
        // Find the pair of pads with the largest separation (footprint corners).
        double best = -1.0; const Pad* pa = nullptr; const Pad* pb = nullptr;
        for (size_t i = 0; i < comp->pads.size(); ++i)
            for (size_t j = i + 1; j < comp->pads.size(); ++j) {
                const double d = std::hypot(comp->pads[i].position.x - comp->pads[j].position.x,
                                            comp->pads[i].position.y - comp->pads[j].position.y);
                if (d > best) { best = d; pa = &comp->pads[i]; pb = &comp->pads[j]; }
            }
        if (!pa || !pb) { m_alignMultiAwaitClick = false; return; }
        m_alignMultiPcb = cv::Point2f(
            static_cast<float>((pa->position.x + pb->position.x) * 0.5),
            static_cast<float>((pa->position.y + pb->position.y) * 0.5));
        m_alignMultiHaveCorner1 = false;
        m_alignMultiAwaitClick  = true;
        m_alignMultiRef = ref;
        // Same red graphic as the pin-1 marker (user request) — these are pads.
        m_mainWindow->boardMinimap()->setClickTargets({
            cv::Point2f(static_cast<float>(pa->position.x), static_cast<float>(pa->position.y)),
            cv::Point2f(static_cast<float>(pb->position.x), static_cast<float>(pb->position.y))},
            QColor(255, 70, 70));
        setStatus(
            tr("Click the two RED target pads of %1 in the image (opposite "
               "corners of the footprint), in any order. Or pick another "
               "component to switch.").arg(qref));
        break;
    }
    default: {  // 0: two body corners → midpoint, anchored to bbox center.
        m_alignMultiPcb = cv::Point2f(static_cast<float>(comp->bbox.center().x),
                                      static_cast<float>(comp->bbox.center().y));
        m_alignMultiHaveCorner1 = false;
        m_alignMultiAwaitClick  = true;
        m_alignMultiRef = ref;
        // Mark the two diagonal corners of the body as click targets.
        m_mainWindow->boardMinimap()->setClickTargets({
            cv::Point2f(static_cast<float>(comp->bbox.minX), static_cast<float>(comp->bbox.minY)),
            cv::Point2f(static_cast<float>(comp->bbox.maxX), static_cast<float>(comp->bbox.maxY))});
        setStatus(
            tr("Click the two green target corners of %1's body in the image "
               "(order doesn't matter). Or pick another component to switch.").arg(qref));
        break;
    }
    }
}

void Application::applyMultiAlignment()
{
    // Stop collecting regardless of outcome.
    m_alignMulti = false;
    m_alignMultiAwaitClick = false;
    m_alignMultiHaveCorner1 = false;
    setMultiAlignUIState(false);  // also clears the PCB Map click targets

    const auto& pcb = m_alignMultiPcbPts;
    const auto& img = m_alignMultiImgPts;
    const int n = static_cast<int>(pcb.size());
    if (!m_ibomProject || n < 2) {
        m_mainWindow->updateStatusMessage(
            tr("Multi-align cancelled — need at least 2 components (got %1)").arg(n));
        return;
    }

    // Fit PCB→image transform: ≥4 → homography (perspective), 3 → affine,
    // 2 → similarity (uniform scale + rotation + translation).
    cv::Mat H;  // 3x3, CV_64F
    if (n >= 4) {
        H = cv::findHomography(pcb, img, cv::RANSAC, 5.0);
    } else if (n == 3) {
        cv::Mat A = cv::estimateAffine2D(pcb, img);  // 2x3
        if (!A.empty()) {
            H = cv::Mat::eye(3, 3, CV_64F);
            A.copyTo(H(cv::Rect(0, 0, 3, 2)));
        }
    } else {  // n == 2 — similarity
        const double dxp = pcb[1].x - pcb[0].x, dyp = pcb[1].y - pcb[0].y;
        const double dxi = img[1].x - img[0].x, dyi = img[1].y - img[0].y;
        const double dp = std::hypot(dxp, dyp), di = std::hypot(dxi, dyi);
        if (dp >= 0.1 && di >= 1.0) {
            const double s = di / dp;
            const double rot = std::atan2(dyi, dxi) - std::atan2(dyp, dxp);
            const double c = std::cos(rot) * s, sn = std::sin(rot) * s;
            const double tx = img[0].x - (c * pcb[0].x - sn * pcb[0].y);
            const double ty = img[0].y - (sn * pcb[0].x + c * pcb[0].y);
            H = (cv::Mat_<double>(3, 3) << c, -sn, tx, sn, c, ty, 0, 0, 1);
        }
    }
    if (H.empty() || H.rows != 3 || H.cols != 3) {
        m_mainWindow->updateStatusMessage(
            tr("Multi-align failed — could not fit a transform (try components farther apart)"));
        spdlog::error("Multi-align: transform fit failed (n={})", n);
        return;
    }

    // Project the board bbox corners through H to feed the standard path.
    auto& bb = m_ibomProject->boardInfo.boardBBox;
    std::vector<cv::Point2f> pcbCorners = {
        {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
        {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
        {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
        {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}
    };
    std::vector<cv::Point2f> imgCorners;
    cv::perspectiveTransform(pcbCorners, imgCorners, H);

    ++m_alignmentEpoch;
    if (!m_homography->compute(pcbCorners, imgCorners)) {
        m_mainWindow->updateStatusMessage(tr("Multi-align failed — homography compute error"));
        spdlog::error("Multi-align: Homography::compute failed");
        return;
    }
    if (m_liveMode) m_baseHomography = m_homography->matrix().clone();

    // Scale px/mm from the projected top edge (same as the 4-corner path).
    const double topEdgePx = cv::norm(imgCorners[1] - imgCorners[0]);
    const double boardWMm = bb.width();
    if (boardWMm > 0.0 && topEdgePx > 0.0) {
        m_basePixelsPerMm = topEdgePx / boardWMm;
        m_currentPixelsPerMm = m_basePixelsPerMm;
        if (auto* sp = m_mainWindow->statsPanel())
            sp->setScale(m_currentPixelsPerMm);
    } else {
        updateDynamicScale();
    }
    if (m_trackingWorker)
        QMetaObject::invokeMethod(m_trackingWorker, "resetReference", Qt::QueuedConnection);
    autoStartLiveTracking();

    const char* model = (n >= 4) ? "homography" : (n == 3) ? "affine" : "similarity";
    reportAlignmentResult(
        tr("Multi-align set from %1 components (%2) — scale: %3 px/mm")
        .arg(n).arg(model).arg(m_currentPixelsPerMm, 0, 'f', 1));
    spdlog::info("Multi-align OK: {} components, {} model, scale={:.2f} px/mm",
                 n, model, m_currentPixelsPerMm);
}

void Application::autoAlignBoard(bool silent, bool isRetry)
{
    if (m_autoAligning) return;  // already running, ignore re-clicks
    if (!m_lastColorFrame || m_lastColorFrame->empty()) {
        if (!silent)
            m_mainWindow->updateStatusMessage(tr("Auto-Align: no camera frame available yet"));
        return;
    }

    // Fresh interactive attempt: arm the auto-retry budget (a single
    // badly-timed frame — blur, glare, a hand in view — shouldn't fail the
    // whole click; the completion handler retries on fresh frames).
    if (!silent && !isRetry)
        m_autoAlignRetriesLeft = 2;

    // Cancel any other interactive picking mode.
    m_alignOnComponents = false;
    m_alignCompStep = 0;
    m_pickingHomographyPoints = false;
    m_anchorMode = false;

    m_autoAligning = true;
    if (!silent)
        m_mainWindow->updateStatusMessage(tr("Auto-Align: locating board outline..."));

    const cv::Mat colorCopy = m_lastColorFrame->clone();
    const cv::Mat depthCopy = (m_lastDepthFrame && !m_lastDepthFrame->empty())
        ? m_lastDepthFrame->clone() : cv::Mat();
    const std::shared_ptr<const IBomProject> project = m_ibomProject;

    // Prefer a fresh pixels-per-mm estimate from depth pinhole geometry
    // (fx / distance) over the currently cached scale, which may come from a
    // checkerboard calibration done at a different working distance or with
    // a different camera entirely — that mismatch is exactly what makes
    // BoardLocator::validateSize() reject a correctly-found board outline.
    double expectedPixelsPerMm = 0.0;
#ifdef IBOM_HAVE_REALSENSE
    if (auto* rs = dynamic_cast<camera::RealSenseCapture*>(m_camera.get())) {
        const double fx = rs->colorFx();
        if (fx > 0.0 && m_lastDepthDistanceMm > 0.0)
            expectedPixelsPerMm = fx / m_lastDepthDistanceMm;
    }
#endif
    if (expectedPixelsPerMm <= 0.0) {
        expectedPixelsPerMm = m_currentPixelsPerMm > 0.0
            ? m_currentPixelsPerMm : m_basePixelsPerMm;
    }
    const uint64_t dispatchEpoch = ++m_alignmentEpoch;

    auto* watcher = new QFutureWatcher<overlay::BoardLocateResult>(this);
    connect(watcher, &QFutureWatcher<overlay::BoardLocateResult>::finished, this,
            [this, watcher, project, dispatchEpoch, silent]() {
        const overlay::BoardLocateResult result = watcher->result();
        watcher->deleteLater();
        m_autoAligning = false;

        if (dispatchEpoch != m_alignmentEpoch) {
            // Another alignment action (manual or a second Auto-Align click)
            // already landed while this detection was running — applying this
            // now-stale result would clobber the newer one.
            spdlog::info("Auto-Align: discarding stale result (newer alignment already applied)");
            return;
        }

        if (!result.found) {
            if (silent) {
                m_reanchorFailStreak = std::min(m_reanchorFailStreak + 1, 20);
                spdlog::debug("Periodic re-anchor: board not found ({}), streak {}",
                              result.message, m_reanchorFailStreak);
            } else if (m_autoAlignRetriesLeft > 0) {
                // One frame is a lottery ticket (blur/glare/hand); retry on a
                // fresh frame before reporting failure to the user.
                const int attempt = 3 - m_autoAlignRetriesLeft;
                --m_autoAlignRetriesLeft;
                m_mainWindow->updateStatusMessage(
                    tr("Auto-Align: board not found — retrying on a fresh frame (%1/3)…")
                        .arg(attempt + 1));
                spdlog::info("Auto-Align: not found ({}), retry {} of 2 in 300 ms",
                             result.message, attempt);
                QTimer::singleShot(300, this, [this]() {
                    autoAlignBoard(/*silent=*/false, /*isRetry=*/true);
                });
            } else {
                m_mainWindow->updateStatusMessage(
                    tr("Auto-Align failed after 3 attempts: %1")
                        .arg(QString::fromStdString(result.message)));
                spdlog::warn("Auto-Align failed (3 attempts): {}", result.message);
            }
            return;
        }

        // Periodic (silent) re-anchor: only act on a confident detection, and
        // only when it disagrees enough with the current pose to be worth a
        // re-anchor — otherwise leave healthy live tracking undisturbed (a
        // re-anchor resets the tracking reference and would otherwise stutter).
        if (silent) {
            // BoardLocator found the board here → geometric re-anchor works;
            // clear any back-off streak.
            m_reanchorFailStreak = 0;
            if (result.score < m_config->reanchorMinScore()) {
                spdlog::debug("Periodic re-anchor: score {:.2f} < {:.2f}, skipping",
                              result.score, m_config->reanchorMinScore());
                return;
            }
            const auto& sb = project->boardInfo.boardBBox;
            const std::vector<cv::Point2f> curPcb = {
                {static_cast<float>(sb.minX), static_cast<float>(sb.minY)},
                {static_cast<float>(sb.maxX), static_cast<float>(sb.minY)},
                {static_cast<float>(sb.maxX), static_cast<float>(sb.maxY)},
                {static_cast<float>(sb.minX), static_cast<float>(sb.maxY)}
            };
            double maxShift = 1e9;  // no current pose → always (re)anchor
            if (m_homography && m_homography->isValid() && result.imageCorners.size() == 4) {
                maxShift = 0.0;
                for (size_t i = 0; i < 4; ++i) {
                    const auto cur = m_homography->pcbToImage(curPcb[i]);
                    maxShift = std::max(maxShift,
                        cv::norm(cv::Point2f(cur.x - result.imageCorners[i].x,
                                             cur.y - result.imageCorners[i].y)));
                }
            }
            constexpr double kReanchorMinShiftPx = 12.0;
            if (maxShift < kReanchorMinShiftPx) {
                spdlog::debug("Periodic re-anchor: pose within {:.0f}px (shift {:.1f}), skipping",
                              kReanchorMinShiftPx, maxShift);
                return;
            }
            spdlog::info("Periodic re-anchor: correcting drift (shift {:.1f}px, score {:.2f})",
                         maxShift, result.score);
        }

        // Use the project captured at dispatch time, not the live m_ibomProject,
        // in case the user loaded a different iBOM while detection was running.
        const auto& bb = project->boardInfo.boardBBox;
        const std::vector<cv::Point2f> pcbCorners = {
            {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
            {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
            {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
            {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}
        };

        if (!m_homography->compute(pcbCorners, result.imageCorners)) {
            m_mainWindow->updateStatusMessage(tr("Auto-Align: homography computation failed"));
            return;
        }

        m_baseHomography = m_homography->matrix().clone();
        if (m_mainWindow->boardMinimap())
            m_mainWindow->boardMinimap()->update();

        // Compute scale directly from the new homography (mirrors the manual
        // 4-corner handler's fallback) rather than via updateDynamicScale(),
        // so the displayed px/mm always reflects this alignment regardless of
        // Config::scaleMethod().
        const double pcbW = bb.width();
        const auto cornerTL = m_homography->pcbToImage({static_cast<float>(bb.minX), static_cast<float>(bb.minY)});
        const auto cornerTR = m_homography->pcbToImage({static_cast<float>(bb.maxX), static_cast<float>(bb.minY)});
        const double pixW = cv::norm(cv::Point2f(cornerTL.x - cornerTR.x, cornerTL.y - cornerTR.y));
        if (pcbW > 0.0) {
            m_basePixelsPerMm = pixW / pcbW;
            m_currentPixelsPerMm = m_basePixelsPerMm;
            if (auto* sp = m_mainWindow->statsPanel())
                sp->setScale(m_currentPixelsPerMm);
        }

        if (m_trackingWorker)
            QMetaObject::invokeMethod(m_trackingWorker, "resetReference", Qt::QueuedConnection);
        autoStartLiveTracking();

        if (silent) {
            // Periodic re-anchor: applied silently (already score- and
            // drift-gated above). No popups, no status spam.
            spdlog::info("Periodic re-anchor applied via {} (score {:.2f})",
                         result.method, result.score);
            return;
        }

        // The edge-agreement score is only weakly discriminative on a busy PCB
        // against a cluttered/reflective background: a spatially-offset quad can
        // still overlap ~1/3 of the real edges. Below this level the placement
        // is untrustworthy (often visibly shifted), so flag it rather than
        // reporting a clean "success", and point the user at the reliable
        // manual path instead of leaving them to wonder why it looks wrong.
        constexpr double kAutoAlignTrustScore = 0.45;
        if (result.score < kAutoAlignTrustScore) {
            reportAlignmentResult(
                tr("Auto-Align: LOW confidence (score %1 via %2) — the overlay is "
                   "likely misplaced. Use 'Reset Alignment' then 'Align: Multi-Comp' "
                   "for this board.")
                    .arg(result.score, 0, 'f', 2)
                    .arg(QString::fromStdString(result.method)));
            spdlog::warn("Auto-Align low confidence: via {} score {:.2f} (< {:.2f})",
                         result.method, result.score, kAutoAlignTrustScore);
            QMessageBox::warning(m_mainWindow.get(), tr("Auto-Align — Low Confidence"),
                tr("Auto-Align placed the overlay but with low confidence "
                   "(edge-agreement score %1).\n\n"
                   "On a glossy board over a cluttered background the automatic "
                   "outline detection is unreliable. If the overlay doesn't line "
                   "up, click 'Reset Alignment' and use 'Align: Multi-Comp' — mark "
                   "a few components (the PCB Map shows exactly where to click) for "
                   "a precise, repeatable result that's then saved for next time.")
                    .arg(result.score, 0, 'f', 2));
            return;
        }

        reportAlignmentResult(
            tr("Auto-Align: aligned via %1 (score %2)")
                .arg(QString::fromStdString(result.method))
                .arg(result.score, 0, 'f', 2));
        spdlog::info("Auto-Align succeeded via {} (score {:.2f})", result.method, result.score);
    });

    ai::ComponentDetector* detector = componentDetector();
    watcher->setFuture(QtConcurrent::run([colorCopy, depthCopy, project,
                                          expectedPixelsPerMm, detector]() {
        // Detection-first (docs/AUTO_ALIGN_V2_PLAN.md): with a component
        // detector loaded, register the detected-component constellation
        // against the iBOM layout — needs no visible board outline, so it
        // works exactly where the geometric path structurally fails (board
        // filling the frame, cluttered/glossy background). BoardLocator stays
        // as the model-free fallback.
        if (detector) {
            const std::vector<ai::Detection> detections = detector->detect(colorCopy);
            const auto boot = overlay::ComponentReanchor::bootstrap(
                detections, *project, ibom::Layer::Front, expectedPixelsPerMm);
            if (boot.found) {
                overlay::BoardLocateResult blr;
                blr.found  = true;
                blr.method = "components";
                // Map inliers onto the shared [0,1] score consumed by the
                // trust (0.45) and reanchor (0.5) gates: 8 inliers → 0.67,
                // saturates at 18+. Component consensus is far more specific
                // than edge agreement, hence the generous floor.
                blr.score   = std::min(1.0, 0.4 + boot.inliers / 30.0);
                blr.message = boot.message;
                const auto& bb = project->boardInfo.boardBBox;
                const std::vector<cv::Point2f> pcb = {
                    {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
                    {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
                    {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
                    {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}
                };
                cv::perspectiveTransform(pcb, blr.imageCorners, boot.homography);
                return blr;
            }
            spdlog::info("Auto-Align: component bootstrap didn't lock ({}), "
                         "falling back to board outline", boot.message);
        }
        return overlay::BoardLocator::locate(colorCopy, depthCopy, *project,
                                             expectedPixelsPerMm, ibom::Layer::Front);
    }));
}

void Application::autoStartLiveTracking()
{
    if (m_liveMode) return;  // already tracking — alignment paths rebase it themselves
    if (!m_homography || !m_homography->isValid()) return;
    if (auto* cp = m_mainWindow ? m_mainWindow->controlPanel() : nullptr) {
        spdlog::info("Alignment applied — enabling live tracking automatically");
        cp->setLiveMode(true);
    }
}

void Application::attemptLostRecovery()
{
    using S = overlay::TrackingWorker::State;
    if (!m_liveMode || !m_ibomProject ||
        m_lastTrackingState != static_cast<int>(S::Lost)) {
        m_lostRecoveryArmed = false;  // recovered (or live mode ended) — chain stops
        return;
    }
    if (!m_autoAligning) {
        const bool viaDetector = componentDetector() != nullptr;
        spdlog::info("Tracking LOST — automatic re-anchor attempt via {}",
                     viaDetector ? "component detector" : "board outline");
        if (viaDetector) componentReanchor(/*silent=*/true);
        else             autoAlignBoard(/*silent=*/true);
    }
    // State-change signals only fire on transitions; a persistent loss needs
    // polling until tracking recovers or live mode ends.
    QTimer::singleShot(3000, this, &Application::attemptLostRecovery);
}

void Application::componentReanchor(bool silent)
{
    if (m_autoAligning) return;  // shares the alignment guard with autoAlignBoard
    auto* detector = componentDetector();
    if (!detector) {
        if (!silent)
            m_mainWindow->updateStatusMessage(
                tr("Component re-anchor: no detector loaded (need a model in models/)"));
        return;
    }
    if (!m_lastColorFrame || m_lastColorFrame->empty()) {
        if (!silent)
            m_mainWindow->updateStatusMessage(tr("Component re-anchor: no camera frame yet"));
        return;
    }
    // NOTE: no valid-pose requirement anymore. With a pose, the prior-based
    // estimate() corrects it; without one (or with a stale one after the board
    // was moved/picked up), bootstrap() registers the detection constellation
    // against the layout globally. The detector alone is enough to align.
    if (!m_ibomProject) {
        if (!silent)
            m_mainWindow->updateStatusMessage(tr("Component re-anchor: load an iBOM first"));
        return;
    }

    m_autoAligning = true;
    if (!silent)
        m_mainWindow->updateStatusMessage(tr("Component re-anchor: detecting components..."));

    const cv::Mat colorCopy = m_lastColorFrame->clone();
    const std::shared_ptr<const IBomProject> project = m_ibomProject;
    // Snapshot the current pose so the worker thread reads a stable matrix even
    // if live tracking updates m_homography meanwhile. May be invalid (empty
    // matrix) — the worker then goes straight to bootstrap().
    overlay::Homography priorPose;
    if (m_homography && m_homography->isValid())
        priorPose.setMatrix(m_homography->matrix().clone());
    // Physical scale prior for bootstrap: D405 pinhole (fx / distance) when
    // available, else the current px/mm estimate; 0 = unknown (still works,
    // just a wider hypothesis space). Mirrors autoAlignBoard().
    double scalePrior = 0.0;
#ifdef IBOM_HAVE_REALSENSE
    if (auto* rs = dynamic_cast<camera::RealSenseCapture*>(m_camera.get())) {
        const double fx = rs->colorFx();
        if (fx > 0.0 && m_lastDepthDistanceMm > 0.0)
            scalePrior = fx / m_lastDepthDistanceMm;
    }
#endif
    if (scalePrior <= 0.0)
        scalePrior = m_currentPixelsPerMm > 0.0 ? m_currentPixelsPerMm : m_basePixelsPerMm;
    const uint64_t dispatchEpoch = ++m_alignmentEpoch;

    auto* watcher = new QFutureWatcher<overlay::ComponentReanchorResult>(this);
    connect(watcher, &QFutureWatcher<overlay::ComponentReanchorResult>::finished, this,
            [this, watcher, project, dispatchEpoch, silent]() {
        const overlay::ComponentReanchorResult result = watcher->result();
        watcher->deleteLater();
        m_autoAligning = false;

        if (dispatchEpoch != m_alignmentEpoch) {
            spdlog::info("Component re-anchor: discarding stale result");
            return;
        }
        if (!result.found) {
            if (silent) {
                m_reanchorFailStreak = std::min(m_reanchorFailStreak + 1, 20);
                spdlog::debug("Component re-anchor: {} (streak {})",
                              result.message, m_reanchorFailStreak);
            } else {
                m_mainWindow->updateStatusMessage(
                    tr("Component re-anchor failed: %1")
                        .arg(QString::fromStdString(result.message)));
                spdlog::warn("Component re-anchor failed: {}", result.message);
            }
            return;
        }
        m_reanchorFailStreak = 0;

        // Drift gate (silent only): skip if the new pose barely moves the board
        // corners, to leave healthy tracking undisturbed (a re-anchor resets the
        // tracking reference and would otherwise stutter).
        const auto& bb = project->boardInfo.boardBBox;
        const std::vector<cv::Point2f> pcbCorners = {
            {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
            {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
            {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
            {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}
        };
        std::vector<cv::Point2f> newImg;
        cv::perspectiveTransform(pcbCorners, newImg, result.homography);
        if (silent && m_homography && m_homography->isValid()) {
            double maxShift = 0.0;
            for (size_t i = 0; i < 4; ++i) {
                const auto cur = m_homography->pcbToImage(pcbCorners[i]);
                maxShift = std::max(maxShift,
                    cv::norm(cv::Point2f(cur.x - newImg[i].x, cur.y - newImg[i].y)));
            }
            constexpr double kReanchorMinShiftPx = 12.0;
            if (maxShift < kReanchorMinShiftPx) {
                spdlog::debug("Component re-anchor: pose within {:.0f}px (shift {:.1f}), skipping",
                              kReanchorMinShiftPx, maxShift);
                return;
            }
            spdlog::info("Component re-anchor: correcting drift (shift {:.1f}px, {})",
                         maxShift, result.message);
        }

        m_homography->setMatrix(result.homography);
        m_baseHomography = m_homography->matrix().clone();
        if (m_mainWindow->boardMinimap())
            m_mainWindow->boardMinimap()->update();

        // Refresh px/mm from the new pose (mirrors autoAlignBoard()).
        const double pcbW = bb.width();
        const double pixW = cv::norm(newImg[0] - newImg[1]);
        if (pcbW > 0.0) {
            m_basePixelsPerMm = pixW / pcbW;
            m_currentPixelsPerMm = m_basePixelsPerMm;
            if (auto* sp = m_mainWindow->statsPanel())
                sp->setScale(m_currentPixelsPerMm);
        }

        if (m_trackingWorker)
            QMetaObject::invokeMethod(m_trackingWorker, "resetReference", Qt::QueuedConnection);
        autoStartLiveTracking();

        if (silent) {
            spdlog::info("Component re-anchor applied ({})", result.message);
            return;
        }
        reportAlignmentResult(
            tr("Component re-anchor: %1").arg(QString::fromStdString(result.message)));
        spdlog::info("Component re-anchor succeeded ({})", result.message);
    });

    watcher->setFuture(QtConcurrent::run([detector, colorCopy, project, priorPose,
                                          scalePrior]() {
        const std::vector<ai::Detection> detections = detector->detect(colorCopy);
        // Prior-based correction first (cheap, precise when the pose is only
        // drifting); global bootstrap when there is no usable prior — no pose
        // yet, or a pose so stale (board moved/picked up) that nothing falls
        // inside the matching radius anymore.
        auto res = overlay::ComponentReanchor::estimate(
            detections, *project, priorPose, ibom::Layer::Front);
        if (!res.found)
            res = overlay::ComponentReanchor::bootstrap(
                detections, *project, ibom::Layer::Front, scalePrior);
        return res;
    }));
}

void Application::updateReanchorTimer()
{
    if (!m_reanchorTimer) return;
    const int ms = std::max(500, static_cast<int>(m_config->reanchorIntervalS() * 1000.0));
    m_reanchorTimer->setInterval(ms);
    if (m_config->reanchorEnabled()) {
        if (!m_reanchorTimer->isActive()) m_reanchorTimer->start();
    } else {
        m_reanchorTimer->stop();
    }
}

void Application::logFullState()
{
    const Config& c = *m_config;
    spdlog::info("================= FULL STATE DUMP =================");
    spdlog::info("[camera]   backend={} activeBackend={} index={} {}x{}@{}fps hwDecode={} capturing={} depthDistMm={:.1f}",
                 static_cast<int>(c.cameraBackend()), static_cast<int>(m_activeBackend),
                 c.cameraIndex(), c.cameraWidth(), c.cameraHeight(), c.cameraFps(),
                 c.cameraHwDecode(), (m_camera && m_camera->isCapturing()), m_lastDepthDistanceMm);
    spdlog::info("[scale]    method={} opticalMult={:.3f} basePxPerMm={:.3f} curPxPerMm={:.3f}",
                 static_cast<int>(c.scaleMethod()), c.opticalMultiplier(),
                 m_basePixelsPerMm, m_currentPixelsPerMm);
    spdlog::info("[calib]    isCalibrated={} rms={:.3f}",
                 (m_calibration && m_calibration->isCalibrated()),
                 (m_calibration ? m_calibration->rmsError() : -1.0));
    spdlog::info("[track]    live={} interval={}ms orbKp={} minMatch={} lowe={:.2f} ransac={:.1f} downscale={:.2f} model={}",
                 m_liveMode, c.trackingIntervalMs(), c.orbKeypoints(), c.minMatchCount(),
                 c.matchDistanceRatio(), c.ransacThreshold(), c.trackingDownscale(), c.trackingModel());
    spdlog::info("[track]    oneEuroMinCutoff={:.2f} oneEuroBeta={:.3f} clahe={} opticalFlow={} gpuMode={} incremental={} reanchorDriftPx={:.1f} hybrid={}",
                 c.oneEuroMinCutoff(), c.oneEuroBeta(), c.trackingClahe(), c.trackingOpticalFlow(),
                 c.trackingGpuMode(), c.microscopeIncremental(), c.microscopeReanchorDriftPx(),
                 c.hybridDriftCorrection());
    spdlog::info("[reanchor] enabled={} interval={:.1f}s minScore={:.2f} failStreak={}",
                 c.reanchorEnabled(), c.reanchorIntervalS(), c.reanchorMinScore(), m_reanchorFailStreak);
    spdlog::info("[overlay]  pads={} silk={} fab={} opacity={:.2f} placedOpacity={:.2f} selOutlineW={:.1f} colors[sel={} placed={} normal={}]",
                 c.showPads(), c.showSilkscreen(), c.showFabrication(), c.overlayOpacity(),
                 c.placedOpacity(), c.selectedOutlineWidth(), c.selectedColorHex(),
                 c.placedColorHex(), c.normalColorHex());
    spdlog::info("[ai]       enabled={} model='{}' confidence={:.2f} ready={}",
                 c.aiEnabled(), c.detectorModel(), c.detectionConfidence(), m_aiReady.load());
    spdlog::info("[state]    ibomLoaded={} components={} homographyValid={} selectedRef='{}' placed={} depthView={} cloudView={}",
                 static_cast<bool>(m_ibomProject),
                 (m_ibomProject ? m_ibomProject->components.size() : 0u),
                 (m_homography && m_homography->isValid()), m_selectedRef, m_placedRefs.size(),
                 m_depthViewMode, m_pointCloudMode);
    spdlog::info("[log]      verbose={} file={}", c.verboseLogging(), utils::Logger::logFilePath());
    spdlog::info("==================================================");
}

void Application::refreshCameraDeviceList()
{
    QStringList cameraNames;
    QList<int>  cameraIndices;
#ifdef IBOM_HAVE_REALSENSE
    if (m_config->cameraBackend() == CameraBackend::RealSense) {
        const auto rsDevices = camera::RealSenseCapture::listDevices();
        for (size_t i = 0; i < rsDevices.size(); ++i) {
            cameraNames   << QString("%1: %2").arg(i).arg(QString::fromStdString(rsDevices[i]));
            cameraIndices << static_cast<int>(i);  // RealSense indices are positional
        }
    } else
#endif
    {
        // V4L2: listDevices() returns (real /dev/video index, card name) pairs.
        // Use the real index for both the label and the combo item data so the
        // selected device maps to the correct node (gaps are common: the D405
        // occupies low indices, the USB microscope a higher one).
        const auto v4lDevices = camera::CameraCapture::listDevices();
        for (const auto& [idx, name] : v4lDevices) {
            cameraNames   << QString("%1: %2").arg(idx).arg(QString::fromStdString(name));
            cameraIndices << idx;
        }
    }
    if (cameraNames.isEmpty()) {
        // Enumeration can come back empty transiently — e.g. querying a
        // RealSense while it is busy streaming throws "failed to set power
        // state". When a camera is in fact live we must NOT show "No camera
        // detected", but we also must not keep a list from the *other* backend
        // (that leaves the combo showing the microscope while the D405 streams).
        if (m_camera && m_camera->isCapturing()) {
#ifdef IBOM_HAVE_REALSENSE
            if (m_config->cameraBackend() == CameraBackend::RealSense) {
                // Synthesize a single entry that at least reflects the active
                // backend, instead of keeping the stale V4L2 microscope list.
                cameraNames   << tr("0: Intel RealSense (active)");
                cameraIndices << 0;
            } else
#endif
            {
                // V4L2 enumeration works even while streaming (QUERYCAP is a
                // read-only open), so an empty result here is unexpected —
                // keep whatever is there rather than risk a wrong label.
                spdlog::debug("Device enumeration empty while capturing — keeping current list");
                return;
            }
        } else {
            cameraNames   << tr("No camera detected");
            cameraIndices << 0;
        }
    }
    if (m_mainWindow && m_mainWindow->controlPanel())
        m_mainWindow->controlPanel()->setCameraDevices(
            cameraNames, cameraIndices, m_config->cameraIndex());
}

void Application::openRealSenseControls()
{
#ifdef IBOM_HAVE_REALSENSE
    auto* rs = dynamic_cast<camera::RealSenseCapture*>(m_camera.get());
    if (!rs) {
        QMessageBox::information(m_mainWindow.get(), tr("RealSense Controls"),
            tr("These controls read the live sensor options of a RealSense camera.\n\n"
               "To use them:\n"
               "  1. Connect the RealSense (D405) over USB3 and relaunch.\n"
               "  2. Settings → Camera → Backend = « Intel RealSense (D405) ».\n"
               "  3. Start the camera.\n\n"
               "The active backend is currently the USB microscope (V4L2)."));
        return;
    }
    if (!rs->isCapturing()) {
        QMessageBox::information(m_mainWindow.get(), tr("RealSense Controls"),
            tr("Start the camera first — sensor options are read from the live device.\n\n"
               "If no RealSense is detected, check it is connected over USB3 and that "
               "the container was launched with the camera plugged in "
               "(run_local_gui.sh maps /dev/bus/usb when a RealSense is present)."));
        return;
    }
    auto* dlg = new gui::RealSenseControlsDialog(rs, m_mainWindow.get());
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
#else
    QMessageBox::information(m_mainWindow.get(), tr("RealSense Controls"),
        tr("This build has no RealSense support."));
#endif
}

void Application::updateCalibrationUI()
{
    if (!m_mainWindow) return;

    // Keep the live calibration monitor (Dev tool) in sync with backend
    // switches and calibration runs.
    pushCalibrationMonitorState();

    const bool isRS = (m_activeBackend == CameraBackend::RealSense);

    // Update calibration button in the control panel.
    if (auto* cp = m_mainWindow->controlPanel())
        cp->setCameraBackendUI(isRS);

    // Depth view + 3D point cloud are only available with RealSense (depth).
    m_mainWindow->setDepthViewAvailable(isRS);
    m_mainWindow->setPointCloudAvailable(isRS);
    if (!isRS) { m_depthViewMode = false; m_pointCloudMode = false; }

    // Update calibration info in the stats panel.
    if (auto* sp = m_mainWindow->statsPanel()) {
#ifdef IBOM_HAVE_REALSENSE
        if (isRS) {
            auto* rs = dynamic_cast<camera::RealSenseCapture*>(m_camera.get());
            const double fx = rs ? rs->colorFx() : 0.0;
            QString tip;
            if (rs && fx > 0.0) {
                const double fy  = rs->colorFy();
                const double ppx = rs->colorPpx();
                const double ppy = rs->colorPpy();
                const auto   res = m_camera->resolution();
                const int    w   = res.width();
                const int    h   = res.height();
                // Field of view from the pinhole model: FOV = 2·atan(size / 2f).
                const double rad2deg = 180.0 / 3.14159265358979323846;
                const double hfov = (w > 0 && fx > 0) ? 2.0 * std::atan(w / (2.0 * fx)) * rad2deg : 0.0;
                const double vfov = (h > 0 && fy > 0) ? 2.0 * std::atan(h / (2.0 * fy)) * rad2deg : 0.0;
                tip = tr(
                    "Intel RealSense — factory intrinsics (no calibration needed).\n"
                    "Resolution: %1×%2\n"
                    "Focal length: fx=%3 px, fy=%4 px\n"
                    "Principal point: (%5, %6) px\n"
                    "Field of view: %7° H × %8° V\n\n"
                    "fx scales with width; cross-check on the Jetson with "
                    "`rs-enumerate-devices -c`.")
                    .arg(w).arg(h)
                    .arg(fx, 0, 'f', 1).arg(fy, 0, 'f', 1)
                    .arg(ppx, 0, 'f', 1).arg(ppy, 0, 'f', 1)
                    .arg(hfov, 0, 'f', 1).arg(vfov, 0, 'f', 1);
            }
            sp->setCalibration(fx, 0.0, true, tip);
            return;
        }
#endif
        // V4L2 / USB microscope
        if (m_calibration && m_calibration->isCalibrated()) {
            sp->setCalibration(m_calibration->rmsError(),
                               m_calibration->pixelsPerMm(), false);
        } else {
            sp->setCalibration(0.0, 0.0, false);
        }
        // No depth sensor on a plain UVC camera — clear the depth-derived
        // readouts so they don't show stale values left over from a prior
        // RealSense session (e.g. "Distance: 190 mm", "Depth fill: 77%").
        sp->setDistance(-1.0);
        sp->setFillRate(-1.0);
    }
}

void Application::connectSignals()
{
    connect(this, &Application::shutdownRequested, &m_qapp, &QApplication::quit);

    // ── Runtime logs → in-app Event Log (StatsPanel) ────────────
    // spdlog records (info+) routed through the Qt sink. Queued because the
    // sink may fire from the camera/tracking worker threads.
    if (auto* sp = m_mainWindow->statsPanel()) {
        connect(&utils::LogBridge::instance(), &utils::LogBridge::messageLogged,
                sp, &gui::StatsPanel::addLogEntry, Qt::QueuedConnection);
    }

    // ── Tracking worker → homography update on GUI thread ───────
    if (m_trackingWorker) {
        connect(m_trackingWorker, &overlay::TrackingWorker::homographyUpdated,
                this, [this](cv::Mat combined, int inliers, double reprojErr) {
            if (!m_liveMode || combined.empty() || !m_homography) return;
            m_homography->setMatrix(combined);
            // Scale readout at ~5 Hz is plenty — with optical flow this signal
            // fires at camera rate, and the IBomPads method walks board data
            // (ERREUR #52). Event-driven call sites (alignment, re-anchor)
            // still call updateDynamicScale() directly, unthrottled.
            const qint64 scaleNowMs = QDateTime::currentMSecsSinceEpoch();
            if (scaleNowMs - m_lastScaleUpdateMs >= 200) {
                m_lastScaleUpdateMs = scaleNowMs;
                updateDynamicScale();
            }
            m_mainWindow->boardMinimap()->update();
            // Tracking-quality stream (visible only with verbose logging on).
            spdlog::debug("[track] homography applied: inliers={} reprojErr={:.2f}px scale={:.3f}px/mm",
                          inliers, reprojErr, m_currentPixelsPerMm);
        }, Qt::QueuedConnection);

        connect(m_trackingWorker, &overlay::TrackingWorker::trackingError,
                this, [](const QString& msg) {
            spdlog::warn("Tracking worker error: {}", msg.toStdString());
        }, Qt::QueuedConnection);

        // ── Incremental tracking state → re-anchor badge (§4) ───────
        connect(m_trackingWorker, &overlay::TrackingWorker::trackingStateChanged,
                this, [this](int state) {
            m_lastTrackingState = state;  // read by the loss-recovery poll
            if (!m_liveMode) return;
            using S = overlay::TrackingWorker::State;
            switch (static_cast<S>(state)) {
                case S::Locked:
                    m_mainWindow->updateStatusMessage(tr("Tracking: locked"));
                    break;
                case S::Drifting:
                    m_mainWindow->updateStatusMessage(
                        tr("Tracking: drifting — re-anchor (A) to reset"));
                    break;
                case S::Lost:
                    m_mainWindow->updateStatusMessage(
                        tr("Tracking: LOST — re-anchoring automatically…"));
                    // Give the worker a moment to re-acquire on its own (mask
                    // fallback + jump-recovery usually do), then re-locate the
                    // board outright. One chain only, however often Lost fires.
                    if (!m_lostRecoveryArmed && m_ibomProject) {
                        m_lostRecoveryArmed = true;
                        QTimer::singleShot(800, this, &Application::attemptLostRecovery);
                    }
                    break;
            }
        }, Qt::QueuedConnection);

        // Tracking quality feed for dataset capture (worker → worker, queued).
        if (m_datasetCreator) {
            connect(m_trackingWorker, &overlay::TrackingWorker::homographyUpdated,
                    m_datasetCreator, &features::DatasetCreator::onHomography,
                    Qt::QueuedConnection);
        }
    }

    // ── Dataset capture panel ⇄ worker ──────────────────────────
    if (m_datasetCreator && m_mainWindow->datasetPanel()) {
        auto* panel = m_mainWindow->datasetPanel();
        connect(panel, &gui::DatasetPanel::startRequested,
                m_datasetCreator, &features::DatasetCreator::startSession,
                Qt::QueuedConnection);
        connect(panel, &gui::DatasetPanel::stopRequested,
                m_datasetCreator, &features::DatasetCreator::stopSession,
                Qt::QueuedConnection);
        connect(m_datasetCreator, &features::DatasetCreator::sessionStarted,
                panel, &gui::DatasetPanel::onSessionStarted, Qt::QueuedConnection);
        connect(m_datasetCreator, &features::DatasetCreator::sessionStopped,
                panel, &gui::DatasetPanel::onSessionStopped, Qt::QueuedConnection);
        connect(m_datasetCreator, &features::DatasetCreator::sessionError,
                panel, &gui::DatasetPanel::onSessionError, Qt::QueuedConnection);
        connect(m_datasetCreator, &features::DatasetCreator::statusUpdated,
                panel, &gui::DatasetPanel::updateStatus, Qt::QueuedConnection);
    }

    // ── Camera toggle ───────────────────────────────────────────
    connect(m_mainWindow.get(), &gui::MainWindow::cameraToggled, this, [this](bool start) {
        if (start) {
            m_camera->setDeviceIndex(m_config->cameraIndex());
            m_camera->setResolution(m_config->cameraWidth(), m_config->cameraHeight());
            m_camera->setFps(m_config->cameraFps());
            m_camera->setHardwareDecode(m_config->cameraHwDecode());
            if (!m_camera->start()) {
                m_mainWindow->updateStatusMessage(tr("Failed to start camera"));
            }
        } else {
            m_camera->stop();
        }
    });

    // ── Camera frame/error wiring ───────────────────────────────
    // Extracted into wireCameraSignals() so a backend hot-swap (microscope ↔
    // RealSense) can re-run it against the freshly created m_camera.
    wireCameraSignals();

    // Remaining UI/control connections (kept in a separate function only so the
    // very large frameReady lambda above can live in its own re-callable unit).
    connectControlSignals();
}

void Application::wireCameraSignals()
{
    // ── Camera errors ───────────────────────────────────────────
    connect(m_camera.get(), &camera::ICameraSource::captureError, this, [this](const QString& msg) {
        spdlog::error("Camera error: {}", msg.toStdString());
        m_mainWindow->updateStatusMessage(msg);
    }, Qt::UniqueConnection);

    // ── Camera frame → CameraView + Overlay ─────────────────────
    // Slot receives a shared_ptr<const cv::Mat> — no pixel clone across threads.
    // Pin the backend per-connection: m_activeBackend is already set for the new
    // camera here, and capturing it by value prevents a hot-swap race where
    // queued frames from the old camera would be (un)distorted using the new
    // backend's rule (e.g. double-correcting the last RealSense frames after a
    // switch to V4L2). The connection itself dies with the old camera object.
    const CameraBackend backend = m_activeBackend;
    connect(m_camera.get(), &camera::ICameraSource::frameReady, this,
            [this, backend, intrinsicsShown = false, minimapSized = false](ibom::camera::FrameRef frameRef, qint64 captureNs) mutable {
        if (!frameRef || frameRef->empty()) return;
        const cv::Mat& frame = *frameRef;

        // Cached for on-demand use by Auto-Align (autoAlignBoard()). Zero-copy:
        // frameRef is an immutable shared view, so holding it is cheap.
        m_lastColorFrame = frameRef;

        // The minimap's FOV rectangle is sized from m_config's nominal
        // resolution at startup, but the actual camera (e.g. RealSense
        // defaulting to 848×480 instead of the generic 1920×1080 in config)
        // may not match it. Push the real frame size in once per connection.
        if (!minimapSized && m_mainWindow->boardMinimap()) {
            m_mainWindow->boardMinimap()->setHomography(
                m_homography.get(), QSize(frame.cols, frame.rows));
            minimapSized = true;
        }

        // Dev calibration monitor — feed live frames only while it is visible
        // (it throttles detection internally). Cheap no-op otherwise.
        if (m_calibMonitor && m_calibMonitor->isVisible())
            m_calibMonitor->onFrame(frameRef);

#ifdef IBOM_HAVE_REALSENSE
        // Factory intrinsics are cached by the capture thread when it grabs its
        // first frame — after updateCalibrationUI() already ran synchronously at
        // backend switch (when colorFx() was still 0). Refresh the calibration
        // readout once, now that fx is available. The init-capture flag is
        // per-connection, so it resets automatically on every backend hot-swap.
        if (backend == CameraBackend::RealSense && !intrinsicsShown) {
            if (auto* rs = dynamic_cast<camera::RealSenseCapture*>(m_camera.get());
                rs && rs->colorFx() > 0.0) {
                updateCalibrationUI();
                intrinsicsShown = true;
            }
        }
#endif

        // ── Live tracking: hand the raw frame off to the worker thread ──
        // The worker throttles, downscales and runs ORB without blocking us.
        // tryReserveFrameSlot() is the backpressure valve (F6): at most 2
        // frames in flight — when the worker falls behind, frames are skipped
        // here instead of piling up latency in its event queue.
        if (m_liveMode && m_trackingWorker && m_homography && m_homography->isValid()
            && m_trackingWorker->tryReserveFrameSlot()) {
            QMetaObject::invokeMethod(m_trackingWorker, "processFrame", Qt::QueuedConnection,
                Q_ARG(ibom::camera::FrameRef, frameRef), Q_ARG(qint64, captureNs));
        }

        // ── Dataset capture: same raw frame, own thread, throttles itself ──
        // Raw (non-undistorted) frame on purpose: the tracking homography is
        // estimated on raw frames, so labels and pixels stay consistent.
        if (m_datasetCreator) {
            QMetaObject::invokeMethod(m_datasetCreator, "processFrame", Qt::QueuedConnection,
                Q_ARG(ibom::camera::FrameRef, frameRef));
        }

        // ── Focus assist: Laplacian variance, throttled (~1 ms at 0.25×) ──
        // Same downscale as the dataset capture gate so the displayed value
        // is directly comparable to the dataset.min_sharpness threshold.
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - m_lastSharpnessMs >= 300) {
            m_lastSharpnessMs = nowMs;
            cv::Mat small;
            cv::resize(frame, small, cv::Size(), 0.25, 0.25, cv::INTER_AREA);
            const double sharpness = utils::ImageUtils::computeSharpness(small);
            m_mainWindow->statsPanel()->setSharpness(
                sharpness, sharpness >= m_config->datasetMinSharpness());
        }

        // Apply undistortion if calibrated (allocates a new Mat; unavoidable).
        // Skip for RealSense: librealsense already applies factory calibration.
        cv::Mat processed;
        if (m_calibration && m_calibration->isCalibrated()
            && backend != CameraBackend::RealSense) {
            processed = m_calibration->undistort(frame);
        } else {
            processed = frame;  // header share, no pixel copy
        }

        // Convert to RGB for QImage
        cv::Mat rgb;
        if (processed.channels() == 3)
            cv::cvtColor(processed, rgb, cv::COLOR_BGR2RGB);
        else if (processed.channels() == 1)
            cv::cvtColor(processed, rgb, cv::COLOR_GRAY2RGB);
        else
            rgb = processed;

        QImage qimg(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step),
                    QImage::Format_RGB888);
        // Deep copy: qimg only wraps rgb's pixel buffer, which dies with this
        // scope. The copy is shared (COW) between the camera view and the
        // remote stream.
        const QImage display = qimg.copy();
        // In depth-view mode the colorized depth map drives the view instead.
        // In IR mode the infraredReady handler drives it. Keep overlay/remote
        // paths fed with the color image regardless.
        bool irActive = false;
#ifdef IBOM_HAVE_REALSENSE
        if (auto* rs = dynamic_cast<camera::RealSenseCapture*>(m_camera.get()))
            irActive = rs->emitInfrared();
#endif
        if (!m_depthViewMode && !irActive)
            m_mainWindow->cameraView()->updateFrame(display);

        // Mirror to remote browser clients (RemoteView throttles internally).
        if (m_remoteView && m_remoteView->isRunning() && m_remoteView->clientCount() > 0)
            m_remoteView->pushFrame(display);

        // ── iBOM overlay: board-space buffer + per-frame warp transform ─────
        // The overlay is rendered ONCE in board (PCB) space — it only changes
        // on selection/placed/toggle/color/project changes — and CameraView
        // re-projects that buffer through the current homography (projective
        // QTransform) on every paint. Pose updates therefore cost 9 doubles
        // per frame instead of a full vector re-render: no more 25 fps cap,
        // shape antialiasing is back on, and the overlay is always locked to
        // the freshest pose at paint time (LIVE_TRACKING_ANALYSE F11).
        if (m_ibomProject && m_homography && m_homography->isValid()) {
            const bool drawPads = m_config->showPads();
            const bool drawSilk = m_config->showSilkscreen();

            std::size_t placedHash = m_placedRefs.size();
            for (const auto& r : m_placedRefs)
                placedHash ^= std::hash<std::string>{}(r) + 0x9e3779b97f4a7c15ULL
                              + (placedHash << 6) + (placedHash >> 2);

            const std::string colorKey =
                m_config->selectedColorHex() + '|' + m_config->placedColorHex() + '|'
                + m_config->normalColorHex();
            const float placedAlphaMul = std::clamp(m_config->placedOpacity(), 0.05f, 1.0f);
            const float selectedSilkW  = std::max(1.0f, m_config->selectedOutlineWidth());

            const bool needsRender =
                !m_overlayValid
                || drawPads != m_ovSigPads || drawSilk != m_ovSigSilk
                || m_selectedRef != m_ovSigSelected
                || placedHash != m_ovSigPlacedHash
                || colorKey != m_ovSigColorKey
                || placedAlphaMul != m_ovSigPlacedOpacity
                || selectedSilkW != m_ovSigSelectedSilkW
                || m_ibomProject.get() != m_ovSigProject;

            if (needsRender) {
                overlay::OverlayInputs in;
                in.project     = m_ibomProject;
                in.selectedRef = m_selectedRef;
                in.placedRefs  = m_placedRefs;
                QColor cSel = QColor(QString::fromStdString(m_config->selectedColorHex()));
                QColor cPl  = QColor(QString::fromStdString(m_config->placedColorHex()));
                QColor cNo  = QColor(QString::fromStdString(m_config->normalColorHex()));
                in.cSelected   = cSel.isValid() ? cSel : QColor(0, 229, 255);
                in.cPlaced     = cPl.isValid()  ? cPl  : QColor(72, 200, 72);
                in.cNormal     = cNo.isValid()  ? cNo  : QColor(170, 170, 68);
                in.labelNormal = ibom::gui::theme::labelNormalColor();
                in.placedAlphaMul = placedAlphaMul;
                in.selectedSilkW  = selectedSilkW;
                in.drawPads = drawPads;
                in.drawSilk = drawSilk;

                const auto t0 = std::chrono::steady_clock::now();
                overlay::BoardOverlay bo = overlay::OverlayRenderer::renderBoardSpace(in);
                const double renderMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                m_boardBufferToPcb = bo.pcbToBuffer.inverted();
                m_mainWindow->cameraView()->setBoardOverlayImage(bo.image);
                spdlog::debug("[overlay] board buffer re-rendered {}x{} in {:.1f}ms (pads={} silk={} sel='{}' placed={} comps={})",
                              bo.image.width(), bo.image.height(), renderMs, drawPads, drawSilk,
                              m_selectedRef, m_placedRefs.size(), m_ibomProject->components.size());

                // Remember the inputs this buffer was rendered from.
                m_overlayValid       = true;
                m_ovSigPads          = drawPads;
                m_ovSigSilk          = drawSilk;
                m_ovSigSelected      = m_selectedRef;
                m_ovSigPlacedHash    = placedHash;
                m_ovSigColorKey      = colorKey;
                m_ovSigPlacedOpacity = placedAlphaMul;
                m_ovSigSelectedSilkW = selectedSilkW;
                m_ovSigProject       = m_ibomProject.get();
            }

            // Per-frame: compose buffer→PCB with the freshest PCB→image pose.
            m_mainWindow->cameraView()->setBoardOverlayTransform(
                m_boardBufferToPcb
                * overlay::OverlayRenderer::toQTransform(m_homography->matrix()));
        } else if (m_overlayValid) {
            // No valid homography (e.g. after Reset Alignment): clear the board
            // overlay once, or the last buffer would stay frozen on screen and
            // Reset would appear to "do nothing" (ERREUR #46).
            m_mainWindow->cameraView()->setBoardOverlayImage(QImage());
            m_overlayValid = false;  // force a fresh render once valid again
        }

        // Alignment-picking visual feedback — sole owner of the full-frame
        // overlay channel now that the iBOM overlay lives in the board-space
        // channel (it is no longer suppressed while a valid overlay exists:
        // during a re-alignment both are visible, which is what you want).
        if (m_pickingHomographyPoints && !m_homographyImagePoints.empty()) {
            QImage pickOverlay(rgb.cols, rgb.rows, QImage::Format_ARGB32_Premultiplied);
            pickOverlay.fill(Qt::transparent);
            QPainter pickPainter(&pickOverlay);
            pickPainter.setRenderHint(QPainter::Antialiasing, true);
            pickPainter.setPen(QPen(ibom::gui::theme::pickPointColor(), 2));
            pickPainter.setBrush(ibom::gui::theme::pickPointFill());
            for (const auto& pt : m_homographyImagePoints) {
                pickPainter.drawEllipse(QPointF(pt.x, pt.y), 8, 8);
            }
            // Draw lines between consecutive points
            if (m_homographyImagePoints.size() >= 2) {
                QPen linePen(ibom::gui::theme::pickLineColor(), 1, Qt::DashLine);
                pickPainter.setPen(linePen);
                for (size_t i = 1; i < m_homographyImagePoints.size(); ++i) {
                    pickPainter.drawLine(
                        QPointF(m_homographyImagePoints[i-1].x, m_homographyImagePoints[i-1].y),
                        QPointF(m_homographyImagePoints[i].x, m_homographyImagePoints[i].y));
                }
            }
            pickPainter.end();
            m_mainWindow->cameraView()->setOverlayImage(pickOverlay);
            m_pickOverlayShown = true;
        } else if (m_pickOverlayShown) {
            // Picking ended (or no points yet): release the channel once so
            // stale feedback doesn't linger on screen.
            m_mainWindow->cameraView()->setOverlayImage(QImage());
            m_pickOverlayShown = false;
        }

        // Calibration image collection — capture one image per K press
        // (handled outside frameReady, in calibHandler)

        m_frameCount++;
    }, Qt::QueuedConnection);

#ifdef IBOM_HAVE_REALSENSE
    // ── Depth (RealSense only) → distance readout + auto px/mm scale ──
    if (auto* rs = dynamic_cast<camera::RealSenseCapture*>(m_camera.get())) {
        connect(rs, &camera::RealSenseCapture::depthFrameReady, this,
                [this, rs](ibom::camera::DepthFrameRef depth) {
            if (!depth || depth->empty() || depth->type() != CV_16UC1) return;
            // Note: the depth-view image is produced by rs2::colorizer on the
            // capture thread (colorizedDepthReady), not here — better histogram
            // equalization than a fixed-range colormap.

            // Cached for on-demand use by Auto-Align (autoAlignBoard()).
            m_lastDepthFrame = depth;

            // Throttle to ~3 Hz — distance/scale change slowly on a fixed rig.
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (nowMs - m_lastDepthMs < 300) return;
            m_lastDepthMs = nowMs;

            // Depth fill rate over the whole frame (valid = non-zero).
            const cv::Mat& d = *depth;
            const double area = static_cast<double>(d.rows) * d.cols;
            const double fillRatio = area > 0 ? cv::countNonZero(d) / area : -1.0;
            if (auto* sp = m_mainWindow->statsPanel()) sp->setFillRate(fillRatio);

            // Below this, specular reflections/glare on the board (or pointing
            // at empty space) have likely corrupted too much of the frame to
            // trust a median distance from it — mirrors BoardLocator's
            // kMinDepthFillRatio gate. Bail out instead of showing a
            // precise-looking but wrong "Distance" (e.g. 104mm when the real
            // distance is ~70mm) and feeding a bogus px/mm scale downstream.
            constexpr double kMinDepthFillRatio = 0.20;
            if (fillRatio < kMinDepthFillRatio) {
                if (auto* sp = m_mainWindow->statsPanel()) sp->setDistance(0.0);
                return;
            }

            // Median depth over a central ROI (20%), ignoring 0 = invalid.
            const int rw = std::max(1, d.cols / 5), rh = std::max(1, d.rows / 5);
            const cv::Rect roi((d.cols - rw) / 2, (d.rows - rh) / 2, rw, rh);
            std::vector<uint16_t> vals;
            vals.reserve(static_cast<size_t>(rw) * rh);
            for (int y = roi.y; y < roi.y + roi.height; ++y) {
                const auto* row = d.ptr<uint16_t>(y);
                for (int x = roi.x; x < roi.x + roi.width; ++x)
                    if (row[x] > 0) vals.push_back(row[x]);
            }
            if (vals.size() < 16) {            // too few valid samples
                if (auto* sp = m_mainWindow->statsPanel()) sp->setDistance(0.0);
                return;
            }
            std::nth_element(vals.begin(), vals.begin() + vals.size() / 2, vals.end());
            const double distMm = vals[vals.size() / 2];

            if (auto* sp = m_mainWindow->statsPanel()) sp->setDistance(distMm);
            m_lastDepthDistanceMm = distMm;

            // Auto px/mm from pinhole geometry: pixelsPerMm = fx / distance_mm.
            if (m_config->scaleMethod() == ScaleMethod::Depth) {
                const double fx = rs->colorFx();
                if (fx > 0 && distMm > 0) {
                    const double ppmm = fx / distMm;
                    m_currentPixelsPerMm = ppmm;
                    if (m_calibration) m_calibration->setPixelsPerMm(ppmm);
                    if (auto* sp = m_mainWindow->statsPanel()) sp->setScale(ppmm);
                    if (m_measurement) m_measurement->setCalibration(ppmm);
                    if (auto* cv = m_mainWindow->cameraView()) cv->setPixelsPerMm(ppmm);
                }
            }
        }, Qt::QueuedConnection);

        // 3D point cloud built on the capture thread (rs2::pointcloud) → view.
        connect(rs, &camera::RealSenseCapture::pointCloudReady, this,
                [this](ibom::camera::PointCloudRef cloud) {
            if (auto* pcv = m_mainWindow->pointCloudView())
                pcv->setCloud(cloud);
        }, Qt::QueuedConnection);

        // Histogram-equalized colorized depth (rs2::colorizer) → camera view
        // when the 2D depth view mode is active and IR view is not overriding.
        connect(rs, &camera::RealSenseCapture::colorizedDepthReady, this,
                [this, rs](ibom::camera::FrameRef rgb) {
            if (!m_depthViewMode || rs->emitInfrared() || !rgb || rgb->empty()) return;
            cv::Mat disp;
            cv::cvtColor(*rgb, disp, cv::COLOR_BGR2RGB);
            QImage qimg(disp.data, disp.cols, disp.rows,
                        static_cast<int>(disp.step), QImage::Format_RGB888);
            if (auto* view = m_mainWindow->cameraView())
                view->updateFrame(qimg.copy());
        }, Qt::QueuedConnection);

        // Left IR camera (grayscale-as-BGR) → camera view when IR mode active.
        // IR view overrides both color and colorized depth — useful for
        // reflective surfaces (solder, bare metal) per Intel tuning guide.
        connect(rs, &camera::RealSenseCapture::infraredReady, this,
                [this](ibom::camera::FrameRef ir) {
            if (!ir || ir->empty()) return;
            cv::Mat disp;
            cv::cvtColor(*ir, disp, cv::COLOR_BGR2RGB);
            QImage qimg(disp.data, disp.cols, disp.rows,
                        static_cast<int>(disp.step), QImage::Format_RGB888);
            if (auto* view = m_mainWindow->cameraView())
                view->updateFrame(qimg.copy());
        }, Qt::QueuedConnection);

        // Only pay these costs while the respective views are shown.
        rs->setEmitPointCloud(m_pointCloudMode);
        rs->setEmitColorizedDepth(m_depthViewMode);
        rs->setEmitInfrared(false);  // off by default; toggled in RealSenseControlsDialog
    }
#endif

    // Sync calibration UI for the new (or initial) backend.
    updateCalibrationUI();
}

void Application::connectControlSignals()
{
    // ── Camera settings from control panel ──────────────────────
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::cameraSettingsChanged,
            this, [this](int index, int w, int h, int fps) {
        // Sync to Config
        m_config->setCameraIndex(index);
        m_config->setCameraWidth(w);
        m_config->setCameraHeight(h);
        m_config->setCameraFps(fps);
        bool wasCapturing = m_camera->isCapturing();
        if (wasCapturing) m_camera->stop();
        m_camera->setDeviceIndex(index);
        m_camera->setResolution(w, h);
        m_camera->setFps(fps);
        m_camera->setHardwareDecode(m_config->cameraHwDecode());
        if (wasCapturing) m_camera->start();
        spdlog::info("Camera settings applied: device={} {}x{} @{}fps", index, w, h, fps);
    });

    // ── RealSense sensor controls (dynamic options panel) ───────
    // Reachable from the Control panel button and from Settings → Camera.
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::realSenseControlsRequested,
            this, &Application::openRealSenseControls);
    connect(m_mainWindow.get(), &gui::MainWindow::realSenseControlsRequested,
            this, &Application::openRealSenseControls);

    // ── Overlay opacity ─────────────────────────────────────────
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::overlayOpacityChanged,
            this, [this](float opacity) {
        m_config->setOverlayOpacity(opacity);
        m_mainWindow->cameraView()->setOverlayOpacity(opacity);
    });

    // ── Overlay visibility from control panel ───────────────────
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::showPadsChanged,
            this, [this](bool show) {
        m_config->setShowPads(show);
    });
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::showSilkscreenChanged,
            this, [this](bool show) {
        m_config->setShowSilkscreen(show);
    });
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::showFabricationChanged,
            this, [this](bool show) {
        m_config->setShowFabrication(show);
    });

    // ── AI: status bar indicator + detection confidence ─────────
    // aiStatusChanged is also emitted from the AI init thread; the queued
    // cross-thread delivery to the GUI is handled by Qt automatically.
    connect(this, &Application::aiStatusChanged,
            m_mainWindow.get(), &gui::MainWindow::updateAiStatus);

    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::confidenceChanged,
            this, [this](float conf) {
        m_config->setDetectionConfidence(conf);
        if (auto* detector = componentDetector())
            detector->setConfidenceThreshold(conf);
    });

    // ── iBOM file loading ───────────────────────────────────────
    connect(m_mainWindow.get(), &gui::MainWindow::ibomFileRequested,
            this, &Application::loadIBomFile);

    // ── Screenshot ──────────────────────────────────────────────
    connect(m_mainWindow.get(), &gui::MainWindow::screenshotRequested,
            this, &Application::takeScreenshot);

    // ── Calibration request ─────────────────────────────────────
    auto calibHandler = [this]() {
        spdlog::info("Calibration requested");
        if (!m_camera->isCapturing()) {
            QMessageBox::information(m_mainWindow.get(), tr("Calibration"),
                tr("Start the camera first before calibrating."));
            spdlog::warn("Calibration: camera not capturing");
            return;
        }
        if (!m_collectingCalibImages) {
            // Start calibration mode
            m_calibImages.clear();
            m_collectingCalibImages = true;
            if (m_calibMonitor) m_calibMonitor->setCaptureProgress(true, 0, 5);
            QMessageBox::information(m_mainWindow.get(), tr("Calibration"),
                tr("Calibration mode started.\n\n"
                   "1. Hold a %1x%2 checkerboard (squares %3 mm) in front of the microscope\n"
                   "2. Click 'Calibrate' again to capture (5 images needed)\n"
                   "3. Move the checkerboard to a different angle each time")
                .arg(m_config->calibBoardCols())
                .arg(m_config->calibBoardRows())
                .arg(static_cast<double>(m_config->calibSquareSize()), 0, 'f', 1));
            // Update button text to show progress
            for (auto* b : m_mainWindow->controlPanel()->findChildren<QPushButton*>()) {
                if (b->text().contains("Calibrat")) {
                    b->setText(tr("Capture Calibration Image (0/5)"));
                    break;
                }
            }
            spdlog::info("Calibration image collection started");
        } else {
            // Capture current frame — clone here: we store it long-term and
            // don't want the capture loop to overwrite the shared buffer.
            auto latest = m_camera->latestFrame();
            cv::Mat current = (latest && !latest->empty()) ? latest->clone() : cv::Mat();
            if (!current.empty()) {
                m_calibImages.push_back(current);
                int count = static_cast<int>(m_calibImages.size());
                spdlog::info("Calibration image {}/5 captured", count);
                if (m_calibMonitor) m_calibMonitor->setCaptureProgress(count < 5, count, 5);
                // Update button text
                for (auto* b : m_mainWindow->controlPanel()->findChildren<QPushButton*>()) {
                    if (b->text().contains("Capture") || b->text().contains("Calibrat")) {
                        b->setText(tr("Capture Calibration Image (%1/5)").arg(count));
                        break;
                    }
                }
                m_mainWindow->updateStatusMessage(
                    tr("Calibration: image %1/5 captured").arg(count));
                if (count >= 5) {
                    m_collectingCalibImages = false;
                    // Reset button text
                    for (auto* b : m_mainWindow->controlPanel()->findChildren<QPushButton*>()) {
                        if (b->text().contains("Capture") || b->text().contains("Calibrat")) {
                            b->setText(tr("Calibrate Camera (Checkerboard)"));
                            break;
                        }
                    }
                    runCalibration();
                }
            } else {
                spdlog::warn("Calibration: latestFrame is empty");
                QMessageBox::warning(m_mainWindow.get(), tr("Calibration"),
                    tr("No frame available from camera."));
            }
        }
    };
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::recalibrateRequested,
            this, calibHandler);
    connect(m_mainWindow.get(), &gui::MainWindow::calibrationRequested,
            this, calibHandler);

    // ── BOM panel component selection → overlay highlight + 2-comp align ─
    connect(m_mainWindow->bomPanel(), &gui::BomPanel::componentSelected,
            this, [this](const std::string& ref) {
        m_selectedRef = ref;
        m_mainWindow->boardMinimap()->setSelectedRef(ref);

        // Handle multi-component alignment selection. The marking method is
        // chosen once when multi-align starts, so selecting (or re-selecting) a
        // component here just arms it — no blocking popup, and switching to a
        // different component before clicking is allowed.
        if (m_alignMulti) {
            beginMarkComponent(ref);
            return;
        }

        // Handle 2-component alignment selection
        if (m_alignOnComponents) {
            // Find the component's center position
            const Component* comp = nullptr;
            for (const auto& c : m_ibomProject->components) {
                if (c.reference == ref) { comp = &c; break; }
            }
            if (!comp) return;

            cv::Point2f pcbPt(static_cast<float>(comp->bbox.center().x),
                              static_cast<float>(comp->bbox.center().y));

            if (m_alignCompStep == 0) {
                m_alignRef1 = ref;
                m_alignPcb1 = pcbPt;
                m_alignCompStep = 1;
                spdlog::info("2-comp align: comp1 = {} at PCB ({:.2f}, {:.2f})",
                             ref, pcbPt.x, pcbPt.y);
                m_mainWindow->updateStatusMessage(
                    tr("Now CLICK on %1 in the camera image").arg(QString::fromStdString(ref)));
            } else if (m_alignCompStep == 2) {
                if (ref == m_alignRef1) {
                    m_mainWindow->updateStatusMessage(
                        tr("Choose a DIFFERENT component for point 2"));
                    return;
                }
                m_alignRef2 = ref;
                m_alignPcb2 = pcbPt;
                m_alignCompStep = 3;
                spdlog::info("2-comp align: comp2 = {} at PCB ({:.2f}, {:.2f})",
                             ref, pcbPt.x, pcbPt.y);
                m_mainWindow->updateStatusMessage(
                    tr("Now CLICK on %1 in the camera image").arg(QString::fromStdString(ref)));
            }
            return;
        }

        spdlog::info("Component selected: {}", ref);
    });

    // ── FPS tracking timer ──────────────────────────────────────
    m_fpsTimer = new QTimer(this);
    connect(m_fpsTimer, &QTimer::timeout, this, [this]() {
        if (!m_mainWindow) return;
        double fps = m_frameCount.exchange(0) * 1000.0 / m_fpsTimer->interval();
        m_mainWindow->updateFpsDisplay(fps);
        if (auto* sp = m_mainWindow->statsPanel())
            sp->setFps(fps);
    });
    m_fpsTimer->start(1000);

    // ── Fullscreen toggle ───────────────────────────────────────
    connect(m_mainWindow.get(), &gui::MainWindow::fullscreenToggled,
            this, [this](bool fs) {
        if (fs)
            m_mainWindow->showFullScreen();
        else
            m_mainWindow->showNormal();
    });

    // ── Depth view toggle (RealSense) ───────────────────────────
    connect(m_mainWindow.get(), &gui::MainWindow::depthViewToggled,
            this, [this](bool depth) {
        m_depthViewMode = depth;
        spdlog::info("View mode: {}", depth ? "colorized depth" : "color");
#ifdef IBOM_HAVE_REALSENSE
        if (auto* rs = dynamic_cast<camera::RealSenseCapture*>(m_camera.get()))
            rs->setEmitColorizedDepth(depth);
#endif
    });

    // ── 3D point cloud toggle (RealSense) ───────────────────────
    connect(m_mainWindow.get(), &gui::MainWindow::pointCloudToggled,
            this, [this](bool on) {
        m_pointCloudMode = on;
        spdlog::info("3D point cloud view: {}", on ? "on" : "off");
#ifdef IBOM_HAVE_REALSENSE
        if (auto* rs = dynamic_cast<camera::RealSenseCapture*>(m_camera.get()))
            rs->setEmitPointCloud(on);
#endif
        if (!on && m_mainWindow->pointCloudView())
            m_mainWindow->pointCloudView()->clear();
    });

    // ── Settings changed ────────────────────────────────────────
    connect(m_mainWindow.get(), &gui::MainWindow::settingsChanged,
            this, [this]() {
        // Backend changed in the dialog? Recreate the camera (this also applies
        // resolution/fps/device and restarts if it was running) and stop here —
        // the index/restart logic below would operate on the new backend anyway.
        if (m_config->cameraBackend() != m_activeBackend) {
            switchCameraBackend(m_config->cameraBackend());
            return;
        }

        // Check if camera index changed and restart if needed
        int newIdx = m_config->cameraIndex();
        bool wasCapturing = m_camera->isCapturing();
        // Don't push the V4L2-oriented 1920x1080 default onto the D405 — it
        // doesn't support it and would log a spurious "unsupported, retrying"
        // fallback. Same guard as createCamera(); RealSense keeps its own res.
        const bool genericRes = (m_config->cameraWidth() == 1920 && m_config->cameraHeight() == 1080);
        const bool skipRes = (m_activeBackend == CameraBackend::RealSense && genericRes);
        if (wasCapturing) {
            m_camera->stop();
            m_camera->setDeviceIndex(newIdx);
            if (!skipRes)
                m_camera->setResolution(m_config->cameraWidth(), m_config->cameraHeight());
            m_camera->setFps(m_config->cameraFps());
            m_camera->setHardwareDecode(m_config->cameraHwDecode());
            m_camera->start();
            spdlog::info("Camera restarted on device {} ({}x{} @{}fps)",
                         newIdx, m_config->cameraWidth(), m_config->cameraHeight(),
                         m_config->cameraFps());
        } else {
            m_camera->setDeviceIndex(newIdx);
            if (!skipRes)
                m_camera->setResolution(m_config->cameraWidth(), m_config->cameraHeight());
            m_camera->setFps(m_config->cameraFps());
        }
        // Push updated tracking parameters to the worker — it will recreate
        // its detector and drop the current reference frame.
        if (m_trackingWorker) {
            QMetaObject::invokeMethod(m_trackingWorker, "configure", Qt::QueuedConnection,
                Q_ARG(int,    m_config->orbKeypoints()),
                Q_ARG(int,    m_config->minMatchCount()),
                Q_ARG(double, m_config->matchDistanceRatio()),
                Q_ARG(double, m_config->ransacThreshold()),
                Q_ARG(int,    m_config->trackingIntervalMs()),
                Q_ARG(float,  m_config->trackingDownscale()));
            QMetaObject::invokeMethod(m_trackingWorker, "setStabilization", Qt::QueuedConnection,
                Q_ARG(int,    m_config->trackingModel()),
                Q_ARG(double, m_config->oneEuroMinCutoff()),
                Q_ARG(double, m_config->oneEuroBeta()));
            QMetaObject::invokeMethod(m_trackingWorker, "setAdvanced", Qt::QueuedConnection,
                Q_ARG(bool, m_config->trackingClahe()),
                Q_ARG(bool, m_config->trackingOpticalFlow()),
                Q_ARG(int,  m_config->trackingGpuMode()));
            QMetaObject::invokeMethod(m_trackingWorker, "setIncrementalMode", Qt::QueuedConnection,
                Q_ARG(bool,   (m_config->cameraBackend() == CameraBackend::V4L2)
                              && m_config->microscopeIncremental()),
                Q_ARG(double, m_config->microscopeReanchorDriftPx()));
            QMetaObject::invokeMethod(m_trackingWorker, "setHybridCorrection", Qt::QueuedConnection,
                Q_ARG(bool, m_config->hybridDriftCorrection()));
        }
        // Apply re-anchor enable/interval changes from the dialog.
        updateReanchorTimer();
        spdlog::info("Settings applied (camera={}, ORB={}, interval={}ms, RANSAC={:.1f}, downscale={:.2f})",
                     newIdx, m_config->orbKeypoints(), m_config->trackingIntervalMs(),
                     m_config->ransacThreshold(), m_config->trackingDownscale());
        if (m_config->verboseLogging()) logFullState();  // full audit trail when debugging
        // Sync ControlPanel combo to new camera index
        auto* cp = m_mainWindow->controlPanel();
        if (cp && newIdx >= 0)
            cp->findChild<QComboBox*>()->setCurrentIndex(newIdx);

        // AI confidence may have changed in the dialog — sync the control
        // panel spinner and the live detector.
        m_mainWindow->controlPanel()->setConfidenceThreshold(
            m_config->detectionConfidence());
        if (auto* detector = componentDetector())
            detector->setConfidenceThreshold(m_config->detectionConfidence());

        // Remote view may have been toggled or moved to another port.
        applyRemoteViewConfig();

        // Apply optical multiplier change to pixels-per-mm — but ONLY when the
        // camera isn't checkerboard-calibrated. A real calibration already
        // captures the full optical chain (the microscope's 0.35× adapter +
        // 0.7× lens, etc.), so the multiplier must not skew it; it's purely a
        // manual nudge for the un-calibrated nominal case.
        const bool calibrated = m_calibration && m_calibration->isCalibrated();
        float mult = m_config->opticalMultiplier();
        if (!calibrated && mult > 0 && m_basePixelsPerMm > 0) {
            m_currentPixelsPerMm = m_basePixelsPerMm * mult;
            if (m_calibration)
                m_calibration->setPixelsPerMm(m_currentPixelsPerMm);
            if (auto* sp = m_mainWindow->statsPanel())
                sp->setScale(m_currentPixelsPerMm);
            spdlog::info("Optical multiplier={:.2f}x → effective px/mm={:.2f}",
                         mult, m_currentPixelsPerMm);
        }
    });

    // ── Heatmap toggle ──────────────────────────────────────────
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::showHeatmapChanged,
            this, [this](bool show) {
        m_showHeatmap = show;
        spdlog::info("Heatmap overlay {}", show ? "enabled" : "disabled");
    });

    // ── InspectionWizard wiring ─────────────────────────────────
    auto* wizard = m_mainWindow->inspectionWizard();
    if (wizard) {
        connect(wizard, &gui::InspectionWizard::inspectionStarted,
                this, [this](const QStringList& refs) {
            spdlog::info("Inspection started with {} components", refs.size());
            m_mainWindow->updateStatusMessage(
                tr("Inspection running — %1 components").arg(refs.size()));
        });

        connect(wizard, &gui::InspectionWizard::inspectionCancelled,
                this, [this]() {
            spdlog::info("Inspection cancelled by user");
            m_mainWindow->updateStatusMessage(tr("Inspection cancelled"));
        });

        connect(wizard, &gui::InspectionWizard::inspectionFinished,
                this, [this]() {
            spdlog::info("Inspection finished");
            m_mainWindow->updateStatusMessage(tr("Inspection complete"));
        });

        connect(wizard, &gui::InspectionWizard::componentNavigated,
                this, [this](const std::string& ref) {
            m_selectedRef = ref;
            spdlog::info("Navigate to component: {}", ref);
        });
    }

    // ── Alignment Assistant (guided wizard) ─────────────────────
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::alignmentWizardRequested,
            this, [this]() {
        if (!m_alignWizard) {
            m_alignWizard = new gui::AlignmentWizard(m_mainWindow.get());
            // The wizard only orchestrates: forward its "start" to the existing
            // alignment code paths via the same ControlPanel signals the manual
            // buttons use (signal→signal). The interactive clicking is unchanged.
            connect(m_alignWizard, &gui::AlignmentWizard::startRequested,
                    this, [this](int method) {
                auto* cp = m_mainWindow->controlPanel();
                switch (method) {
                case gui::AlignmentWizard::FourCorners:    emit cp->alignHomographyRequested();   break;
                case gui::AlignmentWizard::TwoComponents:  emit cp->alignOnComponentsRequested(); break;
                case gui::AlignmentWizard::MultiComponent: emit cp->alignMultiRequested();        break;
                case gui::AlignmentWizard::AutoAlign:      emit cp->autoAlignRequested();         break;
                default: break;
                }
            });
        }
        m_alignWizard->setBackendIsRealSense(
            m_config->cameraBackend() == CameraBackend::RealSense);
        m_alignWizard->restart();
        m_alignWizard->show();
        m_alignWizard->raise();
        m_alignWizard->activateWindow();
    });

    // ── Manual Homography — point picking ───────────────────────
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::alignHomographyRequested,
            this, [this]() {
        if (!m_ibomProject) {
            QMessageBox::information(m_mainWindow.get(), tr("Alignment"),
                tr("Load an iBOM file first before setting alignment points."));
            return;
        }
        if (!m_camera->isCapturing()) {
            QMessageBox::information(m_mainWindow.get(), tr("Alignment"),
                tr("Start the camera first before setting alignment points."));
            return;
        }
        m_alignOnComponents = false;  // cancel any 2-comp align in progress
        m_alignMulti = false;         // cancel any multi-comp align in progress
        setMultiAlignUIState(false);
        m_pickingHomographyPoints = true;
        m_homographyImagePoints.clear();
        m_mainWindow->updateStatusMessage(
            tr("Click the TOP-LEFT corner of the PCB in the camera image (1/4)"));
        spdlog::info("Manual homography: point picking started");
    });

    // ── 2-Component Alignment ───────────────────────────────────
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::alignOnComponentsRequested,
            this, [this]() {
        if (!m_ibomProject) {
            QMessageBox::information(m_mainWindow.get(), tr("Alignment"),
                tr("Load an iBOM file first."));
            return;
        }
        if (!m_camera->isCapturing()) {
            QMessageBox::information(m_mainWindow.get(), tr("Alignment"),
                tr("Start the camera first."));
            return;
        }
        m_pickingHomographyPoints = false;  // cancel any 4-corner align
        m_alignMulti = false;               // cancel any multi-comp align
        setMultiAlignUIState(false);
        m_mainWindow->showBomPanel();
        m_alignOnComponents = true;
        m_alignCompStep = 0;
        m_mainWindow->updateStatusMessage(
            tr("Select the FIRST component in the BOM panel (choose 2 components far apart)"));
        spdlog::info("2-component alignment started");
    });

    // ── Multi-Component Alignment (≥2 landmarks; non-rectangular boards) ──
    // Click once to start collecting; click again to finish & compute. Each
    // component is marked either by its 2 opposite corners (midpoint) or pin 1.
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::alignMultiRequested,
            this, [this]() {
        if (m_alignMulti) {
            // Second click = finish & compute.
            applyMultiAlignment();
            return;
        }
        if (!m_ibomProject) {
            QMessageBox::information(m_mainWindow.get(), tr("Alignment"),
                tr("Load an iBOM file first."));
            return;
        }
        if (!m_camera->isCapturing()) {
            QMessageBox::information(m_mainWindow.get(), tr("Alignment"),
                tr("Start the camera first."));
            return;
        }
        m_pickingHomographyPoints = false;
        m_alignOnComponents = false;
        m_alignMulti = true;
        m_alignMultiAwaitClick = false;
        m_alignMultiHaveCorner1 = false;
        m_alignMultiMethod = gui::MultiAlignDialog::OppositePads;  // default
        m_alignMultiPcbPts.clear();
        m_alignMultiImgPts.clear();
        m_alignMultiRefs.clear();
        m_selectedRef.clear();
        setMultiAlignUIState(true);
        m_mainWindow->showBomPanel();
        showMultiAlignDialog();  // persistent, non-modal: method choice + progress
        m_mainWindow->updateStatusMessage(
            tr("Multi-align: pick a component in the BOM panel or the PCB Map, "
               "choose how to mark it, then click the green target(s) in the image."));
        spdlog::info("Multi-component alignment started (method {})", m_alignMultiMethod);
    });

    // ── Reset Alignment ──────────────────────────────────────────
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::resetAlignmentRequested,
            this, [this]() {
        // Cancel any in-progress alignment flow first.
        m_pickingHomographyPoints = false;
        m_alignOnComponents = false;
        m_alignCompStep = 0;
        m_alignMulti = false;
        m_alignMultiAwaitClick = false;
        m_alignMultiHaveCorner1 = false;
        m_anchorMode = false;
        setMultiAlignUIState(false);  // also clears the PCB Map click targets

        m_homography->reset();
        ++m_alignmentEpoch;
        m_currentPixelsPerMm = 0.0;
        if (auto* sp = m_mainWindow->statsPanel()) sp->setScale(0.0);

        // Forget the saved profile too, so it isn't offered back on next reload.
        m_config->clearSavedAlignment();
        m_config->save();

        m_mainWindow->updateStatusMessage(
            tr("Alignment reset — the overlay is unaligned. Use one of the "
               "Align buttons (or the Alignment Assistant) to set it up again."));
        spdlog::info("Alignment reset by user");
    });

    // ── Auto-Align (board outline detection) ────────────────────
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::autoAlignRequested,
            this, [this]() {
        if (!m_ibomProject) {
            QMessageBox::information(m_mainWindow.get(), tr("Auto-Align"),
                tr("Load an iBOM file first."));
            return;
        }
        if (!m_camera->isCapturing()) {
            QMessageBox::information(m_mainWindow.get(), tr("Auto-Align"),
                tr("Start the camera first."));
            return;
        }
        autoAlignBoard();
    });

    connect(m_mainWindow->cameraView(), &gui::CameraView::clicked,
            this, [this](QPointF imagePos) {
        // ── Multi-component alignment click handling ──
        if (m_alignMulti && m_alignMultiAwaitClick) {
            cv::Point2f imgPt(static_cast<float>(imagePos.x()),
                              static_cast<float>(imagePos.y()));
            // Snap the raw click onto the nearest strong local feature
            // (cv::cornerSubPix) — hand clicks are limited by mouse/screen
            // resolution, this recovers sub-pixel precision when there's a
            // real corner/edge nearby (silkscreen edge, pad corner, etc.).
            imgPt = refineClickPoint(imgPt);
            if (m_alignMultiMethod != 1) {  // 2 clicks → midpoint (body corners or opposite pads)
                if (!m_alignMultiHaveCorner1) {
                    m_alignMultiCorner1 = imgPt;
                    m_alignMultiHaveCorner1 = true;
                    m_mainWindow->updateStatusMessage(
                        m_alignMultiMethod == 2
                            ? tr("Click the OPPOSITE pad of %1")
                                  .arg(QString::fromStdString(m_alignMultiRef))
                            : tr("Click the OPPOSITE corner of %1's body")
                                  .arg(QString::fromStdString(m_alignMultiRef)));
                    return;
                }
                cv::Point2f mid((m_alignMultiCorner1.x + imgPt.x) * 0.5f,
                                (m_alignMultiCorner1.y + imgPt.y) * 0.5f);
                m_alignMultiPcbPts.push_back(m_alignMultiPcb);
                m_alignMultiImgPts.push_back(mid);
                m_alignMultiRefs.push_back(m_alignMultiRef);
            } else {  // pin 1: single click
                m_alignMultiPcbPts.push_back(m_alignMultiPcb);
                m_alignMultiImgPts.push_back(imgPt);
                m_alignMultiRefs.push_back(m_alignMultiRef);
            }
            m_alignMultiAwaitClick = false;
            m_alignMultiHaveCorner1 = false;
            m_selectedRef.clear();  // so a method change doesn't re-arm a finished one
            m_mainWindow->boardMinimap()->setClickTargets({});  // done with this component
            const int n = static_cast<int>(m_alignMultiImgPts.size());
            const QString msg =
                tr("Marked %1 component(s). Pick another (BOM panel or PCB Map), "
                   "or press 'Finish && Align'%2.")
                .arg(n)
                .arg(n >= 4 ? tr(" — ≥4: perspective corrected")
                   : n >= 2 ? tr(" — ≥2 OK, 4 is better") : QString());
            m_mainWindow->updateStatusMessage(msg);
            if (m_multiAlignDialog) {
                m_multiAlignDialog->setMarkedCount(n);
                m_multiAlignDialog->setSelectedComponent(QString());
                m_multiAlignDialog->setStatus(msg);
            }
            spdlog::info("Multi-align: marked {} ({} total)", m_alignMultiRef, n);
            return;
        }

        // ── Microscope 1-point anchor click handling ──
        if (m_anchorMode) {
            m_anchorMode = false;
            if (!m_homography) return;

            cv::Point2f imgPt(static_cast<float>(imagePos.x()),
                              static_cast<float>(imagePos.y()));

            // Use the live scale if we have one, else the configured fallback.
            double scale = (m_currentPixelsPerMm > 0.0)
                ? m_currentPixelsPerMm
                : m_config->microscopeAnchorPixelsPerMm();
            if (scale <= 0.0) scale = 20.0;
            double rot = m_config->microscopeAnchorRotationDeg() * CV_PI / 180.0;

            double cosR = std::cos(rot) * scale;
            double sinR = std::sin(rot) * scale;

            // Translation so the target component maps to the clicked point.
            double tx = imgPt.x - (cosR * m_anchorPcb.x - sinR * m_anchorPcb.y);
            double ty = imgPt.y - (sinR * m_anchorPcb.x + cosR * m_anchorPcb.y);

            auto& bb = m_ibomProject->boardInfo.boardBBox;
            std::vector<cv::Point2f> pcbCorners = {
                {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
                {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
                {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
                {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}
            };
            std::vector<cv::Point2f> imgCorners;
            for (const auto& p : pcbCorners) {
                imgCorners.push_back({
                    static_cast<float>(cosR * p.x - sinR * p.y + tx),
                    static_cast<float>(sinR * p.x + cosR * p.y + ty)});
            }

            ++m_alignmentEpoch;
            if (m_homography->compute(pcbCorners, imgCorners)) {
                m_mainWindow->boardMinimap()->update();
                m_basePixelsPerMm = scale;
                m_currentPixelsPerMm = scale;
                if (auto* sp = m_mainWindow->statsPanel())
                    sp->setScale(m_currentPixelsPerMm);
                if (m_trackingWorker)
                    QMetaObject::invokeMethod(m_trackingWorker, "resetReference",
                                              Qt::QueuedConnection);
                m_mainWindow->updateStatusMessage(
                    tr("Anchored on %1 — scale: %2 px/mm (refine with 2-component align if needed)")
                        .arg(QString::fromStdString(m_anchorRef))
                        .arg(scale, 0, 'f', 1));
                spdlog::info("Anchored on {}: scale={:.2f} px/mm rot={:.1f}°",
                             m_anchorRef, scale, m_config->microscopeAnchorRotationDeg());
                autoStartLiveTracking();
            } else {
                m_mainWindow->updateStatusMessage(tr("Anchor failed"));
                spdlog::error("Anchor: homography compute failed");
            }
            return;
        }

        // ── 2-component alignment click handling ──
        if (m_alignOnComponents && (m_alignCompStep == 1 || m_alignCompStep == 3)) {
            cv::Point2f imgPt(static_cast<float>(imagePos.x()),
                              static_cast<float>(imagePos.y()));

            if (m_alignCompStep == 1) {
                m_alignImg1 = imgPt;
                m_alignCompStep = 2;
                spdlog::info("2-comp align: comp1 '{}' clicked at ({:.0f}, {:.0f})",
                             m_alignRef1, imgPt.x, imgPt.y);
                m_mainWindow->updateStatusMessage(
                    tr("Now select the SECOND component in the BOM panel"));
            } else { // step 3
                m_alignImg2 = imgPt;
                spdlog::info("2-comp align: comp2 '{}' clicked at ({:.0f}, {:.0f})",
                             m_alignRef2, imgPt.x, imgPt.y);
                m_alignOnComponents = false;
                m_alignCompStep = 0;

                // Compute similarity transform from 2 point pairs
                // PCB coords → image coords
                // A similarity has 4 DOF: scale, rotation, tx, ty
                // From 2 points we get exactly 4 equations
                double dx_pcb = m_alignPcb2.x - m_alignPcb1.x;
                double dy_pcb = m_alignPcb2.y - m_alignPcb1.y;
                double dx_img = m_alignImg2.x - m_alignImg1.x;
                double dy_img = m_alignImg2.y - m_alignImg1.y;

                double dist_pcb = std::sqrt(dx_pcb * dx_pcb + dy_pcb * dy_pcb);
                double dist_img = std::sqrt(dx_img * dx_img + dy_img * dy_img);

                if (dist_pcb < 0.1 || dist_img < 1.0) {
                    QMessageBox::warning(m_mainWindow.get(), tr("Alignment Failed"),
                        tr("The two components are too close together. Choose components that are far apart."));
                    m_mainWindow->updateStatusMessage(tr("Alignment failed — components too close"));
                    return;
                }

                // Similarity transform: scale, rotation
                double scale = dist_img / dist_pcb;
                double angle_pcb = std::atan2(dy_pcb, dx_pcb);
                double angle_img = std::atan2(dy_img, dx_img);
                double rot = angle_img - angle_pcb;

                double cosR = std::cos(rot) * scale;
                double sinR = std::sin(rot) * scale;

                // Translation from point 1
                double tx = m_alignImg1.x - (cosR * m_alignPcb1.x - sinR * m_alignPcb1.y);
                double ty = m_alignImg1.y - (sinR * m_alignPcb1.x + cosR * m_alignPcb1.y);

                // Build 4 virtual corners from the board bbox using the similarity
                auto& bb = m_ibomProject->boardInfo.boardBBox;
                std::vector<cv::Point2f> pcbCorners = {
                    {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
                    {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
                    {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
                    {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}
                };
                std::vector<cv::Point2f> imgCorners;
                for (const auto& p : pcbCorners) {
                    float ix = static_cast<float>(cosR * p.x - sinR * p.y + tx);
                    float iy = static_cast<float>(sinR * p.x + cosR * p.y + ty);
                    imgCorners.push_back({ix, iy});
                }

                ++m_alignmentEpoch;
                if (m_homography->compute(pcbCorners, imgCorners)) {
                    m_basePixelsPerMm = scale;
                    m_currentPixelsPerMm = scale;
                    if (auto* sp = m_mainWindow->statsPanel())
                        sp->setScale(m_currentPixelsPerMm);
                    if (m_trackingWorker)
                        QMetaObject::invokeMethod(m_trackingWorker, "resetReference",
                                                  Qt::QueuedConnection);

                    spdlog::info("2-comp alignment OK: scale={:.2f} px/unit, rot={:.1f}°",
                                 scale, rot * 180.0 / CV_PI);
                    reportAlignmentResult(
                        tr("Alignment set from %1 + %2 — scale: %3 px/mm")
                        .arg(QString::fromStdString(m_alignRef1))
                        .arg(QString::fromStdString(m_alignRef2))
                        .arg(scale, 0, 'f', 1));
                    autoStartLiveTracking();
                } else {
                    spdlog::error("2-comp alignment: homography compute failed");
                    m_mainWindow->updateStatusMessage(tr("Alignment failed"));
                }
            }
            return;
        }

        // ── 4-corner alignment click handling ──
        if (!m_pickingHomographyPoints) return;

        m_homographyImagePoints.push_back(
            cv::Point2f(static_cast<float>(imagePos.x()),
                        static_cast<float>(imagePos.y())));

        int n = static_cast<int>(m_homographyImagePoints.size());
        static const char* labels[] = {"TOP-LEFT", "TOP-RIGHT", "BOTTOM-RIGHT", "BOTTOM-LEFT"};

        if (n < 4) {
            m_mainWindow->updateStatusMessage(
                tr("Click the %1 corner of the PCB in the camera image (%2/4)")
                .arg(labels[n]).arg(n + 1));
            spdlog::info("Manual homography: point {}/4 at ({:.0f}, {:.0f})",
                         n, imagePos.x(), imagePos.y());
        } else {
            m_pickingHomographyPoints = false;
            spdlog::info("Manual homography: point 4/4 at ({:.0f}, {:.0f})",
                         imagePos.x(), imagePos.y());

            // Compute homography: PCB board corners → clicked image points
            auto& bb = m_ibomProject->boardInfo.boardBBox;
            std::vector<cv::Point2f> pcbPts = {
                {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},  // TL
                {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},  // TR
                {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},  // BR
                {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}   // BL
            };

            ++m_alignmentEpoch;
            if (m_homography->compute(pcbPts, m_homographyImagePoints)) {
                spdlog::info("Manual homography computed successfully (error={:.3f}px)",
                             m_homography->reprojectionError());
                reportAlignmentResult(
                    tr("4-corner alignment set — reprojection error: %1 px")
                    .arg(m_homography->reprojectionError(), 0, 'f', 3));
                // Capture baseline scale for dynamic zoom tracking
                if (m_calibration && m_calibration->pixelsPerMm() > 0) {
                    m_basePixelsPerMm = m_calibration->pixelsPerMm();
                    m_currentPixelsPerMm = m_basePixelsPerMm;
                } else {
                    // Estimate from homography: pixels per PCB unit
                    auto& bb = m_ibomProject->boardInfo.boardBBox;
                    double pcbW = bb.width();
                    auto tl = m_homography->pcbToImage({static_cast<float>(bb.minX), static_cast<float>(bb.minY)});
                    auto tr = m_homography->pcbToImage({static_cast<float>(bb.maxX), static_cast<float>(bb.minY)});
                    double pixW = cv::norm(cv::Point2f(tl.x - tr.x, tl.y - tr.y));
                    if (pcbW > 0) {
                        m_basePixelsPerMm = pixW / pcbW;
                        m_currentPixelsPerMm = m_basePixelsPerMm;
                    }
                }
                if (auto* sp = m_mainWindow->statsPanel())
                    sp->setScale(m_currentPixelsPerMm);
                if (m_trackingWorker)
                    QMetaObject::invokeMethod(m_trackingWorker, "resetReference",
                                              Qt::QueuedConnection);
                autoStartLiveTracking();
            } else {
                spdlog::error("Manual homography computation failed");
                m_mainWindow->updateStatusMessage(tr("Alignment failed — try again"));
                QMessageBox::warning(m_mainWindow.get(), tr("Alignment Failed"),
                    tr("Could not compute homography from the selected points.\n"
                       "Make sure you click the 4 corners in order: TL, TR, BR, BL."));
            }
            m_homographyImagePoints.clear();
        }
    });

    // ── Live Tracking Mode ──────────────────────────────────────
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::liveModeChanged,
            this, [this](bool enabled) {
        m_liveMode = enabled;
        if (enabled) {
            if (!m_homography->isValid()) {
                QMessageBox::information(m_mainWindow.get(), tr("Live Mode"),
                    tr("Set alignment points or load an iBOM file first."));
                m_liveMode = false;
                // Keep the checkbox truthful (it stayed checked while
                // m_liveMode was silently reset). Re-enters this handler with
                // enabled=false — the disable path below is a benign no-op.
                if (auto* cp = m_mainWindow->controlPanel())
                    cp->setLiveMode(false);
                return;
            }
            m_baseHomography = m_homography->matrix().clone();
            if (m_trackingWorker) {
                QMetaObject::invokeMethod(m_trackingWorker, "setBaseHomography",
                    Qt::QueuedConnection, Q_ARG(cv::Mat, m_baseHomography));
                QMetaObject::invokeMethod(m_trackingWorker, "resetReference",
                    Qt::QueuedConnection);
            }
            spdlog::info("Live tracking mode enabled");
            m_mainWindow->updateStatusMessage(tr("Live tracking mode ON"));
        } else {
            // Restore base homography
            if (!m_baseHomography.empty()) {
                m_homography->setMatrix(m_baseHomography);
            }
            if (m_trackingWorker)
                QMetaObject::invokeMethod(m_trackingWorker, "resetReference",
                                          Qt::QueuedConnection);
            spdlog::info("Live tracking mode disabled");
            m_mainWindow->updateStatusMessage(tr("Live tracking mode OFF"));
        }
    });

    // ── Hybrid drift correction (beta) ──────────────────────────
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::hybridModeChanged,
            this, [this](bool enabled) {
        m_config->setHybridDriftCorrection(enabled);
        m_config->save();
        if (m_trackingWorker)
            QMetaObject::invokeMethod(m_trackingWorker, "setHybridCorrection",
                Qt::QueuedConnection, Q_ARG(bool, enabled));
        spdlog::info("Hybrid drift correction {}", enabled ? "enabled" : "disabled");
    });

    // ── Inspection workflow wiring ──────────────────────────────
    auto* inspPanel = m_mainWindow->inspectionPanel();
    auto* bomPanel  = m_mainWindow->bomPanel();

    // ibomLoaded → enable inspection panel
    connect(this, &Application::ibomLoaded,
            inspPanel, &gui::InspectionPanel::onIBomLoaded);

    // Start inspection: load components into PickAndPlace
    connect(inspPanel, &gui::InspectionPanel::startInspectionClicked,
            this, &Application::startInspection);

    // PickAndPlace → InspectionPanel UI + overlay highlight + BomPanel state
    connect(m_pickAndPlace.get(), &features::PickAndPlace::currentStepChanged,
            this, [this, inspPanel, bomPanel](const features::PickAndPlace::PlacementStep& step) {
        m_selectedRef = step.reference;
        if (bomPanel)
            bomPanel->highlightComponent(step.reference);
        inspPanel->onStepChanged(
            QString::fromStdString(step.reference),
            QString::fromStdString(step.value),
            QString::fromStdString(step.footprint),
            step.layer == Layer::Front ? tr("Top") : tr("Bottom"),
            m_pickAndPlace->currentIndex() + 1,
            m_pickAndPlace->totalSteps());
    });

    connect(m_pickAndPlace.get(), &features::PickAndPlace::progressChanged,
            inspPanel, &gui::InspectionPanel::onProgress);
    connect(m_pickAndPlace.get(), &features::PickAndPlace::progressChanged,
            bomPanel, &gui::BomPanel::setProgress);

    connect(m_pickAndPlace.get(), &features::PickAndPlace::stepPlaced,
            this, [this, bomPanel](const std::string& ref) {
        m_placedRefs.insert(ref);
        if (bomPanel) bomPanel->setComponentState(ref, tr("Placed"));
        m_mainWindow->boardMinimap()->setPlacedRefs(m_placedRefs);
        // Persist on every placement: a restart (app or device) resumes
        // exactly where the inspection stopped.
        saveInspectionState();
    });

    connect(m_pickAndPlace.get(), &features::PickAndPlace::allPlaced,
            inspPanel, &gui::InspectionPanel::onAllPlaced);

    // InspectionPanel buttons → PickAndPlace
    connect(inspPanel, &gui::InspectionPanel::placedClicked,
            m_pickAndPlace.get(), &features::PickAndPlace::markPlaced);
    connect(inspPanel, &gui::InspectionPanel::skipClicked,
            this, [this, bomPanel]() {
        if (bomPanel && m_pickAndPlace->currentIndex() < m_pickAndPlace->totalSteps()) {
            const auto& step = m_pickAndPlace->currentStep();
            bomPanel->setComponentState(step.reference, tr("Skipped"));
        }
        m_pickAndPlace->skip();
    });
    connect(inspPanel, &gui::InspectionPanel::backClicked,
            m_pickAndPlace.get(), &features::PickAndPlace::goBack);
    connect(inspPanel, &gui::InspectionPanel::resetClicked,
            this, [this]() {
        m_placedRefs.clear();
        saveInspectionState();  // empty set removes the saved entry
    });
    connect(inspPanel, &gui::InspectionPanel::resetClicked,
            m_pickAndPlace.get(), &features::PickAndPlace::reset);

    // Measurement: mode change toggles CameraView measure mode + sets calibration
    auto* camView = m_mainWindow->cameraView();
    connect(inspPanel, &gui::InspectionPanel::measurementModeChanged,
            this, [this, camView](int mode) {
        bool active = (mode >= 0);
        camView->setMeasurementMode(active);
        if (active) {
            m_measurement->setCalibration(m_currentPixelsPerMm);
            m_measurement->setMode(static_cast<features::Measurement::Mode>(mode));
            m_measurement->clearPoints();
            camView->setPixelsPerMm(m_currentPixelsPerMm);
            camView->setMeasureModeKind(mode);
        }
    });

    // CameraView clicks in measure mode → Measurement::addPoint
    connect(camView, &gui::CameraView::measurePoint,
            this, [this](QPointF imagePos) {
        m_measurement->addPoint(imagePos);
    });

    // Right-click during measurement: cancel current shape
    connect(camView, &gui::CameraView::measureCanceled,
            this, [this]() {
        m_measurement->clearPoints();
    });

    // Double-click in Area mode with ≥3 points: close polygon
    connect(camView, &gui::CameraView::areaCloseRequested,
            this, [this]() {
        m_measurement->commitCurrent();
    });

    connect(m_measurement.get(), &features::Measurement::measurementComplete,
            this, [this, inspPanel, camView](const features::Measurement::MeasureResult& r) {
        QString unit;
        switch (r.mode) {
        case features::Measurement::Mode::Distance:
        case features::Measurement::Mode::PinPitch: unit = "mm";  break;
        case features::Measurement::Mode::Angle:    unit = "deg"; break;
        case features::Measurement::Mode::Area:     unit = "mm²"; break;
        }
        inspPanel->onMeasurementResult(r.valuePixels, r.valueMM, unit);

        // Persist into CameraView history (rendered faded)
        std::vector<QPointF> pts(r.points.begin(), r.points.end());
        camView->appendCompletedMeasure(static_cast<int>(r.mode), pts,
                                        r.valuePixels, r.valueMM);
    });

    connect(inspPanel, &gui::InspectionPanel::clearMeasurementsClicked,
            this, [this, camView]() {
        m_measurement->clearPoints();
        m_measurement->clearHistory();
        camView->clearMeasureHistory();
        camView->clearCurrentMeasurePoints();
    });

    // Snapshot
    connect(inspPanel, &gui::InspectionPanel::snapshotClicked,
            this, &Application::onSnapshot);
    connect(m_snapshotHistory.get(), &features::SnapshotHistory::snapshotTaken,
            inspPanel, &gui::InspectionPanel::onSnapshotTaken);
    connect(inspPanel, &gui::InspectionPanel::openSnapshotsFolderClicked,
            this, [this]() {
        QString dir = m_snapshotHistory->storageDir();
        if (!dir.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });

    // Export
    connect(inspPanel, &gui::InspectionPanel::exportRequested,
            this, &Application::onExport);

    // ── Camera profile selector ─────────────────────────────────
    connect(m_mainWindow.get(), &gui::MainWindow::cameraProfileChangeRequested,
            this, [this](int idx) { switchProfile(idx); });
    connect(this, &Application::cameraProfileChanged,
            m_mainWindow.get(), &gui::MainWindow::setActiveProfile);

    connect(m_mainWindow.get(), &gui::MainWindow::componentAnchorRequested,
            this, [this]() { startComponentAnchor(); });

    // ── BoardMinimap ────────────────────────────────────────────────
    // anchorRequested(pcbPoint): on the microscope backend (narrow FOV), treat
    // it as a 1-point anchor at that PCB position, centering the FOV on the
    // clicked point — reuses the similarity-transform logic that
    // startComponentAnchor() / the CameraView click handler use. On
    // RealSense (wide FOV, the whole board is already visible) re-centering
    // the FOV makes no sense — instead, highlight the nearest component, the
    // same effect as clicking it in the BOM panel.
    connect(m_mainWindow->boardMinimap(), &gui::BoardMinimap::anchorRequested,
            this, [this](cv::Point2f pcbPt) {
        if (!m_ibomProject) return;

        // During Multi-Comp alignment a minimap click picks the nearest
        // component as the next landmark — same as selecting it in the BOM
        // panel, but without leaving the camera view. Switching is free
        // (beginMarkComponent re-arms), so a wrong pick needs no cancel.
        if (m_alignMulti) {
            const Component* pick = componentAtPcb(pcbPt);
            if (pick) beginMarkComponent(pick->reference);
            return;
        }

        if (m_config->cameraBackend() == CameraBackend::RealSense) {
            const Component* nearest = componentAtPcb(pcbPt);
            if (!nearest) return;
            m_selectedRef = nearest->reference;
            m_mainWindow->boardMinimap()->setSelectedRef(m_selectedRef);
            if (auto* bomPanel = m_mainWindow->bomPanel())
                bomPanel->highlightComponent(m_selectedRef);
            m_mainWindow->updateStatusMessage(
                tr("Selected %1").arg(QString::fromStdString(m_selectedRef)));
            return;
        }

        double scale = (m_currentPixelsPerMm > 0.0) ? m_currentPixelsPerMm
                       : m_config->microscopeAnchorPixelsPerMm();
        double rot  = m_config->microscopeAnchorRotationDeg() * CV_PI / 180.0;
        double cosR = std::cos(rot) * scale;
        double sinR = std::sin(rot) * scale;

        // Center of the camera image
        float iw = static_cast<float>(m_camera->resolution().width());
        float ih = static_cast<float>(m_camera->resolution().height());
        if (iw <= 0 || ih <= 0) { iw = 1920; ih = 1080; }
        cv::Point2f imgCenter(iw / 2.f, ih / 2.f);

        // Image origin: imgCenter = H * pcbPt  ⟹ tx = cx - (cosR*px - sinR*py)
        double tx = imgCenter.x - (cosR * pcbPt.x - sinR * pcbPt.y);
        double ty = imgCenter.y - (sinR * pcbPt.x + cosR * pcbPt.y);

        auto& bb = m_ibomProject->boardInfo.boardBBox;
        std::vector<cv::Point2f> pcbCorners = {
            {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
            {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
            {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
            {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}
        };
        std::vector<cv::Point2f> imgCorners;
        imgCorners.reserve(pcbCorners.size());
        for (auto& pc : pcbCorners) {
            imgCorners.push_back({
                static_cast<float>(cosR * pc.x - sinR * pc.y + tx),
                static_cast<float>(sinR * pc.x + cosR * pc.y + ty)
            });
        }
        ++m_alignmentEpoch;
        if (m_homography->compute(pcbCorners, imgCorners)) {
            if (m_liveMode) m_baseHomography = m_homography->matrix().clone();
            updateDynamicScale();
            m_mainWindow->updateStatusMessage(tr("Minimap anchor applied"));
            spdlog::info("Minimap anchor: PCB ({:.2f}, {:.2f}) → image center", pcbPt.x, pcbPt.y);
            autoStartLiveTracking();
        }
    });

    // ── Dev menu: FOV & Scale measurement dialog ─────────────────────
    connect(m_mainWindow.get(), &gui::MainWindow::fovMeasureRequested,
            this, [this]() {
        gui::FovMeasureDialog::Metrics m;

        // Camera & profile
        const auto& profiles = m_config->profiles();
        const int pidx = m_config->activeProfileIndex();
        m.profileName = (pidx >= 0 && pidx < static_cast<int>(profiles.size()))
            ? QString::fromStdString(profiles[pidx].name) : tr("(none)");
        m.isMicroscope = (m_config->cameraBackend() == CameraBackend::V4L2);
        m.backendName = m.isMicroscope
            ? tr("V4L2 (microscope, index %1)").arg(m_config->cameraIndex())
            : tr("RealSense D405 (fixed lens, no zoom)");
        m.camWidth  = m_config->cameraWidth();
        m.camHeight = m_config->cameraHeight();
        if (m_camera) {
            m.camWidth  = m_camera->resolution().width();
            m.camHeight = m_camera->resolution().height();
        }

        // Calibration
        m.calibrated  = m_calibration && m_calibration->isCalibrated();
        m.calibRmsErr = m_calibration ? m_calibration->rmsError() : 0.0;

        // Scale & FOV. The scale source depends on the backend: the D405 derives
        // it live from depth (fx / distance); the microscope from the tracking
        // homography, or the config fallback before the first anchor.
        m.configAnchorPxPerMm = m_config->microscopeAnchorPixelsPerMm();
        if (m_currentPixelsPerMm > 0.0) {
            m.pixelsPerMm = m_currentPixelsPerMm;
            m.scaleSource = (!m.isMicroscope
                             && m_config->scaleMethod() == ScaleMethod::Depth)
                ? tr("depth (fx / working distance)")
                : tr("homography (live)");
        } else if (m.isMicroscope
                   && m_config->microscopeAnchorPixelsPerMm() > 0.0) {
            m.pixelsPerMm = m_config->microscopeAnchorPixelsPerMm();
            m.scaleSource = tr("config fallback (anchor_pixels_per_mm)");
        }
        if (m.pixelsPerMm > 0.0 && m.camWidth > 0) {
            m.fovWidthMm  = m.camWidth  / m.pixelsPerMm;
            m.fovHeightMm = m.camHeight / m.pixelsPerMm;
        }

        // iBOM components in current FOV
        m.totalComponents = m_ibomProject
            ? static_cast<int>(m_ibomProject->components.size()) : 0;
        if (m_ibomProject && m_homography && m_homography->isValid()
                && m.camWidth > 0 && m.camHeight > 0) {
            // Map the four image corners to PCB space, find the bounding rect
            cv::Point2f tl = m_homography->imageToPcb({0.f, 0.f});
            cv::Point2f tr2 = m_homography->imageToPcb(
                {static_cast<float>(m.camWidth), 0.f});
            cv::Point2f br = m_homography->imageToPcb(
                {static_cast<float>(m.camWidth),
                 static_cast<float>(m.camHeight)});
            cv::Point2f bl = m_homography->imageToPcb(
                {0.f, static_cast<float>(m.camHeight)});
            const float xMin = std::min({tl.x, tr2.x, br.x, bl.x});
            const float xMax = std::max({tl.x, tr2.x, br.x, bl.x});
            const float yMin = std::min({tl.y, tr2.y, br.y, bl.y});
            const float yMax = std::max({tl.y, tr2.y, br.y, bl.y});
            int count = 0;
            for (const auto& c : m_ibomProject->components) {
                const auto& bb = c.bbox;
                // Component overlaps FOV if its bbox intersects the FOV rect
                if (bb.maxX >= xMin && bb.minX <= xMax
                        && bb.maxY >= yMin && bb.minY <= yMax)
                    ++count;
            }
            m.componentsInFov = count;
        }

        auto* dlg = new gui::FovMeasureDialog(m, m_mainWindow.get());
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->exec();
    });

    // ── Dev menu: live calibration monitor (non-modal cockpit) ───────
    connect(m_mainWindow.get(), &gui::MainWindow::calibrationMonitorRequested,
            this, [this]() {
        if (!m_calibMonitor) {
            m_calibMonitor = std::make_unique<gui::CalibrationMonitorDialog>(
                *m_config, m_mainWindow.get());
            // "Capture image" in the monitor drives the normal calibration
            // capture flow (signal → signal → the shared calibHandler).
            connect(m_calibMonitor.get(),
                    &gui::CalibrationMonitorDialog::captureRequested,
                    m_mainWindow.get(), &gui::MainWindow::calibrationRequested);
        }
        pushCalibrationMonitorState();
        m_calibMonitor->show();
        m_calibMonitor->raise();
        m_calibMonitor->activateWindow();
    });

    // ── Dev: verbose logging toggle + on-demand full state dump ──────
    connect(m_mainWindow.get(), &gui::MainWindow::verboseLoggingToggled, this, [this](bool on) {
        utils::Logger::setLevel(on ? spdlog::level::trace : spdlog::level::info);
        m_config->setVerboseLogging(on);
        m_config->save();
        spdlog::info("Verbose debug logging {} — log file: {}",
                     on ? "ENABLED" : "disabled", utils::Logger::logFilePath());
        m_mainWindow->updateStatusMessage(
            on ? tr("Verbose debug logging ON — %1")
                     .arg(QString::fromStdString(utils::Logger::logFilePath()))
               : tr("Verbose debug logging off"));
        if (on) logFullState();
    });
    connect(m_mainWindow.get(), &gui::MainWindow::dumpStateRequested, this, [this]() {
        logFullState();
        m_mainWindow->updateStatusMessage(tr("Full state dumped to the log file"));
    });

    connect(m_mainWindow.get(), &gui::MainWindow::componentReanchorRequested, this, [this]() {
        componentReanchor(/*silent=*/false);
    });

    spdlog::info("Signal/slot connections established, FPS timer started.");
}

// ── iBOM File Loading ──────────────────────────────────────────────

void Application::loadIBomFile(const QString& path)
{
    spdlog::info("Loading iBOM file: {}", path.toStdString());
    m_mainWindow->updateStatusMessage(tr("Loading iBOM file..."));

    auto result = m_ibomParser->parseFile(path.toStdString());
    if (!result) {
        auto errMsg = QString::fromStdString(m_ibomParser->lastError());
        spdlog::error("Failed to parse iBOM file: {}", m_ibomParser->lastError());
        m_mainWindow->updateStatusMessage(tr("Failed to load iBOM: %1").arg(errMsg));
        QMessageBox::warning(m_mainWindow.get(), tr("iBOM Load Error"),
            tr("Failed to parse iBOM file:\n%1").arg(errMsg));
        return;
    }

    m_ibomProject = std::make_shared<IBomProject>(std::move(*result));
    spdlog::info("iBOM loaded: {} components, {} BOM groups",
                 m_ibomProject->components.size(), m_ibomProject->bomGroups.size());


    // Feed to BOM panel
    m_mainWindow->bomPanel()->loadBomData(m_ibomProject->bomGroups, m_ibomProject->components);

    // Feed to minimap
    m_mainWindow->boardMinimap()->setIBomData(*m_ibomProject);

    // Feed the board outline to the live tracker so it can mask ORB
    // detection to the board area (avoids the background — table, cables —
    // outvoting the board's own keypoints when the board is moved by hand
    // under a fixed camera; see TrackingWorker::setBoardPolygon).
    if (m_trackingWorker) {
        auto& bb = m_ibomProject->boardInfo.boardBBox;
        std::vector<cv::Point2f> pcbCorners = {
            {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
            {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
            {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
            {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}
        };
        QMetaObject::invokeMethod(m_trackingWorker, "setBoardPolygon", Qt::QueuedConnection,
            Q_ARG(std::vector<cv::Point2f>, pcbCorners));
    }

    // Remember for the File → Open Recent menu and startup auto-reload.
    m_config->setIBomFilePath(path.toStdString());
    m_config->addRecentIbomFile(path.toStdString());
    refreshRecentFilesMenu();

    // Restore the placed state of a previous inspection of this board so the
    // overlay and BOM panel show progress immediately (full resume happens
    // in startInspection).
    m_placedRefs = loadSavedPlacedRefs();
    if (!m_placedRefs.empty()) {
        for (const auto& ref : m_placedRefs)
            m_mainWindow->bomPanel()->setComponentState(ref, tr("Placed"));
        m_mainWindow->boardMinimap()->setPlacedRefs(m_placedRefs);
        spdlog::info("Restored {} placed components from a previous session",
                     m_placedRefs.size());
    }

    // Restore a previously saved alignment for this exact iBOM file, if any —
    // best-effort: we have no way to know whether the camera/board moved
    // since, so this is offered as a starting point, not a guarantee. Only
    // applies if the live tracking/alignment flow hasn't already set one.
    if (!m_homography->isValid()) {
        const auto& sa = m_config->savedAlignment();
        if (sa.valid && sa.ibomFilePath == path.toStdString()) {
            cv::Mat m(3, 3, CV_64F);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    m.at<double>(r, c) = sa.matrix[r * 3 + c];
            m_homography->setMatrix(m);
            if (m_homography->isValid()) {
                ++m_alignmentEpoch;
                if (sa.pixelsPerMm > 0.0) {
                    m_currentPixelsPerMm = sa.pixelsPerMm;
                    m_basePixelsPerMm    = sa.pixelsPerMm;
                    if (auto* sp = m_mainWindow->statsPanel()) sp->setScale(m_currentPixelsPerMm);
                }
                m_mainWindow->updateStatusMessage(
                    tr("Restored previous alignment for this board (saved %1) — "
                       "re-align if the camera or board has moved.")
                        .arg(QString::fromStdString(sa.timestamp)));
                spdlog::info("Restored saved alignment for '{}' (saved {})",
                             path.toStdString(), sa.timestamp);
            }
        }
    }

    // Auto-compute a basic homography based on board bounding box
    if (!m_homography->isValid()) {
        auto& bb = m_ibomProject->boardInfo.boardBBox;
        float bw = static_cast<float>(bb.maxX - bb.minX);
        float bh = static_cast<float>(bb.maxY - bb.minY);
        if (bw > 0 && bh > 0) {
            float iw = static_cast<float>(m_camera->resolution().width());
            float ih = static_cast<float>(m_camera->resolution().height());
            if (iw <= 0 || ih <= 0) { iw = 1920.0f; ih = 1080.0f; }
            float scale = std::min(iw / bw, ih / bh) * 0.8f;
            float ox = (iw - bw * scale) / 2.0f;
            float oy = (ih - bh * scale) / 2.0f;

            std::vector<cv::Point2f> pcbPts = {
                {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
                {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
                {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
                {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}
            };
            std::vector<cv::Point2f> imgPts = {
                {ox, oy}, {ox + bw * scale, oy},
                {ox + bw * scale, oy + bh * scale}, {ox, oy + bh * scale}
            };
            ++m_alignmentEpoch;
            if (m_homography->compute(pcbPts, imgPts)) {
                spdlog::info("Auto-homography computed: board {:.1f}x{:.1f} → scale {:.2f}", bw, bh, scale);
            }
        }
    }

    m_mainWindow->updateStatusMessage(
        tr("iBOM loaded: %1 components").arg(m_ibomProject->components.size()));

    // Hand the project to the dataset capture worker (Front face in v1 —
    // matches the overlay, which also renders Layer::Front only).
    if (m_datasetCreator) {
        QMetaObject::invokeMethod(m_datasetCreator, "setProject", Qt::QueuedConnection,
            Q_ARG(std::shared_ptr<const ibom::IBomProject>,
                  std::shared_ptr<const IBomProject>(m_ibomProject)),
            Q_ARG(ibom::Layer, Layer::Front));
    }

    emit ibomLoaded(static_cast<int>(m_ibomProject->components.size()));
}

// ── Inspection workflow ────────────────────────────────────────────

void Application::startInspection()
{
    if (!m_ibomProject) {
        QMessageBox::information(m_mainWindow.get(), tr("Inspection"),
            tr("Load an iBOM file first."));
        return;
    }
    m_placedRefs.clear();
    m_pickAndPlace->loadComponents(m_ibomProject->components);

    // Apply user-configured sort method (overrides the default sort done in loadComponents)
    switch (m_config->sortMethod()) {
    case SortMethod::ValueCount:      m_pickAndPlace->sortByValueGroupCount(); break;
    case SortMethod::ValueAlphabetic: m_pickAndPlace->sortByValueGroup();      break;
    case SortMethod::Position:        m_pickAndPlace->sortByPosition();        break;
    case SortMethod::FootprintSize:   m_pickAndPlace->sortByFootprintSize();   break;
    }

    // Resume a previous session of this board, if one was saved (state is
    // written on every "Placed" click). The Reset button starts over.
    m_placedRefs = loadSavedPlacedRefs();
    if (!m_placedRefs.empty()) {
        const int restored = m_pickAndPlace->restorePlaced(m_placedRefs);
        if (restored > 0) {
            m_mainWindow->updateStatusMessage(
                tr("Inspection resumed: %1/%2 already placed")
                    .arg(restored).arg(m_pickAndPlace->totalSteps()));
            return;
        }
        m_placedRefs.clear();  // saved refs match nothing in this board
    }

    // Re-emit current step so InspectionPanel shows the first item of the new order
    if (m_pickAndPlace->totalSteps() > 0)
        m_pickAndPlace->reset();

    m_mainWindow->updateStatusMessage(
        tr("Inspection started: %1 components").arg(m_pickAndPlace->totalSteps()));
}

void Application::onSnapshot()
{
    QImage img = m_mainWindow->cameraView()->captureView();
    if (img.isNull()) {
        m_mainWindow->updateStatusMessage(tr("No frame to snapshot"));
        return;
    }
    QString ref = QString::fromStdString(m_selectedRef);
    int id = m_snapshotHistory->takeSnapshot(img, "inspection", ref);
    if (id > 0)
        m_mainWindow->updateStatusMessage(tr("Snapshot saved (#%1)").arg(id));
}

void Application::onExport(const QString& format)
{
    if (!m_ibomProject) {
        QMessageBox::information(m_mainWindow.get(), tr("Export"),
            tr("Load an iBOM file first."));
        return;
    }

    // Build records from PickAndPlace state, looking up positions from iBOM components.
    std::vector<exports::DataExporter::ComponentRecord> records;
    records.reserve(m_ibomProject->components.size());

    // Index components by reference for fast lookup
    std::map<std::string, const Component*> byRef;
    for (const auto& c : m_ibomProject->components)
        byRef[c.reference] = &c;

    const auto& steps = m_pickAndPlace->steps();
    if (steps.empty()) {
        // No inspection in progress — export all components as "pending"
        for (const auto& c : m_ibomProject->components) {
            exports::DataExporter::ComponentRecord rec;
            rec.reference = c.reference;
            rec.value     = c.value;
            rec.footprint = c.footprint;
            rec.layer     = c.layer;
            rec.status    = "pending";
            rec.posX      = c.position.x;
            rec.posY      = c.position.y;
            rec.rotation  = c.rotation;
            records.push_back(std::move(rec));
        }
    } else {
        for (const auto& step : steps) {
            exports::DataExporter::ComponentRecord rec;
            rec.reference = step.reference;
            rec.value     = step.value;
            rec.footprint = step.footprint;
            rec.layer     = step.layer;
            rec.status    = step.placed ? "placed" : "pending";
            if (auto it = byRef.find(step.reference); it != byRef.end()) {
                rec.posX     = it->second->position.x;
                rec.posY     = it->second->position.y;
                rec.rotation = it->second->rotation;
            }
            records.push_back(std::move(rec));
        }
    }

    m_dataExporter->setRecords(records);

    // Determine extension + filter from format
    QString ext, filter, defaultName;
    if (format == "csv") {
        ext = "csv";  filter = tr("CSV (*.csv)");
        defaultName = "inspection_report.csv";
    } else if (format == "json") {
        ext = "json"; filter = tr("JSON (*.json)");
        defaultName = "inspection_report.json";
    } else if (format == "placement") {
        ext = "pos";  filter = tr("Placement (*.pos)");
        defaultName = "placement.pos";
    } else if (format == "bom") {
        ext = "csv";  filter = tr("BOM CSV (*.csv)");
        defaultName = "bom.csv";
    } else if (format == "defects") {
        ext = "csv";  filter = tr("Defects CSV (*.csv)");
        defaultName = "defects.csv";
    } else if (format == "report-html") {
        ext = "html"; filter = tr("HTML report (*.html)");
        defaultName = "inspection_report.html";
    } else if (format == "report-pdf") {
        ext = "pdf";  filter = tr("PDF report (*.pdf)");
        defaultName = "inspection_report.pdf";
    } else {
        spdlog::warn("Unknown export format: {}", format.toStdString());
        return;
    }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                  + "/MicroscopeIBOM";
    QDir().mkpath(dir);
    QString path = QFileDialog::getSaveFileName(
        m_mainWindow.get(), tr("Export %1").arg(format.toUpper()),
        dir + "/" + defaultName, filter);
    if (path.isEmpty()) return;

    bool ok = false;
    if (format == "csv")            ok = m_dataExporter->exportCSV(path);
    else if (format == "json")      ok = m_dataExporter->exportJSON(path, true);
    else if (format == "placement") ok = m_dataExporter->exportPlacement(path);
    else if (format == "bom")       ok = m_dataExporter->exportBOM(path);
    else if (format == "defects")   ok = m_dataExporter->exportDefectsCSV(path);
    else if (format.startsWith("report-")) {
        exports::ReportGenerator gen;

        exports::ReportGenerator::ReportConfig rc;
        rc.projectName      = QString::fromStdString(m_ibomProject->boardInfo.title);
        rc.boardRevision    = QString::fromStdString(m_ibomProject->boardInfo.revision);
        rc.includeSnapshots = false;  // per-component snapshots not collected yet
        gen.setConfig(rc);

        std::vector<exports::ReportGenerator::InspectionResult> results;
        results.reserve(records.size());
        for (const auto& r : records) {
            exports::ReportGenerator::InspectionResult ir;
            ir.reference = r.reference;
            ir.value     = r.value;
            ir.footprint = r.footprint;
            ir.status    = r.status;
            results.push_back(std::move(ir));
        }
        gen.setResults(results);
        gen.setBoardImage(m_mainWindow->cameraView()->captureView());

        ok = (format == "report-pdf") ? gen.generatePDF(path)
                                      : gen.generateHTML(path);
    }

    if (ok) {
        m_mainWindow->updateStatusMessage(
            tr("Exported %1: %2").arg(format.toUpper(), QFileInfo(path).fileName()));
    } else {
        m_mainWindow->updateStatusMessage(tr("Export failed"));
    }
}

// ── Calibration ────────────────────────────────────────────────────

void Application::runCalibration()
{
    spdlog::info("Running calibration with {} images...", m_calibImages.size());
    m_mainWindow->updateStatusMessage(tr("Running calibration..."));

    double error = m_calibration->calibrate(
        m_calibImages,
        cv::Size(m_config->calibBoardCols(), m_config->calibBoardRows()),
        m_config->calibSquareSize());
    m_calibImages.clear();

    if (error < 0) {
        spdlog::error("Calibration failed");
        m_mainWindow->updateStatusMessage(tr("Calibration failed — checkerboard not detected"));
        QMessageBox::warning(m_mainWindow.get(), tr("Calibration Failed"),
            tr("Could not find checkerboard corners in the captured images.\n"
               "Make sure the %1x%2 checkerboard pattern is fully visible.")
            .arg(m_config->calibBoardCols())
            .arg(m_config->calibBoardRows()));
        return;
    }

    // Save path (utils::dataDir() creates the dir). Needed up-front so a poor
    // calibration can be discarded and the previous good one reloaded.
    const QString dataDir  = QString::fromStdString(utils::dataDir().string());
    const QString calibPath = dataDir + "/calibration.yml";

    // ── Quality gate ────────────────────────────────────────────────
    // calibrate() only returns < 0 when corners aren't found; a successful
    // solve can still be garbage. A usable checkerboard calibration has an RMS
    // reprojection error well under 1 px — anything above ~1.5 px means blurry
    // corners, too steep an angle, or too little pose variation between shots
    // (easy to hit with the microscope's narrow field). Applying/saving it
    // would corrupt the undistort maps and px/mm and overwrite a good
    // calibration, so default to discarding and restore the previous one.
    constexpr double kMaxAcceptableRms = 1.5;  // px
    if (error > kMaxAcceptableRms) {
        spdlog::warn("Calibration RMS {:.2f} px exceeds {:.1f} px threshold — poor quality",
                     error, kMaxAcceptableRms);
        const auto choice = QMessageBox::warning(
            m_mainWindow.get(), tr("Calibration Quality Poor"),
            tr("Reprojection error is %1 px — a good calibration is under 1 px.\n\n"
               "This usually means the checkerboard was out of focus, tilted too "
               "far, or barely moved between the 5 shots (easy to hit with the "
               "microscope's narrow field). Tilt the board to a different angle "
               "for each shot and keep it sharp.\n\n"
               "Keep this calibration anyway?").arg(error, 0, 'f', 2),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice != QMessageBox::Yes) {
            // Restore the previous good calibration if one is on disk, and
            // rebuild its undistort maps; otherwise the camera stays as it was.
            if (QFileInfo::exists(calibPath) && m_calibration->load(calibPath.toStdString())) {
                auto res = m_camera->resolution();
                m_calibration->initUndistortMaps(cv::Size(res.width(), res.height()));
                spdlog::info("Discarded poor calibration; restored previous from {}",
                             calibPath.toStdString());
            } else {
                spdlog::warn("Discarded poor calibration; no previous one to restore");
            }
            m_mainWindow->updateStatusMessage(
                tr("Calibration discarded — RMS %1 px too high (kept previous)")
                .arg(error, 0, 'f', 2));
            return;
        }
    }

    // Initialize undistortion maps for the current resolution
    auto res = m_camera->resolution();
    m_calibration->initUndistortMaps(cv::Size(res.width(), res.height()));

    QDir().mkpath(dataDir);
    m_calibration->save(calibPath.toStdString());

    spdlog::info("Calibration succeeded: error={:.4f}, pixels/mm={:.2f}, saved to {}",
                 error, m_calibration->pixelsPerMm(), calibPath.toStdString());
    m_basePixelsPerMm = m_calibration->pixelsPerMm();
    // A checkerboard calibration measures px/mm through the ENTIRE optical
    // chain (sensor + every reducer/lens already in place), so its result is
    // the true effective scale. Do NOT re-apply opticalMultiplier here — that
    // would double-count the optics (e.g. 11.58 × 0.3 = 3.47 px/mm, which is
    // physically meaningless). The multiplier is only a fallback for an
    // un-calibrated nominal scale (see the settings-apply handler).
    m_currentPixelsPerMm = m_basePixelsPerMm;
    if (auto* sp = m_mainWindow->statsPanel())
        sp->setScale(m_currentPixelsPerMm);
    m_mainWindow->updateStatusMessage(
        tr("Calibration done — error: %1, pixels/mm: %2")
        .arg(error, 0, 'f', 4).arg(m_calibration->pixelsPerMm(), 0, 'f', 1));

    updateCalibrationUI();
}

void Application::pushCalibrationMonitorState()
{
    if (!m_calibMonitor) return;

    const bool isRS = (m_activeBackend == CameraBackend::RealSense);
    const QString backend = isRS ? tr("RealSense D405 (factory-calibrated)")
                                 : tr("V4L2 microscope");

    int w = m_config->cameraWidth();
    int h = m_config->cameraHeight();
    if (m_camera) {
        w = m_camera->resolution().width();
        h = m_camera->resolution().height();
    }

    const bool   calibrated = m_calibration && m_calibration->isCalibrated();
    const double rms  = m_calibration ? m_calibration->rmsError() : 0.0;
    const double ppmm = m_currentPixelsPerMm > 0.0
        ? m_currentPixelsPerMm
        : (m_calibration ? m_calibration->pixelsPerMm() : 0.0);

    QString calibPath = QString::fromStdString(utils::dataDir().string())
                        + "/calibration.yml";
    if (!QFileInfo::exists(calibPath)) calibPath.clear();

    m_calibMonitor->setCalibrationStatus(calibrated, rms, ppmm, backend, w, h, calibPath);
}

// ── Dynamic Scale ──────────────────────────────────────────────────

void Application::updateDynamicScale()
{
    if (!m_homography || !m_homography->isValid() || !m_ibomProject)
        return;

    ScaleMethod method = m_config->scaleMethod();
    if (method == ScaleMethod::None)
        return;

    double newPpmm = 0.0;

    if (method == ScaleMethod::Homography) {
        // Extract scale from current homography by measuring the board width in pixels
        auto& bb = m_ibomProject->boardInfo.boardBBox;
        double pcbW = bb.width(); // mm in PCB coords
        if (pcbW <= 0) return;

        auto tl = m_homography->pcbToImage(
            {static_cast<float>(bb.minX), static_cast<float>(bb.minY)});
        auto tr = m_homography->pcbToImage(
            {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)});
        double pixW = cv::norm(cv::Point2f(tl.x - tr.x, tl.y - tr.y));
        newPpmm = pixW / pcbW;

    } else if (method == ScaleMethod::IBomPads) {
        // Reference pad pair for the scale: two far-apart pads whose known mm
        // distance is compared to their projected pixel distance. Recomputed
        // only when the loaded project changes — the old per-call scan reset
        // its best-distance accumulator per component (so "padB" was only the
        // farthest pad of the LAST component) and rescanned every pad on every
        // homography emission (ERREUR #52).
        if (m_ibomProject.get() != m_scaleRefProject) {
            m_scaleRefProject = m_ibomProject.get();
            m_scaleRefDistMm  = 0.0;

            // Far-apart pair in one O(n) pass: extremes along the two board
            // diagonals (x+y and x−y); keep the farther of the two candidate
            // pairs. Not the exact diameter, but ≥ diagonal/√2 — ample for a
            // stable scale baseline.
            const Pad* minSumP = nullptr; const Pad* maxSumP = nullptr;
            const Pad* minDifP = nullptr; const Pad* maxDifP = nullptr;
            double minSum = 0, maxSum = 0, minDif = 0, maxDif = 0;
            for (const auto& c : m_ibomProject->components) {
                for (const auto& p : c.pads) {
                    const double s = p.position.x + p.position.y;
                    const double d = p.position.x - p.position.y;
                    if (!minSumP || s < minSum) { minSum = s; minSumP = &p; }
                    if (!maxSumP || s > maxSum) { maxSum = s; maxSumP = &p; }
                    if (!minDifP || d < minDif) { minDif = d; minDifP = &p; }
                    if (!maxDifP || d > maxDif) { maxDif = d; maxDifP = &p; }
                }
            }
            const auto padDist = [](const Pad* a, const Pad* b) {
                return (a && b) ? std::hypot(a->position.x - b->position.x,
                                             a->position.y - b->position.y)
                                : 0.0;
            };
            const double dSum = padDist(minSumP, maxSumP);
            const double dDif = padDist(minDifP, maxDifP);
            const Pad* a = (dSum >= dDif) ? minSumP : minDifP;
            const Pad* b = (dSum >= dDif) ? maxSumP : maxDifP;
            const double distMm = std::max(dSum, dDif);
            if (a && b && distMm >= 1.0) {  // < 1 mm apart: unreliable baseline
                // Positions copied by value — the project pointer above is an
                // identity tag only, never dereferenced later.
                m_scaleRefA      = cv::Point2f(static_cast<float>(a->position.x),
                                               static_cast<float>(a->position.y));
                m_scaleRefB      = cv::Point2f(static_cast<float>(b->position.x),
                                               static_cast<float>(b->position.y));
                m_scaleRefDistMm = distMm;
            }
        }
        if (m_scaleRefDistMm < 1.0) return;  // no usable pad pair in this project

        const auto imgA = m_homography->pcbToImage(m_scaleRefA);
        const auto imgB = m_homography->pcbToImage(m_scaleRefB);
        const double pixDist = cv::norm(cv::Point2f(imgA.x - imgB.x, imgA.y - imgB.y));
        newPpmm = pixDist / m_scaleRefDistMm;
    }

    if (newPpmm > 0) {
        m_currentPixelsPerMm = newPpmm;
        if (m_calibration)
            m_calibration->setPixelsPerMm(newPpmm);
        if (auto* sp = m_mainWindow->statsPanel())
            sp->setScale(newPpmm);
    }
}

// ── Screenshot ─────────────────────────────────────────────────────

void Application::takeScreenshot()
{
    QImage capture = m_mainWindow->cameraView()->captureView();
    if (capture.isNull()) {
        m_mainWindow->updateStatusMessage(tr("No frame to capture"));
        return;
    }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    QString filename = QFileDialog::getSaveFileName(
        m_mainWindow.get(), tr("Save Screenshot"), dir + "/microscope_capture.png",
        tr("Images (*.png *.jpg *.bmp)"));

    if (filename.isEmpty()) return;

    if (capture.save(filename)) {
        spdlog::info("Screenshot saved to {}", filename.toStdString());
        m_mainWindow->updateStatusMessage(tr("Screenshot saved: %1").arg(filename));
    } else {
        m_mainWindow->updateStatusMessage(tr("Failed to save screenshot"));
    }
}

} // namespace ibom
