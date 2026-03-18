#include "CameraCalibration.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include <fstream>

namespace ibom::camera {

CameraCalibration::CameraCalibration() = default;

double CameraCalibration::calibrate(const std::vector<cv::Mat>& images,
                                     cv::Size boardSize,
                                     float squareSize)
{
    if (images.empty()) {
        spdlog::error("No calibration images provided.");
        return -1.0;
    }

    std::vector<std::vector<cv::Point3f>> objectPoints;
    std::vector<std::vector<cv::Point2f>> imagePoints;

    // Generate 3D object points for the checkerboard
    std::vector<cv::Point3f> objPts;
    for (int r = 0; r < boardSize.height; ++r) {
        for (int c = 0; c < boardSize.width; ++c) {
            objPts.emplace_back(c * squareSize, r * squareSize, 0.0f);
        }
    }

    cv::Size imageSize;

    for (const auto& img : images) {
        cv::Mat gray;
        if (img.channels() == 3) {
            cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
        } else {
            gray = img;
        }

        imageSize = gray.size();

        std::vector<cv::Point2f> corners;
        bool found = cv::findChessboardCorners(gray, boardSize, corners,
            cv::CALIB_CB_ADAPTIVE_THRESH | cv::CALIB_CB_NORMALIZE_IMAGE);

        if (found) {
            cv::cornerSubPix(gray, corners, cv::Size(11, 11), cv::Size(-1, -1),
                cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::COUNT, 30, 0.001));
            imagePoints.push_back(corners);
            objectPoints.push_back(objPts);
        }
    }

    if (imagePoints.empty()) {
        spdlog::error("No checkerboard patterns detected in calibration images.");
        return -1.0;
    }

    spdlog::info("Calibrating with {} images...", imagePoints.size());

    std::vector<cv::Mat> rvecs, tvecs;
    double rmsError = cv::calibrateCamera(
        objectPoints, imagePoints, imageSize,
        m_cameraMatrix, m_distCoeffs, rvecs, tvecs
    );

    m_calibrated = true;

    // Calculate pixels-per-mm from the first image
    if (!imagePoints.empty() && imagePoints[0].size() >= 2) {
        double pixelDist = cv::norm(imagePoints[0][0] - imagePoints[0][1]);
        m_pixelsPerMm = pixelDist / squareSize;
        spdlog::info("Pixels per mm: {:.2f}", m_pixelsPerMm);
    }

    // Pre-compute undistortion maps
    initUndistortMaps(imageSize);

    spdlog::info("Calibration complete. RMS error: {:.4f}", rmsError);
    return rmsError;
}

bool CameraCalibration::load(const std::string& path)
{
    try {
        cv::FileStorage fs(path, cv::FileStorage::READ);
        if (!fs.isOpened()) {
            spdlog::error("Cannot open calibration file: {}", path);
            return false;
        }

        fs["camera_matrix"] >> m_cameraMatrix;
        fs["dist_coeffs"] >> m_distCoeffs;
        fs["pixels_per_mm"] >> m_pixelsPerMm;

        int w, h;
        fs["image_width"] >> w;
        fs["image_height"] >> h;

        m_calibrated = !m_cameraMatrix.empty() && !m_distCoeffs.empty();

        if (m_calibrated) {
            initUndistortMaps(cv::Size(w, h));
            spdlog::info("Calibration loaded from '{}'", path);
        }

        return m_calibrated;
    } catch (const std::exception& e) {
        spdlog::error("Failed to load calibration: {}", e.what());
        return false;
    }
}

bool CameraCalibration::save(const std::string& path) const
{
    if (!m_calibrated) {
        spdlog::warn("No calibration data to save.");
        return false;
    }

    try {
        cv::FileStorage fs(path, cv::FileStorage::WRITE);
        if (!fs.isOpened()) {
            spdlog::error("Cannot create calibration file: {}", path);
            return false;
        }

        fs << "camera_matrix" << m_cameraMatrix;
        fs << "dist_coeffs" << m_distCoeffs;
        fs << "pixels_per_mm" << m_pixelsPerMm;

        if (!m_map1.empty()) {
            fs << "image_width" << m_map1.cols;
            fs << "image_height" << m_map1.rows;
        }

        spdlog::info("Calibration saved to '{}'", path);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Failed to save calibration: {}", e.what());
        return false;
    }
}

cv::Mat CameraCalibration::undistort(const cv::Mat& frame) const
{
    if (!m_calibrated) return frame.clone();

    cv::Mat undistorted;

    if (!m_map1.empty() && !m_map2.empty()) {
        // Fast path: use pre-computed maps
        cv::remap(frame, undistorted, m_map1, m_map2, cv::INTER_LINEAR);
    } else {
        cv::undistort(frame, undistorted, m_cameraMatrix, m_distCoeffs);
    }

    return undistorted;
}

void CameraCalibration::initUndistortMaps(cv::Size imageSize)
{
    if (!m_calibrated) return;

    cv::initUndistortRectifyMap(
        m_cameraMatrix, m_distCoeffs, cv::Mat(),
        m_cameraMatrix, imageSize, CV_32FC1,
        m_map1, m_map2
    );

    spdlog::debug("Undistortion maps initialized for {}x{}", imageSize.width, imageSize.height);
}

} // namespace ibom::camera
