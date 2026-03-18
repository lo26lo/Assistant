#pragma once

#include <QObject>
#include <QString>
#include <vector>
#include <string>
#include <map>

namespace ibom::exports {

/// Export inspection data to CSV, JSON, and other formats
class DataExporter : public QObject {
    Q_OBJECT

public:
    explicit DataExporter(QObject* parent = nullptr);
    ~DataExporter() override = default;

    struct ComponentRecord {
        std::string reference;
        std::string value;
        std::string footprint;
        int         layer = 0;
        std::string status;
        std::string defectType;
        float       confidence   = 0;
        double      posX         = 0;
        double      posY         = 0;
        double      rotation     = 0;
        std::map<std::string, std::string> extraFields;
    };

    void setRecords(const std::vector<ComponentRecord>& records);
    const std::vector<ComponentRecord>& records() const { return m_records; }

    /// Export to CSV
    bool exportCSV(const QString& path, char delimiter = ',') const;

    /// Export to JSON
    bool exportJSON(const QString& path, bool pretty = true) const;

    /// Export to KiCad-compatible placement file
    bool exportPlacement(const QString& path) const;

    /// Export BOM with checkboxes (for re-import)
    bool exportBOM(const QString& path) const;

    /// Export defects-only report
    bool exportDefectsCSV(const QString& path) const;

signals:
    void exported(const QString& path, const QString& format);
    void error(const QString& message);

private:
    std::vector<ComponentRecord> m_records;
};

} // namespace ibom::exports
