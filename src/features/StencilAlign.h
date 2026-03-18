#pragma once

#include <QObject>
#include <opencv2/core.hpp>
#include <vector>

namespace ibom::features {

/// Solder paste stencil alignment helper:
/// guides alignment of stencil with PCB using overlay fiducials
class StencilAlign : public QObject {
    Q_OBJECT

public:
    explicit StencilAlign(QObject* parent = nullptr);
    ~StencilAlign() override = default;

    struct FiducialMark {
        cv::Point2f expected;  // Where the fiducial should be (from iBOM)
        cv::Point2f detected;  // Where it was detected in camera
        float       error = 0; // Alignment error in mm
        bool        found = false;
    };

    /// Set expected fiducial positions from iBOM data
    void setExpectedFiducials(const std::vector<cv::Point2f>& positions);

    /// Detect fiducials in camera frame
    void detectFiducials(const cv::Mat& frame);

    /// Get alignment error for each fiducial
    const std::vector<FiducialMark>& fiducials() const { return m_fiducials; }

    /// Overall alignment quality (0 = bad, 1 = perfect)
    float alignmentQuality() const;

    /// Max error in mm
    float maxError() const;

    /// Draw alignment guides on frame
    cv::Mat drawAlignmentOverlay(const cv::Mat& frame) const;

    /// Set pixels per mm for error calculation
    void setPixelsPerMM(float ppmm) { m_pixelsPerMM = ppmm; }

    /// Is alignment acceptable? (all errors < threshold)
    bool isAligned(float thresholdMM = 0.1f) const;

signals:
    void fiducialDetected(int index, float errorMM);
    void alignmentAchieved();
    void alignmentLost();

private:
    std::vector<cv::Point2f> detectCircularMarks(const cv::Mat& frame);
    cv::Point2f findNearestDetection(cv::Point2f expected,
                                      const std::vector<cv::Point2f>& detections);

    std::vector<FiducialMark> m_fiducials;
    float m_pixelsPerMM = 0;
    bool  m_wasAligned  = false;
};

} // namespace ibom::features
