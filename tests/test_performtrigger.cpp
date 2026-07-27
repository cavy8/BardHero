// tests/test_performtrigger.cpp - suite 20
#include "game/BrowseCameraLogic.h"
#include "game/PerformTriggerLogic.h"

#include <cassert>
#include <cstdio>

using SH::performtrigger::Arming;
using SH::browsecamera::OpenGate;
using SH::browsecamera::PerformanceCameraKind;
using SH::browsecamera::PerformanceVanityAction;

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, \
                         #expr);                                           \
            return 1;                                                      \
        }                                                                  \
    } while (false)

int main() {
    // The hook is authoritative and lands 165-355ms ahead of the poll;
    // the poll must not fire a second start for the same arming.
    {
        Arming a;
        assert(a.NoteHook(0) == true);
        assert(a.NotePoll(0b001) == -1);
    }

    // Two AddTarget calls for one arming (re-equip spam) arm once.
    {
        Arming a;
        assert(a.NoteHook(1) == true);
        assert(a.NoteHook(1) == false);
    }

    // Belt: the poll still triggers anything the hook missed.
    {
        Arming a;
        assert(a.NotePoll(0b010) == 1);
    }

    // Songbook-open sheathe gate (field 2026-07-25: drawn weapons make
    // the graph reject the instrument idle). Raw RE::WEAPON_STATE
    // values: act on the three draw-side states only - already sheathed
    // needs nothing, and a sheathe in flight (4/5) must not be
    // re-requested or the animation can re-trigger.
    {
        using SH::performtrigger::ShouldSheatheForSongbook;
        CHECK(!ShouldSheatheForSongbook(0));  // kSheathed
        CHECK(ShouldSheatheForSongbook(1));   // kWantToDraw
        CHECK(ShouldSheatheForSongbook(2));   // kDrawing
        CHECK(ShouldSheatheForSongbook(3));   // kDrawn
        CHECK(!ShouldSheatheForSongbook(4));  // kWantToSheathe
        CHECK(!ShouldSheatheForSongbook(5));  // kSheathing
    }

    // THE REGRESSION (field 2026-07-26: still armed in the minigame after
    // opening the Songbook mid-unsheathe). The retry loop's termination
    // condition is NOT !ShouldSheatheForSongbook - that is false for 4 and
    // 5 as well as 0, so the loop stopped watching the instant a sheathe
    // merely STARTED, and an in-flight draw completing afterwards went
    // unnoticed. Only state 0 may terminate; 4/5 keep watching WITHOUT
    // re-requesting.
    {
        using SH::performtrigger::NextSheatheStep;
        using Step = SH::performtrigger::SheatheStep;
        CHECK(NextSheatheStep(0) == Step::kSettled);   // kSheathed
        CHECK(NextSheatheStep(1) == Step::kReassert);  // kWantToDraw
        CHECK(NextSheatheStep(2) == Step::kReassert);  // kDrawing
        CHECK(NextSheatheStep(3) == Step::kReassert);  // kDrawn
        CHECK(NextSheatheStep(4) == Step::kWatch);     // kWantToSheathe
        CHECK(NextSheatheStep(5) == Step::kWatch);     // kSheathing
        // Said the other way round, because this is the shape of the bug:
        // exactly one state ends the loop, and the two sheathe-side states
        // must not be mistaken for it.
        for (int ws = 1; ws <= 5; ++ws) {
            CHECK(NextSheatheStep(ws) != Step::kSettled);
        }
    }

    // A lingering ability (browse cancelled, SGT still holds it) must
    // never re-fire, however many passes go by.
    {
        Arming a;
        assert(a.NotePoll(0b001) == 0);
        assert(a.NotePoll(0b001) == -1);
        assert(a.NotePoll(0b001) == -1);
    }

    // THE PHASE 2 CASE: the ability going away clears the latch, so the
    // next equip arms again. NotePoll must ASSIGN the present mask, not
    // OR it - get this backwards and the mod triggers exactly once per
    // game session once native start starts stripping.
    {
        Arming a;
        assert(a.NoteHook(0) == true);
        assert(a.NotePoll(0b000) == -1);
        assert(a.NoteHook(0) == true);
    }

    // The session re-adds the ability itself at song pick (always-add);
    // that must not queue a phantom start when the poll resumes.
    {
        Arming a;
        a.NoteSelfAdd(2);
        assert(a.NotePoll(0b100) == -1);
    }

    // The optional Electric addon is a fourth, independent trigger bit.
    {
        Arming a;
        assert(a.NoteHook(3) == true);
        assert(a.NotePoll(0b1000) == -1);
        assert(a.NotePoll(0b0000) == -1);
        assert(a.NotePoll(0b1000) == 3);
    }

    // Deterministic pick when two appear in one pass.
    {
        Arming a;
        assert(a.NotePoll(0b110) == 1);
    }

    // Out of range is ignored rather than shifting by a bad amount.
    {
        Arming a;
        assert(a.NoteHook(-1) == false);
        assert(a.NoteHook(4) == false);
        assert(a.mask() == 0);
        a.NoteSelfAdd(-1);
        a.NoteSelfAdd(4);
        assert(a.mask() == 0);
    }

    // Songbook publication is gated behind the game-thread camera sample.
    // The session/render-thread request edge must not be visible beforehand.
    {
        OpenGate gate;
        const auto token = gate.Request();
        CHECK(!gate.PublishAllowed(token));
        const auto plan = gate.Sample(token, true, true);
        CHECK(plan.current);
        CHECK(plan.forceThirdPerson);
        CHECK(gate.Complete(token));
        CHECK(gate.PublishAllowed(token));
    }

    // An already-third-person player still opens, without a redundant force.
    {
        OpenGate gate;
        const auto token = gate.Request();
        const auto plan = gate.Sample(token, true, false);
        CHECK(plan.current);
        CHECK(!plan.forceThirdPerson);
        CHECK(gate.Complete(token));
        CHECK(gate.PublishAllowed(token));
    }

    // A load/teardown invalidates a queued game-thread task. A later sample
    // from that stale task must never resurrect the Songbook.
    {
        OpenGate gate;
        const auto stale = gate.Request();
        gate.Cancel();
        const auto plan = gate.Sample(stale, true, true);
        CHECK(!plan.current);
        CHECK(!plan.forceThirdPerson);
        CHECK(!gate.Complete(stale));
        CHECK(!gate.PublishAllowed(stale));
    }

    // Latest request wins when start input is repeated before the task runs.
    {
        OpenGate gate;
        const auto oldToken = gate.Request();
        const auto newToken = gate.Request();
        CHECK(!gate.Sample(oldToken, true, true).current);
        CHECK(gate.Sample(newToken, true, true).current);
        CHECK(gate.Complete(newToken));
        CHECK(gate.PublishAllowed(newToken));
    }

    // Performance candy uses Skyrim's native auto-vanity state, but only
    // after the Songbook's third-person preparation has settled.
    {
        using K = PerformanceCameraKind;
        using A = PerformanceVanityAction;
        CHECK(SH::browsecamera::PlanPerformanceVanity(
                  true, true, true, K::kThirdPerson, 0) ==
              A::kEnterAutoVanity);
        CHECK(SH::browsecamera::PlanPerformanceVanity(
                  true, true, true, K::kAutoVanity, 0) ==
              A::kDone);
        CHECK(SH::browsecamera::PlanPerformanceVanity(
                  true, true, true, K::kFirstPerson, 0) ==
              A::kForceThirdPerson);
        CHECK(SH::browsecamera::PlanPerformanceVanity(
                  true, true, true, K::kTransition, 0) ==
              A::kRetry);
        CHECK(SH::browsecamera::PlanPerformanceVanity(
                  true, true, true, K::kTransition, 8) ==
              A::kGiveUp);
    }

    // Disabled, stale, or ended requests must never mutate the camera.
    {
        using K = PerformanceCameraKind;
        using A = PerformanceVanityAction;
        CHECK(SH::browsecamera::PlanPerformanceVanity(
                  false, true, true, K::kThirdPerson, 0) == A::kCancel);
        CHECK(SH::browsecamera::PlanPerformanceVanity(
                  true, false, true, K::kThirdPerson, 0) == A::kCancel);
        CHECK(SH::browsecamera::PlanPerformanceVanity(
                  true, true, false, K::kThirdPerson, 0) == A::kCancel);
        CHECK(SH::browsecamera::PlanPerformanceVanity(
                  true, true, true, K::kUnavailable, 0) == A::kRetry);
    }

    std::puts("all PerformTrigger tests passed");
    return 0;
}
