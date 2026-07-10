#pragma once

#include <QMainWindow>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QMenuBar>
#include <QMenu>
#include <QTimer>
#include <QLabel>
#include <QDockWidget>
#include <memory>

class QStackedWidget;
class QComboBox;
class QDialog;

namespace ibom {
class Application;
}

namespace ibom::gui {

class CameraView;
class PointCloudView;
class ViewModeBar;
class BomPanel;
class ControlPanel;
class InspectionWizard;
class InspectionPanel;
class StatsPanel;
class DatasetPanel;
class BoardMinimap;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(Application* app, QWidget* parent = nullptr);
    ~MainWindow() override;

    CameraView*       cameraView()       { return m_cameraView; }
    PointCloudView*   pointCloudView()   { return m_pointCloudView; }
    BomPanel*         bomPanel()         { return m_bomPanel; }
    ControlPanel*     controlPanel()     { return m_controlPanel; }
    InspectionWizard* inspectionWizard() { return m_inspectionWizard; }
    InspectionPanel*  inspectionPanel()  { return m_inspectionPanel; }
    StatsPanel*       statsPanel()       { return m_statsPanel; }
    DatasetPanel*     datasetPanel()     { return m_datasetPanel; }
    BoardMinimap*     boardMinimap()     { return m_boardMinimap; }

    /// Raise the BOM dock to the front (e.g. when starting a component-based
    /// alignment flow, so the user doesn't have to find the tab manually).
    void showBomPanel();

    void setDarkMode(bool dark);
    /// Reflect the persisted verbose-logging state in the Dev menu checkmark
    /// (without re-emitting the toggle). Called by Application at startup.
    void setVerboseLoggingChecked(bool on);
    void updateFpsDisplay(double fps);
    void updateStatusMessage(const QString& msg);
    void updateAiStatus(bool ready, const QString& message);
    /// Rebuild the File → Open Recent submenu (most recent first).
    void setRecentFiles(const QStringList& files);
    /// Enable the View → Depth View toggle (only meaningful for RealSense).
    /// Unchecks and disables it when the depth stream is unavailable.
    void setDepthViewAvailable(bool available);
    /// Enable the View → 3D Point Cloud toggle (RealSense depth only).
    /// Leaves 3D mode and disables it when depth becomes unavailable.
    void setPointCloudAvailable(bool available);
    /// True while the central view is showing the 3D point cloud.
    bool pointCloudActive() const { return m_pointCloudActive; }

    /// Update the profile combo without triggering the change signal.
    void setActiveProfile(int idx);

signals:
    void ibomFileRequested(const QString& path);
    void cameraToggled(bool start);
    void screenshotRequested();
    void calibrationRequested();
    void fullscreenToggled(bool fs);
    void settingsChanged();
    /// Toggle the live view between color and colorized depth (RealSense only).
    void depthViewToggled(bool depth);
    /// Toggle the central view between the 2D camera and the 3D point cloud.
    void pointCloudToggled(bool enabled);
    /// Open the RealSense sensor-controls panel (routed from the Settings
    /// dialog; the live camera lives in Application).
    void realSenseControlsRequested();
    /// User selected a different camera profile in the toolbar combo.
    void cameraProfileChangeRequested(int idx);
    /// Arm microscope 1-point anchoring on the currently selected component.
    void componentAnchorRequested();
    /// Dev menu: measure current FOV & scale.
    void fovMeasureRequested();
    /// Dev menu: open the live calibration monitor pop-up.
    void calibrationMonitorRequested();
    /// Dev menu: toggle verbose debug logging (every spdlog::debug → log file).
    void verboseLoggingToggled(bool on);
    /// Dev menu: dump the full current config + runtime state to the log.
    void dumpStateRequested();
    /// Dev menu: trigger an on-demand AI component-level re-anchor.
    void componentReanchorRequested();

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
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
    void repositionViewModeBar();

    Application* m_app = nullptr;

    // Main views
    QStackedWidget*   m_centralStack     = nullptr;
    CameraView*       m_cameraView       = nullptr;
    PointCloudView*   m_pointCloudView   = nullptr;
    ViewModeBar*      m_viewModeBar      = nullptr;
    bool              m_pointCloudActive = false;
    BomPanel*         m_bomPanel         = nullptr;
    QDockWidget*      m_bomDock          = nullptr;
    ControlPanel*     m_controlPanel     = nullptr;
    InspectionWizard* m_inspectionWizard = nullptr;
    InspectionPanel*  m_inspectionPanel  = nullptr;
    StatsPanel*       m_statsPanel       = nullptr;
    DatasetPanel*     m_datasetPanel     = nullptr;
    BoardMinimap*     m_boardMinimap     = nullptr;

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
    QAction*  m_actAnchor     = nullptr;
    QAction*  m_actVerboseLog = nullptr;
    QAction*  m_actExport     = nullptr;
    QAction*  m_actSettings   = nullptr;
    QAction*  m_actDarkMode   = nullptr;
    QAction*  m_actDepthView  = nullptr;
    QAction*  m_actPointCloud = nullptr;
    QComboBox* m_profileCombo = nullptr;

    // Status bar widgets
    QLabel* m_fpsLabel    = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_gpuLabel    = nullptr;
    QLabel* m_aiLabel     = nullptr;

    bool m_darkMode = true;
    bool m_cameraFullscreen = false;

    // Re-anchor debug view (Dev menu): a lazily-created window that displays
    // the newest annotated frame the re-anchor workers drop under
    // dataDir()/debug — the same images used to diagnose alignment in the
    // field, live and in-app instead of hunting for JPGs on disk.
    void toggleReanchorDebugView(bool on);
    void refreshReanchorDebugView();
    QDialog* m_reanchorDbgDialog = nullptr;
    QLabel*  m_reanchorDbgLabel  = nullptr;
    QTimer*  m_reanchorDbgTimer  = nullptr;
    QAction* m_actReanchorDbg    = nullptr;
    qint64   m_reanchorDbgMtime  = 0;
};

} // namespace ibom::gui
