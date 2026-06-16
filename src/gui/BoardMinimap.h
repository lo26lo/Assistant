#pragma once

#include "ibom/IBomData.h"
#include <QWidget>
#include <QImage>
#include <opencv2/core.hpp>
#include <memory>
#include <string>
#include <unordered_set>

namespace ibom::overlay {
class Homography;
}

namespace ibom::gui {

/**
 * Compact top-down PCB overview widget.
 *
 * Shows all components in PCB coordinate space at reduced scale.
 * A cyan rectangle shows the current camera FOV.
 * Clicking anywhere emits anchorRequested(pcbPoint) so the caller
 * can re-anchor the overlay around that PCB position.
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

    /// Currently selected ref (drawn highlighted).
    void setSelectedRef(const std::string& ref);

    /// Components already placed (drawn faded).
    void setPlacedRefs(const std::unordered_set<std::string>& refs);

signals:
    /// User clicked at PCB coordinate (mm). Caller should anchor to this position.
    void anchorRequested(cv::Point2f pcbPoint);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    QSize sizeHint() const override { return {220, 160}; }
    QSize minimumSizeHint() const override { return {140, 100}; }

private:
    void rebuildCache();
    QPointF pcbToWidget(double x, double y) const;

    ibom::IBomProject                   m_project;
    const ibom::overlay::Homography*    m_homography  = nullptr;
    QSize                               m_cameraSize;
    ibom::Layer                         m_activeLayer = ibom::Layer::Front;
    std::string                         m_selectedRef;
    std::unordered_set<std::string>     m_placedRefs;

    // Cached rendering transform: PCB bbox → widget rect
    double  m_pcbMinX  = 0, m_pcbMinY  = 0;
    double  m_pcbMaxX  = 1, m_pcbMaxY  = 1;
    double  m_scaleX   = 1, m_scaleY   = 1;
    int     m_marginPx = 6;
    bool    m_cacheValid = false;
};

} // namespace ibom::gui
