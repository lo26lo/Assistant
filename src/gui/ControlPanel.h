#pragma once

#include <QWidget>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
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

    void setCameraDevices(const QStringList& devices);

signals:
    void overlayOpacityChanged(float opacity);
    void confidenceChanged(float conf);
    void showPadsChanged(bool show);
    void showSilkscreenChanged(bool show);
    void showFabricationChanged(bool show);
    void showHeatmapChanged(bool show);
    void autoInspectChanged(bool enabled);
    void cameraSettingsChanged(int index, int w, int h, int fps);
    void recalibrateRequested();
    void alignHomographyRequested();
    void alignOnComponentsRequested();
    void liveModeChanged(bool enabled);

private:
    void buildUI();
    QGroupBox* createOverlayGroup();
    QGroupBox* createAiGroup();
    QGroupBox* createCameraGroup();
    QGroupBox* createActionsGroup();

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
    QComboBox* m_cameraDevice = nullptr;
    QSpinBox*  m_camWidth     = nullptr;
    QSpinBox*  m_camHeight    = nullptr;
    QSpinBox*  m_camFps       = nullptr;

    // Action buttons
    QPushButton* m_btnCalibrate    = nullptr;
    QPushButton* m_btnAlign        = nullptr;
    QPushButton* m_btnAlignComps   = nullptr;
    QCheckBox*   m_liveMode        = nullptr;
};

} // namespace ibom::gui
