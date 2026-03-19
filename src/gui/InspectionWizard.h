#pragma once

#include <QDialog>
#include <QStackedWidget>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QListWidget>
#include <QTableWidget>
#include <vector>
#include <string>

namespace ibom::gui {

class InspectionWizard : public QDialog {
    Q_OBJECT

public:
    explicit InspectionWizard(QWidget* parent = nullptr);
    ~InspectionWizard() override = default;

    enum class Step {
        SelectComponents,
        Alignment,
        Inspection,
        Results,
    };

    void setStep(Step step);
    void addResult(const std::string& reference, const QString& status, const QString& detail);
    void setInspectionProgress(int current, int total);

signals:
    void inspectionStarted(const QStringList& references);
    void inspectionCancelled();
    void inspectionFinished();
    void componentNavigated(const std::string& reference);

private slots:
    void onNext();
    void onBack();
    void onCancel();
    void onResultClicked(int row);

private:
    void buildSelectPage();
    void buildAlignmentPage();
    void buildInspectionPage();
    void buildResultsPage();
    void updateButtons();

    QStackedWidget* m_stack = nullptr;
    QPushButton*    m_btnNext   = nullptr;
    QPushButton*    m_btnBack   = nullptr;
    QPushButton*    m_btnCancel = nullptr;

    // Pages
    QWidget*      m_selectPage     = nullptr;
    QWidget*      m_alignmentPage  = nullptr;
    QWidget*      m_inspectionPage = nullptr;
    QWidget*      m_resultsPage    = nullptr;

    // Select page
    QListWidget*  m_componentList  = nullptr;

    // Inspection page
    QLabel*       m_currentComp    = nullptr;
    QProgressBar* m_progressBar    = nullptr;
    QLabel*       m_progressLabel  = nullptr;

    // Results page
    QTableWidget* m_resultsTable   = nullptr;
    QLabel*       m_summaryLabel   = nullptr;

    Step m_currentStep = Step::SelectComponents;
};

} // namespace ibom::gui
