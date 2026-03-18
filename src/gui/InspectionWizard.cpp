#include "InspectionWizard.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <spdlog/spdlog.h>

namespace ibom::gui {

InspectionWizard::InspectionWizard(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("PCB Inspection Wizard"));
    setMinimumSize(600, 450);

    auto* mainLayout = new QVBoxLayout(this);

    m_stack = new QStackedWidget;
    buildSelectPage();
    buildAlignmentPage();
    buildInspectionPage();
    buildResultsPage();
    mainLayout->addWidget(m_stack, 1);

    // Buttons
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    m_btnBack   = new QPushButton(tr("< Back"));
    m_btnNext   = new QPushButton(tr("Next >"));
    m_btnCancel = new QPushButton(tr("Cancel"));
    connect(m_btnBack,   &QPushButton::clicked, this, &InspectionWizard::onBack);
    connect(m_btnNext,   &QPushButton::clicked, this, &InspectionWizard::onNext);
    connect(m_btnCancel, &QPushButton::clicked, this, &InspectionWizard::onCancel);
    btnRow->addWidget(m_btnBack);
    btnRow->addWidget(m_btnNext);
    btnRow->addWidget(m_btnCancel);
    mainLayout->addLayout(btnRow);

    setStep(Step::SelectComponents);
}

void InspectionWizard::setStep(Step step)
{
    m_currentStep = step;
    m_stack->setCurrentIndex(static_cast<int>(step));
    updateButtons();
}

void InspectionWizard::addResult(const std::string& reference,
                                  const QString& status,
                                  const QString& detail)
{
    if (!m_resultsTable) return;

    int row = m_resultsTable->rowCount();
    m_resultsTable->insertRow(row);
    m_resultsTable->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(reference)));

    auto* statusItem = new QTableWidgetItem(status);
    if (status == "OK" || status == "Placed")
        statusItem->setForeground(QColor(0, 200, 0));
    else if (status == "Missing")
        statusItem->setForeground(QColor(255, 80, 80));
    else if (status == "Defect")
        statusItem->setForeground(QColor(255, 165, 0));
    m_resultsTable->setItem(row, 1, statusItem);
    m_resultsTable->setItem(row, 2, new QTableWidgetItem(detail));
}

void InspectionWizard::setInspectionProgress(int current, int total)
{
    if (m_progressBar) {
        m_progressBar->setMaximum(total);
        m_progressBar->setValue(current);
    }
    if (m_progressLabel) {
        m_progressLabel->setText(tr("Inspecting %1 of %2").arg(current).arg(total));
    }
}

// ── Build Pages ──────────────────────────────────────────────────

void InspectionWizard::buildSelectPage()
{
    m_selectPage = new QWidget;
    auto* layout = new QVBoxLayout(m_selectPage);

    layout->addWidget(new QLabel(tr("<h3>Select Components to Inspect</h3>"
                                     "<p>Choose which components to include in the inspection run.</p>")));

    m_componentList = new QListWidget;
    m_componentList->setSelectionMode(QAbstractItemView::MultiSelection);
    layout->addWidget(m_componentList, 1);

    auto* btnRow = new QHBoxLayout;
    auto* selectAll = new QPushButton(tr("Select All"));
    auto* deselectAll = new QPushButton(tr("Deselect All"));
    connect(selectAll,   &QPushButton::clicked, m_componentList, &QListWidget::selectAll);
    connect(deselectAll, &QPushButton::clicked, m_componentList, &QListWidget::clearSelection);
    btnRow->addWidget(selectAll);
    btnRow->addWidget(deselectAll);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    m_stack->addWidget(m_selectPage);
}

