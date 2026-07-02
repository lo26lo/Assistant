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
    /// Refresh the flat coefficient caches after any matrix change. The
    /// point transforms apply them inline: the cv::perspectiveTransform path
    /// wrapped every single point in two std::vectors and a Mat — and the
    /// overlay/minimap/scale code calls these thousands of times per frame.
    void cacheCoefficients();
    /// Row-major 3×3 projective apply. Returns `p` unchanged on a degenerate
    /// ray (w ≈ 0), where cv::perspectiveTransform would return ±inf.
    static cv::Point2f applyCached(const double m[9], cv::Point2f p);

    cv::Mat m_homography;  // 3x3 PCB -> Image
    cv::Mat m_inverse;     // 3x3 Image -> PCB
    double  m_h[9]    = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major m_homography
    double  m_hInv[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major m_inverse
    bool    m_valid = false;
    double  m_reprojError = 0.0;
};

} // namespace ibom::overlay
