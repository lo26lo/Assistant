#pragma once

#include <QMainWindow>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QMenuBar>
#include <QTimer>
#include <QLabel>
#include <memory>

namespace ibom {
class Application;
}

namespace ibom::gui {

class CameraView;
class BomPanel;
class ControlPanel;
class InspectionWizard;
class StatsPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(Application* app, QWidget* parent = nullptr);
    ~MainWindow() override;

    CameraView*       cameraView()       { return m_cameraView; }
    BomPanel*         bomPanel()         { return m_bomPanel; }
    ControlPanel*     controlPanel()     { return m_controlPanel; }
    InspectionWizard* inspectionWizard() { return m_inspectionWizard; }
    StatsPanel*       statsPanel()       { return m_statsPanel; }

    void setDarkMode(bool dark);
    void updateFpsDisplay(double fps);
    void updateStatusMessage(const QString& msg);

signals:
    void ibomFileRequested(const QString& path);
    void cameraToggled(bool start);
    void screenshotRequested();
    void calibrationRequested();
    void fullscreenToggled(bool fs);
    void settingsChanged();

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void onOpenIBomFile();
    void onToggleCamera();
    void onTakeScreenshot();
    void onToggleFullscreen();
    void onShowSettings();
    void onShowAbout();
    void onCalibrate();
    void onStartInspection();
    void onExportReport();
    void onGenerateCheckerboard();
    void onOpenCalibrationPDF();
    void onShowHelp(int tab = 0);

private:
    void createActions();
    void createMenuBar();
    void createToolBar();
    void createStatusBar();
    void createDockWidgets();
    void restoreLayoutState();
    void saveLayoutState();
    void applyDarkStylesheet();
    void applyLightStylesheet();
    void toggleCameraFullscreen(bool enter);

    Application* m_app = nullptr;

    // Main views
    CameraView*       m_cameraView       = nullptr;
    BomPanel*         m_bomPanel         = nullptr;
    ControlPanel*     m_controlPanel     = nullptr;
    InspectionWizard* m_inspectionWizard = nullptr;
    StatsPanel*       m_statsPanel       = nullptr;

    // Toolbar & actions
    QToolBar* m_mainToolBar   = nullptr;
    QAction*  m_actOpenIBom   = nullptr;
    QAction*  m_actToggleCam  = nullptr;
    QAction*  m_actScreenshot = nullptr;
    QAction*  m_actFullscreen = nullptr;
    QAction*  m_actCalibrate  = nullptr;
    QAction*  m_actInspect    = nullptr;
    QAction*  m_actExport     = nullptr;
    QAction*  m_actSettings   = nullptr;
    QAction*  m_actDarkMode   = nullptr;

    // Status bar widgets
    QLabel* m_fpsLabel    = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_gpuLabel    = nullptr;

    bool m_darkMode = true;
    bool m_cameraFullscreen = false;
};

} // namespace ibom::gui
