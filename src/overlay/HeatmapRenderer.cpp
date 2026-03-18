#include "HeatmapRenderer.h"

#include <opencv2/imgproc.hpp>
#include <algorithm>

namespace ibom::overlay {

void HeatmapRenderer::initialize(int width, int height, float cellSize)
{
    m_cellSize = cellSize;
    m_width  = static_cast<int>(std::ceil(width / cellSize));
    m_height = static_cast<int>(std::ceil(height / cellSize));
    m_accumulator = cv::Mat::zeros(m_height, m_width, CV_32FC1);
    m_totalDefects = 0;
}

void HeatmapRenderer::addDefect(float x, float y, float weight)
{
    int cx = static_cast<int>(x / m_cellSize);
    int cy = static_cast<int>(y / m_cellSize);

    if (cx >= 0 && cx < m_width && cy >= 0 && cy < m_height) {
        m_accumulator.at<float>(cy, cx) += weight;
        m_totalDefects++;
    }
}

void HeatmapRenderer::clear()
{
    if (!m_accumulator.empty()) {
        m_accumulator.setTo(0);
    }
    m_totalDefects = 0;
}

float HeatmapRenderer::maxValue() const
{
    if (m_accumulator.empty()) return 0.0f;
    double maxVal;
    cv::minMaxLoc(m_accumulator, nullptr, &maxVal);
    return static_cast<float>(maxVal);
}

QImage HeatmapRenderer::render(float opacity) const
{
    if (m_accumulator.empty()) return {};

    // Normalize to 0-255
    cv::Mat normalized;
    double maxVal = maxValue();
    if (maxVal > 0) {
        m_accumulator.convertTo(normalized, CV_8UC1, 255.0 / maxVal);
    } else {
        normalized = cv::Mat::zeros(m_accumulator.size(), CV_8UC1);
    }

    // Apply Gaussian blur for smooth heatmap
    cv::GaussianBlur(normalized, normalized, cv::Size(5, 5), 0);

    // Apply colormap
    cv::Mat colored;
    cv::applyColorMap(normalized, colored, cv::COLORMAP_JET);

    // Convert to QImage
    cv::Mat rgb;
    cv::cvtColor(colored, rgb, cv::COLOR_BGR2RGB);

    return QImage(rgb.data, rgb.cols, rgb.rows,
                  static_cast<int>(rgb.step), QImage::Format_RGB888).copy();
}

cv::Mat HeatmapRenderer::renderOnMat(const cv::Mat& background, float opacity) const
{
    if (m_accumulator.empty() || background.empty()) return background.clone();

    // Normalize
    cv::Mat normalized;
    double maxVal = maxValue();
    if (maxVal <= 0) return background.clone();

    m_accumulator.convertTo(normalized, CV_8UC1, 255.0 / maxVal);
    cv::GaussianBlur(normalized, normalized, cv::Size(5, 5), 0);

    // Resize to match background
    cv::Mat resized;
    cv::resize(normalized, resized, background.size(), 0, 0, cv::INTER_LINEAR);

    // Apply colormap
    cv::Mat colored;
    cv::applyColorMap(resized, colored, cv::COLORMAP_JET);

    // Blend
    cv::Mat result;
    cv::addWeighted(background, 1.0 - opacity, colored, opacity, 0, result);

    return result;
}

} // namespace ibom::overlay
