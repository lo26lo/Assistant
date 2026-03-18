#include "DataExporter.h"

#include <QFile>
#include <QTextStream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace ibom::exports {

DataExporter::DataExporter(QObject* parent)
    : QObject(parent)
{
}

void DataExporter::setRecords(const std::vector<ComponentRecord>& records)
{
    m_records = records;
}

bool DataExporter::exportCSV(const QString& path, char delimiter) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit error(QString("Cannot open file: %1").arg(path));
        return false;
    }

    QTextStream out(&file);
    QString d(delimiter);

    // Header
    out << "Reference" << d << "Value" << d << "Footprint" << d
        << "Layer" << d << "Status" << d << "DefectType" << d
        << "Confidence" << d << "PosX" << d << "PosY" << d << "Rotation\n";

    for (const auto& r : m_records) {
        out << QString::fromStdString(r.reference) << d
            << QString::fromStdString(r.value) << d
            << QString::fromStdString(r.footprint) << d
            << (r.layer == 0 ? "F" : "B") << d
            << QString::fromStdString(r.status) << d
            << QString::fromStdString(r.defectType) << d
            << QString::number(r.confidence, 'f', 2) << d
            << QString::number(r.posX, 'f', 3) << d
            << QString::number(r.posY, 'f', 3) << d
            << QString::number(r.rotation, 'f', 1) << "\n";
    }

    file.close();
    spdlog::info("DataExporter: CSV exported to '{}'", path.toStdString());
    emit exported(path, "CSV");
    return true;
}

bool DataExporter::exportJSON(const QString& path, bool pretty) const
{
    nlohmann::json root;
    root["exportDate"] = QDateTime::currentDateTime().toString(Qt::ISODate).toStdString();
    root["totalComponents"] = m_records.size();

    nlohmann::json components = nlohmann::json::array();
    for (const auto& r : m_records) {
        nlohmann::json comp;
        comp["reference"]  = r.reference;
        comp["value"]      = r.value;
        comp["footprint"]  = r.footprint;
        comp["layer"]      = r.layer == 0 ? "F" : "B";
        comp["status"]     = r.status;
        comp["defectType"] = r.defectType;
        comp["confidence"] = r.confidence;
        comp["position"]   = { {"x", r.posX}, {"y", r.posY} };
        comp["rotation"]   = r.rotation;

        if (!r.extraFields.empty()) {
            nlohmann::json extra;
            for (const auto& [k, v] : r.extraFields) {
                extra[k] = v;
            }
            comp["extra"] = extra;
        }

        components.push_back(std::move(comp));
    }
    root["components"] = components;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit error(QString("Cannot open file: %1").arg(path));
        return false;
    }

    std::string json_str = pretty ? root.dump(2) : root.dump();
    file.write(json_str.c_str(), json_str.size());
    file.close();

    spdlog::info("DataExporter: JSON exported to '{}'", path.toStdString());
    emit exported(path, "JSON");
    return true;
}

bool DataExporter::exportPlacement(const QString& path) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit error(QString("Cannot open file: %1").arg(path));
        return false;
    }

    QTextStream out(&file);

    // KiCad placement file format
    out << "### PCB Inspector — Component Placement File ###\n";
    out << "# Ref    Val    Package    PosX    PosY    Rot    Side    Status\n";

    for (const auto& r : m_records) {
        out << QString::fromStdString(r.reference) << "    "
            << QString::fromStdString(r.value) << "    "
            << QString::fromStdString(r.footprint) << "    "
            << QString::number(r.posX, 'f', 4) << "    "
            << QString::number(r.posY, 'f', 4) << "    "
            << QString::number(r.rotation, 'f', 1) << "    "
            << (r.layer == 0 ? "top" : "bottom") << "    "
            << QString::fromStdString(r.status) << "\n";
    }

    file.close();
    spdlog::info("DataExporter: placement file exported to '{}'", path.toStdString());
    emit exported(path, "Placement");
    return true;
}

bool DataExporter::exportBOM(const QString& path) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit error(QString("Cannot open file: %1").arg(path));
        return false;
    }

    QTextStream out(&file);
    out << "Reference,Value,Footprint,Layer,Placed\n";

    for (const auto& r : m_records) {
        bool placed = (r.status == "placed");
        out << QString::fromStdString(r.reference) << ","
            << QString::fromStdString(r.value) << ","
            << QString::fromStdString(r.footprint) << ","
            << (r.layer == 0 ? "F" : "B") << ","
            << (placed ? "YES" : "NO") << "\n";
    }

    file.close();
    spdlog::info("DataExporter: BOM exported to '{}'", path.toStdString());
    emit exported(path, "BOM");
    return true;
}

bool DataExporter::exportDefectsCSV(const QString& path) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit error(QString("Cannot open file: %1").arg(path));
        return false;
    }

    QTextStream out(&file);
    out << "Reference,Value,Footprint,DefectType,Confidence,PosX,PosY\n";

    for (const auto& r : m_records) {
        if (r.status != "defect" && r.status != "missing") continue;

        out << QString::fromStdString(r.reference) << ","
            << QString::fromStdString(r.value) << ","
            << QString::fromStdString(r.footprint) << ","
            << QString::fromStdString(r.defectType) << ","
            << QString::number(r.confidence, 'f', 2) << ","
            << QString::number(r.posX, 'f', 3) << ","
            << QString::number(r.posY, 'f', 3) << "\n";
    }

    file.close();
    spdlog::info("DataExporter: defects CSV exported to '{}'", path.toStdString());
    emit exported(path, "DefectsCSV");
    return true;
}

} // namespace ibom::exports
