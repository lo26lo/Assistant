#pragma once

#include <opencv2/core.hpp>
#include <vector>

namespace ibom::overlay {

/**
 * @brief Computes and manages the homography between
 *        iBOM PCB coordinates and camera image pixels.
 *
 * Uses fiducial markers or manual point correspondences
 * to establish the mapping.
 */
class Homography {
public:
    Homography() = default;
    ~Homography() = default;

    /// Compute homography from point correspondences.
    /// @param pcbPoints Points in iBOM/PCB coordinate space.
    /// @param imagePoints Corresponding points in camera image space.
    /// @return true if homography was computed successfully.
    bool compute(const std::vector<cv::Point2f>& pcbPoints,
                 const std::vector<cv::Point2f>& imagePoints);

    /// Transform a point from PCB coords to image coords.
    cv::Point2f pcbToImage(cv::Point2f pcbPoint) const;

    /// Transform a point from image coords to PCB coords.
    cv::Point2f imageToPcb(cv::Point2f imagePoint) const;

    /// Transform multiple points PCB -> image.
    std::vector<cv::Point2f> pcbToImage(const std::vector<cv::Point2f>& pcbPoints) const;

    /// Transform a rectangle from PCB coords to image polygon.
    std::vector<cv::Point2f> transformRect(float x, float y, float w, float h) const;

    /// Whether a valid homography is available.
    bool isValid() const { return m_valid; }

    /// Get the homography matrix (PCB -> image).
    const cv::Mat& matrix() const { return m_homography; }

    /// Set the homography matrix directly (e.g., restore from backup).
    void setMatrix(const cv::Mat& matrix);

    /// Get the inverse matrix (image -> PCB).
    const cv::Mat& inverseMatrix() const { return m_inverse; }

    /// Save/load homography to/from file.
    bool save(const std::string& path) const;
    bool load(const std::string& path);

    /// Reset the homography.
    void reset();

    /// Get reprojection error from the last compute().
    double reprojectionError() const { return m_reprojError; }

private:
    cv::Mat m_homography;  // 3x3 PCB -> Image
    cv::Mat m_inverse;     // 3x3 Image -> PCB
    bool    m_valid = false;
    double  m_reprojError = 0.0;
};

} // namespace ibom::overlay
