#include "CameraView.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <cmath>

namespace ibom::gui {

CameraView::CameraView(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(640, 480);
}

void CameraView::updateFrame(const QImage& frame)
{
    {
        std::lock_guard lock(m_frameMutex);
        m_frame = frame;
    }
    update();
}

void CameraView::setOverlayImage(const QImage& overlay)
{
    m_overlay = overlay;
    update();
}

void CameraView::setOverlayOpacity(float opacity)
{
    m_overlayOpacity = std::clamp(opacity, 0.0f, 1.0f);
    update();
}

void CameraView::setCrosshairVisible(bool visible)
{
    m_crosshairVisible = visible;
    update();
}

void CameraView::setZoomLevel(float zoom)
{
    m_zoom = std::clamp(zoom, 0.1f, 20.0f);
    updateTransform();
    update();
    emit zoomChanged(m_zoom);
}

void CameraView::setMeasurementMode(bool enabled)
{
    m_measureMode = enabled;
    m_measuring = false;
    update();
}

QPointF CameraView::mapToImage(const QPoint& widgetPos) const
{
    if (m_frame.isNull() || m_scale <= 0) return {};

    float x = (widgetPos.x() - m_imageRect.x() - m_panOffset.x()) / (m_scale * m_zoom);
    float y = (widgetPos.y() - m_imageRect.y() - m_panOffset.y()) / (m_scale * m_zoom);
    return {x, y};
}

QImage CameraView::captureView() const
{
    QImage capture(size(), QImage::Format_ARGB32);
    capture.fill(Qt::black);
    QPainter p(&capture);

    std::lock_guard lock(m_frameMutex);
    if (!m_frame.isNull()) {
        p.drawImage(m_imageRect.translated(m_panOffset), m_frame);
    }
    if (!m_overlay.isNull()) {
        p.setOpacity(m_overlayOpacity);
        p.drawImage(m_imageRect.translated(m_panOffset), m_overlay);
        p.setOpacity(1.0);
    }
    return capture;
}

// ── Painting ─────────────────────────────────────────────────────

void CameraView::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.fillRect(rect(), Qt::black);

    std::lock_guard lock(m_frameMutex);

    if (m_frame.isNull()) {
        painter.setPen(QColor(120, 120, 120));
        painter.setFont(QFont("Segoe UI", 14));
        painter.drawText(rect(), Qt::AlignCenter, tr("No camera feed\nPress C to start"));
        return;
    }

    // Draw camera frame
    QRectF target = m_imageRect.translated(m_panOffset);
    painter.drawImage(target, m_frame);

    // Draw overlay
    if (!m_overlay.isNull()) {
        painter.setOpacity(m_overlayOpacity);
        painter.drawImage(target, m_overlay);
        painter.setOpacity(1.0);
    }

    // Draw crosshair
    if (m_crosshairVisible)
        drawCrosshair(painter);

    // Draw measurement
    if (m_measureMode && m_measuring)
        drawMeasurement(painter);

    // Zoom indicator
    if (std::abs(m_zoom - 1.0f) > 0.01f)
        drawZoomIndicator(painter);
}

void CameraView::drawCrosshair(QPainter& painter)
{
    painter.setPen(QPen(QColor(0, 255, 0, 128), 1, Qt::DashLine));
    int cx = width()  / 2;
    int cy = height() / 2;
    painter.drawLine(cx, 0, cx, height());
    painter.drawLine(0, cy, width(), cy);

    // Center circle
    painter.drawEllipse(QPoint(cx, cy), 20, 20);
}

