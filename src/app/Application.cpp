#include "Application.h"
#include "Config.h"
#include "gui/MainWindow.h"
#include "gui/CameraView.h"
#include "gui/PointCloudView.h"
#include "gui/ControlPanel.h"
#include "gui/StatsPanel.h"
#include "gui/BomPanel.h"
#include "gui/InspectionWizard.h"
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
#include "features/PickAndPlace.h"
#include "features/Measurement.h"
#include "features/SnapshotHistory.h"
#include "features/DatasetCreator.h"
#include "features/RemoteView.h"
#include "gui/DatasetPanel.h"
#include "gui/BoardMinimap.h"
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
#include <QMediaDevices>
#include <QCameraDevice>
#include <QDesktopServices>
#include <QUrl>
#include <QDateTime>
#include <QFile>
#include <nlohmann/json.hpp>
#include <fstream>
#include <cmath>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>
#include <filesystem>

Q_DECLARE_METATYPE(cv::Mat)
Q_DECLARE_METATYPE(std::shared_ptr<const ibom::IBomProject>)
Q_DECLARE_METATYPE(ibom::Layer)

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

    // Reflect persisted AI settings in the control panel (before initializeAI
    // so the spinner already shows the threshold the detector will use).
    m_mainWindow->controlPanel()->setConfidenceThreshold(m_config->detectionConfidence());

    // AI pipeline — off the GUI thread: first launch with TensorRT compiles
    // the engine (minutes); the app is fully usable without it meanwhile.
    initializeAI();

    // Enumerate cameras and populate ControlPanel.
    // The capture pipeline opens devices by index via OpenCV/V4L2, so we
    // enumerate the same way (CameraCapture::listDevices). QMediaDevices is
    // only queried for friendlier labels: on Jetson-in-Docker its multimedia
    // backend frequently fails to see /dev/video* even when V4L2 capture works
    // perfectly — relying on it alone yields "0 cameras" on a working device.
    {
        refreshCameraDeviceList();
        // Select the configured camera index
        int idx = m_config->cameraIndex();
        if (auto* cp = m_mainWindow->controlPanel()) {
            if (auto* combo = cp->findChild<QComboBox*>()) {
                if (idx >= 0 && idx < combo->count())
                    combo->setCurrentIndex(idx);
            }
        }
    }

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

    // Overlay renderer + homography
    spdlog::info("Creating OverlayRenderer...");
    m_overlayRenderer = std::make_unique<overlay::OverlayRenderer>();
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

    refreshCameraDeviceList();

    spdlog::info("Camera backend switched to {}",
                 backend == CameraBackend::RealSense ? "RealSense" : "V4L2");

    if (wasCapturing) {
        if (!m_camera->start())
            m_mainWindow->updateStatusMessage(tr("Failed to start camera after backend switch"));
    }
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

