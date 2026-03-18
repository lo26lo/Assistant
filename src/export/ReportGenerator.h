#pragma once

#include <QObject>
#include <QString>
#include <QImage>
#include <vector>
#include <string>

namespace ibom::features {
class SnapshotHistory;
}

namespace ibom::exports {

/// Generate PDF and HTML inspection reports
class ReportGenerator : public QObject {
    Q_OBJECT

public:
    explicit ReportGenerator(QObject* parent = nullptr);
    ~ReportGenerator() override = default;

    struct InspectionResult {
        std::string reference;
        std::string value;
        std::string footprint;
        std::string status;       // "placed", "missing", "defect"
        std::string defectType;   // e.g. "bridge", "insufficient", "cold_joint"
        float       confidence = 0;
        QImage      snapshot;     // Optional snapshot of the component
    };

    struct ReportConfig {
        QString     title       = "PCB Inspection Report";
        QString     projectName;
        QString     boardRevision;
        QString     operatorName;
        bool        includeSnapshots = true;
        bool        includeStatistics = true;
        bool        includeDefectDetails = true;
        bool        includeBomChecklist = true;
    };

    void setConfig(const ReportConfig& config) { m_config = config; }
    const ReportConfig& config() const { return m_config; }

    void setResults(const std::vector<InspectionResult>& results);
    void setBoardImage(const QImage& image);

    /// Generate PDF report
    bool generatePDF(const QString& path);

    /// Generate HTML report
    bool generateHTML(const QString& path);

    /// Generate summary stats
    struct Statistics {
        int total       = 0;
        int placed      = 0;
        int missing     = 0;
        int defects     = 0;
        float yieldPct  = 0;
    };
    Statistics computeStatistics() const;

signals:
    void reportGenerated(const QString& path);
    void error(const QString& message);
    void progress(int percent);

private:
    QString generateHTMLContent() const;
    QString resultStatusColor(const std::string& status) const;

    ReportConfig m_config;
    std::vector<InspectionResult> m_results;
    QImage m_boardImage;
};

} // namespace ibom::exports
