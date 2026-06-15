#pragma once

#include <QWidget>
#include <QLabel>
#include <QProgressBar>
#include <QTableWidget>
#include <QGridLayout>
#include <QColor>
#include <QString>
#include <map>
#include <string>

namespace ibom::gui {

class StatsPanel : public QWidget {
    Q_OBJECT

public:
    explicit StatsPanel(QWidget* parent = nullptr);
    ~StatsPanel() override = default;

    void resetStats();
    void setTotalComponents(int total);
    void incrementPlaced();
    void incrementMissing();
    void incrementDefect();
    void setFps(double fps);
    void setInferenceTime(double ms);
    void setGpuMemory(size_t usedMB, size_t totalMB);
    void setScale(double pixelsPerMm);
    /// Live focus assist: Laplacian variance of the current frame.
    /// `good` = above the sharpness threshold (same metric/scale as the
    /// dataset capture gate) — turn the focus ring until the value peaks.
    void setSharpness(double variance, bool good);
    void addDefectEntry(const std::string& reference, const std::string& type);

public slots:
    /// Append a runtime log line to the Event Log.
    /// `level` is the spdlog::level::level_enum value as int.
    void addLogEntry(int level, const QString& logger, const QString& message);

signals:
    void defectClicked(const std::string& reference);

private:
    void buildUI();
    void updateProgress();
    void updateSummaryLabel();
    void appendEventRow(const QString& level, const QString& message,
                        const QColor& color, const QString& defectRef = {});

    // Keep the Event Log bounded to avoid unbounded memory growth.
    static constexpr int kMaxEventRows = 500;

    // Summary
    QLabel*       m_summaryLabel    = nullptr;
    QProgressBar* m_progressBar     = nullptr;

    // Counters
    QLabel* m_placedLabel   = nullptr;
    QLabel* m_missingLabel  = nullptr;
    QLabel* m_defectLabel   = nullptr;
    QLabel* m_pendingLabel  = nullptr;

    // Performance
    QLabel* m_fpsLabel          = nullptr;
    QLabel* m_inferenceLabel    = nullptr;
    QLabel* m_gpuMemLabel       = nullptr;
    QLabel* m_scaleLabel        = nullptr;
    QLabel* m_focusLabel        = nullptr;

    // Event log (runtime logs + defects)
    QTableWidget* m_defectTable = nullptr;

    int m_total   = 0;
    int m_placed  = 0;
    int m_missing = 0;
    int m_defect  = 0;
};

} // namespace ibom::gui
