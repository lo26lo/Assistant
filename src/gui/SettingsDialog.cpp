#include "SettingsDialog.h"
#include "../app/Config.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QApplication>
#include <QMediaDevices>
#include <QCameraDevice>
#include <QColorDialog>

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
    createInspectionTab(tabs);
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

    // Camera device selector with refresh button
    auto* deviceRow = new QHBoxLayout;
    m_cameraDevice = new QComboBox;
    m_cameraDevice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_refreshCameras = new QPushButton(tr("Refresh"));
    m_refreshCameras->setToolTip(tr("Re-scan available cameras"));
    m_refreshCameras->setFixedWidth(70);
    connect(m_refreshCameras, &QPushButton::clicked, this, &SettingsDialog::enumerateCameras);
    deviceRow->addWidget(m_cameraDevice, 1);
    deviceRow->addWidget(m_refreshCameras);
    form->addRow(tr("Camera:"), deviceRow);

    enumerateCameras();

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

    // --- Calibration group ---
    auto* calibGroup = new QGroupBox(tr("Calibration Checkerboard"));
    auto* calibForm  = new QFormLayout(calibGroup);

    m_calibBoardCols = new QSpinBox;
    m_calibBoardCols->setRange(3, 20);
    m_calibBoardCols->setToolTip(tr("Inner corners (columns)"));
    calibForm->addRow(tr("Board cols:"), m_calibBoardCols);

    m_calibBoardRows = new QSpinBox;
    m_calibBoardRows->setRange(3, 20);
    m_calibBoardRows->setToolTip(tr("Inner corners (rows)"));
    calibForm->addRow(tr("Board rows:"), m_calibBoardRows);

    m_calibSquareSize = new QDoubleSpinBox;
    m_calibSquareSize->setRange(0.1, 50.0);
    m_calibSquareSize->setSingleStep(0.5);
    m_calibSquareSize->setSuffix(" mm");
    m_calibSquareSize->setDecimals(1);
    m_calibSquareSize->setToolTip(tr("Physical size of one square in mm"));
    calibForm->addRow(tr("Square size:"), m_calibSquareSize);

    m_scaleMethod = new QComboBox;
    m_scaleMethod->addItem(tr("None (fixed calibration)"),     0);
    m_scaleMethod->addItem(tr("From homography (auto-zoom)"),  1);
    m_scaleMethod->addItem(tr("From iBOM pad distances"),      2);
    m_scaleMethod->setToolTip(tr("How to update px/mm when the microscope zoom changes"));
    calibForm->addRow(tr("Dynamic scale:"), m_scaleMethod);

    m_opticalMultiplier = new QComboBox;
    m_opticalMultiplier->addItem(tr("0.5x (wide FOV)"),  0.5);
    m_opticalMultiplier->addItem(tr("0.75x"),             0.75);
    m_opticalMultiplier->addItem(tr("1x (no adapter)"),   1.0);
    m_opticalMultiplier->addItem(tr("1.5x"),              1.5);
    m_opticalMultiplier->addItem(tr("2x (zoom)"),         2.0);
    m_opticalMultiplier->setToolTip(tr("Optical adapter multiplier — affects pixels/mm ratio"));
    calibForm->addRow(tr("Lens adapter:"), m_opticalMultiplier);

    form->addRow(calibGroup);

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
    m_matchRatio->setRange(0.50, 0.95);
    m_matchRatio->setSingleStep(0.05);
    m_matchRatio->setDecimals(2);
    m_matchRatio->setToolTip(tr("Lowe's ratio test — smaller is stricter (typical 0.7–0.8)"));
    form->addRow(tr("Lowe ratio:"), m_matchRatio);

    m_ransacThreshold = new QDoubleSpinBox;
    m_ransacThreshold->setRange(0.5, 20.0);
    m_ransacThreshold->setSingleStep(0.5);
    m_ransacThreshold->setSuffix(" px");
    m_ransacThreshold->setToolTip(tr("RANSAC reprojection threshold"));
    form->addRow(tr("RANSAC threshold:"), m_ransacThreshold);

    m_trackingDownscale = new QDoubleSpinBox;
    m_trackingDownscale->setRange(0.1, 1.0);
    m_trackingDownscale->setSingleStep(0.1);
    m_trackingDownscale->setDecimals(2);
    m_trackingDownscale->setToolTip(tr("Image scale before ORB (1.0 = full res, 0.5 = half). "
                                        "Smaller = faster but less robust."));
    form->addRow(tr("Downscale:"), m_trackingDownscale);

    tabs->addTab(page, tr("Tracking"));
}

