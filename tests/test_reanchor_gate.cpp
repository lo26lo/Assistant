// ReanchorGate — the silent re-anchor decision logic (drift gate + two-tick
// confirmation + Lost bypass) extracted from Application (suites 123/126/127)
// precisely so these rules could be unit-tested. Each case mirrors a field
// scenario from docs/BLOB_REANCHOR_JITTER_ANALYSE.md.

#include <catch2/catch_test_macros.hpp>

#include "overlay/ReanchorGate.h"

#include <vector>

using ibom::overlay::ReanchorGate;
using Action = ibom::overlay::ReanchorGate::Action;

namespace {

std::vector<cv::Point2f> corners(float dx, float dy = 0.f)
{
    return { {0.f + dx, 0.f + dy}, {800.f + dx, 0.f + dy},
             {800.f + dx, 400.f + dy}, {0.f + dx, 400.f + dy} };
}

const ReanchorGate::Params kP{ /*minShiftPx*/ 12.0, /*confirmTolPx*/ 8.0,
                               /*pendingMaxAgeMs*/ 9000 };

} // namespace

TEST_CASE("no current pose applies immediately", "[reanchorgate]")
{
    ReanchorGate g;
    const auto d = g.evaluate(corners(0), {}, false, 1000, kP);
    REQUIRE(d.action == Action::Apply);
}

TEST_CASE("pose within the drift gate skips", "[reanchorgate]")
{
    ReanchorGate g;
    const auto d = g.evaluate(corners(11.f), corners(0), false, 1000, kP);
    REQUIRE(d.action == Action::Skip);
    REQUIRE(d.maxShiftPx > 10.0);
    REQUIRE(d.maxShiftPx < 12.0);
}

TEST_CASE("a drift correction needs two concordant estimates", "[reanchorgate]")
{
    ReanchorGate g;
    // First estimate 40 px away: held, never applied on its own.
    REQUIRE(g.evaluate(corners(40.f), corners(0), false, 1000, kP).action
            == Action::Hold);
    // Second estimate lands within 8 px of the held one: confirmed.
    const auto d = g.evaluate(corners(44.f), corners(0), false, 4000, kP);
    REQUIRE(d.action == Action::Apply);
}

TEST_CASE("a lone aberrant estimate can never move a healthy overlay", "[reanchorgate]")
{
    ReanchorGate g;
    // Aberrant tick (glare/hand): held.
    REQUIRE(g.evaluate(corners(60.f), corners(0), false, 1000, kP).action
            == Action::Hold);
    // Next sane estimate disagrees with it → replaces the pending, still held.
    REQUIRE(g.evaluate(corners(20.f), corners(0), false, 4000, kP).action
            == Action::Hold);
    // And the one after that confirms the sane one.
    REQUIRE(g.evaluate(corners(22.f), corners(0), false, 7000, kP).action
            == Action::Apply);
}

TEST_CASE("a stale pending never confirms", "[reanchorgate]")
{
    ReanchorGate g;
    REQUIRE(g.evaluate(corners(40.f), corners(0), false, 1000, kP).action
            == Action::Hold);
    // Same corners but far beyond pendingMaxAgeMs: held again, not applied.
    REQUIRE(g.evaluate(corners(40.f), corners(0), false, 60000, kP).action
            == Action::Hold);
    // A prompt follow-up now confirms against the refreshed pending.
    REQUIRE(g.evaluate(corners(40.f), corners(0), false, 63000, kP).action
            == Action::Apply);
}

TEST_CASE("Lost recovery bypasses confirmation", "[reanchorgate]")
{
    ReanchorGate g;
    const auto d = g.evaluate(corners(300.f), corners(0), true, 1000, kP);
    REQUIRE(d.action == Action::Apply);
}

TEST_CASE("a Skip clears the pending (disagreement resolved itself)", "[reanchorgate]")
{
    ReanchorGate g;
    REQUIRE(g.evaluate(corners(40.f), corners(0), false, 1000, kP).action
            == Action::Hold);
    // Tracking caught up: next tick agrees with the current pose → Skip…
    REQUIRE(g.evaluate(corners(2.f), corners(0), false, 4000, kP).action
            == Action::Skip);
    // …so the old pending must NOT confirm a later identical estimate.
    REQUIRE(g.evaluate(corners(40.f), corners(0), false, 7000, kP).action
            == Action::Hold);
}

TEST_CASE("reset() drops the pending (a pose was applied elsewhere)", "[reanchorgate]")
{
    ReanchorGate g;
    REQUIRE(g.evaluate(corners(40.f), corners(0), false, 1000, kP).action
            == Action::Hold);
    g.reset();  // e.g. the user ran a manual alignment in between
    REQUIRE(g.evaluate(corners(40.f), corners(0), false, 4000, kP).action
            == Action::Hold);
}

TEST_CASE("healthy-tracking cap defeats a repeatable aliased estimate", "[reanchorgate]")
{
    // ERREUR #58, the field « clack »: a perfect user-blessed pose, healthy
    // tracking — and a pad-lattice alias 185 px away that reproduces
    // IDENTICALLY on consecutive ticks. Without the cap, tick 1 arms the
    // pending and tick 2 confirms it: the overlay jumps sideways.
    ReanchorGate g;
    ReanchorGate::Params p = kP;
    p.maxShiftPx = 60.0;

    REQUIRE(g.evaluate(corners(185.f), corners(0), false, 1000, p).action
            == Action::Skip);
    // The identical alias on the next tick must NOT confirm anything.
    REQUIRE(g.evaluate(corners(185.f), corners(0), false, 4000, p).action
            == Action::Skip);

    // Lost recovery is exempt: huge shifts are the point there.
    REQUIRE(g.evaluate(corners(185.f), corners(0), true, 7000, p).action
            == Action::Apply);

    // An in-cap real drift still corrects through the two-tick confirmation.
    REQUIRE(g.evaluate(corners(40.f), corners(0), false, 10000, p).action
            == Action::Hold);
    REQUIRE(g.evaluate(corners(41.f), corners(0), false, 13000, p).action
            == Action::Apply);
}
