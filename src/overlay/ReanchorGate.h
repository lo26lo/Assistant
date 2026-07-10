#pragma once

#include <opencv2/core.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace ibom::overlay {

/**
 * @brief Decision logic for SILENT re-anchor corrections (periodic drift
 *        correction + LOST recovery), extracted from Application so the most
 *        delicate alignment logic in the app is unit-testable
 *        (docs/INVESTIGATION_360_2026-07.md §7.1/§8.2; the rules themselves
 *        come from suites 123/126/127 — see BLOB_REANCHOR_JITTER_ANALYSE.md).
 *
 * Pure state machine over board-corner positions: no Qt, no timers, no
 * camera. The caller projects the board bbox corners under the CURRENT pose
 * and under the NEW (candidate) pose, and this class answers what to do:
 *
 *  - Skip:  the new pose agrees with the current one (shift < drift gate) —
 *           healthy tracking must not be disturbed (a re-anchor resets the
 *           tracking reference and would stutter). Any pending confirmation
 *           is dropped: the disagreement resolved itself.
 *  - Hold:  the shift is worth correcting, but a single estimate is not
 *           trusted (one aberrant tick — hand in view, glare — must never
 *           yank a healthy overlay): remember these corners and wait for the
 *           NEXT estimate to land in the same place.
 *  - Apply: correction confirmed (two consecutive concordant estimates), or
 *           there is no current pose to protect, or tracking is Lost
 *           (recovery wants the first plausible pose immediately).
 */
class ReanchorGate {
public:
    struct Params {
        /// Drift gate: below this max corner shift (px), the pose is fine.
        double minShiftPx = 12.0;
        /// Two consecutive estimates agreeing within this (px) confirm.
        double confirmTolPx = 8.0;
        /// A pending confirmation older than this is stale — a fresh estimate
        /// must not be confirmed against minutes-old corners. Callers set it
        /// from the re-anchor interval (e.g. 3×).
        std::int64_t pendingMaxAgeMs = 9000;
        /// Cap on a SILENT correction while tracking is healthy: a shift this
        /// large is not drift — it is either a systematically aliased estimate
        /// (repetitive pad lattice, ERREUR #58) or a real board move, and
        /// board moves recover via the Lost bypass. Without the cap a
        /// REPEATABLE alias defeats the two-tick confirmation (same wrong
        /// pose two ticks in a row → confirmed → a perfect user-blessed pose
        /// yanked 185 px sideways: the field « clack »). 0 disables.
        double maxShiftPx = 0.0;

        /// ── Physical thresholds (INVESTIGATION_360 §1.1) ──
        /// When scalePxPerMm > 0 the three px thresholds above are DERIVED
        /// from these instead (clamped to sane px windows). Pixel thresholds
        /// change meaning with magnification — 12 px is 2.7 mm at a D405 wide
        /// view yet 0.24 mm under a microscope (hyper-sensitive gate);
        /// millimetres don't. Defaults match the historical px behaviour at
        /// the D405 field scale (~4.4 px/mm).
        double scalePxPerMm = 0.0;
        double minShiftMm   = 2.5;   ///< drift gate     → clamp(×s,  6,  60) px
        double confirmTolMm = 1.5;   ///< confirmation   → clamp(×s,  4,  40) px
        double maxShiftMm   = 12.0;  ///< healthy cap    → clamp(×s, 40, 250) px
    };

    enum class Action { Skip, Hold, Apply };

    struct Decision {
        Action action = Action::Apply;
        double maxShiftPx = 0.0;   ///< measured max corner shift vs current pose
        std::string reason;        ///< short log-friendly explanation
    };

    /// Evaluate a new silent pose.
    /// @param newCorners  Board corners under the CANDIDATE pose (size 4).
    /// @param curCorners  Board corners under the CURRENT pose; empty = no
    ///                    valid current pose → Apply (nothing to protect).
    /// @param trackingLost  Live tracking is Lost → bypass confirmation.
    /// @param nowMs       Monotonic milliseconds (any epoch, caller-consistent).
    Decision evaluate(const std::vector<cv::Point2f>& newCorners,
                      const std::vector<cv::Point2f>& curCorners,
                      bool trackingLost,
                      std::int64_t nowMs,
                      const Params& params);

    /// Forget any pending confirmation. Call whenever a pose is APPLIED by
    /// any path (interactive alignment, recovery, confirmed correction) —
    /// the pending candidate belongs to a superseded world.
    void reset() { m_pendingCorners.clear(); m_pendingMs = 0; }

private:
    std::vector<cv::Point2f> m_pendingCorners;
    std::int64_t             m_pendingMs = 0;
};

} // namespace ibom::overlay
