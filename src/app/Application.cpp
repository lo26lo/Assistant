#include "Application.h"
#include "Config.h"
#include "gui/MainWindow.h"
#include "gui/CameraView.h"
#include "gui/ControlPanel.h"
#include "gui/StatsPanel.h"
#include "gui/BomPanel.h"
#include "gui/InspectionWizard.h"
#include "camera/CameraCapture.h"
#include "camera/CameraCalibration.h"
#include "ai/InferenceEngine.h"
#include "ai/ModelManager.h"
#include "ibom/IBomParser.h"
#include "ibom/IBomData.h"
#include "overlay/OverlayRenderer.h"
#include "overlay/Homography.h"
#include "overlay/HeatmapRenderer.h"
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
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/calib3d.hpp>

Q_DECLARE_METATYPE(cv::Mat)

namespace ibom {

Application::Application(QApplication& qapp)
    : QObject(&qapp)
    , m_qapp(qapp)
{
}

Application::~Application() = default;

bool Application::initialize()
{
    // Register cv::Mat for cross-thread signal/slot
    qRegisterMetaType<cv::Mat>("cv::Mat");

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

    // Feature detector for live tracking mode
    m_featureDetector = cv::ORB::create(m_config->orbKeypoints());
    m_featureMatcher = cv::BFMatcher::create(cv::NORM_HAMMING, true);

    // Main window (owns GUI widgets)
    spdlog::info("Creating MainWindow...");
    m_mainWindow = std::make_unique<gui::MainWindow>(this);
    spdlog::info("All subsystems created.");
}

void Application::connectSignals()
{
    connect(this, &Application::shutdownRequested, &m_qapp, &QApplication::quit);

    // ── Camera toggle ───────────────────────────────────────────
    connect(m_mainWindow.get(), &gui::MainWindow::cameraToggled, this, [this](bool start) {
        if (start) {
            auto* cp = m_mainWindow->controlPanel();
            m_camera->setDeviceIndex(cp->cameraIndex());
            m_camera->setResolution(cp->cameraWidth(), cp->cameraHeight());
            m_camera->setFps(cp->cameraFps());
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
    connect(m_camera.get(), &camera::CameraCapture::frameReady, this, [this](const cv::Mat& frame) {
        cv::Mat processed = frame;

        // Apply undistortion if calibrated
        if (m_calibration && m_calibration->isCalibrated()) {
            processed = m_calibration->undistort(processed);
        }

        // ── Live tracking: update homography from feature matching ──
        // Throttled to max ~5 fps to avoid blocking the GUI thread
        if (m_liveMode && m_homography && m_homography->isValid() && m_featureDetector && m_ibomProject) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_lastTrackingTime).count();

            if (elapsed >= m_config->trackingIntervalMs()) {
                m_lastTrackingTime = now;

                cv::Mat gray;
                if (processed.channels() == 3)
                    cv::cvtColor(processed, gray, cv::COLOR_BGR2GRAY);
                else
                    gray = processed.clone();

                if (m_referenceFrame.empty()) {
                    // Capture reference frame and features
                    m_referenceFrame = gray.clone();
                    m_refKeypoints.clear();
                    m_refDescriptors = cv::Mat();
                    m_featureDetector->detectAndCompute(
                        m_referenceFrame, cv::noArray(), m_refKeypoints, m_refDescriptors);
                    m_baseHomography = m_homography->matrix().clone();
                    spdlog::info("Live tracking: reference frame captured ({} keypoints)",
                                 m_refKeypoints.size());
                } else if (!m_refDescriptors.empty() && m_refKeypoints.size() >= 4) {
                    try {
                        std::vector<cv::KeyPoint> curKeypoints;
                        cv::Mat curDescriptors;
                        m_featureDetector->detectAndCompute(
                            gray, cv::noArray(), curKeypoints, curDescriptors);

                        if (!curDescriptors.empty() && curKeypoints.size() >= 4) {
                            std::vector<cv::DMatch> matches;
                            m_featureMatcher->match(m_refDescriptors, curDescriptors, matches);

                            if (matches.size() >= static_cast<size_t>(m_config->minMatchCount())) {
                                double minDist = 1e9;
                                for (const auto& m : matches)
                                    if (m.distance < minDist) minDist = m.distance;
                                double threshold = std::max(m_config->matchDistanceRatio() * minDist, 30.0);

                                std::vector<cv::Point2f> srcPts, dstPts;
                                for (const auto& m : matches) {
                                    if (m.distance <= threshold) {
                                        srcPts.push_back(m_refKeypoints[m.queryIdx].pt);
                                        dstPts.push_back(curKeypoints[m.trainIdx].pt);
                                    }
                                }

                                if (srcPts.size() >= static_cast<size_t>(m_config->minMatchCount())) {
                                    cv::Mat frameH = cv::findHomography(srcPts, dstPts, cv::RANSAC, m_config->ransacThreshold());
                                    if (!frameH.empty() && frameH.rows == 3 && frameH.cols == 3) {
                                        cv::Mat combined = frameH * m_baseHomography;
                                        auto& bb = m_ibomProject->boardInfo.boardBBox;
                                        std::vector<cv::Point2f> pcbCorners = {
                                            {static_cast<float>(bb.minX), static_cast<float>(bb.minY)},
                                            {static_cast<float>(bb.maxX), static_cast<float>(bb.minY)},
                                            {static_cast<float>(bb.maxX), static_cast<float>(bb.maxY)},
                                            {static_cast<float>(bb.minX), static_cast<float>(bb.maxY)}
                                        };
                                        std::vector<cv::Point2f> imgCorners;
                                        cv::perspectiveTransform(pcbCorners, imgCorners, combined);
                                        m_homography->compute(pcbCorners, imgCorners);
                                    }
                                }
                            }
                        }
                    } catch (const cv::Exception& e) {
                        spdlog::warn("Live tracking frame error: {}", e.what());
                    }
                }
            }
        }

        // Convert to RGB for QImage
        cv::Mat rgb;
        if (processed.channels() == 3)
            cv::cvtColor(processed, rgb, cv::COLOR_BGR2RGB);
        else if (processed.channels() == 1)
            cv::cvtColor(processed, rgb, cv::COLOR_GRAY2RGB);
        else
            rgb = processed.clone();

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
            for (const auto& comp : m_ibomProject->components) {
                if (comp.layer != Layer::Front) continue;

                bool isSelected = (comp.reference == m_selectedRef);
                QColor padColor = isSelected ? QColor(255, 200, 0, 200) : QColor(180, 160, 80, 180);
                QColor silkColor = isSelected ? QColor(255, 255, 100, 220) : QColor(170, 170, 68, 180);

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
                        painter.setPen(QPen(silkColor, 1.0));
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
                            painter.setPen(QPen(silkColor, 1.0));
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
                        painter.setPen(QPen(silkColor, 1.0));
                        painter.drawEllipse(QPointF(c.x, c.y), static_cast<qreal>(r), static_cast<qreal>(r));

                    } else if (seg.type == DrawingSegment::Type::Polygon && !seg.points.empty()) {
                        QPolygonF polyPts;
                        for (const auto& pt : seg.points) {
                            cv::Point2f ip = m_homography->pcbToImage(
                                cv::Point2f(static_cast<float>(pt.x), static_cast<float>(pt.y)));
                            polyPts << QPointF(ip.x, ip.y);
                        }
                        painter.setPen(QPen(silkColor, 1.0));
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
                painter.setPen(isSelected ? QColor(255, 255, 200) : QColor(68, 170, 170, 200));
                painter.setFont(QFont("Segoe UI", 7));
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
            pickPainter.setPen(QPen(QColor(255, 50, 50), 2));
            pickPainter.setBrush(QColor(255, 50, 50, 100));
            for (const auto& pt : m_homographyImagePoints) {
                pickPainter.drawEllipse(QPointF(pt.x, pt.y), 8, 8);
            }
            // Draw lines between consecutive points
            if (m_homographyImagePoints.size() >= 2) {
                QPen linePen(QColor(255, 100, 100, 180), 1, Qt::DashLine);
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
                   "1. Hold a 9x6 checkerboard in front of the camera\n"
                   "2. Click 'Calibrate' again to capture (5 images needed)\n"
                   "3. Move the checkerboard to a different angle each time"));
            // Update button text to show progress
            for (auto* b : m_mainWindow->controlPanel()->findChildren<QPushButton*>()) {
                if (b->text().contains("Calibrat")) {
                    b->setText(tr("Capture Calibration Image (0/5)"));
                    break;
                }
            }
            spdlog::info("Calibration image collection started");
        } else {
            // Capture current frame
            cv::Mat current = m_camera->latestFrame();
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

    // ── BOM panel component selection → overlay highlight ───────
    connect(m_mainWindow->bomPanel(), &gui::BomPanel::componentSelected,
            this, [this](const std::string& ref) {
        m_selectedRef = ref;
        if (m_overlayRenderer)
            m_overlayRenderer->setHighlightedRefs({ref});
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
        // Recreate ORB detector with updated keypoint count
        m_featureDetector = cv::ORB::create(m_config->orbKeypoints());
        // Reset reference frame so next tracking cycle recomputes
        m_referenceFrame = cv::Mat();
        spdlog::info("Settings applied (ORB={}, interval={}ms, RANSAC={:.1f})",
                     m_config->orbKeypoints(), m_config->trackingIntervalMs(),
                     m_config->ransacThreshold());
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
        m_pickingHomographyPoints = true;
        m_homographyImagePoints.clear();
        m_mainWindow->updateStatusMessage(
            tr("Click the TOP-LEFT corner of the PCB in the camera image (1/4)"));
        spdlog::info("Manual homography: point picking started");
    });

    connect(m_mainWindow->cameraView(), &gui::CameraView::clicked,
            this, [this](QPointF imagePos) {
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
                // Reset live mode reference if active
                m_referenceFrame = cv::Mat();
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
            // Store the current homography as base
            m_baseHomography = m_homography->matrix().clone();
            // Reference frame will be captured on next frame
            m_referenceFrame = cv::Mat();
            spdlog::info("Live tracking mode enabled");
            m_mainWindow->updateStatusMessage(tr("Live tracking mode ON"));
        } else {
            // Restore base homography
            if (!m_baseHomography.empty()) {
                // Recompute from base
                m_homography->load(""); // reset
                // Re-apply base by using existing PCB→image mapping
            }
            m_referenceFrame = cv::Mat();
            spdlog::info("Live tracking mode disabled");
            m_mainWindow->updateStatusMessage(tr("Live tracking mode OFF"));
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

    // Auto-compute a basic homography based on board bounding box
    if (!m_homography->isValid()) {
        auto& bb = m_ibomProject->boardInfo.boardBBox;
        float bw = static_cast<float>(bb.maxX - bb.minX);
        float bh = static_cast<float>(bb.maxY - bb.minY);
        if (bw > 0 && bh > 0) {
            float iw = 1920.0f;
            float ih = 1080.0f;
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
}

// ── Calibration ────────────────────────────────────────────────────

void Application::runCalibration()
{
    spdlog::info("Running calibration with {} images...", m_calibImages.size());
    m_mainWindow->updateStatusMessage(tr("Running calibration..."));

    double error = m_calibration->calibrate(m_calibImages);
    m_calibImages.clear();

    if (error < 0) {
        spdlog::error("Calibration failed");
        m_mainWindow->updateStatusMessage(tr("Calibration failed — checkerboard not detected"));
        QMessageBox::warning(m_mainWindow.get(), tr("Calibration Failed"),
            tr("Could not find checkerboard corners in the captured images.\n"
               "Make sure the 9x6 checkerboard pattern is fully visible."));
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
    m_mainWindow->updateStatusMessage(
        tr("Calibration done — error: %1, pixels/mm: %2")
        .arg(error, 0, 'f', 4).arg(m_calibration->pixelsPerMm(), 0, 'f', 1));
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
