#pragma once

#include <QWidget>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QList>
#include <QStringList>
#include <QCheckBox>
#include <QGroupBox>
#include <QPushButton>
#include <QLabel>

namespace ibom::gui {

class ControlPanel : public QWidget {
    Q_OBJECT

public:
    explicit ControlPanel(QWidget* parent = nullptr);
    ~ControlPanel() override = default;

    // Getters
    float  overlayOpacity()    const;
    float  confidenceThreshold() const;
    bool   showPads()          const;
    bool   showSilkscreen()    const;
    bool   showFabrication()   const;
    bool   showHeatmap()       const;
    bool   autoInspect()       const;
    int    cameraIndex()       const;
    int    cameraWidth()       const;
    int    cameraHeight()      const;
    int    cameraFps()         const;

    /// Populate the device combo. @p labels are shown to the user; @p indices
    /// are the real V4L2 /dev/video numbers stored as item data and returned by
    /// cameraIndex(). @p currentIndex selects the matching device if present.
    void setCameraDevices(const QStringList& labels, const QList<int>& indices,
                          int currentIndex = -1);
    void setConfidenceThreshold(float conf);
    /// Reflect the persisted hybrid drift-correction flag in the checkbox.
    void setHybridMode(bool enabled);
    bool hybridMode() const;
    /// Switch UI between USB-microscope and RealSense mode.
    /// Disables/relabels the calibration button when RealSense is active
    /// (factory intrinsics are embedded in the SDK — no checkerboard needed).
    void setCameraBackendUI(bool isRealSense);

signals:
    void overlayOpacityChanged(float opacity);
    void confidenceChanged(float conf);
    void showPadsChanged(bool show);
    void showSilkscreenChanged(bool show);
    void showFabricationChanged(bool show);
    void showHeatmapChanged(bool show);
    void autoInspectChanged(bool enabled);
    void cameraSettingsChanged(int index, int w, int h, int fps);
    void realSenseControlsRequested();
    void recalibrateRequested();
    void generateCheckerboardRequested();
    void openCalibrationPdfRequested();
    void alignHomographyRequested();
    void alignOnComponentsRequested();
    void alignMultiRequested();
    void autoAlignRequested();
    void liveModeChanged(bool enabled);
    void hybridModeChanged(bool enabled);

private:
    void buildUI();
    QGroupBox* createOverlayGroup();
    QGroupBox* createAiGroup();
    QGroupBox* createCameraGroup();
    QGroupBox* createCalibrationGroup();

    // Overlay controls
    QSlider*       m_opacitySlider   = nullptr;
    QLabel*        m_opacityLabel    = nullptr;
    QCheckBox*     m_showPads        = nullptr;
    QCheckBox*     m_showSilkscreen  = nullptr;
    QCheckBox*     m_showFabrication = nullptr;
    QCheckBox*     m_showHeatmap     = nullptr;

    // AI controls
    QDoubleSpinBox* m_confidenceSpin = nullptr;
    QCheckBox*      m_autoInspect    = nullptr;

    // Camera controls
    QComboBox* m_cameraDevice  = nullptr;
    QSpinBox*  m_camWidth      = nullptr;
    QSpinBox*  m_camHeight     = nullptr;
    QSpinBox*  m_camFps        = nullptr;
    QWidget*   m_camResWidget  = nullptr;  // container W/H/FPS — hidden for RealSense

    // Calibration & Alignment group. Widgets are shown/hidden by backend:
    // microscope → checkerboard tools; RealSense → factory note + sensor
    // controls. Alignment + live tracking apply to both.
    QLabel*      m_calibInfo      = nullptr;  // backend-specific one-liner
    QPushButton* m_btnCalibrate   = nullptr;  // microscope: checkerboard calib
    QPushButton* m_btnGenPattern  = nullptr;  // microscope: generate/print board
    QPushButton* m_btnOpenPdf      = nullptr;  // microscope: open patterns PDF
    QPushButton* m_btnRealSense    = nullptr;  // RealSense: sensor controls
    QPushButton* m_btnAlign        = nullptr;
    QPushButton* m_btnAlignComps   = nullptr;
    QPushButton* m_btnAlignMulti   = nullptr;
    QPushButton* m_btnAutoAlign    = nullptr;
    QCheckBox*   m_liveMode        = nullptr;
    QCheckBox*   m_hybridMode      = nullptr;
};

} // namespace ibom::gui
