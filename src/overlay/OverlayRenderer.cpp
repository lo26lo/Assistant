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

    // Back side: draw in VIEW space (x mirrored across the board's vertical
    // mid-axis) so text/labels are drawn readable; pcbToBuffer (below) carries
    // the same mirror, so the buffer→image warp stays orientation-preserving.
    const bool back = (in.activeLayer == Layer::Back);
    const auto mapX = [&](double x) {
        return ((back ? (minX + maxX - x) : x) - ox) * s;
    };
    const auto mapY = [&](double y) { return (y - oy) * s; };
    // mapX reverses order for the back view — build rects from normalized
    // extremes instead of assuming x0 maps left of x1.
    const auto rectPcb = [&](double x0, double y0, double x1, double y1) {
        const double a = mapX(x0), b = mapX(x1);
        return QRectF(QPointF(std::min(a, b), mapY(std::min(y0, y1))),
                      QPointF(std::max(a, b), mapY(std::max(y0, y1))));
    };

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
        if (comp.layer != in.activeLayer) continue;

        const bool isSelected = (comp.reference == in.selectedRef);
        const bool isPlaced   = !isSelected && in.placedRefs.count(comp.reference) > 0;

        QColor padColor, silkColor, labelColor;
        qreal silkWidth = 1.0;
        // Revision-diff rework mark for this ref (C1 V2), if any.
        int diffMark = 0;
        if (!in.diffMarks.empty()) {
            const auto dm = in.diffMarks.find(comp.reference);
            if (dm != in.diffMarks.end()) diffMark = dm->second;
        }
        if (isSelected) {
            padColor   = withAlpha(in.cSelected, 220);
            silkColor  = withAlpha(in.cSelected, 240);
            labelColor = withAlpha(in.cSelected, 255);
            silkWidth  = in.selectedSilkW;
        } else if (diffMark != 0) {
            // Rework recolor: REMOVE = red (desolder), CHANGE = orange.
            const QColor dc = (diffMark == 1) ? QColor(240, 60, 60)
                                              : QColor(255, 165, 0);
            padColor   = withAlpha(dc, 210);
            silkColor  = withAlpha(dc, 230);
            labelColor = withAlpha(dc, 255);
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

        // ── Pads ── drawn with their own rotation (INVESTIGATION_360 §1.4 —
        // they used to be axis-aligned, wrong for parts placed at e.g. 45°):
        // local axis-aligned shape around the origin, painter rotated. Under
        // the back-view mirror a rotation flips sign (M∘R(θ) = R(−θ)∘M; the
        // local rect is mirror-symmetric). Angle convention matches iBOM's
        // render.js drawPad (canvas rotate(+angle), y-down — same as Qt).
        if (in.drawPads) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(padColor);
            for (const auto& pad : comp.pads) {
                const double hw = pad.sizeX * 0.5 * s;
                const double hh = pad.sizeY * 0.5 * s;
                const QRectF local(-hw, -hh, 2.0 * hw, 2.0 * hh);
                painter.save();
                painter.translate(mapX(pad.position.x), mapY(pad.position.y));
                if (pad.angle != 0.0)
                    painter.rotate(back ? -pad.angle : pad.angle);
                if (pad.shape == Pad::Shape::Circle || pad.shape == Pad::Shape::Oval)
                    painter.drawEllipse(local);
                else if (pad.shape == Pad::Shape::RoundRect)
                    painter.drawRoundedRect(local, std::min(hw, hh) * 0.5,
                                            std::min(hw, hh) * 0.5);
                else
                    painter.drawRect(local);
                painter.restore();
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
                    painter.drawRect(rectPcb(seg.start.x, seg.start.y,
                                             seg.end.x, seg.end.y));
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

        // ── Rework X (REMOVE) ── across the body so "to desolder" reads at a
        // glance even with pads/silk toggled off.
        if (diffMark == 1) {
            painter.setPen(QPen(withAlpha(QColor(240, 60, 60), 230),
                                std::max(1.0, 0.12 * s)));
            painter.drawLine(QPointF(mapX(comp.bbox.minX), mapY(comp.bbox.minY)),
                             QPointF(mapX(comp.bbox.maxX), mapY(comp.bbox.maxY)));
            painter.drawLine(QPointF(mapX(comp.bbox.minX), mapY(comp.bbox.maxY)),
                             QPointF(mapX(comp.bbox.maxX), mapY(comp.bbox.minY)));
            painter.setBrush(Qt::NoBrush);
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

    // ── Components to ADD (C1 V2) ── exist only in the target revision:
    // green ring + cross + ref at the position where they must be placed.
    if (!in.diffAdds.empty()) {
        const QColor add(80, 220, 90);
        painter.setFont(normalLabelFont);
        for (const auto& [pos, ref] : in.diffAdds) {
            const QPointF c(mapX(pos.x), mapY(pos.y));
            const double r = 1.2 * s;   // ~1.2 mm ring
            painter.setPen(QPen(withAlpha(add, 240), std::max(1.0, 0.12 * s)));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(c, r, r);
            painter.drawLine(c + QPointF(-r, 0), c + QPointF(r, 0));
            painter.drawLine(c + QPointF(0, -r), c + QPointF(0, r));
            painter.setPen(withAlpha(add, 255));
            painter.drawText(c + QPointF(r + 0.2 * s, 0),
                             QString::fromStdString(ref));
        }
    }
    painter.end();

    BoardOverlay out;
    out.image = img;
    // p_buf = (view(p) − o)·s — row-vector convention: (mirror,) translate,
    // scale. For the back view the mirror lives HERE, matching mapX above, so
    // buffer content stays readable and the composed buffer→image transform
    // (this inverse × a mirrored raw-PCB→image homography) is orientation-
    // preserving on screen.
    const QTransform view = back
        ? QTransform(-1, 0, 0, 1, minX + maxX, 0)   // x' = (minX+maxX) − x
        : QTransform();
    out.pcbToBuffer = view
                      * QTransform::fromTranslate(-ox, -oy)
                      * QTransform::fromScale(s, s);
    return out;
}

} // namespace ibom::overlay
