#include "StatsPanel.h"

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
    mainLayout->setContentsMargins(8, 6, 8, 6);
    mainLayout->setSpacing(8);

    // ── Left: Progress & Counters ──
    auto* leftGroup = new QGroupBox(tr("Inspection Progress"));
    auto* leftLayout = new QVBoxLayout(leftGroup);

    m_summaryLabel = new QLabel(tr("No inspection data"));
    m_summaryLabel->setStyleSheet("font-size: 13px; font-weight: bold;");
    leftLayout->addWidget(m_summaryLabel);

    m_progressBar = new QProgressBar;
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    leftLayout->addWidget(m_progressBar);

    auto* counters = new QGridLayout;
    counters->addWidget(new QLabel(tr("Placed:")),   0, 0);
    m_placedLabel = new QLabel("0");
    m_placedLabel->setStyleSheet("color: #40c040; font-weight: bold;");
    counters->addWidget(m_placedLabel, 0, 1);

    counters->addWidget(new QLabel(tr("Missing:")),  1, 0);
    m_missingLabel = new QLabel("0");
    m_missingLabel->setStyleSheet("color: #ff5050; font-weight: bold;");
    counters->addWidget(m_missingLabel, 1, 1);

    counters->addWidget(new QLabel(tr("Defects:")),  2, 0);
    m_defectLabel = new QLabel("0");
    m_defectLabel->setStyleSheet("color: #ffa500; font-weight: bold;");
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

    auto* perfGrid = new QGridLayout;
    perfGrid->addWidget(new QLabel(tr("Camera FPS:")),    0, 0);
    m_fpsLabel = new QLabel("--");
    perfGrid->addWidget(m_fpsLabel, 0, 1);

    perfGrid->addWidget(new QLabel(tr("Inference:")),     1, 0);
    m_inferenceLabel = new QLabel("-- ms");
    perfGrid->addWidget(m_inferenceLabel, 1, 1);

    perfGrid->addWidget(new QLabel(tr("GPU Memory:")),    2, 0);
    m_gpuMemLabel = new QLabel("-- / -- MB");
    perfGrid->addWidget(m_gpuMemLabel, 2, 1);

    perfLayout->addLayout(perfGrid);
    perfLayout->addStretch();
    mainLayout->addWidget(perfGroup);

    // ── Right: Defect Log ──
    auto* defectGroup = new QGroupBox(tr("Defect Log"));
    auto* defectLayout = new QVBoxLayout(defectGroup);

    m_defectTable = new QTableWidget;
    m_defectTable->setColumnCount(3);
    m_defectTable->setHorizontalHeaderLabels({"Time", "Reference", "Type"});
    m_defectTable->horizontalHeader()->setStretchLastSection(true);
    m_defectTable->verticalHeader()->setVisible(false);
    m_defectTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_defectTable->setAlternatingRowColors(true);
    connect(m_defectTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* item = m_defectTable->item(row, 1);
        if (item) emit defectClicked(item->text().toStdString());
    });
    defectLayout->addWidget(m_defectTable);
    mainLayout->addWidget(defectGroup, 1);
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

void StatsPanel::addDefectEntry(const std::string& reference, const std::string& type)
{
    int row = m_defectTable->rowCount();
    m_defectTable->insertRow(row);

    QString time = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_defectTable->setItem(row, 0, new QTableWidgetItem(time));
    m_defectTable->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(reference)));

    auto* typeItem = new QTableWidgetItem(QString::fromStdString(type));
    typeItem->setForeground(QColor(255, 165, 0));
    m_defectTable->setItem(row, 2, typeItem);

    // Scroll to latest
    m_defectTable->scrollToBottom();
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
