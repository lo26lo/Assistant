#include "InspectionPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QFont>

namespace ibom::gui {

InspectionPanel::InspectionPanel(QWidget* parent)
    : QWidget(parent)
{
    buildUI();
    QWidget::setEnabled(false);  // Disabled until iBOM is loaded
}

void InspectionPanel::setEnabled(bool enabled)
{
    QWidget::setEnabled(enabled);
}

// ── UI ────────────────────────────────────────────────────────────

void InspectionPanel::buildUI()
{
    auto* main = new QVBoxLayout(this);
    main->setContentsMargins(8, 8, 8, 8);
    main->setSpacing(10);

    // ── Inspection group ─────────────────────────────────────────
    auto* inspGroup = new QGroupBox(tr("Inspection"), this);
    auto* inspLayout = new QVBoxLayout(inspGroup);
    inspLayout->setSpacing(6);

    m_currentRefLabel = new QLabel(tr("—"), inspGroup);
    QFont refFont = m_currentRefLabel->font();
    refFont.setPointSize(refFont.pointSize() + 4);
    refFont.setBold(true);
    m_currentRefLabel->setFont(refFont);

    m_currentValueLabel = new QLabel(tr("Load iBOM and click Start"), inspGroup);
    m_currentFpLabel    = new QLabel("", inspGroup);
    m_layerLabel        = new QLabel("", inspGroup);
    m_stepLabel         = new QLabel(tr("Step 0/0"), inspGroup);

    m_progressBar = new QProgressBar(inspGroup);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFormat("%p% — %v / %m");

    inspLayout->addWidget(m_currentRefLabel);
    inspLayout->addWidget(m_currentValueLabel);
    inspLayout->addWidget(m_currentFpLabel);
    inspLayout->addWidget(m_layerLabel);
    inspLayout->addWidget(m_stepLabel);
    inspLayout->addWidget(m_progressBar);

    m_btnStart = new QPushButton(tr("Start Inspection"), inspGroup);
    m_btnStart->setToolTip(tr("Load all iBOM components into the placement workflow"));
    inspLayout->addWidget(m_btnStart);

    auto* navRow = new QHBoxLayout();
    m_btnBack   = new QPushButton(tr("◀ Back"),   inspGroup);
    m_btnPlaced = new QPushButton(tr("Placed ✓"), inspGroup);
    m_btnSkip   = new QPushButton(tr("Skip ▶"),   inspGroup);
    m_btnPlaced->setDefault(true);
    navRow->addWidget(m_btnBack);
    navRow->addWidget(m_btnPlaced);
    navRow->addWidget(m_btnSkip);
    inspLayout->addLayout(navRow);

    m_btnReset = new QPushButton(tr("Reset progress"), inspGroup);
    inspLayout->addWidget(m_btnReset);

    main->addWidget(inspGroup);

    // ── Measure group ───────────────────────────────────────────
    auto* measureGroup = new QGroupBox(tr("Measure"), this);
    auto* measureLayout = new QVBoxLayout(measureGroup);
    measureLayout->setSpacing(6);

    auto* modeRow = new QHBoxLayout();
    modeRow->addWidget(new QLabel(tr("Mode:"), measureGroup));
    m_measureMode = new QComboBox(measureGroup);
    m_measureMode->addItem(tr("Off"));
    m_measureMode->addItem(tr("Distance"));
    m_measureMode->addItem(tr("Angle"));
    m_measureMode->addItem(tr("Area"));
    m_measureMode->addItem(tr("Pin Pitch"));
    modeRow->addWidget(m_measureMode, 1);
    measureLayout->addLayout(modeRow);

    m_measureResult = new QLabel(tr("Click 2 points in the camera view"), measureGroup);
    m_measureResult->setWordWrap(true);
    measureLayout->addWidget(m_measureResult);

    m_btnClearMeasure = new QPushButton(tr("Clear measurements"), measureGroup);
    measureLayout->addWidget(m_btnClearMeasure);

    main->addWidget(measureGroup);

    // ── Snapshots group ─────────────────────────────────────────
    auto* snapGroup = new QGroupBox(tr("Snapshots"), this);
    auto* snapLayout = new QVBoxLayout(snapGroup);
    snapLayout->setSpacing(6);

    m_snapshotCount = new QLabel(tr("0 snapshots"), snapGroup);
    snapLayout->addWidget(m_snapshotCount);

    auto* snapRow = new QHBoxLayout();
    m_btnSnapshot   = new QPushButton(tr("📷 Take"),   snapGroup);
    m_btnOpenFolder = new QPushButton(tr("📁 Folder"), snapGroup);
    snapRow->addWidget(m_btnSnapshot);
    snapRow->addWidget(m_btnOpenFolder);
    snapLayout->addLayout(snapRow);

    main->addWidget(snapGroup);

    // ── Export group ────────────────────────────────────────────
    auto* exportGroup = new QGroupBox(tr("Export Report"), this);
    auto* exportGrid = new QGridLayout(exportGroup);
    exportGrid->setSpacing(6);

    m_btnExportCSV       = new QPushButton(tr("CSV"),         exportGroup);
    m_btnExportJSON      = new QPushButton(tr("JSON"),        exportGroup);
    m_btnExportPlacement = new QPushButton(tr("KiCad .pos"),  exportGroup);
    m_btnExportBOM       = new QPushButton(tr("BOM"),         exportGroup);

    exportGrid->addWidget(m_btnExportCSV,       0, 0);
    exportGrid->addWidget(m_btnExportJSON,      0, 1);
    exportGrid->addWidget(m_btnExportPlacement, 1, 0);
    exportGrid->addWidget(m_btnExportBOM,       1, 1);

    main->addWidget(exportGroup);
    main->addStretch();

    // ── Wiring ──────────────────────────────────────────────────
    connect(m_btnStart,   &QPushButton::clicked, this, &InspectionPanel::startInspectionClicked);
    connect(m_btnBack,    &QPushButton::clicked, this, &InspectionPanel::backClicked);
    connect(m_btnPlaced,  &QPushButton::clicked, this, &InspectionPanel::placedClicked);
    connect(m_btnSkip,    &QPushButton::clicked, this, &InspectionPanel::skipClicked);
    connect(m_btnReset,   &QPushButton::clicked, this, &InspectionPanel::resetClicked);

    connect(m_measureMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx){
        // 0=Off → -1 ; 1=Distance → 0 ; 2=Angle → 1 ; 3=Area → 2 ; 4=PinPitch → 3
        emit measurementModeChanged(idx == 0 ? -1 : idx - 1);
    });
    connect(m_btnClearMeasure, &QPushButton::clicked, this, &InspectionPanel::clearMeasurementsClicked);

