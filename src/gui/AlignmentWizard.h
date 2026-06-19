#pragma once

#include <QWizard>
#include <QString>

class QRadioButton;
class QLabel;
class QPushButton;

namespace ibom::gui {

/**
 * Step-by-step alignment assistant.
 *
 * A single entry point ("Alignment…") that walks the user through the choice
 * of alignment method, gives method-specific instructions, lets them run the
 * (interactive) alignment, and finishes on a summary page showing what was
 * obtained (model, scale, error).
 *
 * The wizard is **non-modal**: the actual alignment still happens by clicking
 * in the camera view / BOM panel, so the window must not block the main UI.
 * It only orchestrates and reports — the real work is done by the existing
 * alignment code paths in Application, triggered via startRequested().
 *
 * Flow:
 *   1. MethodPage  — pick one of the 4 methods (recommended one preselected).
 *   2. RunPage     — instructions + "Start alignment" button (emits
 *                    startRequested). Becomes "complete" once Application calls
 *                    reportResult(), enabling Next.
 *   3. SummaryPage — shows the reported result.
 */
class AlignmentWizard : public QWizard {
    Q_OBJECT

public:
    enum Method { FourCorners = 0, TwoComponents = 1, MultiComponent = 2, AutoAlign = 3 };

    explicit AlignmentWizard(QWidget* parent = nullptr);

    /// Tune recommendations/labels for the active backend. Call before show().
    void setBackendIsRealSense(bool isRealSense);

    /// Called by Application when an alignment finishes (any method). Records the
    /// human-readable summary, marks the run page complete, and surfaces the text.
    void reportResult(const QString& summary);

    /// Currently selected method (valid from the run page onward).
    int selectedMethod() const { return m_method; }
    bool backendIsRealSense() const { return m_isRealSense; }
    QString lastResult() const { return m_result; }

signals:
    /// User asked to start the chosen alignment. @p method is a Method value.
    void startRequested(int method);

private:
    friend class AlignMethodPage;
    friend class AlignRunPage;
    friend class AlignSummaryPage;

    int     m_method      = AutoAlign;
    bool    m_isRealSense = false;
    QString m_result;

    class AlignRunPage* m_runPage = nullptr;
};

} // namespace ibom::gui
