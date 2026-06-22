#include "OverlayRenderer.h"

#include "ibom/IBomData.h"

#include <QPainter>
#include <QFont>
#include <QPen>
#include <QPolygonF>
#include <QString>
#include <opencv2/core.hpp>
#include <algorithm>
#include <cmath>

namespace ibom::overlay {

QImage OverlayRenderer::render(const OverlayInputs& in)
{
    if (!in.project || in.size.isEmpty()) return {};

    QImage overlay(in.size, QImage::Format_ARGB32_Premultiplied);
    overlay.fill(Qt::transparent);
    QPainter painter(&overlay);
    // Shape antialiasing OFF: on a board that fills the frame this draws
    // thousands of pad/silk polygons every frame, and AA software fill is
    // several times slower — it was the GUI-thread bottleneck (overlay render
    // taking ~1s). Keep text AA so labels stay legible.
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QFont selectedLabelFont("Segoe UI", 9, QFont::Bold);
    const QFont normalLabelFont("Segoe UI", 7, QFont::Normal);
    auto withAlpha = [](QColor c, int a) { c.setAlpha(a); return c; };

    const Homography& H = in.homo;
    const float fw = static_cast<float>(in.size.width());
    const float fh = static_cast<float>(in.size.height());

    for (const auto& comp : in.project->components) {
        if (comp.layer != Layer::Front) continue;

        // Frustum cull: skip components whose projected bbox is off-frame.
        {
            auto bb = H.transformRect(
                static_cast<float>(comp.bbox.minX),
                static_cast<float>(comp.bbox.minY),
                static_cast<float>(comp.bbox.maxX - comp.bbox.minX),
                static_cast<float>(comp.bbox.maxY - comp.bbox.minY));
            if (bb.size() == 4) {
                float minx = bb[0].x, maxx = bb[0].x;
                float miny = bb[0].y, maxy = bb[0].y;
                for (const auto& p : bb) {
                    minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
                    miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
                }
                if (maxx < 0.f || minx > fw || maxy < 0.f || miny > fh)
                    continue;
            }
        }

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

        // ── Draw pads ──
        if (in.drawPads) {
            for (const auto& pad : comp.pads) {
                auto padCorners = H.transformRect(
                    static_cast<float>(pad.position.x - pad.sizeX / 2.0),
                    static_cast<float>(pad.position.y - pad.sizeY / 2.0),
                    static_cast<float>(pad.sizeX),
                    static_cast<float>(pad.sizeY));
                if (padCorners.size() == 4) {
                    QPolygonF padPoly;
                    for (auto& c : padCorners) padPoly << QPointF(c.x, c.y);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(padColor);
                    if (pad.shape == Pad::Shape::Circle || pad.shape == Pad::Shape::Oval)
                        painter.drawEllipse(padPoly.boundingRect());
                    else
                        painter.drawPolygon(padPoly);
                }
            }
        }

        // ── Draw silkscreen / drawings ──
        if (in.drawSilk) {
            painter.setBrush(Qt::NoBrush);
            for (const auto& seg : comp.drawings) {
                if (seg.type == DrawingSegment::Type::Line) {
                    cv::Point2f s = H.pcbToImage(cv::Point2f(
                        static_cast<float>(seg.start.x), static_cast<float>(seg.start.y)));
                    cv::Point2f e = H.pcbToImage(cv::Point2f(
                        static_cast<float>(seg.end.x), static_cast<float>(seg.end.y)));
                    painter.setPen(QPen(silkColor, silkWidth));
                    painter.drawLine(QPointF(s.x, s.y), QPointF(e.x, e.y));
                } else if (seg.type == DrawingSegment::Type::Rect) {
                    auto rc = H.transformRect(
                        static_cast<float>(std::min(seg.start.x, seg.end.x)),
                        static_cast<float>(std::min(seg.start.y, seg.end.y)),
                        static_cast<float>(std::abs(seg.end.x - seg.start.x)),
                        static_cast<float>(std::abs(seg.end.y - seg.start.y)));
                    if (rc.size() == 4) {
                        QPolygonF rp;
                        for (auto& c : rc) rp << QPointF(c.x, c.y);
                        painter.setPen(QPen(silkColor, silkWidth));
                        painter.drawPolygon(rp);
                    }
                } else if (seg.type == DrawingSegment::Type::Circle) {
                    cv::Point2f c = H.pcbToImage(cv::Point2f(
                        static_cast<float>(seg.start.x), static_cast<float>(seg.start.y)));
                    cv::Point2f edge = H.pcbToImage(cv::Point2f(
                        static_cast<float>(seg.start.x + seg.radius),
                        static_cast<float>(seg.start.y)));
                    float r = std::hypot(edge.x - c.x, edge.y - c.y);
                    painter.setPen(QPen(silkColor, silkWidth));
                    painter.drawEllipse(QPointF(c.x, c.y), static_cast<qreal>(r), static_cast<qreal>(r));
                } else if (seg.type == DrawingSegment::Type::Polygon && !seg.points.empty()) {
                    QPolygonF polyPts;
                    for (const auto& pt : seg.points) {
                        cv::Point2f ip = H.pcbToImage(cv::Point2f(
                            static_cast<float>(pt.x), static_cast<float>(pt.y)));
                        polyPts << QPointF(ip.x, ip.y);
                    }
                    painter.setPen(QPen(silkColor, silkWidth));
                    painter.drawPolygon(polyPts);
                }
            }
        }

        // ── Draw reference label ──
        if (in.drawSilk || isSelected) {
            cv::Point2f bboxCenter(
                static_cast<float>((comp.bbox.minX + comp.bbox.maxX) / 2.0),
                static_cast<float>((comp.bbox.minY + comp.bbox.maxY) / 2.0));
            cv::Point2f imgPt = H.pcbToImage(bboxCenter);
            painter.setPen(labelColor);
            painter.setFont(isSelected ? selectedLabelFont : normalLabelFont);
            painter.drawText(QPointF(imgPt.x, imgPt.y - 3),
                             QString::fromStdString(comp.reference));
        }
    }
    painter.end();
    return overlay;
}

} // namespace ibom::overlay
