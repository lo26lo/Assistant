#pragma once

#include <opencv2/core.hpp>
#include <vector>
#include <string>

namespace ibom::camera {

/**
 * @brief Camera calibration using OpenCV.
 *
 * Handles lens distortion correction for the microscope optics
 * and provides undistortion maps for real-time frame correction.
 */
class CameraCalibration {
public:
    CameraCalibration();
    ~CameraCalibration() = default;

    /// Calibrate using a set of checkerboard images.
    /// @param images Calibration images (checkerboard pattern).
    /// @param boardSize Number of inner corners (cols x rows).
    /// @param squareSize Physical size of a square in mm.
    /// @return Reprojection error (lower is better), -1 on failure.
    double calibrate(const std::vector<cv::Mat>& images,
                     cv::Size boardSize = cv::Size(9, 6),
                     float squareSize = 1.0f);

    /// Load calibration from file.
    bool load(const std::string& path);

    /// Save calibration to file.
    bool save(const std::string& path) const;

    /// Whether calibration data is available.
    bool isCalibrated() const { return m_calibrated; }

    /// Undistort a frame using the calibration data.
    cv::Mat undistort(const cv::Mat& frame) const;

    /// Initialize undistortion maps (call once after calibration for speed).
    void initUndistortMaps(cv::Size imageSize);

    /// Get the camera matrix.
    const cv::Mat& cameraMatrix() const { return m_cameraMatrix; }

    /// Get distortion coefficients.
    const cv::Mat& distCoeffs() const { return m_distCoeffs; }

    /// Get pixels-per-mm ratio (after calibration with known square size).
    double pixelsPerMm() const { return m_pixelsPerMm; }

private:
    bool   m_calibrated = false;
    cv::Mat m_cameraMatrix;
    cv::Mat m_distCoeffs;
    cv::Mat m_map1, m_map2; // Undistortion maps (for remap)
    double  m_pixelsPerMm = 0.0;
};

} // namespace ibom::camera
