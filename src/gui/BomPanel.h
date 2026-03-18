#pragma once

#include <QWidget>
#include <QTableWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <vector>
#include <string>

namespace ibom {
struct Component;
struct BomGroup;
}

namespace ibom::gui {

class BomPanel : public QWidget {
    Q_OBJECT

public:
    explicit BomPanel(QWidget* parent = nullptr);
    ~BomPanel() override = default;

    void loadBomData(const std::vector<BomGroup>& groups,
                     const std::vector<Component>& components);
    void clearBomData();

    void highlightComponent(const std::string& reference);
    void setComponentState(const std::string& reference, const QString& state);
    void setProgress(int placed, int total);

    QStringList getCheckedReferences() const;

signals:
    void componentSelected(const std::string& reference);
    void componentChecked(const std::string& reference, bool checked);
    void filterChanged(const QString& filter);
    void layerFilterChanged(const QString& layer);

private slots:
    void onSearchTextChanged(const QString& text);
    void onLayerFilterChanged(int index);
    void onTableCellClicked(int row, int col);
    void onCheckboxToggled(int row);
    void onSelectAll();
    void onDeselectAll();

private:
    void buildUI();
    void populateTable(const QString& filter = QString(),
                       const QString& layer = QString());

    // Widgets
    QLineEdit*    m_searchBox     = nullptr;
    QComboBox*    m_layerFilter   = nullptr;
    QTableWidget* m_table         = nullptr;
    QLabel*       m_progressLabel = nullptr;
    QPushButton*  m_selectAllBtn  = nullptr;
    QPushButton*  m_deselectAllBtn = nullptr;

    // Data
    struct BomRow {
        std::string reference;
        std::string value;
        std::string footprint;
        std::string layer;
        std::string state;
        bool        checked = false;
    };
    std::vector<BomRow> m_rows;
};

} // namespace ibom::gui
