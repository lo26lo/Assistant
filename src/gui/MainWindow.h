#pragma once

#include <QMainWindow>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QMenuBar>
#include <QMenu>
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
class InspectionPanel;
class StatsPanel;
class DatasetPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(Application* app, QWidget* parent = nullptr);
    ~MainWindow() override;

    CameraView*       cameraView()       { return m_cameraView; }
    BomPanel*         bomPanel()         { return m_bomPanel; }
    ControlPanel*     controlPanel()     { return m_controlPanel; }
    InspectionWizard* inspectionWizard() { return m_inspectionWizard; }
    InspectionPanel*  inspectionPanel()  { return m_inspectionPanel; }
    StatsPanel*       statsPanel()       { return m_statsPanel; }
    DatasetPanel*     datasetPanel()     { return m_datasetPanel; }

    void setDarkMode(bool dark);
    void updateFpsDisplay(double fps);
    void updateStatusMessage(const QString& msg);
    void updateAiStatus(bool ready, const QString& message);
    /// Rebuild the File → Open Recent submenu (most recent first).
    void setRecentFiles(const QStringList& files);

signals:
    void ibomFileRequested(const QString& path);
    void cameraToggled(bool start);
    void screenshotRequested();
    void calibrationRequested();
    void fullscreenToggled(bool fs);
    void settingsChanged();
    /// Open the RealSense sensor-controls panel (routed from the Settings
    /// dialog; the live camera lives in Application).
    void realSenseControlsRequested();

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
    InspectionPanel*  m_inspectionPanel  = nullptr;
    StatsPanel*       m_statsPanel       = nullptr;
    DatasetPanel*     m_datasetPanel     = nullptr;

    // Menus
    QMenu* m_recentMenu = nullptr;

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
    QLabel* m_aiLabel     = nullptr;

    bool m_darkMode = true;
    bool m_cameraFullscreen = false;
};

} // namespace ibom::gui
