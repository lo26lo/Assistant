#pragma once

#include "ibom/IBomData.h"
#include <QWidget>
#include <QImage>
#include <QPixmap>
#include <QColor>
#include <QPolygonF>
#include <QRectF>
#include <QElapsedTimer>
#include <opencv2/core.hpp>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class QToolButton;

namespace ibom::overlay {
class Homography;
}

namespace ibom::gui {

/**
 * Top-down PCB overview widget.
 *
 * Shows all components in PCB coordinate space. A dashed rectangle shows the
 * current camera FOV (coloured by tracking health). Clicking anywhere emits
 * anchorRequested(pcbPoint) so the caller can re-anchor the overlay around
 * that PCB position.
 *
 * View navigation (all per-instance, data is shared via attachPeer):
 *  - mouse wheel: zoom, anchored at the cursor (1× = fit board)
 *  - left-drag on empty area (when zoomed) or middle-drag: pan
 *  - left-drag on the FOV rectangle: move the FOV → anchorRequested on release
 *  - Ctrl+click: pick the component under the cursor (componentPicked)
 *  - double-click: open the large-map view (dock instance) / fit (large view)
 *  - right-click: context menu (follow FOV, coverage, detach, …)
 *
 * "Follow FOV" auto-centres the view on the camera FOV (≈3× the FOV extent),
 * with a global inset in the corner whenever the view is zoomed in.
 * Component rendering is level-of-detail: dots when zoomed way out, bboxes at
 * normal scale, pads + reference labels when zoomed in. The static component
 * layer is cached in a QPixmap so per-frame repaints (FOV updates at camera
 * rate) only redraw the dynamic markers.
 */
class BoardMinimap : public QWidget {
    Q_OBJECT

public:
    explicit BoardMinimap(QWidget* parent = nullptr);

    /// Feed the full iBOM project (call once after loading).
    void setIBomData(const ibom::IBomProject& project);

    /// Update the current homography (image → PCB mapping) and camera resolution
    /// so the FOV rectangle can be recomputed.
    void setHomography(const ibom::overlay::Homography* hom, QSize cameraSize);

    /// Active layer (Front / Back) — only those components are drawn.
    void setActiveLayer(ibom::Layer layer);

    /// Currently selected ref (drawn highlighted). When the view is zoomed in
    /// (and not following the FOV), the view auto-centres on the component.
    void setSelectedRef(const std::string& ref);

    /// Components already placed (drawn faded).
    void setPlacedRefs(const std::unordered_set<std::string>& refs);

    /// Components carrying a pinned note (B2) — drawn with a small amber
    /// marker at their top-right corner.
    void setAnnotatedRefs(const std::unordered_set<std::string>& refs);

    /// Orthorectified board image (last finished scan, A1) drawn under the
    /// components as the map background. `pcbRect` is the image's extent in
    /// raw PCB mm; only drawn while `layer` is the active side. Pass a null
    /// image to clear. Toggleable from the context menu.
    void setBoardImage(const QImage& img, const QRectF& pcbRect,
                       ibom::Layer layer);
    void setBoardImageVisible(bool on);

    /// Highlight the exact PCB points the user must click in the camera image
    /// to calibrate (multi-component alignment) — e.g. the two farthest-apart
    /// pads, pin 1, or the two body corners of the component being marked.
    /// Drawn as prominent numbered markers (ring + crosshair + dot) in the
    /// given colour. Pass an empty vector to clear. The colour lets the
    /// "opposite pads" method reuse the same red graphic as the pin-1 marker.
    void setClickTargets(const std::vector<cv::Point2f>& pcbPts,
                         const QColor& color = QColor(50, 230, 90));

    /// Live tracking health — colours the FOV rectangle (cyan = good,
    /// orange = weak, red = lost). Negative values mean "unknown".
    void setTrackingQuality(int inliers, double reprojErrPx);
    void setTrackingLost(bool lost);

    /// Stamp the current FOV into the inspection-coverage layer. Called by the
    /// application on every homography update (works even while the widget is
    /// hidden behind another dock tab). Internally throttled.
    void accumulateCoverage();
    void setCoverageVisible(bool on);
    void resetCoverage();

    // ── View control ────────────────────────────────────────────
    /// Reset zoom/pan to "fit board".
    void resetView();
    /// Auto-centre the view on the camera FOV (≈3× the FOV extent).
    void setFollowFov(bool on);
    bool followFov() const { return m_followFov; }
    /// Large-map instances: double-click fits instead of re-opening, and the
    /// expand button / detach menu entry are hidden.
    void setExpandedMode(bool on);

    /// Board width/height ratio (0 when no board is loaded) — lets the caller
    /// size a floating window to the board's shape.
    double boardAspectRatio() const;

