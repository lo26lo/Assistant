#pragma once

#include <QOpenGLWidget>
#include <QImage>
#include <QPoint>
#include <QTimer>
#include <opencv2/core.hpp>
#include <mutex>
#include <vector>

namespace ibom::gui {

class CameraView : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit CameraView(QWidget* parent = nullptr);
    ~CameraView() override = default;

    void updateFrame(const QImage& frame);
    void setOverlayImage(const QImage& overlay);
    void setOverlayOpacity(float opacity);
    void setCrosshairVisible(bool visible);
    void setZoomLevel(float zoom);
    void setMeasurementMode(bool enabled);

    /// kind: -1=off, 0=Distance, 1=Angle, 2=Area, 3=PinPitch.
    /// Setting clears the in-progress points list.
    void setMeasureModeKind(int kind);
    int  measureModeKind() const { return m_measureModeKind; }

    /// Live calibration used to render the px/mm label during preview.
    void setPixelsPerMm(double v);

    /// Push a finished measurement into the rendered history.
    void appendCompletedMeasure(int mode, const std::vector<QPointF>& pts,
                                double valuePixels, double valueMM);
    void clearMeasureHistory();
    void clearCurrentMeasurePoints();

    float zoomLevel() const { return m_zoom; }
    QPointF mapToImage(const QPoint& widgetPos) const;
    QImage  captureView() const;

signals:
    void clicked(QPointF imagePos);
    void rightClicked(QPointF imagePos);
    void doubleClicked();
    void hovered(QPointF imagePos);
    void zoomChanged(float zoom);
    void measurePoint(QPointF imagePos);
    void measureCanceled();
    void areaCloseRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateTransform();
    void drawCrosshair(QPainter& painter);
    void drawMeasurement(QPainter& painter);
    void drawZoomIndicator(QPainter& painter);

    QPointF imageToWidget(QPointF imagePos) const;

    struct CompletedMeasure {
        int mode;
        std::vector<QPointF> points;
        double valuePixels;
        double valueMM;
    };

    QImage m_frame;
    QImage m_overlay;
    mutable std::mutex m_frameMutex;

    // View transform
    float   m_zoom       = 1.0f;
    QPointF m_panOffset  = {0, 0};
    QPointF m_lastMouse;
    bool    m_panning    = false;

    // Display options
    float m_overlayOpacity  = 0.5f;
    bool  m_crosshairVisible = true;
    bool  m_measureMode      = false;

    // Measurement (image-space coords)
    int     m_measureModeKind   = -1;     // -1=off, 0=Dist, 1=Angle, 2=Area, 3=Pitch
    double  m_pixelsPerMm       = 0.0;    // 0 = uncalibrated
    std::vector<QPointF>          m_measurePoints;
    QPointF                       m_measureCursor;
    bool                          m_measureCursorValid = false;
    std::vector<CompletedMeasure> m_measureHistory;

    // Computed transform
    QRectF m_imageRect;
    float  m_scale = 1.0f;
};

} // namespace ibom::gui
