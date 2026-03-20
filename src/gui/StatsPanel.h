#pragma once

#include <QWidget>
#include <QLabel>
#include <QProgressBar>
#include <QTableWidget>
#include <QGridLayout>
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
    void addDefectEntry(const std::string& reference, const std::string& type);

signals:
    void defectClicked(const std::string& reference);

private:
    void buildUI();
    void updateProgress();
    void updateSummaryLabel();

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

    // Defect log
    QTableWidget* m_defectTable = nullptr;

    int m_total   = 0;
    int m_placed  = 0;
    int m_missing = 0;
    int m_defect  = 0;
};

} // namespace ibom::gui
