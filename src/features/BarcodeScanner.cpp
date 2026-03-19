#include "BarcodeScanner.h"

#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>

// Conditionally include ZXing if available
#if __has_include(<ZXing/ReadBarcode.h>)
    #include <ZXing/ReadBarcode.h>
    #include <ZXing/BarcodeFormat.h>
    #define HAS_ZXING 1
#else
    #define HAS_ZXING 0
#endif

namespace ibom::features {

BarcodeScanner::BarcodeScanner(QObject* parent)
    : QObject(parent)
{
#if HAS_ZXING
    m_available = true;
    spdlog::info("BarcodeScanner: ZXing-cpp available");
#else
    m_available = false;
    spdlog::warn("BarcodeScanner: ZXing-cpp not available, barcode scanning disabled");
#endif
}

std::vector<BarcodeScanner::ScanResult> BarcodeScanner::scan(const cv::Mat& frame)
{
    std::vector<ScanResult> results;

    if (frame.empty() || !m_available) return results;

#if HAS_ZXING
    // Convert cv::Mat to ZXing ImageView
    auto format = ZXing::ImageFormat::Lum;
    cv::Mat gray;
    if (frame.channels() == 3) {
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = frame;
    }

    ZXing::ImageView image(gray.data, gray.cols, gray.rows, format,
                            static_cast<int>(gray.step));

    // Configure formats
    ZXing::BarcodeFormats formats;
    if (m_enableQR)         formats |= ZXing::BarcodeFormat::QRCode;
    if (m_enableCode128)    formats |= ZXing::BarcodeFormat::Code128;
    if (m_enableDataMatrix) formats |= ZXing::BarcodeFormat::DataMatrix;
    if (m_enableEAN)        formats |= ZXing::BarcodeFormat::EAN13 | ZXing::BarcodeFormat::EAN8;

    ZXing::ReaderOptions options;
    options.setFormats(formats);
    options.setTryHarder(true);
    options.setTryRotate(true);

    auto zxResults = ZXing::ReadBarcodes(image, options);

    for (const auto& r : zxResults) {
        ScanResult sr;
        sr.text = r.text();
        sr.format = ZXing::ToString(r.format());

        auto pos = r.position();
        int minX = std::min({pos[0].x, pos[1].x, pos[2].x, pos[3].x});
        int minY = std::min({pos[0].y, pos[1].y, pos[2].y, pos[3].y});
        int maxX = std::max({pos[0].x, pos[1].x, pos[2].x, pos[3].x});
        int maxY = std::max({pos[0].y, pos[1].y, pos[2].y, pos[3].y});
        sr.boundingBox = cv::Rect(minX, minY, maxX - minX, maxY - minY);
        sr.confidence = 1.0f; // ZXing doesn't provide confidence

        results.push_back(std::move(sr));

        emit barcodeDetected(QString::fromStdString(sr.text),
                              QString::fromStdString(sr.format));
    }
#endif

    return results;
}

QString BarcodeScanner::scanForComponent(const cv::Mat& frame,
                                          const std::vector<std::string>& knownReferences)
{
    auto results = scan(frame);

    for (const auto& r : results) {
        // Direct match
        for (const auto& ref : knownReferences) {
            if (r.text == ref) {
                emit componentMatched(QString::fromStdString(ref));
                return QString::fromStdString(ref);
            }
        }

        // Partial match (barcode might contain prefix/suffix)
        for (const auto& ref : knownReferences) {
            if (r.text.find(ref) != std::string::npos) {
                emit componentMatched(QString::fromStdString(ref));
                return QString::fromStdString(ref);
            }
        }
    }

    return {};
}

void BarcodeScanner::setFormatsEnabled(bool qr, bool code128, bool dataMatrix, bool ean)
{
    m_enableQR = qr;
    m_enableCode128 = code128;
    m_enableDataMatrix = dataMatrix;
    m_enableEAN = ean;
}

} // namespace ibom::features
