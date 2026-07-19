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
#include "ibom/ProjectDiff.h"
#include "overlay/OverlayRenderer.h"
#include "overlay/Homography.h"
#include "overlay/HeatmapRenderer.h"
#include "overlay/TrackingWorker.h"
#include "overlay/BoardLocator.h"
#include "overlay/ComponentReanchor.h"
#include "overlay/BlobComponentDetector.h"
#include "features/PickAndPlace.h"
#include "features/Measurement.h"
#include "features/SnapshotHistory.h"
#include "features/DatasetCreator.h"
#include "features/RemoteView.h"
#include "features/BoardScanner.h"
#include "overlay/AlignmentMath.h"
#include "features/GoldenDiff.h"
#include "features/DepthInspector.h"
#include "utils/SceneQuality.h"
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
#include <QCryptographicHash>
#include <QDialog>
#include <QVBoxLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QRandomGenerator>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <nlohmann/json.hpp>
#include <fstream>
#include <cmath>
#include <functional>
#include <limits>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/calib3d.hpp>
#include <filesystem>

Q_DECLARE_METATYPE(cv::Mat)
Q_DECLARE_METATYPE(std::shared_ptr<const ibom::IBomProject>)
Q_DECLARE_METATYPE(ibom::Layer)
Q_DECLARE_METATYPE(std::vector<cv::Point2f>)

namespace ibom {

namespace {

/// Annotated re-anchor debug frame (ERREUR #59) : detections in RED, the pad
/// constellation projected under the RESULT pose in GREEN, result message on
/// top — so a field misalignment is diagnosed from what the algorithm actually
/// saw instead of guesses over screenshots. Rolling window of 10 files under
/// dataDir()/debug/. Written UNCONDITIONALLY: it's bounded (10 files) and only
/// fires on an alignment action (user click or the slow periodic re-anchor),
/// so the cost is negligible — and gating it on log verbosity proved too
/// fragile to rely on when a field bug needs the evidence (an earlier version
/// keyed off default_logger()->level(), which the verbose switch doesn't
/// necessarily move). Called from the QtConcurrent worker threads — touches
/// only its arguments and the disk.
void dumpReanchorDebug(const cv::Mat& frame,
                       const std::vector<ai::Detection>& detections,
                       const std::vector<ai::Detection>& padDetections,
                       const IBomProject& project,
                       Layer layer,
                       const overlay::ComponentReanchorResult& res)
{
    try {
        cv::Mat vis = frame.clone();
        for (const auto& d : detections) {
            const cv::Point c(static_cast<int>(d.bbox.x + d.bbox.width * 0.5f),
                              static_cast<int>(d.bbox.y + d.bbox.height * 0.5f));
            const int r = std::max(3, static_cast<int>(d.bbox.width * 0.5f));
            cv::circle(vis, c, r, cv::Scalar(0, 0, 255), 1, cv::LINE_AA);
        }
        // Dedicated pad detections (bare-board path) in MAGENTA, so the dump
        // shows both what MSER saw and what the pad detector saw.
        for (const auto& d : padDetections) {
            const cv::Point c(static_cast<int>(d.bbox.x + d.bbox.width * 0.5f),
                              static_cast<int>(d.bbox.y + d.bbox.height * 0.5f));
            const int r = std::max(2, static_cast<int>(d.bbox.width * 0.5f));
            cv::circle(vis, c, r, cv::Scalar(255, 0, 255), 1, cv::LINE_AA);
        }
        if (res.found && !res.homography.empty()) {
            std::vector<cv::Point2f> pcb;
            for (const auto& comp : project.components) {
                if (comp.layer != layer) continue;
                for (const auto& pad : comp.pads)
                    pcb.push_back({ static_cast<float>(pad.position.x),
                                    static_cast<float>(pad.position.y) });
            }
            if (!pcb.empty()) {
                std::vector<cv::Point2f> img;
                cv::perspectiveTransform(pcb, img, res.homography);
                for (const auto& p : img)
                    cv::drawMarker(vis, p, cv::Scalar(0, 255, 0),
                                   cv::MARKER_CROSS, 7, 1, cv::LINE_AA);
            }
        }
        cv::putText(vis, res.message.substr(0, 90), cv::Point(8, 22),
                    cv::FONT_HERSHEY_SIMPLEX, 0.45,
                    cv::Scalar(0, 255, 255), 1, cv::LINE_AA);
        static std::atomic<int> counter{ 0 };
        namespace fs = std::filesystem;
        const fs::path dir = utils::dataDir() / "debug";
        fs::create_directories(dir);
        const int n = counter++;
        const fs::path file =
            dir / ("reanchor_" + std::to_string(n % 10) + ".jpg");
        if (!cv::imwrite(file.string(), vis))
            throw std::runtime_error("cv::imwrite returned false for " + file.string());
        // First write logged at INFO so the exact path is visible in the
        // console without verbose mode — the field diagnostic needs it.
        if (n == 0)
            spdlog::info("[comp-reanchor] debug frames writing to {}", dir.string());
        else
            spdlog::debug("[comp-reanchor] debug frame written: {}", file.string());
    } catch (const std::exception& e) {
        // WARNING, not debug: a silently-failing dump is exactly what wasted a
        // field session looking for images that were never written.
        spdlog::warn("[comp-reanchor] debug dump FAILED: {}", e.what());
    }
}

/// Keep only detections that fall inside the board region — the board quad
/// projected under a KNOWN pose, expanded by a margin (ERREUR #59): the field
/// scene is ~60 % background (wood, mat, and a large glare reflection), a
/// detection magnet that pollutes the prior-free bootstrap fallback and breeds
/// aliases. Only usable when a pose exists (the periodic re-anchor always has
/// one); the initial Auto-Align has no pose to mask by.
std::vector<ai::Detection> filterToBoardRegion(
    const std::vector<ai::Detection>& dets,
    const IBomProject& project,
    const overlay::Homography& pose,
    double marginFrac)
{
    if (!pose.isValid()) return dets;
    const auto& bb = project.boardInfo.boardBBox;
    // Corners expanded outward by the margin, in PCB mm, then projected to
    // image — so the polygon follows any rotation/perspective of the pose.
    const double mx = (bb.maxX - bb.minX) * marginFrac;
    const double my = (bb.maxY - bb.minY) * marginFrac;
    std::vector<cv::Point2f> quad;
    for (const auto& c : { cv::Point2f(bb.minX - mx, bb.minY - my),
                           cv::Point2f(bb.maxX + mx, bb.minY - my),
                           cv::Point2f(bb.maxX + mx, bb.maxY + my),
                           cv::Point2f(bb.minX - mx, bb.maxY + my) })
        quad.push_back(pose.pcbToImage(c));
    std::vector<ai::Detection> kept;
    kept.reserve(dets.size());
    for (const auto& d : dets) {
        const cv::Point2f ctr(d.bbox.x + d.bbox.width * 0.5f,
                              d.bbox.y + d.bbox.height * 0.5f);
        if (cv::pointPolygonTest(quad, ctr, false) >= 0)
            kept.push_back(d);
    }
    return kept;
}

/// Axis-aligned image ROI covering the board quad under a KNOWN pose, expanded
/// by marginFrac (the "board + 1-2 cm" the user asked to scan), clamped to the
/// frame. Empty when the pose is invalid. Cropping detection to this — rather
/// than detecting full-frame and discarding — matters: detectPadBlobs's Otsu
/// threshold is computed over its input, so a bright background glare in the
/// frame raises the cut and suppresses real pads. Restricting to the board
/// makes the threshold adapt to the board alone (ERREUR #59, field follow-up).
cv::Rect boardRoi(const IBomProject& project, const overlay::Homography& pose,
                  double marginFrac, const cv::Size& imageSize)
{
    if (!pose.isValid()) return {};
    const auto& bb = project.boardInfo.boardBBox;
    const double mx = (bb.maxX - bb.minX) * marginFrac;
    const double my = (bb.maxY - bb.minY) * marginFrac;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (const auto& c : { cv::Point2f(bb.minX - mx, bb.minY - my),
                           cv::Point2f(bb.maxX + mx, bb.minY - my),
                           cv::Point2f(bb.maxX + mx, bb.maxY + my),
                           cv::Point2f(bb.minX - mx, bb.maxY + my) }) {
        const cv::Point2f p = pose.pcbToImage(c);
        minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
    }
    const cv::Rect r(static_cast<int>(std::floor(minx)),
                     static_cast<int>(std::floor(miny)),
                     static_cast<int>(std::ceil(maxx - minx)),
                     static_cast<int>(std::ceil(maxy - miny)));
    return r & cv::Rect(0, 0, imageSize.width, imageSize.height);
}

/// Shift detection bboxes back to full-frame coordinates after detecting on a
/// crop.
void offsetDetections(std::vector<ai::Detection>& dets, const cv::Point2f& off)
{
    for (auto& d : dets) { d.bbox.x += off.x; d.bbox.y += off.y; }
}

} // namespace

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

    // In-flight QtConcurrent detections capture the raw detector pointer —
    // they must finish before m_componentDetector/m_inferenceEngine die below
    // (audit B9). isStarted() is false on a never-dispatched default future,
    // so this never blocks on an idle app; a running detection is bounded to
    // a few seconds.
    if (m_autoAlignFuture.isStarted())
        m_autoAlignFuture.waitForFinished();
    if (m_reanchorFuture.isStarted())
        m_reanchorFuture.waitForFinished();

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

