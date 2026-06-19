#include "ControlPanel.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QScrollArea>
#include <QSignalBlocker>

namespace ibom::gui {

ControlPanel::ControlPanel(QWidget* parent)
    : QWidget(parent)
{
    buildUI();
}

void ControlPanel::buildUI()
{
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget;
    auto* layout  = new QVBoxLayout(content);
    layout->setContentsMargins(theme::PanelMargin, theme::PanelMargin,
                               theme::PanelMargin, theme::PanelMargin);
    layout->setSpacing(theme::PanelSpacing);

    layout->addWidget(createOverlayGroup());
    layout->addWidget(createAiGroup());
    layout->addWidget(createCameraGroup());
    layout->addWidget(createCalibrationGroup());
    layout->addStretch();

    scroll->setWidget(content);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(scroll);
}

QGroupBox* ControlPanel::createOverlayGroup()
{
    auto* group  = new QGroupBox(tr("Overlay"));
    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(theme::GroupSpacing);
    layout->setContentsMargins(theme::GroupMarginH, theme::GroupMarginV,
                               theme::GroupMarginH, theme::GroupMarginV);

    // Opacity slider
    auto* opacityRow = new QHBoxLayout;
    opacityRow->addWidget(new QLabel(tr("Opacity:")));
    m_opacitySlider = new QSlider(Qt::Horizontal);
    m_opacitySlider->setRange(0, 100);
    m_opacitySlider->setValue(50);
    m_opacityLabel = new QLabel("50%");
    opacityRow->addWidget(m_opacitySlider, 1);
    opacityRow->addWidget(m_opacityLabel);
    layout->addLayout(opacityRow);

    connect(m_opacitySlider, &QSlider::valueChanged, this, [this](int val) {
        m_opacityLabel->setText(QString("%1%").arg(val));
        emit overlayOpacityChanged(val / 100.0f);
    });

    // Checkboxes
    m_showPads = new QCheckBox(tr("Show Pads"));
    m_showPads->setChecked(true);
    connect(m_showPads, &QCheckBox::toggled, this, &ControlPanel::showPadsChanged);
    layout->addWidget(m_showPads);

    m_showSilkscreen = new QCheckBox(tr("Show Silkscreen"));
    m_showSilkscreen->setChecked(true);
    connect(m_showSilkscreen, &QCheckBox::toggled, this, &ControlPanel::showSilkscreenChanged);
    layout->addWidget(m_showSilkscreen);

    m_showFabrication = new QCheckBox(tr("Show Fabrication"));
    m_showFabrication->setChecked(false);
    connect(m_showFabrication, &QCheckBox::toggled, this, &ControlPanel::showFabricationChanged);
    layout->addWidget(m_showFabrication);

    m_showHeatmap = new QCheckBox(tr("Show Defect Heatmap"));
    m_showHeatmap->setChecked(false);
    connect(m_showHeatmap, &QCheckBox::toggled, this, &ControlPanel::showHeatmapChanged);
    layout->addWidget(m_showHeatmap);

    return group;
}

QGroupBox* ControlPanel::createAiGroup()
{
    auto* group  = new QGroupBox(tr("AI Detection"));
    auto* layout = new QFormLayout(group);
    layout->setSpacing(theme::GroupSpacing);
    layout->setContentsMargins(theme::GroupMarginH, theme::GroupMarginV,
                               theme::GroupMarginH, theme::GroupMarginV);
    layout->setLabelAlignment(Qt::AlignRight);

    m_confidenceSpin = new QDoubleSpinBox;
    m_confidenceSpin->setRange(0.1, 1.0);
    m_confidenceSpin->setSingleStep(0.05);
    m_confidenceSpin->setValue(0.5);
    m_confidenceSpin->setDecimals(2);
    layout->addRow(tr("Confidence:"), m_confidenceSpin);

    connect(m_confidenceSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double val) { emit confidenceChanged(static_cast<float>(val)); });

