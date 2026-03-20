#include "ComponentOverlay.h"
#include "gui/Theme.h"

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
    if (m_state == "placed")            return ibom::gui::theme::placedColor();
    if (m_state == "missing")           return ibom::gui::theme::missingColor();
    if (m_state == "wrong_orientation") return ibom::gui::theme::defectColor();
    if (m_state == "inspected")         return ibom::gui::theme::inspectedColor();
    return ibom::gui::theme::defaultComponentColor();
}

} // namespace ibom::overlay
