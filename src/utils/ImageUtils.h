#pragma once

#include <QImage>
#include <opencv2/core.hpp>
#include <string>

namespace ibom::utils {

/// Image conversion and processing utilities
class ImageUtils {
public:
    /// Convert cv::Mat (BGR) to QImage (RGB)
    static QImage matToQImage(const cv::Mat& mat);

    /// Convert QImage to cv::Mat (BGR)
    static cv::Mat qImageToMat(const QImage& image);

    /// Resize keeping aspect ratio
    static cv::Mat resizeKeepAspect(const cv::Mat& src, int maxWidth, int maxHeight);

    /// Auto-brightness / contrast (CLAHE)
    static cv::Mat autoEnhance(const cv::Mat& src);

    /// White balance correction
    static cv::Mat whiteBalance(const cv::Mat& src);

    /// Save image with timestamp filename
    static std::string saveTimestamped(const cv::Mat& image,
                                        const std::string& directory,
                                        const std::string& prefix = "capture",
                                        const std::string& ext = ".png");

    /// Compute image sharpness (Laplacian variance)
    static double computeSharpness(const cv::Mat& image);

    /// Check if image is too blurry
    static bool isBlurry(const cv::Mat& image, double threshold = 100.0);

    /// Create a side-by-side comparison image
    static cv::Mat sideBySide(const cv::Mat& left, const cv::Mat& right,
                               int gap = 4);

    /// Draw text with background for readability
    static void drawTextWithBg(cv::Mat& image, const std::string& text,
                                cv::Point origin, double fontScale = 0.5,
                                cv::Scalar textColor = {255, 255, 255},
                                cv::Scalar bgColor = {0, 0, 0, 180});
};

} // namespace ibom::utils
