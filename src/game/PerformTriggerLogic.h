// src/game/PerformTriggerLogic.h
#pragma once

#include <cstdint>

// PURE decision layer for the SGT perform trigger (no RE/OS includes -
// suite 20). Three concerns live here, in file order: arming arbitration
// between the hook and the poll, what a trigger should DO once armed
// (DecideTrigger, incl. the duet handoff), and the browse-sheathe state
// machine. Specs: docs/specs/2026-07-20-native-perform-start.md and
// docs/superpowers/specs/2026-07-29-sgt-coexistence-and-doom-lute-
// description-design.md.
//
// Two sources report the same arming:
//   - MagicTarget::AddTarget hook - synchronous, authoritative, field-
//     measured 165-355ms ahead of the poll
//   - the 500ms HasSpell poll - kept as a belt for anything the hook misses
// Exactly one may produce a start request per arming, so the latch lives
// here rather than being smeared across Session.cpp.
//
// The latch is a PRESENT-MASK, not a one-shot: an ability that goes away
// (the Phase 2 native-start strip, SGT's own exit, a save load) clears its
// bit so the next equip arms again. That is why NotePoll ASSIGNS the mask
// instead of OR-ing it - OR-ing would make the mod trigger exactly once
// per game session as soon as native start begins stripping.
//
// Game thread only (both callers run there), so no synchronisation.
namespace SH::performtrigger {

    class Arming {
    public:
        // The hook saw instrument a_inst added. True when THIS call is the
        // one that arms it - the caller then requests the session start.
        // False when it was already armed (re-equip spam, or the poll got
        // there first).
        bool NoteHook(int a_inst) {
            if (a_inst < 0 || a_inst > 3) { return false; }
            const int bit = 1 << a_inst;
            if (armed_ & bit) { return false; }
            armed_ |= bit;
            return true;
        }

        // One poll pass. a_presentMask carries bit i per instrument
        // currently on the player. Returns the instrument to arm, or -1.
        // Also clears the latch for abilities that have gone away.
        int NotePoll(int a_presentMask) {
            const int fresh = a_presentMask & ~armed_;
            armed_          = a_presentMask;
            if (!fresh) { return -1; }
            for (int i = 0; i < 4; ++i) {
                if (fresh & (1 << i)) { return i; }
            }
            return -1;
        }

        // The session re-adds the ability itself at song pick (always-add).
        // Latch it so the poll's absent->present edge cannot queue a
        // phantom start when the session ends.
        void NoteSelfAdd(int a_inst) {
            if (a_inst < 0 || a_inst > 3) { return; }
            armed_ |= 1 << a_inst;
        }

        [[nodiscard]] int mask() const { return armed_; }

    private:
        int armed_ = 0;
    };

    // What a perform trigger should do, decided in one place so the
    // AddTarget hook and FirePerformTrigger cannot disagree.
    //
    // kStandDownForDuet exists because SGT's follower duet is started from
    // _Talent_PlayInstrument.OnEffectStart, which native start prevents from
    // running at all. The dialogue option is a separate record we never
    // touch, so before this the option appeared and then did nothing (Nexus
    // field report, 1.1).
    enum class TriggerAction : std::uint8_t {
        kIgnore,            // not our instrument; the hook never got here
        kStandDownForDuet,  // SGT plays this one, whole performance
        kRun,               // BardHero opens the songbook
    };

    // a_handled     - the instrument resolved to a trigger spell
    // a_passthrough - [SGT] bDuetPassthrough
    // a_duetPending - SGT's _Talent_FollowerPlays global reads nonzero
    [[nodiscard]] constexpr TriggerAction DecideTrigger(
        bool a_handled, bool a_passthrough, bool a_duetPending) noexcept {
        if (!a_handled) { return TriggerAction::kIgnore; }
        if (a_passthrough && a_duetPending) {
            return TriggerAction::kStandDownForDuet;
        }
        return TriggerAction::kRun;
    }

    // The perform idles (instrument ANIO clips and the electric body
    // clip) require a sheathed actor: with a weapon or magic drawn the
    // behavior graph rejects the idle and the instrument never appears
    // (user field report 2026-07-25). The host sheathes as the Songbook
    // opens, so the ~1s draw-down runs during browsing instead of against
    // the perform start. Takes the raw RE::WEAPON_STATE value (0 sheathed
    // .. 5 sheathing) so this header stays RE-free; act only on the three
    // draw-side states - a sheathe already in flight needs no second
    // request, and re-requesting one can re-trigger the animation.
    [[nodiscard]] constexpr bool ShouldSheatheForSongbook(
        int a_weaponState) noexcept {
        return a_weaponState >= 1 && a_weaponState <= 3;
    }

    // Raw RE::WEAPON_STATE::kSheathed. The ONLY state that means the
    // sheathe actually landed.
    inline constexpr int kWeaponSheathed = 0;

    // One pass of the browse-sheathe retry loop.
    //
    // The predicate above answers "should I ask for a sheathe?"; it does
    // NOT answer "am I done watching?", and conflating the two is what let
    // the first fix miss (field 2026-07-26, still armed in the minigame).
    // !ShouldSheatheForSongbook is true for 4/5 as well as 0, so a loop
    // that stopped there stopped the instant a sheathe merely STARTED -
    // precisely the moment the race with an in-flight draw is still live.
    // If that draw then wins and the state returns to 3, nothing is left
    // watching. Hence three outcomes, not two: only 0 terminates.
    enum class SheatheStep {
        kSettled,   // 0 - sheathed; stop
        kReassert,  // 1-3 - draw side; request a sheathe, keep watching
        kWatch,     // 4/5 - sheathe in flight; do NOT re-request, keep watching
    };

    [[nodiscard]] constexpr SheatheStep NextSheatheStep(
        int a_weaponState) noexcept {
        if (a_weaponState == kWeaponSheathed) { return SheatheStep::kSettled; }
        return ShouldSheatheForSongbook(a_weaponState)
            ? SheatheStep::kReassert
            : SheatheStep::kWatch;
    }
}
