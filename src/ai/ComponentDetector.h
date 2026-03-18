#pragma once

#include "InferenceEngine.h"
#include <opencv2/core.hpp>
#include <vector>

namespace ibom::ai {

/**
 * @brief Detects electronic components on a PCB using a YOLO-based model.
 *
 * Wraps InferenceEngine for component-specific detection,
 * including orientation estimation.
 */
class ComponentDetector {
public:
    explicit ComponentDetector(InferenceEngine& engine);
    ~ComponentDetector() = default;

    /// Initialize with the component detection model.
    bool loadModel(const std::string& modelPath);

    /// Detect components in a frame.
    std::vector<Detection> detect(const cv::Mat& frame);

    /// Estimate component orientation (rotation angle in degrees).
    /// Uses a secondary model or geometric analysis.
    float estimateOrientation(const cv::Mat& componentROI);

    /// Check if a detected component matches expected orientation.
    /// @param detected Detected angle.
    /// @param expected Expected angle from iBOM data.
    /// @param tolerance Tolerance in degrees.
    bool isOrientationCorrect(float detected, float expected, float tolerance = 15.0f) const;

    // Settings
    void setConfidenceThreshold(float conf) { m_confThreshold = conf; }
    void setNmsThreshold(float nms) { m_nmsThreshold = nms; }

private:
    InferenceEngine& m_engine;
    float m_confThreshold = 0.5f;
    float m_nmsThreshold  = 0.45f;
};

} // namespace ibom::ai
