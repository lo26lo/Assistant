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
    updateTransform();
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
    if (!enabled) {
        m_measurePoints.clear();
        m_measureCursorValid = false;
        m_measureModeKind = -1;
    }
    update();
}

void CameraView::setMeasureModeKind(int kind)
{
    m_measureModeKind = kind;
    m_measurePoints.clear();
    m_measureCursorValid = false;
    update();
}

void CameraView::setPixelsPerMm(double v)
{
    m_pixelsPerMm = v;
    update();
}

void CameraView::appendCompletedMeasure(int mode, const std::vector<QPointF>& pts,
                                        double valuePixels, double valueMM)
{
    m_measureHistory.push_back({mode, pts, valuePixels, valueMM});
    m_measurePoints.clear();
    update();
}

void CameraView::clearMeasureHistory()
{
    m_measureHistory.clear();
    update();
}

void CameraView::clearCurrentMeasurePoints()
{
    m_measurePoints.clear();
    m_measureCursorValid = false;
    update();
}

QPointF CameraView::imageToWidget(QPointF imagePos) const
{
    return QPointF(imagePos.x() * m_scale + m_imageRect.x() + m_panOffset.x(),
                   imagePos.y() * m_scale + m_imageRect.y() + m_panOffset.y());
}

QPointF CameraView::mapToImage(const QPoint& widgetPos) const
{
    if (m_frame.isNull() || m_scale <= 0) return {};

    float x = (widgetPos.x() - m_imageRect.x() - m_panOffset.x()) / m_scale;
    float y = (widgetPos.y() - m_imageRect.y() - m_panOffset.y()) / m_scale;
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
        // Draw centered placeholder with subtle gradient background
        QLinearGradient grad(0, 0, 0, height());
        grad.setColorAt(0,   QColor(14, 14, 20));
        grad.setColorAt(0.5, QColor(18, 18, 28));
        grad.setColorAt(1,   QColor(14, 14, 20));
        painter.fillRect(rect(), grad);

        // Camera icon (simple geometric)
        int cx = width() / 2;
        int cy = height() / 2 - 20;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(30, 30, 50));
        painter.drawRoundedRect(cx - 32, cy - 22, 64, 44, 8, 8);
        painter.setBrush(QColor(45, 45, 70));
        painter.drawEllipse(QPoint(cx, cy), 14, 14);
        painter.setBrush(QColor(60, 68, 100));
        painter.drawEllipse(QPoint(cx, cy), 8, 8);

        // Text
        QColor textCol(120, 130, 160);
        QColor subtextCol(70, 78, 110);

        painter.setPen(textCol);
        painter.setFont(QFont("Segoe UI", 15, QFont::DemiBold));
        QRect textRect(0, cy + 36, width(), 30);
        painter.drawText(textRect, Qt::AlignHCenter | Qt::AlignTop, tr("No camera feed"));

        painter.setPen(subtextCol);
        painter.setFont(QFont("Segoe UI", 11));
        QRect subRect(0, cy + 62, width(), 24);
        painter.drawText(subRect, Qt::AlignHCenter | Qt::AlignTop, tr("Press C or click Start Camera"));
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

    // Draw measurement (current + history)
    if (m_measureMode || !m_measureHistory.empty())
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
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(QFont("Segoe UI", 10, QFont::Bold));

    auto drawPointDot = [&](QPointF widgetPt, const QColor& c) {
        painter.setPen(QPen(QColor(0, 0, 0, 200), 1));
        painter.setBrush(c);
        painter.drawEllipse(widgetPt, 4.5, 4.5);
    };

    auto drawLabel = [&](QPointF widgetPt, const QString& text, const QColor& fg) {
        QFontMetrics fm(painter.font());
        QRect r = fm.boundingRect(text).adjusted(-4, -2, 4, 2);
        r.moveTo(widgetPt.x() + 8, widgetPt.y() - r.height() - 4);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 170));
        painter.drawRoundedRect(r, 3, 3);
        painter.setPen(fg);
        painter.drawText(r, Qt::AlignCenter, text);
    };

    auto formatLength = [&](double valuePx) -> QString {
        if (m_pixelsPerMm > 0)
            return QString("%1 px  •  %2 mm")
                .arg(valuePx, 0, 'f', 1)
                .arg(valuePx / m_pixelsPerMm, 0, 'f', 3);
        return QString("%1 px").arg(valuePx, 0, 'f', 1);
    };

    auto formatArea = [&](double valuePx2) -> QString {
        if (m_pixelsPerMm > 0)
            return QString("%1 px²  •  %2 mm²")
                .arg(valuePx2, 0, 'f', 0)
                .arg(valuePx2 / (m_pixelsPerMm * m_pixelsPerMm), 0, 'f', 3);
        return QString("%1 px²").arg(valuePx2, 0, 'f', 0);
    };

    auto distance = [](QPointF a, QPointF b) {
        double dx = b.x() - a.x(), dy = b.y() - a.y();
        return std::sqrt(dx * dx + dy * dy);
    };

    auto angleDeg = [](QPointF a, QPointF v, QPointF b) {
        double ax = a.x() - v.x(), ay = a.y() - v.y();
        double bx = b.x() - v.x(), by = b.y() - v.y();
        double magA = std::sqrt(ax * ax + ay * ay);
        double magB = std::sqrt(bx * bx + by * by);
        if (magA < 1e-9 || magB < 1e-9) return 0.0;
        double c = std::clamp((ax * bx + ay * by) / (magA * magB), -1.0, 1.0);
        return std::acos(c) * 180.0 / M_PI;
    };

    auto polygonAreaPx = [](const std::vector<QPointF>& pts) {
        double a = 0;
        int n = static_cast<int>(pts.size());
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            a += pts[i].x() * pts[j].y() - pts[j].x() * pts[i].y();
        }
        return std::abs(a) / 2.0;
    };

    auto centroid = [](const std::vector<QPointF>& pts) {
        QPointF c(0, 0);
        for (auto& p : pts) c += p;
        if (!pts.empty()) c /= static_cast<double>(pts.size());
        return c;
    };

    auto renderShape = [&](int mode, const std::vector<QPointF>& pts,
                           bool live, const QColor& strokeCol) {
        if (pts.empty()) return;

        std::vector<QPointF> w; w.reserve(pts.size());
        for (auto& p : pts) w.push_back(imageToWidget(p));

        QPen pen(strokeCol, 2);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        // Live preview segment from last point to cursor
        QPointF cursorW;
        bool hasCursor = live && m_measureCursorValid &&
                         m_measureMode && mode == m_measureModeKind;
        if (hasCursor) cursorW = imageToWidget(m_measureCursor);

        if (mode == 0 || mode == 3) { // Distance / PinPitch
            if (w.size() >= 2) {
                painter.drawLine(w[0], w[1]);
                double d = distance(pts[0], pts[1]);
                drawLabel((w[0] + w[1]) / 2.0, formatLength(d), strokeCol);
            } else if (w.size() == 1 && hasCursor) {
                QPen pp = pen; pp.setStyle(Qt::DashLine); painter.setPen(pp);
                painter.drawLine(w[0], cursorW);
                double d = distance(pts[0], m_measureCursor);
                drawLabel((w[0] + cursorW) / 2.0, formatLength(d), strokeCol);
            }
        }
        else if (mode == 1) { // Angle: pts[0]→pts[1] (vertex) →pts[2]
            if (w.size() >= 2) painter.drawLine(w[0], w[1]);
            if (w.size() >= 3) {
                painter.drawLine(w[1], w[2]);
                double deg = angleDeg(pts[0], pts[1], pts[2]);
                drawLabel(w[1], QString("%1°").arg(deg, 0, 'f', 1), strokeCol);
            } else if (hasCursor && w.size() >= 1) {
                QPen pp = pen; pp.setStyle(Qt::DashLine); painter.setPen(pp);
                painter.drawLine(w.back(), cursorW);
                if (w.size() == 2) {
                    double deg = angleDeg(pts[0], pts[1], m_measureCursor);
                    drawLabel(w[1], QString("%1°").arg(deg, 0, 'f', 1), strokeCol);
                }
            }
        }
        else if (mode == 2) { // Area: polyline + closing edge
            for (size_t i = 0; i + 1 < w.size(); ++i)
                painter.drawLine(w[i], w[i + 1]);
            if (live) {
                if (hasCursor && !w.empty()) {
                    QPen pp = pen; pp.setStyle(Qt::DashLine); painter.setPen(pp);
                    painter.drawLine(w.back(), cursorW);
                    if (w.size() >= 2) painter.drawLine(cursorW, w.front());
                }
                if (pts.size() >= 3) {
                    double a = polygonAreaPx(pts);
                    drawLabel(imageToWidget(centroid(pts)), formatArea(a), strokeCol);
                }
            } else if (w.size() >= 3) {
                painter.drawLine(w.back(), w.front());
                double a = polygonAreaPx(pts);
                drawLabel(imageToWidget(centroid(pts)), formatArea(a), strokeCol);
            }
        }

        // Restore solid pen for the dots
        painter.setPen(pen);
        for (auto& p : w) drawPointDot(p, strokeCol);
        if (hasCursor) drawPointDot(cursorW, strokeCol.lighter(150));
    };

    // History (faded)
    QColor histCol(255, 220, 0, 110);
    for (auto& m : m_measureHistory)
        renderShape(m.mode, m.points, /*live=*/false, histCol);

    // Current (bright)
    if (m_measureMode && m_measureModeKind >= 0)
        renderShape(m_measureModeKind, m_measurePoints, /*live=*/true,
                    QColor(255, 230, 30));
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

        if (m_measureMode && m_measureModeKind >= 0) {
            // Append locally for instant rendering; Application's Measurement
            // is the source of truth for the computed value and will signal
            // back via appendCompletedMeasure() when the shape closes.
            m_measurePoints.push_back(imgPos);
            update();
            emit measurePoint(imgPos);
        } else {
            emit clicked(imgPos);
        }
    } else if (event->button() == Qt::RightButton) {
        QPointF imgPos = mapToImage(event->pos());
        if (m_measureMode && m_measureModeKind >= 0 && !m_measurePoints.empty()) {
            m_measurePoints.clear();
            update();
            emit measureCanceled();
        } else {
            emit rightClicked(imgPos);
        }
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

    if (m_measureMode && m_measureModeKind >= 0) {
        m_measureCursor = mapToImage(event->pos());
        m_measureCursorValid = true;
        update();
    }

    QPointF imgPos = mapToImage(event->pos());
    emit hovered(imgPos);
}

void CameraView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    // In Area mode with ≥3 points, double-click closes the polygon and
    // commits the measurement instead of toggling fullscreen.
    if (m_measureMode && m_measureModeKind == 2 && m_measurePoints.size() >= 3) {
        emit areaCloseRequested();
        return;
    }
    emit doubleClicked();
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
    m_panOffset += QPointF(diff.x() * m_scale,
                           diff.y() * m_scale);

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
