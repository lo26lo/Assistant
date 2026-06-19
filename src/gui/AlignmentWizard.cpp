#include "AlignmentWizard.h"

#include <QVBoxLayout>
#include <QRadioButton>
#include <QButtonGroup>
#include <QLabel>
#include <QPushButton>
#include <QWizardPage>

namespace ibom::gui {

// ---------------------------------------------------------------------------
// Page 1 — method selection
// ---------------------------------------------------------------------------
class AlignMethodPage : public QWizardPage {
public:
    explicit AlignMethodPage(AlignmentWizard* wiz)
        : QWizardPage(wiz), m_wiz(wiz)
    {
        setTitle(tr("Choose an alignment method"));
        setSubTitle(tr("How should the iBOM overlay be matched to the live board?"));

        auto* layout = new QVBoxLayout(this);
        m_group = new QButtonGroup(this);

        struct Opt { AlignmentWizard::Method m; const char* label; const char* desc; };
        const Opt opts[] = {
            {AlignmentWizard::AutoAlign, QT_TR_NOOP("Auto-Align (Beta)"),
             QT_TR_NOOP("No clicking. Detects the board outline automatically "
                        "(depth-assisted on RealSense). Best with the whole board "
                        "visible and lifted off the table.")},
            {AlignmentWizard::FourCorners, QT_TR_NOOP("4 Corners"),
             QT_TR_NOOP("Click the 4 board corners in the camera image. Best for "
                        "rectangular boards fully in view.")},
            {AlignmentWizard::TwoComponents, QT_TR_NOOP("2 Components"),
             QT_TR_NOOP("Click 2 known components (their centers). Good for narrow "
                        "FOV / microscopes. Similarity transform — no perspective.")},
            {AlignmentWizard::MultiComponent, QT_TR_NOOP("Multi-Component (Beta)"),
             QT_TR_NOOP("Mark several components by pin 1 or 2 opposite corners. "
                        "Works on non-rectangular boards; ≥4 corrects perspective.")},
        };

        for (const auto& o : opts) {
            auto* rb = new QRadioButton(tr(o.label), this);
            rb->setProperty("methodId", static_cast<int>(o.m));
            m_group->addButton(rb, static_cast<int>(o.m));
            layout->addWidget(rb);
            auto* d = new QLabel(tr(o.desc), this);
            d->setWordWrap(true);
            d->setStyleSheet("color:#8892b8; font-size:11px; margin-left:22px; margin-bottom:6px;");
            layout->addWidget(d);
        }
        layout->addStretch();
    }

    void initializePage() override {
        // Preselect the recommended method for the active backend.
        const int rec = m_wiz->backendIsRealSense()
                            ? AlignmentWizard::AutoAlign
                            : AlignmentWizard::TwoComponents;
        if (auto* b = m_group->button(rec)) b->setChecked(true);
    }

    bool validatePage() override {
        if (auto* b = m_group->checkedButton()) {
            m_wiz->m_method = b->property("methodId").toInt();
            return true;
        }
        return false;
    }

    bool isComplete() const override { return m_group->checkedButton() != nullptr; }

private:
    AlignmentWizard* m_wiz;
    QButtonGroup*    m_group = nullptr;
};

// ---------------------------------------------------------------------------
// Page 2 — run + live result
// ---------------------------------------------------------------------------
class AlignRunPage : public QWizardPage {
public:
    explicit AlignRunPage(AlignmentWizard* wiz)
        : QWizardPage(wiz), m_wiz(wiz)
    {
        setTitle(tr("Run the alignment"));
        auto* layout = new QVBoxLayout(this);

        m_instructions = new QLabel(this);
        m_instructions->setWordWrap(true);
        layout->addWidget(m_instructions);

        m_startBtn = new QPushButton(tr("Start alignment"), this);
        connect(m_startBtn, &QPushButton::clicked, this, [this]() {
            m_result.clear();
            m_have = false;
            m_status->setText(tr("Waiting for alignment to complete… "
                                 "(do the clicks in the camera / BOM panel)"));
            emit completeChanged();
            emit m_wiz->startRequested(m_wiz->m_method);
        });
        layout->addWidget(m_startBtn);

        m_status = new QLabel(this);
        m_status->setWordWrap(true);
        m_status->setStyleSheet("color:#8892b8; font-size:11px;");
        layout->addWidget(m_status);
        layout->addStretch();
    }

