#include "ImageUtils.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <QDateTime>
#include <filesystem>
#include <spdlog/spdlog.h>

namespace ibom::utils {

QImage ImageUtils::matToQImage(const cv::Mat& mat)
{
    if (mat.empty()) return {};

    switch (mat.type()) {
    case CV_8UC3: {
        cv::Mat rgb;
        cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
        return QImage(rgb.data, rgb.cols, rgb.rows,
                      static_cast<int>(rgb.step), QImage::Format_RGB888).copy();
    }
    case CV_8UC4: {
        cv::Mat rgba;
        cv::cvtColor(mat, rgba, cv::COLOR_BGRA2RGBA);
        return QImage(rgba.data, rgba.cols, rgba.rows,
                      static_cast<int>(rgba.step), QImage::Format_RGBA8888).copy();
    }
    case CV_8UC1: {
        return QImage(mat.data, mat.cols, mat.rows,
                      static_cast<int>(mat.step), QImage::Format_Grayscale8).copy();
    }
    default:
        spdlog::warn("ImageUtils: unsupported Mat type {}", mat.type());
        return {};
    }
}

cv::Mat ImageUtils::qImageToMat(const QImage& image)
{
    if (image.isNull()) return {};

    QImage converted;
    switch (image.format()) {
    case QImage::Format_RGB888:
        converted = image;
        break;
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
        converted = image.convertToFormat(QImage::Format_RGB888);
        break;
    default:
        converted = image.convertToFormat(QImage::Format_RGB888);
        break;
    }

    cv::Mat mat(converted.height(), converted.width(), CV_8UC3,
                const_cast<uchar*>(converted.bits()),
                static_cast<size_t>(converted.bytesPerLine()));

    cv::Mat bgr;
    cv::cvtColor(mat, bgr, cv::COLOR_RGB2BGR);
    return bgr;
}

cv::Mat ImageUtils::resizeKeepAspect(const cv::Mat& src, int maxWidth, int maxHeight)
{
    if (src.empty()) return {};

    double scale = std::min(
        static_cast<double>(maxWidth) / src.cols,
        static_cast<double>(maxHeight) / src.rows);

    if (scale >= 1.0) return src.clone();

    cv::Mat dst;
    cv::resize(src, dst, cv::Size(), scale, scale, cv::INTER_AREA);
    return dst;
}

cv::Mat ImageUtils::autoEnhance(const cv::Mat& src)
{
    if (src.empty()) return {};

    cv::Mat lab;
    cv::cvtColor(src, lab, cv::COLOR_BGR2Lab);

    std::vector<cv::Mat> channels;
    cv::split(lab, channels);

    // Apply CLAHE to L channel
    auto clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
    clahe->apply(channels[0], channels[0]);

    cv::merge(channels, lab);

    cv::Mat result;
    cv::cvtColor(lab, result, cv::COLOR_Lab2BGR);
    return result;
}

cv::Mat ImageUtils::whiteBalance(const cv::Mat& src)
{
    if (src.empty()) return {};

    // Gray-world white balance
    cv::Scalar mean = cv::mean(src);
    double avg = (mean[0] + mean[1] + mean[2]) / 3.0;

    cv::Mat result;
    std::vector<cv::Mat> channels;
    cv::split(src, channels);

    for (int i = 0; i < 3; ++i) {
        if (mean[i] > 0) {
            channels[i].convertTo(channels[i], -1, avg / mean[i]);
        }
    }

    cv::merge(channels, result);
    return result;
}

std::string ImageUtils::saveTimestamped(const cv::Mat& image,
                                         const std::string& directory,
                                         const std::string& prefix,
                                         const std::string& ext)
{
    namespace fs = std::filesystem;

    // Ensure directory exists
    fs::create_directories(directory);

    // Generate filename
    std::string timestamp = QDateTime::currentDateTime()
                                .toString("yyyyMMdd_HHmmss_zzz")
                                .toStdString();
    std::string filename = prefix + "_" + timestamp + ext;
    std::string path = directory + "/" + filename;

    if (cv::imwrite(path, image)) {
        spdlog::debug("ImageUtils: saved '{}'", path);
        return path;
    } else {
        spdlog::error("ImageUtils: failed to save '{}'", path);
        return {};
    }
}

double ImageUtils::computeSharpness(const cv::Mat& image)
{
    if (image.empty()) return 0;

    cv::Mat gray;
    if (image.channels() == 3)
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    else
        gray = image;

    cv::Mat laplacian;
    cv::Laplacian(gray, laplacian, CV_64F);

    cv::Scalar mean, stddev;
    cv::meanStdDev(laplacian, mean, stddev);

    return stddev[0] * stddev[0]; // Variance
}

bool ImageUtils::isBlurry(const cv::Mat& image, double threshold)
{
    return computeSharpness(image) < threshold;
}

cv::Mat ImageUtils::sideBySide(const cv::Mat& left, const cv::Mat& right, int gap)
{
    if (left.empty() && right.empty()) return {};
    if (left.empty()) return right.clone();
    if (right.empty()) return left.clone();

    // Ensure same height
    int h = std::max(left.rows, right.rows);
    cv::Mat lResized, rResized;

    if (left.rows != h) {
        double scale = static_cast<double>(h) / left.rows;
        cv::resize(left, lResized, cv::Size(), scale, scale);
    } else {
        lResized = left;
    }

    if (right.rows != h) {
        double scale = static_cast<double>(h) / right.rows;
        cv::resize(right, rResized, cv::Size(), scale, scale);
    } else {
        rResized = right;
    }

    int totalWidth = lResized.cols + gap + rResized.cols;
    cv::Mat result = cv::Mat::zeros(h, totalWidth, lResized.type());

    lResized.copyTo(result(cv::Rect(0, 0, lResized.cols, h)));
    rResized.copyTo(result(cv::Rect(lResized.cols + gap, 0, rResized.cols, h)));

    return result;
}

void ImageUtils::drawTextWithBg(cv::Mat& image, const std::string& text,
                                 cv::Point origin, double fontScale,
                                 cv::Scalar textColor, cv::Scalar bgColor)
{
    int baseline = 0;
    cv::Size textSize = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX,
                                         fontScale, 1, &baseline);

    cv::Rect bgRect(origin.x - 2, origin.y - textSize.height - 4,
                     textSize.width + 4, textSize.height + baseline + 8);

    // Clamp to image bounds
    bgRect &= cv::Rect(0, 0, image.cols, image.rows);

    // Draw semi-transparent background
    cv::Mat roi = image(bgRect);
    cv::Mat color(roi.size(), roi.type(), bgColor);
    double alpha = bgColor[3] / 255.0;
    cv::addWeighted(color, alpha, roi, 1.0 - alpha, 0, roi);

    // Draw text
    cv::putText(image, text, origin, cv::FONT_HERSHEY_SIMPLEX,
                fontScale, textColor, 1, cv::LINE_AA);
}

} // namespace ibom::utils
