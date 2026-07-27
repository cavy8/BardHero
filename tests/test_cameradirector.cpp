// tests/test_cameradirector.cpp - performance camera director (pure layer)
//
// Spec test plan:
// docs/superpowers/specs/2026-07-26-performance-camera-director-design.md
//
// The spec is explicit that "a suite authored alongside the implementation
// proves transcription consistency, not correctness. Every behaviour above
// needs at least one case whose expected value was derived independently of
// the implementation." So the weighting cases below carry their arithmetic in
// the comment, worked from the formula rather than read off a run, and the
// behavioural cases (hold, repeats, blacklist, decay) assert properties that
// hold for ANY correct implementation rather than for this one.
#include "harness.h"
#include "game/CameraDirectorLogic.h"

#include <vector>

using namespace SH::camdir;

namespace {

    // A plain trace driver: advance the song at a fixed step, with the bar
    // index derived from a bar length so the cadence is exercised honestly.
    struct Trace {
        Director d;
        double   t{ 0.0 };
        double   barSec{ 2.0 };  // 120bpm, 4/4
        float    glory{ 0.5f };
        int      streak{ 0 };
        int      section{ -1 };
        bool     clamped{ false };

        Snapshot At() const {
            Snapshot s;
            s.songSec      = t;
            s.barIndex     = static_cast<int>(t / barSec);
            s.beatPhase    = 0.0f;
            s.sectionIndex = section;
            s.glory        = glory;
            s.streak       = streak;
            s.clamped      = clamped;
            return s;
        }

        // Advance by dt, returning every cut that landed.
        std::vector<ShotId> Run(double a_seconds, double a_dt = 1.0 / 60.0) {
            std::vector<ShotId> cuts;
            const double until = t + a_seconds;
            while (t < until) {
                const auto out = d.Step(At());
                if (out.cut) { cuts.push_back(out.id); }
                t += a_dt;
            }
            return cuts;
        }
    };
}

