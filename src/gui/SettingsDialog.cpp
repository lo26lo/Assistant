#include "SettingsDialog.h"
#include "../app/Config.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QGroupBox>

SettingsDialog::SettingsDialog(ibom::Config& config, QWidget* parent)
    : QDialog(parent)
    , m_config(config)
{
    setWindowTitle(tr("Settings"));
    setMinimumWidth(420);

    auto* layout = new QVBoxLayout(this);
    auto* tabs   = new QTabWidget;

    createCameraTab(tabs);
    createOverlayTab(tabs);
    createTrackingTab(tabs);
    createAiTab(tabs);

    layout->addWidget(tabs);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    loadFromConfig();
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

void SettingsDialog::createCameraTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);

    m_cameraIndex = new QSpinBox;
    m_cameraIndex->setRange(0, 10);
    form->addRow(tr("Camera index:"), m_cameraIndex);

    m_cameraWidth = new QSpinBox;
    m_cameraWidth->setRange(320, 7680);
    m_cameraWidth->setSingleStep(160);
    form->addRow(tr("Width:"), m_cameraWidth);

    m_cameraHeight = new QSpinBox;
    m_cameraHeight->setRange(240, 4320);
    m_cameraHeight->setSingleStep(120);
    form->addRow(tr("Height:"), m_cameraHeight);

    m_cameraFps = new QSpinBox;
    m_cameraFps->setRange(1, 120);
    form->addRow(tr("FPS:"), m_cameraFps);

    tabs->addTab(page, tr("Camera"));
}

void SettingsDialog::createOverlayTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);

    auto* opacityRow = new QHBoxLayout;
    m_overlayOpacity = new QSlider(Qt::Horizontal);
    m_overlayOpacity->setRange(0, 100);
    m_opacityLabel = new QLabel;
    connect(m_overlayOpacity, &QSlider::valueChanged, this, [this](int v) {
        m_opacityLabel->setText(QString("%1 %").arg(v));
    });
    opacityRow->addWidget(m_overlayOpacity);
    opacityRow->addWidget(m_opacityLabel);
    form->addRow(tr("Overlay opacity:"), opacityRow);

    m_showPads = new QCheckBox(tr("Show pads"));
    form->addRow(m_showPads);

    m_showSilkscreen = new QCheckBox(tr("Show silkscreen"));
    form->addRow(m_showSilkscreen);

    m_showFabrication = new QCheckBox(tr("Show fabrication"));
    form->addRow(m_showFabrication);

    tabs->addTab(page, tr("Overlay"));
}

void SettingsDialog::createTrackingTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);

    m_trackingInterval = new QSpinBox;
    m_trackingInterval->setRange(50, 2000);
    m_trackingInterval->setSuffix(" ms");
    m_trackingInterval->setToolTip(tr("Minimum interval between tracking updates"));
    form->addRow(tr("Tracking interval:"), m_trackingInterval);

    m_orbKeypoints = new QSpinBox;
    m_orbKeypoints->setRange(50, 5000);
    m_orbKeypoints->setSingleStep(50);
    m_orbKeypoints->setToolTip(tr("Number of ORB keypoints to detect"));
    form->addRow(tr("ORB keypoints:"), m_orbKeypoints);

    m_minMatches = new QSpinBox;
    m_minMatches->setRange(4, 100);
    m_minMatches->setToolTip(tr("Minimum matches required for homography"));
    form->addRow(tr("Min matches:"), m_minMatches);

    m_matchRatio = new QDoubleSpinBox;
    m_matchRatio->setRange(1.0, 10.0);
    m_matchRatio->setSingleStep(0.5);
    m_matchRatio->setToolTip(tr("Distance ratio multiplier for match filtering"));
    form->addRow(tr("Match distance ratio:"), m_matchRatio);

    m_ransacThreshold = new QDoubleSpinBox;
    m_ransacThreshold->setRange(0.5, 20.0);
    m_ransacThreshold->setSingleStep(0.5);
    m_ransacThreshold->setSuffix(" px");
    m_ransacThreshold->setToolTip(tr("RANSAC reprojection threshold"));
    form->addRow(tr("RANSAC threshold:"), m_ransacThreshold);

    tabs->addTab(page, tr("Tracking"));
}

void SettingsDialog::createAiTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);

    m_modelsPath = new QLineEdit;
    form->addRow(tr("Models path:"), m_modelsPath);

    m_useTensorRT = new QCheckBox(tr("Use TensorRT acceleration"));
    form->addRow(m_useTensorRT);

    m_aiConfidence = new QDoubleSpinBox;
    m_aiConfidence->setRange(0.05, 1.0);
    m_aiConfidence->setSingleStep(0.05);
    form->addRow(tr("Detection confidence:"), m_aiConfidence);

    tabs->addTab(page, tr("AI"));
}

// ---------------------------------------------------------------------------
// Load / Save
// ---------------------------------------------------------------------------

void SettingsDialog::loadFromConfig()
{
    // Camera
    m_cameraIndex->setValue(m_config.cameraIndex());
    m_cameraWidth->setValue(m_config.cameraWidth());
    m_cameraHeight->setValue(m_config.cameraHeight());
    m_cameraFps->setValue(m_config.cameraFps());

    // Overlay
    m_overlayOpacity->setValue(static_cast<int>(m_config.overlayOpacity() * 100));
    m_showPads->setChecked(m_config.showPads());
    m_showSilkscreen->setChecked(m_config.showSilkscreen());
    m_showFabrication->setChecked(m_config.showFabrication());

    // Tracking
    m_trackingInterval->setValue(m_config.trackingIntervalMs());
    m_orbKeypoints->setValue(m_config.orbKeypoints());
    m_minMatches->setValue(m_config.minMatchCount());
    m_matchRatio->setValue(m_config.matchDistanceRatio());
    m_ransacThreshold->setValue(m_config.ransacThreshold());

    // AI
    m_modelsPath->setText(QString::fromStdString(m_config.modelsPath()));
    m_useTensorRT->setChecked(m_config.useTensorRT());
    m_aiConfidence->setValue(static_cast<double>(m_config.detectionConfidence()));
}

void SettingsDialog::accept()
{
    // Camera
    m_config.setCameraIndex(m_cameraIndex->value());
    m_config.setCameraWidth(m_cameraWidth->value());
    m_config.setCameraHeight(m_cameraHeight->value());
    m_config.setCameraFps(m_cameraFps->value());

    // Overlay
    m_config.setOverlayOpacity(static_cast<float>(m_overlayOpacity->value()) / 100.0f);
    m_config.setShowPads(m_showPads->isChecked());
    m_config.setShowSilkscreen(m_showSilkscreen->isChecked());
    m_config.setShowFabrication(m_showFabrication->isChecked());

    // Tracking
    m_config.setTrackingIntervalMs(m_trackingInterval->value());
    m_config.setOrbKeypoints(m_orbKeypoints->value());
    m_config.setMinMatchCount(m_minMatches->value());
    m_config.setMatchDistanceRatio(m_matchRatio->value());
    m_config.setRansacThreshold(m_ransacThreshold->value());

    // AI
    m_config.setModelsPath(m_modelsPath->text().toStdString());
    m_config.setUseTensorRT(m_useTensorRT->isChecked());
    m_config.setDetectionConfidence(static_cast<float>(m_aiConfidence->value()));

    // Persist
    m_config.save();

    QDialog::accept();
}
