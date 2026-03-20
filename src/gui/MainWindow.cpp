#include "MainWindow.h"
#include "CameraView.h"
#include "BomPanel.h"
#include "ControlPanel.h"
#include "InspectionWizard.h"
#include "StatsPanel.h"
#include "SettingsDialog.h"
#include "Theme.h"
#include "../app/Application.h"
#include "../app/Config.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QDockWidget>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QSettings>
#include <QScreen>
#include <QShortcut>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <spdlog/spdlog.h>

namespace ibom::gui {

MainWindow::MainWindow(Application* app, QWidget* parent)
    : QMainWindow(parent)
    , m_app(app)
{
    setWindowTitle("PCB Inspector — iBOM Overlay");
    setMinimumSize(1280, 720);
    setAcceptDrops(true);

    // Central camera view
    m_cameraView = new CameraView(this);
    setCentralWidget(m_cameraView);

    createActions();
    createMenuBar();
    createToolBar();
    createStatusBar();
    createDockWidgets();
    restoreLayoutState();

    // Apply theme
    m_darkMode = app->config().darkMode();
    if (m_darkMode)
        applyDarkStylesheet();
    else
        applyLightStylesheet();

    // Double-click camera view → camera-only fullscreen
    connect(m_cameraView, &CameraView::doubleClicked, this, [this]() {
        toggleCameraFullscreen(!m_cameraFullscreen);
    });

    spdlog::info("MainWindow initialized");
}

MainWindow::~MainWindow()
{
    saveLayoutState();
}

void MainWindow::setDarkMode(bool dark)
{
    m_darkMode = dark;
    if (dark)
        applyDarkStylesheet();
    else
        applyLightStylesheet();
}

void MainWindow::updateFpsDisplay(double fps)
{
    m_fpsLabel->setText(QString("FPS: %1").arg(fps, 0, 'f', 1));
}

void MainWindow::updateStatusMessage(const QString& msg)
{
    m_statusLabel->setText(msg);
}

// ── Actions ──────────────────────────────────────────────────────

void MainWindow::createActions()
{
    m_actOpenIBom = new QAction(tr("Open iBOM..."), this);
    m_actOpenIBom->setShortcut(QKeySequence::Open);
    m_actOpenIBom->setToolTip(tr("Open an InteractiveHtmlBom HTML file"));
    connect(m_actOpenIBom, &QAction::triggered, this, &MainWindow::onOpenIBomFile);

    m_actToggleCam = new QAction(tr("Start Camera"), this);
    m_actToggleCam->setCheckable(true);
    m_actToggleCam->setShortcut(Qt::Key_C);
    connect(m_actToggleCam, &QAction::triggered, this, &MainWindow::onToggleCamera);

    m_actScreenshot = new QAction(tr("Screenshot"), this);
    m_actScreenshot->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
    connect(m_actScreenshot, &QAction::triggered, this, &MainWindow::onTakeScreenshot);

    m_actFullscreen = new QAction(tr("Fullscreen"), this);
    m_actFullscreen->setCheckable(true);
    m_actFullscreen->setShortcut(Qt::Key_F11);
    connect(m_actFullscreen, &QAction::triggered, this, &MainWindow::onToggleFullscreen);

    m_actCalibrate = new QAction(tr("Calibrate Camera"), this);
    m_actCalibrate->setShortcut(Qt::Key_K);
    connect(m_actCalibrate, &QAction::triggered, this, &MainWindow::onCalibrate);

    m_actInspect = new QAction(tr("Start Inspection"), this);
    m_actInspect->setShortcut(Qt::Key_I);
    connect(m_actInspect, &QAction::triggered, this, &MainWindow::onStartInspection);

    m_actExport = new QAction(tr("Export Report"), this);
    m_actExport->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(m_actExport, &QAction::triggered, this, &MainWindow::onExportReport);

    m_actSettings = new QAction(tr("Settings..."), this);
    m_actSettings->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    connect(m_actSettings, &QAction::triggered, this, &MainWindow::onShowSettings);

    m_actDarkMode = new QAction(tr("Dark Mode"), this);
    m_actDarkMode->setCheckable(true);
    m_actDarkMode->setChecked(m_darkMode);
    connect(m_actDarkMode, &QAction::toggled, this, &MainWindow::setDarkMode);
}

void MainWindow::createMenuBar()
{
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(m_actOpenIBom);
    fileMenu->addSeparator();
    fileMenu->addAction(m_actScreenshot);
    fileMenu->addAction(m_actExport);
    fileMenu->addSeparator();
    fileMenu->addAction(new QAction(tr("&Quit"), this));
    connect(fileMenu->actions().last(), &QAction::triggered, this, &QMainWindow::close);

    auto* cameraMenu = menuBar()->addMenu(tr("&Camera"));
    cameraMenu->addAction(m_actToggleCam);
    cameraMenu->addAction(m_actCalibrate);
    cameraMenu->addSeparator();
    auto* actGenBoard = cameraMenu->addAction(tr("Generate Checkerboard..."));
    actGenBoard->setToolTip(tr("Generate a printable 9x6 checkerboard for camera calibration"));
    connect(actGenBoard, &QAction::triggered, this, &MainWindow::onGenerateCheckerboard);

    auto* inspectMenu = menuBar()->addMenu(tr("&Inspection"));
    inspectMenu->addAction(m_actInspect);

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_actFullscreen);
    viewMenu->addAction(m_actDarkMode);

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    auto* about = helpMenu->addAction(tr("About..."));
    connect(about, &QAction::triggered, this, &MainWindow::onShowAbout);
}

