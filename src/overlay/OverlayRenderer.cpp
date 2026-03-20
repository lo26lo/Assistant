#include "OverlayRenderer.h"

#include "gui/Theme.h"
#include <QPen>
#include <QBrush>
#include <QFont>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>

namespace ibom::overlay {

OverlayRenderer::OverlayRenderer(QObject* parent)
    : QObject(parent)
{
}

OverlayRenderer::~OverlayRenderer() = default;

void OverlayRenderer::setHomography(const Homography& homography)
{
    m_homography = homography;
}

void OverlayRenderer::setIBomData(const IBomProject& project)
{
    m_project = project;
}

QImage OverlayRenderer::render(const cv::Mat& frame, const std::string& selectedRef)
{
    if (frame.empty()) return {};

    // Convert frame to QImage
    QImage image = matToQImage(frame);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Draw board outline
    if (m_homography.isValid()) {
        drawBoardOutline(painter);
    }

    // Draw each component
    for (const auto& comp : m_project.components) {
        if (comp.layer != m_activeLayer) continue;

        bool isHighlighted = (comp.reference == selectedRef) ||
            std::find(m_highlightedRefs.begin(), m_highlightedRefs.end(),
                      comp.reference) != m_highlightedRefs.end();

        // Set opacity based on highlight
        painter.setOpacity(isHighlighted ? 1.0f : m_opacity);

        if (m_showOutlines) drawComponentOutline(painter, comp);
        if (m_showPads)     drawComponentPads(painter, comp);
        if (m_showPin1)     drawPin1Marker(painter, comp);
        if (m_showLabels || isHighlighted) drawComponentLabel(painter, comp);
    }

    painter.end();

    emit overlayUpdated();
    return image;
}

void OverlayRenderer::drawComponentOutline(QPainter& painter, const Component& comp)
{
    QColor color = stateColor(comp.reference);
    QPen pen(color, 2);
    painter.setPen(pen);

    QColor fillColor = color;
    fillColor.setAlphaF(0.15f);
    painter.setBrush(QBrush(fillColor));

    if (m_homography.isValid()) {
        // Transform bounding box corners through homography
        auto corners = m_homography.transformRect(
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
    } else {
        // Fallback: draw in PCB coordinates directly
        painter.drawRect(QRectF(
            comp.bbox.minX, comp.bbox.minY,
            comp.bbox.width(), comp.bbox.height()
        ));
    }
}

void OverlayRenderer::drawComponentLabel(QPainter& painter, const Component& comp)
{
    QFont font("Monospace", 8);
    font.setBold(true);
    painter.setFont(font);

    QColor color = stateColor(comp.reference);
    painter.setPen(color);

    cv::Point2f imgPos;
    if (m_homography.isValid()) {
        imgPos = m_homography.pcbToImage(
            cv::Point2f(static_cast<float>(comp.position.x),
                        static_cast<float>(comp.position.y)));
    } else {
        imgPos = cv::Point2f(static_cast<float>(comp.position.x),
                              static_cast<float>(comp.position.y));
    }

    QString label = QString::fromStdString(comp.reference);
    if (!comp.value.empty()) {
        label += " (" + QString::fromStdString(comp.value) + ")";
    }

    // Draw text with background for readability
    QFontMetrics fm(font);
    QRect textRect = fm.boundingRect(label);
    QPointF textPos(imgPos.x - textRect.width() / 2.0, imgPos.y - 5.0);

    // Background
    painter.setOpacity(0.8);
    painter.fillRect(QRectF(textPos.x() - 2, textPos.y() - textRect.height(),
                             textRect.width() + 4, textRect.height() + 4),
                     QColor(0, 0, 0, 180));

    // Text
    painter.setOpacity(1.0);
    painter.drawText(textPos, label);
}

void OverlayRenderer::drawComponentPads(QPainter& painter, const Component& comp)
{
    for (const auto& pad : comp.pads) {
        cv::Point2f imgPos;
        if (m_homography.isValid()) {
            imgPos = m_homography.pcbToImage(
                cv::Point2f(static_cast<float>(pad.position.x),
                            static_cast<float>(pad.position.y)));
        } else {
            imgPos = cv::Point2f(static_cast<float>(pad.position.x),
                                  static_cast<float>(pad.position.y));
        }

        QColor padColor = pad.isPin1 ? ibom::gui::theme::padPin1Color() : ibom::gui::theme::padRegularColor();
        painter.setPen(QPen(padColor, 1));
        painter.setBrush(QBrush(padColor));

        float padRadius = 3.0f;
        painter.drawEllipse(QPointF(imgPos.x, imgPos.y), padRadius, padRadius);
    }
}

void OverlayRenderer::drawPin1Marker(QPainter& painter, const Component& comp)
{
    for (const auto& pad : comp.pads) {
        if (!pad.isPin1) continue;

        cv::Point2f imgPos;
        if (m_homography.isValid()) {
            imgPos = m_homography.pcbToImage(
                cv::Point2f(static_cast<float>(pad.position.x),
                            static_cast<float>(pad.position.y)));
        } else {
            imgPos = cv::Point2f(static_cast<float>(pad.position.x),
                                  static_cast<float>(pad.position.y));
        }

        // Draw a distinct pin 1 marker (red filled circle)
        QColor pin1Fill = ibom::gui::theme::padPin1Color();
        pin1Fill.setAlpha(150);
        painter.setPen(QPen(ibom::gui::theme::padPin1Color(), 2));
        painter.setBrush(QBrush(pin1Fill));
        painter.drawEllipse(QPointF(imgPos.x, imgPos.y), 5.0, 5.0);
        break;
    }
}

void OverlayRenderer::drawBoardOutline(QPainter& painter)
{
    if (m_project.boardOutline.empty()) return;

    QPen pen(ibom::gui::theme::boardOutlineColor(), 2);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    for (const auto& seg : m_project.boardOutline) {
        if (seg.type == DrawingSegment::Type::Line) {
            auto p1 = m_homography.pcbToImage(
                cv::Point2f(static_cast<float>(seg.start.x),
                            static_cast<float>(seg.start.y)));
            auto p2 = m_homography.pcbToImage(
                cv::Point2f(static_cast<float>(seg.end.x),
                            static_cast<float>(seg.end.y)));
            painter.drawLine(QPointF(p1.x, p1.y), QPointF(p2.x, p2.y));
        }
        // TODO: Handle arcs, circles
    }
}

QColor OverlayRenderer::stateColor(const std::string& ref) const
{
    auto it = m_componentStates.find(ref);
    if (it == m_componentStates.end()) return ibom::gui::theme::defaultComponentColor();

    const auto& state = it->second;
    if (state == "placed")             return ibom::gui::theme::placedColor();
    if (state == "missing")            return ibom::gui::theme::missingColor();
    if (state == "wrong_orientation")  return ibom::gui::theme::defectColor();
    if (state == "inspected")          return ibom::gui::theme::inspectedColor();

    return ibom::gui::theme::defaultComponentColor();
}

void OverlayRenderer::setHighlightedRefs(const std::vector<std::string>& refs)
{
    m_highlightedRefs = refs;
}

void OverlayRenderer::setComponentState(const std::string& ref, const std::string& state)
{
    m_componentStates[ref] = state;
}

QImage OverlayRenderer::matToQImage(const cv::Mat& mat)
{
    if (mat.empty()) return {};

    cv::Mat rgb;
    if (mat.channels() == 3) {
        cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
    } else if (mat.channels() == 1) {
        cv::cvtColor(mat, rgb, cv::COLOR_GRAY2RGB);
    } else {
        rgb = mat;
    }

    return QImage(rgb.data, rgb.cols, rgb.rows,
                  static_cast<int>(rgb.step), QImage::Format_RGB888).copy();
}

} // namespace ibom::overlay