void InspectionWizard::buildAlignmentPage()
{
    m_alignmentPage = new QWidget;
    auto* layout = new QVBoxLayout(m_alignmentPage);

    layout->addWidget(new QLabel(tr("<h3>Board Alignment</h3>"
                                     "<p>Click on 4 known points on the PCB to establish "
                                     "the coordinate mapping between the camera and the iBOM data.</p>"
                                     "<p>Click matching points on the board outline corners.</p>")));

    auto* placeholder = new QLabel(tr("[Camera view with alignment overlay will appear here]"));
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setMinimumHeight(200);
    placeholder->setStyleSheet("border: 1px dashed #666; border-radius: 4px;");
    layout->addWidget(placeholder, 1);

    m_stack->addWidget(m_alignmentPage);
}

void InspectionWizard::buildInspectionPage()
{
    m_inspectionPage = new QWidget;
    auto* layout = new QVBoxLayout(m_inspectionPage);

    layout->addWidget(new QLabel(tr("<h3>Inspection in Progress</h3>")));

    m_currentComp = new QLabel(tr("Preparing..."));
    m_currentComp->setStyleSheet("font-size: 14px; font-weight: bold;");
    layout->addWidget(m_currentComp);

    m_progressBar = new QProgressBar;
    m_progressBar->setRange(0, 100);
    layout->addWidget(m_progressBar);

    m_progressLabel = new QLabel("0 / 0");
    layout->addWidget(m_progressLabel);

    layout->addStretch();

    m_stack->addWidget(m_inspectionPage);
}

void InspectionWizard::buildResultsPage()
{
    m_resultsPage = new QWidget;
    auto* layout = new QVBoxLayout(m_resultsPage);

    layout->addWidget(new QLabel(tr("<h3>Inspection Results</h3>")));

    m_summaryLabel = new QLabel;
    layout->addWidget(m_summaryLabel);

    m_resultsTable = new QTableWidget;
    m_resultsTable->setColumnCount(3);
    m_resultsTable->setHorizontalHeaderLabels({"Reference", "Status", "Detail"});
    m_resultsTable->horizontalHeader()->setStretchLastSection(true);
    m_resultsTable->verticalHeader()->setVisible(false);
    m_resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultsTable->setAlternatingRowColors(true);
    connect(m_resultsTable, &QTableWidget::cellClicked, this, &InspectionWizard::onResultClicked);
    layout->addWidget(m_resultsTable, 1);

    m_stack->addWidget(m_resultsPage);
}

// ── Slots ────────────────────────────────────────────────────────

void InspectionWizard::onNext()
{
    int next = static_cast<int>(m_currentStep) + 1;
    if (next > static_cast<int>(Step::Results)) {
        emit inspectionFinished();
        accept();
        return;
    }

    if (m_currentStep == Step::SelectComponents) {
        // Collect selected references
        QStringList refs;
        for (auto* item : m_componentList->selectedItems()) {
            refs << item->text();
        }
        if (refs.isEmpty()) return;
    }

    if (m_currentStep == Step::Alignment) {
        // Start inspection
        QStringList refs;
        for (auto* item : m_componentList->selectedItems()) {
            refs << item->text();
        }
        emit inspectionStarted(refs);
    }

    setStep(static_cast<Step>(next));
}

void InspectionWizard::onBack()
{
    int prev = static_cast<int>(m_currentStep) - 1;
    if (prev < 0) return;
    setStep(static_cast<Step>(prev));
}

void InspectionWizard::onCancel()
{
    emit inspectionCancelled();
    reject();
}

void InspectionWizard::onResultClicked(int row)
{
    auto* item = m_resultsTable->item(row, 0);
    if (item) {
        emit componentNavigated(item->text().toStdString());
    }
}

void InspectionWizard::updateButtons()
{
    m_btnBack->setEnabled(m_currentStep != Step::SelectComponents);

    switch (m_currentStep) {
    case Step::SelectComponents:
        m_btnNext->setText(tr("Next >"));
        break;
    case Step::Alignment:
        m_btnNext->setText(tr("Start Inspection >"));
        break;
    case Step::Inspection:
        m_btnNext->setEnabled(false);
        m_btnNext->setText(tr("Inspecting..."));
        break;
    case Step::Results:
        m_btnNext->setEnabled(true);
        m_btnNext->setText(tr("Finish"));
        break;
    }
}

} // namespace ibom::gui