void MainWindow::createToolBar()
{
    m_mainToolBar = addToolBar(tr("Main"));
    m_mainToolBar->setMovable(false);
    m_mainToolBar->setIconSize(QSize(theme::ToolbarIcon, theme::ToolbarIcon));

    m_mainToolBar->addAction(m_actOpenIBom);
    m_mainToolBar->addSeparator();
    m_mainToolBar->addAction(m_actToggleCam);
    m_mainToolBar->addAction(m_actCalibrate);
    m_mainToolBar->addSeparator();
    m_mainToolBar->addAction(m_actInspect);
    m_mainToolBar->addAction(m_actScreenshot);
    m_mainToolBar->addAction(m_actExport);
    m_mainToolBar->addSeparator();
    m_mainToolBar->addAction(m_actSettings);
}

void MainWindow::createStatusBar()
{
    m_fpsLabel    = new QLabel("FPS: --");
    m_statusLabel = new QLabel(tr("Ready"));
    m_gpuLabel    = new QLabel("GPU: --");

    // Add spacing between permanent widgets
    m_fpsLabel->setContentsMargins(theme::StatusPadding, 0, theme::StatusPadding, 0);
    m_gpuLabel->setContentsMargins(theme::StatusPadding, 0, theme::StatusPadding, 0);

    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_gpuLabel);
    statusBar()->addPermanentWidget(m_fpsLabel);
}

void MainWindow::createDockWidgets()
{
    // BOM panel (right)
    m_bomPanel = new BomPanel(this);
    auto* bomDock = new QDockWidget(tr("BOM"), this);
    bomDock->setObjectName("BomDock");
    bomDock->setWidget(m_bomPanel);
    addDockWidget(Qt::RightDockWidgetArea, bomDock);

    // Control panel (right, stacked under BOM)
    m_controlPanel = new ControlPanel(this);
    auto* controlDock = new QDockWidget(tr("Controls"), this);
    controlDock->setObjectName("ControlDock");
    controlDock->setWidget(m_controlPanel);
    controlDock->setMinimumWidth(250);
    controlDock->setMaximumWidth(320);
    addDockWidget(Qt::RightDockWidgetArea, controlDock);
    tabifyDockWidget(bomDock, controlDock);
    controlDock->raise(); // Show Controls tab by default

    // Stats panel (bottom)
    m_statsPanel = new StatsPanel(this);
    auto* statsDock = new QDockWidget(tr("Statistics"), this);
    statsDock->setObjectName("StatsDock");
    statsDock->setWidget(m_statsPanel);
    statsDock->setMaximumHeight(260);
    addDockWidget(Qt::BottomDockWidgetArea, statsDock);

    // Inspection wizard (floating, hidden by default)
    m_inspectionWizard = new InspectionWizard(this);

    // Add dock toggle actions to view menu
    auto* viewMenu = menuBar()->actions().at(3)->menu(); // View menu
    if (viewMenu) {
        viewMenu->addSeparator();
        viewMenu->addAction(bomDock->toggleViewAction());
        viewMenu->addAction(controlDock->toggleViewAction());
        viewMenu->addAction(statsDock->toggleViewAction());
    }
}

