#pragma once

#include <QDialog>
#include <QTabWidget>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QSlider>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QColor>

namespace ibom { class Config; }

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(ibom::Config& config, QWidget* parent = nullptr);

signals:
    /// User asked to open the RealSense controls panel from the Camera tab.
    /// MainWindow closes Settings and routes this to Application (live camera).
    void realSenseControlsRequested();

public slots:
    // QDialog::accept() is a public slot; the override stays public so callers
    // (MainWindow closing Settings before opening the RealSense panel) can
    // invoke it.
    void accept() override;

private:
    void createCameraTab(QTabWidget* tabs);
    void createOverlayTab(QTabWidget* tabs);
    void createTrackingTab(QTabWidget* tabs);
    void createInspectionTab(QTabWidget* tabs);
    void createAiTab(QTabWidget* tabs);
    void createFeaturesTab(QTabWidget* tabs);

    void loadFromConfig();
    void enumerateCameras();

    ibom::Config& m_config;

    // Camera
    QComboBox*    m_cameraBackend  = nullptr;
    QComboBox*    m_cameraDevice   = nullptr;
    QPushButton*  m_refreshCameras = nullptr;
    QSpinBox*     m_cameraWidth    = nullptr;
    QSpinBox*     m_cameraHeight   = nullptr;
    QSpinBox*     m_cameraFps      = nullptr;
    QCheckBox*    m_cameraHwDecode = nullptr;

    // Resolution selector — V4L2 uses free spinboxes, RealSense uses profiles
    QWidget*      m_v4l2ResWidget  = nullptr;  // container W/H/FPS (V4L2 only)
    QWidget*      m_rsResWidget    = nullptr;  // container profile combo (RS only)
    QComboBox*    m_rsResCombo     = nullptr;

    void updateCameraResolutionUI();

    // Calibration
    QSpinBox*       m_calibBoardCols  = nullptr;
    QSpinBox*       m_calibBoardRows  = nullptr;
    QDoubleSpinBox* m_calibSquareSize = nullptr;
    QComboBox*      m_scaleMethod     = nullptr;
    QComboBox*      m_opticalMultiplier = nullptr;

    // Overlay
    QSlider*   m_overlayOpacity   = nullptr;
    QLabel*    m_opacityLabel     = nullptr;
    QCheckBox* m_showPads         = nullptr;
    QCheckBox* m_showSilkscreen   = nullptr;
    QCheckBox* m_showFabrication  = nullptr;

    // Tracking
    QSpinBox*       m_trackingInterval  = nullptr;
    QSpinBox*       m_orbKeypoints      = nullptr;
    QSpinBox*       m_minMatches        = nullptr;
    QDoubleSpinBox* m_matchRatio        = nullptr;
    QDoubleSpinBox* m_ransacThreshold   = nullptr;
    QDoubleSpinBox* m_trackingDownscale = nullptr;

    // AI
    QLineEdit*      m_modelsPath     = nullptr;
    QCheckBox*      m_useTensorRT    = nullptr;
    QDoubleSpinBox* m_aiConfidence   = nullptr;

    // Features
    QCheckBox* m_remoteViewEnabled = nullptr;
    QSpinBox*  m_remoteViewPort    = nullptr;
    QCheckBox* m_autoReloadIbom    = nullptr;

    // Inspection
    QComboBox*      m_sortMethod              = nullptr;
    QPushButton*    m_btnSelectedColor        = nullptr;
    QPushButton*    m_btnPlacedColor          = nullptr;
    QPushButton*    m_btnNormalColor          = nullptr;
    QSlider*        m_placedOpacitySlider     = nullptr;
    QLabel*         m_placedOpacityLabel      = nullptr;
    QDoubleSpinBox* m_selectedOutlineWidth    = nullptr;

    QColor m_colorSelected;
    QColor m_colorPlaced;
    QColor m_colorNormal;

    void pickColor(QPushButton* btn, QColor& target);
    void updateColorButton(QPushButton* btn, const QColor& c);
};
