// src/game/SgtProgression.h
#pragma once

// Game-thread interop with SGT's expertise GLOBs (Flute 0x000D61 / Lute
// 0x000D62 / Drum 0x000D63 in SkyrimsGotTalent-Bards.esp). Implements the
// spec's rank-gate clamp (5), effective-tier promotion (6), and XP feed
// (4). Every function except NoteCast/NoteSessionPayout/Tick must run in
// an SKSE task on the game thread; the session thread drives scheduling
// through Tick().
#include "game/StarsLogic.h"

namespace SH::SgtProgression {
    // kDataLoaded (game thread): resolve the three globals. Soft - absent
    // SGT logs once and every later call no-ops.
    void Install();
    bool Available();

    // Clamp the instrument's GLOB to the rank-gate ceiling (game thread).
    void ClampPass(stars::Instrument a_inst, const char* a_reason);

    // Promotion bracket around the deferred payout (game thread):
    // Begin: remember actual, write max(actual, floor).
    // FinishPayout: write RestoredValue(actual, promoted, cur) + feed
    // bonus, then clamp. Also called immediately on dispatch failure.
    void BeginPromotion(stars::Instrument a_inst, int a_floor);
    void FinishPayout(stars::Instrument a_inst, int a_feedBonus);

    // Scheduling (thread-safe; store-only):
    // NoteCast: any watched perform cast - queues clamp checks at +45s and
    // +80s (SGT's vanilla payout smears 32.8s..~75s after the cast).
    void NoteCast(stars::Instrument a_inst, double a_nowQpc);
    // NoteSessionPayout: a whole-song payout was dispatched - queue
    // FinishPayout for after the gold end-capture window closes.
    void NoteSessionPayout(stars::Instrument a_inst, int a_feedBonus,
                           double a_deadlineQpc);
    // Session-thread loop pump: posts due game-thread tasks.
    void Tick(double a_nowQpc);
    // kPostLoadGame: clamp all three instruments (covers offline GLOB
    // drift: bard teaching, console).
    void OnPostLoadGame();
    // kPreLoadGame: invalidate the teaching baseline BEFORE the save's
    // globals are swapped in (see PollTeachingEdge).
    void OnPreLoadGame();

    // Bard teaching (spec 6.3). A lesson is SGT's Bard_Fragment*.psc and
    // its 12 siblings taking 100 gold and raising ALL THREE globals in one
    // dialogue fragment; unlock::IsLessonEdge is that rule and carries the
    // full argument for why nothing else can forge it.
    //
    // WARNING for anyone editing ClampPass: the rule survives ClampPass
    // sweeping all three instruments (OnPostLoadGame and SetClampSuspended
    // both do) ONLY because a clamp is strictly monotone DOWN. A clamp that
    // could ever RAISE a global - a rank FLOOR rather than a ceiling - would
    // forge a lesson on all three at once and silently hand out free charts.
    //
    // Game thread (reads the globals). Returns the lowest instrument index
    // that rose, or -1. For a real lesson that is ALWAYS 0: all three rose.
    // Callers must not read instrument meaning into it - use
    // StarLedger::ActiveInstrument() for that.
    //
    // Re-baselines on EVERY call, including the one suppressed case, so a
    // session's worth of writes can never accumulate into a later edge. The
    // ONLY suppression left is a_sessionActive: a lesson takes a dialogue,
    // which the player cannot reach mid-performance. Deliberately NOT gated
    // on the promotion bracket, the pending payout or the post-cast clamp
    // deadlines - each of those writes ONE instrument or writes down, so
    // none can forge a lesson, and gating on them cost an 80-second blind
    // window after every cast in which a paid lesson vanished in silence.
    int PollTeachingEdge(bool a_sessionActive);

    // Current expertise for one instrument, or -1 (SGT absent, unresolved,
    // non-finite). Game thread. Distinct from UiSampled, which is a
    // session-start snapshot the gold path depends on and must not drift.
    int LiveExpertise(stars::Instrument a_inst);

    // ---- the rank ladder (spec 6.2) ---------------------------------------
    // SGT's own expertise ladder, read off its tier branches
    // (_Talent_PlayInstrument.psc: Expertise < 26 / < 46 / < 66 / < 86, else
    // Pro - the same four boundaries SettingsTool::TierOf renders). Rank 1 is
    // 0-25, 2 is 26-45, 3 is 46-65, 4 is 66-85, 5 is 86+. A negative sample
    // (SGT absent, globals unresolved, nothing sampled yet) falls through the
    // first branch to rank 1, the smallest purse, which is the right way to
    // be wrong here. Pure; any thread.
    int RankFromExpertise(int a_expertise);

    // The EFFECTIVE rank: what the star gate will actually let the player
    // keep, two corrections deep. Lives here, and NOT copied into a caller,
    // because the browser's lock display and the bard-teaching pick MUST
    // answer the same number - a browser that disagrees either dims a chart
    // a lesson already bought, or offers one the confirm path then refuses.
    //
    // The expertise is CLAMPED to the gate ceiling, because a bard's +10 (or
    // any other rise) lands before the star gate claws it back. A rank-1
    // player parked at the ceiling of 25 reads 35 - rank 2 - so the teaching
    // pick would skip every rank-2 chart as "already open", buy a rank-3 one,
    // and then the clamp re-locks the rank-2 charts; worse, a library with
    // nothing above rank 2 would pick NOTHING and 100 gold would buy air.
    // min() with the ceiling predicts where the clamp is about to put them.
    // The dev tier setter suspends the clamp, so it suspends this too - the
    // same test FinishPayout makes.
    //
    // a_expertise is the CALLER'S sample, because that is the one thing the
    // two callers cannot share: LiveExpertise() on the game thread, and
    // UiSampled() from the render thread, which must not read SGT's globals
    // at all. Everything after the sample is here, once.
    //
    // Takes the ledger lock (GateCeilingPeek); safe from any thread, and
    // READ-ONLY - the browser calls this on the render thread every frame.
    int EffectiveRank(stars::Instrument a_inst, int a_expertise);

    // ---- settings-page dev tooling (2026-07-20) ----------------------------
    // Sampled GLOB values for the render-thread readout: PostUiSample queues
    // a game-thread sample; UiSampled returns the last sample (-1 = none/
    // unavailable). CheatSetExpertise writes a tier value directly and
    // SUSPENDS the rank-gate clamp (runtime-only latch, cleared at process
    // start) so ClampPass and the payout cap cannot undo the hand-set value;
    // SetClampSuspended(false) re-arms and immediately re-clamps all three.
    void PostUiSample();                    // any thread
    int  UiSampled(stars::Instrument);      // any thread
    void CheatSetExpertise(stars::Instrument a_inst, int a_value);  // any thread
    bool ClampSuspended();                  // any thread
    void SetClampSuspended(bool a_suspend); // any thread
}
