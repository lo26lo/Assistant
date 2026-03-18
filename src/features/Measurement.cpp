#include "Measurement.h"

#include <cmath>
#include <spdlog/spdlog.h>

namespace ibom::features {

Measurement::Measurement(QObject* parent)
    : QObject(parent)
{
}

void Measurement::setMode(Mode mode)
{
    if (m_mode != mode) {
        m_mode = mode;
        clearPoints();
        emit modeChanged(mode);
    }
}

void Measurement::setCalibration(double pixelsPerMM)
{
    m_pixelsPerMM = pixelsPerMM;
    spdlog::info("Measurement: calibration set to {:.2f} px/mm", pixelsPerMM);
}

void Measurement::addPoint(QPointF point)
{
    m_currentPoints.push_back(point);
    emit pointAdded(point, static_cast<int>(m_currentPoints.size()));

    int requiredPoints = 0;
    switch (m_mode) {
    case Mode::Distance:  requiredPoints = 2; break;
    case Mode::Angle:     requiredPoints = 3; break;
    case Mode::PinPitch:  requiredPoints = 2; break;
    case Mode::Area:      requiredPoints = -1; break; // Manual close
    }

    if (requiredPoints > 0 && static_cast<int>(m_currentPoints.size()) >= requiredPoints) {
        auto result = currentResult();
        if (result) {
            m_history.push_back(*result);
            emit measurementComplete(*result);
            spdlog::info("Measurement: {} = {:.2f} px ({:.3f} mm)",
                         (m_mode == Mode::Distance ? "distance" :
                          m_mode == Mode::Angle ? "angle" :
                          m_mode == Mode::PinPitch ? "pitch" : "area"),
                         result->valuePixels, result->valueMM);
            clearPoints();
        }
    }
}

void Measurement::clearPoints()
{
    m_currentPoints.clear();
}

std::optional<Measurement::MeasureResult> Measurement::currentResult() const
{
    MeasureResult result;
    result.mode   = m_mode;
    result.points = m_currentPoints;

    switch (m_mode) {
    case Mode::Distance:
    case Mode::PinPitch:
        if (m_currentPoints.size() >= 2) {
            result.valuePixels = computeDistance(m_currentPoints[0], m_currentPoints[1]);
            result.valueMM = isCalibrated() ? result.valuePixels / m_pixelsPerMM : 0;
            return result;
        }
        break;

    case Mode::Angle:
        if (m_currentPoints.size() >= 3) {
            result.valuePixels = computeAngle(m_currentPoints[0],
                                               m_currentPoints[1],
                                               m_currentPoints[2]);
            result.valueMM = result.valuePixels; // Degrees, not mm
            return result;
        }
        break;

    case Mode::Area:
        if (m_currentPoints.size() >= 3) {
            result.valuePixels = computePolygonArea(m_currentPoints);
            result.valueMM = isCalibrated()
                ? result.valuePixels / (m_pixelsPerMM * m_pixelsPerMM)
                : 0;
            return result;
        }
        break;
    }

    return std::nullopt;
}

void Measurement::clearHistory()
{
    m_history.clear();
}

double Measurement::computeDistance(QPointF a, QPointF b) const
{
    double dx = b.x() - a.x();
    double dy = b.y() - a.y();
    return std::sqrt(dx * dx + dy * dy);
}

double Measurement::computeAngle(QPointF a, QPointF vertex, QPointF b) const
{
    double ax = a.x() - vertex.x();
    double ay = a.y() - vertex.y();
    double bx = b.x() - vertex.x();
    double by = b.y() - vertex.y();

    double dot = ax * bx + ay * by;
    double magA = std::sqrt(ax * ax + ay * ay);
    double magB = std::sqrt(bx * bx + by * by);

    if (magA < 1e-9 || magB < 1e-9) return 0.0;

    double cosAngle = std::clamp(dot / (magA * magB), -1.0, 1.0);
    return std::acos(cosAngle) * 180.0 / M_PI;
}

double Measurement::computePolygonArea(const std::vector<QPointF>& pts) const
{
    // Shoelace formula
    double area = 0;
    int n = static_cast<int>(pts.size());
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        area += pts[i].x() * pts[j].y();
        area -= pts[j].x() * pts[i].y();
    }
    return std::abs(area) / 2.0;
}

} // namespace ibom::features
