#pragma once

namespace ibom {

/**
 * @brief Pure orchestration state machine for the alignment subsystem —
 *        step 2 of the AlignmentController extraction
 *        (docs/INVESTIGATION_360_2026-07.md §7.1, after ReanchorGate in
 *        suite 133).
 *
 * Application keeps the Qt plumbing (QTimer, QFutureWatcher, signals, status
 * messages) and delegates every scheduling / retry / backoff DECISION here:
 *
 *  - the interactive Auto-Align retry budget (a single badly-timed frame —
 *    blur, glare, a hand — must not fail the whole click: 3 attempts,
 *    300 ms apart on fresh frames);
 *  - the silent re-anchor failure streak (diagnostic counter, capped);
 *  - the Lost-recovery chain (armed once per loss, first poll after 800 ms,
 *    then every 3 s; after 6 failed attempts warn the user exactly once and
 *    back off to 15 s so neither log nor CPU is hammered — field tuning from
 *    ERREUR #54 and suite 127).
 *
 * No Qt, no OpenCV, no clock: callers pass state in, decisions come out —
 * which is what makes the most frequently re-tuned logic of the app
 * unit-testable (§8.2).
 */
class AlignmentController {
public:
    // ── Interactive Auto-Align retry budget ─────────────────────────────
    static constexpr int kInteractiveAttempts = 3;    ///< total tries per click
    static constexpr int kRetryDelayMs        = 300;  ///< fresh-frame delay

    /// The user clicked Auto-Align (NOT called on internal retries).
    void beginInteractiveAlign() { m_retriesLeft = kInteractiveAttempts - 1; }

    struct RetryDecision {
        bool retry   = false;          ///< re-dispatch on a fresh frame
        int  attempt = 0;              ///< 1-based FAILED attempt (for the msg)
        int  delayMs = kRetryDelayMs;
    };
    /// A non-silent Auto-Align came back not-found: retry or give up?
    RetryDecision onInteractiveAlignFailed();

    // ── Silent re-anchor failure streak ─────────────────────────────────
    static constexpr int kFailStreakCap = 20;
    void onSilentResult(bool found);
    int  failStreak() const { return m_failStreak; }

    // ── Lost-recovery chain ─────────────────────────────────────────────
    static constexpr int kRecoveryFastAttempts = 6;      ///< before backoff
    static constexpr int kRecoveryFirstDelayMs = 800;    ///< worker may self-heal
    static constexpr int kRecoveryFastDelayMs  = 3000;
    static constexpr int kRecoveryBackoffMs    = 15000;

    struct ArmDecision {
        bool arm     = false;                    ///< start polling
        int  delayMs = kRecoveryFirstDelayMs;
    };
    /// Tracking transitioned to Lost. Arms ONE chain regardless of how often
    /// Lost fires; a chain needs a loaded project to have anything to align.
    ArmDecision onTrackingLost(bool hasProject);

    struct RecoveryDecision {
        bool stop          = false;  ///< chain ends (recovered / live off / no project)
        bool attempt       = false;  ///< fire a silent component re-anchor now
        bool warnUser      = false;  ///< exactly once, when entering backoff
        int  attemptNumber = 0;      ///< 1-based, for the log line
        int  nextDelayMs   = kRecoveryFastDelayMs;
    };
    /// Recovery poll fired. `busy` = another alignment is in flight (skip the
    /// attempt but keep polling); `stillLost`/`liveMode`/`hasProject` gone →
    /// the chain stops and disarms (a future loss re-arms it fresh).
    RecoveryDecision onLostRecoveryTick(bool liveMode, bool hasProject,
                                        bool stillLost, bool busy);

private:
    int  m_retriesLeft      = 0;
    int  m_failStreak       = 0;
    bool m_recoveryArmed    = false;
    int  m_recoveryAttempts = 0;
};

} // namespace ibom
