#pragma once

#include <QDialog>

class QTabWidget;

namespace ibom::gui {

class HelpDialog : public QDialog
{
    Q_OBJECT
public:
    explicit HelpDialog(QWidget* parent = nullptr);

    /// Open the dialog on a specific tab by index
    void showTab(int index);

private:
    void createTabs();
    QTabWidget* m_tabs = nullptr;
};

} // namespace ibom::gui
