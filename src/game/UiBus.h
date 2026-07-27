// src/game/UiBus.h
#pragma once

// Session-thread <-> render-thread mailboxes for the M4 UI. Same discipline
// as EngineFeed: tiny guarded payloads + lock-free gates. Never hold this
// mutex and EngineFeed::mx at the same time.

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "chart/Scan.h"
#include "game/FlickOpenGate.h"

namespace SH {
    struct UiBus {
        static UiBus& GetSingleton() {
            static UiBus s;
            return s;
        }

        // session thread -> browser window: "open yourself" (start key /
        // perform power at kIdle). The context is published first and frozen
        // by the browser when it consumes the release/acquire request edge.
        // -1 is the context-free debug Songbook; 0/1/2 are lute/flute/drum.
        std::atomic<bool> browserOpenRequest{ false };
        std::atomic<int>  browserInstrumentContext{ -1 };
        void RequestBrowserOpen(int a_instrumentContext) {
            browserInstrumentContext.store(a_instrumentContext,
                                           std::memory_order_relaxed);
            browserOpenRequest.store(true, std::memory_order_release);
        }
        bool TakeBrowserOpenRequest(int& a_instrumentContext) {
            if (!browserOpenRequest.exchange(false,
                                             std::memory_order_acquire)) {
                return false;
            }
            a_instrumentContext =
                browserInstrumentContext.load(std::memory_order_relaxed);
            return true;
        }

        // browser window -> InputHook: "I am open" (render thread mirrors
        // its _open every IsOpen() poll). While set, the hook swallows
        // bound keys from the game so guitar/keyboard menu navigation
        // (arrows, frets, strum) does not steer the character.
        std::atomic<bool> browserOpen{ false };
        // ...and whether that open Songbook is currently showing the practice
        // PICKER rather than the song list. The picker is a sub-view, so
        // browserOpen is true for both and the hint bar cannot tell them
        // apart without this. Mirrored every frame from IsOpen() alongside
        // browserOpen, for the same reason: a latched-true flag would leave
        // the wrong hints on screen forever.
        std::atomic<bool> practicePickerOpen{ false };

        // game thread (OnPreLoadGame) -> browser window: "close yourself"
        // (a loaded save must not resurrect a frozen browse). Consumed with
        // exchange(false) in IsOpen.
        std::atomic<bool> browserCloseRequest{ false };

        // InputHook -> UI windows: navigation intents harvested from the
        // swallowed key events while the browser/results are open. FLICK's
        // ImGui never sees swallowed or injected keys (field round 4:
        // GlovePIE guitar navigation was dead via ImGui polling), so the
        // hook - which DOES see every key - is the source of truth.
        // Consumed with exchange(); windows drain them at first draw so a
        // stray press before the window opens cannot fire into it.
        std::atomic<int>  navMove{ 0 };     // +down / -up (strum bar)
        std::atomic<int>  navHeldMove{ 0 }; // physical +down / -up / released
        std::atomic<bool> navConfirm{ false };  // green fret / Enter (Plus)
        std::atomic<bool> navClose{ false };    // red fret / Esc
        // +harder / -easier: Songbook difficulty, picked at song select the
        // way Guitar Hero does it (2026-07-26). Yellow/blue frets and the
        // Left/Right arrows; accumulates like navMove so two presses in one
        // frame both land.
        std::atomic<int>  navDiff{ 0 };
        // Orange fret: the one fret HarvestBrowserNav did not already spend
        // (green confirm, red back, yellow/blue difficulty). Toggles the
        // Songbook's practice arming, so the picker is reachable from a
        // guitar rather than mouse-only.
        std::atomic<bool> navToggle{ false };