void SettingsDialog::updateColorButton(QPushButton* btn, const QColor& c)
{
    btn->setText(c.name(QColor::HexRgb).toUpper());
    QString fg = (c.lightness() > 128) ? "#000" : "#fff";
    btn->setStyleSheet(QString("QPushButton { background-color: %1; color: %2; "
                               "border: 1px solid #555; padding: 4px 10px; }")
                       .arg(c.name()).arg(fg));
}

void SettingsDialog::pickColor(QPushButton* btn, QColor& target)
{
    QColor chosen = QColorDialog::getColor(target, this, tr("Choose color"));
    if (chosen.isValid()) {
        target = chosen;
        updateColorButton(btn, target);
    }
}

void SettingsDialog::createInspectionTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);

    m_sortMethod = new QComboBox;
    m_sortMethod->addItem(tr("Most numerous group first (recommended)"), 0);
    m_sortMethod->addItem(tr("Alphabetic by value"),                     1);
    m_sortMethod->addItem(tr("Load order (matches PCB layout)"),         2);
    m_sortMethod->addItem(tr("Smallest footprint first"),                3);
    m_sortMethod->setToolTip(tr(
        "Order in which components are presented during inspection.\n"
        "Grouping by quantity minimizes SMD reel changes."));
    form->addRow(tr("Component order:"), m_sortMethod);

    // ── Colors ────────────────────────────────────────────────
    auto* colorsGroup = new QGroupBox(tr("Overlay Colors"));
    auto* colorsForm  = new QFormLayout(colorsGroup);

    m_btnSelectedColor = new QPushButton;
    m_btnSelectedColor->setToolTip(tr("Color of the component currently being inspected"));
    connect(m_btnSelectedColor, &QPushButton::clicked, this, [this]() {
        pickColor(m_btnSelectedColor, m_colorSelected);
    });
    colorsForm->addRow(tr("Selected:"), m_btnSelectedColor);

    m_btnPlacedColor = new QPushButton;
    m_btnPlacedColor->setToolTip(tr("Color of components already placed"));
    connect(m_btnPlacedColor, &QPushButton::clicked, this, [this]() {
        pickColor(m_btnPlacedColor, m_colorPlaced);
    });
    colorsForm->addRow(tr("Placed:"), m_btnPlacedColor);

    m_btnNormalColor = new QPushButton;
    m_btnNormalColor->setToolTip(tr("Color of pending components"));
    connect(m_btnNormalColor, &QPushButton::clicked, this, [this]() {
        pickColor(m_btnNormalColor, m_colorNormal);
    });
    colorsForm->addRow(tr("Pending:"), m_btnNormalColor);

    form->addRow(colorsGroup);

    // ── Visibility tweaks ─────────────────────────────────────
    auto* opacityRow = new QHBoxLayout;
    m_placedOpacitySlider = new QSlider(Qt::Horizontal);
    m_placedOpacitySlider->setRange(5, 100);
    m_placedOpacityLabel = new QLabel;
    connect(m_placedOpacitySlider, &QSlider::valueChanged, this, [this](int v) {
        m_placedOpacityLabel->setText(QString("%1 %").arg(v));
    });
    opacityRow->addWidget(m_placedOpacitySlider);
    opacityRow->addWidget(m_placedOpacityLabel);
    form->addRow(tr("Placed opacity:"), opacityRow);

    m_selectedOutlineWidth = new QDoubleSpinBox;
    m_selectedOutlineWidth->setRange(1.0, 8.0);
    m_selectedOutlineWidth->setSingleStep(0.5);
    m_selectedOutlineWidth->setDecimals(1);
    m_selectedOutlineWidth->setSuffix(" px");
    m_selectedOutlineWidth->setToolTip(tr("Outline thickness for the selected component"));
    form->addRow(tr("Selected outline:"), m_selectedOutlineWidth);

    tabs->addTab(page, tr("Inspection"));
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
    int idx = m_config.cameraIndex();
    if (idx >= 0 && idx < m_cameraDevice->count())
        m_cameraDevice->setCurrentIndex(idx);
    m_cameraWidth->setValue(m_config.cameraWidth());
    m_cameraHeight->setValue(m_config.cameraHeight());
    m_cameraFps->setValue(m_config.cameraFps());

    // Calibration
    m_calibBoardCols->setValue(m_config.calibBoardCols());
    m_calibBoardRows->setValue(m_config.calibBoardRows());
    m_calibSquareSize->setValue(static_cast<double>(m_config.calibSquareSize()));
    m_scaleMethod->setCurrentIndex(
        m_scaleMethod->findData(static_cast<int>(m_config.scaleMethod())));
    m_opticalMultiplier->setCurrentIndex(
        m_opticalMultiplier->findData(static_cast<double>(m_config.opticalMultiplier())));

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
    m_trackingDownscale->setValue(static_cast<double>(m_config.trackingDownscale()));

    // AI
    m_modelsPath->setText(QString::fromStdString(m_config.modelsPath()));
    m_useTensorRT->setChecked(m_config.useTensorRT());
    m_aiConfidence->setValue(static_cast<double>(m_config.detectionConfidence()));

    // Inspection
    m_sortMethod->setCurrentIndex(
        m_sortMethod->findData(static_cast<int>(m_config.sortMethod())));

    m_colorSelected = QColor(QString::fromStdString(m_config.selectedColorHex()));
    m_colorPlaced   = QColor(QString::fromStdString(m_config.placedColorHex()));
    m_colorNormal   = QColor(QString::fromStdString(m_config.normalColorHex()));
    if (!m_colorSelected.isValid()) m_colorSelected = QColor(0, 229, 255);
    if (!m_colorPlaced.isValid())   m_colorPlaced   = QColor(72, 200, 72);
    if (!m_colorNormal.isValid())   m_colorNormal   = QColor(170, 170, 68);
    updateColorButton(m_btnSelectedColor, m_colorSelected);
    updateColorButton(m_btnPlacedColor,   m_colorPlaced);
    updateColorButton(m_btnNormalColor,   m_colorNormal);

    m_placedOpacitySlider->setValue(static_cast<int>(m_config.placedOpacity() * 100));
    m_selectedOutlineWidth->setValue(static_cast<double>(m_config.selectedOutlineWidth()));
}

