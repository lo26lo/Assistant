#pragma once

#include <QObject>
#include <QImage>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace ibom::features {

/// Barcode and QR code scanner using ZXing-cpp
class BarcodeScanner : public QObject {
    Q_OBJECT

public:
    explicit BarcodeScanner(QObject* parent = nullptr);
    ~BarcodeScanner() override = default;

    struct ScanResult {
        std::string text;
        std::string format;      // "QR_CODE", "CODE_128", "DATA_MATRIX", etc.
        cv::Rect    boundingBox;
        float       confidence;
    };

    /// Scan a frame for barcodes/QR codes
    std::vector<ScanResult> scan(const cv::Mat& frame);

    /// Scan and try to match to a component reference or part number
    QString scanForComponent(const cv::Mat& frame,
                             const std::vector<std::string>& knownReferences);

    /// Enable/disable specific barcode formats
    void setFormatsEnabled(bool qr, bool code128, bool dataMatrix, bool ean);

    bool isAvailable() const { return m_available; }

signals:
    void barcodeDetected(const QString& text, const QString& format);
    void componentMatched(const QString& reference);

private:
    bool m_available    = false;
    bool m_enableQR     = true;
    bool m_enableCode128 = true;
    bool m_enableDataMatrix = true;
    bool m_enableEAN    = false;
};

} // namespace ibom::features
