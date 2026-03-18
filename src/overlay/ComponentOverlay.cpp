#include "ComponentOverlay.h"

namespace ibom::overlay {

void ComponentOverlay::draw(QPainter& painter, const Component& comp,
                             const Homography& homography, bool highlighted)
{
    QColor c = color();
    int penWidth = highlighted ? 3 : 2;
    painter.setPen(QPen(c, penWidth));

    QColor fill = c;
    fill.setAlphaF(highlighted ? 0.3f : 0.1f);
    painter.setBrush(QBrush(fill));

    if (homography.isValid()) {
        auto corners = homography.transformRect(
            static_cast<float>(comp.bbox.minX),
            static_cast<float>(comp.bbox.minY),
            static_cast<float>(comp.bbox.width()),
            static_cast<float>(comp.bbox.height())
        );

        QPolygonF polygon;
        for (const auto& pt : corners) {
            polygon << QPointF(pt.x, pt.y);
        }
        painter.drawPolygon(polygon);
    }
}

QColor ComponentOverlay::color() const
{
    if (m_state == "placed")            return QColor(0, 220, 0);
    if (m_state == "missing")           return QColor(255, 0, 0);
    if (m_state == "wrong_orientation") return QColor(255, 165, 0);
    if (m_state == "inspected")         return QColor(0, 200, 200);
    return QColor(0, 180, 255); // Default: light blue
}

} // namespace ibom::overlay