        // session thread -> pause menu window (field round 6): visible
        // while the session is paused; Resume/Quit route back through
        // Session::RequestResume/RequestAbort.
        std::atomic<bool> pauseMenuOpen{ false };
        // Session-thread resume gate -> HUD/InputHook. The cue is the
        // ResumeCountdown::Cue integer (0 done, 3/2/1, 4 GO). `active`
        // remains true for the few callback milliseconds after GO so input
        // stays owned until clock/audio/gameplay engage together.
        std::atomic<bool> resumeCountdownActive{ false };
        std::atomic<int>  resumeCountdownCue{ 0 };
        // session world-pause latch (ShadowPause menu shown, or the
        // legacy numPausesGame bump when menu registration failed). Read
        // by the InputHook capture gating (GameIsPaused is OUR doing
        // then) and the MenuSink resume-gate sampling.
        std::atomic<bool> worldPaused{ false };

        // Crowd-failure jump lockout. Session thread -> InputHook.
        //
        // The recoil animation is the REACTION; this is the GUARANTEE. Two
        // animation events were tried in the field (staggerStart, then
        // recoilLargeStart/recoilStart) and both could be jumped out of, so
        // the behaviour graph is not where this gets solved. The strum bind
        // defaults to Space (InputMapper.h:42), which is also Jump - so
        // failing a song was ending with the player leaping.
        //
        // While set, the hook swallows ONLY the strum binds, and only on the
        // pass-through path. Deliberately NOT Skyrim's own jump binding:
        // reading that means ControlMap, and CLAUDE.md forbids touching
        // enabledControls on AE 1.6.1170 because its layout is wrong past
        // controlMap[].
        //
        // A stuck flag here swallows Space forever, which is unrecoverable
        // for the player, so it is cleared on EVERY failure-feedback exit AND
        // unconditionally at session start.
        std::atomic<bool> failureInputLockout{ false };

        // ---- practice HUD (plan P5) --------------------------------------
        // Session thread -> practice HUD strip. ATOMICS ONLY, deliberately:
        // the HUD reads these while it already holds EngineFeed::mx for the
        // engine stats, and taking UiBus::mx there would be the one lock
        // order this file forbids.
        //
        // Section INDICES, not names. The render thread already has the
        // names - SliceChart copies `sections` verbatim, so feed.song->
        // chart.sections is the full marker list even mid-practice - and
        // publishing strings would need the mutex these must not take.
        // -1/-1 is whole song.
        std::atomic<bool>  practiceActive{ false };
        std::atomic<int>   practiceLoop{ 0 };
        std::atomic<float> practiceSpeed{ 1.0f };
        std::atomic<int>   practiceStartSection{ -1 };
        std::atomic<int>   practiceEndSection{ -1 };
        // Practice SUMMARY, shown when looping is off and the range has had
        // its one pass. Deliberately its own tiny atomic payload with its own
        // window, NOT UiBus::Results + StageResults: that path is gated on
        // `completed`, and routing practice through it would walk straight
        // back through every recording gate. Nothing here is ever persisted.
        //
        // STAGED, then published, for the same reason Results is: this panel
        // raises the kIdle world freeze, and a freeze landing on the same
        // frame as the session end stops a dismissed skeleton band's 1.5s
        // unsummon dead - the ensemble pops out of existence instead of
        // fading (field 2026-07-26). The session thread holds the publish
        // until bard::band::ResultsMayPublish opens.
        std::atomic<bool>  practiceSummaryPending{ false };
        std::atomic<bool>  practiceSummaryReady{ false };
        std::atomic<int>   practiceSummaryHit{ 0 };
        std::atomic<int>   practiceSummaryMissed{ 0 };
        std::atomic<int>   practiceSummaryTotal{ 0 };
        std::atomic<int>   practiceSummaryCombo{ 0 };
        std::atomic<float> practiceSummarySpeed{ 1.0f };
        void StagePracticeSummary() {
            practiceSummaryPending.store(true, std::memory_order_release);
        }
        bool TryPublishStagedPracticeSummary() {
            if (!practiceSummaryPending.load(std::memory_order_acquire)) {
                return false;
            }
            practiceSummaryPending.store(false, std::memory_order_release);
            practiceSummaryReady.store(true, std::memory_order_release);
            return true;
        }
        // Clears the STAGED summary too. A run that starts - or a save that
        // loads - inside the reveal hold must not leave one queued to pop
        // over whatever comes next.
        void ClearPracticeSummary() {
            practiceSummaryPending.store(false, std::memory_order_release);
            practiceSummaryReady.store(false, std::memory_order_release);
        }

