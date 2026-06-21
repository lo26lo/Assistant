#pragma once

#include "Homography.h"

#include <QImage>
#include <QSize>
#include <QColor>
#include <memory>
#include <string>
#include <unordered_set>

namespace ibom {
struct IBomProject;
}

namespace ibom::overlay {

/**
 * @brief Self-contained snapshot of everything the overlay render needs.
 *
 * All members are value copies so the render can run on a worker thread
 * (QtConcurrent) without touching the Application / GUI objects: the
 * IBomProject is shared and immutable after load, and the Homography holds a
 * refcounted cv::Mat. Colors/toggles are resolved on the GUI thread (from
 * Config + Theme) and captured here.
 */
struct OverlayInputs {
    std::shared_ptr<const IBomProject> project;
    Homography                         homo;
    QSize                              size;
    std::string                        selectedRef;
    std::unordered_set<std::string>    placedRefs;
    QColor cSelected, cPlaced, cNormal, labelNormal;
    float  placedAlphaMul = 1.0f;
    float  selectedSilkW  = 1.0f;
    bool   drawPads = true;
    bool   drawSilk = true;
};

/**
 * @brief Stateless iBOM overlay renderer — the single overlay draw path.
 *
 * Pure pixel work: draws component pads, silkscreen and reference labels into a
 * transparent ARGB image, with frustum culling of off-frame components and the
 * label fonts built once per render. No widget / Application / QObject state, so
 * render() is safe to call from a worker thread.
 */
class OverlayRenderer {
public:
    static QImage render(const OverlayInputs& in);
};

} // namespace ibom::overlay