void SettingsDialog::accept()
{
    // Camera
    m_config.setCameraIndex(m_cameraDevice->currentIndex());
    m_config.setCameraWidth(m_cameraWidth->value());
    m_config.setCameraHeight(m_cameraHeight->value());
    m_config.setCameraFps(m_cameraFps->value());

    // Calibration
    m_config.setCalibBoardCols(m_calibBoardCols->value());
    m_config.setCalibBoardRows(m_calibBoardRows->value());
    m_config.setCalibSquareSize(static_cast<float>(m_calibSquareSize->value()));
    m_config.setScaleMethod(
        static_cast<ibom::ScaleMethod>(m_scaleMethod->currentData().toInt()));
    m_config.setOpticalMultiplier(
        static_cast<float>(m_opticalMultiplier->currentData().toDouble()));

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
    m_config.setTrackingDownscale(static_cast<float>(m_trackingDownscale->value()));

    // AI
    m_config.setModelsPath(m_modelsPath->text().toStdString());
    m_config.setUseTensorRT(m_useTensorRT->isChecked());
    m_config.setDetectionConfidence(static_cast<float>(m_aiConfidence->value()));

    // Inspection
    m_config.setSortMethod(
        static_cast<ibom::SortMethod>(m_sortMethod->currentData().toInt()));
    m_config.setSelectedColorHex(m_colorSelected.name(QColor::HexRgb).toStdString());
    m_config.setPlacedColorHex  (m_colorPlaced  .name(QColor::HexRgb).toStdString());
    m_config.setNormalColorHex  (m_colorNormal  .name(QColor::HexRgb).toStdString());
    m_config.setPlacedOpacity(static_cast<float>(m_placedOpacitySlider->value()) / 100.0f);
    m_config.setSelectedOutlineWidth(static_cast<float>(m_selectedOutlineWidth->value()));

    // Persist
    m_config.save();

    QDialog::accept();
}

void SettingsDialog::enumerateCameras()
{
    int previousIndex = m_cameraDevice->currentIndex();
    m_cameraDevice->clear();

    QApplication::setOverrideCursor(Qt::WaitCursor);

    const auto cameras = QMediaDevices::videoInputs();
    if (cameras.isEmpty()) {
        m_cameraDevice->addItem(tr("No camera detected"));
    } else {
        for (int i = 0; i < cameras.size(); ++i) {
            QString label = QString("%1: %2").arg(i).arg(cameras[i].description());
            m_cameraDevice->addItem(label, i);
        }
    }

    QApplication::restoreOverrideCursor();

    if (previousIndex >= 0 && previousIndex < m_cameraDevice->count())
        m_cameraDevice->setCurrentIndex(previousIndex);
}
