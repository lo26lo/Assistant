#pragma once

#include "IBomData.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

namespace ibom {

/**
 * @brief Spatial index for fast component lookup by position.
 *
 * Maps iBOM components into a grid for quick spatial queries
 * (e.g., "which component is at pixel X,Y after homography?").
 */
class ComponentMap {
public:
    ComponentMap() = default;
    ~ComponentMap() = default;

    /// Build the spatial index from a list of components.
    void build(const std::vector<Component>& components);

    /// Find the component closest to a given PCB coordinate.
    const Component* findNearest(Point2D pos, double maxDistance = 5.0) const;

    /// Find all components within a rectangle (PCB coords).
    std::vector<const Component*> findInRect(const BBox& rect) const;

    /// Find a component by reference designator.
    const Component* findByReference(const std::string& ref) const;

    /// Find all components matching a value (e.g., "10k").
    std::vector<const Component*> findByValue(const std::string& value) const;

    /// Find all components on a given layer.
    std::vector<const Component*> findByLayer(Layer layer) const;

    /// Get all components.
    const std::vector<Component>& allComponents() const { return m_components; }

    /// Number of components indexed.
    size_t count() const { return m_components.size(); }

private:
    std::vector<Component> m_components;

    // Index by reference: "R1" -> index
    std::unordered_map<std::string, size_t> m_refIndex;

    // Simple grid-based spatial index
    struct GridCell {
        std::vector<size_t> componentIndices;
    };
    std::unordered_map<int64_t, GridCell> m_grid;
    double m_cellSize = 5.0; // mm per grid cell

    int64_t gridKey(double x, double y) const;
    std::vector<int64_t> neighborKeys(double x, double y, double radius) const;
};

} // namespace ibom
