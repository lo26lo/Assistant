#include "ReportGenerator.h"

#include <QFile>
#include <QTextStream>
#include <QBuffer>
#include <QDateTime>
#include <spdlog/spdlog.h>

// Conditionally use libharu for PDF generation
#if __has_include(<hpdf.h>)
    #include <hpdf.h>
    #define HAS_LIBHARU 1
#else
    #define HAS_LIBHARU 0
#endif

namespace ibom::exports {

ReportGenerator::ReportGenerator(QObject* parent)
    : QObject(parent)
{
}

void ReportGenerator::setResults(const std::vector<InspectionResult>& results)
{
    m_results = results;
}

void ReportGenerator::setBoardImage(const QImage& image)
{
    m_boardImage = image;
}

ReportGenerator::Statistics ReportGenerator::computeStatistics() const
{
    Statistics stats;
    stats.total = static_cast<int>(m_results.size());

    for (const auto& r : m_results) {
        if (r.status == "placed")      stats.placed++;
        else if (r.status == "missing") stats.missing++;
        else if (r.status == "defect")  stats.defects++;
    }

    stats.yieldPct = stats.total > 0
        ? (stats.placed * 100.0f / stats.total) : 0;

    return stats;
}

bool ReportGenerator::generatePDF(const QString& path)
{
#if HAS_LIBHARU
    emit progress(0);

    HPDF_Doc pdf = HPDF_New(nullptr, nullptr);
    if (!pdf) {
        emit error("Failed to create PDF document");
        return false;
    }

    HPDF_SetCompressionMode(pdf, HPDF_COMP_ALL);
    HPDF_UseUTFEncodings(pdf);

    // Title page
    HPDF_Page page = HPDF_AddPage(pdf);
    HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_A4, HPDF_PAGE_PORTRAIT);
    float pageW = HPDF_Page_GetWidth(page);
    float pageH = HPDF_Page_GetHeight(page);

    HPDF_Font font = HPDF_GetFont(pdf, "Helvetica-Bold", nullptr);
    HPDF_Page_SetFontAndSize(page, font, 24);
    HPDF_Page_BeginText(page);
    HPDF_Page_TextOut(page, 50, pageH - 80,
                       m_config.title.toStdString().c_str());
    HPDF_Page_EndText(page);

    HPDF_Font fontNormal = HPDF_GetFont(pdf, "Helvetica", nullptr);
    HPDF_Page_SetFontAndSize(page, fontNormal, 12);
    float y = pageH - 120;

    HPDF_Page_BeginText(page);
    if (!m_config.projectName.isEmpty()) {
        HPDF_Page_TextOut(page, 50, y,
            QString("Project: %1").arg(m_config.projectName).toStdString().c_str());
        y -= 20;
    }
    HPDF_Page_TextOut(page, 50, y,
        QString("Date: %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm")).toStdString().c_str());
    y -= 20;

    if (!m_config.operatorName.isEmpty()) {
        HPDF_Page_TextOut(page, 50, y,
            QString("Operator: %1").arg(m_config.operatorName).toStdString().c_str());
        y -= 20;
    }
    HPDF_Page_EndText(page);

    emit progress(20);

    // Statistics
    if (m_config.includeStatistics) {
        auto stats = computeStatistics();
        y -= 30;

        HPDF_Page_SetFontAndSize(page, font, 16);
        HPDF_Page_BeginText(page);
        HPDF_Page_TextOut(page, 50, y, "Inspection Summary");
        HPDF_Page_EndText(page);
        y -= 25;

        HPDF_Page_SetFontAndSize(page, fontNormal, 12);
        HPDF_Page_BeginText(page);
        HPDF_Page_TextOut(page, 50, y,
            QString("Total: %1 | Placed: %2 | Missing: %3 | Defects: %4 | Yield: %5%")
                .arg(stats.total).arg(stats.placed).arg(stats.missing)
                .arg(stats.defects).arg(stats.yieldPct, 0, 'f', 1).toStdString().c_str());
        HPDF_Page_EndText(page);
    }

    emit progress(50);

    // Results table
    if (m_config.includeDefectDetails) {
        page = HPDF_AddPage(pdf);
        HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_A4, HPDF_PAGE_PORTRAIT);
        y = pageH - 50;

        HPDF_Page_SetFontAndSize(page, font, 16);
        HPDF_Page_BeginText(page);
        HPDF_Page_TextOut(page, 50, y, "Detailed Results");
        HPDF_Page_EndText(page);
        y -= 30;

        HPDF_Page_SetFontAndSize(page, fontNormal, 10);
        for (const auto& r : m_results) {
            if (y < 50) {
                page = HPDF_AddPage(pdf);
                HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_A4, HPDF_PAGE_PORTRAIT);
                y = pageH - 50;
                HPDF_Page_SetFontAndSize(page, fontNormal, 10);
            }

            std::string line = r.reference + " | " + r.value + " | " +
                               r.footprint + " | " + r.status;
            if (!r.defectType.empty()) line += " (" + r.defectType + ")";

            HPDF_Page_BeginText(page);
            HPDF_Page_TextOut(page, 50, y, line.c_str());
            HPDF_Page_EndText(page);
            y -= 15;
        }
    }

    emit progress(80);

    HPDF_SaveToFile(pdf, path.toStdString().c_str());
    HPDF_Free(pdf);

    emit progress(100);
    emit reportGenerated(path);
    spdlog::info("ReportGenerator: PDF saved to '{}'", path.toStdString());
    return true;
