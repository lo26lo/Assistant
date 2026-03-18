#include "ComponentMap.h"

#include <cmath>
#include <algorithm>
#include <limits>

namespace ibom {

int64_t ComponentMap::gridKey(double x, double y) const
{
    int32_t gx = static_cast<int32_t>(std::floor(x / m_cellSize));
    int32_t gy = static_cast<int32_t>(std::floor(y / m_cellSize));
    return (static_cast<int64_t>(gx) << 32) | (static_cast<int64_t>(gy) & 0xFFFFFFFF);
}

std::vector<int64_t> ComponentMap::neighborKeys(double x, double y, double radius) const
{
    std::vector<int64_t> keys;
    int cells = static_cast<int>(std::ceil(radius / m_cellSize));
    int32_t cx = static_cast<int32_t>(std::floor(x / m_cellSize));
    int32_t cy = static_cast<int32_t>(std::floor(y / m_cellSize));

    for (int dx = -cells; dx <= cells; ++dx) {
        for (int dy = -cells; dy <= cells; ++dy) {
            int32_t gx = cx + dx;
            int32_t gy = cy + dy;
            keys.push_back((static_cast<int64_t>(gx) << 32) | (static_cast<int64_t>(gy) & 0xFFFFFFFF));
        }
    }
    return keys;
}

void ComponentMap::build(const std::vector<Component>& components)
{
    m_components = components;
    m_refIndex.clear();
    m_grid.clear();

    for (size_t i = 0; i < m_components.size(); ++i) {
        const auto& comp = m_components[i];

        // Index by reference
        m_refIndex[comp.reference] = i;

        // Spatial index
        int64_t key = gridKey(comp.position.x, comp.position.y);
        m_grid[key].componentIndices.push_back(i);
    }
}

const Component* ComponentMap::findNearest(Point2D pos, double maxDistance) const
{
    const Component* best = nullptr;
    double bestDist = std::numeric_limits<double>::max();

    auto keys = neighborKeys(pos.x, pos.y, maxDistance);

    for (int64_t key : keys) {
        auto it = m_grid.find(key);
        if (it == m_grid.end()) continue;

        for (size_t idx : it->second.componentIndices) {
            const auto& comp = m_components[idx];
            double dx = comp.position.x - pos.x;
            double dy = comp.position.y - pos.y;
            double dist = std::sqrt(dx * dx + dy * dy);

            if (dist < bestDist && dist <= maxDistance) {
                bestDist = dist;
                best = &comp;
            }
        }
    }

    return best;
}

std::vector<const Component*> ComponentMap::findInRect(const BBox& rect) const
{
    std::vector<const Component*> result;

    // Iterate over grid cells that overlap the rectangle
    int32_t minGx = static_cast<int32_t>(std::floor(rect.minX / m_cellSize));
    int32_t maxGx = static_cast<int32_t>(std::floor(rect.maxX / m_cellSize));
    int32_t minGy = static_cast<int32_t>(std::floor(rect.minY / m_cellSize));
    int32_t maxGy = static_cast<int32_t>(std::floor(rect.maxY / m_cellSize));

    for (int32_t gx = minGx; gx <= maxGx; ++gx) {
        for (int32_t gy = minGy; gy <= maxGy; ++gy) {
            int64_t key = (static_cast<int64_t>(gx) << 32) | (static_cast<int64_t>(gy) & 0xFFFFFFFF);
            auto it = m_grid.find(key);
            if (it == m_grid.end()) continue;

            for (size_t idx : it->second.componentIndices) {
                const auto& comp = m_components[idx];
                if (comp.position.x >= rect.minX && comp.position.x <= rect.maxX &&
                    comp.position.y >= rect.minY && comp.position.y <= rect.maxY) {
                    result.push_back(&comp);
                }
            }
        }
    }

    return result;
}

const Component* ComponentMap::findByReference(const std::string& ref) const
{
    auto it = m_refIndex.find(ref);
    if (it != m_refIndex.end()) {
        return &m_components[it->second];
    }
    return nullptr;
}

std::vector<const Component*> ComponentMap::findByValue(const std::string& value) const
{
    std::vector<const Component*> result;
    for (const auto& comp : m_components) {
        if (comp.value == value) {
            result.push_back(&comp);
        }
    }
    return result;
}

std::vector<const Component*> ComponentMap::findByLayer(Layer layer) const
{
    std::vector<const Component*> result;
    for (const auto& comp : m_components) {
        if (comp.layer == layer) {
            result.push_back(&comp);
        }
    }
    return result;
}

} // namespace ibom