// ── Slots ────────────────────────────────────────────────────────

void MainWindow::onOpenIBomFile()
{
    QString path = QFileDialog::getOpenFileName(
        this, tr("Open iBOM File"), QString(),
        tr("HTML Files (*.html *.htm);;All Files (*.*)"));

    if (!path.isEmpty()) {
        emit ibomFileRequested(path);
        updateStatusMessage(tr("Loaded: %1").arg(QFileInfo(path).fileName()));
    }
}

void MainWindow::onToggleCamera()
{
    bool start = m_actToggleCam->isChecked();
    m_actToggleCam->setText(start ? tr("Stop Camera") : tr("Start Camera"));
    emit cameraToggled(start);
}

void MainWindow::onTakeScreenshot()
{
    emit screenshotRequested();
    updateStatusMessage(tr("Screenshot saved"));
}

void MainWindow::onToggleFullscreen()
{
    if (isFullScreen()) {
        showNormal();
        m_actFullscreen->setChecked(false);
    } else {
        showFullScreen();
        m_actFullscreen->setChecked(true);
    }
    emit fullscreenToggled(isFullScreen());
}

void MainWindow::onShowSettings()
{
    SettingsDialog dlg(m_app->config(), this);
    if (dlg.exec() == QDialog::Accepted) {
        spdlog::info("Settings saved");
        emit settingsChanged();
    }
}

void MainWindow::onShowAbout()
{
    QMessageBox::about(this, tr("About PCB Inspector"),
        tr("<h2>PCB Inspector</h2>"
           "<p>Version 1.0</p>"
           "<p>AI-assisted PCB inspection with iBOM overlay.</p>"
           "<p>Built with Qt6, OpenCV, ONNX Runtime.</p>"));
}

void MainWindow::onCalibrate()
{
    emit calibrationRequested();
}

void MainWindow::onStartInspection()
{
    if (m_inspectionWizard) {
        m_inspectionWizard->show();
        m_inspectionWizard->raise();
    }
}

void MainWindow::onExportReport()
{
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export Report"), "inspection_report.pdf",
        tr("PDF Files (*.pdf);;CSV Files (*.csv);;JSON Files (*.json)"));

    if (!path.isEmpty()) {
        spdlog::info("Export report to: {}", path.toStdString());
        updateStatusMessage(tr("Report exported: %1").arg(QFileInfo(path).fileName()));
    }
}