        void ClearPracticeHud() {
            practiceActive.store(false, std::memory_order_release);
            practiceLoop.store(0);
            practiceSpeed.store(1.0f);
            practiceStartSection.store(-1);
            practiceEndSection.store(-1);
        }

        // Session-thread crowd model -> render-thread Glory HUD. This is the
        // same rolling sentiment that drives SGT's reaction globals; it is
        // intentionally not the old disconnected EngineStats::glory value.
        std::atomic<float> glory{ 0.5f };
        std::atomic<bool>  gloryDanger{ false };
        void DrainNav() {
            navMove.store(0);
            navHeldMove.store(0);
            navConfirm.store(false);
            navClose.store(false);
            navDiff.store(0);
            navToggle.store(false);
        }

        // browser -> session thread: chosen song
        struct PendingStart {
            bard::SongEntry entry;
            int             difficulty = 3;
            int             instrumentContext = -1;
            // Practice mode (plan P4). Section indices index
            // ParsedChart::sections; -1/-1 means whole song, which is what
            // practice::ResolveRange reads an unusable pair as, and is the
            // common case because most charts carry no markers.
            bool practice     = false;
            int  startSection = -1;
            int  endSection   = -1;
            // Looping OFF plays the range once and ends with the practice
            // summary instead of snapping back. Defaults TRUE so any caller
            // that forgets it gets the looping behaviour practice has always
            // had, rather than silently ending after one pass.
            bool loopEnabled = true;
            // Playback speed chosen BEFORE the run starts (Guitar Hero
            // convention). 1.0 is normal; the picker offers presets.
            double speed = 1.0;
        };
        // Last-write-wins over an unconsumed pending start is intentional:
        // a later browser click supersedes an earlier one.
        void PushStart(const bard::SongEntry& e, int diff,
                       int instrumentContext) {
            std::scoped_lock lk(mx);
            pending = PendingStart{ e, diff, instrumentContext };
        }
        void PushPracticeStart(const bard::SongEntry& e, int diff,
                               int instrumentContext, int startSection,
                               int endSection, bool loopEnabled = true,
                               double speed = 1.0) {
            std::scoped_lock lk(mx);
            pending = PendingStart{ e,           diff,         instrumentContext,
                                    true,        startSection, endSection,
                                    loopEnabled, speed };
        }

        // session thread -> Songbook: open STRAIGHT INTO the practice picker
        // for this song, skipping the song list entirely. The pause menu's
        // practice toggle uses it: that path already knows which song is
        // playing, so making the player find it again in the list just to
        // choose a section would be busywork.
        std::atomic<bool> practicePickerRequest{ false };
        void RequestPracticePicker(const bard::SongEntry& a_entry, int a_diff,
                                   int a_context) {
            {
                std::scoped_lock lk(mx);
                practicePickerEntry = a_entry;
                practicePickerDiff  = a_diff;
                practicePickerCtx   = a_context;
            }
            practicePickerRequest.store(true, std::memory_order_release);
        }
        bool TakePracticePickerRequest(bard::SongEntry& a_entry, int& a_diff,
                                       int& a_context) {
            if (!practicePickerRequest.exchange(false,
                                                std::memory_order_acquire)) {
                return false;
            }
            std::scoped_lock lk(mx);
            a_entry   = practicePickerEntry;
            a_diff    = practicePickerDiff;
            a_context = practicePickerCtx;
            return true;
        }

        // ---- practice section picker (plan P4) ---------------------------
        // The Songbook hosts the picker as a SUB-VIEW rather than opening a
        // second FLICK window (see render/PracticeLayout.h for why), so this
        // is the whole cross-thread protocol: one request, one answer.
        //
        // It exists because sections are NOT in SongEntry - they only appear
        // in ParsedChart, after LoadSong parses the file off disk. The render
        // thread must never do that itself, so it asks the session thread.
        struct PracticeSection {
            double      time = 0.0;
            std::string name;
        };