void CameraView::drawMeasurement(QPainter& painter)
{
    painter.setPen(QPen(QColor(255, 255, 0), 2));

    // Convert image coords to widget
    QPointF s(m_measureStart.x() * m_scale * m_zoom + m_imageRect.x() + m_panOffset.x(),
              m_measureStart.y() * m_scale * m_zoom + m_imageRect.y() + m_panOffset.y());
    QPointF e(m_measureEnd.x()   * m_scale * m_zoom + m_imageRect.x() + m_panOffset.x(),
              m_measureEnd.y()   * m_scale * m_zoom + m_imageRect.y() + m_panOffset.y());

    painter.drawLine(s, e);

    // Distance in pixels
    double dx = m_measureEnd.x() - m_measureStart.x();
    double dy = m_measureEnd.y() - m_measureStart.y();
    double dist = std::sqrt(dx * dx + dy * dy);

    QPointF mid = (s + e) / 2.0;
    painter.setFont(QFont("Segoe UI", 10, QFont::Bold));
    painter.drawText(mid + QPointF(5, -5), QString("%1 px").arg(dist, 0, 'f', 1));
}

void CameraView::drawZoomIndicator(QPainter& painter)
{
    painter.setPen(Qt::white);
    painter.setFont(QFont("Segoe UI", 10));
    QString text = QString("%1x").arg(m_zoom, 0, 'f', 1);
    QRect r(width() - 60, 8, 52, 22);
    painter.fillRect(r, QColor(0, 0, 0, 160));
    painter.drawText(r, Qt::AlignCenter, text);
}

// ── Mouse Events ─────────────────────────────────────────────────

void CameraView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_lastMouse = event->position();
        setCursor(Qt::ClosedHandCursor);
    } else if (event->button() == Qt::LeftButton) {
        QPointF imgPos = mapToImage(event->pos());

        if (m_measureMode) {
            if (!m_measuring) {
                m_measureStart = imgPos;
                m_measuring = true;
            } else {
                m_measureEnd = imgPos;
                m_measuring = false;
                update();
                emit measurePoint(imgPos);
            }
        } else {
            emit clicked(imgPos);
        }
    } else if (event->button() == Qt::RightButton) {
        QPointF imgPos = mapToImage(event->pos());
        emit rightClicked(imgPos);
    }
}

void CameraView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_panning) {
        QPointF delta = event->position() - m_lastMouse;
        m_panOffset += delta;
        m_lastMouse = event->position();
        update();
    }

    if (m_measureMode && m_measuring) {
        m_measureEnd = mapToImage(event->pos());
        update();
    }

    QPointF imgPos = mapToImage(event->pos());
    emit hovered(imgPos);
}

void CameraView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        m_panning = false;
        setCursor(Qt::ArrowCursor);
    }
}

void CameraView::wheelEvent(QWheelEvent* event)
{
    float factor = event->angleDelta().y() > 0 ? 1.15f : 1.0f / 1.15f;
    float newZoom = std::clamp(m_zoom * factor, 0.1f, 20.0f);

    // Zoom toward mouse position
    QPointF mousePos = event->position();
    QPointF beforeZoom = mapToImage(mousePos.toPoint());

    m_zoom = newZoom;
    updateTransform();

    QPointF afterZoom = mapToImage(mousePos.toPoint());
    QPointF diff = afterZoom - beforeZoom;
    m_panOffset += QPointF(diff.x() * m_scale * m_zoom,
                           diff.y() * m_scale * m_zoom);

    update();
    emit zoomChanged(m_zoom);
}

void CameraView::resizeEvent(QResizeEvent* event)
{
    QOpenGLWidget::resizeEvent(event);
    updateTransform();
}

void CameraView::updateTransform()
{
    if (m_frame.isNull()) return;

    float imgW = m_frame.width();
    float imgH = m_frame.height();
    float widW = width();
    float widH = height();

    m_scale = std::min(widW / imgW, widH / imgH) * m_zoom;

    float scaledW = imgW * m_scale;
    float scaledH = imgH * m_scale;
    float x = (widW - scaledW) / 2.0f;
    float y = (widH - scaledH) / 2.0f;

    m_imageRect = QRectF(x, y, scaledW, scaledH);
}

} // namespace ibom::gui
