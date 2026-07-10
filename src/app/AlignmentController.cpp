#include "AlignmentController.h"

#include <algorithm>

namespace ibom {

AlignmentController::RetryDecision AlignmentController::onInteractiveAlignFailed()
{
    RetryDecision d;
    if (m_retriesLeft <= 0) return d;  // budget exhausted → report failure
    // 1-based number of the attempt that just FAILED: with a full budget of
    // 3 and 2 retries left, attempt #1 failed (the status message shows
    // "retrying (2/3)").
    d.attempt = kInteractiveAttempts - m_retriesLeft;
    --m_retriesLeft;
    d.retry = true;
    return d;
}

void AlignmentController::onSilentResult(bool found)
{
    if (found) m_failStreak = 0;
    else       m_failStreak = std::min(m_failStreak + 1, kFailStreakCap);
}

AlignmentController::ArmDecision AlignmentController::onTrackingLost(bool hasProject)
{
    ArmDecision d;
    if (m_recoveryArmed || !hasProject) return d;  // one chain only
    m_recoveryArmed    = true;
    m_recoveryAttempts = 0;
    d.arm = true;
    return d;
}

AlignmentController::RecoveryDecision AlignmentController::onLostRecoveryTick(
    bool liveMode, bool hasProject, bool stillLost, bool busy)
{
    RecoveryDecision d;
    if (!liveMode || !hasProject || !stillLost) {
        // Recovered (or live mode ended / project unloaded) — the chain stops
        // and disarms so the NEXT loss starts a fresh one.
        m_recoveryArmed    = false;
        m_recoveryAttempts = 0;
        d.stop = true;
        return d;
    }
    if (!busy) {
        ++m_recoveryAttempts;
        d.attempt       = true;
        d.attemptNumber = m_recoveryAttempts;
        // Warn exactly once, when the fast phase gives way to the backoff:
        // the scene defeats the re-anchor (bare/sparse board, blobs can't
        // lock) — the user should align manually rather than wait.
        d.warnUser = (m_recoveryAttempts == kRecoveryFastAttempts);
    }
    // A busy tick keeps polling at the cadence of the CURRENT phase.
    d.nextDelayMs = (m_recoveryAttempts < kRecoveryFastAttempts)
                        ? kRecoveryFastDelayMs
                        : kRecoveryBackoffMs;
    return d;
}

} // namespace ibom