        // Songbook -> session thread. Returns the generation stamp the
        // caller must quote when reading the answer back.
        unsigned RequestPracticeSections(const bard::SongEntry& e) {
            unsigned gen = 0;
            {
                std::scoped_lock lk(mx);
                practiceRequestEntry = e;
                gen = ++practiceRequestGen;
                practiceSections.clear();
                practiceSectionsGen = 0;
            }
            practiceSectionsReady.store(false, std::memory_order_release);
            practiceSectionsRequest.store(true, std::memory_order_release);
            return gen;
        }
        bool TakePracticeSectionsRequest(bard::SongEntry& a_entry,
                                         unsigned&        a_gen) {
            if (!practiceSectionsRequest.exchange(
                    false, std::memory_order_acquire)) {
                return false;
            }
            std::scoped_lock lk(mx);
            a_entry = practiceRequestEntry;
            a_gen   = practiceRequestGen;
            return true;
        }
        // session thread -> Songbook. a_songEndSec is what ResolveRange
        // needs to close an open last section.
        void PublishPracticeSections(std::vector<PracticeSection> a_sections,
                                     double a_songEndSec, unsigned a_gen) {
            {
                std::scoped_lock lk(mx);
                // A late answer for a song the player has already navigated
                // away from must not overwrite a newer one.
                if (a_gen != practiceRequestGen) { return; }
                practiceSections    = std::move(a_sections);
                practiceSongEndSec  = a_songEndSec;
                practiceSectionsGen = a_gen;
            }
            practiceSectionsReady.store(true, std::memory_order_release);
        }
        // Quoting the generation is what makes a stale answer unreadable
        // rather than merely unlikely.
        bool ReadPracticeSections(std::vector<PracticeSection>& a_out,
                                  double& a_songEndSec, unsigned a_gen) {
            if (!practiceSectionsReady.load(std::memory_order_acquire)) {
                return false;
            }
            std::scoped_lock lk(mx);
            if (practiceSectionsGen != a_gen || a_gen == 0) { return false; }
            a_out        = practiceSections;
            a_songEndSec = practiceSongEndSec;
            return true;
        }
        // The session thread drops any in-flight request when it leaves
        // idle, so a pick made during a session cannot resurrect a picker.
        void ClearPracticeSections() {
            practiceSectionsRequest.store(false, std::memory_order_release);
            practiceSectionsReady.store(false, std::memory_order_release);
            std::scoped_lock lk(mx);
            practiceSections.clear();
            practiceSectionsGen = 0;
        }
        std::optional<PendingStart> TakePendingStart() {
            std::scoped_lock lk(mx);
            auto p = std::move(pending);
            pending.reset();
            return p;
        }

        // session thread -> results window: a COMPLETED run only
        struct Results {
            std::string songName, artist;
            long long   score = 0;
            int  notesHit = 0, notesTotal = 0, maxCombo = 0;
            int  overstrums = 0, spPhrases = 0;
            int  stars = 0;
            bool fullCombo = false;

            // ---- what the run CHANGED (spec 2026-07-22-performance-ui 3.3)
            // The run's own numbers above say how it went; these say what it
            // was worth, which is the payoff moment the mod was missing.
            //
            // Every one of these is filled on the session thread at
            // EndSession, where the ledger, the purse inputs and the
            // expertise sample are all still in hand. The render thread must
            // never go looking for them itself.
            int  prevBest = 0;      // best stars for this chart BEFORE the run
            bool newBest  = false;  // ...and this run beat it

            // The difficulty the run actually PLAYED (0..3 Easy..Expert),
            // i.e. LoadedSong::resolvedDifficulty and not the requested
            // setting - the same value the star record is keyed on, so the
            // results page can never disagree with the ledger it wrote.
            // -1 only if a snapshot is read before one was staged.
            int difficulty = -1;