    m_autoInspect = new QCheckBox(tr("Auto-inspect on detection"));
    m_autoInspect->setChecked(false);
    connect(m_autoInspect, &QCheckBox::toggled, this, &ControlPanel::autoInspectChanged);
    layout->addRow(m_autoInspect);

    return group;
}

QGroupBox* ControlPanel::createCameraGroup()
{
    auto* group  = new QGroupBox(tr("Camera"));
    auto* vbox   = new QVBoxLayout(group);
    vbox->setSpacing(theme::GroupSpacing);
    vbox->setContentsMargins(theme::GroupMarginH, theme::GroupMarginV,
                             theme::GroupMarginH, theme::GroupMarginV);

    // Device selector — always visible
    auto* devForm = new QFormLayout;
    devForm->setLabelAlignment(Qt::AlignRight);
    m_cameraDevice = new QComboBox;
    m_cameraDevice->addItem(tr("Default (0)"), 0);
    // Long device names (e.g. "Intel RealSense D405 <serial>") must not force
    // the dock wider — let the combo shrink and elide instead.
    m_cameraDevice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_cameraDevice->setMinimumContentsLength(8);
    m_cameraDevice->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    devForm->addRow(tr("Device:"), m_cameraDevice);
    vbox->addLayout(devForm);

    // W/H/FPS + Apply — hidden when RealSense is active (fixed resolution profiles)
    m_camResWidget = new QWidget;
    auto* resForm  = new QFormLayout(m_camResWidget);
    resForm->setContentsMargins(0, 0, 0, 0);
    resForm->setSpacing(theme::GroupSpacing);
    resForm->setLabelAlignment(Qt::AlignRight);

    m_camWidth = new QSpinBox;
    m_camWidth->setRange(320, 4096);
    m_camWidth->setValue(1920);
    resForm->addRow(tr("Width:"), m_camWidth);

    m_camHeight = new QSpinBox;
    m_camHeight->setRange(240, 2160);
    m_camHeight->setValue(1080);
    resForm->addRow(tr("Height:"), m_camHeight);

    m_camFps = new QSpinBox;
    m_camFps->setRange(1, 120);
    m_camFps->setValue(30);
    resForm->addRow(tr("FPS:"), m_camFps);

    auto* applyBtn = new QPushButton(tr("Apply Camera"));
    connect(applyBtn, &QPushButton::clicked, this, [this]() {
        emit cameraSettingsChanged(cameraIndex(),
                                    m_camWidth->value(), m_camHeight->value(),
                                    m_camFps->value());
    });
    resForm->addRow(applyBtn);

    vbox->addWidget(m_camResWidget);

    return group;
}

