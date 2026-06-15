#include "StatsPanel.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QDateTime>

namespace ibom::gui {

StatsPanel::StatsPanel(QWidget* parent)
    : QWidget(parent)
{
    buildUI();
}

void StatsPanel::buildUI()
{
    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(theme::PanelMargin, 6,
                                   theme::PanelMargin, 6);
    mainLayout->setSpacing(theme::PanelSpacing * 2);

    // ── Left: Progress & Counters ──
    auto* leftGroup = new QGroupBox(tr("Inspection Progress"));
    auto* leftLayout = new QVBoxLayout(leftGroup);
    leftLayout->setContentsMargins(8, 4, 8, 4);
    leftLayout->setSpacing(4);

    m_summaryLabel = new QLabel(tr("No inspection data"));
    m_summaryLabel->setStyleSheet("font-size: 13px; font-weight: bold;");
    leftLayout->addWidget(m_summaryLabel);

    m_progressBar = new QProgressBar;
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFixedHeight(18);
    m_progressBar->setStyleSheet(
        "QProgressBar { background: #1a1a2e; border: none; border-radius: 4px;"
        "  height: 18px; text-align: center; font-size: 10px; color: #8892b8; }"
        "QProgressBar::chunk { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,"
        "  stop:0 #4a68c8, stop:1 #6488e8); border-radius: 4px; }"
    );
    leftLayout->addWidget(m_progressBar);

    auto* counters = new QGridLayout;
    counters->setVerticalSpacing(2);
    counters->setHorizontalSpacing(6);
    counters->addWidget(new QLabel(tr("Placed:")),   0, 0);
    m_placedLabel = new QLabel("0");
    m_placedLabel->setStyleSheet(theme::placedCSS());
    counters->addWidget(m_placedLabel, 0, 1);

    counters->addWidget(new QLabel(tr("Missing:")),  1, 0);
    m_missingLabel = new QLabel("0");
    m_missingLabel->setStyleSheet(theme::missingCSS());
    counters->addWidget(m_missingLabel, 1, 1);

    counters->addWidget(new QLabel(tr("Defects:")),  2, 0);
    m_defectLabel = new QLabel("0");
    m_defectLabel->setStyleSheet(theme::defectCSS());
    counters->addWidget(m_defectLabel, 2, 1);

    counters->addWidget(new QLabel(tr("Pending:")),  3, 0);
    m_pendingLabel = new QLabel("0");
    counters->addWidget(m_pendingLabel, 3, 1);

    leftLayout->addLayout(counters);
    leftLayout->addStretch();
    mainLayout->addWidget(leftGroup);

    // ── Center: Performance ──
    auto* perfGroup = new QGroupBox(tr("Performance"));
    auto* perfLayout = new QVBoxLayout(perfGroup);
    perfLayout->setContentsMargins(8, 4, 8, 4);
    perfLayout->setSpacing(2);

    auto* perfGrid = new QGridLayout;
    perfGrid->setVerticalSpacing(2);
    perfGrid->setHorizontalSpacing(8);
    perfGrid->addWidget(new QLabel(tr("Camera FPS:")),    0, 0);
    m_fpsLabel = new QLabel("0.0");
    perfGrid->addWidget(m_fpsLabel, 0, 1);

    perfGrid->addWidget(new QLabel(tr("Inference:")),     1, 0);
    m_inferenceLabel = new QLabel("-- ms");
    perfGrid->addWidget(m_inferenceLabel, 1, 1);

    perfGrid->addWidget(new QLabel(tr("GPU Memory:")),    2, 0);
    m_gpuMemLabel = new QLabel("-- / -- MB");
    perfGrid->addWidget(m_gpuMemLabel, 2, 1);

    perfGrid->addWidget(new QLabel(tr("Scale:")),          3, 0);
    m_scaleLabel = new QLabel("-- px/mm");
    perfGrid->addWidget(m_scaleLabel, 3, 1);

    perfGrid->addWidget(new QLabel(tr("Focus:")),          4, 0);
    m_focusLabel = new QLabel("--");
    m_focusLabel->setToolTip(tr("Image sharpness (Laplacian variance). "
                                "Turn the focus ring until the value peaks; "
                                "green = sharp enough for dataset capture."));
    perfGrid->addWidget(m_focusLabel, 4, 1);

    perfGrid->addWidget(new QLabel(tr("Distance:")),       5, 0);
    m_distanceLabel = new QLabel("—");
    m_distanceLabel->setToolTip(tr("Distance from a depth camera (RealSense) "
                                   "to the board. Drives automatic px/mm scale."));
    perfGrid->addWidget(m_distanceLabel, 5, 1);

    perfGrid->addWidget(new QLabel(tr("Calibration:")),    6, 0);
    m_calibLabel = new QLabel("—");
    m_calibLabel->setToolTip(tr("USB microscope: OpenCV checkerboard RMS + px/mm.\n"
                                "RealSense: factory intrinsics (focal length)."));
    perfGrid->addWidget(m_calibLabel, 6, 1);

    perfLayout->addLayout(perfGrid);
    perfLayout->addStretch();
    mainLayout->addWidget(perfGroup);

    // ── Right: Event Log (runtime logs + defects) ──
    auto* defectGroup = new QGroupBox(tr("Event Log"));
    auto* defectLayout = new QVBoxLayout(defectGroup);
    defectLayout->setContentsMargins(8, 4, 8, 4);
    defectLayout->setSpacing(2);

    m_defectTable = new QTableWidget;
    m_defectTable->setColumnCount(3);
    m_defectTable->setHorizontalHeaderLabels({tr("Time"), tr("Level"), tr("Message")});
    m_defectTable->horizontalHeader()->setStretchLastSection(true);
    m_defectTable->verticalHeader()->setVisible(false);
    m_defectTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_defectTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_defectTable->setAlternatingRowColors(true);
    m_defectTable->setColumnWidth(0, 70);
    m_defectTable->setColumnWidth(1, 60);
    // A defect row stashes its component reference in column 0's UserRole so a
    // click can still navigate to it; plain log rows leave it empty.
    connect(m_defectTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* item = m_defectTable->item(row, 0);
        if (!item) return;
        const QString ref = item->data(Qt::UserRole).toString();
        if (!ref.isEmpty()) emit defectClicked(ref.toStdString());
    });
    defectLayout->addWidget(m_defectTable);
    mainLayout->addWidget(defectGroup, 1);
}

