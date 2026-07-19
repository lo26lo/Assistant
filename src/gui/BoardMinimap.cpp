#include "BoardMinimap.h"
#include "gui/Theme.h"
#include "overlay/Homography.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QToolButton>
#include <QColor>
#include <algorithm>
#include <cmath>

namespace ibom::gui {

namespace {
// LOD thresholds, in widget px per PCB unit (mm).
constexpr double kPointModeBelow = 1.0;   // rects would collapse to ≤1 px
constexpr double kPadsAbove      = 5.0;
constexpr double kRefsAbove      = 6.0;
constexpr int    kPadsMaxComps   = 800;   // dense-view caps keep the cache
constexpr int    kRefsMaxComps   = 400;   // rebuild bounded on huge boards
constexpr double kMaxZoom        = 100.0;
constexpr int    kCoverageMaxDim = 512;   // coverage raster resolution cap
constexpr int    kDragThresholdPx = 6;
} // namespace

BoardMinimap::BoardMinimap(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(false);
    setCursor(Qt::CrossCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto mkBtn = [this](const QString& text, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setCursor(Qt::ArrowCursor);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedSize(20, 20);
        b->setStyleSheet(
            "QToolButton{color:#c8d0e0;background:rgba(30,34,46,170);"
            "border:1px solid rgba(120,130,160,90);border-radius:3px;"
            "font-size:11px;padding:0;}"
            "QToolButton:hover{background:rgba(60,70,100,210);}"
            "QToolButton:checked{background:rgba(100,136,232,170);color:#ffffff;}");
        return b;
    };
    m_btnFit = mkBtn(QStringLiteral("⌂"), tr("Fit board (reset zoom)"));
    connect(m_btnFit, &QToolButton::clicked, this, &BoardMinimap::resetView);

    m_btnFollow = mkBtn(QStringLiteral("◎"), tr("Follow the camera FOV"));
    m_btnFollow->setCheckable(true);
    connect(m_btnFollow, &QToolButton::toggled,
            this, [this](bool on) { setFollowFov(on); });

    m_btnExpand = mkBtn(QStringLiteral("⛶"), tr("Open large map (M)"));
    connect(m_btnExpand, &QToolButton::clicked,
            this, [this]() { emit expandRequested(); });

    layoutButtons();
}

// ---------------------------------------------------------------------------
// Data setters (mirrored to the peer instance)
// ---------------------------------------------------------------------------

void BoardMinimap::setIBomData(const ibom::IBomProject& project)
{
    m_project = project;
    m_boundsValid = false;
    m_selectedRef.clear();
    m_fovValid = false;
    rebuildBounds();
    resetView();
    if (m_peer) m_peer->setIBomData(project);
}

void BoardMinimap::setHomography(const ibom::overlay::Homography* hom, QSize cameraSize)
{
    m_homography = hom;
    m_cameraSize = cameraSize;
    update();
    if (m_peer) m_peer->setHomography(hom, cameraSize);
}

void BoardMinimap::setActiveLayer(ibom::Layer layer)
{
    if (m_activeLayer != layer) {
        m_activeLayer = layer;
        markStaticDirty();
        update();
    }
    if (m_peer) m_peer->setActiveLayer(layer);
}

void BoardMinimap::setSelectedRef(const std::string& ref)
{
    m_selectedRef = ref;

    // When zoomed in (and not tied to the FOV), bring the selection into view —
    // on a large board the selected part is usually outside the current crop.
    if (!ref.empty() && m_zoom > 1.05 && !m_followFov) {
        for (const auto& c : m_project.components) {
            if (c.reference != ref) continue;
            m_viewCenterX = c.bbox.center().x;
            m_viewCenterY = c.bbox.center().y;
            clampViewCenter();
            markStaticDirty();
            break;
        }
    }
    update();
    if (m_peer) m_peer->setSelectedRef(ref);
}

void BoardMinimap::setPlacedRefs(const std::unordered_set<std::string>& refs)
{
    m_placedRefs = refs;
    markStaticDirty();
    update();
    if (m_peer) m_peer->setPlacedRefs(refs);
}

void BoardMinimap::setAnnotatedRefs(const std::unordered_set<std::string>& refs)
{
    m_annotatedRefs = refs;
    update();
    if (m_peer) m_peer->setAnnotatedRefs(refs);
}

void BoardMinimap::setBoardImage(const QImage& img, const QRectF& pcbRect,
                                 ibom::Layer layer)
{
    m_boardImage      = img;
    m_boardImageRect  = pcbRect;
    m_boardImageLayer = layer;
    markStaticDirty();
    update();
    if (m_peer) m_peer->setBoardImage(img, pcbRect, layer);
}

void BoardMinimap::setBoardImageVisible(bool on)
{
    if (m_boardImageVisible != on) {
        m_boardImageVisible = on;
        markStaticDirty();
        update();
    }
    if (m_peer) m_peer->setBoardImageVisible(on);
}

void BoardMinimap::setClickTargets(const std::vector<cv::Point2f>& pcbPts,
                                   const QColor& color)
{
    m_clickTargets = pcbPts;
    m_clickTargetColor = color;
    update();
    if (m_peer) m_peer->setClickTargets(pcbPts, color);
}

void BoardMinimap::setTrackingQuality(int inliers, double reprojErrPx)
{
    const QColor before = fovColor();
    m_inliers = inliers;
    m_reprojErr = reprojErrPx;
    if (fovColor() != before) update();
    if (m_peer) m_peer->setTrackingQuality(inliers, reprojErrPx);
}

void BoardMinimap::setTrackingLost(bool lost)
{
    if (m_trackingLost != lost) {
        m_trackingLost = lost;
        update();
    }
    if (m_peer) m_peer->setTrackingLost(lost);
}

void BoardMinimap::attachPeer(BoardMinimap* peer)
{
    m_peer = peer;
    if (!peer) return;
    // Push the current state so the large view opens in sync.
    peer->setIBomData(m_project);
    peer->setHomography(m_homography, m_cameraSize);
    peer->setActiveLayer(m_activeLayer);
    peer->setPlacedRefs(m_placedRefs);
    peer->setSelectedRef(m_selectedRef);
    peer->setClickTargets(m_clickTargets, m_clickTargetColor);
    peer->setAnnotatedRefs(m_annotatedRefs);
    peer->setBoardImage(m_boardImage, m_boardImageRect, m_boardImageLayer);
    peer->setBoardImageVisible(m_boardImageVisible);
    peer->setTrackingQuality(m_inliers, m_reprojErr);
    peer->setTrackingLost(m_trackingLost);
    peer->m_coverage = m_coverage;          // share the trail accumulated so far
    peer->m_coverageScale = m_coverageScale;
    peer->m_coverageVisible = m_coverageVisible;
}

// ---------------------------------------------------------------------------
// Coverage
// ---------------------------------------------------------------------------

void BoardMinimap::accumulateCoverage()
{
    if (m_peer) m_peer->accumulateCoverage();
    if (m_coverage.isNull()) return;
    if (m_coverageTimer.isValid() && m_coverageTimer.elapsed() < 150) return;

    std::vector<cv::Point2f> poly;
    if (!computeFovPcb(poly)) return;

    QPolygonF covPoly;
    for (const auto& pt : poly)
        covPoly << QPointF((pt.x - m_pcbMinX) * m_coverageScale,
                           (pt.y - m_pcbMinY) * m_coverageScale);
    QPainter cp(&m_coverage);
    cp.setPen(Qt::NoPen);
    cp.setBrush(theme::placedColor());
    cp.drawPolygon(covPoly);
    m_coverageTimer.restart();
    // No update() here: the FOV repaint that follows every homography change
    // already refreshes the widget.
}

void BoardMinimap::setCoverageVisible(bool on)
{
    if (m_coverageVisible != on) {
        m_coverageVisible = on;
        update();
    }
    if (m_peer) m_peer->setCoverageVisible(on);
}

void BoardMinimap::resetCoverage()
{
    if (!m_coverage.isNull()) m_coverage.fill(Qt::transparent);
    update();
    if (m_peer) m_peer->resetCoverage();
}

// ---------------------------------------------------------------------------
// View control
// ---------------------------------------------------------------------------

void BoardMinimap::resetView()
{
    m_zoom = 1.0;
    m_viewCenterX = (m_pcbMinX + m_pcbMaxX) / 2.0;
    m_viewCenterY = (m_pcbMinY + m_pcbMaxY) / 2.0;
    if (m_followFov) {
        m_followFov = false;
        if (m_btnFollow) {
            QSignalBlocker block(m_btnFollow);
            m_btnFollow->setChecked(false);
        }
    }
    markStaticDirty();
    update();
}

void BoardMinimap::setFollowFov(bool on)
{
    m_followFov = on;
    if (m_btnFollow && m_btnFollow->isChecked() != on) {
        QSignalBlocker block(m_btnFollow);
        m_btnFollow->setChecked(on);
    }
    if (on) {
        // Start at ≈3× the FOV extent; the wheel adjusts from there.
        std::vector<cv::Point2f> poly;
        if (computeFovPcb(poly)) {
            float minX = poly[0].x, maxX = poly[0].x;
            float minY = poly[0].y, maxY = poly[0].y;
            for (const auto& pt : poly) {
                minX = std::min(minX, pt.x); maxX = std::max(maxX, pt.x);
                minY = std::min(minY, pt.y); maxY = std::max(maxY, pt.y);
            }
            const double extent = std::max({double(maxX - minX), double(maxY - minY), 1e-3});
            const int ww = std::max(width()  - 2 * m_marginPx, 1);
            const int wh = std::max(height() - 2 * m_marginPx, 1);
            const double desired = std::min(ww, wh) / (3.0 * extent);
            m_zoom = std::clamp(desired / fitScale(), 1.0, kMaxZoom);
            m_viewCenterX = (minX + maxX) / 2.0;
            m_viewCenterY = (minY + maxY) / 2.0;
            clampViewCenter();
        }
    }
    markStaticDirty();
    update();
}

void BoardMinimap::setExpandedMode(bool on)
{
    m_expandedMode = on;
    if (m_btnExpand) m_btnExpand->setVisible(!on);
    layoutButtons();
}

double BoardMinimap::boardAspectRatio() const
{
    if (!m_boundsValid) return 0.0;
    const double w = m_pcbMaxX - m_pcbMinX;
    const double h = m_pcbMaxY - m_pcbMinY;
    return (w > 1e-6 && h > 1e-6) ? w / h : 0.0;
}

// ---------------------------------------------------------------------------
// Coordinate helpers
// ---------------------------------------------------------------------------

void BoardMinimap::rebuildBounds()
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
    m_boundsValid = true;

    // (Re)allocate the coverage raster to the board's shape.
    const double pcbW = std::max(m_pcbMaxX - m_pcbMinX, 1e-6);
    const double pcbH = std::max(m_pcbMaxY - m_pcbMinY, 1e-6);
    m_coverageScale = kCoverageMaxDim / std::max(pcbW, pcbH);
    const int cw = std::max(1, int(std::ceil(pcbW * m_coverageScale)));
    const int ch = std::max(1, int(std::ceil(pcbH * m_coverageScale)));
    m_coverage = QImage(cw, ch, QImage::Format_ARGB32_Premultiplied);
    m_coverage.fill(Qt::transparent);
    m_coverageTimer.invalidate();
}

double BoardMinimap::fitScale() const
{
    const double pcbW = std::max(m_pcbMaxX - m_pcbMinX, 1e-6);
    const double pcbH = std::max(m_pcbMaxY - m_pcbMinY, 1e-6);
    const int ww = std::max(width()  - 2 * m_marginPx, 1);
    const int wh = std::max(height() - 2 * m_marginPx, 1);
    return std::min(ww / pcbW, wh / pcbH);
}

QPointF BoardMinimap::pcbToWidget(double x, double y) const
{
    const double s = viewScale();
    return { width()  / 2.0 + (x - m_viewCenterX) * s,
             height() / 2.0 + (y - m_viewCenterY) * s };
}

cv::Point2d BoardMinimap::widgetToPcb(const QPointF& wp) const
{
    const double s = viewScale();
    return { (wp.x() - width()  / 2.0) / s + m_viewCenterX,
             (wp.y() - height() / 2.0) / s + m_viewCenterY };
}

void BoardMinimap::clampViewCenter()
{
    const double s = viewScale();
    const double halfW = (width()  / 2.0) / s;
    const double halfH = (height() / 2.0) / s;
    const double slack = m_marginPx / s;

    auto clampAxis = [](double c, double lo, double hi) {
        return (lo > hi) ? (lo + hi) / 2.0 : std::clamp(c, lo, hi);
    };
    m_viewCenterX = clampAxis(m_viewCenterX,
                              m_pcbMinX + halfW - slack, m_pcbMaxX - halfW + slack);
    m_viewCenterY = clampAxis(m_viewCenterY,
                              m_pcbMinY + halfH - slack, m_pcbMaxY - halfH + slack);
}

bool BoardMinimap::computeFovPcb(std::vector<cv::Point2f>& out) const
{
    if (!m_homography || !m_homography->isValid() || !m_cameraSize.isValid())
        return false;
    const float iw = static_cast<float>(m_cameraSize.width());
    const float ih = static_cast<float>(m_cameraSize.height());
    const cv::Point2f imgCorners[4] = { {0.f, 0.f}, {iw, 0.f}, {iw, ih}, {0.f, ih} };
    out.clear();
    out.reserve(4);
    for (const auto& ic : imgCorners) {
        cv::Point2f pc = m_homography->imageToPcb(ic);
        // Sanity-check: reject wildly out-of-range points
        if (std::abs(pc.x) > 1e6 || std::abs(pc.y) > 1e6) return false;
        out.push_back(pc);
    }
    return true;
}

QColor BoardMinimap::fovColor() const
{
    if (m_trackingLost) return QColor(255, 70, 70);
    if (m_inliers >= 0 && (m_inliers < 15 || m_reprojErr > 6.0))
        return QColor(255, 165, 0);
    return QColor(0, 220, 220);
}

const ibom::Component* BoardMinimap::componentAt(const cv::Point2d& pcb) const
{
    // Smallest bbox containing the point wins (nested parts, dense areas);
    // otherwise the nearest centre within a small pick radius.
    const ibom::Component* best = nullptr;
    double bestArea = 1e18;
    for (const auto& c : m_project.components) {
        if (c.layer != m_activeLayer) continue;
        if (pcb.x < c.bbox.minX || pcb.x > c.bbox.maxX ||
            pcb.y < c.bbox.minY || pcb.y > c.bbox.maxY) continue;
        const double area = std::max(c.bbox.width() * c.bbox.height(), 1e-9);
        if (area < bestArea) { bestArea = area; best = &c; }
    }
    if (best) return best;

    const double pickRadius = 10.0 / std::max(viewScale(), 1e-9);  // 10 widget px
    double bestDist = pickRadius * pickRadius;
    for (const auto& c : m_project.components) {
        if (c.layer != m_activeLayer) continue;
        const double dx = c.bbox.center().x - pcb.x;
        const double dy = c.bbox.center().y - pcb.y;
        const double d2 = dx * dx + dy * dy;
        if (d2 < bestDist) { bestDist = d2; best = &c; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Static layer cache (background + board + components, LOD by zoom)
// ---------------------------------------------------------------------------

void BoardMinimap::ensureStaticCache()
{
    const qreal dpr = devicePixelRatioF();
    const QSize wanted = size() * dpr;
    if (!m_staticDirty && m_staticCache.size() == wanted) return;

    QPixmap pm(wanted);
    pm.setDevicePixelRatio(dpr);
    pm.fill(QColor(25, 25, 35));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Board body + outline (real edge-cuts when the iBOM provides them)
    const QPointF tl = pcbToWidget(m_pcbMinX, m_pcbMinY);
    const QPointF br = pcbToWidget(m_pcbMaxX, m_pcbMaxY);
    const QRectF boardRect(tl, br);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(35, 40, 50));
    p.drawRect(boardRect);

    // Scanned-board background (A1 mosaic): the real board photo under the
    // component layer. The mosaic canvas is in raw PCB mm by construction —
    // the same frame as this map — so a plain rect draw registers it.
    if (m_boardImageVisible && !m_boardImage.isNull()
        && m_boardImageLayer == m_activeLayer && m_boardImageRect.isValid()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QRectF target(
            pcbToWidget(m_boardImageRect.left(),  m_boardImageRect.top()),
            pcbToWidget(m_boardImageRect.right(), m_boardImageRect.bottom()));
        p.drawImage(target, m_boardImage);
    }

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(theme::boardOutlineColor(), 1));
    if (!m_project.boardOutline.empty()) {
        const double s = viewScale();
        for (const auto& seg : m_project.boardOutline) {
            using T = ibom::DrawingSegment::Type;
            switch (seg.type) {
            case T::Line:
                p.drawLine(pcbToWidget(seg.start.x, seg.start.y),
                           pcbToWidget(seg.end.x, seg.end.y));
                break;
            case T::Circle: {
                const double r = seg.radius * s;
                p.drawEllipse(pcbToWidget(seg.start.x, seg.start.y), r, r);
                break;
            }
            case T::Arc: {
                // Rounded corners at last: pcbdata arcs carry centre
                // (`start`), radius and startangle/endangle in degrees,
                // y-down screen-clockwise (canvas convention). Qt's drawArc
                // counts 1/16° counter-clockwise on screen → negate both.
                if (seg.radius <= 0.0 ||
                    (seg.startAngle == 0.0 && seg.endAngle == 0.0))
                    break;   // no angle data (old file) — don't guess
                const double r = seg.radius * s;
                const QPointF c = pcbToWidget(seg.start.x, seg.start.y);
                const QRectF rect(c.x() - r, c.y() - r, 2.0 * r, 2.0 * r);
                double sweep = std::fmod(seg.endAngle - seg.startAngle, 360.0);
                if (sweep < 0.0) sweep += 360.0;
                p.drawArc(rect,
                          static_cast<int>(std::lround(-seg.startAngle * 16.0)),
                          static_cast<int>(std::lround(-sweep * 16.0)));
                break;
            }
            default:
                break;
            }
        }
    } else {
        p.drawRect(boardRect);
    }

    // Visible PCB window for culling (zoomed views on large boards)
    const cv::Point2d v0 = widgetToPcb(QPointF(0, 0));
    const cv::Point2d v1 = widgetToPcb(QPointF(width(), height()));

    std::vector<const ibom::Component*> vis;
    vis.reserve(m_project.components.size());
    for (const auto& comp : m_project.components) {
        if (comp.layer != m_activeLayer) continue;
        if (comp.bbox.maxX < v0.x || comp.bbox.minX > v1.x ||
            comp.bbox.maxY < v0.y || comp.bbox.minY > v1.y) continue;
        vis.push_back(&comp);
    }

    const double s = viewScale();
    auto colorFor = [this](const ibom::Component& comp) {
        if (comp.reference == m_selectedRef) return QColor(210, 30, 30);
        if (m_placedRefs.count(comp.reference)) return theme::placedColor().darker(130);
        return theme::accentDark();
    };

    if (s < kPointModeBelow) {
        // Dot mode: on a big board zoomed out, rects are sub-pixel noise —
        // a density-style dot field reads better and draws far faster.
        QVector<QPointF> normal, placed;
        for (const auto* comp : vis) {
            const QPointF c = pcbToWidget(comp->bbox.center().x, comp->bbox.center().y);
            (m_placedRefs.count(comp->reference) ? placed : normal) << c;
        }
        p.setPen(QPen(theme::accentDark(), 2.0));
        p.drawPoints(normal.constData(), int(normal.size()));
        p.setPen(QPen(theme::placedColor().darker(130), 2.0));
        p.drawPoints(placed.constData(), int(placed.size()));
    } else {
        for (const auto* comp : vis) {
            const QColor col = colorFor(*comp);
            const bool selected = comp->reference == m_selectedRef;
            QPointF ctl = pcbToWidget(comp->bbox.minX, comp->bbox.minY);
            QPointF cbr = pcbToWidget(comp->bbox.maxX, comp->bbox.maxY);
            QRectF r(ctl, cbr);
            if (r.width()  < 1.5) r.setWidth(1.5);
            if (r.height() < 1.5) r.setHeight(1.5);
            QColor fill = col;
            fill.setAlphaF(selected ? 0.55f : 0.25f);
            p.setPen(QPen(col, selected ? 1.2f : 0.8f));
            p.setBrush(fill);
            p.drawRect(r);
        }

        // Zoomed-in detail: pads, then reference labels on top.
        if (s >= kPadsAbove && int(vis.size()) <= kPadsMaxComps) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 220, 50, 110));
            for (const auto* comp : vis) {
                for (const auto& pad : comp->pads) {
                    const QPointF c = pcbToWidget(pad.position.x, pad.position.y);
                    const double rw = std::max(1.2, pad.sizeX * s * 0.5);
                    const double rh = std::max(1.2, pad.sizeY * s * 0.5);
                    p.drawEllipse(c, rw, rh);
                }
            }
        }
        if (s >= kRefsAbove && int(vis.size()) <= kRefsMaxComps) {
            p.setFont(QFont(QStringLiteral("monospace"), 7));
            p.setPen(QColor(205, 210, 225, 220));
            for (const auto* comp : vis) {
                const double wpx = comp->bbox.width() * s;
                if (wpx < 16.0) continue;
                QPointF ctl = pcbToWidget(comp->bbox.minX, comp->bbox.minY);
                QPointF cbr = pcbToWidget(comp->bbox.maxX, comp->bbox.maxY);
                p.drawText(QRectF(ctl, cbr), Qt::AlignCenter,
                           QString::fromStdString(comp->reference));
            }
        }
    }

    m_staticCache = pm;
    m_staticDirty = false;
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void BoardMinimap::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    if (m_project.components.empty()) {
        p.fillRect(rect(), QColor(25, 25, 35));
        p.setPen(QColor(100, 100, 120));
        p.drawText(rect(), Qt::AlignCenter, tr("No iBOM loaded"));
        p.setPen(QPen(QColor(80, 90, 110), 1));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
        return;
    }
    if (!m_boundsValid) rebuildBounds();

    // FOV first: follow mode re-centres the view before anything is drawn.
    std::vector<cv::Point2f> fovPcb;
    m_fovValid = computeFovPcb(fovPcb);
    if (m_fovValid) {
        cv::Point2f c(0, 0);
        for (const auto& pt : fovPcb) c += pt;
        m_fovCenterPcb = c * 0.25f;

        if (m_followFov) {
            // Dead-band: only re-centre once the FOV strays 15% of the view,
            // so the cached component layer isn't rebuilt on every frame.
            const double s = viewScale();
            const double dx = (m_fovCenterPcb.x - m_viewCenterX) * s;
            const double dy = (m_fovCenterPcb.y - m_viewCenterY) * s;
            if (std::hypot(dx, dy) > 0.15 * std::min(width(), height())) {
                m_viewCenterX = m_fovCenterPcb.x;
                m_viewCenterY = m_fovCenterPcb.y;
                clampViewCenter();
                markStaticDirty();
            }
        }
    }

    ensureStaticCache();
    p.drawPixmap(0, 0, m_staticCache);

    // Inspection coverage (translucent tint of everything the FOV has visited)
    if (m_coverageVisible && !m_coverage.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QRectF target(pcbToWidget(m_pcbMinX, m_pcbMinY),
                            pcbToWidget(m_pcbMaxX, m_pcbMaxY));
        p.setOpacity(0.14);
        p.drawImage(target, m_coverage);
        p.setOpacity(1.0);
    }

    // Selected component guide: draw its pads and mark pin 1. This is the
    // visual reference used during multi-component alignment — it lets the user
    // read the part's orientation and locate pin 1 on the real board before
    // clicking in the camera image (crucial for irregular footprints like an
    // ESP32 module where "the body" is otherwise ambiguous).
    //
    // Also draw a fixed-size halo + crosshair at the component's center,
    // independent of its actual bbox size: a small SMD part's highlighted
    // rect above can be 1-2px on a dense/crowded board and effectively
    // invisible — the halo guarantees the selection is always spottable.
    if (!m_selectedRef.empty()) {
        for (const auto& comp : m_project.components) {
            if (comp.reference != m_selectedRef) continue;

            QPointF center = pcbToWidget(comp.bbox.center().x, comp.bbox.center().y);
            const QColor halo(210, 30, 30);  // dark red — clearly visible on the dark map

            p.setPen(QPen(halo, 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawLine(QPointF(center.x() - 10, center.y()), QPointF(center.x() + 10, center.y()));
            p.drawLine(QPointF(center.x(), center.y() - 10), QPointF(center.x(), center.y() + 10));
            p.drawEllipse(center, 6.0, 6.0);

            if (comp.layer != m_activeLayer) break;  // pads below: front/back only

            // Pad outlines (thin) + pin 1 (prominent red "click here" marker:
            // filled dot + ring + crosshair). The user relies on this red pin
            // — rather than a green target — when marking pin 1 in multi-align.
            for (const auto& pad : comp.pads) {
                QPointF c = pcbToWidget(pad.position.x, pad.position.y);
                if (pad.isPin1) {
                    // Dark halo so the red stays visible over yellow pad dots.
                    p.setPen(QPen(QColor(0, 0, 0, 160), 3.0));
                    p.setBrush(Qt::NoBrush);
                    p.drawEllipse(c, 6.0, 6.0);
                    p.setPen(QPen(QColor(255, 70, 70), 2.0f));
                    p.drawEllipse(c, 6.0, 6.0);
                    p.drawLine(QPointF(c.x() - 8, c.y()), QPointF(c.x() + 8, c.y()));
                    p.drawLine(QPointF(c.x(), c.y() - 8), QPointF(c.x(), c.y() + 8));
                    p.setPen(Qt::NoPen);
                    p.setBrush(QColor(255, 70, 70, 230));
                    p.drawEllipse(c, 3.0, 3.0);
                } else {
                    p.setPen(QPen(QColor(255, 220, 50, 200), 0.8f));
                    p.setBrush(QColor(255, 220, 50, 90));
                    p.drawEllipse(c, 1.6, 1.6);
                }
            }
            break;
        }
    }

    // 📌 note markers (B2): a small amber dot with a dark rim at the
    // top-right corner of every annotated component of the active side —
    // always visible regardless of LOD (few notes expected, O(n) scan with
    // set lookups is cheap at paint time).
    if (!m_annotatedRefs.empty()) {
        p.setPen(QPen(QColor(40, 30, 0), 1.0));
        p.setBrush(QColor(255, 200, 40, 235));
        for (const auto& comp : m_project.components) {
            if (comp.layer != m_activeLayer) continue;
            if (!m_annotatedRefs.count(comp.reference)) continue;
            const QPointF c = pcbToWidget(comp.bbox.maxX, comp.bbox.minY);
            p.drawEllipse(c, 3.0, 3.0);
        }
    }

    // Click targets: the exact point(s) the user must click in the camera
    // image to calibrate (multi-component alignment). Drawn last among the
    // guides, as prominent bright-green numbered rings so they stand out over
    // the dark-red selection halo and yellow pad dots.
    if (!m_clickTargets.empty()) {
        p.setFont(QFont("monospace", 8, QFont::Bold));
        for (size_t i = 0; i < m_clickTargets.size(); ++i) {
            QPointF c = pcbToWidget(m_clickTargets[i].x, m_clickTargets[i].y);
            const QColor ring = m_clickTargetColor;  // green (corners) / red (pads)

            // Same graphic as the pin-1 marker: dark halo + coloured ring +
            // crosshair + filled centre dot, so "opposite pads" reads the same
            // way as "pin 1".
            p.setPen(QPen(QColor(0, 0, 0, 160), 3.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, 7.0, 7.0);
            p.setPen(QPen(ring, 2.0));
            p.drawEllipse(c, 7.0, 7.0);
            p.drawLine(QPointF(c.x() - 9, c.y()), QPointF(c.x() + 9, c.y()));
            p.drawLine(QPointF(c.x(), c.y() - 9), QPointF(c.x(), c.y() + 9));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(ring.red(), ring.green(), ring.blue(), 230));
            p.drawEllipse(c, 3.0, 3.0);

            // Number the targets when there is more than one (click order is free).
            if (m_clickTargets.size() > 1) {
                p.setPen(QColor(0, 0, 0));
                p.drawText(QRectF(c.x() + 8, c.y() - 14, 14, 14),
                           Qt::AlignCenter, QString::number(i + 1));
                p.setPen(ring);
                p.drawText(QRectF(c.x() + 7, c.y() - 15, 14, 14),
                           Qt::AlignCenter, QString::number(i + 1));
            }
        }
    }

    // FOV polygon, coloured by tracking health. While the user drags it, draw
    // at the dragged position (solid) — release emits anchorRequested there.
    m_lastFovPolyWidget.clear();
    if (m_fovValid) {
        const bool dragging = (m_drag == Drag::Fov);
        QPolygonF poly;
        for (const auto& pt : fovPcb)
            poly << pcbToWidget(pt.x + (dragging ? m_fovDragDelta.x : 0.0),
                                pt.y + (dragging ? m_fovDragDelta.y : 0.0));
        const QColor col = fovColor();
        QPen fovPen(QColor(col.red(), col.green(), col.blue(), 200), 1.5,
                    dragging ? Qt::SolidLine : Qt::DashLine);
        p.setPen(fovPen);
        p.setBrush(QColor(col.red(), col.green(), col.blue(), dragging ? 45 : 25));
        p.drawPolygon(poly);
        m_lastFovPolyWidget = poly;
    }

    // Global inset when zoomed in: whole board + current view + FOV.
    if (m_zoom > 1.05) drawInset(p, m_lastFovPolyWidget);

    drawScaleBar(p);

    // Border
    p.setPen(QPen(QColor(80, 90, 110), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    // Label: board dimensions + zoom factor
    QString label = tr("PCB Map");
    const double bw = m_pcbMaxX - m_pcbMinX;
    const double bh = m_pcbMaxY - m_pcbMinY;
    if (bw > 0.01 && bh > 0.01)
        label += QStringLiteral("  %1×%2 mm").arg(bw, 0, 'f', 0).arg(bh, 0, 'f', 0);
    if (m_zoom > 1.05)
        label += QStringLiteral("  ×%1").arg(m_zoom, 0, 'f', 1);
    p.setPen(QColor(140, 150, 170));
    p.setFont(QFont("monospace", 7));
    p.drawText(rect().adjusted(3, 2, -3, -2), Qt::AlignTop | Qt::AlignRight, label);
}

void BoardMinimap::drawInset(QPainter& p, const QPolygonF& fovWidgetPoly)
{
    const double pcbW = std::max(m_pcbMaxX - m_pcbMinX, 1e-6);
    const double pcbH = std::max(m_pcbMaxY - m_pcbMinY, 1e-6);

    double iw = std::clamp(width() * 0.26, 44.0, 130.0);
    double ih = iw * pcbH / pcbW;
    const double maxH = height() * 0.30;
    if (ih > maxH) { ih = maxH; iw = ih * pcbW / pcbH; }

    const QRectF inset(width() - iw - 8.0, height() - ih - 8.0, iw, ih);
    p.setPen(QPen(QColor(120, 130, 160, 160), 1));
    p.setBrush(QColor(15, 15, 24, 210));
    p.drawRect(inset);

    auto toInset = [&](double x, double y) {
        return QPointF(inset.left() + (x - m_pcbMinX) / pcbW * inset.width(),
                       inset.top()  + (y - m_pcbMinY) / pcbH * inset.height());
    };

    // Current view window
    const cv::Point2d v0 = widgetToPcb(QPointF(0, 0));
    const cv::Point2d v1 = widgetToPcb(QPointF(width(), height()));
    QRectF viewRect(toInset(std::max(v0.x, m_pcbMinX), std::max(v0.y, m_pcbMinY)),
                    toInset(std::min(v1.x, m_pcbMaxX), std::min(v1.y, m_pcbMaxY)));
    p.setPen(QPen(QColor(230, 230, 240, 200), 1));
    p.setBrush(QColor(230, 230, 240, 30));
    p.drawRect(viewRect);

    // FOV position (dot is enough at this size)
    if (m_fovValid) {
        Q_UNUSED(fovWidgetPoly);
        const QColor col = fovColor();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(col.red(), col.green(), col.blue(), 230));
        p.drawEllipse(toInset(m_fovCenterPcb.x, m_fovCenterPcb.y), 2.5, 2.5);
    }
}

void BoardMinimap::drawScaleBar(QPainter& p)
{
    const double s = viewScale();  // px per mm
    if (s <= 1e-9) return;

    // Nice length (1/2/5 × 10^n mm) rendering between 40 px and 35% of width.
    const double maxPx = std::max(40.0, width() * 0.35);
    double bestMm = 0;
    for (double mag = 0.01; mag <= 1000.0; mag *= 10.0) {
        for (double m : {1.0, 2.0, 5.0}) {
            const double mm = m * mag;
            const double px = mm * s;
            if (px >= 40.0 && px <= maxPx) bestMm = mm;
        }
    }
    if (bestMm <= 0) return;

    const double px = bestMm * s;
    const double y = height() - 10.0;
    const double x0 = 8.0;
    p.setPen(QPen(QColor(200, 205, 220, 220), 1.5));
    p.drawLine(QPointF(x0, y), QPointF(x0 + px, y));
    p.drawLine(QPointF(x0, y - 3), QPointF(x0, y + 3));
    p.drawLine(QPointF(x0 + px, y - 3), QPointF(x0 + px, y + 3));
    p.setFont(QFont("monospace", 7));
    const QString txt = bestMm >= 1.0
        ? QStringLiteral("%1 mm").arg(bestMm, 0, 'f', 0)
        : QStringLiteral("%1 mm").arg(bestMm, 0, 'g', 2);
    p.drawText(QRectF(x0, y - 16, std::max(px, 60.0), 12), Qt::AlignLeft, txt);
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

void BoardMinimap::mousePressEvent(QMouseEvent* ev)
{
    if (!m_boundsValid) return;

    if (ev->button() == Qt::MiddleButton) {
        m_drag = Drag::Pan;
        m_pressPos = ev->pos();
        m_pressPcb = widgetToPcb(ev->position());
        return;
    }
    if (ev->button() != Qt::LeftButton) return;

    m_drag = Drag::Pending;
    m_pressPos = ev->pos();
    m_pressPcb = widgetToPcb(ev->position());
    m_pressInsideFov = !m_lastFovPolyWidget.isEmpty() &&
        m_lastFovPolyWidget.containsPoint(ev->position(), Qt::OddEvenFill);
    m_fovDragDelta = {0, 0};
}

void BoardMinimap::mouseMoveEvent(QMouseEvent* ev)
{
    if (m_drag == Drag::Pending) {
        if ((ev->pos() - m_pressPos).manhattanLength() < kDragThresholdPx) return;
        if (m_pressInsideFov && m_fovValid) {
            m_drag = Drag::Fov;
        } else if (m_zoom > 1.05) {
            m_drag = Drag::Pan;
            // Panning by hand breaks the auto-follow contract.
            if (m_followFov) setFollowFov(false);
        } else {
            m_drag = Drag::None;  // slipped click at fit zoom: cancel
            return;
        }
    }

    if (m_drag == Drag::Pan) {
        const QPointF d = ev->position() - QPointF(m_pressPos);
        const double s = viewScale();
        m_viewCenterX -= d.x() / s;
        m_viewCenterY -= d.y() / s;
        clampViewCenter();
        m_pressPos = ev->pos();
        markStaticDirty();
        update();
    } else if (m_drag == Drag::Fov) {
        const cv::Point2d cur = widgetToPcb(ev->position());
        m_fovDragDelta = { cur.x - m_pressPcb.x, cur.y - m_pressPcb.y };
        update();
    }
}

void BoardMinimap::mouseReleaseEvent(QMouseEvent* ev)
{
    if (ev->button() == Qt::MiddleButton && m_drag == Drag::Pan) {
        m_drag = Drag::None;
        return;
    }
    if (ev->button() != Qt::LeftButton) return;

    const Drag drag = m_drag;
    m_drag = Drag::None;

    if (drag == Drag::Fov) {
        // Drag-to-anchor: re-centre the FOV where the user dropped it.
        const double px = std::clamp(double(m_fovCenterPcb.x) + m_fovDragDelta.x,
                                     m_pcbMinX, m_pcbMaxX);
        const double py = std::clamp(double(m_fovCenterPcb.y) + m_fovDragDelta.y,
                                     m_pcbMinY, m_pcbMaxY);
        m_fovDragDelta = {0, 0};
        update();
        emit anchorRequested(cv::Point2f(float(px), float(py)));
        return;
    }
    if (drag != Drag::Pending) return;  // pan ended, or cancelled slip

    // Plain click.
    const cv::Point2d pcb = widgetToPcb(ev->position());
    if (ev->modifiers() & Qt::ControlModifier) {
        if (const auto* comp = componentAt(pcb))
            emit componentPicked(comp->reference);
        return;
    }
    const double px = std::clamp(pcb.x, m_pcbMinX, m_pcbMaxX);
    const double py = std::clamp(pcb.y, m_pcbMinY, m_pcbMaxY);
    emit anchorRequested(cv::Point2f(float(px), float(py)));
}

void BoardMinimap::mouseDoubleClickEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) return;
    if (m_expandedMode)
        resetView();
    else
        emit expandRequested();
}

void BoardMinimap::wheelEvent(QWheelEvent* ev)
{
    if (!m_boundsValid) return;
    const double steps = ev->angleDelta().y() / 120.0;
    if (steps == 0.0) return;

    const cv::Point2d anchor = widgetToPcb(ev->position());
    const double newZoom = std::clamp(m_zoom * std::pow(1.3, steps), 1.0, kMaxZoom);
    if (newZoom == m_zoom) return;
    m_zoom = newZoom;

    if (m_zoom <= 1.001) {
        m_viewCenterX = (m_pcbMinX + m_pcbMaxX) / 2.0;
        m_viewCenterY = (m_pcbMinY + m_pcbMaxY) / 2.0;
    } else if (!m_followFov) {
        // Keep the PCB point under the cursor fixed while zooming.
        const double s = viewScale();
        m_viewCenterX = anchor.x - (ev->position().x() - width()  / 2.0) / s;
        m_viewCenterY = anchor.y - (ev->position().y() - height() / 2.0) / s;
        clampViewCenter();
    }
    markStaticDirty();
    update();
    ev->accept();
}

void BoardMinimap::contextMenuEvent(QContextMenuEvent* ev)
{
    QMenu menu(this);

    auto* actFollow = menu.addAction(tr("Follow camera FOV"));
    actFollow->setCheckable(true);
    actFollow->setChecked(m_followFov);
    connect(actFollow, &QAction::toggled, this, [this](bool on) { setFollowFov(on); });

    auto* actFit = menu.addAction(tr("Fit board (reset zoom)"));
    connect(actFit, &QAction::triggered, this, &BoardMinimap::resetView);

    if (!m_expandedMode) {
        auto* actExpand = menu.addAction(tr("Open large map\tM"));
        connect(actExpand, &QAction::triggered, this, [this]() { emit expandRequested(); });
        auto* actFloat = menu.addAction(tr("Detach / re-attach panel"));
        connect(actFloat, &QAction::triggered, this, [this]() { emit floatRequested(); });
    }

    if (!m_boardImage.isNull()) {
        auto* actImg = menu.addAction(tr("Show scanned board image"));
        actImg->setCheckable(true);
        actImg->setChecked(m_boardImageVisible);
        connect(actImg, &QAction::toggled,
                this, [this](bool on) { setBoardImageVisible(on); });
    }

    menu.addSeparator();
    auto* actCov = menu.addAction(tr("Show inspection coverage"));
    actCov->setCheckable(true);
    actCov->setChecked(m_coverageVisible);
    connect(actCov, &QAction::toggled, this, [this](bool on) { setCoverageVisible(on); });
    auto* actCovReset = menu.addAction(tr("Reset coverage"));
    connect(actCovReset, &QAction::triggered, this, &BoardMinimap::resetCoverage);

    menu.exec(ev->globalPos());
}

void BoardMinimap::layoutButtons()
{
    int x = 4;
    for (QToolButton* b : {m_btnFit, m_btnFollow, m_btnExpand}) {
        if (!b || !b->isVisibleTo(this)) continue;
        b->move(x, 4);
        x += b->width() + 2;
    }
}

void BoardMinimap::resizeEvent(QResizeEvent*)
{
    markStaticDirty();
    clampViewCenter();
    layoutButtons();
}

} // namespace ibom::gui
