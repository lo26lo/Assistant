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

namespace ibom { class Config; }

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(ibom::Config& config, QWidget* parent = nullptr);

private slots:
    void accept() override;

private:
    void createCameraTab(QTabWidget* tabs);
    void createOverlayTab(QTabWidget* tabs);
    void createTrackingTab(QTabWidget* tabs);
    void createAiTab(QTabWidget* tabs);

    void loadFromConfig();

    ibom::Config& m_config;

    // Camera
    QSpinBox* m_cameraIndex  = nullptr;
    QSpinBox* m_cameraWidth  = nullptr;
    QSpinBox* m_cameraHeight = nullptr;
    QSpinBox* m_cameraFps    = nullptr;

    // Calibration
    QSpinBox*       m_calibBoardCols  = nullptr;
    QSpinBox*       m_calibBoardRows  = nullptr;
    QDoubleSpinBox* m_calibSquareSize = nullptr;
    QComboBox*      m_scaleMethod     = nullptr;

    // Overlay
    QSlider*   m_overlayOpacity   = nullptr;
    QLabel*    m_opacityLabel     = nullptr;
    QCheckBox* m_showPads         = nullptr;
    QCheckBox* m_showSilkscreen   = nullptr;
    QCheckBox* m_showFabrication  = nullptr;

    // Tracking
    QSpinBox*       m_trackingInterval = nullptr;
    QSpinBox*       m_orbKeypoints     = nullptr;
    QSpinBox*       m_minMatches       = nullptr;
    QDoubleSpinBox* m_matchRatio       = nullptr;
    QDoubleSpinBox* m_ransacThreshold  = nullptr;

    // AI
    QLineEdit*      m_modelsPath     = nullptr;
    QCheckBox*      m_useTensorRT    = nullptr;
    QDoubleSpinBox* m_aiConfidence   = nullptr;
};
