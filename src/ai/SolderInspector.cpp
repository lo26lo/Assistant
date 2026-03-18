#include "SolderInspector.h"

#include <spdlog/spdlog.h>

namespace ibom::ai {

SolderInspector::SolderInspector(InferenceEngine& engine)
    : m_engine(engine)
{
}

bool SolderInspector::loadModel(const std::string& modelPath)
{
    spdlog::info("Loading solder inspection model: {}", modelPath);
    return m_engine.loadModel(modelPath);
}

std::vector<SolderResult> SolderInspector::inspect(const cv::Mat& frame)
{
    auto detections = m_engine.detect(frame, m_confThreshold, 0.45f);

    std::vector<SolderResult> results;
    for (const auto& det : detections) {
        SolderResult result;
        result.location   = det.bbox;
        result.confidence = det.confidence;

        // Map class ID to solder quality
        switch (det.classId) {
            case 0: result.quality = SolderQuality::Good;         break;
            case 1: result.quality = SolderQuality::Insufficient; break;
            case 2: result.quality = SolderQuality::Excess;       break;
            case 3: result.quality = SolderQuality::Bridge;       break;
            case 4: result.quality = SolderQuality::Cold;         break;
            case 5: result.quality = SolderQuality::Missing;      break;
            default: result.quality = SolderQuality::Unknown;     break;
        }

        results.push_back(result);
    }

    return results;
}

std::vector<SolderResult> SolderInspector::inspectComponent(const cv::Mat& roi,
                                                              const std::string& componentRef)
{
    auto results = inspect(roi);
    for (auto& r : results) {
        r.componentRef = componentRef;
    }
    return results;
}

std::string SolderInspector::qualityString(SolderQuality q)
{
    switch (q) {
        case SolderQuality::Good:         return "Good";
        case SolderQuality::Insufficient: return "Insufficient";
        case SolderQuality::Excess:       return "Excess";
        case SolderQuality::Bridge:       return "Bridge";
        case SolderQuality::Cold:         return "Cold Joint";
        case SolderQuality::Missing:      return "Missing";
        default:                          return "Unknown";
    }
}

cv::Scalar SolderInspector::qualityColor(SolderQuality q)
{
    switch (q) {
        case SolderQuality::Good:         return {0, 255, 0};      // Green
        case SolderQuality::Insufficient: return {0, 165, 255};    // Orange
        case SolderQuality::Excess:       return {0, 255, 255};    // Yellow
        case SolderQuality::Bridge:       return {0, 0, 255};      // Red
        case SolderQuality::Cold:         return {255, 0, 255};    // Magenta
        case SolderQuality::Missing:      return {0, 0, 200};      // Dark red
        default:                          return {128, 128, 128};  // Gray
    }
}

} // namespace ibom::ai