void StatsPanel::appendEventRow(const QString& level, const QString& message,
                                const QColor& color, const QString& defectRef)
{
    // Cap the log: drop the oldest row once we exceed the limit.
    if (m_defectTable->rowCount() >= kMaxEventRows)
        m_defectTable->removeRow(0);

    const int row = m_defectTable->rowCount();
    m_defectTable->insertRow(row);

    const QString time = QDateTime::currentDateTime().toString("HH:mm:ss");
    auto* timeItem = new QTableWidgetItem(time);
    if (!defectRef.isEmpty())
        timeItem->setData(Qt::UserRole, defectRef);
    m_defectTable->setItem(row, 0, timeItem);

    auto* levelItem = new QTableWidgetItem(level);
    levelItem->setForeground(color);
    m_defectTable->setItem(row, 1, levelItem);

    auto* msgItem = new QTableWidgetItem(message);
    msgItem->setForeground(color);
    m_defectTable->setItem(row, 2, msgItem);

    m_defectTable->scrollToBottom();
}

void StatsPanel::addLogEntry(int level, const QString& /*logger*/, const QString& message)
{
    // spdlog levels: trace=0, debug=1, info=2, warn=3, err=4, critical=5
    QString label;
    QColor  color;
    switch (level) {
        case 5: label = "CRIT"; color = theme::missingColor(); break;
        case 4: label = "ERR";  color = theme::missingColor(); break;
        case 3: label = "WARN"; color = theme::defectColor();  break;
        default: label = "INFO"; color = theme::pendingColor(); break;
    }
    appendEventRow(label, message, color);
}

