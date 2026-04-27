#pragma once

#include <QObject>
#include <QPointF>
#include <vector>
#include <optional>

namespace ibom::features {

/// On-screen measurement tool for distances, angles, and areas
class Measurement : public QObject {
    Q_OBJECT

public:
    explicit Measurement(QObject* parent = nullptr);
    ~Measurement() override = default;

    enum class Mode {
        Distance,    // Two-point distance
        Angle,       // Three-point angle
        Area,        // Polygon area
        PinPitch,    // Measure pin-to-pin pitch
    };

    struct MeasureResult {
        Mode   mode;
        double valuePixels = 0;   // in pixels
        double valueMM     = 0;   // in mm (if calibrated)
        std::vector<QPointF> points;
    };

    void setMode(Mode mode);
    Mode mode() const { return m_mode; }

    /// Set pixels-per-mm calibration factor
    void setCalibration(double pixelsPerMM);
    double calibration() const { return m_pixelsPerMM; }
    bool isCalibrated() const { return m_pixelsPerMM > 0; }

    /// Add a measurement point
    void addPoint(QPointF point);

    /// Clear current measurement
    void clearPoints();

    /// Points accumulated for the in-progress measurement (read-only).
    const std::vector<QPointF>& currentPoints() const { return m_currentPoints; }

    /// Force-complete the in-progress measurement (used for Area "close polygon",
    /// where the required-point count is unbounded). Returns true if a result was
    /// produced; false if there are not enough points to compute.
    bool commitCurrent();

    /// Get current measurement result (if enough points)
    std::optional<MeasureResult> currentResult() const;

    /// Get all completed measurements
    const std::vector<MeasureResult>& history() const { return m_history; }

    /// Clear history
    void clearHistory();

signals:
    void pointAdded(QPointF point, int totalPoints);
    void measurementComplete(const MeasureResult& result);
    void modeChanged(Mode mode);

private:
    double computeDistance(QPointF a, QPointF b) const;
    double computeAngle(QPointF a, QPointF vertex, QPointF b) const;
    double computePolygonArea(const std::vector<QPointF>& pts) const;

    Mode   m_mode = Mode::Distance;
    double m_pixelsPerMM = 0;

    std::vector<QPointF>       m_currentPoints;
    std::vector<MeasureResult> m_history;
};

} // namespace ibom::features
