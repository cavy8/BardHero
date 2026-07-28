// src/game/SgtVm.h
#pragma once

#include <cstdint>

// No sibling header in this repo that needs an RE:: type in its own public
// interface relies on the plugin PCH for it (see Ducking.h) - forward
// declare the FormID alias here so this header still stands alone for
// tooling that doesn't force-include PCH.h. FormID is just RE's uint32_t
// handle typedef (RE/B/BSCoreTypes.h); redeclaring a `using` alias to the
// exact same type it already denotes is not an ODR conflict.
namespace RE {
    using FormID = std::uint32_t;
}

namespace SH::SgtVm {
    // Game-thread-only Papyrus interop with SGT's _Talent_PlayInstrument
    // active-effect script (loose .psc read 2026-07-19). Every function here
    // MUST run inside an SKSE task on the game thread.
    struct PassResult {
        bool effectPresent = false;
        bool clipStopped    = false;
    };

    // One keeper pass: find the perform effect; if present, kill its 32.8s
    // exit timer (idempotent, re-sent every pass), stop its ~33s clip once
    // the script's SongToPlay instance exists, optionally re-up the play
    // idle, observe SGT's follower-duet scenes (log-first: their multi-minute
    // behavior is unknown), and log the audience-quest stage for the field
    // run. a_keepFollower gates the (guarded, default-off) duet re-start path.
    PassResult KeeperPass(RE::FormID a_spellId, bool a_keepIdle,
                          bool a_keepFollower);

    // Session-end payout: dispatch SGT's own MessageAndEXP() on the effect
    // script (messages/potions/XP/ovation/bow/gold under ITS rules).
    // Returns true if the dispatch was queued.
    //
    // ⚠ NOT the default path since 2026-07-22 - see DispatchEnding. Kept
    // behind [SGT] bOwnEnding = 0 as an A/B escape hatch, because it is the
    // only way back to vanilla SGT behaviour if our ending misreads a run.
    bool DispatchPayout(RE::FormID a_spellId);

    // Session-end reaction, decided by the RUN instead of by rank.
    //
    // SGT's MessageAndEXP sets the ovation global unconditionally on its
    // first line, picks its message with a dice roll inside an expertise
    // band, and pays flat gold to anyone at 66+ in an inn - so a 0%-accuracy
    // run drew cheering, praise and coin (field 2026-07-22). This does the
    // parts worth keeping, from the performance: ovation only if earned,
    // one of SGT's own messages chosen by stars, and no gold at all (the
    // purse is payout::Deserved's business). Experience is granted by the
    // caller through SgtProgression's feed bonus.
    //
    // a_expertise selects which of SGT's message tiers the line is drawn
    // from, so the wording still fits the character's standing; a_stars
    // decides whether it is a good, indifferent or bad one.
    // a_finishedGreat is the final live Glory zone; top praise requires it
    // so a severe late fumble cannot be followed by an ovation.
    bool DispatchEnding(RE::FormID a_spellId, int a_stars, int a_expertise,
                        bool a_finishedGreat);

    // A committed non-great crowd state can change SGT's next dialogue line,
    // but it does not automatically release an audience NPC already inside
    // SGT's dance/drink idle. Reset only SGT's live human audience aliases;
    // later topic fragments remain free to play their own negative gesture.
    void ReleaseAudienceCelebration();

    // Immediately asks one of SGT's live audience aliases to say SGT's own
    // during-performance negative topic. The caller must keep the Terrible
    // global and audience quest alive briefly after this dispatch so the
    // conditioned INFO and its voice line can begin.
    bool DispatchAudienceFailureBark();

    // No-applause hard teardown. SGT's ordinary effect finish deliberately
    // enters stage 20 before its stage-200 failsafe. A crowd loss or a
    // completed performance outside Glory's green zone must skip that
    // applause phase, terminate the quest at SGT's own stage 200, and
    // release current audience idles before the perform spell is removed.
    void StopAudienceWithoutApplause();

    // Re-arm the always-gather audience nudge. Posted at session arm: the
    // nudge is once per performance, and SGT moves the quest to 20 then 200
    // as it tears the crowd down, so the next song has to be able to ask
    // for a crowd again.
    void ResetAudience();

    // Reset the per-performance one-shot latches (clip-stop dedup + the
    // once-per-performance diagnostics). Posted at session arm so every
    // performance starts clean no matter how the previous one ended.
    void ResetPerformanceLatches();

    // One browse-standstill pass (design 2026-07-19): find the perform
    // effect; if present, kill the 32.8s exit timer (every pass, so a late
    // registration dies too) and stop SGT's clip the moment SongToPlay
    // fills. With a_strip, remove the ability instead - SGT's own
    // OnEffectFinish then does the cleanup (IdleStop, EnablePlayerControls,
    // camera return). Returns sgtperform::Standstill::Pass as int:
    // 0 absent / 1 present-not-started / 2 clip stopped (now or earlier).
    int StandstillPass(RE::FormID a_spellId, bool a_strip);

    // Reset the standstill clip-stop latch (also re-arms the idle blank).
    // Posted when a standstill begins (FIFO: lands before its first pass).
    void ResetStandstillLatch();

