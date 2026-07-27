// src/game/SgtStartLead.h
#pragma once

#include <algorithm>

// PURE sizing of the audio lead-in against SGT's own start wind-up (no
// RE/OS includes - suite 21).
//
// THE DEFECT THIS EXISTS FOR (field 2026-07-20). Native start (Phase 2)
// suppresses SGT's OnEffectStart during BROWSE, so its whole start flow
// now runs at SONG PICK instead. Our session meanwhile begins its audio
// and chart the instant StartSession returns. With a weapon drawn the
// player was still sheathing and drawing the lute while notes were
// already scrolling.
//
// The costs below are read off SGT's `_Talent_PlayInstrument.psc`, from
// the pick-time AddSpell to `PlayIdle(IdleToPlay)` - the moment the
// character is visibly playing:
//
//   OnEffectStart  ... SheatheWeapon() + Utility.Wait(2)   only if drawn
//                      While IsInMenuMode / Utility.Wait(0.3)
//                      ForceThirdPerson + Utility.Wait(0.1)
//   PlayMusic      ... Utility.Wait(0.1)
//                      PlayIdle(IdleToPlay)                <- ready here
//
// ⚠ DO NOT gate the countdown on SGT's `SongToPlay` variable, which an
// earlier design proposed as the readiness signal. Reading the psc,
// `SongToPlay` is assigned by the `<tier>Songs.Play(PlayerRef)` call that
// sits AFTER `PlayIdle` and after a further `Utility.Wait(0.9)` - it
// fills roughly a second LATE, so gating on it would leave the character
// strumming in silence. `PlayIdle` is the readiness moment, and its
// arrival is a fixed script cost, which is why this is arithmetic rather
// than a poll.
namespace SH::sgtlead {

    // psc-derived wind-up costs, seconds.
    inline constexpr double kMenuWaitSec         = 0.3;
    inline constexpr double kForceThirdPersonSec = 0.1;
    inline constexpr double kPlayMusicWaitSec    = 0.1;

    // The branch Phase 3's browse sheathe exists to remove: SGT only pays
    // it when IsWeaponDrawn() is true as it reaches psc line 133.
    inline constexpr double kSheatheBranchSec = 2.0;

    // Headroom over the estimate. Papyrus `Utility.Wait` resolves on VM
    // cadence and always overshoots slightly, so the estimate is a floor.
    inline constexpr double kReadyMarginSec = 0.25;

    // Seconds from the pick-time AddSpell to the character visibly
    // playing.
    [[nodiscard]] inline double WindUpSeconds(bool a_weaponDrawn) {
        const double base =
            kMenuWaitSec + kForceThirdPersonSec + kPlayMusicWaitSec;
        return a_weaponDrawn ? base + kSheatheBranchSec : base;
    }

    // The lead-in to hand AudioEngine::ScheduleStart. Never shorter than
    // the caller's base (a longer configured lead-in wins) and never
    // shorter than the wind-up plus margin.
    //
    // With Phase 3's browse sheathe working, a_weaponDrawn is false and
    // this returns a_baseLead unchanged - the sheathed path must not gain
    // a needless delay. The drawn case is the belt for a sheathe that did
    // not take (combat auto-redraw): we know SGT will pay the 2s branch,
    // so the lead-in absorbs it up front. No readiness poll and no
    // re-scheduling of already-committed audio.
    [[nodiscard]] inline double LeadSeconds(double a_baseLead,
                                            bool   a_weaponDrawn) {
        return (std::max)(a_baseLead,
                          WindUpSeconds(a_weaponDrawn) + kReadyMarginSec);
    }
}
