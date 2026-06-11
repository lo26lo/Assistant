#pragma once

#include <QWidget>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>

#include "features/DatasetCreator.h"  // ibom::features::DatasetStatus

namespace ibom::gui {

/**
 * @brief Control panel for dataset capture sessions (Phase A).
 *
 * Start/stop a session, shows the 5 quality gates live (green/red), the
 * saved/rejected counters broken down by cause, and the label count of the
 * last saved frame. The existing pad/silkscreen overlay is the visual
 * alignment check — if boxes drift, stop and re-align.
 */
class DatasetPanel : public QWidget {
    Q_OBJECT

public:
    explicit DatasetPanel(QWidget* parent = nullptr);
    ~DatasetPanel() override = default;

public slots:
    void updateStatus(ibom::features::DatasetStatus status);
    void onSessionStarted(const QString& directory);
    void onSessionStopped(int savedCount);
    void onSessionError(const QString& message);

signals:
    void startRequested(QString boardName, QString lightingTag);
    void stopRequested();

private:
    void buildUI();
    void setGate(QLabel* label, const QString& name, bool ok, const QString& detail);

    QLineEdit*   m_boardEdit    = nullptr;
    QComboBox*   m_lightingBox  = nullptr;
    QPushButton* m_startStopBtn = nullptr;

    QLabel* m_gateTracking  = nullptr;
    QLabel* m_gateReproj    = nullptr;
    QLabel* m_gateSharpness = nullptr;
    QLabel* m_gateExposure  = nullptr;
    QLabel* m_gateFresh     = nullptr;

    QLabel* m_savedLabel    = nullptr;
    QLabel* m_rejectedLabel = nullptr;
    QLabel* m_labelsLabel   = nullptr;
    QLabel* m_statusLabel   = nullptr;

    bool m_running = false;
};

} // namespace ibom::gui