QGroupBox* ControlPanel::createCalibrationGroup()
{
    auto* group  = new QGroupBox(tr("Calibration && Alignment"));
    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(theme::GroupSpacing);
    layout->setContentsMargins(theme::GroupMarginH, theme::GroupMarginV,
                               theme::GroupMarginH, theme::GroupMarginV);

    // Backend-specific one-liner (set by setCameraBackendUI).
    m_calibInfo = new QLabel;
    m_calibInfo->setWordWrap(true);
    m_calibInfo->setStyleSheet("color: #8892b8; font-size: 11px;");
    layout->addWidget(m_calibInfo);

    // ── Microscope (V4L2): lens calibration via printed checkerboard ──
    m_btnCalibrate = new QPushButton(tr("Calibrate (Checkerboard)"));
    m_btnCalibrate->setToolTip(tr("Capture checkerboard views and compute lens "
                                  "distortion correction (OpenCV)."));
    connect(m_btnCalibrate, &QPushButton::clicked, this, &ControlPanel::recalibrateRequested);
    layout->addWidget(m_btnCalibrate);

    m_btnGenPattern = new QPushButton(tr("Generate Checkerboard…"));
    m_btnGenPattern->setToolTip(tr("Generate a printable checkerboard at the "
                                   "configured square size."));
    connect(m_btnGenPattern, &QPushButton::clicked, this, &ControlPanel::generateCheckerboardRequested);
    layout->addWidget(m_btnGenPattern);

    m_btnOpenPdf = new QPushButton(tr("Open Patterns PDF…"));
    m_btnOpenPdf->setToolTip(tr("Open the bundled patterns (0.5 / 1 / 2 mm squares)."));
    connect(m_btnOpenPdf, &QPushButton::clicked, this, &ControlPanel::openCalibrationPdfRequested);
    layout->addWidget(m_btnOpenPdf);

    // ── RealSense: factory-calibrated, expose live sensor controls ──
    m_btnRealSense = new QPushButton(tr("RealSense Controls…"));
    m_btnRealSense->setToolTip(tr("Exposure, gain, laser power, depth presets… — every "
                                  "sensor option of a connected RealSense, each with its "
                                  "SDK description. Requires the RealSense backend running."));
    connect(m_btnRealSense, &QPushButton::clicked, this, &ControlPanel::realSenseControlsRequested);
    layout->addWidget(m_btnRealSense);

    // ── Alignment + live tracking (both backends) ──
    // Guided assistant: single entry point that walks through method choice,
    // instructions, and a final summary. The individual buttons below remain
    // for power users who already know which method they want.
    m_btnAlignWizard = new QPushButton(tr("Alignment Assistant…"));
    m_btnAlignWizard->setToolTip(tr("Step-by-step guide: pick a method, follow the "
                                    "instructions, and see a summary of the result. "
                                    "Recommended if you're unsure which alignment to use."));
    connect(m_btnAlignWizard, &QPushButton::clicked, this, &ControlPanel::alignmentWizardRequested);
    layout->addWidget(m_btnAlignWizard);

    m_btnAlign = new QPushButton(tr("Align: 4 Corners"));
    m_btnAlign->setToolTip(tr("Set alignment by clicking the 4 board corners."));
    connect(m_btnAlign, &QPushButton::clicked, this, &ControlPanel::alignHomographyRequested);
    layout->addWidget(m_btnAlign);

    m_btnAlignComps = new QPushButton(tr("Align: 2 Components"));
    m_btnAlignComps->setToolTip(tr("Align overlay by clicking 2 known components — best for small FOV microscopes"));
    connect(m_btnAlignComps, &QPushButton::clicked, this, &ControlPanel::alignOnComponentsRequested);
    layout->addWidget(m_btnAlignComps);

    m_btnAlignMulti = new QPushButton(tr("Align: Multi-Comp (Beta)"));
    m_btnAlignMulti->setToolTip(tr("Align using several components (≥2, ≥4 for perspective). "
                                   "Works on non-rectangular boards.\n"
                                   "1) Click a component row in the BOM panel.\n"
                                   "2) Choose 'Pin 1' (one click in the image, most precise — "
                                   "best for ESP32 & complex footprints) or '2 Opposite Corners' "
                                   "(two clicks, any diagonal — midpoint is used).\n"
                                   "The PCB Map highlights the selected part and marks pin 1 "
                                   "(red dot) to guide you.\n"
                                   "3) Repeat, then click this button again to finish."));
    connect(m_btnAlignMulti, &QPushButton::clicked, this, &ControlPanel::alignMultiRequested);
    layout->addWidget(m_btnAlignMulti);

    m_btnAutoAlign = new QPushButton(tr("Auto-Align (Beta)"));
    m_btnAutoAlign->setToolTip(tr("Automatically locate the board outline in the current frame "
                                  "and orient the overlay to match — no clicking required. Works "
                                  "best with the whole board visible (depth-assisted on RealSense)."));
    connect(m_btnAutoAlign, &QPushButton::clicked, this, &ControlPanel::autoAlignRequested);
    layout->addWidget(m_btnAutoAlign);

    m_liveMode = new QCheckBox(tr("Live Tracking Mode"));
    m_liveMode->setToolTip(tr("Track PCB movement in real-time using feature matching"));
    connect(m_liveMode, &QCheckBox::toggled, this, &ControlPanel::liveModeChanged);
    layout->addWidget(m_liveMode);

    m_hybridMode = new QCheckBox(tr("Hybrid drift correction (beta)"));
    m_hybridMode->setToolTip(tr("During incremental tracking, snap back to the original "
                                "anchor view whenever it is recognized again — eliminates "
                                "long-term drift while keeping smooth tracking. Only affects "
                                "incremental (narrow-FOV / microscope) tracking."));
    m_hybridMode->setChecked(true);  // matches Config default; overridden by setHybridMode()
    m_hybridMode->setStyleSheet("margin-left: 18px;");  // nest under Live Tracking Mode
    connect(m_hybridMode, &QCheckBox::toggled, this, &ControlPanel::hybridModeChanged);
    layout->addWidget(m_hybridMode);

    // Default to microscope view until the backend is known.
    setCameraBackendUI(false);

    return group;
}

