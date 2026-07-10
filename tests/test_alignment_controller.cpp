// AlignmentController — the pure orchestration state machine extracted from
// Application (INVESTIGATION_360 §7.1, step 2 after ReanchorGate): interactive
// Auto-Align retry budget, silent-failure streak, and the Lost-recovery chain
// with its one-shot warning and backoff. Behaviour must match the field-tuned
// inline logic it replaces (ERREUR #54, suite 127) exactly.

#include <catch2/catch_test_macros.hpp>

#include "app/AlignmentController.h"

using ibom::AlignmentController;

TEST_CASE("interactive align: 3 attempts per click, then give up", "[alignctl]")
{
    AlignmentController c;

    // Failing before any click: no budget, no retry.
    REQUIRE_FALSE(c.onInteractiveAlignFailed().retry);

    c.beginInteractiveAlign();
    const auto r1 = c.onInteractiveAlignFailed();
    REQUIRE(r1.retry);
    REQUIRE(r1.attempt == 1);
    REQUIRE(r1.delayMs == AlignmentController::kRetryDelayMs);
    const auto r2 = c.onInteractiveAlignFailed();
    REQUIRE(r2.retry);
    REQUIRE(r2.attempt == 2);
    // Third failure exhausts the budget of 3 attempts.
    REQUIRE_FALSE(c.onInteractiveAlignFailed().retry);

    // A new click re-arms the budget in full.
    c.beginInteractiveAlign();
    REQUIRE(c.onInteractiveAlignFailed().retry);
}

TEST_CASE("silent failure streak counts up, caps, and resets on success", "[alignctl]")
{
    AlignmentController c;
    for (int i = 0; i < 30; ++i) c.onSilentResult(false);
    REQUIRE(c.failStreak() == AlignmentController::kFailStreakCap);
    c.onSilentResult(true);
    REQUIRE(c.failStreak() == 0);
}

TEST_CASE("lost recovery: one chain, fast phase, one warning, backoff", "[alignctl]")
{
    AlignmentController c;

    // Arms once per loss — repeated Lost signals must not spawn chains.
    REQUIRE(c.onTrackingLost(/*hasProject=*/true).arm);
    REQUIRE_FALSE(c.onTrackingLost(true).arm);

    // Fast phase: attempts 1-5 at the fast cadence, no warning.
    for (int i = 1; i <= 5; ++i) {
        const auto d = c.onLostRecoveryTick(true, true, /*stillLost=*/true,
                                            /*busy=*/false);
        REQUIRE(d.attempt);
        REQUIRE(d.attemptNumber == i);
        REQUIRE_FALSE(d.warnUser);
        REQUIRE(d.nextDelayMs == AlignmentController::kRecoveryFastDelayMs);
    }
    // Attempt 6: warn the user exactly once, enter backoff.
    const auto d6 = c.onLostRecoveryTick(true, true, true, false);
    REQUIRE(d6.attempt);
    REQUIRE(d6.warnUser);
    REQUIRE(d6.nextDelayMs == AlignmentController::kRecoveryBackoffMs);
    // Attempt 7+: keep trying at the backoff cadence, never warn again.
    const auto d7 = c.onLostRecoveryTick(true, true, true, false);
    REQUIRE(d7.attempt);
    REQUIRE_FALSE(d7.warnUser);
    REQUIRE(d7.nextDelayMs == AlignmentController::kRecoveryBackoffMs);
}

TEST_CASE("lost recovery: busy ticks poll without consuming attempts", "[alignctl]")
{
    AlignmentController c;
    REQUIRE(c.onTrackingLost(true).arm);
    const auto d = c.onLostRecoveryTick(true, true, true, /*busy=*/true);
    REQUIRE_FALSE(d.attempt);
    REQUIRE_FALSE(d.stop);
    REQUIRE(d.nextDelayMs == AlignmentController::kRecoveryFastDelayMs);
    // The skipped tick did not burn an attempt number.
    REQUIRE(c.onLostRecoveryTick(true, true, true, false).attemptNumber == 1);
}

TEST_CASE("lost recovery: stops and disarms on recovery, re-arms fresh", "[alignctl]")
{
    AlignmentController c;
    REQUIRE(c.onTrackingLost(true).arm);
    (void)c.onLostRecoveryTick(true, true, true, false);
    (void)c.onLostRecoveryTick(true, true, true, false);

    SECTION("tracking recovered") {
        REQUIRE(c.onLostRecoveryTick(true, true, /*stillLost=*/false, false).stop);
    }
    SECTION("live mode ended") {
        REQUIRE(c.onLostRecoveryTick(/*liveMode=*/false, true, true, false).stop);
    }
    SECTION("project unloaded") {
        REQUIRE(c.onLostRecoveryTick(true, /*hasProject=*/false, true, false).stop);
    }
    // After any stop, the next loss starts a FRESH chain from attempt 1.
    (void)c.onLostRecoveryTick(true, true, false, false);
    REQUIRE(c.onTrackingLost(true).arm);
    REQUIRE(c.onLostRecoveryTick(true, true, true, false).attemptNumber == 1);
}

TEST_CASE("lost recovery: no project, no chain", "[alignctl]")
{
    AlignmentController c;
    REQUIRE_FALSE(c.onTrackingLost(/*hasProject=*/false).arm);
}