    void initializePage() override {
        m_have = false;
        m_result.clear();
        m_status->clear();
        const char* steps = "";
        switch (m_wiz->m_method) {
        case AlignmentWizard::AutoAlign:
            steps = QT_TR_NOOP("Make sure the whole board is visible (and lifted off "
                               "the table on RealSense). Click \"Start alignment\" — "
                               "no further input needed.");
            m_startBtn->setText(tr("Run Auto-Align"));
            break;
        case AlignmentWizard::FourCorners:
            steps = QT_TR_NOOP("Click \"Start alignment\", then click the 4 board "
                               "corners in the camera image (top-left, top-right, "
                               "bottom-right, bottom-left).");
            m_startBtn->setText(tr("Start 4-Corner align"));
            break;
        case AlignmentWizard::TwoComponents:
            steps = QT_TR_NOOP("Click \"Start alignment\". Select component #1 in the "
                               "BOM panel, click its center in the image; repeat for "
                               "component #2.");
            m_startBtn->setText(tr("Start 2-Component align"));
            break;
        case AlignmentWizard::MultiComponent:
            steps = QT_TR_NOOP("Click \"Start alignment\". For each landmark: select it "
                               "in the BOM panel, choose Pin 1 (1 click) or 2 opposite "
                               "corners (2 clicks). The PCB Map marks pin 1 (red). Mark "
                               "≥2 (≥4 for perspective), then press \"Start alignment\" "
                               "again to finish.");
            m_startBtn->setText(tr("Start Multi-Component align"));
            break;
        }
        m_instructions->setText(tr(steps));
    }

    bool isComplete() const override { return m_have; }

    void setResult(const QString& summary) {
        m_result = summary;
        m_have = true;
        m_status->setText(tr("✔ %1").arg(summary));
        emit completeChanged();
    }

    QString result() const { return m_result; }

private:
    AlignmentWizard* m_wiz;
    QLabel*  m_instructions = nullptr;
    QPushButton* m_startBtn = nullptr;
    QLabel*  m_status       = nullptr;
    QString  m_result;
    bool     m_have = false;
};

// ---------------------------------------------------------------------------
// Page 3 — summary
// ---------------------------------------------------------------------------
class AlignSummaryPage : public QWizardPage {
public:
    explicit AlignSummaryPage(AlignmentWizard* wiz)
        : QWizardPage(wiz), m_wiz(wiz)
    {
        setTitle(tr("Alignment summary"));
        setSubTitle(tr("Result of the alignment you just performed."));
        auto* layout = new QVBoxLayout(this);
        m_summary = new QLabel(this);
        m_summary->setWordWrap(true);
        m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(m_summary);
        m_hint = new QLabel(
            tr("Tip: enable \"Live Tracking Mode\" to keep the overlay locked as "
               "the board moves. You can re-run the wizard anytime to refine."), this);
        m_hint->setWordWrap(true);
        m_hint->setStyleSheet("color:#8892b8; font-size:11px; margin-top:8px;");
        layout->addWidget(m_hint);
        layout->addStretch();
    }

    void initializePage() override {
        const QString r = m_wiz->lastResult();
        m_summary->setText(r.isEmpty()
            ? tr("No result was reported — the alignment may have been cancelled.")
            : tr("<b>%1</b>").arg(r));
    }

private:
    AlignmentWizard* m_wiz;
    QLabel* m_summary = nullptr;
    QLabel* m_hint    = nullptr;
};

// ---------------------------------------------------------------------------
// Wizard
// ---------------------------------------------------------------------------
AlignmentWizard::AlignmentWizard(QWidget* parent)
    : QWizard(parent)
{
    setWindowTitle(tr("Alignment Assistant"));
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::NoBackButtonOnStartPage, true);
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumSize(440, 360);

    addPage(new AlignMethodPage(this));
    m_runPage = new AlignRunPage(this);
    addPage(m_runPage);
    addPage(new AlignSummaryPage(this));
}

void AlignmentWizard::setBackendIsRealSense(bool isRealSense)
{
    m_isRealSense = isRealSense;
}

void AlignmentWizard::reportResult(const QString& summary)
{
    m_result = summary;
    if (m_runPage) m_runPage->setResult(summary);
}

} // namespace ibom::gui