void Application::refreshCameraDeviceList()
{
    QStringList cameraNames;
#ifdef IBOM_HAVE_REALSENSE
    if (m_config->cameraBackend() == CameraBackend::RealSense) {
        const auto rsDevices = camera::RealSenseCapture::listDevices();
        for (size_t i = 0; i < rsDevices.size(); ++i)
            cameraNames << QString("%1: %2").arg(i).arg(QString::fromStdString(rsDevices[i]));
    } else
#endif
    {
        const auto v4lDevices = camera::CameraCapture::listDevices();
        const auto qtCameras  = QMediaDevices::videoInputs();
        for (size_t i = 0; i < v4lDevices.size(); ++i) {
            const int qi = static_cast<int>(i);
            QString label = (qi < qtCameras.size())
                ? qtCameras[qi].description()
                : QString::fromStdString(v4lDevices[i]);
            cameraNames << QString("%1: %2").arg(qi).arg(label);
        }
    }
    if (cameraNames.isEmpty())
        cameraNames << tr("No camera detected");
    if (m_mainWindow && m_mainWindow->controlPanel())
        m_mainWindow->controlPanel()->setCameraDevices(cameraNames);
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
                this, [this](cv::Mat combined, int /*inliers*/, double /*reprojErr*/) {
            if (!m_liveMode || combined.empty() || !m_homography) return;
            m_homography->setMatrix(combined);
            if (m_overlayRenderer)
                m_overlayRenderer->setHomography(*m_homography);
            updateDynamicScale();
            m_mainWindow->boardMinimap()->update();
        }, Qt::QueuedConnection);

        connect(m_trackingWorker, &overlay::TrackingWorker::trackingError,
                this, [](const QString& msg) {
            spdlog::warn("Tracking worker error: {}", msg.toStdString());
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
            [this, backend](ibom::camera::FrameRef frameRef) {
        if (!frameRef || frameRef->empty()) return;
        const cv::Mat& frame = *frameRef;

        // ── Live tracking: hand the raw frame off to the worker thread ──
        // The worker throttles, downscales and runs ORB without blocking us.
        if (m_liveMode && m_trackingWorker && m_homography && m_homography->isValid()) {
            QMetaObject::invokeMethod(m_trackingWorker, "processFrame", Qt::QueuedConnection,
                Q_ARG(ibom::camera::FrameRef, frameRef));
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

        // Render iBOM overlay if data is loaded and homography is set
        if (m_ibomProject && m_homography && m_homography->isValid()) {
            // Create a transparent overlay the same size as the frame
            QImage overlay(rgb.cols, rgb.rows, QImage::Format_ARGB32_Premultiplied);
            overlay.fill(Qt::transparent);
            QPainter painter(&overlay);
            painter.setRenderHint(QPainter::Antialiasing, true);

            // Draw component pads, silkscreen outlines, and labels
            const bool drawPads = m_config->showPads();
            const bool drawSilk = m_config->showSilkscreen();
            [[maybe_unused]] const bool drawFab = m_config->showFabrication();

            // Resolve per-state base colors from Config (user-customizable via Settings)
            QColor cSelected = QColor(QString::fromStdString(m_config->selectedColorHex()));
            QColor cPlaced   = QColor(QString::fromStdString(m_config->placedColorHex()));
            QColor cNormal   = QColor(QString::fromStdString(m_config->normalColorHex()));
            if (!cSelected.isValid()) cSelected = QColor(0, 229, 255);
            if (!cPlaced.isValid())   cPlaced   = QColor(72, 200, 72);
            if (!cNormal.isValid())   cNormal   = QColor(170, 170, 68);

            const float placedAlphaMul = std::clamp(m_config->placedOpacity(), 0.05f, 1.0f);
            const float selectedSilkW  = std::max(1.0f, m_config->selectedOutlineWidth());

            auto withAlpha = [](QColor c, int a) { c.setAlpha(a); return c; };

            for (const auto& comp : m_ibomProject->components) {
                if (comp.layer != Layer::Front) continue;

                const bool isSelected = (comp.reference == m_selectedRef);
                const bool isPlaced   = !isSelected && m_placedRefs.count(comp.reference) > 0;

                // Per-state pad/silk/label colors and stroke widths
                QColor padColor, silkColor, labelColor;
                qreal silkWidth = 1.0;
                if (isSelected) {
                    padColor   = withAlpha(cSelected, 220);
                    silkColor  = withAlpha(cSelected, 240);
                    labelColor = withAlpha(cSelected, 255);
                    silkWidth  = selectedSilkW;
                } else if (isPlaced) {
                    int a = static_cast<int>(180 * placedAlphaMul);
                    padColor   = withAlpha(cPlaced, a);
                    silkColor  = withAlpha(cPlaced, std::min(255, a + 30));
                    labelColor = withAlpha(cPlaced, std::min(255, a + 60));
                } else {
                    padColor   = withAlpha(cNormal, 180);
                    silkColor  = withAlpha(cNormal, 180);
                    labelColor = ibom::gui::theme::labelNormalColor();
                }

                // ── Draw pads ──
                if (drawPads) {
                for (const auto& pad : comp.pads) {
                    cv::Point2f padCenter(
                        static_cast<float>(pad.position.x),
                        static_cast<float>(pad.position.y));
                    [[maybe_unused]] cv::Point2f imgPad = m_homography->pcbToImage(padCenter);

                    // Transform pad size: map a rect at pad position
                    auto padCorners = m_homography->transformRect(
                        static_cast<float>(pad.position.x - pad.sizeX / 2.0),
                        static_cast<float>(pad.position.y - pad.sizeY / 2.0),
                        static_cast<float>(pad.sizeX),
                        static_cast<float>(pad.sizeY));

                    if (padCorners.size() == 4) {
                        QPolygonF padPoly;
                        for (auto& c : padCorners)
                            padPoly << QPointF(c.x, c.y);

                        painter.setPen(Qt::NoPen);
                        painter.setBrush(padColor);

                        if (pad.shape == Pad::Shape::Circle || pad.shape == Pad::Shape::Oval) {
                            painter.drawEllipse(padPoly.boundingRect());
                        } else {
                            painter.drawPolygon(padPoly);
                        }
                    }
                }
                } // drawPads

                // ── Draw silkscreen / drawings ──
                if (drawSilk) {
                painter.setBrush(Qt::NoBrush);
                for (const auto& seg : comp.drawings) {
                    if (seg.type == DrawingSegment::Type::Line) {
                        cv::Point2f s = m_homography->pcbToImage(
                            cv::Point2f(static_cast<float>(seg.start.x), static_cast<float>(seg.start.y)));
                        cv::Point2f e = m_homography->pcbToImage(
                            cv::Point2f(static_cast<float>(seg.end.x), static_cast<float>(seg.end.y)));
                        painter.setPen(QPen(silkColor, silkWidth));
                        painter.drawLine(QPointF(s.x, s.y), QPointF(e.x, e.y));

                    } else if (seg.type == DrawingSegment::Type::Rect) {
                        auto rc = m_homography->transformRect(
                            static_cast<float>(std::min(seg.start.x, seg.end.x)),
                            static_cast<float>(std::min(seg.start.y, seg.end.y)),
                            static_cast<float>(std::abs(seg.end.x - seg.start.x)),
                            static_cast<float>(std::abs(seg.end.y - seg.start.y)));
                        if (rc.size() == 4) {
                            QPolygonF rp;
                            for (auto& c : rc) rp << QPointF(c.x, c.y);
                            painter.setPen(QPen(silkColor, silkWidth));
                            painter.drawPolygon(rp);
                        }

                    } else if (seg.type == DrawingSegment::Type::Circle) {
                        cv::Point2f c = m_homography->pcbToImage(
                            cv::Point2f(static_cast<float>(seg.start.x), static_cast<float>(seg.start.y)));
                        // Approximate radius in image space
                        cv::Point2f edge = m_homography->pcbToImage(
                            cv::Point2f(static_cast<float>(seg.start.x + seg.radius),
                                        static_cast<float>(seg.start.y)));
                        float r = std::hypot(edge.x - c.x, edge.y - c.y);
                        painter.setPen(QPen(silkColor, silkWidth));
                        painter.drawEllipse(QPointF(c.x, c.y), static_cast<qreal>(r), static_cast<qreal>(r));

                    } else if (seg.type == DrawingSegment::Type::Polygon && !seg.points.empty()) {
                        QPolygonF polyPts;
                        for (const auto& pt : seg.points) {
                            cv::Point2f ip = m_homography->pcbToImage(
                                cv::Point2f(static_cast<float>(pt.x), static_cast<float>(pt.y)));
                            polyPts << QPointF(ip.x, ip.y);
                        }
                        painter.setPen(QPen(silkColor, silkWidth));
                        painter.drawPolygon(polyPts);
                    }
                }
                } // drawSilk

                // ── Draw reference label ──
                if (drawSilk || isSelected) {
                cv::Point2f bboxCenter(
                    static_cast<float>((comp.bbox.minX + comp.bbox.maxX) / 2.0),
                    static_cast<float>((comp.bbox.minY + comp.bbox.maxY) / 2.0));
                cv::Point2f imgPt = m_homography->pcbToImage(bboxCenter);
                painter.setPen(labelColor);
                painter.setFont(QFont("Segoe UI", isSelected ? 9 : 7,
                                       isSelected ? QFont::Bold : QFont::Normal));
                painter.drawText(QPointF(imgPt.x, imgPt.y - 3),
                                 QString::fromStdString(comp.reference));
                } // drawSilk || isSelected
            }
            painter.end();
            m_mainWindow->cameraView()->setOverlayImage(overlay);
        }

        // Draw alignment point picking visual feedback
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
            // Merge with existing overlay or set directly
            if (!m_ibomProject || !m_homography || !m_homography->isValid()) {
                m_mainWindow->cameraView()->setOverlayImage(pickOverlay);
            }
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

            // Throttle to ~3 Hz — distance/scale change slowly on a fixed rig.
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (nowMs - m_lastDepthMs < 300) return;
            m_lastDepthMs = nowMs;

            // Depth fill rate over the whole frame (valid = non-zero).
            const cv::Mat& d = *depth;
            if (auto* sp = m_mainWindow->statsPanel()) {
                const double area = static_cast<double>(d.rows) * d.cols;
                sp->setFillRate(area > 0 ? cv::countNonZero(d) / area : -1.0);
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
        if (m_overlayRenderer) m_overlayRenderer->setShowPads(show);
    });
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::showSilkscreenChanged,
            this, [this](bool show) {
        m_config->setShowSilkscreen(show);
        if (m_overlayRenderer) m_overlayRenderer->setShowLabels(show);
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
        if (m_overlayRenderer)
            m_overlayRenderer->setHighlightedRefs({ref});
        m_mainWindow->boardMinimap()->setSelectedRef(ref);

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
        }
        spdlog::info("Settings applied (camera={}, ORB={}, interval={}ms, RANSAC={:.1f}, downscale={:.2f})",
                     newIdx, m_config->orbKeypoints(), m_config->trackingIntervalMs(),
                     m_config->ransacThreshold(), m_config->trackingDownscale());
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

        // Apply optical multiplier change to pixels-per-mm
        float mult = m_config->opticalMultiplier();
        if (mult > 0 && m_basePixelsPerMm > 0) {
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
            if (m_overlayRenderer)
                m_overlayRenderer->setHighlightedRefs({ref});
            spdlog::info("Navigate to component: {}", ref);
        });
    }

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
        m_alignOnComponents = true;
        m_alignCompStep = 0;
        m_mainWindow->updateStatusMessage(
            tr("Select the FIRST component in the BOM panel (choose 2 components far apart)"));
        spdlog::info("2-component alignment started");
    });

    connect(m_mainWindow->cameraView(), &gui::CameraView::clicked,
            this, [this](QPointF imagePos) {
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

            if (m_homography->compute(pcbCorners, imgCorners)) {
                m_overlayRenderer->setHomography(*m_homography);
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

                if (m_homography->compute(pcbCorners, imgCorners)) {
                    m_overlayRenderer->setHomography(*m_homography);
                    m_basePixelsPerMm = scale;
                    m_currentPixelsPerMm = scale;
                    if (auto* sp = m_mainWindow->statsPanel())
                        sp->setScale(m_currentPixelsPerMm);
                    if (m_trackingWorker)
                        QMetaObject::invokeMethod(m_trackingWorker, "resetReference",
                                                  Qt::QueuedConnection);

                    spdlog::info("2-comp alignment OK: scale={:.2f} px/unit, rot={:.1f}°",
                                 scale, rot * 180.0 / CV_PI);
                    m_mainWindow->updateStatusMessage(
                        tr("Alignment set from %1 + %2 — scale: %3 px/mm")
                        .arg(QString::fromStdString(m_alignRef1))
                        .arg(QString::fromStdString(m_alignRef2))
                        .arg(scale, 0, 'f', 1));
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

            if (m_homography->compute(pcbPts, m_homographyImagePoints)) {
                m_overlayRenderer->setHomography(*m_homography);
                spdlog::info("Manual homography computed successfully (error={:.3f}px)",
                             m_homography->reprojectionError());
                m_mainWindow->updateStatusMessage(
                    tr("Alignment set — reprojection error: %1 px")
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
                m_overlayRenderer->setHomography(*m_homography);
            }
            if (m_trackingWorker)
                QMetaObject::invokeMethod(m_trackingWorker, "resetReference",
                                          Qt::QueuedConnection);
            spdlog::info("Live tracking mode disabled");
            m_mainWindow->updateStatusMessage(tr("Live tracking mode OFF"));
        }
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
        if (m_overlayRenderer)
            m_overlayRenderer->setHighlightedRefs({step.reference});
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
    // anchorRequested(pcbPoint): treat as a 1-point anchor at that PCB position,
    // centering the FOV on the clicked point.  Reuses the same similarity-transform
    // logic that startComponentAnchor() / the CameraView click handler use.
    connect(m_mainWindow->boardMinimap(), &gui::BoardMinimap::anchorRequested,
            this, [this](cv::Point2f pcbPt) {
        if (!m_ibomProject) return;
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
        if (m_homography->compute(pcbCorners, imgCorners)) {
            m_overlayRenderer->setHomography(*m_homography);
            if (m_liveMode) m_baseHomography = m_homography->matrix().clone();
            updateDynamicScale();
            m_mainWindow->updateStatusMessage(tr("Minimap anchor applied"));
            spdlog::info("Minimap anchor: PCB ({:.2f}, {:.2f}) → image center", pcbPt.x, pcbPt.y);
        }
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

    // Feed to overlay renderer
    m_overlayRenderer->setIBomData(*m_ibomProject);

    // Feed to BOM panel
    m_mainWindow->bomPanel()->loadBomData(m_ibomProject->bomGroups, m_ibomProject->components);

    // Feed to minimap
    m_mainWindow->boardMinimap()->setIBomData(*m_ibomProject);

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
            if (m_homography->compute(pcbPts, imgPts)) {
                m_overlayRenderer->setHomography(*m_homography);
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

    // Initialize undistortion maps for the current resolution
    auto res = m_camera->resolution();
    m_calibration->initUndistortMaps(cv::Size(res.width(), res.height()));

    // Save calibration under the unified data dir (utils::dataDir() creates it).
    QString dataDir = QString::fromStdString(utils::dataDir().string());
    auto calibPath = dataDir + "/calibration.yml";
    QDir().mkpath(dataDir);
    m_calibration->save(calibPath.toStdString());

    spdlog::info("Calibration succeeded: error={:.4f}, pixels/mm={:.2f}, saved to {}",
                 error, m_calibration->pixelsPerMm(), calibPath.toStdString());
    m_basePixelsPerMm = m_calibration->pixelsPerMm();
    m_currentPixelsPerMm = m_basePixelsPerMm * m_config->opticalMultiplier();
    if (auto* sp = m_mainWindow->statsPanel())
        sp->setScale(m_currentPixelsPerMm);
    m_mainWindow->updateStatusMessage(
        tr("Calibration done — error: %1, pixels/mm: %2")
        .arg(error, 0, 'f', 4).arg(m_calibration->pixelsPerMm(), 0, 'f', 1));

    updateCalibrationUI();
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
        // Find two pads that are far apart and use their known real distance
        // vs their projected pixel distance
        const auto& comps = m_ibomProject->components;
        if (comps.size() < 2) return;

        // Pick first pad of first and last component as reference points
        const Pad* padA = nullptr;
        const Pad* padB = nullptr;
        for (const auto& c : comps) {
            if (!c.pads.empty()) {
                if (!padA) { padA = &c.pads[0]; continue; }
                // Pick the pad farthest from padA
                double bestDist = 0;
                for (const auto& p : c.pads) {
                    double dx = p.position.x - padA->position.x;
                    double dy = p.position.y - padA->position.y;
                    double d = dx*dx + dy*dy;
                    if (d > bestDist) { bestDist = d; padB = &p; }
                }
            }
        }
        if (!padA || !padB) return;

        double realDist = std::sqrt(
            std::pow(padA->position.x - padB->position.x, 2) +
            std::pow(padA->position.y - padB->position.y, 2));
        if (realDist < 1.0) return; // too close, unreliable

        auto imgA = m_homography->pcbToImage(
            {static_cast<float>(padA->position.x), static_cast<float>(padA->position.y)});
        auto imgB = m_homography->pcbToImage(
            {static_cast<float>(padB->position.x), static_cast<float>(padB->position.y)});
        double pixDist = cv::norm(cv::Point2f(imgA.x - imgB.x, imgA.y - imgB.y));
        newPpmm = pixDist / realDist;
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
