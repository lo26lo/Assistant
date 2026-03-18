#pragma once

#include <opencv2/core.hpp>
#include <QImage>
#include <vector>

namespace ibom::overlay {

/**
 * @brief Generates defect density heatmaps across multiple inspections.
 *
 * Accumulates defect locations over multiple PCB inspections
 * and renders a color-coded heatmap overlay.
 */
class HeatmapRenderer {
public:
    HeatmapRenderer() = default;
    ~HeatmapRenderer() = default;

    /// Initialize the heatmap for a given board size (PCB coords).
    void initialize(int width, int height, float cellSize = 1.0f);

    /// Add a defect at a given PCB location.
    void addDefect(float x, float y, float weight = 1.0f);

    /// Clear all accumulated data.
    void clear();

    /// Render the heatmap as an overlay image.
    /// @param opacity Overlay opacity (0.0 - 1.0).
    QImage render(float opacity = 0.5f) const;

    /// Render the heatmap onto an existing OpenCV mat.
    cv::Mat renderOnMat(const cv::Mat& background, float opacity = 0.5f) const;

    /// Get total number of defects recorded.
    int totalDefects() const { return m_totalDefects; }

    /// Get the hottest cell value.
    float maxValue() const;

private:
    cv::Mat m_accumulator;       // 2D float map of defect density
    float   m_cellSize = 1.0f;   // mm per cell
    int     m_totalDefects = 0;
    int     m_width = 0;
    int     m_height = 0;
};

} // namespace ibom::overlay