            // Electric song-end sting, as a ui_sfx::Cue index (-1 = none:
            // lute context, or the crowd-loss failure path which fires its
            // own sting at the loss). Carried in the snapshot because the
            // sting must pop WITH the results menu - firing it when the
            // verdict is computed landed ~1.75s early (field 2026-07-25).
            // Plain int so this bus header stays free of the logic header.
            int stingCue = -1;
            // ...and the vanilla instruments' equivalent: play Skyrim's own
            // level-up sound on a cleared lute/flute/drum song. Rides the
            // snapshot for exactly the same timing reason as stingCue, and
            // is mutually exclusive with it by construction
            // (ui_sfx::VanillaClearSting excludes the electric context).
            bool clearSting = false;

            // The purse, and one line naming why. Only meaningful when
            // goldKnown: the non-deferred path pays through GoldScale's
            // delta instead and there is no single figure to name.
            bool        goldKnown = false;
            int         gold      = 0;
            std::string goldReason;

            // Rank standing, PREDICTED. The experience grant is held with
            // the payout until this very window closes, so at draw time the
            // rise has not happened yet - these are expertise-before plus
            // the XP we are about to hand over, run through the same
            // EffectiveRank the browser uses (so a gate-blocked rank-up is
            // correctly not claimed). Only filled when rankKnown: SGT absent,
            // or SGT's own MessageAndEXP running instead of ours, means the
            // grant is a dice roll we cannot predict.
            bool rankKnown      = false;
            int  expertiseBefore = 0;
            int  expertiseAfter = 0;
            int  rankBefore     = 1;
            int  rankAfter      = 1;
            int  xpGain         = 0;
            int  instrument     = 0;  // stars::Instrument serialization code
        };
        std::atomic<bool> resultsReady{ false };  // window-open gate
        void StageResults(const Results& r) {
            {
                std::scoped_lock lk(mx);
                results = r;
            }
            // Stop new input calls into FUCK before exposing Results. Any
            // call already inside it is counted and must drain first.
            resultsHostOpen.BeginOpen();
            resultsPending.store(true, std::memory_order_release);
        }
        bool TryPublishStagedResults() {
            if (!resultsPending.load(std::memory_order_acquire) ||
                !resultsHostOpen.CanPublish()) {
                return false;
            }
            resultsPending.store(false, std::memory_order_release);
            resultsReady.store(true, std::memory_order_release);
            return true;
        }
        bool TryEnterFlickInput() {
            return resultsHostOpen.TryEnterInput();
        }
        void LeaveFlickInput() {
            resultsHostOpen.LeaveInput();
        }
        void CompleteResultsFirstDraw() {
            resultsHostOpen.CompleteFirstDraw();
        }
        bool ResultsHostOpenBlocking() const {
            return resultsHostOpen.BlockingInput();
        }
        void CloseResults() {
            resultsPending.store(false, std::memory_order_release);
            resultsReady.store(false, std::memory_order_release);
            resultsHostOpen.Cancel();
        }
        Results ReadResults() {
            std::scoped_lock lk(mx);
            return results;
        }

    private:
        std::mutex                  mx;
        std::optional<PendingStart> pending;  // guarded by mx
        Results                     results;  // guarded by mx
        std::atomic<bool>            resultsPending{ false };
        flick_open::Gate             resultsHostOpen;

        // practice picker (see the block above). The two flags are the
        // edges; everything with content is guarded by mx.
        std::atomic<bool>            practiceSectionsRequest{ false };
        std::atomic<bool>            practiceSectionsReady{ false };
        bard::SongEntry              practicePickerEntry;    // guarded by mx
        int                          practicePickerDiff = 3;  // guarded by mx
        int                          practicePickerCtx  = -1; // guarded by mx
        bard::SongEntry              practiceRequestEntry;   // guarded by mx
        unsigned                     practiceRequestGen = 0;  // guarded by mx
        std::vector<PracticeSection> practiceSections;        // guarded by mx
        unsigned                     practiceSectionsGen = 0; // guarded by mx
        double                       practiceSongEndSec = 0.0;  // by mx
    };
}
