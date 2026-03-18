#include "ComponentDetector.h"

#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include <cmath>

namespace ibom::ai {

ComponentDetector::ComponentDetector(InferenceEngine& engine)
    : m_engine(engine)
{
}

bool ComponentDetector::loadModel(const std::string& modelPath)
{
    return m_engine.loadModel(modelPath);
}

std::vector<Detection> ComponentDetector::detect(const cv::Mat& frame)
{
    return m_engine.detect(frame, m_confThreshold, m_nmsThreshold);
}

float ComponentDetector::estimateOrientation(const cv::Mat& componentROI)
{
    if (componentROI.empty()) return 0.0f;

    cv::Mat gray;
    if (componentROI.channels() == 3)
        cv::cvtColor(componentROI, gray, cv::COLOR_BGR2GRAY);
    else
        gray = componentROI;

    // Apply threshold to isolate the component body
    cv::Mat thresh;
    cv::threshold(gray, thresh, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);

    // Find contours
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(thresh, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    if (contours.empty()) return 0.0f;

    // Find largest contour
    auto largest = std::max_element(contours.begin(), contours.end(),
        [](const auto& a, const auto& b) {
            return cv::contourArea(a) < cv::contourArea(b);
        });

    if (largest->size() < 5) return 0.0f;

    // Fit minimum area rectangle to get orientation
    cv::RotatedRect rotRect = cv::minAreaRect(*largest);

    float angle = rotRect.angle;

    // Normalize to [0, 360) range
    if (angle < 0) angle += 360.0f;

    return angle;
}

bool ComponentDetector::isOrientationCorrect(float detected, float expected, float tolerance) const
{
    // Normalize both angles to [0, 360)
    auto normalize = [](float a) -> float {
        a = std::fmod(a, 360.0f);
        if (a < 0) a += 360.0f;
        return a;
    };

    float d = normalize(detected);
    float e = normalize(expected);

    float diff = std::abs(d - e);
    if (diff > 180.0f) diff = 360.0f - diff;

    return diff <= tolerance;
}

} // namespace ibom::ai