void StatsPanel::resetStats()
{
    m_total = m_placed = m_missing = m_defect = 0;
    m_placedLabel->setText("0");
    m_missingLabel->setText("0");
    m_defectLabel->setText("0");
    m_pendingLabel->setText("0");
    m_progressBar->setValue(0);
    m_defectTable->setRowCount(0);
    updateSummaryLabel();
}

void StatsPanel::setTotalComponents(int total)
{
    m_total = total;
    updateProgress();
}

void StatsPanel::incrementPlaced()
{
    m_placed++;
    m_placedLabel->setText(QString::number(m_placed));
    updateProgress();
}

void StatsPanel::incrementMissing()
{
    m_missing++;
    m_missingLabel->setText(QString::number(m_missing));
    updateProgress();
}

void StatsPanel::incrementDefect()
{
    m_defect++;
    m_defectLabel->setText(QString::number(m_defect));
    updateProgress();
}

void StatsPanel::setFps(double fps)
{
    m_fpsLabel->setText(QString("%1").arg(fps, 0, 'f', 1));
}

void StatsPanel::setInferenceTime(double ms)
{
    m_inferenceLabel->setText(QString("%1 ms").arg(ms, 0, 'f', 1));
}

void StatsPanel::setGpuMemory(size_t usedMB, size_t totalMB)
{
    m_gpuMemLabel->setText(QString("%1 / %2 MB").arg(usedMB).arg(totalMB));
}

void StatsPanel::setScale(double pixelsPerMm)
{
    if (pixelsPerMm > 0)
        m_scaleLabel->setText(QString("%1 px/mm").arg(pixelsPerMm, 0, 'f', 1));
    else
        m_scaleLabel->setText("-- px/mm");
}

void StatsPanel::setDistance(double mm)
{
    if (mm > 0)
        m_distanceLabel->setText(QString("%1 mm").arg(mm, 0, 'f', 1));
    else
        m_distanceLabel->setText("—");
}

void StatsPanel::setSharpness(double variance, bool good)
{
    m_focusLabel->setText(QString::number(variance, 'f', 0));
    m_focusLabel->setStyleSheet(good ? theme::placedCSS() : theme::defectCSS());
}

void StatsPanel::setCalibration(double rmsOrFx, double ppmm, bool isFactory)
{
    if (isFactory) {
        // RealSense: show factory focal length
        m_calibLabel->setText(QString("Factory  fx=%1 px").arg(rmsOrFx, 0, 'f', 1));
        m_calibLabel->setStyleSheet(theme::placedCSS());
    } else if (ppmm > 0) {
        // V4L2 calibrated
        m_calibLabel->setText(QString("RMS %1  %2 px/mm")
            .arg(rmsOrFx, 0, 'f', 2).arg(ppmm, 0, 'f', 2));
        m_calibLabel->setStyleSheet(rmsOrFx < 1.0 ? theme::placedCSS() : theme::defectCSS());
    } else {
        m_calibLabel->setText("—");
        m_calibLabel->setStyleSheet({});
    }
}

void StatsPanel::addDefectEntry(const std::string& reference, const std::string& type)
{
    const QString ref = QString::fromStdString(reference);
    const QString msg = QString("%1 — %2").arg(ref, QString::fromStdString(type));
    // Stash the reference so a click on this row navigates to the component.
    appendEventRow(tr("DEFECT"), msg, theme::defectColor(), ref);
}

void StatsPanel::updateProgress()
{
    int done = m_placed + m_missing + m_defect;
    int pending = std::max(0, m_total - done);
    m_pendingLabel->setText(QString::number(pending));

    if (m_total > 0) {
        int pct = (done * 100) / m_total;
        m_progressBar->setValue(pct);
    }
    updateSummaryLabel();
}

void StatsPanel::updateSummaryLabel()
{
    if (m_total == 0) {
        m_summaryLabel->setText(tr("No inspection data"));
        return;
    }
    int done = m_placed + m_missing + m_defect;
    double yieldPct = m_total > 0 ? (m_placed * 100.0 / m_total) : 0;
    m_summaryLabel->setText(tr("%1/%2 inspected — Yield: %3%")
                                .arg(done).arg(m_total)
                                .arg(yieldPct, 0, 'f', 1));
}

} // namespace ibom::gui
