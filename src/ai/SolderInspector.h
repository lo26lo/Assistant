#pragma once

#include "InferenceEngine.h"
#include <opencv2/core.hpp>
#include <vector>

namespace ibom::ai {

/// Solder joint quality classification.
enum class SolderQuality {
    Good,           // Well-formed joint
    Insufficient,   // Too little solder
    Excess,         // Too much solder
    Bridge,         // Solder bridge between pads
    Cold,           // Cold/cracked joint
    Missing,        // No solder at all
    Unknown
};

/// Result for a single solder joint inspection.
struct SolderResult {
    cv::Rect2f    location;
    SolderQuality quality = SolderQuality::Unknown;
    float         confidence = 0.0f;
    std::string   componentRef; // Which component this belongs to
    std::string   pinNumber;
};

/**
 * @brief Inspects solder joints on assembled PCBs.
 *
 * Uses a trained model to classify solder joint quality.
 */
class SolderInspector {
public:
    explicit SolderInspector(InferenceEngine& engine);
    ~SolderInspector() = default;

    /// Load the solder inspection model.
    bool loadModel(const std::string& modelPath);

    /// Inspect all solder joints in a frame.
    std::vector<SolderResult> inspect(const cv::Mat& frame);

    /// Inspect solder joints for a specific component ROI.
    std::vector<SolderResult> inspectComponent(const cv::Mat& roi,
                                                const std::string& componentRef);

    /// Get human-readable quality string.
    static std::string qualityString(SolderQuality q);

    /// Get color for rendering (BGR).
    static cv::Scalar qualityColor(SolderQuality q);

private:
    InferenceEngine& m_engine;
    float m_confThreshold = 0.4f;
};

} // namespace ibom::ai
