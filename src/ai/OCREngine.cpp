#include "OCREngine.h"
#include "InferenceEngine.h"

#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <numeric>

namespace ibom::ai {

OCREngine::OCREngine(InferenceEngine& engine)
    : m_engine(engine)
{
}

bool OCREngine::loadModel(const std::string& modelPath)
{
    spdlog::info("Loading OCR model: {}", modelPath);
    return m_engine.loadModel(modelPath);
}

std::vector<OCRResult> OCREngine::recognize(const cv::Mat& roi)
{
    if (roi.empty()) return {};

    // Preprocess the ROI for better recognition
    cv::Mat processed = preprocess(roi);

    // TODO: Run OCR-specific inference pipeline
    // For now, return placeholder — will be connected to a
    // text recognition model (e.g., CRNN + CTC decoder)
    spdlog::debug("OCR recognize called on {}x{} ROI", roi.cols, roi.rows);

    return {};
}

cv::Mat OCREngine::preprocess(const cv::Mat& roi)
{
    cv::Mat gray, enhanced;

    // Convert to grayscale
    if (roi.channels() == 3)
        cv::cvtColor(roi, gray, cv::COLOR_BGR2GRAY);
    else
        gray = roi.clone();

    // Apply CLAHE for contrast enhancement
    auto clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
    clahe->apply(gray, enhanced);

    // Gaussian blur to reduce noise
    cv::GaussianBlur(enhanced, enhanced, cv::Size(3, 3), 0);

    // Adaptive threshold for binarization
    cv::Mat binary;
    cv::adaptiveThreshold(enhanced, binary, 255,
        cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY, 11, 2);

    return binary;
}

bool OCREngine::matchesExpected(const std::string& recognized,
                                  const std::string& expected,
                                  float similarityThreshold) const
{
    if (recognized.empty() || expected.empty()) return false;

    float sim = stringSimilarity(recognized, expected);
    return sim >= similarityThreshold;
}

float OCREngine::stringSimilarity(const std::string& a, const std::string& b)
{
    if (a.empty() && b.empty()) return 1.0f;
    if (a.empty() || b.empty()) return 0.0f;

    // Levenshtein distance
    size_t lenA = a.size();
    size_t lenB = b.size();

    std::vector<std::vector<size_t>> dp(lenA + 1, std::vector<size_t>(lenB + 1));

    for (size_t i = 0; i <= lenA; ++i) dp[i][0] = i;
    for (size_t j = 0; j <= lenB; ++j) dp[0][j] = j;

    for (size_t i = 1; i <= lenA; ++i) {
        for (size_t j = 1; j <= lenB; ++j) {
            size_t cost = (std::tolower(a[i-1]) == std::tolower(b[j-1])) ? 0 : 1;
            dp[i][j] = std::min({
                dp[i-1][j] + 1,      // Deletion
                dp[i][j-1] + 1,      // Insertion
                dp[i-1][j-1] + cost  // Substitution
            });
        }
    }

    size_t maxLen = std::max(lenA, lenB);
    return 1.0f - static_cast<float>(dp[lenA][lenB]) / static_cast<float>(maxLen);
}

} // namespace ibom::ai