    connect(m_btnSnapshot,   &QPushButton::clicked, this, &InspectionPanel::snapshotClicked);
    connect(m_btnOpenFolder, &QPushButton::clicked, this, &InspectionPanel::openSnapshotsFolderClicked);

    connect(m_btnExportCSV,       &QPushButton::clicked, this, [this](){ emit exportRequested("csv");       });
    connect(m_btnExportJSON,      &QPushButton::clicked, this, [this](){ emit exportRequested("json");      });
    connect(m_btnExportPlacement, &QPushButton::clicked, this, [this](){ emit exportRequested("placement"); });
    connect(m_btnExportBOM,       &QPushButton::clicked, this, [this](){ emit exportRequested("bom");       });
}

// ── Slots ─────────────────────────────────────────────────────────

void InspectionPanel::onStepChanged(const QString& reference, const QString& value,
                                     const QString& footprint, const QString& layer,
                                     int currentStep, int totalSteps)
{
    m_currentRefLabel->setText(reference.isEmpty() ? tr("—") : reference);
    m_currentValueLabel->setText(value);
    m_currentFpLabel->setText(footprint);
    m_layerLabel->setText(tr("Layer: %1").arg(layer));
    m_stepLabel->setText(tr("Step %1 / %2").arg(currentStep).arg(totalSteps));
}

void InspectionPanel::onProgress(int placed, int total)
{
    m_progressBar->setRange(0, std::max(1, total));
    m_progressBar->setValue(placed);
}

void InspectionPanel::onMeasurementResult(double valuePixels, double valueMM, const QString& unit)
{
    if (valueMM > 0 && unit == "mm") {
        m_measureResult->setText(tr("Result: %1 mm  (%2 px)")
            .arg(valueMM, 0, 'f', 3)
            .arg(valuePixels, 0, 'f', 1));
    } else if (unit == "deg") {
        m_measureResult->setText(tr("Result: %1°").arg(valuePixels, 0, 'f', 2));
    } else if (unit == "mm²") {
        m_measureResult->setText(tr("Result: %1 mm²  (%2 px²)")
            .arg(valueMM, 0, 'f', 3)
            .arg(valuePixels, 0, 'f', 1));
    } else {
        m_measureResult->setText(tr("Result: %1 px").arg(valuePixels, 0, 'f', 1));
    }
}

void InspectionPanel::onSnapshotTaken(int /*id*/, const QString& /*label*/)
{
    m_snapshotTotal++;
    m_snapshotCount->setText(tr("%1 snapshot(s)").arg(m_snapshotTotal));
}

void InspectionPanel::onIBomLoaded(int componentCount)
{
    QWidget::setEnabled(componentCount > 0);
    m_currentValueLabel->setText(componentCount > 0
        ? tr("%1 components loaded — click Start").arg(componentCount)
        : tr("Load iBOM and click Start"));
}

void InspectionPanel::onAllPlaced()
{
    m_currentRefLabel->setText(tr("✓ Done"));
    m_currentValueLabel->setText(tr("Inspection complete — export report below"));
    m_currentFpLabel->clear();
    m_layerLabel->clear();
}

} // namespace ibom::gui
