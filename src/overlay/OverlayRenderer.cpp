#include "OverlayRenderer.h"

#include "ibom/IBomData.h"

#include <QPainter>
#include <QFont>
#include <QPen>
#include <QPolygonF>
#include <QString>
#include <algorithm>
#include <cmath>
#include <limits>

namespace ibom::overlay {

QTransform OverlayRenderer::toQTransform(const cv::Mat& h3x3)
{
    if (h3x3.empty() || h3x3.rows != 3 || h3x3.cols != 3)
        return {};
    cv::Mat h;
    h3x3.convertTo(h, CV_64F);
    // cv maps column vectors (p' = H·p); QTransform maps row vectors
    // (p' = p·T). T = Hᵀ, passed row-major to the 9-argument constructor.
    return QTransform(
        h.at<double>(0, 0), h.at<double>(1, 0), h.at<double>(2, 0),
        h.at<double>(0, 1), h.at<double>(1, 1), h.at<double>(2, 1),
        h.at<double>(0, 2), h.at<double>(1, 2), h.at<double>(2, 2));
}

BoardOverlay OverlayRenderer::renderBoardSpace(const OverlayInputs& in)
{
    if (!in.project) return {};
    const auto& bb = in.project->boardInfo.boardBBox;
    double minX = bb.minX, minY = bb.minY, maxX = bb.maxX, maxY = bb.maxY;
    if (maxX - minX <= 0.0 || maxY - minY <= 0.0) {
        // Degenerate board bbox (iBOM without edge drawings): fall back to the
        // union of component bboxes so the overlay still renders — the old
        // full-frame renderer never depended on the board bbox at all.
        minX = minY = std::numeric_limits<double>::max();
        maxX = maxY = std::numeric_limits<double>::lowest();
        for (const auto& c : in.project->components) {
            minX = std::min(minX, c.bbox.minX);
            minY = std::min(minY, c.bbox.minY);
            maxX = std::max(maxX, c.bbox.maxX);
            maxY = std::max(maxY, c.bbox.maxY);
        }
    }
    const double bw = maxX - minX;
    const double bh = maxY - minY;
    if (bw <= 0.0 || bh <= 0.0) return {};

    // Silkscreen can overhang the board edge slightly — pad the canvas.
    constexpr double kMarginMm = 2.0;
    // Buffer resolution (larger side). 2048 ≈ 2.4× the D405's 848 px frame
    // width and ~1.9× a 1080p microscope frame — enough that the projective
    // warp stays crisp at typical zooms. Bump it if the overlay looks soft at
    // very high magnification (cost: ~4·W·H bytes of ARGB).
    constexpr int kMaxDim = 2048;

    const double totalW = bw + 2.0 * kMarginMm;
    const double totalH = bh + 2.0 * kMarginMm;
    const double s  = kMaxDim / std::max(totalW, totalH);  // buffer px per PCB mm
    const double ox = minX - kMarginMm;                    // buffer (0,0) in PCB coords
    const double oy = minY - kMarginMm;
    const int bufW = std::max(1, static_cast<int>(std::lround(totalW * s)));
    const int bufH = std::max(1, static_cast<int>(std::lround(totalH * s)));

    QImage img(bufW, bufH, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter painter(&img);
    // This render is rare (selection/placed/toggle/color/project changes
    // only), so shape antialiasing is affordable again — the per-frame cost
    // is the warp in CameraView, not this tessellation.
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const auto mapX = [&](double x) { return (x - ox) * s; };
    const auto mapY = [&](double y) { return (y - oy) * s; };

    // Labels live in board space now, so they scale with zoom (AR-style)
    // instead of keeping a fixed screen size: ~0.6 mm cap height reads when
    // the board fills the frame and grows under magnification.
    QFont normalLabelFont("Segoe UI");
    normalLabelFont.setPixelSize(std::max(6, static_cast<int>(std::lround(0.6 * s))));
    QFont selectedLabelFont("Segoe UI");
    selectedLabelFont.setPixelSize(std::max(8, static_cast<int>(std::lround(0.9 * s))));
    selectedLabelFont.setBold(true);

    auto withAlpha = [](QColor c, int a) { c.setAlpha(a); return c; };

    for (const auto& comp : in.project->components) {
        if (comp.layer != Layer::Front) continue;

        const bool isSelected = (comp.reference == in.selectedRef);
        const bool isPlaced   = !isSelected && in.placedRefs.count(comp.reference) > 0;

        QColor padColor, silkColor, labelColor;
        qreal silkWidth = 1.0;
        if (isSelected) {
            padColor   = withAlpha(in.cSelected, 220);
            silkColor  = withAlpha(in.cSelected, 240);
            labelColor = withAlpha(in.cSelected, 255);
            silkWidth  = in.selectedSilkW;
        } else if (isPlaced) {
            int a = static_cast<int>(180 * in.placedAlphaMul);
            padColor   = withAlpha(in.cPlaced, a);
            silkColor  = withAlpha(in.cPlaced, std::min(255, a + 30));
            labelColor = withAlpha(in.cPlaced, std::min(255, a + 60));
        } else {
            padColor   = withAlpha(in.cNormal, 180);
            silkColor  = withAlpha(in.cNormal, 180);
            labelColor = in.labelNormal;
        }

        // ── Pads ── axis-aligned in PCB space (pad rotation was already
        // ignored by the previous renderer — sizeX/sizeY only), so a plain
        // rect/ellipse in buffer coords is the exact same geometry.
        if (in.drawPads) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(padColor);
            for (const auto& pad : comp.pads) {
                const QRectF r(mapX(pad.position.x - pad.sizeX / 2.0),
                               mapY(pad.position.y - pad.sizeY / 2.0),
                               pad.sizeX * s, pad.sizeY * s);
                if (pad.shape == Pad::Shape::Circle || pad.shape == Pad::Shape::Oval)
                    painter.drawEllipse(r);
                else
                    painter.drawRect(r);
            }
        }

        // ── Silkscreen / drawings ──
        if (in.drawSilk) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(silkColor, silkWidth));
            for (const auto& seg : comp.drawings) {
                if (seg.type == DrawingSegment::Type::Line) {
                    painter.drawLine(QPointF(mapX(seg.start.x), mapY(seg.start.y)),
                                     QPointF(mapX(seg.end.x),   mapY(seg.end.y)));
                } else if (seg.type == DrawingSegment::Type::Rect) {
                    const QRectF r(QPointF(mapX(std::min(seg.start.x, seg.end.x)),
                                           mapY(std::min(seg.start.y, seg.end.y))),
                                   QPointF(mapX(std::max(seg.start.x, seg.end.x)),
                                           mapY(std::max(seg.start.y, seg.end.y))));
                    painter.drawRect(r);
                } else if (seg.type == DrawingSegment::Type::Circle) {
                    painter.drawEllipse(QPointF(mapX(seg.start.x), mapY(seg.start.y)),
                                        seg.radius * s, seg.radius * s);
                } else if (seg.type == DrawingSegment::Type::Polygon && !seg.points.empty()) {
                    QPolygonF polyPts;
                    polyPts.reserve(static_cast<int>(seg.points.size()));
                    for (const auto& pt : seg.points)
                        polyPts << QPointF(mapX(pt.x), mapY(pt.y));
                    painter.drawPolygon(polyPts);
                }
            }
        }

        // ── Reference label ──
        if (in.drawSilk || isSelected) {
            const double cx = (comp.bbox.minX + comp.bbox.maxX) / 2.0;
            const double cy = (comp.bbox.minY + comp.bbox.maxY) / 2.0;
            painter.setPen(labelColor);
            painter.setFont(isSelected ? selectedLabelFont : normalLabelFont);
            painter.drawText(QPointF(mapX(cx), mapY(cy) - 0.15 * s),
                             QString::fromStdString(comp.reference));
        }
    }
    painter.end();

    BoardOverlay out;
    out.image = img;
    // p_buf = (p − o)·s — row-vector convention: translate first, then scale.
    out.pcbToBuffer = QTransform::fromTranslate(-ox, -oy)
                      * QTransform::fromScale(s, s);
    return out;
}

} // namespace ibom::overlay