    /// Mirror every future data setter into `peer` (one level, no cycles) and
    /// push the current state immediately. Used by the large-map dialog so
    /// Application only ever talks to the dock instance. View state (zoom,
    /// pan, follow) stays per-instance.
    void attachPeer(BoardMinimap* peer);

signals:
    /// User clicked at PCB coordinate (mm). Caller should anchor to this position.
    void anchorRequested(cv::Point2f pcbPoint);
    /// Ctrl+click on a component (its reference).
    void componentPicked(const std::string& ref);
    /// Expand button / double-click: open the large-map view.
    void expandRequested();
    /// Context menu: detach / re-attach the dock panel.
    void floatRequested();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    QSize sizeHint() const override { return {220, 160}; }
    QSize minimumSizeHint() const override { return {140, 100}; }

private:
    // Coordinate mapping (view = zoom + centre over the board bbox)
    double  fitScale() const;                  ///< px per PCB unit at zoom 1
    double  viewScale() const { return fitScale() * m_zoom; }
    QPointF pcbToWidget(double x, double y) const;
    cv::Point2d widgetToPcb(const QPointF& wp) const;
    void    clampViewCenter();
    void    markStaticDirty() { m_staticDirty = true; }

    void rebuildBounds();                      ///< board bbox + coverage alloc
    void ensureStaticCache();                  ///< background + components (LOD)
    bool computeFovPcb(std::vector<cv::Point2f>& out) const;
    QColor fovColor() const;
    const ibom::Component* componentAt(const cv::Point2d& pcb) const;
    void layoutButtons();
    void drawInset(QPainter& p, const QPolygonF& fovWidgetPoly);
    void drawScaleBar(QPainter& p);

    ibom::IBomProject                   m_project;
    const ibom::overlay::Homography*    m_homography  = nullptr;
    QSize                               m_cameraSize;
    ibom::Layer                         m_activeLayer = ibom::Layer::Front;
    std::string                         m_selectedRef;
    std::unordered_set<std::string>     m_placedRefs;
    std::unordered_set<std::string>     m_annotatedRefs;  // 📌 note markers (B2)
    std::vector<cv::Point2f>            m_clickTargets;  // PCB points to click (multi-align)
    QColor                              m_clickTargetColor{50, 230, 90};  // ring colour

    // Scanned-board background (A1 mosaic, raw PCB mm extent, per face)
    QImage      m_boardImage;
    QRectF      m_boardImageRect;
    ibom::Layer m_boardImageLayer = ibom::Layer::Front;
    bool        m_boardImageVisible = true;

    // Board bbox (PCB units)
    double  m_pcbMinX  = 0, m_pcbMinY  = 0;
    double  m_pcbMaxX  = 1, m_pcbMaxY  = 1;
    bool    m_boundsValid = false;
    int     m_marginPx = 6;

    // View state (per instance — not mirrored to the peer)
    double  m_zoom = 1.0;                      // 1 = fit board, wheel-zoomable
    double  m_viewCenterX = 0.5, m_viewCenterY = 0.5;  // PCB coords
    bool    m_followFov = false;
    bool    m_expandedMode = false;

    // Static layer cache (background + board + components at current view)
    QPixmap m_staticCache;
    bool    m_staticDirty = true;

    // Tracking health (FOV colour)
    int     m_inliers   = -1;
    double  m_reprojErr = -1.0;
    bool    m_trackingLost = false;

    // Inspection coverage (accumulated FOV footprint, PCB-space raster)
    QImage        m_coverage;
    double        m_coverageScale = 1.0;       // coverage px per PCB unit
    bool          m_coverageVisible = true;
    QElapsedTimer m_coverageTimer;

    // Mouse interaction
    enum class Drag { None, Pending, Pan, Fov };
    Drag        m_drag = Drag::None;
    QPoint      m_pressPos;
    cv::Point2d m_pressPcb{0, 0};
    bool        m_pressInsideFov = false;
    cv::Point2d m_fovDragDelta{0, 0};          // live offset while dragging the FOV

    // Last-painted FOV geometry (widget coords for hit-testing, PCB centre
    // for drag-to-anchor)
    QPolygonF   m_lastFovPolyWidget;
    cv::Point2f m_fovCenterPcb{0, 0};
    bool        m_fovValid = false;

    // Overlay buttons (fit / follow / expand)
    QToolButton* m_btnFit    = nullptr;
    QToolButton* m_btnFollow = nullptr;
    QToolButton* m_btnExpand = nullptr;

    BoardMinimap* m_peer = nullptr;            // large-map mirror (never cyclic)
};

} // namespace ibom::gui