void MainWindow::onGenerateCheckerboard()
{
    // 9x6 inner corners = 10x7 squares
    const int cols = 10;
    const int rows = 7;
    const int squarePx = 100; // pixels per square at 300 DPI ≈ ~8.5mm
    const int margin = squarePx;

    int imgW = cols * squarePx + 2 * margin;
    int imgH = rows * squarePx + 2 * margin;

    QImage img(imgW, imgH, QImage::Format_RGB32);
    img.fill(Qt::white);

    QPainter painter(&img);
    painter.setPen(Qt::NoPen);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            if ((r + c) % 2 == 0) {
                painter.setBrush(Qt::black);
                painter.drawRect(margin + c * squarePx, margin + r * squarePx, squarePx, squarePx);
            }
        }
    }

    // Draw title and info
    painter.setPen(Qt::black);
    painter.setFont(QFont("Segoe UI", 10));
    painter.drawText(margin, margin - 10,
        tr("Checkerboard 9x6 inner corners — Print at 100% scale (no fit-to-page)"));
    painter.end();

    // Ask user: Save as image or Print directly
    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Generate Checkerboard"));
    msgBox.setText(tr("Checkerboard pattern generated (9x6 inner corners).\n"
                      "How would you like to output it?"));
    auto* btnSave  = msgBox.addButton(tr("Save as Image..."), QMessageBox::ActionRole);
    auto* btnPrint = msgBox.addButton(tr("Print..."), QMessageBox::ActionRole);
    msgBox.addButton(QMessageBox::Cancel);
    msgBox.exec();

    if (msgBox.clickedButton() == btnSave) {
        QString path = QFileDialog::getSaveFileName(
            this, tr("Save Checkerboard"), "checkerboard_9x6.png",
            tr("PNG Image (*.png);;JPEG Image (*.jpg);;BMP Image (*.bmp)"));
        if (!path.isEmpty()) {
            // Save at 300 DPI for printing
            QImage hiRes(imgW * 3, imgH * 3, QImage::Format_RGB32);
            hiRes.fill(Qt::white);
            QPainter hp(&hiRes);
            hp.setPen(Qt::NoPen);
            int sq3 = squarePx * 3;
            int mg3 = margin * 3;
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    if ((r + c) % 2 == 0) {
                        hp.setBrush(Qt::black);
                        hp.drawRect(mg3 + c * sq3, mg3 + r * sq3, sq3, sq3);
                    }
                }
            }
            hp.setPen(Qt::black);
            hp.setFont(QFont("Segoe UI", 24));
            hp.drawText(mg3, mg3 - 20,
                tr("Checkerboard 9x6 inner corners — Print at 100%% scale"));
            hp.end();
            hiRes.setDotsPerMeterX(11811); // 300 DPI
            hiRes.setDotsPerMeterY(11811);
            hiRes.save(path);
            spdlog::info("Checkerboard saved to: {}", path.toStdString());
            updateStatusMessage(tr("Checkerboard saved: %1").arg(QFileInfo(path).fileName()));
        }
    } else if (msgBox.clickedButton() == btnPrint) {
        QPrinter printer(QPrinter::HighResolution);
        printer.setPageSize(QPageSize::A4);
        QPrintDialog dlg(&printer, this);
        if (dlg.exec() == QDialog::Accepted) {
            QPainter pp(&printer);
            // Scale to fit page with correct aspect ratio
            QRect pageRect = printer.pageRect(QPrinter::DevicePixel).toRect();
            double scale = std::min(
                static_cast<double>(pageRect.width()) / imgW,
                static_cast<double>(pageRect.height()) / imgH);
            pp.scale(scale, scale);
            pp.drawImage(0, 0, img);
            pp.end();
            spdlog::info("Checkerboard printed");
            updateStatusMessage(tr("Checkerboard printed"));
        }
    }
}

