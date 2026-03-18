#pragma once

#include "ibom/IBomData.h"
#include "Homography.h"

#include <QObject>
#include <QPainter>
#include <QImage>
#include <opencv2/core.hpp>
#include <vector>

namespace ibom::overlay {

/**
 * @brief Renders iBOM component overlays onto the camera view.
 *
 * This is the main overlay compositor that combines:
 * - Component outlines from iBOM data
 * - AI detection results
 * - Color-coded states (placed, missing, wrong orientation)
 * - Info labels (reference, value, footprint)
 */
class OverlayRenderer : public QObject {
    Q_OBJECT

public:
    explicit OverlayRenderer(QObject* parent = nullptr);
    ~OverlayRenderer() override;

    /// Set the homography for coordinate mapping.
    void setHomography(const Homography& homography);

    /// Set the iBOM project data.
    void setIBomData(const IBomProject& project);

    /// Render overlays onto a camera frame.
    /// @param frame The camera frame (will be modified in-place or returned as copy).
    /// @param selectedRef Currently selected component reference (highlighted).
    /// @return Frame with overlays drawn.
    QImage render(const cv::Mat& frame, const std::string& selectedRef = "");

    // --- Visibility settings ---
    void setShowOutlines(bool show) { m_showOutlines = show; }
    void setShowLabels(bool show) { m_showLabels = show; }
    void setShowPads(bool show) { m_showPads = show; }
    void setShowPin1(bool show) { m_showPin1 = show; }
    void setOpacity(float opacity) { m_opacity = opacity; }
    void setActiveLayer(Layer layer) { m_activeLayer = layer; }

    /// Set which component references to highlight (e.g., from BOM selection).
    void setHighlightedRefs(const std::vector<std::string>& refs);

    /// Set component states for color coding.
    void setComponentState(const std::string& ref, const std::string& state);

signals:
    void overlayUpdated();

private:
    void drawComponentOutline(QPainter& painter, const Component& comp);
    void drawComponentLabel(QPainter& painter, const Component& comp);
    void drawComponentPads(QPainter& painter, const Component& comp);
    void drawPin1Marker(QPainter& painter, const Component& comp);
    void drawBoardOutline(QPainter& painter);
    QColor stateColor(const std::string& ref) const;

    /// Convert cv::Mat (BGR) to QImage (RGB).
    static QImage matToQImage(const cv::Mat& mat);

    Homography    m_homography;
    IBomProject   m_project;

    bool  m_showOutlines = true;
    bool  m_showLabels   = true;
    bool  m_showPads     = false;
    bool  m_showPin1     = true;
    float m_opacity      = 0.7f;
    Layer m_activeLayer  = Layer::Front;

    std::vector<std::string> m_highlightedRefs;
    std::map<std::string, std::string> m_componentStates;
    // States: "placed", "missing", "wrong_orientation", "inspected", ""
};

} // namespace ibom::overlay
