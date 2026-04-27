#include "Application.h"
#include "Config.h"
#include "gui/MainWindow.h"
#include "gui/CameraView.h"
#include "gui/ControlPanel.h"
#include "gui/StatsPanel.h"
#include "gui/BomPanel.h"
#include "gui/InspectionWizard.h"
#include "gui/InspectionPanel.h"
#include "camera/CameraCapture.h"
#include "camera/CameraCalibration.h"
#include "ai/InferenceEngine.h"
#include "ai/ModelManager.h"
#include "ibom/IBomParser.h"
#include "ibom/IBomData.h"
#include "overlay/OverlayRenderer.h"
#include "overlay/Homography.h"
#include "overlay/HeatmapRenderer.h"
#include "overlay/TrackingWorker.h"
#include "features/PickAndPlace.h"
#include "features/Measurement.h"
#include "features/SnapshotHistory.h"
#include "export/DataExporter.h"
#include "gui/Theme.h"
#include "utils/Logger.h"

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
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>

Q_DECLARE_METATYPE(cv::Mat)

namespace ibom {

Application::Application(QApplication& qapp)
    : QObject(&qapp)
    , m_qapp(qapp)
{
}

Application::~Application()
{
    if (m_trackingThread) {
        m_trackingThread->quit();
        m_trackingThread->wait();
    }
}

bool Application::initialize()
{
    // Register types for cross-thread signal/slot marshalling.
    qRegisterMetaType<cv::Mat>("cv::Mat");
    qRegisterMetaType<ibom::camera::FrameRef>("ibom::camera::FrameRef");

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

    // Enumerate cameras and populate ControlPanel
    {
        QStringList cameraNames;
        const auto cameras = QMediaDevices::videoInputs();
        for (int i = 0; i < cameras.size(); ++i)
            cameraNames << QString("%1: %2").arg(i).arg(cameras[i].description());
        if (cameraNames.isEmpty())
            cameraNames << tr("No camera detected");
        m_mainWindow->controlPanel()->setCameraDevices(cameraNames);
        // Select the configured camera index
        int idx = m_config->cameraIndex();
        if (auto* cp = m_mainWindow->controlPanel()) {
            if (idx >= 0 && idx < cameraNames.size())
                cp->findChild<QComboBox*>()->setCurrentIndex(idx);
        }
        spdlog::info("Found {} camera(s)", cameras.size());
    }

    // Show main window
    m_mainWindow->show();

    spdlog::info("Application initialized successfully.");
    return true;
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

    // Camera capture
    spdlog::info("Creating CameraCapture...");
    m_camera = std::make_unique<camera::CameraCapture>(m_config->cameraIndex());

    // Camera calibration
    spdlog::info("Creating CameraCalibration...");
    m_calibration = std::make_unique<camera::CameraCalibration>();
    // Try to load existing calibration
    auto calibPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                     + "/calibration.yml";
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

    // Inspection workflow features
    spdlog::info("Creating inspection workflow features...");
    m_pickAndPlace    = std::make_unique<features::PickAndPlace>(this);
    m_measurement     = std::make_unique<features::Measurement>(this);
    m_snapshotHistory = std::make_unique<features::SnapshotHistory>(this);
    m_dataExporter    = std::make_unique<exports::DataExporter>(this);

    // Configure storage for snapshots — silent saves under AppData
    QString snapDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                      + "/snapshots";
    m_snapshotHistory->setStorageDir(snapDir);

    // Main window (owns GUI widgets)
    spdlog::info("Creating MainWindow...");
    m_mainWindow = std::make_unique<gui::MainWindow>(this);
    spdlog::info("All subsystems created.");
}

void Application::connectSignals()
{
    connect(this, &Application::shutdownRequested, &m_qapp, &QApplication::quit);

    // ── Tracking worker → homography update on GUI thread ───────
    if (m_trackingWorker) {
        connect(m_trackingWorker, &overlay::TrackingWorker::homographyUpdated,
                this, [this](cv::Mat combined) {
            if (!m_liveMode || combined.empty() || !m_homography) return;
            m_homography->setMatrix(combined);
            if (m_overlayRenderer)
                m_overlayRenderer->setHomography(*m_homography);
            updateDynamicScale();
        }, Qt::QueuedConnection);

        connect(m_trackingWorker, &overlay::TrackingWorker::trackingError,
                this, [](const QString& msg) {
            spdlog::warn("Tracking worker error: {}", msg.toStdString());
        }, Qt::QueuedConnection);
    }

    // ── Camera toggle ───────────────────────────────────────────
    connect(m_mainWindow.get(), &gui::MainWindow::cameraToggled, this, [this](bool start) {
        if (start) {
            m_camera->setDeviceIndex(m_config->cameraIndex());
            m_camera->setResolution(m_config->cameraWidth(), m_config->cameraHeight());
            m_camera->setFps(m_config->cameraFps());
            if (!m_camera->start()) {
                m_mainWindow->updateStatusMessage(tr("Failed to start camera"));
            }
        } else {
            m_camera->stop();
        }
    });

    // ── Camera errors ───────────────────────────────────────────
    connect(m_camera.get(), &camera::CameraCapture::captureError, this, [this](const QString& msg) {
        spdlog::error("Camera error: {}", msg.toStdString());
        m_mainWindow->updateStatusMessage(msg);
    });

    // ── Camera frame → CameraView + Overlay ─────────────────────
    // Slot receives a shared_ptr<const cv::Mat> — no pixel clone across threads.
    connect(m_camera.get(), &camera::CameraCapture::frameReady, this,
            [this](ibom::camera::FrameRef frameRef) {
        if (!frameRef || frameRef->empty()) return;
        const cv::Mat& frame = *frameRef;

        // ── Live tracking: hand the raw frame off to the worker thread ──
        // The worker throttles, downscales and runs ORB without blocking us.
        if (m_liveMode && m_trackingWorker && m_homography && m_homography->isValid()) {
            QMetaObject::invokeMethod(m_trackingWorker, "processFrame", Qt::QueuedConnection,
                Q_ARG(ibom::camera::FrameRef, frameRef));
        }

        // Apply undistortion if calibrated (allocates a new Mat; unavoidable).
        cv::Mat processed;
        if (m_calibration && m_calibration->isCalibrated()) {
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
        m_mainWindow->cameraView()->updateFrame(qimg.copy());

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
            const bool drawFab  = m_config->showFabrication();

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
                    cv::Point2f imgPad = m_homography->pcbToImage(padCenter);

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
        if (wasCapturing) m_camera->start();
        spdlog::info("Camera settings applied: device={} {}x{} @{}fps", index, w, h, fps);
    });

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

    // ── Settings changed ────────────────────────────────────────
    connect(m_mainWindow.get(), &gui::MainWindow::settingsChanged,
            this, [this]() {
        // Check if camera index changed and restart if needed
        int newIdx = m_config->cameraIndex();
        bool wasCapturing = m_camera->isCapturing();
        if (wasCapturing) {
            m_camera->stop();
            m_camera->setDeviceIndex(newIdx);
            m_camera->setResolution(m_config->cameraWidth(), m_config->cameraHeight());
            m_camera->setFps(m_config->cameraFps());
            m_camera->start();
            spdlog::info("Camera restarted on device {} ({}x{} @{}fps)",
                         newIdx, m_config->cameraWidth(), m_config->cameraHeight(),
                         m_config->cameraFps());
        } else {
            m_camera->setDeviceIndex(newIdx);
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
            this, [this]() { m_placedRefs.clear(); });
    connect(inspPanel, &gui::InspectionPanel::resetClicked,
            m_pickAndPlace.get(), &features::PickAndPlace::reset);

    // Measurement: mode change toggles CameraView measure mode + sets calibration
    connect(inspPanel, &gui::InspectionPanel::measurementModeChanged,
            this, [this](int mode) {
        bool active = (mode >= 0);
        m_mainWindow->cameraView()->setMeasurementMode(active);
        if (active) {
            m_measurement->setCalibration(m_currentPixelsPerMm);
            m_measurement->setMode(static_cast<features::Measurement::Mode>(mode));
            m_measurement->clearPoints();
        }
    });

    // CameraView clicks in measure mode → Measurement::addPoint
    connect(m_mainWindow->cameraView(), &gui::CameraView::measurePoint,
            this, [this](QPointF imagePos) {
        m_measurement->addPoint(imagePos);
    });

    connect(m_measurement.get(), &features::Measurement::measurementComplete,
            inspPanel, [inspPanel](const features::Measurement::MeasureResult& r) {
        QString unit;
        switch (r.mode) {
        case features::Measurement::Mode::Distance:
        case features::Measurement::Mode::PinPitch: unit = "mm";  break;
        case features::Measurement::Mode::Angle:    unit = "deg"; break;
        case features::Measurement::Mode::Area:     unit = "mm²"; break;
        }
        inspPanel->onMeasurementResult(r.valuePixels, r.valueMM, unit);
    });

    connect(inspPanel, &gui::InspectionPanel::clearMeasurementsClicked,
            this, [this]() {
        m_measurement->clearPoints();
        m_measurement->clearHistory();
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

    // Save calibration
    auto calibPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                     + "/calibration.yml";
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
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
