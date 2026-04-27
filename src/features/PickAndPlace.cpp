#include "PickAndPlace.h"
#include "../ibom/IBomData.h"

#include <algorithm>
#include <unordered_map>
#include <spdlog/spdlog.h>

namespace ibom::features {

PickAndPlace::PickAndPlace(QObject* parent)
    : QObject(parent)
{
}

void PickAndPlace::loadComponents(const std::vector<Component>& components)
{
    m_steps.clear();
    m_currentIndex = 0;

    int order = 0;
    for (const auto& comp : components) {
        PlacementStep step;
        step.reference  = comp.reference;
        step.value      = comp.value;
        step.footprint  = comp.footprint;
        step.layer      = comp.layer;
        step.placed     = false;
        step.order      = order++;
        m_steps.push_back(std::move(step));
    }

    spdlog::info("PickAndPlace: loaded {} components", m_steps.size());
    sortByValueGroup();

    if (!m_steps.empty()) {
        emit currentStepChanged(m_steps[0]);
    }
    emitProgress();
}

void PickAndPlace::sortByValueGroup()
{
    std::stable_sort(m_steps.begin(), m_steps.end(), [](const auto& a, const auto& b) {
        // Group by value, then by reference within group
        if (a.value != b.value) return a.value < b.value;
        return a.reference < b.reference;
    });

    for (int i = 0; i < static_cast<int>(m_steps.size()); ++i) {
        m_steps[i].order = i;
    }
}

void PickAndPlace::sortByValueGroupCount()
{
    // Count how many components share each value
    std::unordered_map<std::string, int> counts;
    for (const auto& s : m_steps) counts[s.value]++;

    std::stable_sort(m_steps.begin(), m_steps.end(),
        [&counts](const auto& a, const auto& b) {
            int ca = counts[a.value];
            int cb = counts[b.value];
            if (ca != cb)              return ca > cb;          // bigger group first
            if (a.value != b.value)    return a.value < b.value;
            return a.reference < b.reference;
        });

    for (int i = 0; i < static_cast<int>(m_steps.size()); ++i)
        m_steps[i].order = i;
}

void PickAndPlace::sortByPosition()
{
    // Already loaded from iBOM data which has position info —
    // we sort by order (original load order corresponds to spatial layout for many boards)
    std::stable_sort(m_steps.begin(), m_steps.end(), [](const auto& a, const auto& b) {
        return a.order < b.order;
    });
}

void PickAndPlace::sortByFootprintSize()
{
    // Heuristic: shorter footprint name = smaller package (rough approximation)
    std::stable_sort(m_steps.begin(), m_steps.end(), [](const auto& a, const auto& b) {
        if (a.footprint.size() != b.footprint.size())
            return a.footprint.size() < b.footprint.size();
        return a.footprint < b.footprint;
    });

    for (int i = 0; i < static_cast<int>(m_steps.size()); ++i) {
        m_steps[i].order = i;
    }
}

const PickAndPlace::PlacementStep& PickAndPlace::currentStep() const
{
    static PlacementStep empty;
    if (m_steps.empty() || m_currentIndex >= static_cast<int>(m_steps.size()))
        return empty;
    return m_steps[m_currentIndex];
}

void PickAndPlace::markPlaced()
{
    if (m_currentIndex >= static_cast<int>(m_steps.size())) return;

    m_steps[m_currentIndex].placed = true;
    std::string ref = m_steps[m_currentIndex].reference;
    spdlog::info("PickAndPlace: placed {}", ref);
    emit stepPlaced(ref);

    m_currentIndex++;
    emitProgress();

    if (isComplete()) {
        emit allPlaced();
    } else if (m_currentIndex < static_cast<int>(m_steps.size())) {
        emit currentStepChanged(m_steps[m_currentIndex]);
    }
}

void PickAndPlace::skip()
{
    if (m_currentIndex < static_cast<int>(m_steps.size()) - 1) {
        m_currentIndex++;
        emit currentStepChanged(m_steps[m_currentIndex]);
    }
}

void PickAndPlace::goBack()
{
    if (m_currentIndex > 0) {
        m_currentIndex--;
        emit currentStepChanged(m_steps[m_currentIndex]);
    }
}

void PickAndPlace::reset()
{
    for (auto& step : m_steps) {
        step.placed = false;
    }
    m_currentIndex = 0;
    if (!m_steps.empty()) {
        emit currentStepChanged(m_steps[0]);
    }
    emitProgress();
}

int PickAndPlace::placedCount() const
{
    return static_cast<int>(
        std::count_if(m_steps.begin(), m_steps.end(),
                      [](const auto& s) { return s.placed; }));
}

bool PickAndPlace::isComplete() const
{
    return std::all_of(m_steps.begin(), m_steps.end(),
                       [](const auto& s) { return s.placed; });
}

void PickAndPlace::emitProgress()
{
    emit progressChanged(placedCount(), static_cast<int>(m_steps.size()));
}

} // namespace ibom::features
