#include "DatasetPanel.h"

#include <QGroupBox>
#include <QFormLayout>
#include <QVBoxLayout>

namespace ibom::gui {

DatasetPanel::DatasetPanel(QWidget* parent)
    : QWidget(parent)
{
    buildUI();
}

void DatasetPanel::buildUI()
{
    auto* root = new QVBoxLayout(this);

    // ── Session ─────────────────────────────────────────────────
    auto* sessionBox = new QGroupBox(tr("Capture session"), this);
    auto* form = new QFormLayout(sessionBox);

    m_boardEdit = new QLineEdit(this);
    m_boardEdit->setPlaceholderText(tr("e.g. controller-v2"));
    form->addRow(tr("Board:"), m_boardEdit);

    m_lightingBox = new QComboBox(this);
    m_lightingBox->addItems({"ring", "lateral", "ambient", "low"});
    m_lightingBox->setEditable(true);
    form->addRow(tr("Lighting:"), m_lightingBox);

    m_startStopBtn = new QPushButton(tr("Start capture"), this);
    form->addRow(m_startStopBtn);

    connect(m_startStopBtn, &QPushButton::clicked, this, [this]() {
        if (m_running)
            emit stopRequested();
        else
            emit startRequested(m_boardEdit->text(), m_lightingBox->currentText());
    });
    root->addWidget(sessionBox);

    // ── Gates ───────────────────────────────────────────────────
    auto* gatesBox = new QGroupBox(tr("Quality gates"), this);
    auto* gates = new QVBoxLayout(gatesBox);
    m_gateTracking  = new QLabel(this);
    m_gateReproj    = new QLabel(this);
    m_gateSharpness = new QLabel(this);
    m_gateExposure  = new QLabel(this);
    m_gateFresh     = new QLabel(this);
    for (auto* l : {m_gateTracking, m_gateReproj, m_gateSharpness,
                    m_gateExposure, m_gateFresh})
        gates->addWidget(l);
    root->addWidget(gatesBox);

    // ── Counters ────────────────────────────────────────────────
    auto* countBox = new QGroupBox(tr("Session"), this);
    auto* counts = new QVBoxLayout(countBox);
    m_savedLabel    = new QLabel(this);
    m_rejectedLabel = new QLabel(this);
    m_labelsLabel   = new QLabel(this);
    counts->addWidget(m_savedLabel);
    counts->addWidget(m_rejectedLabel);
    counts->addWidget(m_labelsLabel);
    root->addWidget(countBox);

    m_statusLabel = new QLabel(tr("Live tracking must be active for capture."), this);
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    root->addStretch();

    // Initial state — all gates idle.
    updateStatus(features::DatasetStatus{});
}

void DatasetPanel::setGate(QLabel* label, const QString& name, bool ok,
                           const QString& detail)
{
    // Plain "●" + color: emoji glyphs are missing from the container fonts.
    label->setText(QString("<span style=\"color:%1\">●</span> %2 — %3")
                       .arg(ok ? "#4caf50" : "#e53935", name, detail));
}

void DatasetPanel::updateStatus(features::DatasetStatus s)
{
    setGate(m_gateTracking, tr("Tracking"), s.gateTracking,
            tr("%1 inliers").arg(s.inliers));
    setGate(m_gateReproj, tr("Reprojection"), s.gateReproj,
            QString("%1 px").arg(s.reprojErrPx, 0, 'f', 2));
    setGate(m_gateSharpness, tr("Sharpness"), s.gateSharpness,
            QString::number(s.sharpness, 'f', 0));
    setGate(m_gateExposure, tr("Exposure"), s.gateExposure,
            tr("%1% bad px").arg(s.badExposureFrac * 100.0, 0, 'f', 1));
    setGate(m_gateFresh, tr("Homography"), s.gateFresh,
            s.homographyAgeMs > 1e6 ? tr("no update")
                                    : tr("%1 ms old").arg(s.homographyAgeMs, 0, 'f', 0));

    m_savedLabel->setText(tr("Saved: %1").arg(s.saved));
    m_rejectedLabel->setText(tr("Rejected: %1 gates · %2 pose · %3 no-label")
                                 .arg(s.rejectedGates)
                                 .arg(s.rejectedPose)
                                 .arg(s.rejectedLabels));
    m_labelsLabel->setText(tr("Labels in last frame: %1").arg(s.lastLabelCount));
}

void DatasetPanel::onSessionStarted(const QString& directory)
{
    m_running = true;
    m_startStopBtn->setText(tr("Stop capture"));
    m_boardEdit->setEnabled(false);
    m_lightingBox->setEnabled(false);
    m_statusLabel->setText(tr("Capturing → %1").arg(directory));
}

void DatasetPanel::onSessionStopped(int savedCount)
{
    m_running = false;
    m_startStopBtn->setText(tr("Start capture"));
    m_boardEdit->setEnabled(true);
    m_lightingBox->setEnabled(true);
    m_statusLabel->setText(tr("Session done — %1 image(s) saved.").arg(savedCount));
}

void DatasetPanel::onSessionError(const QString& message)
{
    m_statusLabel->setText(tr("Error: %1").arg(message));
}

} // namespace ibom::gui
