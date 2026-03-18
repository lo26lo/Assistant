#include "MainWindow.h"
#include "CameraView.h"
#include "BomPanel.h"
#include "ControlPanel.h"
#include "InspectionWizard.h"
#include "StatsPanel.h"
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
    m_mainToolBar->setIconSize(QSize(24, 24));

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
    addDockWidget(Qt::RightDockWidgetArea, controlDock);
    tabifyDockWidget(bomDock, controlDock);

    // Stats panel (bottom)
    m_statsPanel = new StatsPanel(this);
    auto* statsDock = new QDockWidget(tr("Statistics"), this);
    statsDock->setObjectName("StatsDock");
    statsDock->setWidget(m_statsPanel);
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
    // TODO: Open a settings dialog
    spdlog::info("Settings dialog requested");
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
    updateStatusMessage(tr("Calibrating... Show checkerboard to camera"));
    spdlog::info("Calibration requested");
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

// ── Protected ────────────────────────────────────────────────────

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveLayoutState();
    event->accept();
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && isFullScreen()) {
        showNormal();
        m_actFullscreen->setChecked(false);
    }
    QMainWindow::keyPressEvent(event);
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
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("windowState").toByteArray());
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
        QMainWindow { background: #1e1e2e; }
        QMenuBar { background: #181825; color: #cdd6f4; }
        QMenuBar::item:selected { background: #313244; }
        QMenu { background: #1e1e2e; color: #cdd6f4; border: 1px solid #45475a; }
        QMenu::item:selected { background: #313244; }
        QToolBar { background: #181825; border: none; spacing: 4px; }
        QStatusBar { background: #181825; color: #a6adc8; }
        QDockWidget { color: #cdd6f4; }
        QDockWidget::title { background: #181825; padding: 4px; }
        QLabel { color: #cdd6f4; }
        QPushButton {
            background: #313244; color: #cdd6f4;
            border: 1px solid #45475a; border-radius: 4px;
            padding: 6px 12px;
        }
        QPushButton:hover { background: #45475a; }
        QPushButton:pressed { background: #585b70; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
            background: #313244; color: #cdd6f4;
            border: 1px solid #45475a; border-radius: 4px;
            padding: 4px;
        }
        QTableWidget, QTreeWidget, QListWidget {
            background: #1e1e2e; color: #cdd6f4;
            border: 1px solid #45475a;
            gridline-color: #313244;
            alternate-background-color: #181825;
        }
        QHeaderView::section {
            background: #181825; color: #cdd6f4;
            border: 1px solid #313244; padding: 4px;
        }
        QScrollBar:vertical {
            background: #1e1e2e; width: 8px;
        }
        QScrollBar::handle:vertical {
            background: #45475a; border-radius: 4px;
        }
        QGroupBox {
            color: #cdd6f4;
            border: 1px solid #45475a; border-radius: 4px;
            margin-top: 8px; padding-top: 16px;
        }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; }
        QSlider::groove:horizontal {
            background: #313244; height: 4px; border-radius: 2px;
        }
        QSlider::handle:horizontal {
            background: #89b4fa; width: 14px; height: 14px;
            margin: -5px 0; border-radius: 7px;
        }
        QTabWidget::pane { border: 1px solid #45475a; }
        QTabBar::tab {
            background: #181825; color: #a6adc8;
            padding: 6px 16px; border: 1px solid #45475a;
        }
        QTabBar::tab:selected { background: #313244; color: #cdd6f4; }
    )");
}

void MainWindow::applyLightStylesheet()
{
    setStyleSheet(R"(
        QMainWindow { background: #eff1f5; }
        QMenuBar { background: #e6e9ef; color: #4c4f69; }
        QMenuBar::item:selected { background: #ccd0da; }
        QMenu { background: #eff1f5; color: #4c4f69; border: 1px solid #9ca0b0; }
        QMenu::item:selected { background: #ccd0da; }
        QToolBar { background: #e6e9ef; border: none; spacing: 4px; }
        QStatusBar { background: #e6e9ef; color: #6c6f85; }
        QDockWidget { color: #4c4f69; }
        QDockWidget::title { background: #e6e9ef; padding: 4px; }
        QLabel { color: #4c4f69; }
        QPushButton {
            background: #ccd0da; color: #4c4f69;
            border: 1px solid #9ca0b0; border-radius: 4px;
            padding: 6px 12px;
        }
        QPushButton:hover { background: #bcc0cc; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
            background: #ccd0da; color: #4c4f69;
            border: 1px solid #9ca0b0; border-radius: 4px;
            padding: 4px;
        }
        QTableWidget, QTreeWidget, QListWidget {
            background: #eff1f5; color: #4c4f69;
            border: 1px solid #9ca0b0;
            alternate-background-color: #e6e9ef;
        }
        QHeaderView::section {
            background: #e6e9ef; color: #4c4f69;
            border: 1px solid #ccd0da; padding: 4px;
        }
        QGroupBox {
            color: #4c4f69;
            border: 1px solid #9ca0b0; border-radius: 4px;
            margin-top: 8px; padding-top: 16px;
        }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; }
        QSlider::groove:horizontal {
            background: #ccd0da; height: 4px; border-radius: 2px;
        }
        QSlider::handle:horizontal {
            background: #1e66f5; width: 14px; height: 14px;
            margin: -5px 0; border-radius: 7px;
        }
    )");
}

} // namespace ibom::gui
