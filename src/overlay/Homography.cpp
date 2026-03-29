#include "Homography.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include <chrono>

namespace ibom::overlay {

bool Homography::compute(const std::vector<cv::Point2f>& pcbPoints,
                          const std::vector<cv::Point2f>& imagePoints)
{
    if (pcbPoints.size() < 4 || pcbPoints.size() != imagePoints.size()) {
        spdlog::error("Homography requires at least 4 matching point pairs. Got {}.", pcbPoints.size());
        m_valid = false;
        return false;
    }

    // Use RANSAC for robustness
    std::vector<uchar> inliersMask;
    m_homography = cv::findHomography(pcbPoints, imagePoints, cv::RANSAC, 3.0, inliersMask);

    if (m_homography.empty()) {
        spdlog::error("Failed to compute homography.");
        m_valid = false;
        return false;
    }

    // Compute inverse
    m_inverse = m_homography.inv();

    // Count inliers
    int inliers = cv::countNonZero(inliersMask);

    // Compute reprojection error
    m_reprojError = 0.0;
    std::vector<cv::Point2f> projected;
    cv::perspectiveTransform(pcbPoints, projected, m_homography);
    for (size_t i = 0; i < projected.size(); ++i) {
        m_reprojError += cv::norm(projected[i] - imagePoints[i]);
    }
    m_reprojError /= projected.size();

    // Only log when not called in a tight loop (>1s since last log)
    static auto lastLogTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLogTime).count() > 1000) {
        spdlog::info("Homography computed: {}/{} inliers, error: {:.3f} px", inliers, pcbPoints.size(), m_reprojError);
        lastLogTime = now;
    }

    m_valid = true;
    return true;
}

cv::Point2f Homography::pcbToImage(cv::Point2f pcbPoint) const
{
    if (!m_valid) return pcbPoint;

    std::vector<cv::Point2f> in = {pcbPoint};
    std::vector<cv::Point2f> out;
    cv::perspectiveTransform(in, out, m_homography);
    return out[0];
}

cv::Point2f Homography::imageToPcb(cv::Point2f imagePoint) const
{
    if (!m_valid) return imagePoint;

    std::vector<cv::Point2f> in = {imagePoint};
    std::vector<cv::Point2f> out;
    cv::perspectiveTransform(in, out, m_inverse);
    return out[0];
}

std::vector<cv::Point2f> Homography::pcbToImage(const std::vector<cv::Point2f>& pcbPoints) const
{
    if (!m_valid) return pcbPoints;

    std::vector<cv::Point2f> out;
    cv::perspectiveTransform(pcbPoints, out, m_homography);
    return out;
}

std::vector<cv::Point2f> Homography::transformRect(float x, float y, float w, float h) const
{
    std::vector<cv::Point2f> corners = {
        {x,     y},
        {x + w, y},
        {x + w, y + h},
        {x,     y + h}
    };
    return pcbToImage(corners);
}

bool Homography::save(const std::string& path) const
{
    if (!m_valid) return false;

    cv::FileStorage fs(path, cv::FileStorage::WRITE);
    if (!fs.isOpened()) return false;

    fs << "homography" << m_homography;
    fs << "inverse" << m_inverse;
    fs << "reproj_error" << m_reprojError;

    spdlog::debug("Homography saved to '{}'", path);
    return true;
}

bool Homography::load(const std::string& path)
{
    cv::FileStorage fs(path, cv::FileStorage::READ);
    if (!fs.isOpened()) return false;

    fs["homography"] >> m_homography;
    fs["inverse"] >> m_inverse;
    fs["reproj_error"] >> m_reprojError;

    m_valid = !m_homography.empty() && !m_inverse.empty();
    if (m_valid) {
        spdlog::info("Homography loaded from '{}' (error: {:.3f})", path, m_reprojError);
    }
    return m_valid;
}

void Homography::reset()
{
    m_homography = cv::Mat();
    m_inverse = cv::Mat();
    m_valid = false;
    m_reprojError = 0.0;
}

void Homography::setMatrix(const cv::Mat& matrix)
{
    if (matrix.empty()) {
        reset();
        return;
    }
    m_homography = matrix.clone();
    m_inverse = m_homography.inv();
    m_valid = true;
}

} // namespace ibom::overlay
