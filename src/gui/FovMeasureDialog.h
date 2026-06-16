#pragma once

#include <QDialog>
#include <QString>
#include <opencv2/core.hpp>

namespace ibom::gui {

/**
 * Dev tool: displays the current field-of-view metrics for the active camera
 * so the developer can measure Q2/Q3 from MICROSCOPE_PLACEMENT_PLAN.md:
 *   Q2 — actual FOV in mm at working magnification
 *   Q3 — number of iBOM components visible in the current FOV
 *
 * Also suggests config values to update (anchor_pixels_per_mm).
 * Read-only display; all inputs are passed in via the constructor.
 */
class FovMeasureDialog : public QDialog {
    Q_OBJECT

public:
    struct Metrics {
        // Camera & profile
        QString profileName;
        QString backendName;      // "V4L2 (microscope)" or "RealSense D405"
        bool    isMicroscope = false;  // V4L2 microscope (continuous zoom) vs D405 (fixed)
        int     camWidth  = 0;
        int     camHeight = 0;

        // Calibration
        bool   calibrated   = false;
        double calibRmsErr  = 0.0;   // reprojection RMS in px (0 = uncalibrated)

        // Scale & FOV
        double pixelsPerMm  = 0.0;   // 0 = unknown
        QString scaleSource;          // "homography (live)", "config fallback", …

        // Derived (computed from the above)
        double fovWidthMm   = 0.0;
        double fovHeightMm  = 0.0;

        // iBOM spatial query
        int  componentsInFov = -1;    // -1 = N/A (no homography or no iBOM)
        int  totalComponents = 0;

        // Config comparison
        double configAnchorPxPerMm = 0.0;  // current stored fallback
    };

    explicit FovMeasureDialog(const Metrics& m, QWidget* parent = nullptr);
};

} // namespace ibom::gui