#else
    // Fallback: generate HTML and note that PDF requires libharu
    spdlog::warn("ReportGenerator: libharu not available, generating HTML instead");
    QString htmlPath = path;
    htmlPath.replace(".pdf", ".html");
    return generateHTML(htmlPath);
#endif
}

bool ReportGenerator::generateHTML(const QString& path)
{
    emit progress(0);

    QString html = generateHTMLContent();

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit error(QString("Failed to write HTML report to: %1").arg(path));
        return false;
    }

    QTextStream out(&file);
    out << html;
    file.close();

    emit progress(100);
    emit reportGenerated(path);
    spdlog::info("ReportGenerator: HTML report saved to '{}'", path.toStdString());
    return true;
}

QString ReportGenerator::generateHTMLContent() const
{
    auto stats = computeStatistics();

    QString html;
    html += "<!DOCTYPE html>\n<html>\n<head>\n";
    html += "<meta charset='utf-8'>\n";
    html += QString("<title>%1</title>\n").arg(m_config.title);
    html += R"(<style>
body { font-family: 'Segoe UI', Arial, sans-serif; margin: 20px; background: #f5f5f5; }
.container { max-width: 960px; margin: 0 auto; background: white; padding: 30px; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1); }
h1 { color: #2c3e50; border-bottom: 2px solid #3498db; padding-bottom: 10px; }
h2 { color: #34495e; margin-top: 30px; }
.stats { display: grid; grid-template-columns: repeat(4, 1fr); gap: 15px; margin: 20px 0; }
.stat-card { padding: 15px; border-radius: 8px; text-align: center; }
.stat-card h3 { margin: 0; font-size: 28px; }
.stat-card p { margin: 5px 0 0; font-size: 12px; color: #666; }
.placed  { background: #d4edda; color: #155724; }
.missing { background: #f8d7da; color: #721c24; }
.defect  { background: #fff3cd; color: #856404; }
.total   { background: #d1ecf1; color: #0c5460; }
table { width: 100%; border-collapse: collapse; margin-top: 15px; }
th { background: #2c3e50; color: white; padding: 10px; text-align: left; }
td { padding: 8px 10px; border-bottom: 1px solid #eee; }
tr:hover { background: #f8f9fa; }
.status-placed  { color: #28a745; font-weight: bold; }
.status-missing { color: #dc3545; font-weight: bold; }
.status-defect  { color: #ffc107; font-weight: bold; }
.footer { margin-top: 30px; font-size: 11px; color: #999; text-align: center; }
</style>
</head>
<body>
<div class='container'>
)";

    html += QString("<h1>%1</h1>\n").arg(m_config.title);

    html += "<div style='color:#666; margin-bottom: 20px;'>";
    if (!m_config.projectName.isEmpty())
        html += QString("Project: <b>%1</b> &nbsp;|&nbsp; ").arg(m_config.projectName);
    if (!m_config.boardRevision.isEmpty())
        html += QString("Rev: <b>%1</b> &nbsp;|&nbsp; ").arg(m_config.boardRevision);
    html += QString("Date: <b>%1</b>").arg(
        QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm"));
    if (!m_config.operatorName.isEmpty())
        html += QString(" &nbsp;|&nbsp; Operator: <b>%1</b>").arg(m_config.operatorName);
    html += "</div>\n";

    // Statistics cards
    if (m_config.includeStatistics) {
        html += "<h2>Summary</h2>\n<div class='stats'>\n";
        html += QString("<div class='stat-card total'><h3>%1</h3><p>Total</p></div>\n").arg(stats.total);
        html += QString("<div class='stat-card placed'><h3>%1</h3><p>Placed</p></div>\n").arg(stats.placed);
        html += QString("<div class='stat-card missing'><h3>%1</h3><p>Missing</p></div>\n").arg(stats.missing);
        html += QString("<div class='stat-card defect'><h3>%1</h3><p>Defects</p></div>\n").arg(stats.defects);
        html += "</div>\n";
        html += QString("<p>Yield: <b>%1%</b></p>\n").arg(stats.yieldPct, 0, 'f', 1);
    }

    // Results table
    if (m_config.includeDefectDetails) {
        html += "<h2>Component Results</h2>\n";
        html += "<table>\n<tr><th>Reference</th><th>Value</th><th>Footprint</th>"
                "<th>Status</th><th>Detail</th><th>Confidence</th></tr>\n";

        for (const auto& r : m_results) {
            QString statusClass = "status-" + QString::fromStdString(r.status);
            html += "<tr>";
            html += "<td>" + QString::fromStdString(r.reference) + "</td>";
            html += "<td>" + QString::fromStdString(r.value) + "</td>";
            html += "<td>" + QString::fromStdString(r.footprint) + "</td>";
            html += "<td class='" + statusClass + "'>" + QString::fromStdString(r.status) + "</td>";
            html += "<td>" + QString::fromStdString(r.defectType) + "</td>";
            html += QString("<td>%1%</td>").arg(r.confidence * 100, 0, 'f', 0);
            html += "</tr>\n";
        }
        html += "</table>\n";
    }

    html += "<div class='footer'>Generated by PCB Inspector — iBOM AI Overlay</div>\n";
    html += "</div>\n</body>\n</html>";

    return html;
}

QString ReportGenerator::resultStatusColor(const std::string& status) const
{
    if (status == "placed")  return "#28a745";
    if (status == "missing") return "#dc3545";
    if (status == "defect")  return "#ffc107";
    return "#6c757d";
}

} // namespace ibom::exports
