#include "StencilAlign.h"

#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace ibom::features {

StencilAlign::StencilAlign(QObject* parent)
    : QObject(parent)
{
}

void StencilAlign::setExpectedFiducials(const std::vector<cv::Point2f>& positions)
{
    m_fiducials.clear();
    for (const auto& pos : positions) {
        FiducialMark mark;
        mark.expected = pos;
        m_fiducials.push_back(mark);
    }
    spdlog::info("StencilAlign: set {} expected fiducial positions", positions.size());
}

void StencilAlign::detectFiducials(const cv::Mat& frame)
{
    if (frame.empty() || m_fiducials.empty()) return;

    auto detections = detectCircularMarks(frame);

    bool allFound = true;
    for (auto& fid : m_fiducials) {
        if (detections.empty()) {
            fid.found = false;
            allFound = false;
            continue;
        }

        cv::Point2f nearest = findNearestDetection(fid.expected, detections);
        float dist = cv::norm(nearest - fid.expected);

        // Accept if within reasonable range
        float maxDistPx = m_pixelsPerMM > 0 ? m_pixelsPerMM * 5.0f : 100.0f;
        if (dist < maxDistPx) {
            fid.detected = nearest;
            fid.found = true;
            fid.error = m_pixelsPerMM > 0 ? dist / m_pixelsPerMM : dist;

            emit fiducialDetected(static_cast<int>(&fid - &m_fiducials[0]), fid.error);
        } else {
            fid.found = false;
            allFound = false;
        }
    }

    bool aligned = isAligned();
    if (aligned && !m_wasAligned) {
        emit alignmentAchieved();
        spdlog::info("StencilAlign: alignment achieved!");
    } else if (!aligned && m_wasAligned) {
        emit alignmentLost();
        spdlog::warn("StencilAlign: alignment lost");
    }
    m_wasAligned = aligned;
}

float StencilAlign::alignmentQuality() const
{
    if (m_fiducials.empty()) return 0;

    int found = 0;
    float totalError = 0;
    for (const auto& f : m_fiducials) {
        if (f.found) {
            found++;
            totalError += f.error;
        }
    }

    if (found == 0) return 0;

    float avgError = totalError / found;
    float coverage = static_cast<float>(found) / m_fiducials.size();

    // Quality score: 1.0 when all found and error is 0
    // Degrades exponentially with error
    float errorScore = std::exp(-avgError * 10.0f); // ~0 at 0.5mm error
    return coverage * errorScore;
}

float StencilAlign::maxError() const
{
    float maxErr = 0;
    for (const auto& f : m_fiducials) {
        if (f.found) maxErr = std::max(maxErr, f.error);
    }
    return maxErr;
}

cv::Mat StencilAlign::drawAlignmentOverlay(const cv::Mat& frame) const
{
    cv::Mat output = frame.clone();

    for (const auto& fid : m_fiducials) {
        cv::Point2f exp = fid.expected;

        // Draw expected position (crosshair)
        cv::drawMarker(output, cv::Point(exp), cv::Scalar(0, 255, 255),
                        cv::MARKER_CROSS, 20, 2);

        if (fid.found) {
            cv::Point2f det = fid.detected;

            // Color based on error
            cv::Scalar color;
            if (fid.error < 0.05f)      color = cv::Scalar(0, 255, 0);   // Green: excellent
            else if (fid.error < 0.1f)   color = cv::Scalar(0, 200, 255); // Yellow: OK
            else                          color = cv::Scalar(0, 0, 255);   // Red: bad

            // Draw detected position
            cv::circle(output, cv::Point(det), 8, color, 2);

            // Draw error vector
            cv::arrowedLine(output, cv::Point(det), cv::Point(exp), color, 2, cv::LINE_AA, 0, 0.3);

            // Error text
            std::string text = cv::format("%.3f mm", fid.error);
            cv::putText(output, text, cv::Point(det) + cv::Point(12, -8),
                        cv::FONT_HERSHEY_SIMPLEX, 0.4, color, 1);
        } else {
            // Not found: red X
            cv::drawMarker(output, cv::Point(exp), cv::Scalar(0, 0, 255),
                            cv::MARKER_TILTED_CROSS, 15, 2);
            cv::putText(output, "?", cv::Point(exp) + cv::Point(12, -8),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 255), 1);
        }
    }

    // Overall status text
    float quality = alignmentQuality();
    std::string status = cv::format("Alignment: %.0f%%", quality * 100);
    cv::Scalar statusColor = quality > 0.9f ? cv::Scalar(0, 255, 0)
                            : quality > 0.5f ? cv::Scalar(0, 200, 255)
                            : cv::Scalar(0, 0, 255);
    cv::putText(output, status, cv::Point(10, 30),
                cv::FONT_HERSHEY_SIMPLEX, 0.8, statusColor, 2);

    return output;
}

bool StencilAlign::isAligned(float thresholdMM) const
{
    if (m_fiducials.empty()) return false;

    for (const auto& f : m_fiducials) {
        if (!f.found || f.error > thresholdMM) return false;
    }
    return true;
}

std::vector<cv::Point2f> StencilAlign::detectCircularMarks(const cv::Mat& frame)
{
    cv::Mat gray;
    if (frame.channels() == 3)
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    else
        gray = frame;

    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(9, 9), 2);

    // Detect circles using HoughCircles
    std::vector<cv::Vec3f> circles;
    cv::HoughCircles(blurred, circles, cv::HOUGH_GRADIENT,
                      1,              // dp
                      30,             // minDist between centers
                      100,            // param1 (Canny high threshold)
                      30,             // param2 (accumulator threshold)
                      5,              // minRadius
                      50);            // maxRadius

    std::vector<cv::Point2f> centers;
    for (const auto& c : circles) {
        centers.emplace_back(c[0], c[1]);
    }

    return centers;
}

cv::Point2f StencilAlign::findNearestDetection(cv::Point2f expected,
                                                 const std::vector<cv::Point2f>& detections)
{
    cv::Point2f nearest = detections[0];
    float minDist = cv::norm(nearest - expected);

    for (size_t i = 1; i < detections.size(); ++i) {
        float d = cv::norm(detections[i] - expected);
        if (d < minDist) {
            minDist = d;
            nearest = detections[i];
        }
    }
    return nearest;
}

} // namespace ibom::features