    if (m_scanThread) {
        m_scanThread->quit();
        m_scanThread->wait();
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

    // Announce the re-anchor debug-dump path (ERREUR #59): every Auto-Align /
    // periodic re-anchor drops an annotated frame here, so a field
    // misalignment is diagnosed from what the detector saw, not screenshots.
    spdlog::info("Re-anchor debug frames: {}",
                 (utils::dataDir() / "debug").string());

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

    // Board library (C2): the registry of every board opened so far.
    m_boardLibrary.open(utils::dataDir() / "board_library.json");

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

void Application::setActiveLayer(ibom::Layer layer)
{
    if (layer == m_activeLayer) return;
    m_activeLayer = layer;
    const bool back = (layer == ibom::Layer::Back);
    spdlog::info("Active board side → {}", back ? "BACK" : "FRONT");

    // The board was physically flipped: the current pose is meaningless for
    // the new side. Cancel any picking flow and drop the alignment — but keep
    // the on-disk saved alignment (it belongs to the front view restored at
    // iBOM load). The overlay re-renders via the m_ovSigLayer signature; the
    // ERREUR #46 clear path blanks it until a new pose exists.
    m_pickingHomographyPoints = false;
    m_alignOnComponents = false;
    m_alignCompStep = 0;
    m_alignMulti = false;
    m_alignMultiAwaitClick = false;
    m_alignMultiHaveCorner1 = false;
    m_anchorMode = false;
    setMultiAlignUIState(false);
    if (m_homography) m_homography->reset();
    m_reanchorGate.reset();  // any held silent candidate belongs to the old side
    ++m_alignmentEpoch;
    m_currentPixelsPerMm = 0.0;
    if (auto* sp = m_mainWindow ? m_mainWindow->statsPanel() : nullptr)
        sp->setScale(0.0);

    // Follow with the BOM panel's layer filter (1 = Front, 2 = Back) so the
    // list shows the same side as the overlay.
    if (auto* bp = m_mainWindow ? m_mainWindow->bomPanel() : nullptr)
        bp->setLayerFilterIndex(back ? 2 : 1);

    // The PCB Map draws only the active side's components.
    if (m_mainWindow && m_mainWindow->boardMinimap())
        m_mainWindow->boardMinimap()->setActiveLayer(layer);

    // Dataset capture labels the active side's components.
    if (m_datasetCreator && m_ibomProject) {
        QMetaObject::invokeMethod(m_datasetCreator, "setProject", Qt::QueuedConnection,
            Q_ARG(std::shared_ptr<const ibom::IBomProject>,
                  std::shared_ptr<const IBomProject>(m_ibomProject)),
            Q_ARG(ibom::Layer, m_activeLayer));
    }

    if (m_mainWindow) {
        m_mainWindow->updateStatusMessage(back
            ? tr("Back side active (mirrored view) — re-align the flipped board "
                 "(Auto-Align works on the back too)")
            : tr("Front side active — re-align the board"));
    }
}

void Application::refreshInspectionStats()
{
    // Push the absolute inspection progress to the StatsPanel. Called after
    // every m_placedRefs mutation (place, reset, session restore) and on iBOM
    // load — the panel's increment API alone can't represent restore/reset,
    // which is why "Inspection Progress" stayed at 0% forever (ERREUR #40).
    auto* sp = m_mainWindow ? m_mainWindow->statsPanel() : nullptr;
    if (!sp) return;
    sp->setTotalComponents(m_ibomProject
        ? static_cast<int>(m_ibomProject->components.size()) : 0);
    sp->setPlacedCount(static_cast<int>(m_placedRefs.size()));
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

    // Keep the board library's progress column current (C2) — cheap, the
    // registry is a small JSON.
    updateBoardLibraryEntry();
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

    // Access token (roadmap §3.2): the server listens on all interfaces, so
    // gate the stream behind a token. Generated once, persisted in config —
    // stable across restarts so a bookmarked viewer URL keeps working.
    if (m_config->remoteViewToken().empty()) {
        static const char kAlphabet[] =
            "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
        QString tok;
        auto* rng = QRandomGenerator::system();
        for (int i = 0; i < 10; ++i)
            tok.append(QLatin1Char(kAlphabet[rng->bounded(
                static_cast<quint32>(sizeof(kAlphabet) - 1))]));
        m_config->setRemoteViewToken(tok.toStdString());
        m_config->save();
    }
    m_remoteView->setToken(QString::fromStdString(m_config->remoteViewToken()));

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
                     "(open with ?host=<jetson-ip>&token={} from another machine)",
                     port, viewerPath.string(), m_config->remoteViewToken());
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

    // Board-scan worker (A1) on its own thread — the per-frame
    // warpPerspective into the mosaic canvas must not block the GUI.
    m_scanThread = new QThread(this);
    m_scanThread->setObjectName("BoardScan");
    m_boardScanner = new features::BoardScanner();
    m_boardScanner->moveToThread(m_scanThread);
    connect(m_scanThread, &QThread::finished,
            m_boardScanner, &QObject::deleteLater);
    m_scanThread->start();

    // Periodic re-anchor timer (plan B): during live tracking, correct
    // accumulated drift via the component path — it matches detections
    // (trained model OR model-free blobs) to iBOM positions and works when the
    // board fills the frame. The timeout re-checks all conditions;
    // updateReanchorTimer() starts/stops it.
    m_reanchorTimer = new QTimer(this);
    m_reanchorTimer->setObjectName("ReanchorTimer");
    connect(m_reanchorTimer, &QTimer::timeout, this, [this]() {
        if (!m_config->reanchorEnabled() || !m_liveMode || !m_ibomProject || m_autoAligning)
            return;
        // Periodic drift correction needs a repeatable absolute pose. The blob
        // pose used to wander ~30 px tick to tick (13-63 px field log
        // 2026-07-03) — not because of the detections, but because the 8-DOF
        // findHomography fit noise-fitted its perspective terms, levering the
        // board corners the drift gate measures. Blob poses are now fitted as
        // a 4-DOF similarity on stable region centroids
        // (docs/BLOB_REANCHOR_JITTER_ANALYSE.md): tick-to-tick corner jitter
        // sits under the 12 px drift gate, so the periodic tick skips instead
        // of yanking the overlay — the component path is safe again with or
        // without a trained model. Belt on top: a correction only applies
        // after two consecutive concordant estimates (ReanchorGate, unit-
        // tested), so one aberrant tick can't move anything either.
        componentReanchor(/*silent=*/true);
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

    // The cached depth data belongs to the previous backend — a stale D405
    // frame must not feed Depth-Check / Auto-Align once the microscope is
    // active (audit B6; same family as ERREUR #25, which only blanked the
    // displayed stats).
    m_lastDepthFrame.reset();
    m_lastDepthDistanceMm = 0.0;

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
    // just "what we had last time", not a correctness guarantee. Front only:
    // a back-side pose saved here would be restored as-if-front at the next
    // load (restore is likewise gated on the front side).
    if (m_activeLayer == ibom::Layer::Front &&
        m_homography && m_homography->isValid() && m_config) {
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
        if (c.layer != m_activeLayer) continue;  // matches the rendered overlay
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
        // Back side: the camera sees the layout mirrored, which a similarity
        // cannot represent — alignmath fits in the view frame (pcb x negated,
        // vx) so the resulting 3×3, expressed against RAW pcb coords, carries
        // the mirror (negative determinant). n ≥ 3 needs no special casing:
        // affine and homography fits represent a mirror natively.
        const double vx = (m_activeLayer == ibom::Layer::Back) ? -1.0 : 1.0;
        H = overlay::alignmath::similarityFromTwoPoints(pcb[0], pcb[1],
                                                        img[0], img[1], vx);
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
            // Drift gate in physical units (roadmap §1.3): a fixed 12 px means
            // 0.05 mm under the microscope (hyper-sensitive) but ~3 mm in a
            // wide view. 2.5 mm ≈ the old 12 px at the D405's ~4.4 px/mm;
            // px clamps keep it sane when the scale estimate is off.
            constexpr double kReanchorMinShiftMm = 2.5;
            const double ppm = m_currentPixelsPerMm > 0.0
                ? m_currentPixelsPerMm : m_basePixelsPerMm;
            const double reanchorMinShiftPx = ppm > 0.0
                ? std::clamp(kReanchorMinShiftMm * ppm, 6.0, 48.0)
                : 12.0;   // scale unknown → legacy px default
            if (maxShift < reanchorMinShiftPx) {
                spdlog::debug("Periodic re-anchor: pose within {:.0f}px (shift {:.1f}), skipping",
                              reanchorMinShiftPx, maxShift);
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
    const ibom::Layer activeLayer = m_activeLayer;  // snapshot for the worker
    m_autoAlignFuture = QtConcurrent::run([colorCopy, depthCopy, project,
                                           expectedPixelsPerMm, detector,
                                           activeLayer]() {
        // Detection-first (docs/AUTO_ALIGN_V2_PLAN.md): register the detected-
        // component constellation against the iBOM layout — needs no visible
        // board outline, so it works exactly where the geometric path
        // structurally fails (board filling the frame, cluttered/glossy
        // background). Detections come from the trained model when loaded,
        // else from the model-free blob detector (classic CV) — so component-
        // based alignment works even with an empty models/. BoardLocator stays
        // as the last resort.
        std::vector<ai::Detection> detections;
        const char* detSrc = "model";
        if (detector) {
            detections = detector->detect(colorCopy);
        } else {
            // High cap: the per-constellation subsets below re-cap. With the
            // old 300 cap, large background junk (wood grain, shadows) evicted
            // the small pad blobs it outranked by area (ERREUR #58).
            detections = overlay::detectComponentBlobs(colorCopy, expectedPixelsPerMm,
                                                       /*maxDetections=*/600);
            detSrc = "blobs";
        }
        if (!detections.empty()) {
            // Blob centers are noisier than model detections: fit a 4-DOF
            // similarity so the pose stays repeatable at the board corners
            // (docs/BLOB_REANCHOR_JITTER_ANALYSE.md — a full homography
            // noise-fits its perspective terms there).
            overlay::ComponentReanchor::Params rp;
            rp.fitSimilarity = (detector == nullptr);
            // Component-body attempt: the 300 largest (historic behaviour —
            // detectComponentBlobs returns area-descending).
            std::vector<ai::Detection> compDets = detections;
            if (!detector && compDets.size() > 300) compDets.resize(300);
            auto boot = overlay::ComponentReanchor::bootstrap(
                compDets, *project, activeLayer, expectedPixelsPerMm, rp);
            std::vector<ai::Detection> padDets;
            if (detector == nullptr) {
                // Model-free blobs on a bare/partially populated board are
                // the shiny PADS, not component bodies — matching them against
                // component centers is a constellation coincidence that
                // aliases (ERREUR #57: 40/117 inliers applied as score 1.00).
                // The pad attempt uses the DEDICATED pad detector (bright-on-
                // mask top-hat, ERREUR #59) — MSER in a dim scene yields
                // pad-sized noise blobs while the actual pads go undetected.
                padDets = overlay::detectPadBlobs(colorCopy, expectedPixelsPerMm);
                overlay::ComponentReanchor::Params rpPads = rp;
                rpPads.constellation =
                    overlay::ComponentReanchor::Constellation::Pads;
                const auto bootPads = overlay::ComponentReanchor::bootstrap(
                    padDets, *project, activeLayer, expectedPixelsPerMm, rpPads);
                const auto ratio = [](const overlay::ComponentReanchorResult& r) {
                    return r.matches > 0
                        ? static_cast<double>(r.inliers) / r.matches : 0.0;
                };
                if (bootPads.found && (!boot.found || ratio(bootPads) > ratio(boot)))
                    boot = bootPads;
            }
            // Full detection sets on purpose: junk must be VISIBLE in the dump.
            dumpReanchorDebug(colorCopy, detections, padDets, *project,
                              activeLayer, boot);
            if (boot.found) {
                overlay::BoardLocateResult blr;
                blr.found  = true;
                blr.method = std::string("components(") + detSrc + ")";
                // Honest confidence for the trust (0.45) and reanchor (0.5)
                // gates: the inlier fraction of the gated matches. The old
                // mapping (0.4 + inliers/30) saturated at 1.00 from 18 inliers
                // no matter how many matches disagreed — the ERREUR #57
                // aliased lock scored a perfect 1.00.
                blr.score   = boot.matches > 0
                    ? std::min(1.0, static_cast<double>(boot.inliers) / boot.matches)
                    : 0.0;
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
            spdlog::info("Auto-Align: component bootstrap ({}) didn't lock ({}), "
                         "falling back to board outline", detSrc, boot.message);
        }
        auto blr = overlay::BoardLocator::locate(colorCopy, depthCopy, *project,
                                                 expectedPixelsPerMm, activeLayer);
        if (blr.found && !detector && blr.imageCorners.size() == 4) {
            // Contour + pads fusion (field insight, suite 142): the located
            // outline reliably fixes position, scale and the board SURFACE —
            // mask detections to it (+20 % margin ≈ 1-2 cm) — while the PADS
            // decide the orientation the edge-agreement score can't (weakly
            // discriminative on a busy PCB). Four discrete hypotheses with a
            // known surface beat the open-ended prior-free bootstrap.
            const auto& bb = project->boardInfo.boardBBox;
            const std::vector<cv::Point2f> pcb = {
                {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
                {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
                {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
                {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}
            };
            overlay::Homography quadPose;
            quadPose.setMatrix(cv::getPerspectiveTransform(pcb, blr.imageCorners));
            // Scan only the board + margin (the located surface + ~1-2 cm) so
            // the pad detector's Otsu adapts to the board, not the background.
            const cv::Rect roi = boardRoi(*project, quadPose, 0.2, colorCopy.size());
            const cv::Mat detImg = roi.area() > 0 ? colorCopy(roi) : colorCopy;
            std::vector<ai::Detection> padsOnBoard =
                overlay::detectPadBlobs(detImg, expectedPixelsPerMm);
            offsetDetections(padsOnBoard,
                             { static_cast<float>(roi.x), static_cast<float>(roi.y) });
            padsOnBoard = filterToBoardRegion(padsOnBoard, *project, quadPose, 0.2);
            overlay::ComponentReanchor::Params rpo;
            rpo.fitSimilarity = true;
            rpo.constellation = overlay::ComponentReanchor::Constellation::Pads;
            rpo.matchGateMm   = 5.0;
            rpo.scalePxPerMm  = expectedPixelsPerMm;
            const auto vote = overlay::ComponentReanchor::estimateOrientations(
                padsOnBoard, *project, blr.imageCorners, activeLayer, rpo);
            if (vote.found) {
                blr.method  += "+pads";
                // Honest confidence: pad-support ratio of the winning rotation.
                blr.score    = vote.matches > 0
                    ? std::min(1.0, static_cast<double>(vote.inliers) / vote.matches)
                    : 0.0;
                blr.message  = vote.message;
                cv::perspectiveTransform(pcb, blr.imageCorners, vote.homography);
                dumpReanchorDebug(colorCopy, {}, padsOnBoard, *project,
                                  activeLayer, vote);
            }
        }
        return blr;
    });
    watcher->setFuture(m_autoAlignFuture);
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
        m_lostRecoveryArmed    = false;  // recovered (or live mode ended) — chain stops
        m_lostRecoveryAttempts = 0;
        return;
    }

    if (!m_autoAligning) {
        ++m_lostRecoveryAttempts;
        // componentReanchor() registers a component constellation against the
        // iBOM — from the trained model when loaded, else the model-free blob
        // detector (classic CV). Either way it needs no visible board outline,
        // so it can recover on the board-fills-the-frame / coplanar view where
        // BoardLocator structurally can't (ERREUR #41/#54). It internally
        // bootstraps (no current pose required).
        const bool viaModel = componentDetector() != nullptr;
        spdlog::info("Tracking LOST — auto re-anchor attempt #{} via {} component detection",
                     m_lostRecoveryAttempts, viaModel ? "AI-model" : "blob");
        componentReanchor(/*silent=*/true);

        // If it keeps failing (hard scene: bare/sparse board, blobs can't lock),
        // tell the user once and slow down so the log/CPU aren't hammered.
        if (m_lostRecoveryAttempts == 6) {
            m_mainWindow->updateStatusMessage(tr(
                "Tracking still lost — component re-anchor can't lock on this view. "
                "Align manually (4-corner / Multi-Comp)%1.")
                .arg(viaModel ? QString() : tr(", or load an AI model for better detection")));
            spdlog::warn("Tracking LOST — component re-anchor still failing after 6 tries; "
                         "backing off to 15 s. Manual alignment recommended.");
        }
    }

    // State-change signals only fire on transitions; a persistent loss needs
    // polling. Cadence backs off after several failures so nothing is hammered.
    const int delayMs = (m_lostRecoveryAttempts < 6) ? 3000 : 15000;
    QTimer::singleShot(delayMs, this, &Application::attemptLostRecovery);
}

void Application::componentReanchor(bool silent)
{
    if (m_autoAligning) return;  // shares the alignment guard with autoAlignBoard
    // No detector requirement anymore: without a trained model we detect
    // component blobs with classic CV (BlobComponentDetector) and bootstrap
    // on those. A model is better, but not mandatory.
    auto* detector = componentDetector();
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
            // Drift gate + two-tick confirmation + Lost bypass live in
            // ReanchorGate (unit-tested — the rules come from suites 123/
            // 126/127, see docs/BLOB_REANCHOR_JITTER_ANALYSE.md).
            std::vector<cv::Point2f> curImg;
            curImg.reserve(4);
            for (const auto& p : pcbCorners)
                curImg.push_back(m_homography->pcbToImage(p));
            overlay::ReanchorGate::Params gp;
            gp.pendingMaxAgeMs = 3 * std::max<std::int64_t>(500,
                static_cast<std::int64_t>(m_config->reanchorIntervalS() * 1000.0));
            // 12 mm cap (clamped 40-250 px) on silent corrections while
            // tracking is healthy: beyond that the "correction" is an aliased
            // estimate (repetitive pad lattice) or a real board move — and
            // board moves recover via Lost. Without it a repeatable alias
            // passes the two-tick confirmation and yanks a perfect pose
            // sideways (ERREUR #58, the field « clack », shift 185 px).
            gp.maxShiftPx = m_currentPixelsPerMm > 0.0
                ? std::clamp(12.0 * m_currentPixelsPerMm, 40.0, 250.0)
                : 150.0;
            // Drift gate + confirmation tolerance in physical units too
            // (roadmap §1.3, same rationale as the BoardLocator path): the
            // px defaults (12/8) only hold near the D405's ~4.4 px/mm.
            {
                const double ppm = m_currentPixelsPerMm > 0.0
                    ? m_currentPixelsPerMm : m_basePixelsPerMm;
                if (ppm > 0.0) {
                    gp.minShiftPx   = std::clamp(2.5 * ppm, 6.0, 48.0);
                    gp.confirmTolPx = std::clamp(1.5 * ppm, 4.0, 32.0);
                }
            }
            const bool trackingLost = m_lastTrackingState ==
                static_cast<int>(overlay::TrackingWorker::State::Lost);
            const auto gate = m_reanchorGate.evaluate(
                newImg, curImg, trackingLost,
                QDateTime::currentMSecsSinceEpoch(), gp);
            switch (gate.action) {
            case overlay::ReanchorGate::Action::Skip:
                spdlog::debug("Component re-anchor: {} (shift {:.1f}px), skipping",
                              gate.reason, gate.maxShiftPx);
                return;
            case overlay::ReanchorGate::Action::Hold:
                spdlog::info("Component re-anchor: HELD (shift {:.1f}px, {}) — {}",
                             gate.maxShiftPx, result.message, gate.reason);
                return;
            case overlay::ReanchorGate::Action::Apply:
                spdlog::info("Component re-anchor: correcting drift "
                             "(shift {:.1f}px, {} — {})",
                             gate.maxShiftPx, result.message, gate.reason);
                break;
            }
        }

        // Applying a pose (silent confirmed, interactive, or Lost recovery)
        // supersedes any held confirmation candidate.
        m_reanchorGate.reset();
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

    const ibom::Layer activeLayer = m_activeLayer;  // snapshot for the worker
    m_reanchorFuture = QtConcurrent::run([detector, colorCopy, project, priorPose,
                                          scalePrior, activeLayer]() {
        // Scan ONLY the board + margin, not the whole frame (field follow-up):
        // the periodic re-anchor always has a prior pose, so crop detection to
        // the board ROI up front. This isn't just cosmetic — detectPadBlobs's
        // Otsu adapts to its input, so cropping out the wood/mat/glare that
        // fills ~60 % of the frame keeps the threshold on the board and stops
        // the glare from suppressing real pads. Detections are offset back to
        // full-frame coords so the pose and the debug dump stay in frame space.
        const cv::Rect roi = boardRoi(*project, priorPose, 0.25, colorCopy.size());
        const cv::Mat detImg = roi.area() > 0 ? colorCopy(roi) : colorCopy;
        const cv::Point2f roiOff(static_cast<float>(roi.x), static_cast<float>(roi.y));

        std::vector<ai::Detection> detections =
            detector ? detector->detect(colorCopy)
                     : overlay::detectComponentBlobs(detImg, scalePrior,
                                                     /*maxDetections=*/600);
        std::vector<ai::Detection> padDets;
        if (!detector) {
            // Pad attempt: DEDICATED bright-on-mask pad detector (ERREUR #59)
            // — physically grounded, holds up in dim scenes where MSER yields
            // pad-sized noise blobs while the actual pads go undetected.
            padDets = overlay::detectPadBlobs(detImg, scalePrior);
            offsetDetections(detections, roiOff);
            offsetDetections(padDets, roiOff);
        }
        std::vector<ai::Detection> compDets = detections;
        // Final polygon trim: the ROI is the axis-aligned box of a possibly
        // rotated quad, so its corners spill past the board — drop those.
        if (priorPose.isValid()) {
            compDets = filterToBoardRegion(compDets, *project, priorPose, 0.25);
            padDets  = filterToBoardRegion(padDets,  *project, priorPose, 0.25);
        }
        if (!detector && compDets.size() > 300) compDets.resize(300);
        // Blob centers are noisier than model detections: fit a 4-DOF
        // similarity so the pose is repeatable at the board corners — the
        // periodic drift gate measures there, and the 8-DOF fit's noise-fitted
        // perspective terms are what shook the overlay 13-63 px per tick
        // (docs/BLOB_REANCHOR_JITTER_ANALYSE.md).
        overlay::ComponentReanchor::Params rp;
        rp.fitSimilarity = (detector == nullptr);
        // Physical matching gate (ERREUR #57 / INVESTIGATION_360 §1.1): 5 mm
        // around each predicted position instead of a fixed 60 px — which is
        // 13.6 mm at a D405 wide view, wide enough to gate almost anything
        // onto something.
        rp.matchGateMm  = 5.0;
        rp.scalePxPerMm = scalePrior;
        overlay::ComponentReanchor::Params rpPads = rp;
        rpPads.constellation = overlay::ComponentReanchor::Constellation::Pads;
        const auto ratio = [](const overlay::ComponentReanchorResult& r) {
            return r.matches > 0 ? static_cast<double>(r.inliers) / r.matches : 0.0;
        };
        // Prior-based correction first (cheap, precise when the pose is only
        // drifting); with model-free blobs, try BOTH constellations — bodies
        // on a populated board, pads on a bare one (ERREUR #57) — and keep
        // the better-supported fit. Global bootstrap when there is no usable
        // prior — no pose yet, or a pose so stale (board moved/picked up)
        // that nothing falls inside the matching radius anymore.
        auto res = overlay::ComponentReanchor::estimate(
            compDets, *project, priorPose, activeLayer, {}, rp);
        if (!detector) {
            const auto resPads = overlay::ComponentReanchor::estimate(
                padDets, *project, priorPose, activeLayer, {}, rpPads);
            if (resPads.found && (!res.found || ratio(resPads) > ratio(res)))
                res = resPads;
        }
        if (!res.found) {
            res = overlay::ComponentReanchor::bootstrap(
                compDets, *project, activeLayer, scalePrior, rp);
            if (!detector) {
                const auto bootPads = overlay::ComponentReanchor::bootstrap(
                    padDets, *project, activeLayer, scalePrior, rpPads);
                if (bootPads.found && (!res.found || ratio(bootPads) > ratio(res)))
                    res = bootPads;
            }
        }
        // Full detection sets on purpose: junk must be VISIBLE in the dump.
        dumpReanchorDebug(colorCopy, detections, padDets, *project,
                          activeLayer, res);
        return res;
    });
    watcher->setFuture(m_reanchorFuture);
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
            // Latest quality, read by the board-scan frame gate (A1): only
            // feed the mosaic while the pose is healthy.
            m_lastTrackInliers   = inliers;
            m_lastTrackReprojErr = reprojErr;
            // FOV colour (tracking health) + inspection-coverage trail, then
            // repaint. accumulateCoverage() is throttled internally and works
            // even while the map is hidden behind another dock tab.
            m_mainWindow->boardMinimap()->setTrackingQuality(inliers, reprojErr);
            m_mainWindow->boardMinimap()->accumulateCoverage();
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
            // FOV rectangle turns red on the PCB Map while tracking is lost.
            m_mainWindow->boardMinimap()->setTrackingLost(
                static_cast<S>(state) == S::Lost);
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
                        m_lostRecoveryArmed    = true;
                        m_lostRecoveryAttempts = 0;
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

        // ── Board scan (A1): feed the mosaic worker under a healthy pose ──
        // Gate: valid homography, and when live tracking runs, decent quality
        // (same thresholds as the minimap health colour) — a drifting pose
        // must not get printed into the mosaic. Throttled to ~5 Hz; the
        // worker warps on its own thread.
        if (m_scanActive && m_boardScanner && m_homography && m_homography->isValid()) {
            const bool healthy = !m_liveMode
                || (m_lastTrackInliers >= 15 && m_lastTrackReprojErr <= 6.0);
            const qint64 scanNowMs = QDateTime::currentMSecsSinceEpoch();
            if (healthy && scanNowMs - m_lastScanForwardMs >= 200) {
                m_lastScanForwardMs = scanNowMs;
                QMetaObject::invokeMethod(m_boardScanner, "processFrame",
                    Qt::QueuedConnection,
                    Q_ARG(ibom::camera::FrameRef, frameRef),
                    Q_ARG(cv::Mat, m_homography->matrix().clone()));
            }
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

            // ── Scene advisor (D1): exposure / glare / blur, ≤ ~1 Hz ──
            // Reuses the 0.25× frame. Advisory only: a status-bar banner
            // raised after 3 consecutive bad analyses, cleared after 3 good
            // ones — one reflection glint or one shaky frame stays silent.
            if (nowMs - m_lastSceneMs >= 700) {
                m_lastSceneMs = nowMs;
                std::vector<cv::Point> roi;
                if (m_ibomProject && m_homography && m_homography->isValid()) {
                    const auto& bb = m_ibomProject->boardInfo.boardBBox;
                    const cv::Point2f pcbCorners[4] = {
                        {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
                        {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
                        {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
                        {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}};
                    for (const auto& c : pcbCorners) {
                        const cv::Point2f ip = m_homography->pcbToImage(c);
                        roi.emplace_back(static_cast<int>(ip.x * 0.25f),
                                         static_cast<int>(ip.y * 0.25f));
                    }
                }
                utils::SceneQualityParams sp;
                // Half the dataset gate: warn on "clearly defocused", not on
                // "not quite dataset-grade".
                sp.blurSharpness = m_config->datasetMinSharpness() * 0.5;
                const auto rep = utils::analyzeScene(small, roi, sp);

                if (rep.anyIssue()) {
                    m_sceneBadStreak++;
                    m_sceneGoodStreak = 0;
                } else {
                    m_sceneGoodStreak++;
                    m_sceneBadStreak = 0;
                }
                if (m_sceneBadStreak >= 3) {
                    QStringList issues;
                    if (rep.glare)
                        issues << tr("glare on the board — move the light");
                    if (rep.dark)
                        issues << tr("scene too dark — Auto-Align degraded");
                    if (rep.blurry)
                        issues << tr("image out of focus");
                    const QString msg = issues.join(QStringLiteral(" · "));
                    if (!m_sceneWarnActive)
                        spdlog::info("[scene] advisor raised: {}", msg.toStdString());
                    m_sceneWarnActive = true;
                    m_mainWindow->setSceneWarning(msg);
                } else if (m_sceneWarnActive && m_sceneGoodStreak >= 3) {
                    m_sceneWarnActive = false;
                    m_mainWindow->setSceneWarning(QString());
                    spdlog::info("[scene] advisor cleared");
                }
            }
        }

        // Display path — ZERO-COPY (INVESTIGATION_360 §3.1): QImage renders
        // BGR natively (Format_BGR888), so the old per-frame BGR→RGB cvtColor
        // AND the deep QImage::copy() (2 × ~6 MB per 1080p frame, every
        // frame, on the GUI thread) are both gone. The wrap keeps the pixel
        // buffer alive through the QImage's cleanup hook: the immutable
        // FrameRef directly, or the fresh undistort output (remap allocates a
        // new Mat — unavoidable, and skipped for RealSense: librealsense
        // already applies factory calibration).
        QImage display;
        if (m_calibration && m_calibration->isCalibrated()
            && backend != CameraBackend::RealSense) {
            display = utils::ImageUtils::wrapMatOwned(m_calibration->undistort(frame));
        } else {
            display = utils::ImageUtils::wrapMatShared(frameRef);
        }
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

        // Mirror to remote browser clients — the same composition as the
        // local view (camera + warped iBOM overlay), via captureView()
        // (roadmap §3.2: the raw camera image was half the value). That
        // repaints on the GUI thread, so throttle here; RemoteView's own
        // timer only caps the *network* rate, not the composition cost.
        if (m_remoteView && m_remoteView->isRunning() && m_remoteView->clientCount() > 0) {
            constexpr qint64 kRemoteCompositeIntervalMs = 100;   // ~10 fps
            if (!m_remotePushTimer.isValid()
                || m_remotePushTimer.elapsed() >= kRemoteCompositeIntervalMs) {
                m_remotePushTimer.restart();
                m_remoteView->pushFrame(m_mainWindow->cameraView()->captureView());
            }
        }

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
                || m_ibomProject.get() != m_ovSigProject
                || m_activeLayer != m_ovSigLayer
                || m_showHeatmap != m_ovSigHeatmap
                || m_heatmapRev != m_ovSigHeatRev
                || m_revDiffRev != m_ovSigDiffRev;

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
                in.activeLayer = m_activeLayer;
                // C1 V2 — rework coloring from the last revision compare.
                in.diffMarks = m_revDiffMarks;
                in.diffAdds  = (m_activeLayer == ibom::Layer::Front)
                                   ? m_revDiffAddsFront : m_revDiffAddsBack;

                const auto t0 = std::chrono::steady_clock::now();
                overlay::BoardOverlay bo = overlay::OverlayRenderer::renderBoardSpace(in);
                const double renderMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();

                // Defect heatmap (A2, golden diff): composited into the
                // board-space buffer in PCB coords — pcbToBuffer carries the
                // Back-face mirror, so it lands correctly on either side.
                if (m_showHeatmap && m_heatmapRenderer
                    && m_heatmapRenderer->totalDefects() > 0) {
                    const QImage heat = m_heatmapRenderer->renderArgb(0.6f);
                    if (!heat.isNull()) {
                        const auto& bb = m_ibomProject->boardInfo.boardBBox;
                        QPainter hp(&bo.image);
                        hp.setRenderHint(QPainter::SmoothPixmapTransform, true);
                        hp.setTransform(bo.pcbToBuffer);
                        hp.drawImage(QRectF(bb.minX, bb.minY,
                                            bb.width(), bb.height()), heat);
                    }
                }

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
                m_ovSigLayer         = m_activeLayer;
                m_ovSigHeatmap       = m_showHeatmap;
                m_ovSigHeatRev       = m_heatmapRev;
                m_ovSigDiffRev       = m_revDiffRev;
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
            QImage pickOverlay(frame.cols, frame.rows, QImage::Format_ARGB32_Premultiplied);
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
    // Named so the minimap's Ctrl+click (componentPicked) can share it:
    // BomPanel only emits componentSelected on a real cell click, so a
    // programmatic highlightComponent() would not re-enter this handler.
    auto onComponentSelected = [this](const std::string& ref) {
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
    };
    connect(m_mainWindow->bomPanel(), &gui::BomPanel::componentSelected,
            this, onComponentSelected);

    // Ctrl+click on the PCB Map picks the component under the cursor — same
    // behaviour as clicking its BOM row, plus the row scrolls into view.
    connect(m_mainWindow->boardMinimap(), &gui::BoardMinimap::componentPicked,
            this, [this, onComponentSelected](const std::string& ref) {
        if (auto* bp = m_mainWindow->bomPanel()) bp->highlightComponent(ref);
        onComponentSelected(ref);
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
        // Sync the ControlPanel device combo to the new camera index — via
        // refreshCameraDeviceList(), which re-selects by item DATA (findData).
        // The old `findChild<QComboBox*>()->setCurrentIndex(newIdx)` treated
        // the real /dev/video index as a combo POSITION (ERREUR #22's exact
        // confusion — with the microscope on video6 and a 2-item combo it
        // blanked the selection) and grabbed whichever combo findChild hit
        // first (audit B5).
        refreshCameraDeviceList();

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
        m_overlayValid = false;   // re-render the board buffer ± heatmap layer
        spdlog::info("Heatmap overlay {}", show ? "enabled" : "disabled");
    });

    // ── Board side (front/back) ─────────────────────────────────
    connect(m_mainWindow->controlPanel(), &gui::ControlPanel::boardSideChanged,
            this, [this](bool back) {
        setActiveLayer(back ? ibom::Layer::Back : ibom::Layer::Front);
    });

    // ── Board scan / golden diff / depth check (Inspection menu) ──
    connect(m_mainWindow.get(), &gui::MainWindow::boardScanToggled,
            this, &Application::onBoardScanToggled);
    connect(m_mainWindow.get(), &gui::MainWindow::goldenSaveRequested,
            this, &Application::saveGolden);
    connect(m_mainWindow.get(), &gui::MainWindow::goldenCompareRequested,
            this, &Application::compareGolden);
    connect(m_mainWindow.get(), &gui::MainWindow::depthInspectRequested,
            this, &Application::runDepthInspection);
    if (m_boardScanner) {
        connect(m_boardScanner, &features::BoardScanner::scanProgress,
                this, [this](double coverage, int frames) {
            m_mainWindow->updateStatusMessage(
                tr("Scanning board… %1% coverage (%2 frames) — uncheck "
                   "Inspection → Scan Board to finish")
                    .arg(static_cast<int>(coverage * 100)).arg(frames));
        }, Qt::QueuedConnection);
        connect(m_boardScanner, &features::BoardScanner::scanFinished,
                this, &Application::onScanFinished, Qt::QueuedConnection);
        connect(m_boardScanner, &features::BoardScanner::scanError,
                this, [this](const QString& msg) {
            m_scanActive = false;
            m_mainWindow->setBoardScanChecked(false);
            m_mainWindow->updateStatusMessage(msg);
            spdlog::warn("[scan] {}", msg.toStdString());
        }, Qt::QueuedConnection);
    }

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

        // Drop the pose FIRST, then turn live tracking off. Order matters: the
        // live-mode toggle's disable path restores m_baseHomography onto
        // m_homography — releasing it here makes that a no-op so the reset
        // isn't undone. Leaving live tracking ON after a reset meant the frame
        // handler kept feeding the tracker and the periodic re-anchor kept
        // firing on a board with no reference — exactly when a blob alias could
        // silently re-apply a wrong pose.
        m_homography->reset();
        m_baseHomography.release();  // stale — the pose it captured is gone
        if (m_liveMode) {
            if (auto* cp = m_mainWindow->controlPanel()) cp->setLiveMode(false);
            spdlog::info("Live tracking mode disabled (alignment reset)");
        }
        m_reanchorGate.reset();  // no pose left for a held candidate to correct
        ++m_alignmentEpoch;
        m_currentPixelsPerMm = 0.0;
        if (auto* sp = m_mainWindow->statsPanel()) sp->setScale(0.0);

        // Forget the saved profile too, so it isn't offered back on next reload.
        m_config->clearSavedAlignment();
        m_config->save();

        m_mainWindow->updateStatusMessage(
            tr("Alignment reset — the overlay is unaligned and live tracking is "
               "off. Use one of the Align buttons (or the Alignment Assistant) "
               "to set it up again."));
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
            const double rot = m_config->microscopeAnchorRotationDeg() * CV_PI / 180.0;

            // Back side: mirrored view frame (vx) — alignmath carries the
            // mirror into the similarity (see applyMultiAlignment()).
            const double vx = (m_activeLayer == ibom::Layer::Back) ? -1.0 : 1.0;
            const cv::Mat simH = overlay::alignmath::similarityFromAnchor(
                scale, rot, vx, m_anchorPcb, imgPt);

            auto& bb = m_ibomProject->boardInfo.boardBBox;
            std::vector<cv::Point2f> pcbCorners = {
                {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
                {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
                {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
                {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}
            };
            std::vector<cv::Point2f> imgCorners;
            cv::perspectiveTransform(pcbCorners, imgCorners, simH);

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

                // Similarity from the 2 correspondences (4 DOF = 4 equations).
                // Back side: fit in the mirrored view frame (vx) — see
                // applyMultiAlignment(); the mirror ends up inside the
                // homography, which keeps mapping RAW pcb coords.
                const double vx = (m_activeLayer == ibom::Layer::Back) ? -1.0 : 1.0;
                double scale = 0.0, rot = 0.0;
                const cv::Mat simH = overlay::alignmath::similarityFromTwoPoints(
                    m_alignPcb1, m_alignPcb2, m_alignImg1, m_alignImg2, vx,
                    &scale, &rot);
                if (simH.empty()) {
                    QMessageBox::warning(m_mainWindow.get(), tr("Alignment Failed"),
                        tr("The two components are too close together. Choose components that are far apart."));
                    m_mainWindow->updateStatusMessage(tr("Alignment failed — components too close"));
                    return;
                }

                // Build 4 virtual corners from the board bbox using the similarity
                auto& bb = m_ibomProject->boardInfo.boardBBox;
                std::vector<cv::Point2f> pcbCorners = {
                    {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
                    {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
                    {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
                    {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}
                };
                std::vector<cv::Point2f> imgCorners;
                cv::perspectiveTransform(pcbCorners, imgCorners, simH);

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

            // Compute homography: PCB board corners → clicked image points.
            // The user clicks the corners AS SEEN: on the back side the view
            // is mirrored, so "top-left as seen" is the RAW top-RIGHT corner —
            // swap TL↔TR and BR↔BL and findHomography encodes the mirror.
            auto& bb = m_ibomProject->boardInfo.boardBBox;
            const bool back = (m_activeLayer == ibom::Layer::Back);
            std::vector<cv::Point2f> pcbPts = {
                {static_cast<float>(back ? bb.maxX : bb.minX), static_cast<float>(bb.minY)},  // TL as seen
                {static_cast<float>(back ? bb.minX : bb.maxX), static_cast<float>(bb.minY)},  // TR as seen
                {static_cast<float>(back ? bb.minX : bb.maxX), static_cast<float>(bb.maxY)},  // BR as seen
                {static_cast<float>(back ? bb.maxX : bb.minX), static_cast<float>(bb.maxY)}   // BL as seen
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
        // Guided tour (B1): the PCB Map follows the route — setSelectedRef
        // recenters the view on the target when zoomed (suite 145 behavior).
        m_mainWindow->boardMinimap()->setSelectedRef(step.reference);
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
        m_placedOrder.push_back(ref);            // Ctrl+Z stack (B3)
        appendInspectionLog(QStringLiteral("placed"), ref);
        if (bomPanel) bomPanel->setComponentState(ref, tr("Placed"));
        m_mainWindow->boardMinimap()->setPlacedRefs(m_placedRefs);
        refreshInspectionStats();
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
            this, &Application::tourSkip);
    connect(inspPanel, &gui::InspectionPanel::backClicked,
            m_pickAndPlace.get(), &features::PickAndPlace::goBack);
    connect(inspPanel, &gui::InspectionPanel::resetClicked,
            this, [this]() {
        m_placedRefs.clear();
        m_placedOrder.clear();
        appendInspectionLog(QStringLiteral("reset"), "*");
        // Reflect the reset on EVERY surface (audit B2): the BOM rows kept
        // their "Placed"/"Skipped" labels and the PCB Map its green dots —
        // only the counters and the camera overlay were actually reset.
        if (auto* bp = m_mainWindow->bomPanel()) bp->clearAllStates();
        m_mainWindow->boardMinimap()->setPlacedRefs(m_placedRefs);
        refreshInspectionStats();
        saveInspectionState();  // empty set removes the saved entry
    });

    // ── Guided tour: hands-free shortcuts (B1/B3) ───────────────
    // P / N / Shift+N / Ctrl+Z go through the exact same paths as the
    // InspectionPanel buttons, so state, journal and persistence stay single.
    auto requireTour = [this]() {
        if (m_pickAndPlace->totalSteps() > 0) return true;
        m_mainWindow->updateStatusMessage(
            tr("Guided tour: start an inspection first (I)"));
        return false;
    };
    connect(m_mainWindow.get(), &gui::MainWindow::tourMarkPlaced,
            this, [this, requireTour]() {
        if (requireTour()) m_pickAndPlace->markPlaced();
    });
    connect(m_mainWindow.get(), &gui::MainWindow::tourNext,
            this, [this, requireTour]() {
        if (requireTour()) tourSkip();
    });
    connect(m_mainWindow.get(), &gui::MainWindow::tourPrev,
            this, [this, requireTour]() {
        if (requireTour()) m_pickAndPlace->goBack();
    });
    connect(m_mainWindow.get(), &gui::MainWindow::tourUndo,
            this, &Application::undoLastPlacement);
    connect(m_mainWindow.get(), &gui::MainWindow::revisionCompareRequested,
            this, &Application::compareRevision);
    connect(m_mainWindow.get(), &gui::MainWindow::revisionMarksClearRequested,
            this, [this]() { clearRevisionMarks(/*notify=*/true); });
    connect(m_mainWindow.get(), &gui::MainWindow::componentNoteRequested,
            this, &Application::editComponentNote);
    connect(m_mainWindow.get(), &gui::MainWindow::boardLibraryRequested,
            this, &Application::showBoardLibrary);
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
        if (scale <= 0.0) scale = 20.0;  // same fallback as the click-anchor path
        const double rot = m_config->microscopeAnchorRotationDeg() * CV_PI / 180.0;

        // Back side: mirrored view frame (vx) — same convention as the anchor
        // click handler / applyMultiAlignment (audit B8; the shared alignmath
        // helper is what keeps the four similarity paths from drifting apart
        // again).
        const double vx = (m_activeLayer == ibom::Layer::Back) ? -1.0 : 1.0;

        // Center of the camera image
        float iw = static_cast<float>(m_camera->resolution().width());
        float ih = static_cast<float>(m_camera->resolution().height());
        if (iw <= 0 || ih <= 0) { iw = 1920; ih = 1080; }
        const cv::Point2f imgCenter(iw / 2.f, ih / 2.f);

        const cv::Mat simH = overlay::alignmath::similarityFromAnchor(
            scale, rot, vx, pcbPt, imgCenter);

        auto& bb = m_ibomProject->boardInfo.boardBBox;
        std::vector<cv::Point2f> pcbCorners = {
            {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
            {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
            {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
            {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}
        };
        std::vector<cv::Point2f> imgCorners;
        cv::perspectiveTransform(pcbCorners, imgCorners, simH);
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
    // Everything cached from the PREVIOUS board must die here, or it gets
    // keyed/rendered against the new board (audit B3/B4). Stop a scan in
    // flight (its canvas geometry belongs to the old board — onScanFinished
    // discards its queued result via m_scanIbomHash), drop the cached mosaic
    // (Save-as-Golden would store it under the NEW board's hash) and purge the
    // defect heatmap (its cells are relative to the old board's bbox).
    if (m_scanActive)
        onBoardScanToggled(false);
    m_lastScanMosaic.release();
    m_lastScanMask.release();
    m_lastScanGeo = {};
    if (m_heatmapRenderer && m_heatmapRenderer->totalDefects() > 0) {
        m_heatmapRenderer->clear();
        ++m_heatmapRev;   // re-signature: the overlay cache re-renders without it
    }
    m_selectedRef.clear();  // the selection belonged to the old board
    clearRevisionMarks(/*notify=*/false);  // rework coloring too (C1 V2)
    // The scanned-board minimap background belongs to the old board.
    m_mainWindow->boardMinimap()->setBoardImage(QImage(), QRectF(),
                                                ibom::Layer::Front);

    // Reset the session undo stack — it must never cross board boundaries.
    m_placedOrder.clear();


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

    // Cache the board hash (audit log / scan-result key). Must run AFTER
    // setIBomFilePath: ibomContentHash() reads the configured path, so hashing
    // earlier stamped every entry with the PREVIOUS board's hash (audit B13).
    m_ibomHash = ibomContentHash();

    // B2 — bind the per-board annotation store (hash-keyed like golden/) and
    // surface the 📌 markers on the PCB Map.
    if (!m_ibomHash.isEmpty())
        m_annotations.open(utils::dataDir() / "annotations"
                           / (m_ibomHash.toStdString() + ".json"));
    else
        m_annotations.clear();
    m_mainWindow->boardMinimap()->setAnnotatedRefs(m_annotations.annotatedRefs());

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
    refreshInspectionStats();  // total from the new project, placed from restore

    // Restore a previously saved alignment for this exact iBOM file, if any —
    // best-effort: we have no way to know whether the camera/board moved
    // since, so this is offered as a starting point, not a guarantee. Only
    // applies if the live tracking/alignment flow hasn't already set one.
    if (!m_homography->isValid() && m_activeLayer == ibom::Layer::Front) {
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

    // Hand the project to the dataset capture worker — same side as the
    // overlay (kept in sync by setActiveLayer()).
    if (m_datasetCreator) {
        QMetaObject::invokeMethod(m_datasetCreator, "setProject", Qt::QueuedConnection,
            Q_ARG(std::shared_ptr<const ibom::IBomProject>,
                  std::shared_ptr<const IBomProject>(m_ibomProject)),
            Q_ARG(ibom::Layer, m_activeLayer));
    }

    // C2 — register/refresh this board in the library.
    updateBoardLibraryEntry();

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
    case SortMethod::ValueCount:      m_pickAndPlace->sortByValueGroupCount();  break;
    case SortMethod::ValueAlphabetic: m_pickAndPlace->sortByValueGroup();       break;
    case SortMethod::Position:        m_pickAndPlace->sortByPosition();         break;
    case SortMethod::FootprintSize:   m_pickAndPlace->sortByFootprintSize();    break;
    case SortMethod::NearestNeighbor: m_pickAndPlace->sortByNearestNeighbor();  break;
    }

    m_placedOrder.clear();   // fresh session = fresh undo stack
    appendInspectionLog(QStringLiteral("start"), "*");

    // Resume a previous session of this board, if one was saved (state is
    // written on every "Placed" click). The Reset button starts over.
    m_placedRefs = loadSavedPlacedRefs();
    if (!m_placedRefs.empty()) {
        const int restored = m_pickAndPlace->restorePlaced(m_placedRefs);
        if (restored > 0) {
            refreshInspectionStats();
            m_mainWindow->updateStatusMessage(
                tr("Inspection resumed: %1/%2 already placed")
                    .arg(restored).arg(m_pickAndPlace->totalSteps()));
            return;
        }
        m_placedRefs.clear();  // saved refs match nothing in this board
        // The load-time restore may have marked BOM rows / minimap dots from
        // that stale set — blank them so the fresh inspection starts clean
        // on every surface (audit B11).
        if (auto* bp = m_mainWindow->bomPanel()) bp->clearAllStates();
        m_mainWindow->boardMinimap()->setPlacedRefs(m_placedRefs);
    }
    refreshInspectionStats();

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
            // B2 — pinned note injected into the report's Detail column
            // (defectType feeds it; SolderInspector, its original producer,
            // is not wired, so the column is free).
            ir.defectType = m_annotations.noteText(r.reference);
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

// ── Guided tour / audit / revision diff (FEATURE_PROPOSALS B1/B3/C1) ──────

namespace {
// Defined below with the golden-diff/depth-check section.
void showResultsDialog(QWidget* parent, const QString& title,
                       const QStringList& headers,
                       const std::vector<QStringList>& rows);
} // namespace

void Application::tourSkip()
{
    if (m_pickAndPlace->currentIndex() < m_pickAndPlace->totalSteps()) {
        const auto& step = m_pickAndPlace->currentStep();
        if (auto* bomPanel = m_mainWindow->bomPanel())
            bomPanel->setComponentState(step.reference, tr("Skipped"));
        appendInspectionLog(QStringLiteral("skipped"), step.reference);
    }
    m_pickAndPlace->skip();
}

void Application::undoLastPlacement()
{
    if (m_placedOrder.empty()) {
        m_mainWindow->updateStatusMessage(
            tr("Nothing to undo (only this session's placements are undoable)"));
        return;
    }
    const std::string ref = m_placedOrder.back();
    m_placedOrder.pop_back();

    if (!m_pickAndPlace->unplace(ref)) {
        // Steps were reloaded/reset since — the stack entry is stale.
        m_mainWindow->updateStatusMessage(
            tr("Cannot undo %1 — inspection was reset since")
                .arg(QString::fromStdString(ref)));
        return;
    }
    m_placedRefs.erase(ref);
    if (auto* bomPanel = m_mainWindow->bomPanel())
        bomPanel->setComponentState(ref, QString());
    m_mainWindow->boardMinimap()->setPlacedRefs(m_placedRefs);
    refreshInspectionStats();
    saveInspectionState();
    appendInspectionLog(QStringLiteral("undo"), ref);
    m_mainWindow->updateStatusMessage(
        tr("Undid placement of %1").arg(QString::fromStdString(ref)));
}

void Application::appendInspectionLog(const QString& action, const std::string& ref)
{
    const auto path = utils::dataDir() / "inspection_log.csv";
    const bool fresh = !std::filesystem::exists(path);
    std::ofstream ofs(path, std::ios::app);
    if (!ofs) return;   // audit is best-effort, never blocks the workflow
    if (fresh) ofs << "timestamp,board,face,action,ref\n";
    ofs << QDateTime::currentDateTime().toString(Qt::ISODate).toStdString() << ','
        << m_ibomHash.toStdString() << ','
        << (m_activeLayer == Layer::Front ? "front" : "back") << ','
        << action.toStdString() << ',' << ref << '\n';
}

void Application::compareRevision()
{
    if (!m_ibomProject) {
        m_mainWindow->updateStatusMessage(tr("Revision diff: load an iBOM first"));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        m_mainWindow.get(), tr("Select the target revision's iBOM file"),
        QString(), tr("iBOM HTML files (*.html *.htm)"));
    if (path.isEmpty()) return;

    IBomParser parser;
    auto other = parser.parseFile(path.toStdString());
    if (!other) {
        m_mainWindow->updateStatusMessage(
            tr("Revision diff: failed to parse %1").arg(path));
        return;
    }

    const RevisionDiff diff = diffProjects(*m_ibomProject, *other);
    if (diff.empty()) {
        clearRevisionMarks(/*notify=*/false);   // stale coloring would lie
        m_mainWindow->updateStatusMessage(
            tr("Revision diff: identical BOMs (%1 components)")
                .arg(diff.unchanged));
        return;
    }

    // C1 V2 — rework coloring on the overlay: REMOVE red (with an X), CHANGE
    // orange; ADD as green ring markers at the target revision's positions
    // (those components don't exist in the current project). Split per side
    // so the overlay only shows the active face's adds.
    m_revDiffMarks.clear();
    m_revDiffAddsFront.clear();
    m_revDiffAddsBack.clear();
    for (const auto& ref : diff.removed) m_revDiffMarks[ref] = 1;
    for (const auto& ch : diff.changed)  m_revDiffMarks[ch.ref] = 2;
    if (!diff.added.empty()) {
        const std::unordered_set<std::string> added(diff.added.begin(),
                                                    diff.added.end());
        for (const auto& c : other->components) {
            if (!added.count(c.reference)) continue;
            auto& dst = (c.layer == Layer::Front) ? m_revDiffAddsFront
                                                  : m_revDiffAddsBack;
            dst.emplace_back(c.position, c.reference);
        }
    }
    ++m_revDiffRev;   // re-signature: the overlay re-renders with the marks

    // Rework order: desolder first, then place, then exchanges.
    std::vector<QStringList> rows;
    for (const auto& ref : diff.removed)
        rows.push_back({QString::fromStdString(ref), tr("REMOVE"),
                        tr("not in the target revision")});
    for (const auto& ref : diff.added)
        rows.push_back({QString::fromStdString(ref), tr("ADD"),
                        tr("new in the target revision")});
    for (const auto& ch : diff.changed) {
        QStringList what;
        if (ch.valueChanged)
            what << tr("value %1 → %2")
                        .arg(QString::fromStdString(ch.oldValue),
                             QString::fromStdString(ch.newValue));
        if (ch.footprintChanged)
            what << tr("footprint %1 → %2")
                        .arg(QString::fromStdString(ch.oldFootprint),
                             QString::fromStdString(ch.newFootprint));
        if (ch.layerChanged) what << tr("changes side");
        if (ch.moved)
            what << tr("moves %1 mm").arg(ch.moveDistMm, 0, 'f', 1);
        rows.push_back({QString::fromStdString(ch.ref), tr("CHANGE"),
                        what.join(QStringLiteral(" · "))});
    }

    showResultsDialog(m_mainWindow.get(),
                      tr("Revision diff — %1 vs %2")
                          .arg(QFileInfo(QString::fromStdString(
                                   m_config->ibomFilePath())).fileName(),
                               QFileInfo(path).fileName()),
                      {tr("Ref"), tr("Action"), tr("Detail")}, rows);

    spdlog::info("[revdiff] {} removed, {} added, {} changed, {} unchanged",
                 diff.removed.size(), diff.added.size(), diff.changed.size(),
                 diff.unchanged);
    m_mainWindow->updateStatusMessage(
        tr("Revision diff: %1 to remove (red), %2 to add (green), %3 to change "
           "(orange) — coloring shown on the overlay (Inspection → Clear "
           "Revision Marks to remove)")
            .arg(diff.removed.size()).arg(diff.added.size())
            .arg(diff.changed.size()));
}

void Application::clearRevisionMarks(bool notify)
{
    const bool had = !m_revDiffMarks.empty() || !m_revDiffAddsFront.empty()
                     || !m_revDiffAddsBack.empty();
    m_revDiffMarks.clear();
    m_revDiffAddsFront.clear();
    m_revDiffAddsBack.clear();
    if (had) ++m_revDiffRev;   // re-signature: overlay re-renders clean
    if (notify && m_mainWindow) {
        m_mainWindow->updateStatusMessage(had
            ? tr("Revision marks cleared")
            : tr("No revision marks to clear"));
    }
}

// ── Notes & board library (FEATURE_PROPOSALS B2 / C2) ─────────────────────

void Application::editComponentNote()
{
    if (!m_ibomProject) {
        m_mainWindow->updateStatusMessage(tr("Notes: load an iBOM first"));
        return;
    }
    if (m_selectedRef.empty()) {
        m_mainWindow->updateStatusMessage(
            tr("Notes: select a component first (BOM panel or PCB Map)"));
        return;
    }
    if (m_ibomHash.isEmpty()) {
        m_mainWindow->updateStatusMessage(
            tr("Notes: cannot hash the iBOM file — notes not available"));
        return;
    }

    const QString ref = QString::fromStdString(m_selectedRef);
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(
        m_mainWindow.get(), tr("Note — %1").arg(ref),
        tr("Free-text note pinned to %1 (leave empty to remove):").arg(ref),
        QString::fromStdString(m_annotations.noteText(m_selectedRef)), &ok);
    if (!ok) return;   // cancelled — leave the note untouched

    const QString trimmed = text.trimmed();
    m_annotations.setNote(
        m_selectedRef, trimmed.toStdString(),
        QDateTime::currentDateTime().toString(Qt::ISODate).toStdString(),
        m_activeLayer == Layer::Front ? "front" : "back");
    appendInspectionLog(QStringLiteral("note"), m_selectedRef);
    m_mainWindow->boardMinimap()->setAnnotatedRefs(m_annotations.annotatedRefs());
    updateBoardLibraryEntry();
    m_mainWindow->updateStatusMessage(trimmed.isEmpty()
        ? tr("Note removed from %1").arg(ref)
        : tr("Note saved on %1").arg(ref));
}

void Application::updateBoardLibraryEntry()
{
    if (!m_ibomProject || m_ibomHash.isEmpty()) return;
    features::BoardLibraryEntry e;
    e.hash  = m_ibomHash.toStdString();
    e.path  = m_config->ibomFilePath();
    e.title = m_ibomProject->boardInfo.title;
    if (e.title.empty())
        e.title = QFileInfo(QString::fromStdString(e.path))
                      .fileName().toStdString();
    e.lastOpened = QDateTime::currentDateTimeUtc()
                       .toString(Qt::ISODate).toStdString();
    e.components = static_cast<int>(m_ibomProject->components.size());
    e.placed     = static_cast<int>(m_placedRefs.size());
    std::error_code ec;
    e.hasGolden  = std::filesystem::exists(
        utils::dataDir() / "golden" / e.hash, ec);
    e.hasNotes   = m_annotations.count() > 0;
    m_boardLibrary.touch(e);
}

void Application::showBoardLibrary()
{
    const auto entries = m_boardLibrary.entries();

    auto* dlg = new QDialog(m_mainWindow.get());
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("Board Library"));
    dlg->resize(760, 420);
    auto* lay = new QVBoxLayout(dlg);

    auto* hint = new QLabel(
        entries.empty()
            ? tr("No board opened yet — boards are registered here "
                 "automatically when you open an iBOM file.")
            : tr("Double-click a board to open it. Its state (inspection "
                 "progress, golden scan, notes) is keyed by file CONTENT, so "
                 "it survives moving or renaming the HTML."),
        dlg);
    hint->setWordWrap(true);
    lay->addWidget(hint);

    auto* table = new QTableWidget(static_cast<int>(entries.size()), 6, dlg);
    table->setHorizontalHeaderLabels({tr("Board"), tr("Components"),
                                      tr("Placed"), tr("Golden"), tr("Notes"),
                                      tr("Last opened")});
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->horizontalHeader()->setStretchLastSection(true);
    for (int r = 0; r < static_cast<int>(entries.size()); ++r) {
        const auto& e = entries[r];
        auto* title = new QTableWidgetItem(QString::fromStdString(e.title));
        title->setToolTip(QString::fromStdString(e.path));
        title->setData(Qt::UserRole, QString::fromStdString(e.path));
        table->setItem(r, 0, title);
        table->setItem(r, 1, new QTableWidgetItem(QString::number(e.components)));
        table->setItem(r, 2, new QTableWidgetItem(QString::number(e.placed)));
        table->setItem(r, 3, new QTableWidgetItem(e.hasGolden ? QStringLiteral("✓") : QString()));
        table->setItem(r, 4, new QTableWidgetItem(e.hasNotes  ? QStringLiteral("📌") : QString()));
        table->setItem(r, 5, new QTableWidgetItem(QString::fromStdString(e.lastOpened)));
    }
    table->resizeColumnsToContents();
    lay->addWidget(table);

    connect(table, &QTableWidget::cellDoubleClicked, dlg,
            [this, dlg, table](int row, int) {
        auto* item = table->item(row, 0);
        if (!item) return;
        const QString path = item->data(Qt::UserRole).toString();
        dlg->close();
        if (!QFileInfo::exists(path)) {
            // The registry keeps the LAST known path — the file may have
            // moved since. The stored state (hash-keyed) is intact; the user
            // just needs to re-open the file from its new location.
            m_mainWindow->updateStatusMessage(
                tr("Board library: %1 no longer exists — use File → Open and "
                   "its saved state will reattach (content-keyed)").arg(path));
            return;
        }
        loadIBomFile(path);
    });

    dlg->show();
    dlg->raise();
}

// ── Board scan / golden diff / depth check (FEATURE_PROPOSALS A1-A3) ──────

namespace {

/// Small non-modal results table (golden diff, depth check). The dialog
/// deletes itself on close; rows are plain strings — the caller formats.
void showResultsDialog(QWidget* parent, const QString& title,
                       const QStringList& headers,
                       const std::vector<QStringList>& rows)
{
    auto* dlg = new QDialog(parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(title);
    dlg->resize(460, 520);
    auto* lay = new QVBoxLayout(dlg);

    auto* table = new QTableWidget(static_cast<int>(rows.size()),
                                   static_cast<int>(headers.size()), dlg);
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->horizontalHeader()->setStretchLastSection(true);
    for (int r = 0; r < static_cast<int>(rows.size()); ++r)
        for (int c = 0; c < rows[r].size() && c < headers.size(); ++c)
            table->setItem(r, c, new QTableWidgetItem(rows[r][c]));
    table->resizeColumnsToContents();
    lay->addWidget(table);

    dlg->show();
    dlg->raise();
}

} // namespace

QString Application::ibomContentHash() const
{
    const QString path = QString::fromStdString(m_config->ibomFilePath());
    if (path.isEmpty()) return {};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha1);
    if (!h.addData(&f)) return {};
    return QString::fromLatin1(h.result().toHex().left(12));
}

void Application::onBoardScanToggled(bool on)
{
    if (!m_boardScanner) return;

    if (!on) {
        if (!m_scanActive) return;
        m_scanActive = false;
        QMetaObject::invokeMethod(m_boardScanner, "stopScan", Qt::QueuedConnection);
        return;
    }

    if (!m_ibomProject) {
        m_mainWindow->updateStatusMessage(tr("Board scan: load an iBOM first"));
        m_mainWindow->setBoardScanChecked(false);
        return;
    }
    if (!m_homography || !m_homography->isValid()) {
        m_mainWindow->updateStatusMessage(
            tr("Board scan: align the overlay first (the scan follows the pose)"));
        m_mainWindow->setBoardScanChecked(false);
        return;
    }

    // Canvas resolution: follow the live optics where known (no point storing
    // more than the camera resolves), inside a sane band. The RAM guard in
    // BoardMosaic::initialize caps huge boards regardless.
    const double pxPerMm = m_currentPixelsPerMm > 0.0
        ? std::clamp(m_currentPixelsPerMm, 4.0, 12.0) : 8.0;
    const auto& bb = m_ibomProject->boardInfo.boardBBox;
    const QString exportDir =
        QString::fromStdString((utils::dataDir() / "scans").string());

    m_scanActive        = true;
    m_lastScanForwardMs = 0;
    m_scanIbomHash      = m_ibomHash;   // board identity guard, see onScanFinished
    QMetaObject::invokeMethod(m_boardScanner, "startScan", Qt::QueuedConnection,
        Q_ARG(double, pxPerMm),
        Q_ARG(double, bb.minX), Q_ARG(double, bb.minY),
        Q_ARG(double, bb.maxX), Q_ARG(double, bb.maxY),
        Q_ARG(int, m_activeLayer == Layer::Front ? 0 : 1),
        Q_ARG(QString, exportDir));
    m_mainWindow->updateStatusMessage(
        tr("Board scan started — sweep the whole board, then uncheck "
           "Inspection → Scan Board"));
}

void Application::onScanFinished(QString pngPath, cv::Mat mosaic, cv::Mat writtenMask,
                                 double coverageFrac, double pxPerMm,
                                 double minXmm, double minYmm, int layerInt)
{
    m_scanActive = false;
    m_mainWindow->setBoardScanChecked(false);

    // The worker's scanFinished is queued: when an iBOM load stopped this scan
    // it arrives AFTER the caches were purged — caching it would resurrect the
    // old board's mosaic and Save-as-Golden would key it under the NEW board's
    // hash (audit B3). Discard results from a board that is no longer loaded.
    if (m_scanIbomHash != m_ibomHash) {
        m_mainWindow->updateStatusMessage(
            tr("Board scan discarded — a different iBOM was loaded during the scan"));
        spdlog::info("[scan] result discarded (board changed during the scan)");
        return;
    }

    if (mosaic.empty() || writtenMask.empty()) {
        m_mainWindow->updateStatusMessage(
            tr("Board scan finished with no frames — was tracking healthy?"));
        return;
    }

    m_lastScanMosaic = mosaic;
    m_lastScanMask   = writtenMask;
    m_lastScanGeo    = {};
    m_lastScanGeo.pxPerMm  = pxPerMm;
    m_lastScanGeo.minXmm   = minXmm;
    m_lastScanGeo.minYmm   = minYmm;
    m_lastScanGeo.widthPx  = mosaic.cols;
    m_lastScanGeo.heightPx = mosaic.rows;
    m_lastScanLayer = layerInt == 0 ? Layer::Front : Layer::Back;

    // Show the orthorectified scan as the PCB Map's background (the mosaic is
    // in raw PCB mm by construction — same frame as the minimap). Deep copy
    // via matToQImage: once per finished scan, not per frame.
    if (pxPerMm > 0.0) {
        const QRectF pcbRect(minXmm, minYmm,
                             mosaic.cols / pxPerMm, mosaic.rows / pxPerMm);
        m_mainWindow->boardMinimap()->setBoardImage(
            utils::ImageUtils::matToQImage(mosaic), pcbRect, m_lastScanLayer);
    }

    if (pngPath.isEmpty()) {
        m_mainWindow->updateStatusMessage(
            tr("Board scan finished (%1% coverage) — PNG export failed, see log")
                .arg(static_cast<int>(coverageFrac * 100)));
    } else {
        m_mainWindow->updateStatusMessage(
            tr("Board scan exported: %1 (%2% coverage)")
                .arg(pngPath).arg(static_cast<int>(coverageFrac * 100)));
    }
}

void Application::saveGolden()
{
    if (m_lastScanMosaic.empty()) {
        m_mainWindow->updateStatusMessage(
            tr("No finished board scan — run Inspection → Scan Board first"));
        return;
    }
    const QString hash = ibomContentHash();
    if (hash.isEmpty()) {
        m_mainWindow->updateStatusMessage(
            tr("Cannot hash the iBOM file — golden not saved"));
        return;
    }

    namespace fs = std::filesystem;
    const fs::path dir = utils::dataDir() / "golden" / hash.toStdString();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string base = m_lastScanLayer == Layer::Front ? "front" : "back";

    if (!cv::imwrite((dir / (base + ".png")).string(), m_lastScanMosaic)
        || !cv::imwrite((dir / (base + "_mask.png")).string(), m_lastScanMask)) {
        m_mainWindow->updateStatusMessage(
            tr("Failed to write the golden images under %1")
                .arg(QString::fromStdString(dir.string())));
        return;
    }
    nlohmann::json meta;
    meta["px_per_mm"] = m_lastScanGeo.pxPerMm;
    meta["min_x_mm"]  = m_lastScanGeo.minXmm;
    meta["min_y_mm"]  = m_lastScanGeo.minYmm;
    meta["layer"]     = base;
    meta["ibom_path"] = m_config->ibomFilePath();   // hint only, hash is the key
    std::ofstream ofs(dir / (base + ".json"));
    ofs << meta.dump(2);

    spdlog::info("[golden] saved {} face under {}", base, dir.string());
    updateBoardLibraryEntry();   // the library's "Golden ✓" column (C2)
    m_mainWindow->updateStatusMessage(
        tr("Golden %1 face saved for this board")
            .arg(m_lastScanLayer == Layer::Front ? tr("front") : tr("back")));
}

void Application::compareGolden()
{
    if (m_lastScanMosaic.empty()) {
        m_mainWindow->updateStatusMessage(
            tr("No finished board scan — run Inspection → Scan Board first"));
        return;
    }
    if (!m_ibomProject) return;
    const QString hash = ibomContentHash();
    if (hash.isEmpty()) {
        m_mainWindow->updateStatusMessage(tr("Cannot hash the iBOM file"));
        return;
    }

    namespace fs = std::filesystem;
    const std::string base = m_lastScanLayer == Layer::Front ? "front" : "back";
    const fs::path dir = utils::dataDir() / "golden" / hash.toStdString();
    const fs::path goldenPath = dir / (base + ".png");
    if (!fs::exists(goldenPath)) {
        m_mainWindow->updateStatusMessage(
            tr("No golden stored for this board/face — use "
               "Inspection → Save Last Scan as Golden first"));
        return;
    }

    cv::Mat golden = cv::imread(goldenPath.string(), cv::IMREAD_COLOR);
    cv::Mat goldenMask = cv::imread((dir / (base + "_mask.png")).string(),
                                    cv::IMREAD_GRAYSCALE);
    if (golden.empty()) {
        m_mainWindow->updateStatusMessage(tr("Failed to read the golden image"));
        return;
    }
    if (goldenMask.empty())
        goldenMask = cv::Mat(golden.size(), CV_8U, cv::Scalar(255));
    // Different scan resolution than when the golden was taken → resample the
    // golden onto the current canvas (same board rect + margin by
    // construction, so a plain resize aligns them).
    if (golden.size() != m_lastScanMosaic.size()) {
        cv::resize(golden, golden, m_lastScanMosaic.size(), 0, 0, cv::INTER_AREA);
        cv::resize(goldenMask, goldenMask, m_lastScanMosaic.size(), 0, 0,
                   cv::INTER_NEAREST);
    }

    const cv::Mat diff = features::computeDiffMap(
        golden, goldenMask, m_lastScanMosaic, m_lastScanMask);
    const auto anomalies = features::scoreComponents(
        diff, m_lastScanGeo, m_ibomProject->components, m_lastScanLayer);
    if (anomalies.empty()) {
        m_mainWindow->updateStatusMessage(
            tr("Golden compare: no comparable components (coverage overlap "
               "too small?)"));
        return;
    }

    // Feed the defect heatmap (this is what HeatmapRenderer was born for) and
    // invalidate the overlay cache so the composite refreshes.
    constexpr double kSuspectThreshold = 0.30;
    const auto& bb = m_ibomProject->boardInfo.boardBBox;
    m_heatmapRenderer->initialize(static_cast<int>(std::ceil(bb.width())),
                                  static_cast<int>(std::ceil(bb.height())), 1.0f);
    int suspects = 0;
    for (const auto& a : anomalies) {
        if (a.score < kSuspectThreshold) continue;
        ++suspects;
        m_heatmapRenderer->addDefect(
            static_cast<float>(a.pcbCenter.x - bb.minX),
            static_cast<float>(a.pcbCenter.y - bb.minY),
            static_cast<float>(a.score));
    }
    ++m_heatmapRev;
    m_overlayValid = false;

    std::vector<QStringList> rows;
    for (const auto& a : anomalies) {
        if (rows.size() >= 200) break;
        rows.push_back({QString::fromStdString(a.ref),
                        QString::number(a.score, 'f', 2),
                        a.score >= kSuspectThreshold ? tr("⚠ check") : tr("ok")});
    }
    showResultsDialog(m_mainWindow.get(),
                      tr("Golden comparison — %1 face")
                          .arg(m_lastScanLayer == Layer::Front ? tr("front") : tr("back")),
                      {tr("Ref"), tr("Anomaly"), tr("Verdict")}, rows);

    spdlog::info("[golden] compared {} components, {} suspicious (threshold {})",
                 anomalies.size(), suspects, kSuspectThreshold);
    m_mainWindow->updateStatusMessage(
        tr("Golden compare: %1 components, %2 suspicious — enable "
           "'Show Defect Heatmap' to see them on the overlay")
            .arg(anomalies.size()).arg(suspects));
}

void Application::runDepthInspection()
{
    if (!m_ibomProject) {
        m_mainWindow->updateStatusMessage(tr("Depth check: load an iBOM first"));
        return;
    }
    if (!m_homography || !m_homography->isValid()) {
        m_mainWindow->updateStatusMessage(tr("Depth check: align the overlay first"));
        return;
    }
    // Explicit backend gate: the cached depth frame is cleared on backend
    // switch (audit B6), but never cross a microscope pose with depth data.
    if (m_activeBackend != CameraBackend::RealSense) {
        m_mainWindow->updateStatusMessage(
            tr("Depth check: switch to the RealSense backend first"));
        return;
    }
    if (!m_lastDepthFrame || m_lastDepthFrame->empty()) {
        m_mainWindow->updateStatusMessage(
            tr("Depth check: no depth frame — RealSense backend required"));
        return;
    }

    // Snapshot the shared depth view (the capture thread keeps publishing).
    const cv::Mat depth = m_lastDepthFrame->clone();

    // Board quad under the current pose → the plane-fit region.
    const auto& bb = m_ibomProject->boardInfo.boardBBox;
    std::vector<cv::Point2f> quad;
    for (const auto& c : {cv::Point2f(bb.minX, bb.minY), cv::Point2f(bb.maxX, bb.minY),
                          cv::Point2f(bb.maxX, bb.maxY), cv::Point2f(bb.minX, bb.maxY)})
        quad.push_back(m_homography->pcbToImage(c));

    const features::BoardPlane plane = features::fitBoardPlane(depth, quad);
    if (!plane.valid) {
        m_mainWindow->updateStatusMessage(
            tr("Depth check: board plane not found (too many dropouts?)"));
        return;
    }
    spdlog::info("[depth-check] plane fit: z = {:.4f}x + {:.4f}y + {:.1f} "
                 "(inliers {:.0f}%)",
                 plane.a, plane.b, plane.c, plane.inlierFrac * 100.0);

    const auto verdicts = features::inspectComponents(
        depth, m_homography->matrix(), m_ibomProject->components,
        m_activeLayer, plane);
    if (verdicts.empty()) {
        m_mainWindow->updateStatusMessage(
            tr("Depth check: no component visible in the depth frame"));
        return;
    }

    using S = features::DepthVerdictStatus;
    int absent = 0, present = 0, uncertain = 0;
    for (const auto& v : verdicts) {
        if (v.status == S::Absent)       ++absent;
        else if (v.status == S::Present) ++present;
        else                             ++uncertain;
    }

    // Absent first (that's what the user is looking for), then uncertain.
    auto rank = [](S s) { return s == S::Absent ? 0 : s == S::Uncertain ? 1 : 2; };
    auto sorted = verdicts;
    std::sort(sorted.begin(), sorted.end(),
              [&](const features::DepthVerdict& a, const features::DepthVerdict& b) {
                  return rank(a.status) < rank(b.status);
              });
    std::vector<QStringList> rows;
    for (const auto& v : sorted) {
        if (rows.size() >= 300) break;
        const QString st = v.status == S::Absent  ? tr("ABSENT")
                        : v.status == S::Present ? tr("present")
                                                 : tr("uncertain");
        rows.push_back({QString::fromStdString(v.ref), st,
                        QString::number(v.heightMm, 'f', 2) + " mm",
                        QString::number(v.validPx)});
    }
    showResultsDialog(m_mainWindow.get(), tr("Depth check (D405)"),
                      {tr("Ref"), tr("Status"), tr("Height"), tr("Depth px")}, rows);

    spdlog::info("[depth-check] {} components: {} present, {} ABSENT, {} uncertain",
                 verdicts.size(), present, absent, uncertain);
    m_mainWindow->updateStatusMessage(
        tr("Depth check: %1 present, %2 absent, %3 uncertain")
            .arg(present).arg(absent).arg(uncertain));
}

} // namespace ibom
