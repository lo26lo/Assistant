#pragma once

#include <QColor>
#include <QImage>
#include <QTransform>
#include <opencv2/core.hpp>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ibom/IBomData.h"   // ibom::Layer

namespace ibom::overlay {

/**
 * @brief Self-contained snapshot of everything the overlay render needs.
 *
 * All members are value copies so the render never touches Application / GUI
 * objects while running: the IBomProject is shared and immutable after load,
 * colors/toggles are resolved on the GUI thread (Config + Theme) and captured
 * here. The homography is deliberately absent — the overlay is rendered in
 * board (PCB) space and re-projected at paint time by CameraView with the
 * current pose, so pose changes never trigger a re-render.
 */
struct OverlayInputs {
    std::shared_ptr<const IBomProject> project;
    std::string                        selectedRef;
    std::unordered_set<std::string>    placedRefs;
    QColor cSelected, cPlaced, cNormal, labelNormal;
    float  placedAlphaMul = 1.0f;
    float  selectedSilkW  = 1.0f;   // outline width in buffer px (scales with zoom)
    bool   drawPads = true;
    bool   drawSilk = true;

    /// Which side of the board the camera is looking at. Back renders THAT
    /// layer's components into a MIRRORED (view-space) buffer — labels stay
    /// readable in the buffer, and pcbToBuffer carries the mirror so the
    /// composed buffer→image warp (with a raw-PCB→image homography whose
    /// determinant is negative for a back view) remains orientation-
    /// preserving on screen.
    ibom::Layer activeLayer = ibom::Layer::Front;

    /// Revision-diff rework marks (C1 V2). Refs present here are recolored
    /// over their normal/placed colors (selection still wins): 1 = REMOVE
    /// (desolder — red, with an X across the body), 2 = CHANGE (orange).
    std::map<std::string, int> diffMarks;
    /// Components to ADD in the target revision: they don't exist in the
    /// current project, so they're drawn as green ring markers at their
    /// target positions (already filtered to the active layer by the caller).
    std::vector<std::pair<Point2D, std::string>> diffAdds;
};

/// A board-space overlay buffer plus the PCB→buffer mapping it was drawn with.
struct BoardOverlay {
    QImage     image;        // ARGB32_Premultiplied, transparent background
    QTransform pcbToBuffer;  // PCB coords (mm) → buffer px (translate + scale)
};

/**
 * @brief Stateless iBOM overlay renderer — the single overlay draw path.
 *
 * Renders the front layer's pads, silkscreen and labels ONCE into a
 * board-space buffer (antialiased — this only reruns when the selection,
 * placed set, toggles, colors or project change, never per frame). CameraView
 * then warps the buffer through the current homography as a projective
 * QTransform on every paint, so the overlay stays locked to the freshest pose
 * at a fixed, component-count-independent per-frame cost. This replaces the
 * previous full-frame vector re-render, which had to run per homography
 * update with AA off under a 25 fps cap to fit the GUI-thread budget
 * (suite 100 / LIVE_TRACKING_ANALYSE_2026-07.md F11).
 */
class OverlayRenderer {
public:
    static BoardOverlay renderBoardSpace(const OverlayInputs& in);

    /// cv 3×3 (column-vector convention: p' = H·p) → QTransform (row-vector
    /// convention: p' = p·T), i.e. the transpose. Empty/ill-sized input → identity.
    static QTransform toQTransform(const cv::Mat& h3x3);
};

} // namespace ibom::overlay