// ── Protected ────────────────────────────────────────────────────

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveLayoutState();
    event->accept();
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        if (m_cameraFullscreen) {
            toggleCameraFullscreen(false);
            return;
        }
        if (isFullScreen()) {
            showNormal();
            m_actFullscreen->setChecked(false);
        }
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::toggleCameraFullscreen(bool enter)
{
    if (enter == m_cameraFullscreen) return;
    m_cameraFullscreen = enter;

    // Hide/show all dock widgets
    const auto docks = findChildren<QDockWidget*>();
    for (auto* dock : docks)
        dock->setVisible(!enter);

    // Hide/show chrome
    menuBar()->setVisible(!enter);
    m_mainToolBar->setVisible(!enter);
    statusBar()->setVisible(!enter);

    if (enter)
        showFullScreen();
    else
        showNormal();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        for (const auto& url : event->mimeData()->urls()) {
            if (url.toLocalFile().endsWith(".html", Qt::CaseInsensitive)) {
                event->acceptProposedAction();
                return;
            }
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    for (const auto& url : event->mimeData()->urls()) {
        QString path = url.toLocalFile();
        if (path.endsWith(".html", Qt::CaseInsensitive)) {
            emit ibomFileRequested(path);
            updateStatusMessage(tr("Loaded: %1").arg(QFileInfo(path).fileName()));
            break;
        }
    }
}

// ── Layout State ─────────────────────────────────────────────────

void MainWindow::restoreLayoutState()
{
    QSettings settings("PCBInspector", "MainWindow");
    // Only restore if we have saved state; otherwise use defaults
    if (settings.contains("geometry")) {
        restoreGeometry(settings.value("geometry").toByteArray());
        restoreState(settings.value("windowState").toByteArray());
    } else {
        // Default size: 80% of screen
        if (auto* screen = QApplication::primaryScreen()) {
            QRect available = screen->availableGeometry();
            int w = available.width() * 80 / 100;
            int h = available.height() * 80 / 100;
            resize(w, h);
            move(available.center() - QPoint(w / 2, h / 2));
        }
    }
}

void MainWindow::saveLayoutState()
{
    QSettings settings("PCBInspector", "MainWindow");
    settings.setValue("geometry",    saveGeometry());
    settings.setValue("windowState", saveState());
}

// ── Stylesheets ──────────────────────────────────────────────────

void MainWindow::applyDarkStylesheet()
{
    setStyleSheet(R"(
        * {
            font-family: "Segoe UI", "Inter", sans-serif;
            font-size: 12px;
        }

        /* ── Main Window ── */
        QMainWindow { background: #111118; }

        /* ── Menu Bar ── */
        QMenuBar {
            background: #16161e; color: #c0c8e8;
            font-size: 13px; padding: 2px 4px;
            border-bottom: 1px solid #1c1c2e;
        }
        QMenuBar::item {
            padding: 6px 14px; border-radius: 4px;
            margin: 1px 0;
        }
        QMenuBar::item:selected { background: #252540; color: #e0e6ff; }
        QMenu {
            background: #1a1a2e; color: #c0c8e8;
            border: 1px solid #2e2e4a; border-radius: 8px;
            padding: 6px 4px;
        }
        QMenu::item {
            padding: 8px 28px 8px 16px; border-radius: 4px;
            margin: 1px 4px;
        }
        QMenu::item:selected { background: #2e2e50; color: #fff; }
        QMenu::separator { height: 1px; background: #2a2a42; margin: 4px 12px; }

        /* ── Toolbar ── */
        QToolBar {
            background: #16161e; border: none;
            border-bottom: 1px solid #1c1c2e;
            spacing: 4px; padding: 4px 10px;
        }
        QToolBar QToolButton {
            color: #8892b8; font-size: 12px;
            padding: 6px 12px; border-radius: 5px; border: none;
            font-weight: 500;
        }
        QToolBar QToolButton:hover {
            background: #22223a; color: #c8d0f0;
        }
        QToolBar QToolButton:pressed { background: #2a2a48; }
        QToolBar QToolButton:checked {
            background: rgba(100, 140, 255, 0.15);
            color: #7aa2f7;
        }
        QToolBar::separator {
            width: 1px; background: #2a2a42;
            margin: 6px 4px;
        }

        /* ── Status Bar ── */
        QStatusBar {
            background: #0e0e14; color: #5a6282;
            font-size: 11px; padding: 4px 12px;
            border-top: 1px solid #1c1c2e;
        }
        QStatusBar QLabel { color: #5a6282; font-size: 11px; }

        /* ── Dock Widgets ── */
        QDockWidget {
            color: #c0c8e8; font-size: 12px;
            titlebar-close-icon: none;
        }
        QDockWidget::title {
            background: #16161e; padding: 10px 14px;
            border-bottom: 1px solid #1c1c2e;
            font-weight: 600; font-size: 12px;
        }
        QDockWidget::close-button, QDockWidget::float-button {
            background: transparent;
            border: none; padding: 2px;
            icon-size: 12px;
        }
        QDockWidget::close-button:hover, QDockWidget::float-button:hover {
            background: #2a2a48; border-radius: 3px;
        }
        /* Force dock content panels to dark background */
        QDockWidget > QWidget {
            background: #111118;
        }

        /* ── Scroll Areas (prevent system white bg) ── */
        QScrollArea {
            background: #111118; border: none;
        }
        QScrollArea > QWidget > QWidget {
            background: #111118;
        }

        /* ── Labels ── */
        QLabel { color: #9098b8; font-size: 12px; }

        /* ── Buttons ── */
        QPushButton {
            background: #1e1e34; color: #c0c8e8;
            border: 1px solid #2a2a48; border-radius: 6px;
            padding: 8px 18px; font-size: 12px;
            font-weight: 500;
        }
        QPushButton:hover {
            background: #282848; border-color: #3a3a60;
            color: #e0e6ff;
        }
        QPushButton:pressed { background: #32325a; }
        QPushButton:disabled {
            color: #3e3e58; background: #141420;
            border-color: #1e1e30;
        }

        /* ── Inputs ── */
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
            background: #0e0e18; color: #b0b8d8;
            border: 1px solid #22223a; border-radius: 6px;
            padding: 6px 10px; font-size: 12px;
            selection-background-color: #3a3a68;
            selection-color: #fff;
        }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
            border-color: #6488e8;
        }
        QComboBox::drop-down {
            border: none; width: 28px;
            subcontrol-position: right center;
        }
        QComboBox::down-arrow {
            width: 10px; height: 10px;
        }
        QComboBox QAbstractItemView {
            background: #1a1a2e; color: #c0c8e8;
            border: 1px solid #2e2e4a; border-radius: 6px;
            selection-background-color: #2e2e50;
            padding: 4px;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button,
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            background: transparent;
            border: none;
            width: 18px;
        }
        QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
        QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
            background: #2a2a48;
        }

        /* ── Tables ── */
        QTableWidget, QTreeWidget, QListWidget {
            background: #0e0e18; color: #b0b8d8;
            border: 1px solid #1c1c2e; border-radius: 6px;
            gridline-color: #1a1a2a;
            alternate-background-color: #111120;
            font-size: 12px;
            outline: none;
        }
        QTableWidget::item:selected, QTreeWidget::item:selected {
            background: rgba(100, 140, 255, 0.12);
            color: #e0e8ff;
        }
        QTableWidget::item:hover {
            background: rgba(100, 140, 255, 0.06);
        }
        QHeaderView::section {
            background: #14141e; color: #7882a8;
            border: none; border-bottom: 1px solid #22223a;
            padding: 8px 10px; font-size: 11px;
            font-weight: 600; text-transform: uppercase;
        }

        /* ── Scrollbars ── */
        QScrollBar:vertical {
            background: transparent; width: 8px; margin: 2px 1px;
        }
        QScrollBar::handle:vertical {
            background: #2a2a48; border-radius: 4px; min-height: 30px;
        }
        QScrollBar::handle:vertical:hover { background: #3a3a60; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
        QScrollBar:horizontal {
            background: transparent; height: 8px; margin: 1px 2px;
        }
        QScrollBar::handle:horizontal {
            background: #2a2a48; border-radius: 4px; min-width: 30px;
        }
        QScrollBar::handle:horizontal:hover { background: #3a3a60; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }

        /* ── Group Boxes ── */
        QGroupBox {
            color: #6488e8; font-size: 11px; font-weight: 600;
            text-transform: uppercase; letter-spacing: 0.5px;
            border: none; border-top: 2px solid #1e1e34;
            border-radius: 0px;
            margin-top: 18px; padding: 18px 8px 10px 8px;
            background: transparent;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            left: 2px; top: 0px;
            padding: 0 4px;
            background: transparent;
        }

        /* ── Checkboxes ── */
        QCheckBox { color: #9098b8; spacing: 8px; font-size: 12px; }
        QCheckBox::indicator {
            width: 18px; height: 18px; border-radius: 4px;
            border: 2px solid #2a2a48; background: #0e0e18;
        }
        QCheckBox::indicator:checked {
            background: #6488e8; border-color: #6488e8;
        }
        QCheckBox::indicator:hover { border-color: #5070c0; }
        QCheckBox:hover { color: #c0c8e8; }

        /* ── Sliders ── */
        QSlider::groove:horizontal {
            background: #1a1a2e; height: 6px; border-radius: 3px;
        }
        QSlider::handle:horizontal {
            background: #6488e8; width: 16px; height: 16px;
            margin: -5px 0; border-radius: 8px;
            border: 2px solid #4a68c0;
        }
        QSlider::handle:horizontal:hover {
            background: #7a9cf0; border-color: #5a78d0;
        }
        QSlider::sub-page:horizontal {
            background: #6488e8; border-radius: 3px;
        }

        /* ── Tab Bars (for dock tabs) ── */
        QTabWidget::pane {
            border: 1px solid #1c1c2e;
            background: #111118;
        }
        QTabBar {
            background: #16161e;
        }
        QTabBar::tab {
            background: #16161e; color: #5a6282;
            padding: 10px 24px; border: none;
            border-bottom: 2px solid transparent;
            font-size: 12px; font-weight: 500;
        }
        QTabBar::tab:selected {
            color: #6488e8;
            border-bottom: 2px solid #6488e8;
        }
        QTabBar::tab:hover:!selected { color: #9098b8; }

        /* ── Progress Bar ── */
        QProgressBar {
            background: #1a1a2e; border: none; border-radius: 4px;
            height: 8px; text-align: center;
            font-size: 0px;
        }
        QProgressBar::chunk {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 #4a68c8, stop:1 #6488e8);
            border-radius: 4px;
        }

        /* ── Splitters ── */
        QSplitter::handle { background: #1a1a2e; width: 1px; }

        /* ── Tooltips ── */
        QToolTip {
            background: #1e1e36; color: #c0c8e8;
            border: 1px solid #2e2e4a; border-radius: 6px;
            padding: 6px 10px; font-size: 11px;
        }

        /* ── Dialogs ── */
        QDialog {
            background: #111118;
        }
    )");
}

void MainWindow::applyLightStylesheet()
{
    setStyleSheet(R"(
        * {
            font-family: "Segoe UI", "Inter", sans-serif;
            font-size: 12px;
        }
        QMainWindow { background: #f5f6fa; }
        QMenuBar {
            background: #ebedf5; color: #3a4060;
            font-size: 13px; padding: 2px 4px;
            border-bottom: 1px solid #d8dae4;
        }
        QMenuBar::item { padding: 6px 14px; border-radius: 4px; margin: 1px 0; }
        QMenuBar::item:selected { background: #d8dce8; color: #2a3050; }
        QMenu {
            background: #f5f6fa; color: #3a4060;
            border: 1px solid #d0d4e0; border-radius: 8px; padding: 6px 4px;
        }
        QMenu::item { padding: 8px 28px 8px 16px; border-radius: 4px; margin: 1px 4px; }
        QMenu::item:selected { background: #dce0ec; }
        QMenu::separator { height: 1px; background: #d0d4e0; margin: 4px 12px; }
        QToolBar {
            background: #ebedf5; border: none;
            border-bottom: 1px solid #d8dae4;
            spacing: 4px; padding: 4px 10px;
        }
        QToolBar QToolButton {
            color: #5a6282; font-size: 12px; font-weight: 500;
            padding: 6px 12px; border-radius: 5px; border: none;
        }
        QToolBar QToolButton:hover { background: #dce0ec; color: #2a3050; }
        QToolBar QToolButton:checked { background: rgba(68, 102, 204, 0.12); color: #4466cc; }
        QToolBar::separator { width: 1px; background: #d0d4e0; margin: 6px 4px; }
        QStatusBar {
            background: #e8eaf2; color: #6a7090;
            font-size: 11px; padding: 4px 12px;
            border-top: 1px solid #d8dae4;
        }
        QStatusBar QLabel { color: #6a7090; font-size: 11px; }
        QDockWidget { color: #3a4060; }
        QDockWidget::title {
            background: #ebedf5; padding: 10px 14px;
            border-bottom: 1px solid #d8dae4; font-weight: 600;
        }
        QDockWidget::close-button, QDockWidget::float-button {
            background: transparent; border: none; padding: 2px;
        }
        QDockWidget::close-button:hover, QDockWidget::float-button:hover {
            background: #d4d8e8; border-radius: 3px;
        }
        QDockWidget > QWidget {
            background: #f5f6fa;
        }
        QScrollArea {
            background: #f5f6fa; border: none;
        }
        QScrollArea > QWidget > QWidget {
            background: #f5f6fa;
        }
        QLabel { color: #4a5272; font-size: 12px; }
        QPushButton {
            background: #e4e8f2; color: #3a4060;
            border: 1px solid #c8ccd8; border-radius: 6px;
            padding: 8px 18px; font-size: 12px; font-weight: 500;
        }
        QPushButton:hover { background: #d4d8e8; border-color: #b0b4c8; color: #2a3050; }
        QPushButton:pressed { background: #c8cce0; }
        QPushButton:disabled {
            color: #a0a4b0; background: #e8eaf2;
            border-color: #d4d8e0;
        }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
            background: #ffffff; color: #3a4060;
            border: 1px solid #c8ccd8; border-radius: 6px;
            padding: 6px 10px; font-size: 12px;
        }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
            border-color: #4466cc;
        }
        QComboBox::drop-down { border: none; width: 28px; }
        QComboBox QAbstractItemView {
            background: #fff; color: #3a4060;
            border: 1px solid #d0d4e0; selection-background-color: #dce0ec;
            padding: 4px;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button,
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            background: transparent; border: none; width: 18px;
        }
        QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
        QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
            background: #dce0ec;
        }
        QTableWidget, QTreeWidget, QListWidget {
            background: #ffffff; color: #3a4060;
            border: 1px solid #d8dae4; border-radius: 6px;
            alternate-background-color: #f8f9fc; outline: none;
        }
        QTableWidget::item:selected { background: rgba(68, 102, 204, 0.1); }
        QTableWidget::item:hover { background: rgba(68, 102, 204, 0.05); }
        QHeaderView::section {
            background: #ebedf5; color: #5a6282;
            border: none; border-bottom: 1px solid #d0d4e0;
            padding: 8px 10px; font-size: 11px; font-weight: 600;
        }
        QScrollBar:vertical { background: transparent; width: 8px; margin: 2px 1px; }
        QScrollBar::handle:vertical { background: #c0c4d4; border-radius: 4px; min-height: 30px; }
        QScrollBar::handle:vertical:hover { background: #a8acbc; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
        QScrollBar:horizontal { background: transparent; height: 8px; margin: 1px 2px; }
        QScrollBar::handle:horizontal { background: #c0c4d4; border-radius: 4px; min-width: 30px; }
        QScrollBar::handle:horizontal:hover { background: #a8acbc; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }
        QGroupBox {
            color: #4466cc; font-size: 11px; font-weight: 600;
            text-transform: uppercase;
            border: none; border-top: 2px solid #d8dae4;
            border-radius: 0; margin-top: 18px;
            padding: 18px 8px 10px 8px; background: transparent;
        }
        QGroupBox::title {
            subcontrol-origin: margin; subcontrol-position: top left;
            left: 2px; top: 0px; padding: 0 4px;
        }
        QCheckBox { color: #4a5272; spacing: 8px; }
        QCheckBox::indicator {
            width: 18px; height: 18px; border-radius: 4px;
            border: 2px solid #c0c4d4; background: #ffffff;
        }
        QCheckBox::indicator:checked { background: #4466cc; border-color: #4466cc; }
        QCheckBox::indicator:hover { border-color: #4466cc; }
        QSlider::groove:horizontal { background: #d8dae4; height: 6px; border-radius: 3px; }
        QSlider::handle:horizontal {
            background: #4466cc; width: 16px; height: 16px;
            margin: -5px 0; border-radius: 8px; border: 2px solid #3354b8;
        }
        QSlider::sub-page:horizontal { background: #4466cc; border-radius: 3px; }
        QTabWidget::pane {
            border: 1px solid #d8dae4; background: #f5f6fa;
        }
        QTabBar {
            background: #ebedf5;
        }
        QTabBar::tab {
            background: #ebedf5; color: #6a7090;
            padding: 10px 24px; border: none;
            border-bottom: 2px solid transparent; font-size: 12px; font-weight: 500;
        }
        QTabBar::tab:selected { color: #4466cc; border-bottom: 2px solid #4466cc; }
        QTabBar::tab:hover:!selected { color: #3a4060; }
        QProgressBar {
            background: #e0e2ec; border: none; border-radius: 4px;
            height: 8px; font-size: 0px;
        }
        QProgressBar::chunk {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 #3354b8, stop:1 #4466cc);
            border-radius: 4px;
        }
        QSplitter::handle { background: #d8dae4; width: 1px; }
        QToolTip {
            background: #f5f6fa; color: #3a4060;
            border: 1px solid #d0d4e0; border-radius: 6px;
            padding: 6px 10px; font-size: 11px;
        }
        QDialog {
            background: #f5f6fa;
        }
    )");
}

} // namespace ibom::gui
