#include "ControlPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QScrollArea>

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
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(4);

    layout->addWidget(createOverlayGroup());
    layout->addWidget(createAiGroup());
    layout->addWidget(createCameraGroup());
    layout->addWidget(createActionsGroup());
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
    layout->setSpacing(8);
    layout->setContentsMargins(4, 8, 4, 8);

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
    layout->setSpacing(8);
    layout->setContentsMargins(4, 8, 4, 8);
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
    auto* layout = new QFormLayout(group);
    layout->setSpacing(8);
    layout->setContentsMargins(4, 8, 4, 8);
    layout->setLabelAlignment(Qt::AlignRight);

    m_cameraDevice = new QComboBox;
    m_cameraDevice->addItem(tr("Default (0)"));
    layout->addRow(tr("Device:"), m_cameraDevice);

    m_camWidth = new QSpinBox;
    m_camWidth->setRange(320, 4096);
    m_camWidth->setValue(1920);
    layout->addRow(tr("Width:"), m_camWidth);

    m_camHeight = new QSpinBox;
    m_camHeight->setRange(240, 2160);
    m_camHeight->setValue(1080);
    layout->addRow(tr("Height:"), m_camHeight);

    m_camFps = new QSpinBox;
    m_camFps->setRange(1, 120);
    m_camFps->setValue(30);
    layout->addRow(tr("FPS:"), m_camFps);

    auto* applyBtn = new QPushButton(tr("Apply Camera"));
    connect(applyBtn, &QPushButton::clicked, this, [this]() {
        emit cameraSettingsChanged(m_cameraDevice->currentIndex(),
                                    m_camWidth->value(), m_camHeight->value(),
                                    m_camFps->value());
    });
    layout->addRow(applyBtn);

    return group;
}

QGroupBox* ControlPanel::createActionsGroup()
{
    auto* group  = new QGroupBox(tr("Actions"));
    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(8);
    layout->setContentsMargins(4, 8, 4, 8);

    m_btnCalibrate = new QPushButton(tr("Calibrate Camera (Checkerboard)"));
    connect(m_btnCalibrate, &QPushButton::clicked, this, &ControlPanel::recalibrateRequested);
    layout->addWidget(m_btnCalibrate);

    m_btnAlign = new QPushButton(tr("Set Alignment Points"));
    connect(m_btnAlign, &QPushButton::clicked, this, &ControlPanel::alignHomographyRequested);
    layout->addWidget(m_btnAlign);

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

int ControlPanel::cameraIndex()  const { return m_cameraDevice->currentIndex(); }
int ControlPanel::cameraWidth()  const { return m_camWidth->value(); }
int ControlPanel::cameraHeight() const { return m_camHeight->value(); }
int ControlPanel::cameraFps()    const { return m_camFps->value(); }

void ControlPanel::setCameraDevices(const QStringList& devices)
{
    m_cameraDevice->clear();
    for (int i = 0; i < devices.size(); ++i) {
        m_cameraDevice->addItem(QString("%1: %2").arg(i).arg(devices[i]));
    }
}

} // namespace ibom::gui