static void RunTests() {
    // ---- weighting, worked by hand from the formulae -------------------
    //
    // EnergyTarget = clamp(0.10 + 0.70*glory + streakBonus, 0, 1).
    //   glory 0, streak 0   -> 0.10 + 0    + 0    = 0.10
    //   glory 1, streak 100 -> 0.10 + 0.70 + 0.20 = 1.00
    //   glory 0.5, streak 0 -> 0.10 + 0.35 + 0    = 0.45
    CHECK_NEAR(EnergyTarget(0.0f, 0), 0.10, 1e-6);
    CHECK_NEAR(EnergyTarget(1.0f, 100), 1.00, 1e-6);
    CHECK_NEAR(EnergyTarget(0.5f, 0), 0.45, 1e-6);
    // Out-of-range input must not escape the range.
    CHECK_NEAR(EnergyTarget(-5.0f, 0), 0.10, 1e-6);
    CHECK_NEAR(EnergyTarget(9.0f, 9999), 1.00, 1e-6);

    // ShotWeight = 1 + 3*(1 - |energy - target|).
    //   exact match     -> 1 + 3*1    = 4.00  (the peak)
    //   full mismatch   -> 1 + 3*0    = 1.00  (the FLOOR - never zero, so a
    //                                          strong run cannot lock the
    //                                          camera onto one angle)
    //   |d| = 0.25      -> 1 + 3*0.75 = 3.25
    CHECK_NEAR(ShotWeight(0.5f, 0.5f), 4.00, 1e-6);
    CHECK_NEAR(ShotWeight(0.0f, 1.0f), 1.00, 1e-6);
    CHECK_NEAR(ShotWeight(0.25f, 0.5f), 3.25, 1e-6);

    // "Glory weighting shifts the pool in the expected direction at the
    // extremes" - asserted as the ORDERING between a wide shot and a tight
    // one, which is the claim, rather than as the numbers.
    {
        const float wide  = ShotFor(ShotId::kWideStage).energy;
        const float tight = ShotFor(ShotId::kFrontLow).energy;
        CHECK(wide < tight);  // the library must actually be ordered
        const float weak   = EnergyTarget(0.0f, 0);
        const float strong = EnergyTarget(1.0f, 0);
        // A collapsing performance favours the wide, band-focused shot...
        CHECK(ShotWeight(wide, weak) > ShotWeight(tight, weak));
        // ...and a strong one favours the tight, low, urgent shot.
        CHECK(ShotWeight(tight, strong) > ShotWeight(wide, strong));
    }

    // ---- the first frame engages, and does not double-cut ---------------
    {
        Director  d;
        Snapshot  s;
        s.songSec = 0.0;
        const auto first = d.Step(s);
        CHECK(first.cut);                     // the field gate needs this line
        CHECK(first.id == kFallback);         // engage wide, then earn tighter
        CHECK(d.CutCount() == 1);
        // Same instant again: the hold has not elapsed, so nothing may cut.
        const auto again = d.Step(s);
        CHECK(!again.cut);
        CHECK(d.CutCount() == 1);
    }

    // ---- the minimum hold is never violated -----------------------------
    // Including the case the spec calls out by name: an event override
    // landing immediately after a scheduled cut.
    {
        Trace tr;
        tr.Run(30.0);
        const int before = tr.d.CutCount();
        // An SP activation on the very next frame after whatever just
        // happened must not cut, however loud the event is.
        Snapshot s     = tr.At();
        s.spJustActivated = true;
        // Walk forward in small steps and record the gap between cuts.
        double lastCut = -1.0;
        double minGap  = 1e9;
        for (int i = 0; i < 4000; ++i) {
            s          = tr.At();
            s.spJustActivated = (i % 7 == 0);   // spam the override
            s.streakMilestone = (i % 11 == 0);
            s.finalNote       = (i % 13 == 0);
            const auto out = tr.d.Step(s);
            if (out.cut) {
                if (lastCut >= 0.0) {
                    minGap = std::min(minGap, tr.t - lastCut);
                }
                lastCut = tr.t;
            }
            tr.t += 1.0 / 60.0;
        }
        CHECK(tr.d.CutCount() > before);       // the overrides DID cut
        // The invariant: no two cuts closer than the hold. One frame of
        // tolerance for the sampling step itself.
        CHECK(minGap >= kMinHoldSec - (1.0 / 60.0));
    }

    // ---- a cut never repeats the current shot ---------------------------
    {
        Trace tr;
        tr.glory = 0.7f;
        const auto cuts = tr.Run(400.0);
        CHECK(cuts.size() > 10);   // the cadence actually produced cuts
        bool repeated = false;
        for (std::size_t i = 1; i < cuts.size(); ++i) {
            if (cuts[i] == cuts[i - 1]) { repeated = true; }
        }
        CHECK(!repeated);
    }

    // ---- cut legibility: the 30-degree rule -----------------------------
    //
    // Worked by hand from the convention, not from the table: a cut must
    // move at least 30 degrees around the subject OR change the shot size
    // substantially. Both halves are needed - angle alone forbids the
    // cut-in, size alone allows a six-degree nudge.
    {
        // Wrap. 170 to -170 is 20 degrees the short way, not 340.
        CHECK_NEAR(YawSeparation(170.0f, -170.0f), 20.0, 1e-4);
        CHECK_NEAR(YawSeparation(-170.0f, 170.0f), 20.0, 1e-4);
        CHECK_NEAR(YawSeparation(0.0f, 180.0f), 180.0, 1e-4);
        CHECK_NEAR(YawSeparation(-52.0f, 52.0f), 104.0, 1e-4);
        CHECK_NEAR(YawSeparation(10.0f, 10.0f), 0.0, 1e-4);

        Shot a{ }; a.id = ShotId::kWideStage;
        Shot b{ }; b.id = ShotId::kFrontLow;
        // Same angle, same size: a jump cut, and the thing being forbidden.
        a.yawDeg = 0.0f; a.distance = 0.40f;
        b.yawDeg = 6.0f; b.distance = 0.40f;
        CHECK(!CutIsLegible(a, b));
        // Same angle, big size change: a CUT-IN, which is legible and must
        // stay allowed.
        b.distance = 0.10f;
        CHECK(CutIsLegible(a, b));
        // Big angle change, same size: also legible.
        b.yawDeg = 52.0f; b.distance = 0.40f;
        CHECK(CutIsLegible(a, b));
        // A shot is never a legible cut from itself, whatever the geometry.
        CHECK(!CutIsLegible(a, a));
    }
    {
        // The invariant that matters, over a real run: every consecutive
        // pair of cuts the director actually makes reads as a cut.
        Trace tr;
        tr.glory = 0.55f;
        std::vector<ShotId> seen;
        const double until = 900.0;
        while (tr.t < until) {
            const auto out = tr.d.Step(tr.At());
            if (out.cut) { seen.push_back(out.id); }
            tr.t += 1.0 / 60.0;
        }
        CHECK(seen.size() > 20);
        for (std::size_t i = 1; i < seen.size(); ++i) {
            CHECK(CutIsLegible(ShotFor(seen[i - 1]), ShotFor(seen[i])));
        }
    }
    {
        // Legibility is a PREFERENCE, not a hard gate: it must never be the
        // reason the camera stops cutting. With the pool thinned by a
        // blacklist, the director still has to move.
        //
        // Retire exactly ONE shot, not "clamp for a while" - a sustained
        // clamp retires EVERYTHING and then correctly goes quiet, which is a
        // different behaviour with its own test above. Getting that wrong is
        // how this case first failed.
        Trace tr;
        tr.Run(20.0);
        int guard = 0;
        while (tr.d.Current() == kFallback && guard++ < 100) {
            tr.Run(10.0);   // the fallback is unblacklistable
        }
        const ShotId victim = tr.d.Current();
        CHECK(victim != kFallback);
        tr.clamped = true;
        guard = 0;
        while (!tr.d.Blacklisted(victim) && guard++ < 100000) {
            (void)tr.d.Step(tr.At());
            tr.t += 1.0 / 60.0;
        }
        tr.clamped = false;   // one entry, then the room is clear again
        CHECK(tr.d.Blacklisted(victim));
        const int before = tr.d.CutCount();
        tr.Run(200.0);
        CHECK(tr.d.CutCount() > before);
    }

    // ---- the library actually looks at the band -------------------------
    //
    // yawDeg is the orbit offset from the PLAYER'S FACING, so 0 is behind
    // them and 180 is in front looking back at the band's faces. The first
    // library put five of six shots within 52 degrees of 0 - all behind
    // everyone - which is what "majority feel like they are from the back"
    // was describing (field 2026-07-26). Locked as an invariant so a future
    // shot cannot quietly tip it back.
    {
        int front = 0, back = 0;
        for (int i = 0; i < kShotCount; ++i) {
            if (IsFrontShot(ShotFor(static_cast<ShotId>(i)))) {
                ++front;
            } else {
                ++back;
            }
        }
        CHECK(front > back);   // front-weighted, not merely present
        CHECK(front >= 4);
        CHECK(back >= 2);      // ...but the over-shoulder look still exists
        // The classifier itself, worked by hand: 0 is behind, 180 is dead
        // in front, and the boundary is a quarter turn either way.
        Shot s{ };
        s.yawDeg = 0.0f;    CHECK(!IsFrontShot(s));
        s.yawDeg = 52.0f;   CHECK(!IsFrontShot(s));
        s.yawDeg = 180.0f;  CHECK(IsFrontShot(s));
        s.yawDeg = 145.0f;  CHECK(IsFrontShot(s));
        s.yawDeg = -145.0f; CHECK(IsFrontShot(s));
        s.yawDeg = -170.0f; CHECK(IsFrontShot(s));
        s.yawDeg = 200.0f;  CHECK(IsFrontShot(s));  // wraps to 160
        s.yawDeg = 89.0f;   CHECK(!IsFrontShot(s));
        s.yawDeg = 91.0f;   CHECK(IsFrontShot(s));
    }
    {
        // ...and a real run must actually USE the front. A library that is
        // front-weighted on paper is worth nothing if the weighting keeps
        // choosing the three shots behind the player.
        Trace tr;
        tr.glory = 0.5f;
        std::vector<ShotId> seen;
        while (tr.t < 600.0) {
            const auto out = tr.d.Step(tr.At());
            if (out.cut) { seen.push_back(out.id); }
            tr.t += 1.0 / 60.0;
        }
        CHECK(seen.size() > 15);
        int front = 0;
        for (const auto id : seen) {
            if (IsFrontShot(ShotFor(id))) { ++front; }
        }
        // Comfortably more than half the cuts, over a long run.
        CHECK(front * 2 > static_cast<int>(seen.size()));
    }

    // ---- every shot has its own lens ------------------------------------
    // The point of per-shot FOV is that two shots at a similar angle still
    // read differently, so the wide and the tight ones must not share a lens.
    {
        CHECK(ShotFor(ShotId::kWideStage).fovDelta >
              ShotFor(ShotId::kFrontLow).fovDelta);
        CHECK(ShotFor(ShotId::kWideStage).fovDelta >
              ShotFor(ShotId::kPushIn).fovDelta);
        // ...and the tight shots are on LONGER lenses (negative delta),
        // which is what compresses the stage behind the player.
        CHECK(ShotFor(ShotId::kPushIn).fovDelta < 0.0f);
        CHECK(ShotFor(ShotId::kFrontLow).fovDelta < 0.0f);
        // Bounded either way: a delta that swamps the player's own worldFOV
        // reads as a bug rather than as a lens.
        for (int i = 0; i < kShotCount; ++i) {
            const float f = ShotFor(static_cast<ShotId>(i)).fovDelta;
            CHECK(f > -20.0f && f < 20.0f);
        }
    }

    // ---- determinism: same trace, same schedule -------------------------
    {
        Trace a, b;
        a.glory = b.glory = 0.62f;
        a.streak = b.streak = 24;
        const auto ca = a.Run(300.0);
        const auto cb = b.Run(300.0);
        CHECK(ca.size() == cb.size());
        CHECK(ca == cb);
    }

    // ---- sections take precedence over the bar cadence ------------------
    {
        // A section boundary cuts even when the four-bar grid would not.
        // barSec 2.0 and kBarsPerCut 4 means the bar cadence cannot fire
        // before t = 8.0; a section edge at t = 3.0 must still cut.
        Trace tr;
        tr.Run(2.0);                       // past the minimum hold
        const int before = tr.d.CutCount();
        tr.section = 1;                    // section edge
        tr.Run(0.1);
        CHECK(tr.d.CutCount() == before + 1);
        CHECK(tr.t < 8.0);                 // ...and the bars had not come round
    }
    {
        // Absence falls back cleanly: a chart with no markers carries -1
        // forever and still cuts, on bars alone.
        Trace tr;
        tr.section = -1;
        const auto cuts = tr.Run(120.0);
        CHECK(cuts.size() > 3);
    }

    // ---- blacklisting -----------------------------------------------------
    {
        Trace tr;
        tr.Run(20.0);
        // Drive off the fallback first - the fallback is unblacklistable by
        // design, so blacklisting has nothing to prove while we sit on it.
        int guard = 0;
        while (tr.d.Current() == kFallback && guard++ < 100) {
            tr.Run(10.0);
        }
        const ShotId victim = tr.d.Current();
        CHECK(victim != kFallback);
        // Hold the clamp past the threshold: one sustained clamp, one cut,
        // one blacklist entry.
        tr.clamped = true;
        tr.Run(kClampCutSec + 0.5);
        tr.clamped = false;
        CHECK(tr.d.Blacklisted(victim));
        CHECK(tr.d.Current() != victim);
        // ...and it is gone for the REST of the song, not just this cut.
        const auto later = tr.Run(600.0);
        for (const auto id : later) { CHECK(id != victim); }
    }
    {
        // The pool never empties, and the fallback is unblacklistable.
        // Clamp continuously for a long time: every blacklistable shot goes,
        // and the director must end up parked on the fallback rather than
        // returning something invalid or spinning.
        Trace tr;
        tr.clamped = true;
        tr.Run(600.0);
        CHECK(!tr.d.Blacklisted(kFallback));
        CHECK(tr.d.Current() == kFallback);
        // Still answers, still valid.
        const auto out = tr.d.Step(tr.At());
        CHECK(static_cast<int>(out.id) >= 0);
        CHECK(static_cast<int>(out.id) < kShotCount);
        // ...and it goes QUIET. With the pool exhausted there is nothing to
        // cut to, so the director must stop claiming cuts rather than
        // reporting one to the shot it is already on - which would both
        // break the never-repeat invariant and spam the cut log once per
        // phrase for the rest of the song.
        tr.clamped = false;
        const int settled = tr.d.CutCount();
        const auto quiet  = tr.Run(300.0);
        CHECK(quiet.empty());
        CHECK(tr.d.CutCount() == settled);
    }

    // ---- shake is bounded and returns to zero ---------------------------
    {
        Director d;
        Snapshot s;
        s.glory     = 1.0f;   // worst case: amplitude scales with glory
        s.beatPhase = 0.25f;  // and the beat term is at its peak
        s.songSec   = 0.0;
        (void)d.Step(s);
        // Fire every impulse at once.
        s.spJustActivated = true;
        s.streakMilestone = true;
        s.finalNote       = true;
        (void)d.Step(s);
        s.spJustActivated = s.streakMilestone = s.finalNote = false;
        float peak = 0.0f;
        for (int i = 0; i < 600; ++i) {
            s.songSec = i / 60.0;
            s.beatPhase = static_cast<float>(i % 30) / 30.0f;
            const float sh = d.Shake(s);
            peak = std::max(peak, std::abs(sh));
            CHECK(std::abs(sh) <= kMaxShakeDeg + 1e-4f);
        }
        CHECK(peak > 0.0f);   // it actually moved
        // Ten seconds later, with the beat term nulled, the impulse has
        // decayed to nothing. exp(-10/0.28) is ~1e-16, so "zero" is fair.
        s.songSec   = 10.0;
        s.beatPhase = 0.0f;
        CHECK_NEAR(d.Shake(s), 0.0, 1e-6);
    }
    {
        // Silence is silent: no glory, no beat, no impulse ever fired.
        Director d;
        Snapshot s;
        s.glory     = 0.0f;
        s.beatPhase = 0.5f;
        (void)d.Step(s);
        CHECK_NEAR(d.Shake(s), 0.0, 1e-6);
    }

    // ---- effect timers never use 0.0 as a sentinel ----------------------
    // The convention exists because 0.0 is a REAL song time - the first
    // note - so a zero sentinel fires every effect once at song start. A
    // director that has seen no events must be still at songSec 0.
    {
        Director d;
        Snapshot s;
        s.songSec   = 0.0;
        s.glory     = 1.0f;
        s.beatPhase = 0.0f;
        const auto out = d.Step(s);
        CHECK_NEAR(d.Shake(s), 0.0, 1e-6);
        CHECK_NEAR(out.fovDelta, ShotFor(kFallback).fovDelta, 1e-6);
    }

    // ---- Star Power pushes the FOV, and lets go afterwards --------------
    {
        Director d;
        Snapshot s;
        s.songSec = 0.0;
        (void)d.Step(s);
        s.songSec         = 5.0;
        s.spJustActivated = true;
        s.spActive        = true;
        const auto kick = d.Step(s);
        s.spJustActivated = false;
        const auto held = d.Step(s);
        // Held at full while SP is up...
        CHECK(held.fovDelta >= kSpFovDelta - 1e-3f);
        CHECK(kick.fovDelta >= kSpFovDelta - 1e-3f);
        // ...and decayed away once it ends.
        s.spActive = false;
        s.songSec  = 15.0;
        const auto after = d.Step(s);
        CHECK(after.fovDelta < 0.01f + ShotFor(after.id).fovDelta);
    }

    // ---- every shot in the library is well-formed -----------------------
    // Cheap, and it is the check that catches a hand-authored table drifting
    // out of step with the enum - the same one-array contract the SFX bank
    // and the crowd bank both lock.
    for (int i = 0; i < kShotCount; ++i) {
        const auto id = static_cast<ShotId>(i);
        const auto sh = ShotFor(id);
        CHECK(sh.id == id);                       // no transcription slip
        CHECK(sh.distance > 0.0f);
        CHECK(sh.energy >= 0.0f && sh.energy <= 1.0f);
        CHECK(sh.pitchDeg > -90.0f && sh.pitchDeg < 90.0f);
        CHECK(sh.yawDeg >= -180.0f && sh.yawDeg <= 180.0f);
    }
    // No shot may be STRANDED: every one has to be reachable by a legible
    // cut from somewhere, or the weighting will keep picking it and the
    // relaxation pass will keep having to rescue it. Cheap to assert, and
    // it catches a new shot dropped in next to an existing one.
    for (int i = 0; i < kShotCount; ++i) {
        int reachable = 0;
        for (int j = 0; j < kShotCount; ++j) {
            if (i == j) { continue; }
            if (CutIsLegible(ShotFor(static_cast<ShotId>(j)),
                             ShotFor(static_cast<ShotId>(i)))) {
                ++reachable;
            }
        }
        CHECK(reachable >= 2);
    }
    // Reset returns a used director to its opening state.
    {
        Trace tr;
        tr.clamped = true;
        tr.Run(120.0);
        tr.d.Reset();
        CHECK(tr.d.CutCount() == 0);
        CHECK(tr.d.Current() == kFallback);
        for (int i = 0; i < kShotCount; ++i) {
            CHECK(!tr.d.Blacklisted(static_cast<ShotId>(i)));
        }
    }
}

TEST_MAIN("CameraDirector")