    // Session thread, at song pick, BEFORE the unfreeze is posted: the
    // world-freeze queues every standstill pass task, and they all burst at
    // unfreeze - AFTER the pick decision. A pick CONTINUES the trigger's
    // effect instance (field 2026-07-20: the blanked instance killed the
    // play idle, the keeper re-up AND the end-cleanup's IdleStop - invisible
    // lute + control lock), so the blank must be disarmed the instant a
    // pick happens. Atomic: crosses session -> game thread.
    void DisarmStandstillBlank();

    // Session thread -> game thread, at a browse CANCEL (the browser closed
    // with no song picked and a standstill still running). Until the strip,
    // every standstill pass checks the script-disable mask and hands the
    // controls back as soon as SGT's start thread has taken them - the
    // player walks away sub-second instead of after the 3-4s settle. Firing
    // once at the cancel edge instead would LOSE THE RACE: the world-freeze
    // froze the Papyrus VM too, so SGT's DisablePlayerControls usually has
    // not run yet at that point. Atomic: crosses session -> game thread.
    void ArmCancelControlRelease();

    // Game-thread task, posted at session start: if the standstill blank
    // already landed on the effect instance this session continues, write
    // the saved original IdleToPlay/IdleStop_Loose values back. No-op when
    // nothing was blanked.
    void RestoreIdleProperties(RE::FormID a_spellId);

    // Game thread, once per session end, only while the world is unpaused:
    // send IdleForceDefaultState to the player's animation graph. The THIRD
    // control layer (field 2026-07-20): with unk11C cleared and ControlMap
    // restored the player still could not walk, because the lute idle never
    // ends - SGT's IdleStop_Loose cleanup dies on the dispelled effect.
    void ReleasePerformPose();

    // Game thread, at song pick BEFORE the AddSpell that starts SGT's real
    // performance. Saves the player's `_Talent_EnableMovement` and forces
    // it to 1, because SGT's OnEffectStart branches on it: at 1 it leaves
    // movement alone and adds the DummyInstrument prop, at 0 it takes
    // movement and adds no prop. Field 2026-07-20, one global behind both
    // "the lute is invisible" and "i cannot walk, but i can jump".
    // Restored by EnablePlayerControlsFallback on every removal path.
    void ForceEnableMovementGlobal();
    void RestoreEnableMovementGlobal();

    // Game thread. While a guitar session is live, point Skyrim's vanilla
    // lute animation object at the addon's hand-aligned SG mesh. Passing
    // false restores the original lute model. Also writes the addon's
    // BardHeroElectricPlayerPerform global (1 on enable, 0 on disable) -
    // the OAR "Player Guitar" submod conditions the player's electric
    // body clip on it. This changes no animation timing, controls, or
    // SGT records.
    void SetGuitarAnimObjectOverride(bool a_enabled);

    // Game thread. Re-point the shared AnimObjectLute record at whatever
    // the CURRENT session's baseline is: the guitar mesh while the guitar
    // override is armed, the saved original after it, or the vanilla lute
    // path when nothing was ever saved. The band's per-member model swaps
    // (BandStage) leave the record aimed at the LAST member's instrument -
    // bass, when no rhythm stem exists and the rhythm guitarist never
    // starts - and any graph that captures it afterwards (the player's on
    // a pose refresh, the next session, a vanilla tavern bard) wears that
    // instrument (field 2026-07-28: "sometimes we get the wrong guitar,
    // or even a bass"). BandStage calls this one capture-interval after
    // each of its swaps and again at teardown.
    void ReassertAnimObjectBaseline();

    // Game thread, kDataLoaded AND post-load/new-game. The electric
    // perform global lives in saves: a save written mid-electric-perform
    // carries 1.0, which would leak the electric body clip into lute
    // performances on that timeline. Idempotent 0-write.
    void ClearElectricPerformGlobal();

    // Game thread, after EVERY RemoveSpell of the perform ability. Hands
    // SGT's `_Talent_EnableMovement` global back, and NOTHING ELSE.
    // Idempotent; safe on every removal path.
    //
    // ⚠ DO NOT ADD ENGINE CONTROL-STATE WRITES HERE. THEY WERE THE BUG. ⚠
    //
    // This used to dispatch Game.EnablePlayerControls, hand-clear
    // ControlMap::unk11C and drive ToggleControls off a baseline snapshot,
    // all to "recover" from a post-song input lock. On AE 1.6.1170 the
    // members it wrote are not the control masks at all - CommonLibSSE-NG's
    // ControlMap layout is shifted +8 past `controlMap[]`, so
    // `enabledControls` is really `contextPriorityStack::_size`. The
    // recovery was corrupting the input context stack in the name of
    // repairing it, which is why one pass killed every input while every
    // other flag read exactly like free roam.
    //
    // The lock it existed to paper over came from MovementGuard writing the
    // same field. Both were removed 2026-07-22, along with the
    // iControlRecoveryPasses setting that gated this half; field-confirmed
    // by bBlockMovement=0 clearing the lock AND the FUCK.dll CTD in one run.
    // Evidence: docs/evidence/2026-07-22-controlmap-offsets-are-wrong-on-ae.md
    //
    // If controls go missing again, find what took them. Do not write engine
    // control state to paper over it.
    void EnablePlayerControlsFallback();
}
