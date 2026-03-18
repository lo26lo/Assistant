#pragma once

#include "ibom/IBomData.h"
#include "Homography.h"
#include <QPainter>
#include <QColor>
#include <map>

namespace ibom::overlay {

/**
 * @brief Per-component overlay drawing with state management.
 */
class ComponentOverlay {
public:
    ComponentOverlay() = default;
    ~ComponentOverlay() = default;

    /// Draw a single component overlay.
    void draw(QPainter& painter, const Component& comp,
              const Homography& homography, bool highlighted = false);

    /// Set the state (affects rendering color/style).
    void setState(const std::string& state) { m_state = state; }

    /// Color for the component based on state.
    QColor color() const;

private:
    std::string m_state; // "placed", "missing", "wrong_orientation", etc.
};

} // namespace ibom::overlay