// ── Getters ──────────────────────────────────────────────────────

float ControlPanel::overlayOpacity() const
{
    return m_opacitySlider->value() / 100.0f;
}

float ControlPanel::confidenceThreshold() const
{
    return static_cast<float>(m_confidenceSpin->value());
}

bool ControlPanel::showPads() const { return m_showPads->isChecked(); }
bool ControlPanel::showSilkscreen() const { return m_showSilkscreen->isChecked(); }
bool ControlPanel::showFabrication() const { return m_showFabrication->isChecked(); }
bool ControlPanel::showHeatmap() const { return m_showHeatmap->isChecked(); }
bool ControlPanel::autoInspect() const { return m_autoInspect->isChecked(); }

int ControlPanel::cameraIndex()  const
{
    // The real /dev/video index is stored as item data (see setCameraDevices);
    // fall back to the list position only for legacy/empty entries.
    const QVariant d = m_cameraDevice->currentData();
    return d.isValid() ? d.toInt() : m_cameraDevice->currentIndex();
}
int ControlPanel::cameraWidth()  const { return m_camWidth->value(); }
int ControlPanel::cameraHeight() const { return m_camHeight->value(); }
int ControlPanel::cameraFps()    const { return m_camFps->value(); }

void ControlPanel::setCameraDevices(const QStringList& labels,
                                    const QList<int>& indices, int currentIndex)
{
    QSignalBlocker block(m_cameraDevice);
    m_cameraDevice->clear();
    for (int i = 0; i < labels.size(); ++i) {
        const int realIdx = (i < indices.size()) ? indices[i] : i;
        m_cameraDevice->addItem(labels[i], realIdx);
    }
    if (currentIndex >= 0) {
        const int pos = m_cameraDevice->findData(currentIndex);
        if (pos >= 0)
            m_cameraDevice->setCurrentIndex(pos);
    }
}

void ControlPanel::setConfidenceThreshold(float conf)
{
    QSignalBlocker blocker(m_confidenceSpin);
    m_confidenceSpin->setValue(static_cast<double>(conf));
}

void ControlPanel::setHybridMode(bool enabled)
{
    if (!m_hybridMode) return;
    QSignalBlocker blocker(m_hybridMode);
    m_hybridMode->setChecked(enabled);
}

bool ControlPanel::hybridMode() const
{
    return m_hybridMode && m_hybridMode->isChecked();
}

void ControlPanel::setCameraBackendUI(bool isRealSense)
{
    if (!m_btnCalibrate) return;  // group not built yet

    // Camera group: hide free W/H/FPS spinboxes for RealSense (fixed profiles)
    if (m_camResWidget) m_camResWidget->setVisible(!isRealSense);

    // Show only the tools relevant to the active backend. Alignment + live
    // tracking stay visible for both.
    m_btnCalibrate->setVisible(!isRealSense);
    m_btnGenPattern->setVisible(!isRealSense);
    m_btnOpenPdf->setVisible(!isRealSense);
    m_btnRealSense->setVisible(isRealSense);

    if (m_calibInfo) {
        m_calibInfo->setText(isRealSense
            ? tr("RealSense: factory-calibrated. No checkerboard needed — see the "
                 "intrinsics in Statistics. Set scale to “From depth” in Settings.")
            : tr("USB microscope: print a checkerboard, then calibrate to correct "
                 "lens distortion and derive px/mm."));
    }
}

} // namespace ibom::gui
