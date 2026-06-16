#include "BoardMinimap.h"
#include "gui/Theme.h"
#include "overlay/Homography.h"

#include <QPainter>
#include <QMouseEvent>
#include <QColor>
#include <algorithm>
#include <cmath>

namespace ibom::gui {

BoardMinimap::BoardMinimap(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(false);
    setCursor(Qt::CrossCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void BoardMinimap::setIBomData(const ibom::IBomProject& project)
{
    m_project = project;
    m_cacheValid = false;
    update();
}

void BoardMinimap::setHomography(const ibom::overlay::Homography* hom, QSize cameraSize)
{
    m_homography = hom;
    m_cameraSize = cameraSize;
    update();
}

void BoardMinimap::setActiveLayer(ibom::Layer layer)
{
    if (m_activeLayer == layer) return;
    m_activeLayer = layer;
    update();
}

void BoardMinimap::setSelectedRef(const std::string& ref)
{
    m_selectedRef = ref;
    update();
}

void BoardMinimap::setPlacedRefs(const std::unordered_set<std::string>& refs)
{
    m_placedRefs = refs;
    update();
}

// ---------------------------------------------------------------------------
// Coordinate helpers
// ---------------------------------------------------------------------------

void BoardMinimap::rebuildCache()
{
    // Use board bbox if available, fall back to component union bbox
    if (m_project.boardInfo.boardBBox.maxX > m_project.boardInfo.boardBBox.minX) {
        const auto& bb = m_project.boardInfo.boardBBox;
        m_pcbMinX = bb.minX; m_pcbMinY = bb.minY;
        m_pcbMaxX = bb.maxX; m_pcbMaxY = bb.maxY;
    } else {
        m_pcbMinX = 1e9; m_pcbMinY = 1e9;
        m_pcbMaxX = -1e9; m_pcbMaxY = -1e9;
        for (const auto& c : m_project.components) {
            m_pcbMinX = std::min(m_pcbMinX, c.bbox.minX);
            m_pcbMinY = std::min(m_pcbMinY, c.bbox.minY);
            m_pcbMaxX = std::max(m_pcbMaxX, c.bbox.maxX);
            m_pcbMaxY = std::max(m_pcbMaxY, c.bbox.maxY);
        }
        if (m_pcbMinX > m_pcbMaxX) { m_pcbMinX = 0; m_pcbMinY = 0; m_pcbMaxX = 100; m_pcbMaxY = 100; }
    }

    double pcbW = m_pcbMaxX - m_pcbMinX;
    double pcbH = m_pcbMaxY - m_pcbMinY;
    int ww = width()  - 2 * m_marginPx;
    int wh = height() - 2 * m_marginPx;
    if (ww < 1) ww = 1;
    if (wh < 1) wh = 1;

    // Uniform scale, keep aspect ratio
    double scale = std::min(ww / pcbW, wh / pcbH);
    m_scaleX = scale;
    m_scaleY = scale;
    m_cacheValid = true;
}

QPointF BoardMinimap::pcbToWidget(double x, double y) const
{
    int ww = width()  - 2 * m_marginPx;
    int wh = height() - 2 * m_marginPx;
    double pcbW = m_pcbMaxX - m_pcbMinX;
    double pcbH = m_pcbMaxY - m_pcbMinY;
    double renderedW = pcbW * m_scaleX;
    double renderedH = pcbH * m_scaleY;
    double offX = m_marginPx + (ww - renderedW) / 2.0;
    double offY = m_marginPx + (wh - renderedH) / 2.0;
    return { offX + (x - m_pcbMinX) * m_scaleX,
             offY + (y - m_pcbMinY) * m_scaleY };
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void BoardMinimap::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Background
    p.fillRect(rect(), QColor(25, 25, 35));

    if (m_project.components.empty()) {
        p.setPen(QColor(100, 100, 120));
        p.drawText(rect(), Qt::AlignCenter, tr("No iBOM loaded"));
        return;
    }

    if (!m_cacheValid) rebuildCache();

    // Board outline
    {
        QPointF tl = pcbToWidget(m_pcbMinX, m_pcbMinY);
        QPointF br = pcbToWidget(m_pcbMaxX, m_pcbMaxY);
        p.setPen(QPen(theme::boardOutlineColor(), 1));
        p.setBrush(QColor(35, 40, 50));
        p.drawRect(QRectF(tl, br));
    }

    // Draw components
    for (const auto& comp : m_project.components) {
        if (comp.layer != m_activeLayer) continue;

        bool placed   = m_placedRefs.count(comp.reference) > 0;
        bool selected = comp.reference == m_selectedRef;

        QColor col;
        if (selected)       col = QColor(255, 220, 50);
        else if (placed)    col = theme::placedColor().darker(130);
        else                col = theme::accentDark();

        QPointF tl = pcbToWidget(comp.bbox.minX, comp.bbox.minY);
        QPointF br = pcbToWidget(comp.bbox.maxX, comp.bbox.maxY);
        QRectF  r(tl, br);

        if (r.width() < 1.5) r.setWidth(1.5);
        if (r.height() < 1.5) r.setHeight(1.5);

        QColor fill = col;
        fill.setAlphaF(selected ? 0.55f : 0.25f);
        p.setPen(QPen(col, selected ? 1.2f : 0.8f));
        p.setBrush(fill);
        p.drawRect(r);
    }

    // FOV rectangle (only when homography is valid and camera size known)
    if (m_homography && m_homography->isValid() && m_cameraSize.isValid()) {
        // Four image corners → PCB coords
        float iw = static_cast<float>(m_cameraSize.width());
        float ih = static_cast<float>(m_cameraSize.height());
        std::vector<cv::Point2f> imgCorners = {
            {0.f, 0.f}, {iw, 0.f}, {iw, ih}, {0.f, ih}
        };

        QPolygonF poly;
        bool ok = true;
        for (auto& ic : imgCorners) {
            cv::Point2f pc = m_homography->imageToPcb(ic);
            // Sanity-check: reject wildly out-of-range points
            if (std::abs(pc.x) > 1e6 || std::abs(pc.y) > 1e6) { ok = false; break; }
            poly << pcbToWidget(pc.x, pc.y);
        }

        if (ok) {
            QPen fovPen(QColor(0, 220, 220, 200), 1.5, Qt::DashLine);
            p.setPen(fovPen);
            p.setBrush(QColor(0, 200, 200, 25));
            p.drawPolygon(poly);
        }
    }

    // Border
    p.setPen(QPen(QColor(80, 90, 110), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    // Label
    p.setPen(QColor(140, 150, 170));
    p.setFont(QFont("monospace", 7));
    p.drawText(rect().adjusted(3, 2, -3, -2), Qt::AlignTop | Qt::AlignRight, tr("PCB Map"));
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

void BoardMinimap::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) return;
    if (!m_cacheValid) return;

    // Widget pos → PCB coords
    QPointF wp = ev->pos();
    int ww = width()  - 2 * m_marginPx;
    int wh = height() - 2 * m_marginPx;
    double pcbW = m_pcbMaxX - m_pcbMinX;
    double pcbH = m_pcbMaxY - m_pcbMinY;
    double renderedW = pcbW * m_scaleX;
    double renderedH = pcbH * m_scaleY;
    double offX = m_marginPx + (ww - renderedW) / 2.0;
    double offY = m_marginPx + (wh - renderedH) / 2.0;

    double px = (wp.x() - offX) / m_scaleX + m_pcbMinX;
    double py = (wp.y() - offY) / m_scaleY + m_pcbMinY;

    // Clamp to board area
    px = std::clamp(px, m_pcbMinX, m_pcbMaxX);
    py = std::clamp(py, m_pcbMinY, m_pcbMaxY);

    emit anchorRequested(cv::Point2f(static_cast<float>(px),
                                     static_cast<float>(py)));
}

void BoardMinimap::resizeEvent(QResizeEvent*)
{
    m_cacheValid = false;
}

} // namespace ibom::gui
