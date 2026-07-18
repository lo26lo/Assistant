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
        step.position   = comp.position;
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
    // Raster order over the real PCB coordinates: top→bottom, left→right.
    // Sorting "by original load order" here was a no-op — loadComponents()
    // runs sortByValueGroup() which renumbers `order`, so the load order is
    // gone by the time a user sort is applied (audit B1, BUG_RESEARCH
    // 2026-07-18).
    std::stable_sort(m_steps.begin(), m_steps.end(), [](const auto& a, const auto& b) {
        if (a.position.y != b.position.y) return a.position.y < b.position.y;
        return a.position.x < b.position.x;
    });

    for (int i = 0; i < static_cast<int>(m_steps.size()); ++i) {
        m_steps[i].order = i;
    }
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

void PickAndPlace::sortByNearestNeighbor()
{
    const int n = static_cast<int>(m_steps.size());
    if (n < 3) return;

    // Greedy nearest-neighbor walk. O(n²) — fine for BOM sizes (even 5000
    // parts is ~25M cheap distance evaluations, done once per inspection).
    auto dist2 = [](const PlacementStep& a, const PlacementStep& b) {
        const double dx = a.position.x - b.position.x;
        const double dy = a.position.y - b.position.y;
        return dx * dx + dy * dy;
    };

    // Start at the top-left-most component (min x+y) — a stable, intuitive
    // route entry point.
    int start = 0;
    for (int i = 1; i < n; ++i)
        if (m_steps[i].position.x + m_steps[i].position.y
            < m_steps[start].position.x + m_steps[start].position.y)
            start = i;

    std::vector<PlacementStep> route;
    route.reserve(n);
    std::vector<bool> used(n, false);
    int cur = start;
    used[cur] = true;
    route.push_back(m_steps[cur]);
    for (int k = 1; k < n; ++k) {
        int best = -1;
        double bestD = 0.0;
        for (int i = 0; i < n; ++i) {
            if (used[i]) continue;
            const double d = dist2(m_steps[cur], m_steps[i]);
            if (best < 0 || d < bestD) { best = i; bestD = d; }
        }
        used[best] = true;
        route.push_back(m_steps[best]);
        cur = best;
    }
    m_steps = std::move(route);

    for (int i = 0; i < n; ++i)
        m_steps[i].order = i;
}

bool PickAndPlace::unplace(const std::string& reference)
{
    for (int i = 0; i < static_cast<int>(m_steps.size()); ++i) {
        if (m_steps[i].reference != reference) continue;
        if (!m_steps[i].placed) return false;
        m_steps[i].placed = false;
        m_currentIndex = i;
        spdlog::info("PickAndPlace: unplaced {} (undo)", reference);
        emit currentStepChanged(m_steps[i]);
        emitProgress();
        return true;
    }
    return false;
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
    // Placing the LAST step while earlier steps were skipped used to dead-end:
    // no signal fired and P/N went inert (audit B10). Wrap to the first
    // remaining unplaced step so the tour keeps cycling over the leftovers.
    const int n = static_cast<int>(m_steps.size());
    if (m_currentIndex >= n && !isComplete()) {
        for (int i = 0; i < n; ++i) {
            if (!m_steps[i].placed) { m_currentIndex = i; break; }
        }
    }
    emitProgress();

    if (isComplete()) {
        emit allPlaced();
    } else if (m_currentIndex < n) {
        emit currentStepChanged(m_steps[m_currentIndex]);
    }
}

void PickAndPlace::skip()
{
    const int n = static_cast<int>(m_steps.size());
    if (m_currentIndex < n - 1) {
        m_currentIndex++;
        emit currentStepChanged(m_steps[m_currentIndex]);
        return;
    }
    // Skipping the last step: wrap to the first unplaced step, if any, so the
    // remaining skipped work stays reachable with N alone (audit B10).
    for (int i = 0; i < n; ++i) {
        if (!m_steps[i].placed && i != m_currentIndex) {
            m_currentIndex = i;
            emit currentStepChanged(m_steps[i]);
            return;
        }
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

int PickAndPlace::restorePlaced(const std::unordered_set<std::string>& placedRefs)
{
    int restored = 0;
    for (auto& step : m_steps) {
        step.placed = placedRefs.count(step.reference) > 0;
        if (step.placed) restored++;
    }

    // Position on the first unplaced step.
    m_currentIndex = 0;
    while (m_currentIndex < static_cast<int>(m_steps.size()) &&
           m_steps[m_currentIndex].placed) {
        m_currentIndex++;
    }

    spdlog::info("PickAndPlace: restored {} placed of {} steps", restored, m_steps.size());
    emitProgress();
    if (isComplete()) {
        emit allPlaced();
    } else if (m_currentIndex < static_cast<int>(m_steps.size())) {
        emit currentStepChanged(m_steps[m_currentIndex]);
    }
    return restored;
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
