// tests/test_streakfire.cpp - streak fire (GH flame cloak) pure model
//
// The design is a SUSTAINED CLOAK on an effect shader, and StreakFireLogic.h
// carries the six-round field account of why the art object is gone. The
// cases below lock the two things that have actually broken in the field:
// the cloak must not GAP while a streak holds, and it must go out when the
// streak breaks.
#include "harness.h"
#include "game/StreakFireLogic.h"

using namespace SH::streakfire;

static void RunTests() {
    Params p;   // lights at 30, hold 1.60, refresh 0.80

    // ---- the threshold, worked from the definition --------------------
    CHECK(!LitAt(0, p));
    CHECK(!LitAt(29, p));
    CHECK(LitAt(30, p));    // inclusive
    CHECK(LitAt(400, p));

    // 0 DISABLES the feature. It cannot mean "light at a combo of zero",
    // which is what a bare threshold test would make it - the player would
    // catch light on the first note of every song.
    {
        Params off;
        off.firstAt = 0;
        CHECK(!Enabled(off));
        CHECK(!LitAt(0, off));
        CHECK(!LitAt(500, off));
        CHECK(Enabled(p));

        State s;
        CHECK(s.Step(500, 0.0, off) == Action::kNone);
        CHECK(!s.Lit());
    }

    // ---- lighting up --------------------------------------------------
    {
        State s;
        CHECK(s.Step(29, 0.0, p) == Action::kNone);
        CHECK(!s.Lit());
        CHECK(s.Step(30, 0.1, p) == Action::kApply);
        CHECK(s.Lit());
    }

    // ---- THE REFRESH CADENCE IS OWNED HERE, NOT BY THE POLL RATE ------
    // THE POINT OF THE WHOLE MODEL. The session polls at ~10Hz; that must
    // not become ten shader applications a second while a streak holds.
    {
        State s;
        CHECK(s.Step(50, 0.0, p) == Action::kApply);      // lit at t=0
        // Polled every 0.1s. Nothing until the refresh interval elapses.
        for (int i = 1; i < 8; ++i) {
            CHECK(s.Step(50, i * 0.1, p) == Action::kNone);
        }
        CHECK(s.Step(50, 0.80, p) == Action::kApply);     // exactly due
        CHECK(s.Step(50, 0.85, p) == Action::kNone);
        CHECK(s.Step(50, 1.60, p) == Action::kApply);
    }
    {
        // A poll that lands LATE still refreshes, and schedules the next one
        // from when it actually ran rather than from when it was due - or a
        // single hitch would make every later refresh late too.
        State s;
        CHECK(s.Step(50, 0.0, p) == Action::kApply);
        CHECK(s.Step(50, 5.0, p) == Action::kApply);      // very late
        CHECK(s.Step(50, 5.5, p) == Action::kNone);       // 0.8 from 5.0
        CHECK(s.Step(50, 5.8, p) == Action::kApply);
    }

    // ---- a break puts it out ------------------------------------------
    {
        State s;
        CHECK(s.Step(130, 0.0, p) == Action::kApply);
        CHECK(s.Lit());
        // Combo resets to 0 on any miss in this engine.
        CHECK(s.Step(0, 0.1, p) == Action::kNone);
        CHECK(!s.Lit());
        // No refresh while dark, however long we poll.
        CHECK(s.Step(0, 9.0, p) == Action::kNone);
        CHECK(s.Step(29, 9.1, p) == Action::kNone);
        // The next streak lights it again IMMEDIATELY on crossing - the
        // cloak is a state, not a celebration, so there is nothing to
        // re-earn and no latch to strand.
        CHECK(s.Step(30, 9.2, p) == Action::kApply);
        CHECK(s.Lit());
    }
    {
        // A fall to a value still ABOVE the threshold is NOT a break: the
        // streak is still standing, so the cloak stays lit and simply keeps
        // refreshing on its own cadence. (The burst design had to treat this
        // as a break to avoid celebrating a mistake; a sustained cloak has
        // no such problem.)
        State s;
        CHECK(s.Step(200, 0.0, p) == Action::kApply);
        CHECK(s.Step(100, 0.1, p) == Action::kNone);      // still lit, not due
        CHECK(s.Lit());
        CHECK(s.Step(100, 0.9, p) == Action::kApply);     // ordinary refresh
    }
    {
        // Two breaks in a row, nothing lit in between, must not poison the
        // state - an earlier sustained version had exactly this bug.
        State s;
        CHECK(s.Step(50, 0.0, p) == Action::kApply);
        CHECK(s.Step(0, 0.1, p) == Action::kNone);
        CHECK(s.Step(20, 0.2, p) == Action::kNone);
        CHECK(s.Step(0, 0.3, p) == Action::kNone);
        CHECK(s.Step(30, 0.4, p) == Action::kApply);
        CHECK(s.Lit());
    }

    // ---- a seek rewinds the clock -------------------------------------
    // Practice loops seek constantly. Without the rewind guard the next
    // refresh sits in a future the rewound clock must climb back to, and the
    // cloak gaps for exactly as long as the seek jumped back.
    {
        State s;
        CHECK(s.Step(50, 40.0, p) == Action::kApply);     // next due at 40.8
        CHECK(s.Step(50, 5.0, p) == Action::kApply);      // seek back to 5s
        CHECK(s.Step(50, 5.1, p) == Action::kNone);       // and back on cadence
        CHECK(s.Step(50, 5.8, p) == Action::kApply);
    }

    // A nonsense combo must not light it.
    {
        State s;
        CHECK(s.Step(-5, 0.0, p) == Action::kNone);
        CHECK(!s.Lit());
    }

    // ---- Reset --------------------------------------------------------
    {
        State s;
        CHECK(s.Step(130, 0.0, p) == Action::kApply);
        s.Reset();
        CHECK(!s.Lit());
        // Per-song, so it lights again - and on the rewound clock a fresh
        // song starts with, which Reset must not leave stale.
        CHECK(s.Step(130, 0.0, p) == Action::kApply);
    }

    // ---- the shipping defaults are sane -------------------------------
    {
        const Params d;
        CHECK(Enabled(d));
        CHECK(d.bigAt > d.firstAt);        // it must be a BUILD-UP
        // THE INVARIANT. A refresh at or past the hold duration makes a held
        // streak GAP on every cycle; that shipped once as 1.60 against 1.20.
        // The margin absorbs a poll that lands late.
        CHECK(d.refreshSec < d.holdSec);
        CHECK(d.refreshSec <= d.holdSec * 0.75);
        CHECK(d.refreshSec > 0.0);
        // Sparse enough not to hammer the engine at the ~10Hz poll rate.
        CHECK(d.refreshSec >= 0.25);
    }
}

TEST_MAIN("StreakFire")
