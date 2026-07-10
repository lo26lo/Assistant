#include "ReanchorGate.h"

#include <algorithm>

namespace ibom::overlay {

namespace {

double maxCornerShift(const std::vector<cv::Point2f>& a,
                      const std::vector<cv::Point2f>& b)
{
    double maxShift = 0.0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        maxShift = std::max(maxShift, static_cast<double>(cv::norm(a[i] - b[i])));
    return maxShift;
}

} // namespace

ReanchorGate::Decision ReanchorGate::evaluate(
    const std::vector<cv::Point2f>& newCorners,
    const std::vector<cv::Point2f>& curCorners,
    bool trackingLost,
    std::int64_t nowMs,
    const Params& params)
{
    Decision d;

    // No current pose → nothing to protect, anchor immediately.
    if (curCorners.size() < 4 || newCorners.size() < 4) {
        reset();
        d.action = Action::Apply;
        d.reason = "no current pose";
        return d;
    }

    // Effective thresholds: physical (mm × scale, clamped) when the caller
    // provides a scale — px thresholds change meaning with magnification
    // (§1.1) — else the raw px values.
    double minShift   = params.minShiftPx;
    double confirmTol = params.confirmTolPx;
    double maxShift   = params.maxShiftPx;
    if (params.scalePxPerMm > 0.0) {
        minShift   = std::clamp(params.minShiftMm   * params.scalePxPerMm,  6.0,  60.0);
        confirmTol = std::clamp(params.confirmTolMm * params.scalePxPerMm,  4.0,  40.0);
        maxShift   = std::clamp(params.maxShiftMm   * params.scalePxPerMm, 40.0, 250.0);
    }

    d.maxShiftPx = maxCornerShift(newCorners, curCorners);

    // Drift gate: the pose is fine — and whatever disagreement a previous
    // tick held onto resolved itself (e.g. tracking caught up), so drop it.
    if (d.maxShiftPx < minShift) {
        reset();
        d.action = Action::Skip;
        d.reason = "pose within drift gate";
        return d;
    }

    // Lost recovery: the first plausible pose wins — there is no healthy
    // tracking to protect, and demanding a second tick would add a full
    // re-anchor interval to the "board picked up and put back" scenario.
    if (trackingLost) {
        reset();
        d.action = Action::Apply;
        d.reason = "tracking lost — immediate recovery";
        return d;
    }

    // Healthy-tracking cap: a huge disagreement with a pose the tracker is
    // confidently following is not drift — distrust the ESTIMATE, not the
    // pose. Crucially this also drops any pending: a systematic alias
    // (repetitive layout) reproduces identically on the next tick and would
    // otherwise sail through the two-tick confirmation (ERREUR #58).
    if (maxShift > 0.0 && d.maxShiftPx > maxShift) {
        reset();
        d.action = Action::Skip;
        d.reason = "correction exceeds healthy-tracking cap — board moves recover via Lost";
        return d;
    }

    // Two-tick confirmation: apply only when this estimate lands where the
    // previously held one did. A lone aberrant estimate arms a pending that
    // the next sane estimate fails to confirm (and replaces).
    const bool pendingFresh = m_pendingCorners.size() >= 4 &&
                              (nowMs - m_pendingMs) <= params.pendingMaxAgeMs;
    if (pendingFresh &&
        maxCornerShift(newCorners, m_pendingCorners) <= confirmTol) {
        reset();
        d.action = Action::Apply;
        d.reason = "confirmed by second concordant estimate";
        return d;
    }

    m_pendingCorners = newCorners;
    m_pendingMs = nowMs;
    d.action = Action::Hold;
    d.reason = pendingFresh ? "estimate disagrees with held candidate"
                            : "first estimate — awaiting confirmation";
    return d;
}

} // namespace ibom::overlay
