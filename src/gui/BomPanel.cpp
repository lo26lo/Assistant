#include "BomPanel.h"
#include "../ibom/IBomData.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <spdlog/spdlog.h>

namespace ibom::gui {

BomPanel::BomPanel(QWidget* parent)
    : QWidget(parent)
{
    buildUI();
}

void BomPanel::buildUI()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // Search row
    auto* searchRow = new QHBoxLayout;
    m_searchBox = new QLineEdit;
    m_searchBox->setPlaceholderText(tr("Search reference, value..."));
    m_searchBox->setClearButtonEnabled(true);
    connect(m_searchBox, &QLineEdit::textChanged, this, &BomPanel::onSearchTextChanged);

    m_layerFilter = new QComboBox;
    m_layerFilter->addItems({"All Layers", "Front (F)", "Back (B)"});
    connect(m_layerFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &BomPanel::onLayerFilterChanged);

    searchRow->addWidget(m_searchBox, 1);
    searchRow->addWidget(m_layerFilter);
    layout->addLayout(searchRow);

    // Table
    m_table = new QTableWidget;
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({"✓", "Ref", "Value", "Footprint", "State"});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_table->setColumnWidth(0, 30);
    m_table->setColumnWidth(1, 60);
    m_table->setColumnWidth(2, 100);
    m_table->setColumnWidth(3, 120);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(true);
    connect(m_table, &QTableWidget::cellClicked, this, &BomPanel::onTableCellClicked);
    layout->addWidget(m_table, 1);

    // Bottom row
    auto* bottomRow = new QHBoxLayout;
    m_progressLabel = new QLabel("0 / 0");
    m_selectAllBtn   = new QPushButton(tr("All"));
    m_deselectAllBtn = new QPushButton(tr("None"));
    m_selectAllBtn->setMaximumWidth(50);
    m_deselectAllBtn->setMaximumWidth(50);
    connect(m_selectAllBtn,   &QPushButton::clicked, this, &BomPanel::onSelectAll);
    connect(m_deselectAllBtn, &QPushButton::clicked, this, &BomPanel::onDeselectAll);

    bottomRow->addWidget(m_progressLabel, 1);
    bottomRow->addWidget(m_selectAllBtn);
    bottomRow->addWidget(m_deselectAllBtn);
    layout->addLayout(bottomRow);
}

void BomPanel::loadBomData(const std::vector<BomGroup>& groups,
                           const std::vector<Component>& components)
{
    m_rows.clear();
    for (const auto& comp : components) {
        BomRow row;
        row.reference  = comp.reference;
        row.value      = comp.value;
        row.footprint  = comp.footprint;
        row.layer      = comp.layer == Layer::Front ? "F" : "B";
        row.state      = "pending";
        row.checked    = false;
        m_rows.push_back(std::move(row));
    }

    populateTable();
    spdlog::info("BOM loaded: {} components", m_rows.size());
}

void BomPanel::clearBomData()
{
    m_rows.clear();
    m_table->setRowCount(0);
    m_progressLabel->setText("0 / 0");
}

void BomPanel::highlightComponent(const std::string& reference)
{
    for (int r = 0; r < m_table->rowCount(); ++r) {
        auto* item = m_table->item(r, 1);
        if (item && item->text().toStdString() == reference) {
            m_table->selectRow(r);
            m_table->scrollToItem(item);
            return;
        }
    }
}

void BomPanel::setComponentState(const std::string& reference, const QString& state)
{
    for (auto& row : m_rows) {
        if (row.reference == reference) {
            row.state = state.toStdString();
            break;
        }
    }
    // Update visible table
    for (int r = 0; r < m_table->rowCount(); ++r) {
        auto* item = m_table->item(r, 1);
        if (item && item->text().toStdString() == reference) {
            auto* stateItem = m_table->item(r, 4);
            if (stateItem) {
                stateItem->setText(state);
                // Color by state
                if (state == "placed")      stateItem->setForeground(QColor(0, 200, 0));
                else if (state == "missing") stateItem->setForeground(QColor(255, 80, 80));
                else if (state == "defect")  stateItem->setForeground(QColor(255, 165, 0));
                else                         stateItem->setForeground(QColor(180, 180, 180));
            }
            return;
        }
    }
}

void BomPanel::setProgress(int placed, int total)
{
    m_progressLabel->setText(QString("%1 / %2 placed").arg(placed).arg(total));
}

QStringList BomPanel::getCheckedReferences() const
{
    QStringList checked;
    for (const auto& row : m_rows) {
        if (row.checked) {
            checked.append(QString::fromStdString(row.reference));
        }
    }
    return checked;
}

// ── Private Slots ────────────────────────────────────────────────

void BomPanel::onSearchTextChanged(const QString& text)
{
    QString layer;
    if (m_layerFilter->currentIndex() == 1) layer = "F";
    else if (m_layerFilter->currentIndex() == 2) layer = "B";
    populateTable(text, layer);
    emit filterChanged(text);
}

void BomPanel::onLayerFilterChanged(int index)
{
    QString layer;
    if (index == 1) layer = "F";
    else if (index == 2) layer = "B";
    populateTable(m_searchBox->text(), layer);
    emit layerFilterChanged(layer);
}

void BomPanel::onTableCellClicked(int row, int col)
{
    if (col == 0) {
        onCheckboxToggled(row);
        return;
    }
    auto* item = m_table->item(row, 1);
    if (item) {
        emit componentSelected(item->text().toStdString());
    }
}

void BomPanel::onCheckboxToggled(int row)
{
    auto* item = m_table->item(row, 1);
    if (!item) return;
    std::string ref = item->text().toStdString();

    for (auto& r : m_rows) {
        if (r.reference == ref) {
            r.checked = !r.checked;
            auto* checkItem = m_table->item(row, 0);
            if (checkItem) {
                checkItem->setText(r.checked ? "☑" : "☐");
            }
            emit componentChecked(ref, r.checked);
            break;
        }
    }
}

void BomPanel::onSelectAll()
{
    for (auto& r : m_rows) r.checked = true;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto* item = m_table->item(row, 0);
        if (item) item->setText("☑");
    }
}

void BomPanel::onDeselectAll()
{
    for (auto& r : m_rows) r.checked = false;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto* item = m_table->item(row, 0);
        if (item) item->setText("☐");
    }
}

// ── Private ──────────────────────────────────────────────────────

void BomPanel::populateTable(const QString& filter, const QString& layer)
{
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);

    QString lowerFilter = filter.toLower();
    int row = 0;

    for (const auto& r : m_rows) {
        // Layer filter
        if (!layer.isEmpty() && QString::fromStdString(r.layer) != layer)
            continue;

        // Text filter
        if (!lowerFilter.isEmpty()) {
            bool match = QString::fromStdString(r.reference).toLower().contains(lowerFilter)
                      || QString::fromStdString(r.value).toLower().contains(lowerFilter)
                      || QString::fromStdString(r.footprint).toLower().contains(lowerFilter);
            if (!match) continue;
        }

        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(r.checked ? "☑" : "☐"));
        m_table->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(r.reference)));
        m_table->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(r.value)));
        m_table->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(r.footprint)));

        auto* stateItem = new QTableWidgetItem(QString::fromStdString(r.state));
        if (r.state == "placed")      stateItem->setForeground(QColor(0, 200, 0));
        else if (r.state == "missing") stateItem->setForeground(QColor(255, 80, 80));
        else if (r.state == "defect")  stateItem->setForeground(QColor(255, 165, 0));
        m_table->setItem(row, 4, stateItem);

        row++;
    }

    m_table->setSortingEnabled(true);
}

} // namespace ibom::gui
