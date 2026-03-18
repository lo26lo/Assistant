#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace ibom::ai {

class InferenceEngine;

/// OCR result for a detected text region.
struct OCRResult {
    std::string text;
    float       confidence = 0.0f;
    cv::Rect2f  bbox;
};

/**
 * @brief Optical Character Recognition for component markings.
 *
 * Reads text printed on IC packages, resistor codes, etc.
 * and verifies them against BOM data.
 */
class OCREngine {
public:
    explicit OCREngine(InferenceEngine& engine);
    ~OCREngine() = default;

    /// Load the OCR model.
    bool loadModel(const std::string& modelPath);

    /// Recognize text in a component ROI.
    std::vector<OCRResult> recognize(const cv::Mat& roi);

    /// Preprocess ROI for better OCR (contrast, rotation, etc.).
    cv::Mat preprocess(const cv::Mat& roi);

    /// Check if recognized text matches expected value.
    /// Uses fuzzy matching to handle partial reads.
    bool matchesExpected(const std::string& recognized,
                          const std::string& expected,
                          float similarityThreshold = 0.7f) const;

    /// Compute string similarity (Levenshtein-based, 0.0 to 1.0).
    static float stringSimilarity(const std::string& a, const std::string& b);

private:
    InferenceEngine& m_engine;
};

} // namespace ibom::ai
