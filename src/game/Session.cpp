// src/game/Session.cpp
#include "PCH.h"
#include "game/Session.h"

#include "QpcClock.h"
#include "Settings.h"
#include "audio/AudioEngine.h"
// The frame arithmetic behind AudioEngine::SeekStems. Used directly here so
// a practice session's START can seek the stems BEFORE ScheduleStart and
// still hand AnchorEstimator a start frame for song position 0 (see the
// practice block in StartSession).
#include "audio/StemSeekLogic.h"
#include "game/AudienceLifecycleLogic.h"
#include "game/BandLifecycleLogic.h"
#include "game/BandStage.h"
#include "game/PerformanceCamera.h"
#include "game/WidgetMuffle.h"
#include "game/ShadowPause.h"
#include "game/CrowdMoodLogic.h"
#include "game/StreakFireLogic.h"
#include "game/CrowdReactions.h"
#include "game/UiSfx.h"
#include "game/BrowseCameraLogic.h"
#include "game/Ducking.h"
#include "game/DifficultyTuning.h"
#include "game/EngineFeed.h"
#include "game/EndingLogic.h"
#include "game/GoldScale.h"
// probe line "d" - our own hook's frame counters. NB the log literal is
// "#1d".."#8d": the "N" in "#Nd" is the probe NUMBER, so grepping the log
// for "#Nd" matches nothing. Grep "hook passthrough".
#include "game/InputHook.h"
#include "game/MoodGlobals.h"
#include "game/MovementGuard.h"
#include "game/NativeMenuLogic.h"
#include "game/PayoutMath.h"
#include "game/PerformTriggerLogic.h"
#include "game/ResultsLogic.h"
#include "clock/ResumeWorldPauseLogic.h"
#include "game/SgtPerformLogic.h"
#include "game/SgtProgression.h"
#include "game/SgtStartLead.h"
#include "game/SgtVm.h"
#include "game/SongEligibility.h"
#include "game/SongIdentity.h"
#include "game/SongLibrary.h"  // bard teaching picks over the browser's list
#include "game/StarLedger.h"
#include "game/StarsLogic.h"
#include "game/UiBus.h"
#include "game/UnlockLogic.h"

#include "render/RenderUi.h"  // ForceCursor refcount, for the lock probe

#include "chart/LoadSong.h"
#include "chart/Scan.h"
#include "clock/AnchorEstimator.h"
#include "clock/MasterClock.h"
#include "clock/ResumeCountdown.h"
#include "clock/SlaveController.h"
#include "engine/EngineParams.h"
#include "engine/GuitarEngine.h"
#include "practice/PracticeLoop.h"   // ShouldRestart / RestartTime / rules
#include "practice/PracticeRange.h"  // ResolveRange
#include "practice/PracticeSlice.h"  // SliceChart
#include "util/PathText.h"

#include "RE/B/BGSKeyword.h"   // LocTypeInn, the payout's venue test
#include "RE/B/BGSLocation.h"  // GetCurrentLocation()->HasKeyword
#include "RE/C/ControlMap.h"   // post-session control probe
#include "RE/I/InventoryMenu.h"  // MENU_NAME, closed at the perform trigger
#include "RE/L/LoadingMenu.h"  // MENU_NAME (umbrella should cover it; explicit is safe)
#include "RE/M/Misc.h"  // DebugNotification, the native standing-guard message
#include "RE/U/UIMessageQueue.h"  // close the inventory at the trigger
#include "RE/P/PlayerCamera.h"             // probe: camera state id
#include "RE/P/PlayerControls.h"           // probe: per-handler enables
#include "RE/S/ScriptEventSourceHolder.h"  // TESSpellCastEvent source
#include "RE/S/SpellItem.h"                // perform-power lookup
#include "RE/T/TESDataHandler.h"           // LookupForm
#include "RE/T/TESSpellCastEvent.h"        // perform-power start trigger

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h (house ordering)

#include "FUCK_API.h"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>  // strcmp, resolving LocTypeInn by editor ID
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>  // the teaching pick's parallel rank/taught lists

namespace SH {
    namespace {
        enum class State { kIdle, kPlaying, kPaused, kResuming };

        struct SessionData {
            bard::LoadedSong                     song;
            bard::MasterClock                    clock;
            std::optional<bard::AnchorEstimator> est;
            bard::SlaveController                ctrl;
            std::unique_ptr<bard::GuitarEngine>  engine;
            AudioEngine                          audio;
            double songLen   = 0.0;
            double nextLog   = 0.0;
            // live crowd mood feed rate limit. Unlike nextLog this starts
            // far negative ON PURPOSE: song time runs negative through the
            // lead-in and the mood model is built to see it, so a 0.0 here
            // would silently drop every pre-song sample.
            double nextMoodFeed = -1e9;
            double nextMoodSync = -1e9;
            bool audienceCommentsReleased = false;
            // song time of the last mood feed, so the payout tally can
            // attribute a real interval to the level the crowd held over it.
            // -1e9 = "none yet": the first feed has no interval behind it,
            // and 1e9 seconds of anything would swamp the whole tally.
            double lastMoodFeed = -1e9;
            double nextGapLog = 0.0;  // transient anchor-gap log rate limit
            double maxAbsDelta = 0.0, sumDelta = 0.0;
            double maxDeltaAt  = 0.0;  // SongTime of the max|delta| sample
            double winMin = 1e9, winMax = -1e9;
            long   samples = 0;
            bool   refined = false;        // one-shot countdown re-projection
            std::uint64_t prevFrames = 0;  // last anchor frame count (settle)
            int    settleTicks = 0;        // >0 = hold summary stats this tick
            double resumeStartedAt = 0.0;  // QPC at kPaused -> kResuming
            bard::ResumeCountdown resumeCountdown;
            bool   resumeAudioStarted = false;
            std::uint32_t startCell = 0;   // FormID captured at start
            std::uint32_t startWs   = 0;   // worldspace FormID (0 = interior)
            bool          startInterior = false;
            int           difficulty    = 3;  // as requested (0..3 Easy..Expert)
            stars::Instrument instrument = stars::Instrument::kLute;
            int instrumentContext = songeligibility::kLute;
            bard::band::StemAvailability bandStems;
            std::string       chartKey;  // song folder leaf name

            // ---- practice mode (spec 2026-07-26-practice-mode 6) ---------
            // `song.chart` is the SLICE once practice is on; fullChart keeps
            // the unsliced original so a later range change can re-slice
            // without a second LoadSong.
            bool                          practice = false;
            bool                          loopEnabled = true;
            bard::ParsedChart             fullChart;
            bard::practice::PracticeRange range;
            double                        speed     = 1.0;  // P6 drives this
            int                           loopCount = 0;
            // Resolved ONCE at start; P3 consumes it at the nine commit
            // sites. Defaulted to PerformanceRules and NOT to the
            // value-initialised (all-false) PracticeRules on purpose: an
            // all-false default would silently disable recording for a
            // normal performance if an assignment were ever missed, whereas
            // this default is exactly today's behaviour.
            bard::practice::PracticeSessionRules rules =
                bard::practice::PerformanceRules();
            // Song time the start countdown lands on. 0.0 for a normal
            // session; a practice range starts AT the range, so the whole
            // start projection is shifted by this (see StartSession).
            double startSongSec = 0.0;
        };

        // What StartSession is asked to practice. Section indices index
        // ParsedChart::sections; the -1/-1 default is "whole song", which
        // ResolveRange already handles as its unusable-input case.
        struct PracticeRequest {
            bool practice     = false;
            int  startSection = -1;
            int  endSection   = -1;
            // Looping OFF runs the range once and ends with the practice
            // summary. Defaults TRUE so a caller that forgets it gets the
            // behaviour practice has always had.
            bool loopEnabled  = true;
            // Speed chosen in the picker, applied once the session is up.
            double speed      = 1.0;
        };

        std::atomic<State> g_state{ State::kIdle };
        std::atomic<bool>  g_reqPause{ false }, g_reqAbort{ false };
        std::atomic<bool>  g_reqResume{ false };  // pause-key toggle
        std::atomic<bool>  g_reqRestart{ false };  // pause menu Restart
        std::atomic<bool>  g_reqPracticeToggle{ false };  // pause menu
        std::atomic<bool>  g_reqStart{ false };  // perform-power cast -> start
        // Index = Songbook initiation context. Guitar is distinct here even
        // though its progression identity aliases to lute.
        std::atomic<RE::FormID>
            g_performSpell[songeligibility::kInstrumentContextCount]{};
        std::atomic<bool>  g_inLoadingMenu{ false };
        std::atomic<bool>  g_uiPaused{ false };  // sink-sampled GameIsPaused
        std::atomic<bool>  g_nativeMenuOpen{ false };
        // Forced dialogue (a guard's arrest) - the ONE menu class that both
        // forces itself on the player and cannot progress in a frozen
        // world. Sampled at every state; the browse guard reads it.
        std::atomic<bool>  g_dialogueOpen{ false };
        // Cursor Menu is BardHero/FLICK's cursor surface; the other
        // exclusions are passive display/fade layers that cannot trap input.
        // The tracker is written by MenuSink on the game thread and cleared
        // by session start/teardown, hence its tiny dedicated mutex.
        std::mutex g_menuTrackMx;
        native_menu::Tracker g_nativeMenus;
        std::atomic<HWND>  g_gameHwnd{ nullptr };
        std::atomic<std::uint32_t> g_playerCell{ 0 };
        std::atomic<std::uint32_t> g_playerWs{ 0 };
        std::atomic<bool>          g_playerInterior{ false };
        // SGT's own venue test for the gold branch, sampled on the game
        // thread alongside the cell/worldspace capture above
        // (_Talent_PlayInstrument.psc:389-421 -
        // GetCurrentLocation().HasKeyword(LocTypeInn)).
        std::atomic<bool>          g_playerAtInn{ false };
        // whole-song SGT keeper (plan 2026-07-19). Logic is session-thread
        // only; the atomic carries the keeper pass' observation back.
        sgtperform::Logic g_sgtLogic{ sgtperform::Config{} };
        std::atomic<int>  g_sgtSeen{ -1 };  // sgtperform::Seen tri-state
        std::atomic<bool> g_sgtClipStopped{ false };
        std::atomic<std::uint32_t> g_sgtGen{ 0 };  // rejects keeper stores
                                                   // from a prior session
        // browse standstill (design 2026-07-19): the trigger's performance
        // is stripped while the browser is open; the real one starts at
        // song pick. Logic is session-thread only; the atomic carries the
        // game-thread pass observation back (Standstill::Pass as int).
        sgtperform::Standstill g_standstill;
        std::atomic<int>       g_ssSeen{ -1 };
        std::atomic<int>  g_lastTriggerInst{ 0 };  // written at trigger
        RE::FormID g_ssSpell = 0;  // session thread only: ability to strip
        bool g_sgtPauseDeathLogged = false;  // session thread only
        // Delayed end-of-song strip (field round 3): MessageAndEXP is a
        // QUEUED external VM call - RemoveSpell in the same task dispelled
        // the effect before the call could bind its script object, so SGT's
        // own payout (EXP/messages/ovation) never ran (both round-3
        // sessions: expertise unmoved 12s after "payout dispatched").
        // Vanilla runs MessageAndEXP from an in-flight thread that survives
        // the dispel; a queued call has no such grace. So: dispatch the
        // payout at session end, strip 3s later. Session thread only.
        RE::FormID g_endStripSpell = 0;
        double     g_endStripAt    = 0.0;
        // Completed-song payout held until the RESULTS-CLOSE unfreeze
        // (world-pause design 2026-07-20): the results box freezes the
        // world, and a frozen Papyrus VM would leave a song-end payout
        // dispatch QUEUED while the QPC-based 3s end-strip dispelled its
        // effect - the round-3 dead-payout bug reborn - and the gold
        // end-capture window would expire in frozen time. The whole chain
        // (dispatch -> gold end-capture -> strip 3s later) now starts when
        // the freeze lifts. Session thread only; the drop flag crosses
        // from OnPreLoadGame (a loaded save must not receive phantom
        // XP/gold from a song completed in the abandoned timeline).
        struct PendingPayout {
            stars::Instrument inst = stars::Instrument::kLute;
            int               instrumentContext = songeligibility::kLute;
            int               stars     = 0;
            int               feedBonus = 0;
            bool              finishedGreat = false;
        };
        std::optional<PendingPayout> g_pendingPayout;
        std::atomic<bool>            g_dropPendingPayout{ false };
        audience::FailureFeedbackSequence g_failureFeedback;
        int g_failureInstrumentContext = songeligibility::kLute;
        std::atomic<bool> g_failureFeedbackActive{ false };
        std::atomic<bool> g_dropFailureFeedback{ false };
        std::atomic<std::uint64_t> g_failureEpoch{ 0 };
        std::uint64_t g_failureToken = 0;  // session thread only
        // Browse/results world-freeze tracker (session thread only):
        // mirrors the last PostWorldPause the kIdle watcher posted.
        bool g_uiFreeze = false;
        // A completed Electric performance lets its conjured band finish the
        // real-time unsummon presentation before Results pauses the world.
        // Session thread only.
        double g_resultsPublishNotBefore = 0.0;
        // the browser/results window closed on THIS loop iteration; resolved
        // a few lines later into "pick" (cleared by the pick branch) or
        // "cancel" (see the browse-cancel block). Session thread only.
        bool g_uiClosedEdge = false;
        // Post-session control-state probe (field round 3: input dead after
        // closing the results box - unexplained; log-first).
        //
        // Schedule in seconds after session end. The first sample has to land
        // immediately: the symptom IS "cannot open menus", so the only exit
        // left to the player is killing the process, and the 19:02 field run
        // died at +3.61s - 0.89s short of the single +4.5s probe it was
        // carrying. Nothing was wrong with that run; the fuse was simply
        // longer than the patience of someone stuck in a frozen game. Several
        // samples, not one: the read is whether `filtered` is CLIMBING, and a
        // rate needs two points.
        constexpr double kProbeSchedule[] = { 0.0, 0.75, 1.5,  3.0,
                                              5.0, 8.0,  12.0, 20.0 };
        constexpr int    kProbeCount      = 8;
        double g_probeEndAt = 0.0;          // QpcSec() at session end
        int    g_probeIdx   = kProbeCount;  // >= kProbeCount = disarmed
        // control-recovery retry window (session thread). Armed alongside
        // the probes at session end; retries only while the world is
        // unpaused, because the immediate attempt fires under our own pause.
        double g_recoverUntil = 0.0;
        double g_recoverNext  = 0.0;
        // one-shot animation-graph release, armed with the retry window;
        // atomic because the game-thread task consumes it
        std::atomic<bool> g_poseReleasePending{ false };
        // menu open/close logging window after a session end (field round
        // 5: song-end wedge, log truncated before the probes could be
        // read - menu events name any message box / dialogue involved)
        std::atomic<double> g_menuLogUntil{ 0.0 };
        // live crowd mood (spec 2026-07-21 section 5). Session thread only;
        // the writes it produces are posted to the game thread.
        crowd::RockMeter   g_mood;
        crowd::FailureGate g_failureGate;
        // How long the room spent at each committed level (spec 5.6). The
        // PAYOUT gates on the dominant level, not on the one live at the
        // final note - see payout::MoodTally for why. Session thread only,
        // exactly like g_mood, and reset with it.
        payout::MoodTally  g_moodTally;
        // Crowd-punctuation edge detectors, session thread only and reset
        // with g_mood. g_lastCheerMilestone is the highest cheerEveryNotes
        // multiple already cheered for, NOT the raw combo: the reaction site
        // samples at ~10Hz, so it must detect a CROSSING rather than an
        // exact landing (see the comment at the site).
        int                g_lastCheerMilestone = 0;
        int                g_prevCombo          = 0;
        SessionData*       g_s = nullptr;  // session thread only

        // What the LAST session was started with. Session thread only, and
        // written by StartSession itself, so it survives the session it
        // describes. It exists for the P2 practice debug key, which has to
        // name a song before P4's section picker exists; an empty optional
        // means "no session yet this run", and the debug key then falls back
        // to the legacy first-scanned-song path.
        std::optional<bard::SongEntry> g_lastPicked;
        int g_lastDifficulty        = 3;
        int g_lastInstrumentContext = songeligibility::kLute;

        // LocTypeInn, resolved once by editor ID at kDataLoaded. House
        // pattern from MoodGlobals.cpp: resolving by editor ID means a
        // merged or repacked Skyrim.esm still works, and a failure surfaces
        // as a log line instead of a venue test that silently reads false
        // forever. Written before the session thread exists, so later reads
        // from any thread need no synchronization.
        RE::BGSKeyword*       g_locTypeInn     = nullptr;
        constexpr const char* kLocTypeInnEdid  = "LocTypeInn";

        bool AnySpell() {
            for (const auto& spell : g_performSpell) {
                if (spell.load() != 0) { return true; }
            }
            return false;
        }

        RE::FormID PerformSpellForContext(int a_context) {
            if (!songeligibility::IsBoundContext(a_context)) { return 0; }
            return g_performSpell[a_context].load();
        }

        // A detected lesson waiting on a song library that has not finished
        // scanning yet. Game thread only (TryBardTeaching and the two load
        // hooks are the sole touchers - all game-thread). Deliberately does
        // NOT cache the expertise: the rank is re-read at the moment the
        // pick is actually made, so a hold cannot price it off a stale value.
        bool g_teachPending = false;

        // Bard teaching (spec 6.3), GAME THREAD - it reads the SGT globals
        // and ends in DebugNotification, both game-thread only.
        //
        // Paying a bard 100 gold grants +10 expertise, which the star gate
        // would claw straight back at the next enforcement point; the player
        // would have paid for nothing. So a lesson buys a CHART outright
        // instead: the lowest-tier one still locked, falling back to an
        // untaught chart so a max-rank lesson still has a named result. The
        // +10 still lands and is still clamped - teaching buys a song, not
        // a rank.
        //
        // The wiring lives here, not in SgtProgression, on purpose:
        // SgtProgression owns SGT's globals and nothing else, and must not
        // learn about the song library, the ledger or the UI to do it. This
        // file already meets all four.
        void TryBardTeaching(bool a_sessionActive) {
            if (SgtProgression::PollTeachingEdge(a_sessionActive) >= 0) {
                // The edge is one-shot - PollTeachingEdge re-baselines on
                // every sample - so a lesson taken before the library
                // finished scanning would be lost outright. Hold it.
                g_teachPending = true;
            }
            if (!g_teachPending) { return; }
            auto& lib = SongLibrary::GetSingleton();
            lib.EnsureScan();  // once-latch, free after the first call
            const auto snap = lib.Snapshot();
            if (!snap || snap->empty()) {
                spdlog::info(
                    "[unlock] bard lesson held - no charts scanned yet");
                return;  // retried on the next poll
            }
            g_teachPending = false;
            auto&      ledger = StarLedger::GetSingleton();
            const auto inst   = ledger.ActiveInstrument();
            // The EFFECTIVE rank. The ceiling correction and the dev-latch
            // exception live in SgtProgression::EffectiveRank, because the
            // BROWSER has to answer the same number - see its comment.
            //
            // The instrument is the ACTIVE one, not the detector's return:
            // a lesson raises all three, so that is structurally kLute
            // every time, and a flute main (Flute 60, Lute 5) would be
            // priced as rank 1 and sold a chart they can already play.
            //
            // The sample is LIVE, not UiSampled: this is the game thread,
            // and pricing a lesson off a stale snapshot is exactly the
            // drift the hold above exists to avoid.
            const int rank = SgtProgression::EffectiveRank(
                inst, SgtProgression::LiveExpertise(inst));
            std::vector<int>  required;
            std::vector<bool> taught;
            required.reserve(snap->size());
            taught.reserve(snap->size());
            for (const auto& s : *snap) {
                required.push_back(s.requiredRank);
                const bool sameInstrument = songeligibility::IsEligible(
                    s.instrument, static_cast<int>(inst));
                taught.push_back(
                    !sameInstrument ||
                    ledger.Taught(path_text::Utf8(
                        s.entry.folder.filename())));
            }
            const int pick =
                unlock::PickLessonTarget(required, taught, rank);
            if (pick < 0) {
                // Every chart has already been taught. SGT's +10 still
                // lands; there is simply no fresh named lesson to announce.
                spdlog::info(
                    "[unlock] bard lesson: nothing left to teach (rank {}, "
                    "{} charts)", rank, snap->size());
                return;
            }
            const auto& song = (*snap)[static_cast<std::size_t>(pick)];
            const auto  key  =
                path_text::Utf8(song.entry.folder.filename());
            ledger.MarkTaught(key);
            const std::string title = song.name.empty() ? key : song.name;
            const std::string msg =
                unlock::LearnedSongNotification(title, song.instrument);
            RE::DebugNotification(msg.c_str());
            spdlog::info(
                "[unlock] taught \"{}\" (needs rank {}, player rank {})",
                key, required[static_cast<std::size_t>(pick)], rank);
        }

        bool KeyPressedEdge(int vk, bool& held) {
            const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            const bool edge = down && !held;
            held            = down;
            return edge;
        }

        bool GameHwndForeground() {
            const HWND hw = g_gameHwnd.load();
            return !hw || GetForegroundWindow() == hw;
        }

        // ---- main-thread helpers (SKSE task interface) -------------------
        void ResolveLocTypeInn() {  // game thread, kDataLoaded
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) { return; }
            for (auto* k : dh->GetFormArray<RE::BGSKeyword>()) {
                if (!k) { continue; }
                const char* edid = k->GetFormEditorID();
                if (edid && std::strcmp(edid, kLocTypeInnEdid) == 0) {
                    g_locTypeInn = k;
                    break;
                }
            }
            if (g_locTypeInn) {
                spdlog::info("[payout] {} = 0x{:08X}", kLocTypeInnEdid,
                             g_locTypeInn->GetFormID());
            } else {
                spdlog::warn(
                    "[payout] {} not found - every performance will be "
                    "priced as busking", kLocTypeInnEdid);
            }
        }

        void PublishPlayerLocation() {  // game thread only
            if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                if (auto* cell = pc->GetParentCell()) {
                    // store cell LAST: it is the non-zero readiness sentinel
                    // the late-capture reads, so ws/interior must land first
                    g_playerInterior.store(cell->IsInteriorCell());
                    // SGT's gold branch verbatim
                    // (_Talent_PlayInstrument.psc:389-421). Sampled HERE and
                    // not at song end because EndSession runs on the session
                    // thread and may not touch RE at all - and it cannot go
                    // stale within a completed session anyway: the spec-10
                    // guard aborts on any cell change that involves an
                    // interior, and an inn IS an interior, so a session that
                    // reaches the payout ended where it started.
                    const auto* loc = pc->GetCurrentLocation();
                    const bool  atInn =
                        loc && g_locTypeInn && loc->HasKeyword(g_locTypeInn);
                    g_playerAtInn.store(atInn);
                    // Field 2026-07-22: a run inside a tavern still priced as
                    // busking (inn=false), and the payout line alone cannot
                    // tell "no Location record on this cell" from "Location
                    // present but not LocTypeInn" - many interiors carry no
                    // Location at all. Name what we actually read so the next
                    // run settles it without another round trip.
                    spdlog::info(
                        "[payout] venue: location={} keyword={} -> inn={}",
                        loc ? (loc->GetFormEditorID() ? loc->GetFormEditorID()
                                                      : "<no editor id>")
                            : "<none on this cell>",
                        g_locTypeInn ? "resolved" : "MISSING", atInn);
                    const auto* ws = pc->GetWorldspace();
                    g_playerWs.store(ws ? ws->GetFormID() : 0);
                    g_playerCell.store(cell->GetFormID());
                }
            }
        }
        void PostCaptureContext() {
            SKSE::GetTaskInterface()->AddTask([] {
                g_gameHwnd.store(GetForegroundWindow());
                PublishPlayerLocation();
                const auto& st = Settings::GetSingleton();
                Ducking::GetSingleton().Apply(st.duckCurrentMusic,
                                              st.duckAmbience);
            });
        }
        void PostRestoreDucking() {
            SKSE::GetTaskInterface()->AddTask(
                [] { Ducking::GetSingleton().Restore(); });
        }
        // World pause while the session is paused (field round 6 ask).
        // Preferred: show our movie-less kPausesGame ShadowPause menu so
        // the ENGINE does the pause bookkeeping - that is what pauses
        // in-flight voice lines like the Journal does; the old manual
        // numPausesGame bump froze the world but let dialogue keep
        // talking through the pause (field 2026-07-25). The bump remains
        // as the fallback when menu registration failed. The game-thread
        // `held` latch pairs the transitions so no path can leak a pause;
        // Post(false) is called from every teardown path.
        void ApplyWorldPause(bool a_on) {  // game thread only
            static bool held = false;
            auto*       ui   = RE::UI::GetSingleton();
            if (!ui || a_on == held) { return; }
            if (bard::ShadowPause::IsAvailable()) {
                // kShow/kHide land on the next UI-queue drain, so
                // GameIsPaused flips a frame after worldPaused. Benign in
                // both directions: pause-on leaves the capture gate in its
                // unpaused (open) shape for that frame, and pause-off
                // leaks at most one input frame into a world that is
                // still frozen.
                if (a_on) {
                    bard::ShadowPause::Show();
                } else {
                    bard::ShadowPause::Hide();
                }
            } else if (a_on) {
                ui->numPausesGame += 1;
            } else if (ui->numPausesGame > 0) {
                ui->numPausesGame -= 1;
            }
            held = a_on;
            UiBus::GetSingleton().worldPaused.store(a_on);
            spdlog::info("[session] world {} ({})",
                         a_on ? "paused" : "unpaused",
                         bard::ShadowPause::IsAvailable()
                             ? "shadow menu"
                             : "manual bump");
        }
        void PostWorldPause(bool a_on) {
            if (a_on && !Settings::GetSingleton().pauseWorld) { return; }
            SKSE::GetTaskInterface()->AddTask(
                [a_on] { ApplyWorldPause(a_on); });
        }
        // SGT interop (field strand 2026-07-19): a BardHero session outlives
        // SGT's ~33s performance AND swallows its manual exit key all song
        // (Jump = the strum bind). If its perform ability is still on the
        // player at session end, remove it - exactly what SGT's own exit
        // does - so ITS cleanup runs (stop the play idle, stop its sound,
        // EnablePlayerControls). No-op when SGT already cleaned up; the
        // field log proved OUR ControlMap pair balanced while the player
        // stayed rooted at SGT's script/animation layer.
        // a_context: the SESSION's frozen initiation context (the live active
        // progression instrument can drift via a mid-session cast). Remove
        // that exact ability, including the optional guitar ability.
        void PostEndSgtPerformance(int a_context) {
            const auto spell = PerformSpellForContext(a_context);
            if (spell == 0) { return; }
            SKSE::GetTaskInterface()->AddTask([spell] {
                auto* pc = RE::PlayerCharacter::GetSingleton();
                auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(spell);
                if (pc && sp && pc->HasSpell(sp)) {
                    pc->RemoveSpell(sp);
                    spdlog::info(
                        "[session] SGT perform ability still active at "
                        "session end - removed (drives SGT's own exit)");
                    SgtVm::EnablePlayerControlsFallback();
                }
            });
        }

        // Crowd-loss stagger. Does two jobs at once, which is why it is worth
        // having: it is the physical reaction a failed performance was
        // missing, AND the stagger animation locks out the jump that Space -
        // which is also the strum bind - otherwise triggers the instant the
        // session hands input back. A player who fails a song was, until
        // now, quite likely to leap in the air about it.
        //
        // The graph variables are set BEFORE the event on purpose: the
        // behaviour graph samples them when staggerStart is handled, so
        // firing the event first replays whatever magnitude happened to be
        // left over from the last real stagger the player took.
        void PostPlayerStagger(float a_magnitude) {
            SKSE::GetTaskInterface()->AddTask([a_magnitude] {
                auto* pc = RE::PlayerCharacter::GetSingleton();
                if (!pc) { return; }
                // staggerDirection is a 0..1 fraction of a turn, NOT degrees.
                // 0 reads as a stagger straight back from what is in front of
                // the player, which is the crowd.
                pc->SetGraphVariableFloat("staggerDirection", 0.0f);
                pc->SetGraphVariableFloat("staggerMagnitude", a_magnitude);
                // recoilStart, NOT staggerStart. The small stagger leaves
                // enough control that the player could still jump out of it
                // (field 2026-07-26), which defeats half the reason this
                // exists - Space is both strum and Jump, so a failed song
                // was ending with a hop. recoilLargeStart is the heavier
                // recoil-family event; recoilStart is the fallback, and
                // staggerStart the last resort, so a graph that lacks the
                // heavier ones still produces SOME reaction rather than
                // none.
                const bool fired =
                    pc->NotifyAnimationGraph("recoilLargeStart") ||
                    pc->NotifyAnimationGraph("recoilStart") ||
                    pc->NotifyAnimationGraph("staggerStart");
                spdlog::info(
                    "[failure] player stagger: fired={} magnitude={:.2f}",
                    fired, a_magnitude);
            });
        }

        // Whole-song completed end: bracket SGT's own payout with the tier
        // promotion (spec 6), then RemoveSpell so its cleanup runs. The
        // feed lands in SgtProgression::FinishPayout after the gold
        // end-capture window closes (spec 4).
        void PostSgtFinishPerformance(int a_context,
                                      stars::Instrument a_inst, int a_stars,
                                      int a_feedBonus,
                                      bool a_finishedGreat) {
            const auto spell = PerformSpellForContext(a_context);
            if (spell == 0) {
                // Believed unreachable - Confirmed() requires the keeper to
                // have seen this same spell - but it is the ONE exit from
                // here that touches neither OpenEndCapture nor
                // CancelDeferred, so it would leave g_defArmed set until the
                // next OnSessionStart cleans up after it. Close the arm.
                GoldScale::CancelDeferred();
                return;
            }
            SKSE::GetTaskInterface()->AddTask([spell, a_inst, a_stars,
                                               a_feedBonus,
                                               a_finishedGreat] {
                const auto& se = Settings::GetSingleton();
                // Read the player's REAL standing before the promotion
                // bracket arms - it is what picks which of SGT's message
                // tiers the reaction is worded from, and a promoted value
                // would make a struggling bard read as a master.
                const int expertise =
                    SgtProgression::LiveExpertise(a_inst);
                stars::PromotionParams pp{ se.promote4Floor,
                                           se.promote5Floor };
                // The tier promotion exists ONLY to push SGT's MessageAndEXP
                // into a more generous expertise branch. When we run the
                // ending ourselves that branch is never taken, so there is
                // nothing to promote for - but BeginPromotion still has to
                // be called, because it arms the bracket FinishPayout needs
                // to apply the experience feed.
                const int floorVal =
                    (se.ownEnding || !se.tierPromotion)
                        ? 0
                        : stars::PromotionFloor(a_stars, pp);
                SgtProgression::BeginPromotion(a_inst, floorVal);
                // Experience: SGT's own RandomInt(1,3) only lands if we call
                // MessageAndEXP, so when we do not, ours is the only grant.
                const ending::XpParams xp{ se.xpPerStar, se.xpBonus5 };
                const int              feed =
                    a_feedBonus + (se.ownEnding
                                       ? ending::PerformanceXp(a_stars, xp)
                                       : 0);
                const bool dispatched =
                    se.ownEnding
                        ? SgtVm::DispatchEnding(spell, a_stars, expertise,
                                                a_finishedGreat)
                        : SgtVm::DispatchPayout(spell);
                if (dispatched) {
                    GoldScale::OpenEndCapture();
                    SgtProgression::NoteSessionPayout(
                        a_inst, feed,
                        QpcSec() + se.sgtEndCaptureSec + 2.0);
                    spdlog::info("[sgt] end-of-song payout dispatched");
                } else {
                    // OPEN the window anyway, do not cancel the arm. The
                    // performance top-up (spec 5.6) needs nothing from SGT's
                    // payout - it exists precisely to pay for runs SGT will
                    // not pay for - so cancelling here forfeited SGT's gold
                    // AND ours on the one path where ours matters most. With
                    // the window open TickDeferred still fires, captures
                    // nothing, and pays the shortfall against observed = 0.
                    // NoteSessionPayout is still skipped: FinishPayout runs
                    // now, because no payout is coming to bracket.
                    GoldScale::OpenEndCapture();
                    SgtProgression::FinishPayout(a_inst, feed);
                    spdlog::warn(
                        "[sgt] payout dispatch failed - no effect/script "
                        "at session end");
                    spdlog::info(
                        "[sgt] end-capture window opened anyway - the "
                        "performance top-up does not depend on SGT's "
                        "payout dispatch");
                }
                // RemoveSpell deliberately NOT here (field round 3): the
                // queued MessageAndEXP call must bind its script object
                // while the effect is alive. The session thread strips 3s
                // later (g_endStripSpell).
            });
        }

        // Session thread: fire a held completed-song payout (see
        // g_pendingPayout) and arm the 3s end-strip behind it. Runs at the
        // results-close unfreeze, at a song pick that supersedes the box,
        // or not at all (dropped on save load).
        void DispatchPendingPayout(const char* a_when) {
            if (!g_pendingPayout) { return; }
            // a load can set the drop flag between the loop's drop-check
            // and this dispatch (same iteration) - honor it here too
            if (g_dropPendingPayout.exchange(false)) {
                g_pendingPayout.reset();
                spdlog::info(
                    "[sgt] held payout dropped (save load raced the "
                    "dispatch)");
                return;
            }
            const auto pp = *g_pendingPayout;
            g_pendingPayout.reset();
            PostSgtFinishPerformance(pp.instrumentContext, pp.inst, pp.stars,
                                     pp.feedBonus,
                                     pp.finishedGreat);
            g_endStripSpell = PerformSpellForContext(pp.instrumentContext);
            g_endStripAt = QpcSec() + 3.0;
            spdlog::info("[sgt] held payout dispatched ({})", a_when);
        }

        // ---- menu sink (game thread) -------------------------------------
        class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
        public:
            static MenuSink* GetSingleton() {
                static MenuSink s;
                return &s;
            }
            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent* ev,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                // Our own ShadowPause menu is pause INFRASTRUCTURE, not a
                // native menu: counting it in g_nativeMenus would pin
                // native-menu recovery on for the whole pause, and its
                // open edge (GameIsPaused just went true) would re-request
                // the session pause that opened it.
                if (ev && ev->menuName.c_str()
                              == bard::ShadowPause::kMenuName) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (ev) {
                    const std::string name = ev->menuName.c_str();
                    // Forced dialogue is sampled at EVERY state (2026-07-27):
                    // the browse phase lives inside kIdle, and its guard
                    // needs this fresh - a guard's arrest dialogue during
                    // the songbook's sheathe-hold window deadlocked the
                    // world when the delayed browse freeze landed on top
                    // of it. ONLY dialogue: the trigger flow itself churns
                    // inventory/favorites menus while opening the browser,
                    // and a first cut that watched the whole non-passive
                    // tracker at idle read those closing transients as
                    // ownership loss and killed every legitimate browse
                    // (field, same day).
                    if (name == "Dialogue Menu") {
                        g_dialogueOpen.store(ev->opening,
                                             std::memory_order_release);
                    }
                    if (g_state.load() != State::kIdle) {
                        std::scoped_lock lk(g_menuTrackMx);
                        g_nativeMenus.Observe(name, ev->opening);
                        g_nativeMenuOpen.store(g_nativeMenus.Open(),
                                               std::memory_order_release);
                    }
                }
                // post-session wedge diagnostics (field round 5): runs
                // BEFORE the kIdle early-out - the wedge window IS kIdle
                if (ev && QpcSec() < g_menuLogUntil.load()) {
                    spdlog::info("[probe] menu {} {}", ev->menuName.c_str(),
                                 ev->opening ? "opened" : "closed");
                }
                if (!ev || g_state.load() == State::kIdle) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const bool isLoading =
                    ev->menuName == RE::LoadingMenu::MENU_NAME;
                if (ev->opening) {
                    if (isLoading) g_inLoadingMenu.store(true);
                    auto* ui = RE::UI::GetSingleton();
                    // any pausing menu or a load screen: the player cannot
                    // see/hear the session sensibly (spec 8/10)
                    if (isLoading || (ui && ui->GameIsPaused())) {
                        g_reqPause.store(true);
                    }
                } else if (isLoading) {
                    g_inLoadingMenu.store(false);
                    // load finished: publish the (possibly new) player
                    // location; the session thread compares it against the
                    // start capture and aborts on a scene change (spec 10
                    // refined), else the session stays paused for a manual
                    // resume
                    PublishPlayerLocation();
                }
                // resume gate (M2 review): GameIsPaused flips exactly at
                // menu open/close edges, and this sink runs on the game
                // thread, so the RE:: call is legal here - the session
                // thread only reads the sampled atomic. Our own world
                // pause makes GameIsPaused true WITHOUT a menu - subtract
                // it, else the resume gate wedges shut (field round 6).
                if (auto* ui = RE::UI::GetSingleton()) {
                    g_uiPaused.store(
                        ui->GameIsPaused() &&
                        !UiBus::GetSingleton().worldPaused.load());
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // ---- spell-cast sink (game thread): perform-power start trigger ---
        // The player casting the configured spell/power requests a start,
        // exactly like the start key. Only requests - the session thread owns
        // the kIdle gate, so a mid-session cast never pauses/resumes/aborts.
        class SpellSink : public RE::BSTEventSink<RE::TESSpellCastEvent> {
        public:
            static SpellSink* GetSingleton() {
                static SpellSink s;
                return &s;
            }
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESSpellCastEvent* ev,
                RE::BSTEventSource<RE::TESSpellCastEvent>*) override {
                // pre-guard diagnostic (field 2026-07-19): the no-browser
                // report showed ZERO events reaching the player guard for a
                // lute activation - if the engine ever emits a cast event
                // for a perform spell at all, record it with its caster
                if (ev) {
                    for (int i = 0;
                         i < songeligibility::kInstrumentContextCount; ++i) {
                        const auto sp = g_performSpell[i].load();
                        if (sp != 0 && ev->spell == sp) {
                            spdlog::info(
                                "[session] perform-spell cast event: "
                                "spell=0x{:X} object=0x{:X} player={}",
                                ev->spell,
                                ev->object ? ev->object->GetFormID() : 0,
                                ev->object && ev->object->IsPlayerRef());
                            break;
                        }
                    }
                }
                if (!ev || !ev->object || !ev->object->IsPlayerRef()) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                bool matched = false;
                for (int i = 0;
                     i < songeligibility::kInstrumentContextCount; ++i) {
                    const auto spell = g_performSpell[i].load();
                    if (spell != 0 && ev->spell == spell) {
                        g_lastTriggerInst.store(i);
                        const auto progression =
                            static_cast<stars::Instrument>(
                                songeligibility::ProgressionContext(i));
                        StarLedger::GetSingleton().SetActiveInstrument(
                            progression);
                        SgtProgression::NoteCast(progression, QpcSec());
                        GoldScale::NotePerformCast();
                        g_reqStart.store(true);
                        matched = true;
                        break;
                    }
                }
                // field diagnostic (2026-07-19 no-browser report): the
                // trigger path is otherwise silent until the browser first
                // draws - log the boundary so one repro localizes a break
                if (matched) {
                    spdlog::info(
                        "[session] player cast 0x{:X} - perform trigger, "
                        "start requested",
                        ev->spell);
                } else if (Settings::GetSingleton().verboseLog) {
                    spdlog::info("[session] player cast 0x{:X} (no match)",
                                 ev->spell);
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // Primary trigger (field 2026-07-19): SGT applies the perform
        // ability via a plain AddSpell in _Talent_Givespell.psc, which does
        // NOT emit TESSpellCastEvent - the cast sink above never fired in
        // the field. Poll the ability's presence on the player at idle and
        // treat an ABSENT -> PRESENT edge as the cast. Game thread only
        // (SKSE task); the edge latch means a lingering ability (browser
        // cancelled, SGT still busking) cannot re-trigger.
        // Shared arming latch for BOTH trigger sources - the synchronous
        // AddTarget hook and this poll. Game thread only, so no lock.
        performtrigger::Arming g_arming;

        // Native start strips the perform ability at the AddTarget hook.
        // But WE re-add that same ability deliberately at song pick and at
        // pause-resume, and the hook fires on those too - field 2026-07-20:
        // `performance started (AddSpell at song pick)` was followed in the
        // SAME MILLISECOND by `stripped before OnEffectStart`, so the
        // performance we were starting died instantly and the player never
        // played the lute. Suppress the strip for our own adds.
        //
        // AddTarget runs synchronously inside AddSpell on this same thread,
        // so a plain flag spanning the call is sufficient - no queue, no
        // race. RAII so an early return or throw cannot strand it set.
        std::atomic<bool> g_selfAdd{ false };

        struct SelfAddGuard {
            SelfAddGuard() { g_selfAdd.store(true); }
            ~SelfAddGuard() { g_selfAdd.store(false); }
            SelfAddGuard(const SelfAddGuard&)            = delete;
            SelfAddGuard& operator=(const SelfAddGuard&) = delete;
        };

        // The instrument is equipped FROM the inventory, so that menu (and
        // the TweenMenu behind it) is still up when the trigger fires. Our
        // browser is kCloseOnGameMenu, so it stays HIDDEN behind them and
        // the player had to close both by hand before they could pick a
        // song. Close them instead. Posted as a task: the trigger arrives
        // from inside the engine's equip processing.
        void PostCloseInventory() {
            if (!Settings::GetSingleton().closeInventoryOnTrigger) { return; }
            SKSE::GetTaskInterface()->AddTask([] {
                auto* ui = RE::UI::GetSingleton();
                auto* q  = RE::UIMessageQueue::GetSingleton();
                if (!ui || !q) { return; }
                int closed = 0;
                // innermost first - hiding InventoryMenu drops back to the
                // TweenMenu that opened it, so both have to go
                for (const auto* name : { RE::InventoryMenu::MENU_NAME.data(),
                                          "TweenMenu" }) {
                    if (ui->IsMenuOpen(name)) {
                        q->AddMessage(name, RE::UI_MESSAGE_TYPE::kHide,
                                      nullptr);
                        ++closed;
                    }
                }
                if (closed) {
                    spdlog::info(
                        "[session] perform trigger closed {} inventory "
                        "menu(s) - the browser was hidden behind them",
                        closed);
                }
            });
        }

        // Was a weapon drawn the last time the game thread looked? Written
        // by the browse-sheathe task and refreshed by every poll pass (both
        // game thread); read by StartSession on the session thread to size
        // the audio lead-in. Seeded true so a trigger that somehow never
        // observed the player errs toward the LONGER lead - a slightly late
        // first note is a far smaller defect than an early one.
        std::atomic<bool> g_weaponDrawn{ true };

        // ---- browse-time sheathe -----------------------------------------
        //
        // THIRD ATTEMPT, and the first two failed for reasons that had
        // nothing to do with which weapon states to test. The field log for
        // 2026-07-26 settled both:
        //
        //   [16:07:02.796] camera ready before open request published
        //   [16:07:02.796] weapons still not sheathed after 240 passes
        //                  (0 re-asserts, weaponState=5)
        //   [16:07:02.811] world paused (shadow menu)
        //   [16:07:03.906] world unpaused
        //
        // 1. A TASK THAT RE-ADDS ITSELF DOES NOT YIELD A GAME FRAME. All
        //    240 passes ran inside ONE millisecond, so the retry never
        //    observed a later frame and the animation could not advance
        //    between passes. (The camera-prep retry beside it burned 13
        //    passes the same way in the same millisecond - same bug class.)
        //    Pacing must come from real time, which is why the watch below
        //    is a detached sleeper posting one task per tick - the idiom
        //    BandStage::EndNow already uses for its dismissal delay.
        //
        // 2. THE BROWSE FREEZE LANDS 15ms AFTER THE REQUEST AND HOLDS FOR
        //    THE WHOLE BROWSE. A frozen world does not advance an
        //    animation, so no amount of correct retrying inside that window
        //    can finish a ~1s sheathe. The weapon therefore arrived at the
        //    session pinned at 5 (kSheathing) - and IsWeaponDrawn() reports
        //    TRUE for 5, which is why the lead-in widened to 2.75s "because
        //    SGT will pay its 2s sheathe branch" when native start had
        //    already stripped SGT's flow. The idle was then requested
        //    against a drawn weapon, the graph rejected it, and the keeper
        //    re-sent IdleLuteStart 88 times over the next 56 seconds
        //    without ever taking: the player performed the whole song stuck
        //    in the weapon idle.
        //
        // So the fix is not a better predicate. It is to give the sheathe
        // LIVE WORLD TIME by holding the browse freeze off until the weapon
        // is actually away - capped, because a wedged weapon state must
        // never leave the world running under an open Songbook forever.
        constexpr auto   kSheatheTick     = std::chrono::milliseconds(50);
        constexpr int    kSheatheTicks    = 40;    // 2.0s of REAL time
        constexpr double kSheatheHoldSec  = 2.0;   // must match the above

        // Game thread -> session thread (the kIdle freeze watcher). Seeded
        // true so any path that never arms a watch cannot hold the freeze
        // off.
        std::atomic<bool>          g_weaponsSettled{ true };
        std::atomic<double>        g_sheatheHoldUntil{ 0.0 };
        std::atomic<std::uint64_t> g_sheatheWatchGen{ 0 };

        // ---- streak fire -------------------------------------------------
        //
        // Guitar Hero's fire, on Skyrim's own flame effects. Local FormIDs
        // read out of Skyrim.esm rather than guessed.
        //
        // THE ART OBJECT, FROM THE ACTOR ROOT, ONE BURST PER MILESTONE.
        //
        // FireCloakHandEffects is the art vanilla Flame Cloak puts on the
        // hand nodes, and it is the only thing tried so far that produced
        // visible flames. Substituting effect shaders lost the flames
        // entirely AND kept the noise, which also proved the noise is not
        // this NIF - see StreakFireLogic.h for the full account and for why
        // the sustained-cloak design is gone.
        //
        // ---- A SUSTAINED FLAME CLOAK, ON AN EFFECT SHADER ----------------
        //
        // User ask, twice: a flame cloak on the whole body, silent, that
        // stays on. All three fall out of using an EFFECT SHADER rather than
        // an art object - a shader applies over the whole MODEL rather than
        // at a node, and an EFSH record carries ICON/ICO2/NAM7-9 texture
        // paths and a DATA blob and NOTHING ELSE. There is no sound field on
        // one, verified by dumping both record types (STATUS.md), so this is
        // silent by construction rather than by muting, and needs no custom
        // asset. Silence is also what makes SUSTAINED safe: the whole reason
        // the original cloak was abandoned was a looping fire sound that
        // came from the art object's NIF, and the art object is now gone.
        //
        // StreakFireLogic.h owns the cadence and carries the six-round field
        // account of why the ARTO is beyond rescue on the player. The short
        // version: it drew nothing across every attach node, cadence and
        // count tried, while a control firing SummonTargetFX at the player
        // in the same burst WAS visible - so effects do render on the
        // player, and the shader is simply the mechanism that works here.
        //
        // ⚠ Round 5 also established that InstantiateHitArt/HitShader return
        // 0x1 on 1.6.1170 - a constant, not a pointer. See the call site.
        constexpr auto       kSkyrimEsm            = "Skyrim.esm";
        constexpr RE::FormID kFireCloakShaderLocal = 0x02ACD8;

        streakfire::State  g_streakFire;              // session thread only
        streakfire::Params g_streakFireParams;


        void ResetStreakFire() { g_streakFire.Reset(); }

        // Applied on every kApply - both the first light and each refresh.
        // a_first only distinguishes them in the LOG: a refresh at ~1.25Hz
        // would otherwise bury the log in identical lines, so the refreshes
        // are logged at debug and only the ignition at info.
        void PostStreakCloak(bool a_first) {
            const float secs =
                static_cast<float>(g_streakFireParams.holdSec);
            SKSE::GetTaskInterface()->AddTask([secs, a_first] {
                auto* pc   = RE::PlayerCharacter::GetSingleton();
                auto* data = RE::TESDataHandler::GetSingleton();
                if (!pc || !data) { return; }
                auto* shader = data->LookupForm<RE::TESEffectShader>(
                    kFireCloakShaderLocal, kSkyrimEsm);
                if (!shader) {
                    spdlog::warn("[fire] cloak shader 0x{:06X} MISSING",
                                 kFireCloakShaderLocal);
                    return;
                }

                // ⚠ "AN EFSH RECORD CARRIES NO SOUND FIELD" WAS WRONG, and it
                // is written down as fact in the 2026-07-26 handoff - it is
                // the stated reason a SUSTAINED cloak was thought safe.
                // TESEffectShader::EffectShaderData has `ambientSound`
                // (BGSSoundDescriptorForm*, 0x140), and FireCloakFXShader
                // sets it. Field 2026-07-27, on a second machine: the fire is
                // audibly annoying. The record dump missed a field; the ear
                // did not.
                //
                // The 0.8s refresh against a 1.6s hold is what turns it from
                // present into grating - each refresh re-triggers the loop.
                //
                // Cleared ONCE rather than saved/restored around the call: a
                // shader effect starts its ambient sound when it first TICKS,
                // not synchronously inside InstantiateHitShader, so restoring
                // straight afterwards would hand the sound back before
                // anything had read the null.
                //
                // The cost, stated plainly: this is a VANILLA record, so the
                // clear also drops the ambient layer from anything else using
                // FireCloakFXShader this session - vanilla Flame Cloak
                // included. That spell keeps its magic-effect sounds, so it
                // goes thinner rather than silent. If that ever bites, the
                // fix is our own EFSH copy, not putting this back.
                static bool s_ambientCleared = false;
                if (!s_ambientCleared) {
                    s_ambientCleared = true;
                    if (shader->data.ambientSound) {
                        spdlog::info(
                            "[fire] clearing FireCloakFXShader ambientSound "
                            "(0x{:08X}) - the cloak is silent from here",
                            shader->data.ambientSound->GetFormID());
                        shader->data.ambientSound = nullptr;
                    } else {
                        spdlog::info(
                            "[fire] FireCloakFXShader has no ambientSound - "
                            "the noise is coming from somewhere else");
                    }
                }

                // ⚠ NEVER DEREFERENCE WHAT THIS RETURNS. CommonLibSSE types
                // it as an effect pointer, but on 1.6.1170 the sibling call
                // InstantiateHitArt came back as literally 0x1 - reading
                // `artObject3D` (offset 0xC8) faulted on 0xC9 and took the
                // game down, field 2026-07-26. Whatever the native call
                // leaves in RAX, it is not a pointer we own. Log the raw
                // VALUE, never follow it, and treat the truthiness of
                // `!= nullptr` as meaningless.
                const auto ret = reinterpret_cast<std::uintptr_t>(
                    pc->InstantiateHitShader(shader, secs, pc, false, false));
                if (a_first) {
                    spdlog::info(
                        "[fire] cloak LIT ({:.2f}s hold, refresh {:.2f}s) - "
                        "ret=0x{:X}",
                        secs, g_streakFireParams.refreshSec, ret);
                } else {
                    spdlog::debug("[fire] cloak refresh - ret=0x{:X}", ret);
                }
            });
        }

        // Put the cloak OUT, now.
        //
        // While a song runs the cloak is deliberately allowed to LAPSE on a
        // streak break rather than being cancelled - that is what stops a
        // held streak flickering between refreshes. At session end the same
        // design leaves the player burning for up to holdSec after the
        // minigame is over, and a refresh landing just before the last note
        // makes that the common case rather than the rare one. Field
        // 2026-07-27: "it can persist".
        //
        // Targeted, NOT ProcessLists::StopAllMagicEffects: that would end
        // every other magic effect on the player - their own buffs, cloaks
        // and visuals - to put out a fire we lit. We stop exactly the
        // instances whose shader is ours and whose target is the player.
        void StopStreakCloak() {
            SKSE::GetTaskInterface()->AddTask([] {
                auto* pc    = RE::PlayerCharacter::GetSingleton();
                auto* data  = RE::TESDataHandler::GetSingleton();
                auto* procs = RE::ProcessLists::GetSingleton();
                if (!pc || !data || !procs) { return; }
                auto* shader = data->LookupForm<RE::TESEffectShader>(
                    kFireCloakShaderLocal, kSkyrimEsm);
                if (!shader) { return; }

                int stopped = 0;
                procs->ForEachMagicTempEffect(
                    [&](RE::BSTempEffect& a_effect) {
                        auto* fx =
                            skyrim_cast<RE::ShaderReferenceEffect*>(&a_effect);
                        if (fx && fx->effectData == shader &&
                            fx->target.get().get() == pc) {
                            // `finished` is what the engine reads to retire
                            // the effect on its next pass. Detach() is not
                            // called here: tearing an effect down from
                            // inside the engine's own iteration over that
                            // list is not ours to do.
                            fx->finished = true;
                            ++stopped;
                        }
                        return RE::BSContainer::ForEachResult::kContinue;
                    });
                if (stopped > 0) {
                    spdlog::info("[fire] cloak stopped at session end "
                                 "({} instance(s))", stopped);
                }
            });
        }

        void PublishWeaponDrawn(RE::PlayerCharacter* a_pc);

        // One watch tick, on the game thread. Terminates on weaponState 0
        // and NOTHING else: states 4/5 mean a sheathe has merely STARTED,
        // and an in-flight draw can still win and put the weapon back out,
        // so 4/5 keep watching WITHOUT re-requesting (re-requesting a
        // sheathe that is already running can re-trigger the animation).
        // NextSheatheStep owns that three-way split (suite 20).
        void SheatheWatchPass(std::uint64_t a_gen, int a_tick,
                              int a_reasserts) {
            if (a_gen != g_sheatheWatchGen.load(std::memory_order_acquire)) {
                return;  // a newer browse open owns the watch
            }
            auto* pc = RE::PlayerCharacter::GetSingleton();
            auto* st = pc ? pc->AsActorState() : nullptr;
            if (!pc || !st) { return; }
            const int  ws   = static_cast<int>(st->GetWeaponState());
            using Step      = performtrigger::SheatheStep;
            const Step step = performtrigger::NextSheatheStep(ws);
            if (step == Step::kSettled) {
                // Publish BEFORE releasing the hold: the lead-in reads
                // g_weaponDrawn, and IsWeaponDrawn() is true for kSheathing,
                // so a pick landing on this frame must see the settled truth
                // rather than widen itself by 2.25s for a branch nobody is
                // going to run.
                PublishWeaponDrawn(pc);
                g_weaponsSettled.store(true, std::memory_order_release);
                spdlog::info(
                    "[browser] weapons settled (weaponState=0) after {} "
                    "ticks of live world, {} re-asserts - the browse freeze "
                    "is released",
                    a_tick, a_reasserts);
                return;
            }
            if (step == Step::kReassert) {
                pc->DrawWeaponMagicHands(false);
                // Keyed on the re-assert count, not the tick: tick one can
                // land on 4/5 and issue nothing, and the first real request
                // is the one worth a line.
                if (a_reasserts == 0) {
                    spdlog::info(
                        "[browser] sheathing player weapons for the "
                        "performance (weaponState={})",
                        ws);
                }
            }
        }

        // Real-time paced, and that is the whole point - see the block above
        // this file's kSheatheTick. One detached sleeper, one game-thread
        // task per tick, so consecutive samples are actually separated by
        // frames in which the animation can advance.
        void StartSheatheWatch(std::uint64_t a_gen) {
            std::thread([a_gen] {
                int reasserts = 0;
                for (int tick = 1; tick <= kSheatheTicks; ++tick) {
                    std::this_thread::sleep_for(kSheatheTick);
                    if (a_gen != g_sheatheWatchGen.load(
                                     std::memory_order_acquire)) {
                        return;
                    }
                    if (g_weaponsSettled.load(std::memory_order_acquire)) {
                        return;
                    }
                    SKSE::GetTaskInterface()->AddTask(
                        [a_gen, tick, reasserts] {
                            SheatheWatchPass(a_gen, tick, reasserts);
                        });
                    // Counted on THIS side because the tasks run later and
                    // cannot report back without another round trip. It only
                    // feeds a log line, so an off-by-one at the boundary
                    // costs nothing.
                    ++reasserts;
                }
                if (a_gen == g_sheatheWatchGen.load(std::memory_order_acquire)
                    && !g_weaponsSettled.load(std::memory_order_acquire)) {
                    // Release the hold regardless. A weapon state that never
                    // reaches 0 in two seconds of live world is wedged, and
                    // an open Songbook over a RUNNING world is the worse
                    // failure - the player can be attacked while browsing.
                    g_weaponsSettled.store(true, std::memory_order_release);
                    spdlog::warn(
                        "[browser] weapons still not sheathed after {:.1f}s "
                        "of live world - releasing the browse freeze anyway; "
                        "the instrument idle may be rejected",
                        kSheatheHoldSec);
                }
            }).detach();
        }

        // Called on the game-thread pass that is about to publish the
        // Songbook open, BEFORE the publication itself so the freeze watcher
        // can never sample browserOpen=true with the hold not yet armed.
        void BeginBrowseSheathe() {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            auto* st = pc ? pc->AsActorState() : nullptr;
            if (!pc || !st) {
                g_weaponsSettled.store(true, std::memory_order_release);
                return;
            }
            const int ws = static_cast<int>(st->GetWeaponState());
            // The COMMON case by a wide margin (7 of 8 samples in the
            // 2026-07-26 log): nothing was drawn, so nothing is held off and
            // the browse freezes immediately as it always did.
            if (performtrigger::NextSheatheStep(ws) ==
                performtrigger::SheatheStep::kSettled) {
                g_weaponsSettled.store(true, std::memory_order_release);
                return;
            }
            const auto gen =
                g_sheatheWatchGen.fetch_add(1, std::memory_order_acq_rel) + 1;
            g_weaponsSettled.store(false, std::memory_order_release);
            g_sheatheHoldUntil.store(QpcSec() + kSheatheHoldSec,
                                     std::memory_order_release);
            spdlog::info(
                "[browser] weaponState={} at Songbook open - holding the "
                "browse freeze up to {:.1f}s so the sheathe can actually "
                "finish",
                ws, kSheatheHoldSec);
            StartSheatheWatch(gen);
        }

        void PublishWeaponDrawn(RE::PlayerCharacter* a_pc) {
            if (auto* st = a_pc ? a_pc->AsActorState() : nullptr) {
                g_weaponDrawn.store(st->IsWeaponDrawn());
            }
        }

        // Camera to third person immediately BEFORE Songbook publication,
        // not first at pick (field 2026-07-20): starting a song from first
        // person produced a lute
        // idle with an INVISIBLE lute, while a third-person start showed it
        // - user-confirmed discriminator. The visible instrument is an
        // AnimObject the graph attaches when the idle starts; SGT's
        // ForceThirdPerson runs ~0.2s before its PlayIdle, so the idle
        // begins inside the 1st->3rd transition and the attach is lost.
        // Re-upping the same idle later does not re-attach (already-active
        // idle = no restart). Moving the swap to browse open gives the
        // camera the whole browse to settle - the same shape as the browse
        // sheathe - and SGT's own camera branch then never fires (it checks
        // GetCameraState()==0, already false by pick).
        //
        // Remembered so the session END can restore first person: SGT's own
        // restore sits after the Wait(2.5) latent where its cleanup dies,
        // so it never runs in our flow - we own both directions or neither.
        std::atomic<bool> g_wasFirstPerson{ false };
        browsecamera::OpenGate g_browseOpenGate;
        std::atomic<std::uint64_t> g_performanceVanityGeneration{ 0 };

        browsecamera::PerformanceCameraKind PerformanceCameraKindFor(
            RE::PlayerCamera* a_camera) {
            using Kind = browsecamera::PerformanceCameraKind;
            if (!a_camera ||
                !a_camera->cameraStates[RE::CameraStates::kAutoVanity]) {
                return Kind::kUnavailable;
            }
            if (!a_camera->currentState) { return Kind::kTransition; }
            switch (a_camera->currentState->id) {
                case RE::CameraStates::kFirstPerson:
                    return Kind::kFirstPerson;
                case RE::CameraStates::kAutoVanity:
                    return Kind::kAutoVanity;
                case RE::CameraStates::kThirdPerson:
                    return Kind::kThirdPerson;
                default:
                    return Kind::kTransition;
            }
        }

        void RunPerformanceVanity(std::uint64_t a_token,
                                  const char* a_source, int a_attempt) {
            const bool current =
                g_performanceVanityGeneration.load(
                    std::memory_order_acquire) == a_token;
            const bool active =
                EngineFeed::GetSingleton().active.load(
                    std::memory_order_acquire) &&
                g_state.load() != State::kIdle;
            const bool enabled =
                Settings::GetSingleton().performanceVanityCamera;
            auto* cam = RE::PlayerCamera::GetSingleton();
            const auto kind = PerformanceCameraKindFor(cam);
            using Action = browsecamera::PerformanceVanityAction;
            const auto action = browsecamera::PlanPerformanceVanity(
                current, active, enabled, kind, a_attempt);

            auto retry = [a_token, a_source, a_attempt] {
                SKSE::GetTaskInterface()->AddTask(
                    [a_token, a_source, a_attempt] {
                        RunPerformanceVanity(a_token, a_source,
                                             a_attempt + 1);
                    });
            };

            switch (action) {
                case Action::kCancel:
                case Action::kDone:
                    return;
                case Action::kForceThirdPerson:
                    cam->ForceThirdPerson();
                    retry();
                    return;
                case Action::kEnterAutoVanity: {
                    auto* autoVanity =
                        cam->cameraStates[RE::CameraStates::kAutoVanity].get();
                    if (!autoVanity) {
                        retry();
                        return;
                    }
                    cam->SetState(autoVanity);
                    const int id = cam->currentState
                                       ? static_cast<int>(
                                             cam->currentState->id)
                                       : -1;
                    spdlog::info(
                        "[camera] performance vanity requested ({}; "
                        "attempt={} state={})",
                        a_source, a_attempt + 1, id);
                    return;
                }
                case Action::kRetry:
                    retry();
                    return;
                case Action::kGiveUp:
                    spdlog::warn(
                        "[camera] performance vanity unavailable after {} "
                        "attempts ({}; state={})",
                        a_attempt,
                        a_source,
                        cam && cam->currentState
                            ? static_cast<int>(cam->currentState->id)
                            : -1);
                    return;
            }
        }

        void PostBeginPerformanceVanity(const char* a_source) {
            if (!Settings::GetSingleton().performanceVanityCamera) { return; }
            const auto token =
                g_performanceVanityGeneration.fetch_add(
                    1, std::memory_order_acq_rel) + 1;
            SKSE::GetTaskInterface()->AddTask(
                [token, a_source] {
                    RunPerformanceVanity(token, a_source, 0);
                });
        }

        void PostEndPerformanceVanity(const char* a_source) {
            const auto token =
                g_performanceVanityGeneration.fetch_add(
                    1, std::memory_order_acq_rel) + 1;
            SKSE::GetTaskInterface()->AddTask([token, a_source] {
                if (g_performanceVanityGeneration.load(
                        std::memory_order_acquire) != token) {
                    return;
                }
                auto* cam = RE::PlayerCamera::GetSingleton();
                if (!cam || !cam->currentState ||
                    cam->currentState->id !=
                        RE::CameraStates::kAutoVanity) {
                    return;
                }
                cam->ForceThirdPerson();
                spdlog::info(
                    "[camera] performance vanity ended ({})", a_source);
            });
        }

        // Log EVERY outcome, both branches. The silent early-return cost two
        // diagnoses: `browse camera` absent was read as "the fix never ran"
        // and then as "the run started in third person", and neither could
        // be told apart from the log. Field 18:23 settles it - the player
        // was in FIRST person, the lute was invisible, and this still
        // logged nothing, so IsInFirstPerson() read false at the trigger.
        // The trigger fires from inside the equip, while the inventory menu
        // is still up, which is exactly where that sample is unreliable.
        void PostForceThirdPerson(const char* a_when) {
            SKSE::GetTaskInterface()->AddTask([a_when] {
                auto* cam = RE::PlayerCamera::GetSingleton();
                if (!cam) { return; }
                const int id = cam->currentState
                                   ? static_cast<int>(cam->currentState->id)
                                   : -1;
                if (!cam->IsInFirstPerson()) {
                    spdlog::info(
                        "[sgt] browse camera: no force at {} "
                        "(IsInFirstPerson=false, cam={})",
                        a_when, id);
                    return;
                }
                g_wasFirstPerson.store(true);
                cam->ForceThirdPerson();
                spdlog::info(
                    "[sgt] browse camera: forced third person at {} (cam was "
                    "{}); first person restored at exit",
                    a_when, id);
            });
        }

        void RunBrowserCameraPrep(browsecamera::OpenGate::Token a_token,
                                   const char* a_source,
                                   bool a_waitForInventory, int a_attempt,
                                   int a_instrumentContext) {
            const auto plan =
                g_browseOpenGate.Sample(a_token, false, false);
            if (!plan.current) {
                spdlog::info(
                    "[browser] camera prep discarded ({}; stale token={})",
                    a_source, a_token);
                return;
            }

            // The close task posts UI hide messages. Wait until those messages
            // have actually taken effect, then give the camera one additional
            // game frame to leave its transient inventory/Tween state. This is
            // the state in which IsInFirstPerson previously lied.
            if (a_waitForInventory) {
                auto* ui = RE::UI::GetSingleton();
                const bool inventoryOpen =
                    ui && (ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME) ||
                           ui->IsMenuOpen("TweenMenu"));
                if (inventoryOpen && a_attempt < 12) {
                    SKSE::GetTaskInterface()->AddTask(
                        [a_token, a_source, a_attempt, a_instrumentContext] {
                            RunBrowserCameraPrep(a_token, a_source, true,
                                                 a_attempt + 1,
                                                 a_instrumentContext);
                        });
                    return;
                }
                if (inventoryOpen) {
                    spdlog::warn(
                        "[browser] inventory still open after {} camera-prep "
                        "passes; sampling before deferred Songbook open",
                        a_attempt + 1);
                }
                SKSE::GetTaskInterface()->AddTask(
                    [a_token, a_source, a_attempt, a_instrumentContext] {
                        RunBrowserCameraPrep(a_token, a_source, false,
                                             a_attempt,
                                             a_instrumentContext);
                    });
                return;
            }

            auto* cam = RE::PlayerCamera::GetSingleton();
            const int id = cam && cam->currentState
                               ? static_cast<int>(cam->currentState->id)
                               : -1;
            const auto cameraPlan = g_browseOpenGate.Sample(
                a_token, cam != nullptr, cam && cam->IsInFirstPerson());
            if (!cameraPlan.current) { return; }
            if (cameraPlan.forceThirdPerson) {
                g_wasFirstPerson.store(true);
                cam->ForceThirdPerson();
                spdlog::info(
                    "[sgt] browse camera: forced third person before "
                    "Songbook open ({}; cam was {}); first person restored "
                    "at exit",
                    a_source, id);
            } else if (cam) {
                spdlog::info(
                    "[sgt] browse camera: already third person before "
                    "Songbook open ({}; cam={})",
                    a_source, id);
            } else {
                spdlog::warn(
                    "[sgt] browse camera: PlayerCamera unavailable before "
                    "Songbook open ({})",
                    a_source);
            }

            // Complete only after ForceThirdPerson returned. Cancel/newer
            // request can still overtake this task, so recheck immediately
            // before exposing the request to BrowserWindow's render thread.
            if (!g_browseOpenGate.Complete(a_token) ||
                !g_browseOpenGate.PublishAllowed(a_token)) {
                spdlog::info(
                    "[browser] camera-ready open discarded ({}; stale "
                    "token={})",
                    a_source, a_token);
                return;
            }
            // Weapons or magic out at perform start make the graph reject
            // the instrument idle (field 2026-07-25: "the instrument
            // animation doesn't play normally"). Armed BEFORE the
            // publication, not after: publishing is what lets the kIdle
            // watcher raise the world freeze, and the sheathe needs the
            // world to keep RUNNING for a moment or the animation cannot
            // finish (field 2026-07-26 - see kSheatheTick).
            BeginBrowseSheathe();
            UiBus::GetSingleton().RequestBrowserOpen(a_instrumentContext);
            spdlog::info(
                "[browser] camera ready before open request published "
                "({}; instrument context={})",
                a_source, a_instrumentContext);
        }

        void PostOpenBrowserAfterCameraPrep(const char* a_source,
                                             bool a_waitForInventory,
                                             int a_instrumentContext) {
            const auto token = g_browseOpenGate.Request();
            SKSE::GetTaskInterface()->AddTask(
                [token, a_source, a_waitForInventory, a_instrumentContext] {
                    RunBrowserCameraPrep(token, a_source, a_waitForInventory,
                                         0, a_instrumentContext);
                });
        }

        void CancelPendingBrowserOpen() {
            g_browseOpenGate.Cancel();
            UiBus::GetSingleton().browserOpenRequest.store(
                false, std::memory_order_release);
        }

        // Crowd-loss outcome, queued while the SGT effect and audience quest
        // are still alive.
        // Reuses SGT's own negative message and Negative1/Negative2 effect
        // carrier through DispatchEnding(0 stars), and immediately asks one
        // of SGT's gathered audience aliases to say its existing conditioned
        // negative topic. Teardown is deliberately separate and delayed by
        // audience::FailureFeedbackSequence so the voiced reaction can land.
        void PostCrowdFailureOutcome(int a_context,
                                     stars::Instrument a_inst,
                                     std::uint64_t a_token,
                                     bool a_stopAudience) {
            const auto spell = PerformSpellForContext(a_context);
            SKSE::GetTaskInterface()->AddTask(
                [spell, a_inst, a_token, a_stopAudience] {
                if (g_failureEpoch.load() != a_token) {
                    spdlog::info(
                        "[failure] immediate feedback dropped (timeline "
                        "changed)");
                    return;
                }
                // EndSession normally clears the pair before removing SGT's
                // effect. A failure instead pins Terrible until the delayed
                // teardown because the topic INFO conditions are evaluated
                // when Say dispatches, not when BardHero detects the loss.
                MoodGlobals::Write(crowd::Level::kTerrible);
                SgtVm::ReleaseAudienceCelebration();
                bool dispatched = false;
                if (spell != 0) {
                    const int expertise =
                        SgtProgression::LiveExpertise(a_inst);
                    dispatched =
                        SgtVm::DispatchEnding(spell, 0, expertise, false);
                }
                if (!dispatched) {
                    RE::DebugNotification(
                        "The crowd drives you from the stage.");
                }
                const bool barked = SgtVm::DispatchAudienceFailureBark();
                if (a_stopAudience) {
                    // The last field run reached stage 20 inside the old
                    // 1.75-second hold. Actor.Say is queued and Terrible
                    // remains pinned, but the applause-owning audience quest
                    // must stop at the failure edge.
                    SgtVm::StopAudienceWithoutApplause();
                }
                spdlog::info(
                    "[failure] immediate negative feedback: ending={} bark={} "
                    "spell={:08X}",
                    dispatched, barked, spell);
                });
        }

        void PostCrowdFailureTeardown(int a_context,
                                      std::uint64_t a_token) {
            const auto spell = PerformSpellForContext(a_context);
            SKSE::GetTaskInterface()->AddTask([spell, a_token] {
                if (g_failureEpoch.load() != a_token) {
                    spdlog::info(
                        "[failure] delayed teardown dropped (timeline "
                        "changed)");
                    return;
                }
                // The audience stopped at the failure edge. This delayed
                // task only keeps the negative INFO conditions and perform
                // effect alive long enough for the queued bark to begin.
                auto* pc = RE::PlayerCharacter::GetSingleton();
                auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(spell);
                if (pc && sp && pc->HasSpell(sp)) {
                    pc->RemoveSpell(sp);
                    SgtVm::EnablePlayerControlsFallback();
                }
                MoodGlobals::Clear();
                spdlog::info(
                    "[failure] delayed teardown complete spell={:08X}",
                    spell);
            });
        }

        // Inventory close can temporarily report camera state 9 even when
        // the player returns to first person one frame later. While the
        // songbook is genuinely open, sample on the game thread until that
        // transition settles and force only when first person is observed.
        // No "already third person" log here: this runs on a short cadence.
        void PostKeepThirdPersonForBrowse() {
            SKSE::GetTaskInterface()->AddTask([] {
                auto* cam = RE::PlayerCamera::GetSingleton();
                if (!cam || !cam->IsInFirstPerson()) { return; }
                const int id = cam->currentState
                    ? static_cast<int>(cam->currentState->id)
                    : -1;
                g_wasFirstPerson.store(true);
                cam->ForceThirdPerson();
                spdlog::info(
                    "[sgt] browse camera: forced third person while song "
                    "browser open (cam was {})", id);
            });
        }

        void PostRestoreFirstPerson(const char* a_why) {
            if (!g_wasFirstPerson.exchange(false)) { return; }
            SKSE::GetTaskInterface()->AddTask([a_why] {
                auto* cam = RE::PlayerCamera::GetSingleton();
                if (!cam) { return; }
                cam->ForceFirstPerson();
                spdlog::info("[sgt] browse camera: first person restored ({})",
                             a_why);
            });
        }

        // Phase 3 (design 2026-07-20): sheathe at the trigger edge, while
        // the browser is opening.
        //
        // SGT's OnEffectStart pays `SheatheWeapon(); Utility.Wait(2)` when
        // IsWeaponDrawn() is true (psc 133-136). Native start moved that
        // whole flow from browse time to song PICK, so the 2s landed where
        // the player could see it: notes scrolled while the character was
        // still putting the sword away and pulling out the lute. Sheathing
        // here means IsWeaponDrawn() is already false when SGT looks, so it
        // skips the branch entirely - and it costs the player nothing,
        // because they are standing in a browser at this point anyway.
        //
        // Posted as a task: the trigger arrives from inside the engine's
        // equip processing (the AddTarget hook), and this runs after the
        // inventory close queued just above it - game-thread FIFO.
        void PostNativeBrowseSheathe() {
            if (!Settings::GetSingleton().sgtBrowseSheathe) { return; }
            SKSE::GetTaskInterface()->AddTask([] {
                auto* pc = RE::PlayerCharacter::GetSingleton();
                if (!pc) { return; }
                // Publish BEFORE the sheathe, not after: a pick landing in
                // the next few hundred ms sees the same drawn weapon SGT
                // will, and gets the wide lead. Later poll passes walk it
                // back down once the sheathe animation has actually landed.
                PublishWeaponDrawn(pc);
                auto* st = pc->AsActorState();
                if (!st || !st->IsWeaponDrawn()) {
                    spdlog::info(
                        "[sgt] browse sheathe: weapon already away - SGT "
                        "will skip its 2s sheathe branch");
                    return;
                }
                pc->DrawWeaponMagicHands(false);
                spdlog::info(
                    "[sgt] browse sheathe: sheathing during browse so the "
                    "pick skips SGT's SheatheWeapon + Wait(2)");
            });
        }

        // SGT's own "you must be standing" guard (psc 95-99) is the FIRST
        // thing in OnEffectStart:
        //
        //   If PlayerRef.GetSitState() > 0 || PlayerRef.IsSwimming() == 1
        //       Game.GetPlayer().RemoveSpell(InstrumentSpell)
        //       _Message_Standing.Show()
        //       return
        //
        // Native start (Phase 2) strips the ability before Papyrus ever
        // schedules OnEffectStart, so that guard can no longer fire - and
        // without a replacement the browser would open while the player is
        // sat at a bench, on a mount or mid-swim. This is not a maybe: the
        // guard lives in the exact code path Phase 2 suppresses.
        //
        // Same condition SGT uses. SIT_SLEEP_STATE::kNormal is 0 and every
        // other value is greater, so `!= kNormal` IS Papyrus's
        // `GetSitState() > 0` - and it picks up kRidingMount too, which
        // shares kIsSitting's value.
        bool PerformBlockedByPose(RE::PlayerCharacter* a_pc) {
            auto* st = a_pc ? a_pc->AsActorState() : nullptr;
            if (!st) { return false; }
            return st->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal ||
                   st->IsSwimming();
        }

        // ---- world-audio placement ---------------------------------------
        // Written by a game-thread task, read by the SESSION thread, which
        // is the only owner of the AudioEngine. Going through atomics rather
        // than letting the task touch g_s directly means the task can never
        // outlive the session it was posted for - EndSession deletes g_s and
        // a queued task holding that pointer would be a use-after-free.
        //
        // Individual relaxed atomics, no lock: a read that mixes components
        // from two different frames is a sub-millisecond placement error and
        // is inaudible, whereas a mutex on the 5ms tick is not free.
        std::atomic<float> g_camPos[3]{}, g_camFwd[3]{}, g_camUp[3]{};
        std::atomic<float> g_srcPos[3]{};
        std::atomic<bool>  g_placementValid{ false };
        // One line per session naming the actual listener frame. Silence
        // under bWorldAudio was diagnosed twice by inference because the
        // vectors were never logged: a zero or non-normalized direction
        // degenerates miniaudio's listener basis, and a source that never
        // leaves the origin attenuates against the wrong point. Log the
        // numbers once so the next silence report is settled by reading,
        // not by an INI bisect.
        std::atomic<bool>  g_placementLogged{ false };

        void PostUpdateWorldAudioPlacement() {
            SKSE::GetTaskInterface()->AddTask([] {
                auto* cam = RE::PlayerCamera::GetSingleton();
                auto* pc  = RE::PlayerCharacter::GetSingleton();
                if (!cam || !pc || !cam->cameraRoot) { return; }
                const auto& w = cam->cameraRoot->world;
                g_camPos[0].store(w.translate.x, std::memory_order_relaxed);
                g_camPos[1].store(w.translate.y, std::memory_order_relaxed);
                g_camPos[2].store(w.translate.z, std::memory_order_relaxed);
                // Skyrim is Z-up / Y-forward and NiMatrix3 stores the local
                // axes as COLUMNS, so forward is the Y column and up is the
                // Z column. Taking rows here would pan the song sideways.
                for (int i = 0; i < 3; ++i) {
                    g_camFwd[i].store(w.rotate.entry[i][1],
                                      std::memory_order_relaxed);
                    g_camUp[i].store(w.rotate.entry[i][2],
                                     std::memory_order_relaxed);
                }
                const auto p = pc->GetPosition();
                g_srcPos[0].store(p.x, std::memory_order_relaxed);
                g_srcPos[1].store(p.y, std::memory_order_relaxed);
                g_srcPos[2].store(p.z, std::memory_order_relaxed);
                g_placementValid.store(true, std::memory_order_release);
                if (!g_placementLogged.exchange(true)) {
                    const auto& t   = w.translate;
                    const float fwd[3] = { w.rotate.entry[0][1],
                                           w.rotate.entry[1][1],
                                           w.rotate.entry[2][1] };
                    const float dx  = p.x - t.x, dy = p.y - t.y,
                                dz  = p.z - t.z;
                    // |fwd| should read 1.00; anything else means the basis
                    // is degenerate and the spatializer output is suspect
                    spdlog::info(
                        "[audio] placement: listener=({:.0f},{:.0f},{:.0f}) "
                        "fwd=({:.2f},{:.2f},{:.2f}) |fwd|={:.2f} "
                        "src=({:.0f},{:.0f},{:.0f}) dist={:.0f}u "
                        "(panning only - no distance attenuation)",
                        t.x, t.y, t.z, fwd[0], fwd[1], fwd[2],
                        std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] +
                                  fwd[2] * fwd[2]),
                        p.x, p.y, p.z,
                        std::sqrt(dx * dx + dy * dy + dz * dz));
                }
            });
        }

        // The one place a perform trigger turns into a start request,
        // whichever source saw it first.
        void FirePerformTrigger(int a_inst, const char* a_source) {
            const int progressionContext =
                songeligibility::ProgressionContext(a_inst);
            if (progressionContext < 0) {
                spdlog::warn(
                    "[session] invalid perform trigger context {} ignored",
                    a_inst);
                return;
            }
            const auto progression =
                static_cast<stars::Instrument>(progressionContext);
            // Game thread (both the AddTarget hook and the poll task), so
            // the actor-state read is safe here.
            if (auto* pc = RE::PlayerCharacter::GetSingleton();
                PerformBlockedByPose(pc)) {
                spdlog::info(
                    "[sgt] perform trigger ({}) refused - the player is "
                    "sitting or swimming (SGT's own standing guard cannot "
                    "fire under native start)",
                    a_source);
                // The ability is already gone: native start stripped it at
                // the hook, exactly as SGT's guard would have. With native
                // start OFF, SGT's OnEffectStart runs and shows its own
                // _Message_Standing - do not stack a second message on it.
                if (Settings::GetSingleton().sgtNativeStart) {
                    RE::DebugNotification(
                        "You must be standing to play an instrument.");
                }
                return;
            }
            spdlog::info(
                "[session] perform ability detected ({}) - inst={} start "
                "requested",
                a_source, a_inst);
            g_lastTriggerInst.store(a_inst);
            StarLedger::GetSingleton().SetActiveInstrument(
                progression);
            SgtProgression::NoteCast(progression, QpcSec());
            GoldScale::NotePerformCast();
            g_reqStart.store(true);
            PostCloseInventory();
            PostNativeBrowseSheathe();
        }

        void PollPerformAbility() {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!pc) { return; }
            // cheap ride-along: keeps the lead-in input fresh through the
            // whole browse window, including a weapon drawn AFTER the
            // trigger sheathed one away
            PublishWeaponDrawn(pc);
            int mask = 0;
            for (int i = 0;
                 i < songeligibility::kInstrumentContextCount; ++i) {
                const auto id = g_performSpell[i].load();
                if (id == 0) { continue; }
                auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(id);
                if (sp && pc->HasSpell(sp)) { mask |= 1 << i; }
            }
            const int inst = g_arming.NotePoll(mask);
            if (inst >= 0) { FirePerformTrigger(inst, "poll"); }
        }

        // Post-session control-state probe (game thread; field round 3:
        // "couldn't walk or input anything" after closing the results box,
        // completed sessions only - unexplained). One line per probe names
        // the locking layer: ControlMap bits (ToggleControls side), the
        // script-disable mask, menu/pause state (a stuck unpaused
        // MessageBox), the sit/sleep state (stuck play pose), the camera
        // state. Log-first - no blind fix.
        // Snapshot the input-context stack as text, or "" when it is not
        // safe to read.
        //
        // CRASH 2026-07-20 20:14:29, startup, EXCEPTION_ACCESS_VIOLATION at
        // BardHero.dll+0xB843F inside an SKSE task, RDX=0x43 dereferenced as
        // a pointer. ControlMap::GetSingleton() returns a valid pointer from
        // very early in startup, but its BSTArrays are NOT constructed yet:
        // contextPriorityStack._data held a small garbage value and iterating
        // it faulted. The always-on ctlwatch had read this same object since
        // day one without trouble because enabledControls/unk11C are plain
        // integers - garbage reads, never faults. An uninitialised POINTER is
        // a different class of hazard, and adding one to an always-on path is
        // what broke it.
        //
        // Two independent gates, because one of them can be fooled: the
        // player having 3D means we are actually in the world (not the main
        // menu, not mid-load), and a size bounded by the number of context
        // ids that can exist rejects a garbage length.
        std::string ContextStackText(RE::ControlMap* a_cm) {
            // DISABLED 2026-07-20 20:24 after a SECOND CTD. Do not re-enable
            // without offline verification of the offset first - see below.
            //
            // Crash 1 (20:14, startup): RDX=0x43 dereferenced. Diagnosed as
            // "ControlMap exists before its arrays are constructed" and gated
            // on PlayerCharacter::Get3D().
            //
            // Crash 2 (20:24, right after a save loaded): SAME base value,
            // RCX=0x43, at `mov ecx,[rcx+rax*4]` - index 0 of a 4-byte array
            // based at 0x43. The gate provably WORKED at the main menu, the
            // log shows `ctx [ctx-gated]` at 20:23:58 while paused. It then
            // let the read through once the player had 3D, and data() STILL
            // returned 0x43. So the array is garbage even with the world up,
            // and the startup-timing theory is dead.
            //
            // What that leaves: the field is not where CommonLib says it is
            // on this runtime, or it is not a plain BSTArray here. The header
            // arithmetic is self-consistent (0x60 + 17*8 = 0xE8 linkedMappings,
            // +0x18 = 0x100 this field, +0x18 = 0x118 enabledControls, which
            // demonstrably reads correctly on AE 1.6.1170) - but self-consistent
            // is not verified, and 0x43 showing up as the base BOTH times is a
            // stable wrong pointer, not random uninitialised memory.
            //
            // Next step is OFFLINE: resolve BardHero.dll+0xB8DA7 against the
            // PDB to confirm this function is even the faulting one, then
            // verify the real offset against the AE binary in Ghidra
            // ([[skyrim-re-workspace]]). No more field runs on a guess - this
            // has cost two crashes already.
            (void)a_cm;
            return "ctx-disabled";
        }

        void LogControlProbe(int a_probeNo) {
            auto* cm  = RE::ControlMap::GetSingleton();
            auto* ui  = RE::UI::GetSingleton();
            auto* pc  = RE::PlayerCharacter::GetSingleton();
            auto* cam = RE::PlayerCamera::GetSingleton();
            // Second probe line: the layers the first one CANNOT see. Field
            // 2026-07-20: ControlMap restored, idle force-defaulted,
            // sitSleep=0, cam settled - and WASD still dead with jump alive.
            // Every remaining suspect is here: kMovementBlocked (bit 27) is
            // the SetDontMove flag, kParalyzed (bit 31) speaks for itself,
            // bAnimationDriven roots input-driven motion, Speed is the
            // locomotion graph var Menu Studio has seen stick before.
            if (pc) {
                const auto bf =
                    pc->GetActorRuntimeData().boolFlags.underlying();
                const auto bb =
                    pc->GetActorRuntimeData().boolBits.underlying();
                bool  animDriven = false;
                float speed      = -1.0f;
                pc->GetGraphVariableBool("bAnimationDriven", animDriven);
                pc->GetGraphVariableFloat("Speed", speed);
                spdlog::info(
                    "[probe] #{}b: dontMove={} paralyzed={} boolFlags=0x{:X} "
                    "boolBits=0x{:X} animDriven={} speed={:.2f}",
                    a_probeNo,
                    (bf & (1u << 27)) != 0,  // BOOL_FLAGS::kMovementBlocked
                    (bb & (1u << 31)) != 0,  // BOOL_BITS::kParalyzed
                    bf, bb, animDriven, speed);
            }
            // Third line: PlayerControls per-handler enables - the layer
            // UNDER the ControlMap. Each PlayerInputHandler carries its own
            // inputEventHandlingEnabled; a movement handler switched off
            // kills WASD while the jump handler keeps jumping, which is the
            // exact 2026-07-20 lock signature every layer above has failed
            // to explain.
            if (auto* pctl = RE::PlayerControls::GetSingleton()) {
                const auto hs = [](RE::PlayerInputHandler* a_h) {
                    return a_h ? (a_h->inputEventHandlingEnabled ? 1 : 0)
                               : -1;
                };
                spdlog::info(
                    "[probe] #{}c: handlers move={} look={} jump={} sneak={} "
                    "attackBlock={}",
                    a_probeNo, hs(pctl->movementHandler),
                    hs(pctl->lookHandler), hs(pctl->jumpHandler),
                    hs(pctl->sneakHandler), hs(pctl->attackBlockHandler));
            }
            // Sixth line: the movement INPUT VECTOR, the layer directly under
            // the handler enables. #Nc only says the movement handler is
            // switched ON; it cannot say the handler ever received a
            // direction. moveInputVec is what WASD accumulates into, so with
            // a key physically held it is non-zero if and only if the press
            // became a movement user-event. That splits the last two
            // survivors of 2026-07-21: a POPULATED vector with Speed stuck at
            // 0 puts the fault below the handler (behaviour graph), an EMPTY
            // one puts it in input routing (the context stack we still cannot
            // read). blockPlayerInput and povScriptMode are plain bools that
            // nothing has ever sampled, and either alone would produce this
            // exact signature - every mask healthy, no motion. All plain
            // members of a struct we already read handlers from, so unlike
            // ContextStackText there is no pointer to walk and nothing to
            // fault on.
            if (auto* pctl = RE::PlayerControls::GetSingleton()) {
                const auto& d = pctl->data;
                spdlog::info(
                    "[probe] #{}g: blockInput={} moveInputVec=({:.2f},{:.2f}) "
                    "prevMoveVec=({:.2f},{:.2f}) lookInputVec=({:.2f},{:.2f}) "
                    "autoMove={} running={} povScript={} povBeast={} remap={}",
                    a_probeNo, pctl->blockPlayerInput, d.moveInputVec.x,
                    d.moveInputVec.y, d.prevMoveVec.x, d.prevMoveVec.y,
                    d.lookInputVec.x, d.lookInputVec.y, d.autoMove, d.running,
                    d.povScriptMode, d.povBeastMode, d.remapMode);
            }
            // Fourth line: OUR OWN input hook. Standalone proved the lock
            // survives with SGT's script never running, and every game-side
            // layer measures healthy, so the remaining suspect is the one
            // component we have never instrumented. `filtered` still rising
            // after a session ended means the hook never returned to
            // pass-through; both counters frozen means it is not being
            // called at all (chain above us).
            {
                auto& feed = EngineFeed::GetSingleton();
                auto& bus  = UiBus::GetSingleton();
                const double at =
                    (a_probeNo >= 1 && a_probeNo <= kProbeCount)
                        ? kProbeSchedule[a_probeNo - 1]
                        : -1.0;
                spdlog::info(
                    "[probe] #{}d: t=+{:.2f}s hook passthrough={} filtered={} "
                    "lastCapture={} | active={} engaged={} browserOpen={} "
                    "resultsReady={} worldPaused={}",
                    a_probeNo, at, InputHook::PassthroughFrames(),
                    InputHook::FilteredFrames(), InputHook::LastCapture(),
                    feed.active.load(std::memory_order_relaxed),
                    feed.engaged.load(std::memory_order_relaxed),
                    bus.browserOpen.load(std::memory_order_relaxed),
                    bus.resultsReady.load(std::memory_order_relaxed),
                    bus.worldPaused.load(std::memory_order_relaxed));
            }
            // Fifth line: the input ROUTING layer, never sampled once in ten
            // rounds. ControlMap::contextPriorityStack (offset 0x100, sitting
            // immediately before the enabledControls/unk11C pair every probe
            // has stared at) decides WHICH context's key mappings are live. A
            // context pushed and never popped leaves every flag, every
            // handler and every actor bit reading healthy while WASD, the
            // menu key and the console key all map to nothing - which is this
            // symptom exactly, and would explain why the hook measured
            // innocent. ids: 0 gameplay / 1 menuMode / 2 console / 3 itemMenu
            // / 4 inventory / 9 cursor / 12 journal.
            //
            // Also kConsole (UEFlag 0x10) - the one flag that speaks directly
            // to "cannot open the console", and which no probe has ever
            // printed - and our own ForceCursor refcount, because forcing the
            // cursor pushes kCursor and that latch is a plain int shared by
            // three windows.
            {
                const std::string ctx = ContextStackText(cm);
                // RAW, and deliberately NOT a dereference. Following data()
                // as a pointer gave 0x43 twice and cost two CTDs, so instead
                // read the 24 bytes AT ControlMap+0x100 and just print them.
                // sizeof(ControlMap) is 0x128 (static_assert in the header)
                // and we already read +0x118 every tick without faulting, so
                // this memory is provably in-object and cannot crash.
                //
                // If CommonLib is right and this really is a BSTArray, the
                // triple reads {size, data, capacity} - a small count, then a
                // plausible 0x1xx'xxxx'xxxx heap pointer, then a capacity >=
                // the size. Anything else says the field is not what the
                // header claims on AE 1.6.1170, and names what to fix.
                std::uint64_t raw[3] = { 0, 0, 0 };
                if (cm) {
                    std::memcpy(
                        raw,
                        reinterpret_cast<const std::uint8_t*>(cm) + 0x100,
                        sizeof(raw));
                }
                spdlog::info(
                    "[probe] #{}e: ctxStack=[{}] console={} cursorRefs={} "
                    "cursorMenu={} | raw@0x100=[{:#018x} {:#018x} {:#018x}]",
                    a_probeNo, ctx,
                    cm ? cm->IsConsoleControlsEnabled() : false,
                    RenderUi::CursorRefs(),
                    ui ? ui->IsMenuOpen("Cursor Menu") : false,
                    raw[0], raw[1], raw[2]);
                // #Nh: RAW ControlMap MEMBER BYTES, no dereference of any
                // kind - the two earlier CTDs came from following a pointer
                // at a header offset that is wrong on AE 1.6.1170. This is a
                // plain in-object memcpy, so it cannot fault, and it does not
                // need the offset to be right: probe #0 fires in free roam
                // (healthy) and #1..#8 fire post-session (locked), both from
                // this same instrument, so DIFFING the two names the field
                // that changed without anyone having to guess a layout.
                if (cm) {
                    std::uint64_t blk[12] = {};
                    std::memcpy(
                        blk,
                        reinterpret_cast<const std::uint8_t*>(cm) + 0xC0,
                        sizeof(blk));
                    std::string dump;
                    for (int i = 0; i < 12; ++i) {
                        dump += fmt::format("{:#018x} ", blk[i]);
                    }
                    spdlog::info("[probe] #{}h: cm@0xC0: {}", a_probeNo,
                                 dump);
                }
            }
            // Sixth line: EVERY open menu, by name.
            //
            // The blind spot this closes: SkyrimSoulsRE (v3.1.2) is installed,
            // and Souls makes menus NOT pause the game. So `paused=false
            // numPauses=0` - which every probe in this hunt has read and every
            // reading has treated as "no menu is open" - proves nothing of the
            // kind. A menu left open with input focus eats movement, the menu
            // key AND the console key at once, while ControlMap flags, the
            // per-handler enables and the actor bits all stay healthy. That is
            // the symptom, exactly, and it has never been measured.
            //
            // IsMenuOpen is a name lookup and cannot fault - the probe already
            // calls it five times a pass. No pointers are followed here.
            {
                static constexpr const char* kMenus[] = {
                    "Console",       "Console Native UI Menu",
                    "Cursor Menu",   "Fader Menu",
                    "LoadWaitSpinner", "Loading Menu",
                    "HUD Menu",      "MessageBoxMenu",
                    "Dialogue Menu", "InventoryMenu",
                    "ContainerMenu", "MagicMenu",
                    "MapMenu",       "Journal Menu",
                    "StatsMenu",     "BarterMenu",
                    "GiftMenu",      "FavoritesMenu",
                    "TweenMenu",     "Sleep/Wait Menu",
                    "Training Menu", "Lockpicking Menu",
                    "Crafting Menu", "Book Menu",
                    "RaceSex Menu",  "LevelUp Menu",
                    "Quantity Menu", "Tutorial Menu",
                    "Overlay Menu",  "Overlay Interaction Menu",
                    "Main Menu",     "TitleSequence Menu",
                    "Credits Menu",  "Mist Menu",
                    "StreamingInstallMenu",
                };
                std::string open;
                if (ui) {
                    for (const char* m : kMenus) {
                        if (ui->IsMenuOpen(m)) {
                            if (!open.empty()) { open += ", "; }
                            open += m;
                        }
                    }
                }
                spdlog::info(
                    "[probe] #{}f: openMenus=[{}] | pauseMenuOpen={}",
                    a_probeNo, open.empty() ? std::string("none") : open,
                    UiBus::GetSingleton().pauseMenuOpen.load(
                        std::memory_order_relaxed));
            }
            // ...and the binding table again well after the session ended, in
            // case something unbinds movement later than our own restore.
            MovementGuard::LogBinds("post-session");
            spdlog::info(
                // controls=/unk11C=/move=/activate=/fight=/menu=/look= were
                // dropped 2026-07-22. Every one of them resolved through
                // ControlMap::enabledControls, which on AE is the input
                // context stack's SIZE - so they reported a stack depth and
                // five booleans derived from it, and reading them as control
                // state is what sent three sessions after the wrong layer.
                // See MovementGuard.h.
                "[probe] #{}: paused={} "
                "numPauses={} msgbox={} dialogue={} | sitSleep={} cam={}",
                a_probeNo,
                ui ? ui->GameIsPaused() : false,
                ui ? ui->numPausesGame : 0,
                ui ? ui->IsMenuOpen("MessageBoxMenu") : false,
                ui ? ui->IsMenuOpen("Dialogue Menu") : false,
                pc ? static_cast<int>(
                         pc->AsActorState()->GetSitSleepState())
                   : -1,
                cam && cam->currentState
                    ? static_cast<int>(cam->currentState->id)
                    : -1);
        }

        // Browse-standstill counterpart (design 2026-07-19): the trigger
        // STRIPPED SGT's performance while the browser was open - the picked
        // session starts the real one. Always-add: any session start re-adds
        // the ability when the whole-song integration is on and the session's
        // instrument has a configured spell (a fast pick can land before the
        // strip; AddSpell is then a no-op and the trigger's performance
        // simply continues). The task also:
        //  - sets the poll's prev-mask bit (same game thread as the poll) so
        //    the absent->present edge of OUR AddSpell cannot queue a phantom
        //    start when the poll resumes at session end
        //  - re-stamps NoteCast/NotePerformCast: the REAL performance starts
        //    now, so the clamp windows and the legacy gold window follow it
        void PostBeginSgtPerformance(
            int a_context,
            stars::Instrument a_inst,
            bard::band::StemAvailability a_bandStems) {
            if (!Settings::GetSingleton().wholeSongPerform) { return; }
            const auto spell = PerformSpellForContext(a_context);
            if (spell == 0) { return; }
            // Re-assert the camera HERE, not only at the trigger. This is
            // the point that actually matters: OnEffectStart is what starts
            // IdleLuteStart, and the AnimObject attaches to the third-person
            // skeleton. The trigger-time check samples the camera while the
            // inventory menu is still up; by now the menus are closed and
            // IsInFirstPerson() means what it says. Queued BEFORE the
            // AddSpell task so game-thread FIFO puts the camera change ahead
            // of the effect that reads it. A no-op in third person, and it
            // logs which branch it took either way.
            PostForceThirdPerson("song pick");
            const bool standalone =
                Settings::GetSingleton().standalonePerform;
            const bool guitarProp =
                songeligibility::UsesGuitarPerformanceProp(a_context);
            SKSE::GetTaskInterface()->AddTask(
                [spell, a_context, a_inst, standalone, guitarProp,
                 a_bandStems] {
                    auto* pc = RE::PlayerCharacter::GetSingleton();
                    auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(spell);
                    if (!pc || !sp) { return; }
                    g_arming.NoteSelfAdd(a_context);
                    SgtProgression::NoteCast(a_inst, QpcSec());
                    GoldScale::NotePerformCast();
                    // STANDALONE: the ability was stripped at the trigger and
                    // we do NOT put it back, so _Talent_PlayInstrument never
                    // runs. Nothing seizes controls, nothing forces the
                    // camera, and there is no OnEffectFinish latent left to
                    // die - which is the entire point. Stage 1 loses the
                    // idle, the prop, the crowd and SGT's payout with it;
                    // that is expected and the run is a lock experiment, not
                    // a playable performance.
                    if (standalone) {
                        spdlog::info(
                            "[sgt] STANDALONE: perform ability NOT re-added "
                            "- _Talent_PlayInstrument never runs this session "
                            "(no idle, no prop, no crowd, no SGT payout by "
                            "design). If the exit lock survives THIS, it was "
                            "never SGT.");
                        return;
                    }
                    // BEFORE the AddSpell: OnEffectStart reads this global
                    // the moment the effect starts, and at 0 it takes
                    // movement and skips the instrument prop entirely.
                    SgtVm::SetGuitarAnimObjectOverride(guitarProp);
                    SgtVm::ForceEnableMovementGlobal();
                    if (guitarProp) {
                        bard::BandStage::Begin(a_context, a_bandStems);
                    }
                    if (!pc->HasSpell(sp)) {
                        {
                            // our add - native start must not strip it
                            SelfAddGuard g;
                            pc->AddSpell(sp);
                        }
                        spdlog::info(
                            "[sgt] performance started (AddSpell at song "
                            "pick)");
                    } else {
                        spdlog::info(
                            "[sgt] performance already live at song pick "
                            "(fast pick)");
                    }
                    // If a standstill blank already landed on the instance
                    // this session continues (unfrozen browse, or a
                    // pre-disarm race), put the original idle properties
                    // back. The play idle, keeper re-up and end-cleanup's
                    // IdleStop all need them.
                    SgtVm::RestoreIdleProperties(spell);
                });
        }

        // Session thread. Fill in the half of the results snapshot that is
        // about PROGRESSION rather than about the run: the purse and why it
        // was that size, and the rank standing the run is about to buy.
        // Spec: docs/specs/2026-07-22-performance-ui.md section 3.3.
        //
        // ⚠ Both halves are PREDICTIONS made a few seconds early, and they
        // are honest ones for a specific reason: the results window is the
        // thing HOLDING the payout. `g_pendingPayout` is dispatched when the
        // box closes, so at draw time the gold has not been granted and the
        // experience has not been fed. Rather than leave the player's own
        // payoff moment blank, we compute exactly what is queued to happen -
        // same inputs, same functions, same settings - so what the box says
        // and what the log then reports cannot disagree.
        //
        // Everything is gated. `goldKnown` is false unless the deferred
        // whole-song purse is actually the payer, and `rankKnown` is false
        // unless we own the ending too (when SGT's MessageAndEXP runs the XP
        // is its own RandomInt and nothing here can predict it).
        //
        // a_feedBonus is an OUT parameter: the deferred dispatch below needs
        // the identical number, and computing it twice is how the box and
        // the grant would drift apart.
        void FillProgressionFeedback(UiBus::Results& r, double a_acc,
                                     int a_stars, bool a_deferPayout,
                                     stars::Instrument a_inst,
                                     double a_songLen, int a_difficulty,
                                     int& a_feedBonus) {
            const auto& se = Settings::GetSingleton();
            // Frozen with the session, just like the score and chart. The
            // render thread must not infer a possibly changed live perform
            // power when naming the proficiency this run advanced.
            r.instrument = static_cast<int>(a_inst);

            // The experience feed, computed here and handed back so the
            // dispatch reuses it verbatim.
            const stars::FeedParams fp{ se.xpFeedBase, se.xpFeedMinAccuracy };
            a_feedBonus =
                se.xpFeed ? stars::FeedBonus(a_acc, a_songLen, fp) : 0;
            if (!a_deferPayout) { return; }

            // ---- the purse ---------------------------------------------
            // Same params, same arguments, same function as
            // GoldScale::TickDeferred. `observed` is 0 on this path (we do
            // not call MessageAndEXP, so SGT pays nothing), which is why the
            // deserved figure IS what gets granted - see the TopUp comment
            // in EndingLogic.h.
            if (se.performancePayout) {
                payout::Params pp;
                pp.buskBase        = se.buskBase;
                pp.buskOutside     = se.buskOutside;
                pp.renownAtRank1   = se.renownAtRank1;
                pp.moodPayTerrible = se.moodPayTerrible;
                pp.cap             = se.payoutCap;
                pp.minStars        = se.payoutMinStars;
                pp.lengthRefSec    = se.payoutLengthRefSec;
                pp.lengthMin       = se.payoutLengthMin;
                pp.lengthMax       = se.payoutLengthMax;
                const int mood = g_moodTally.Dominant();
                const int rank = SgtProgression::RankFromExpertise(
                    SgtProgression::UiSampled(a_inst));
                r.gold = payout::Deserved(a_stars, mood, a_difficulty, rank,
                                          g_playerAtInn.load(), a_songLen, pp);
                results::GoldFacts gf;
                gf.gold       = r.gold;
                gf.stars      = a_stars;
                gf.minStars   = pp.minStars;
                gf.moodLevel  = mood;
                gf.lengthMult = payout::LengthMult(a_songLen, pp);
                gf.atInn      = g_playerAtInn.load();
                r.goldReason  = results::GoldReason(gf);
                r.goldKnown   = true;
            }

            // ---- the standing ------------------------------------------
            if (!se.ownEnding) { return; }
            // UiSampled, not LiveExpertise: this is the session thread and
            // LiveExpertise reads SGT's globals, which is game-thread only.
            // It is also the sample the gold path already committed to, so
            // the two cannot disagree about who the player is.
            const int before = SgtProgression::UiSampled(a_inst);
            if (before < 0) { return; }  // SGT absent / nothing sampled
            const ending::XpParams xp{ se.xpPerStar, se.xpBonus5 };
            r.expertiseBefore = before;
            r.xpGain = a_feedBonus + ending::PerformanceXp(a_stars, xp);
            // Clamped for the same reason EffectiveRank is: the gate ceiling
            // is where FinishPayout will actually leave the player, and a
            // progress bar drawn past it would creep toward a boundary it
            // is not allowed to cross. GateCeilingPeek is the READER's
            // derivation - it must not persist the merged mask from here.
            r.expertiseAfter = std::min(
                before + r.xpGain,
                StarLedger::GetSingleton().GateCeilingPeek(a_inst));
            // EffectiveRank, not RankFromExpertise: it clamps to the star
            // gate ceiling, so a rank-up the gate is about to claw back is
            // never announced. This runs AFTER StarLedger::Record, so the
            // stars this run just earned already count toward lifting it.
            r.rankBefore = SgtProgression::EffectiveRank(a_inst, before);
            r.rankAfter  = SgtProgression::EffectiveRank(
                a_inst, r.expertiseAfter);
            r.rankKnown  = true;
        }

        // ---- session thread ----------------------------------------------
        void EndSession(const char* verdictLine, bool completed = false,
                        bool crowdFailed = false) {
            const bool bandWasActive = bard::BandStage::Active();
            bard::BandStage::EndAsync(
                verdictLine ? std::string_view{ verdictLine }
                            : std::string_view{ "session end" });
            // Before the g_s gate: restoring is idempotent and restores
            // only what OnSessionStart hid, so it can never over-fire.
            WidgetMuffle::OnSessionEnd();
            if (!g_s) return;
            // Gated on the BAND alone, deliberately not on `completed`.
            // `completed` is the RECORDING switch (it suppresses stars,
            // gold and the ledger for practice); it had quietly acquired a
            // second job as a presentation gate, which is why a finished
            // practice run froze the world on the same frame the ensemble
            // was dismissed and the skeletons vanished without their
            // unsummon (field 2026-07-26). Whether a panel must wait is a
            // question about the band, not about what was banked. Harmless
            // when no panel follows: publication needs something staged.
            g_resultsPublishNotBefore =
                bandWasActive
                ? QpcSec() + bard::band::kResultsRevealDelaySeconds
                : 0.0;
            // unpublish BEFORE touching the engine: after the nulling no
            // hook frame can hold a pointer (it dereferences under mx)
            auto& feed = EngineFeed::GetSingleton();
            feed.engaged.store(false);
            feed.active.store(false);
            PostEndPerformanceVanity("session end");
            // Hand the camera back BEFORE results. The director must never
            // be steering while the results box is up, and this covers every
            // ending together - completion, abort and crowd failure all
            // funnel through EndSession. It only raises a request; the
            // restore itself lands on the next game-thread camera pass.
            PerformanceCamera::Release();
            // Same funnel, same reason: every ending goes through here, so
            // the fire goes out here rather than in three separate places.
            StopStreakCloak();
            {
                std::scoped_lock lk(g_menuTrackMx);
                g_nativeMenus.Clear();
            }
            g_nativeMenuOpen.store(false, std::memory_order_release);
            EngineFeed::Counters ic;
            {
                std::scoped_lock lk(feed.mx);
                ic          = feed.counters;
                feed.engine = nullptr;
                feed.clock  = nullptr;
                feed.song       = nullptr;
                feed.audio      = nullptr;
                feed.songLen    = 0.0;
                feed.maxSustainSec = 0.0;
                feed.guitarStem = -1;
            }
            feed.heldFrets.store(0);
            if (ic.fed > 0) {
                // the M3 exit measurement: delivery age of every event fed
                // to the engine (stamp accuracy itself is M0's ~1ms)
                spdlog::info(
                    "[session] INPUT SUMMARY: fed={} maxAge={:.1f}ms "
                    "meanAge={:.1f}ms stale={} bufferFull={} - {}",
                    ic.fed, ic.maxAgeMs, ic.sumAgeMs / ic.fed, ic.stale,
                    ic.bufferFull,
                    (ic.sumAgeMs / ic.fed) < 20.0 ? "PASS (<20ms mean)"
                                                  : "FAIL (>=20ms mean)");
            }
            if (g_s->samples > 0) {
                spdlog::info(
                    "[session] DRIFT SUMMARY: max|delta|={:.2f}ms @t={:.1f}s "
                    "mean={:+.2f}ms corrections={} samples={} - {}",
                    g_s->maxAbsDelta * 1000.0, g_s->maxDeltaAt,
                    g_s->sumDelta / g_s->samples * 1000.0,
                    g_s->ctrl.Corrections(), g_s->samples,
                    g_s->maxAbsDelta < 0.005 ? "PASS (<5ms)" : "FAIL (>=5ms)");
            }
            spdlog::info("[session] {}", verdictLine);
            if (g_s->engine) {
                const auto& st = g_s->engine->Stats();
                spdlog::info("[session] engine: score={} hit={} missed={}",
                             st.score, st.notesHit, st.notesMissed);
            }
            // hoisted so the deferred-payout branch (below `delete g_s`) can
            // reuse them; only the completed path assigns real values (both
            // deferPayout and the Results block are completed-gated).
            double acc    = 0.0;
            int    earned = 0;
            int    feedBonus = 0;
            // These four moved ABOVE the Results block on 2026-07-22: the
            // results window now reports the purse and the rank the run
            // earned, and both need to know whether this is the deferred
            // whole-song path before the snapshot is published. They read
            // only g_s, which is alive here and gone by the old site.
            const bool sgtWhole = Settings::GetSingleton().wholeSongPerform &&
                                  g_sgtLogic.Confirmed() && !g_sgtLogic.Lost();
            // computed HERE while g_s is alive - the call site below runs
            // after `delete g_s`
            const bool deferPayout = sgtWhole && completed &&
                                     g_s->engine != nullptr;
            const auto   inst    = g_s->instrument;
            const int    instrumentContext = g_s->instrumentContext;
            const double songLen = g_s->songLen;
            // Capture before teardown resets the public HUD value. Stars
            // describe the whole performance; this last live zone prevents
            // an excellent average followed by a severe fumble from earning
            // the same praise and ovation as a strong finish.
            const bool finishedGreat =
                completed &&
                g_mood.Committed() == crowd::Level::kGreat;
            if (sgtWhole &&
                ending::ShouldSuppressAudienceApplause(
                    completed, finishedGreat)) {
                // _Talent_ReceiveOvation=false affects SGT's message logic,
                // but its ordinary effect finish still enters the stage-20
                // applause scene. Stop only the audience quest now, while
                // leaving the perform effect alive for the deferred payout.
                SKSE::GetTaskInterface()->AddTask([] {
                    SgtVm::StopAudienceWithoutApplause();
                    spdlog::info(
                        "[ending] non-green completion: SGT applause "
                        "suppressed");
                });
            }
            if (completed && g_s->engine) {
                const auto&    st = g_s->engine->Stats();
                UiBus::Results r;
                r.songName   = g_s->song.chart.meta.name;
                r.artist     = g_s->song.chart.meta.artist;
                r.score      = st.score;
                r.notesHit   = st.notesHit;
                r.notesTotal =
                    static_cast<int>(g_s->song.chart.notes.size());
                r.maxCombo    = st.maxCombo;
                r.overstrums  = st.overstrums;
                r.spPhrases   = st.spPhrasesCompleted;
                r.fullCombo   = st.notesMissed == 0 && st.overstrums == 0 &&
                                r.notesTotal > 0;
                acc =
                    r.notesTotal > 0
                        ? static_cast<double>(st.notesHit) / r.notesTotal
                        : 0.0;
                stars::StarParams sp;
                {
                    const auto& se = Settings::GetSingleton();
                    sp.t[0] = se.star1; sp.t[1] = se.star2;
                    sp.t[2] = se.star3; sp.t[3] = se.star4;
                    sp.t[4] = se.star5;
                }
                earned = stars::StarsFromAccuracy(acc, sp);
                // read BEFORE Record, or the run has already overwritten
                // the record it is supposed to be compared against.
                // Keyed on the difficulty the run ACTUALLY played
                // (LoadSong's fallback pick), never the requested setting -
                // GH convention: an "Expert" pick on a Hard-only chart is
                // a Hard record.
                const int playedDiff = g_s->song.resolvedDifficulty;
                r.difficulty = playedDiff;
                r.prevBest = StarLedger::GetSingleton().Best(
                    g_s->chartKey, g_s->instrument, playedDiff);
                r.newBest  = earned > r.prevBest;
                StarLedger::GetSingleton().Record(g_s->chartKey,
                                                  g_s->instrument,
                                                  playedDiff, earned);
                r.stars = earned;
                FillProgressionFeedback(r, acc, earned, deferPayout, inst,
                                        songLen, g_s->difficulty, feedBonus);
                if (r.rankKnown && r.rankAfter > r.rankBefore) {
                    const auto songs = SongLibrary::GetSingleton().Snapshot();
                    if (songs) {
                        auto& ledger = StarLedger::GetSingleton();
                        for (const auto& song : *songs) {
                            if (!songeligibility::IsEligible(
                                    song.instrument,
                                    instrumentContext)) {
                                continue;
                            }
                            const int need = song.requiredRank;
                            if (need > r.rankBefore &&
                                need <= r.rankAfter) {
                                ledger.MarkNew(song_identity::ChartKey(
                                    song.entry.folder));
                            }
                        }
                    }
                }
                // The room's verdict, once the stars are known. Completed
                // songs only: an abort has no verdict to give, and `earned`
                // is 0 on that path, which would sting the player with the
                // awkward silence for quitting. Computed BEFORE staging so
                // the results snapshot can carry the electric sting cue:
                // firing the sting here landed ~1.75s before the menu
                // (field 2026-07-25 log: sting 22:34:43.47, results
                // published 22:34:45.22), so ResultsWindow fires it on its
                // first draw instead. The crowd-loss FAILED teardown never
                // reaches this block - its sting fires at the loss itself.
                const auto& se = Settings::GetSingleton();
                const ending::Thresholds reactionThresholds{
                    se.reactionNeutralStars, se.reactionPositiveStars
                };
                const auto endingValence = ending::ValenceForPerformance(
                    earned, finishedGreat, reactionThresholds);
                if (const auto sting = ui_sfx::StingForEnding(
                        instrumentContext, endingValence)) {
                    r.stingCue = static_cast<int>(*sting);
                }
                r.clearSting = ui_sfx::VanillaClearSting(instrumentContext,
                                                         endingValence);
                UiBus::GetSingleton().StageResults(r);
                spdlog::info(
                    "[results] staged; waiting for FUCK input lookup to "
                    "quiesce");
                // Reset first because the middling verdict is a kCheer, and
                // kCheer is rate-limited: a milestone cheer three seconds
                // before the last note would otherwise eat the only reaction
                // the whole performance was building to. The cooldown's
                // history belongs to the song that just ended.
                //
                // This is why the crowd has its own audio device. The
                // session's engine is uninitialised a few hundred
                // microseconds below, which is a fraction of one 10ms device
                // period: an applause played on it would be dropped
                // unrendered, and by ear a tester could not tell that from
                // its never having fired. On the crowd's own engine the clip
                // outlives the teardown and plays into the silence after the
                // final note, which is where it belongs.
                CrowdReactions::Reset();
                if (endingValence == ending::Valence::kPositive) {
                    CrowdReactions::Fire(
                        CrowdReactions::Kind::kApplause, QpcSec());
                } else if (endingValence == ending::Valence::kNegative) {
                    CrowdReactions::Fire(
                        CrowdReactions::Kind::kAwkward, QpcSec());
                }
                // The rank-up swell (spec 3.4). kSwell was loaded, indexed
                // and callerless since the bank was written - this is the
                // moment it was reserved for. AFTER the room's own reaction
                // so the order reads applause-then-swell, and bypassing the
                // cooldown for the same reason that pair does.
                //
                // ⚠ It is still behind [SGT] bCrowdReactions, which now
                // DEFAULTS TO 0 (the ambient one-shots were redundant on top
                // of SGT's own NPC chatter). So the log line is the proof
                // the crossing was detected; the sound is a separate
                // question about that setting and the placeholder .wav.
                if (r.rankKnown && r.rankAfter > r.rankBefore) {
                    spdlog::info(
                        "[rank] {} -> {} ({} -> {}) on {} - swell",
                        r.rankBefore, r.rankAfter,
                        results::StandingName(r.rankBefore),
                        results::StandingName(r.rankAfter),
                        static_cast<int>(g_s->instrument));
                    CrowdReactions::Fire(CrowdReactions::Kind::kSwell,
                                         QpcSec());
                }
            }
            if (g_s->engine) {
                const auto& es = g_s->engine->Stats();
                if (sgtWhole && completed) {
                    // defer-and-scale: SGT's own payout at the honest time,
                    // then the accuracy multiplier on what it actually paid,
                    // and finally the spec 5.6 top-up for everything SGT
                    // never pays for at all.
                    //
                    // The mood argument is the room's DOMINANT level, not
                    // merely the needle zone at the final note. One bad
                    // ending phrase must not erase a mostly great run. See
                    // payout::MoodTally. An empty tally answers middling,
                    // which pays half rather than nothing.
                    GoldScale::ArmDeferred(
                        es.notesHit,
                        static_cast<int>(g_s->song.chart.notes.size()),
                        g_s->difficulty, earned, g_moodTally.Dominant(),
                        SgtProgression::RankFromExpertise(
                            SgtProgression::UiSampled(inst)),
                        g_playerAtInn.load(), songLen);
                } else {
                    GoldScale::OnSessionEnd(
                        completed, es.notesHit,
                        static_cast<int>(g_s->song.chart.notes.size()),
                        g_s->difficulty);
                }
            }
            g_s->audio.Uninit();
            delete g_s;
            g_s = nullptr;
            UiBus::GetSingleton().glory.store(0.5f);
            UiBus::GetSingleton().gloryDanger.store(false);
            PostRestoreDucking();
            MovementGuard::Post(false);
            // Hand SGT's reaction pair back the way its own OnEffectFinish
            // leaves it (0/0). This is the SHARED teardown - "song
            // complete", "aborted", "aborted: SGT performance ended
            // externally" and "aborted: cell/worldspace change" all land
            // here - so it covers every exit but the load interrupt, which
            // OnPreLoadGame does.
            //
            // NOT on the deferred-payout path. There we deliberately keep
            // SGT's effect alive past song end so it pays out at the honest
            // time, and that gap IS the applause: the Good/Terrible CTDAs
            // sit on INFO records, so the scene re-reads the pair for every
            // line it picks. Clearing here would land ~3s early and
            // disqualify the lot. That path clears at the strip instead
            // (g_endStripSpell), which is also where SGT would have zeroed
            // the pair itself if its OnEffectFinish could still run.
            //
            // Deliberately NOT in PauseSession either: a pause is not an
            // end, and clearing there would wipe the crowd every time the
            // player opens the pause menu mid-song.
            if (!deferPayout && !crowdFailed) {
                SKSE::GetTaskInterface()->AddTask(
                    [] { MoodGlobals::Clear(); });
            }
            if (crowdFailed) {
                g_failureInstrumentContext = instrumentContext;
                g_failureToken = g_failureEpoch.fetch_add(1) + 1;
                const auto plan = g_failureFeedback.Begin(QpcSec());
                g_failureFeedbackActive.store(true);
                // ...and lock Space out for the same window. The recoil
                // animation is the reaction; this is what actually stops the
                // jump (see UiBus::failureInputLockout).
                UiBus::GetSingleton().failureInputLockout.store(
                    true, std::memory_order_release);
                if (plan.dispatchFeedback) {
                    PostCrowdFailureOutcome(instrumentContext, inst,
                                            g_failureToken,
                                            plan.stopAudience);
                }
            }
            if (deferPayout) {
                // feedBonus was computed in FillProgressionFeedback, which
                // runs on this same completed path - the results window had
                // to predict the same grant this dispatch is about to make,
                // and two copies of that arithmetic would drift.
                const int bonus = feedBonus;
                if (FUCK::GetInterface()) {
                    // the results box is about to freeze the world - hold
                    // the payout until its close lifts the freeze (see
                    // g_pendingPayout; the kIdle watcher dispatches)
                    g_pendingPayout = PendingPayout{
                        inst, instrumentContext, earned, bonus, finishedGreat
                    };
                } else {
                    // no UI = no results box, no freeze: dispatch now and
                    // strip after MessageAndEXP had time to run (see the
                    // g_endStripSpell comment); a new session start cancels
                    PostSgtFinishPerformance(
                        instrumentContext, inst, earned, bonus,
                        finishedGreat);
                    g_endStripSpell =
                        PerformSpellForContext(instrumentContext);
                    g_endStripAt = QpcSec() + 3.0;
                }
            } else if (!crowdFailed) {
                PostEndSgtPerformance(instrumentContext);
            }
            // post-session lock diagnostics (field round 3): kProbeSchedule,
            // from +0.0s (baseline at the instant of end) out to +20s
            g_probeEndAt = QpcSec();
            g_probeIdx   = 0;
            // ...and log the first events that actually reach the hook on the
            // pass-through path. passthrough=N proves the hook RUNS, not that
            // any input ARRIVES - a frame with zero events counts the same.
            InputHook::ArmPassthroughTrace(60);
            // arm the recovery retry: the session-end attempt runs under our
            // own world pause and stands down, so keep trying once the world
            // is actually unpaused
            g_recoverUntil = QpcSec() + 8.0;
            g_recoverNext  = 0.0;
            // the performance idle has to END, not just have controls back -
            // the third lock layer (see SgtVm::ReleasePerformPose)
            g_poseReleasePending.store(true);
            g_sgtLogic.OnSessionEnd();
            GoldScale::SetWholeSongLive(false);
            // a start pushed while this session was active is stale by
            // definition - drain it so a minutes-old pick can't fire at the
            // next idle tick
            UiBus::GetSingleton().TakePendingStart();
            UiBus::GetSingleton().ClearPracticeHud();
            UiBus::GetSingleton().pauseMenuOpen.store(false);
            UiBus::GetSingleton().resumeCountdownActive.store(false);
            UiBus::GetSingleton().resumeCountdownCue.store(0);
            PostWorldPause(false);
            g_menuLogUntil.store(QpcSec() + 30.0);  // wedge diagnostics
            g_state.store(State::kIdle);
        }

        bool StartSession(const bard::SongEntry* chosen, int difficulty,
                          int a_instrumentContext,
                          PracticeRequest a_practice = {}) {
            const auto& st = Settings::GetSingleton();
            // a pending delayed end-strip must not fire into this session's
            // performance (fast re-start: the ability carries over and the
            // strip would kill it mid-song -> keeper kLost -> abort)
            g_endStripSpell = 0;
            auto        s  = std::make_unique<SessionData>();
            s->difficulty  = difficulty;

            bard::SongEntry picked;
            if (chosen) {
                picked = *chosen;
            } else {  // legacy path: no FLICK -> first scanned song
                std::vector<std::filesystem::path> roots{
                    path_text::FromUtf8(st.songsFolder)
                };
                if (!st.userSongsFolder.empty()) {
                    roots.push_back(st.userSongsFolder);
                }
                const auto scan = bard::ScanSongs(roots);
                for (const auto& bad : scan.bad) {
                    spdlog::warn("[session] bad song folder {}: {}",
                                 path_text::Utf8(bad.folder), bad.reason);
                }
                if (scan.songs.empty()) {
                    spdlog::error(
                        "[session] no songs in configured song roots");
                    return false;
                }
                picked = scan.songs.front();
            }
            std::string err;
            if (!bard::LoadSong(picked, difficulty, s->song, &err)) {
                spdlog::error("[session] LoadSong failed: {}", err);
                return false;
            }
            spdlog::info("[session] \"{}\" by \"{}\" ({} notes)",
                         s->song.chart.meta.name, s->song.chart.meta.artist,
                         s->song.chart.notes.size());
            // The field tell for the per-difficulty split: which difficulty
            // the fallback actually built vs what was requested, plus the
            // chart's availability mask (bit 0..3 = Easy..Expert).
            spdlog::info(
                "[session] difficulty requested={} resolved={} mask=0x{:X}",
                difficulty, s->song.resolvedDifficulty,
                s->song.difficultyMask);
            // Freeze both identities. Guitar remains a distinct ability and
            // repertoire context, while its progression and payout identity
            // deliberately reuse lute.
            s->instrumentContext =
                songeligibility::IsBoundContext(a_instrumentContext)
                  ? a_instrumentContext
                  : static_cast<int>(
                        StarLedger::GetSingleton().ActiveInstrument());
            const int progressionContext =
                songeligibility::ProgressionContext(s->instrumentContext);
            if (progressionContext < 0) {
                spdlog::error(
                    "[session] invalid instrument context {} at start",
                    s->instrumentContext);
                return false;
            }
            s->instrument =
                static_cast<stars::Instrument>(progressionContext);
            s->chartKey   = song_identity::ChartKey(picked.folder);
            for (const auto& [name, path] : picked.stems) {
                (void)path;
                s->bandStems.Note(name);
            }

            if (!s->audio.Init()) return false;
            s->audio.SetSongVolume(Settings::GetSingleton().songVolume);
            {
                // Star Power flanger: push the INI tuning while the filter
                // is guaranteed idle (SP cannot be active before the song
                // starts). The per-tick SetSpFilter publish below gates on
                // bSpFilter, so disabling it never even arms the effect.
                const auto&     cfg = Settings::GetSingleton();
                SpFlangerParams fp;
                fp.rateHz   = static_cast<float>(cfg.spFilterRateHz);
                fp.baseMs   = static_cast<float>(cfg.spFilterBaseMs);
                fp.depthMs  = static_cast<float>(cfg.spFilterDepthMs);
                fp.feedback = static_cast<float>(cfg.spFilterFeedback);
                fp.wet      = static_cast<float>(cfg.spFilterWet);
                s->audio.SetSpFilterParams(fp, cfg.spFilterSongStem);
            }
            const int stemCount = s->audio.LoadStems(
                picked.stems, Settings::GetSingleton().worldAudio);
            if (stemCount == 0) {
                spdlog::error("[session] no stems loaded");
                s->audio.Uninit();
                return false;
            }
            if (st.missSfx) {
                std::vector<std::filesystem::path> sfx;
                std::error_code                    ec, ec2;
                if (ui_sfx::ElectricSoundContext(s->instrumentContext)) {
                    // Electric context: the five recorded guitar flubs
                    // replace the synthesized miss1..3 rotation
                    // (request-to-main-2026-07-25-ui-sfx.md). Explicit
                    // names rather than a scan - sfx/ui also holds the
                    // non-miss UI cues - and the same LoadMissSfx path,
                    // so the InputHook trigger, the round-robin, and
                    // fMissSfxVolume stay exactly as they are (the files
                    // are level-matched to gen_miss_sfx.py's -6 dBFS).
                    for (int i = 1; i <= 5; ++i) {
                        sfx.emplace_back(
                            "Data/SKSE/Plugins/BardHero/sfx/ui/"
                            "miss_electric" +
                            std::to_string(i) + ".wav");
                    }
                } else {
                    for (const auto& e :
                         std::filesystem::directory_iterator(
                             "Data/SKSE/Plugins/BardHero/sfx", ec)) {
                        // is_regular_file, not just the extension: this
                        // folder has crowd/ and ui/ subdirectories in it.
                        // Non-recursive iteration would not descend into
                        // them today, but a directory literally named
                        // "something.wav" would still pass an
                        // extension-only test, and a later switch to
                        // recursive_directory_iterator would quietly pull
                        // every crowd and UI clip into the miss bank.
                        if (e.is_regular_file(ec2) &&
                            e.path().extension() == L".wav") {
                            sfx.push_back(e.path());
                        }
                    }
                    std::sort(sfx.begin(), sfx.end());
                }
                if (ec || sfx.empty()) {
                    spdlog::warn("[session] no miss sfx found - feature off");
                } else {
                    s->audio.LoadMissSfx(
                        sfx, static_cast<float>(st.missSfxVolume));
                }
            }
            s->songLen = s->audio.MaxStemLengthSec();

            // ---- practice mode (spec 2026-07-26-practice-mode 6) ---------
            // Resolved here because this is the first point where BOTH the
            // chart (LoadSong) and the song length (the stems) are known,
            // and it is still ahead of every consumer: the engine below is
            // built from song.chart, so slicing first is all it takes for
            // the engine to hold the slice.
            //
            // The rules are resolved for EITHER mode, unconditionally. P3
            // reads named fields off this instead of re-testing `practice`
            // at each of the nine commit sites.
            s->practice    = a_practice.practice;
            s->loopEnabled = a_practice.loopEnabled;
            // Clamped through the same stepper the live keys use, so a
            // preset and a key press can never disagree about what is a
            // legal speed. Non-practice is always 1.0.
            s->speed = a_practice.practice
                           ? bard::practice::StepSpeed(a_practice.speed, 0)
                           : 1.0;
            s->rules    = s->practice ? bard::practice::PracticeRules()
                                      : bard::practice::PerformanceRules();
            if (s->practice) {
                s->fullChart = s->song.chart;  // keep the unsliced original
                s->range     = bard::practice::ResolveRange(
                    s->song.chart.sections, a_practice.startSection,
                    a_practice.endSection, s->songLen);
                // Sliced from notesFromSec, NOT startSec. The gap between
                // them is the ease-in: playback seeks to startSec so the
                // player hears their way in, but the notes belonging to the
                // previous section are omitted rather than thrown at them
                // during that window. Slicing from startSec is what made
                // every loop restart open with notes from a section the
                // player is not practising.
                s->song.chart = bard::practice::SliceChart(
                    s->fullChart, s->range.notesFromSec, s->range.endSec);
                s->startSongSec = bard::practice::RestartTime(s->range);
                spdlog::info(
                    "[practice] range [{:.2f}s, {:.2f}s] sections {}..{} - "
                    "{} of {} notes sliced in (song {:.1f}s, {} markers)",
                    s->range.startSec, s->range.endSec, s->range.startSection,
                    s->range.endSection, s->song.chart.notes.size(),
                    s->fullChart.notes.size(), s->songLen,
                    s->fullChart.sections.size());
            }
            // Practice HUD (P5). Published unconditionally so a normal
            // session actively CLEARS a strip left over from a practice run
            // that ended badly, rather than relying on the teardown having
            // got there - a stale "LOOP 7" over a real performance is the
            // kind of latch this project keeps paying for.
            {
                auto& bus = UiBus::GetSingleton();
                // A summary from the previous practice run must not still be
                // on screen over this one.
                bus.ClearPracticeSummary();
                // Unconditional belt-and-braces clear. There are three other
                // paths that clear the failure lockout; a session starting
                // with Space still swallowed would be unrecoverable, so this
                // does not trust any of them.
                bus.failureInputLockout.store(false,
                                              std::memory_order_release);
                if (s->practice) {
                    bus.practiceLoop.store(0);
                    bus.practiceSpeed.store(static_cast<float>(s->speed));
                    bus.practiceStartSection.store(s->range.startSection);
                    bus.practiceEndSection.store(s->range.endSection);
                    bus.practiceActive.store(true, std::memory_order_release);
                } else {
                    bus.ClearPracticeHud();
                }
            }

            // wait for the first anchor so the start projection has a base
            for (int i = 0; i < 100 && !s->audio.AnchorPublished(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!s->audio.AnchorPublished()) {
                spdlog::error("[session] no audio callback within 200ms");
                s->audio.Uninit();
                return false;
            }

            // Size the lead-in against SGT's start wind-up, so the first
            // note never lands before the character is playing (see
            // SgtStartLead.h). With Phase 3's browse sheathe working the
            // weapon is already away and this IS kSongStartDelay - the
            // sheathed path must not gain a needless delay. It only widens
            // when a weapon is somehow still drawn at pick, where we know
            // SGT is about to pay its 2s branch.
            const bool   drawn = g_weaponDrawn.load();
            const double lead  = sgtlead::LeadSeconds(
                bard::MasterClock::kSongStartDelay, drawn);
            if (lead > bard::MasterClock::kSongStartDelay) {
                spdlog::info(
                    "[session] lead-in widened to {:.2f}s - a weapon is "
                    "still drawn, so SGT will pay its 2s sheathe branch "
                    "before the lute idle starts",
                    lead);
            }
            // Practice starts AT its range, not at song 0. The seek happens
            // BEFORE ScheduleStart, which is the one ordering that needs no
            // second seek path: the stems are not started yet, so the cursor
            // simply sits at the range while the countdown runs, and
            // ScheduleStart's X becomes "the engine frame at which song
            // position startSongSec plays". Seeking AFTER ScheduleStart -
            // via SeekTo, the loop's own path - would be wrong here, because
            // SeekStems derives its start frame from engine time NOW while
            // the stems do not actually begin until frame X, `lead` seconds
            // later: the whole run would sit `lead` out of sync.
            //
            // startSongSec is 0.0 for a normal session, and every expression
            // below collapses to exactly what it was then
            // (FramesForSongSec(0) == 0, StartFrameForSeek(X, 0) == X).
            const double startSongSec = s->startSongSec;
            if (startSongSec > 0.0) { s->audio.SeekStems(startSongSec); }
            const auto X = s->audio.ScheduleStart(lead);
            const auto a = s->audio.ReadAnchor();
            // project the QPC instant at which the engine consumes frame X;
            // clock and audio then share one time base and the constant
            // pipeline offset cancels in the delta (M0 result)
            const double tX =
                a.qpc + static_cast<double>(static_cast<std::int64_t>(
                            X - a.frames)) / AudioEngine::kSampleRate;
            // NOT `tX - lead`: MasterClock::Start adds kSongStartDelay
            // itself, so this expression is what pins InputTime 0 to tX -
            // the projected QPC of audio frame X - whatever the lead is.
            // The countdown gets longer because X moved, not because the
            // clock was rebased; feeding the lead in here too would run the
            // chart ahead of the audio by (lead - kSongStartDelay).
            //
            // The extra startSongSec term pins InputTime startSongSec (NOT
            // 0) to tX, because that is the song position the seeked stems
            // begin playing at frame X.
            s->clock.Start(tX - bard::MasterClock::kSongStartDelay -
                           startSongSec);
            // Apply a pre-chosen practice speed HERE, through SetSpeed,
            // rather than threading it into Start() above. Start()'s offset
            // is derived from the projected audio frame and the lead-in, and
            // folding a speed term into that arithmetic is a second way to
            // get it wrong; SetSpeed exists precisely to rebase the offset so
            // the outputs stay continuous, and it is the same call the live
            // -/= keys already use. One tested path, not two.
            if (s->speed != 1.0) {
                s->audio.SetSongSpeed(s->speed);
                s->clock.SetSpeed(QpcSec(), s->speed);
                spdlog::info("[practice] starting at {:.2f}x", s->speed);
            }
            s->est.emplace(AudioEngine::kSampleRate,
                           s->audio.CallbackPeriodSec());
            // X is the engine frame at which startSongSec plays; the
            // estimator wants the frame at which song 0 plays, i.e.
            // X - songFrames. That difference is NEGATIVE whenever the range
            // starts later in the song than this AudioEngine has been alive,
            // which is nearly always: Init runs a fraction of a second
            // above, so X is only ~2-3 seconds' worth of frames.
            // StartFrameForSeek carries it as a modular value and
            // AnchorEstimator::Position reads it back EXACTLY - its
            // `static_cast<std::int64_t>(a.frames - _start)` is the matching
            // modular subtraction and the result always fits in an int64.
            //
            // SeekStems' own return value is NOT what belongs here, and that
            // is not a style choice: it is measured from engine time NOW,
            // while these stems do not begin until frame X, `lead` seconds
            // later. Using it would put the whole run `lead` out of sync.
            // The base has to be X. (The loop's SeekTo is the opposite case
            // and does use the return value.)
            const auto songFrames = stem_seek::FramesForSongSec(
                startSongSec, AudioEngine::kSampleRate);
            s->est->SetStartFrame(stem_seek::StartFrameForSeek(X, songFrames));
            s->engine = std::make_unique<bard::GuitarEngine>(
                s->song.chart,
                difficulty::EngineParamsFor(
                    Settings::GetSingleton().tuning));

            g_s = s.release();
            g_s->startCell = 0;
            g_playerCell.store(0);  // clear stale value from a prior session
            g_playerWs.store(0);
            g_playerInterior.store(false);
            g_playerAtInn.store(false);
            // request flags can survive a same-tick teardown of the prior
            // session (the sink early-returns on kIdle and never clears them)
            g_reqPause.store(false);
            g_reqAbort.store(false);
            g_reqStart.store(false);
            g_reqRestart.store(false);
            g_inLoadingMenu.store(false);
            g_uiPaused.store(false);
            {
                std::scoped_lock lk(g_menuTrackMx);
                g_nativeMenus.Clear();
            }
            g_nativeMenuOpen.store(false, std::memory_order_release);
            UiBus::GetSingleton().resumeCountdownActive.store(false);
            UiBus::GetSingleton().resumeCountdownCue.store(0);
            GoldScale::OnSessionStart();
            {
                // spec 5 enforcement point: session start
                const auto inst = g_s->instrument;
                // ClampPass is a WRITE, despite reading like a check: it
                // mutates the persisted lifted mask and can drive the
                // expertise global DOWNWARD. That is why "practice records
                // nothing" has to cover it (rules.enforceRankGate) even
                // though nothing about it looks like scoring.
                if (g_s->rules.enforceRankGate) {
                    SKSE::GetTaskInterface()->AddTask([inst] {
                        SgtProgression::ClampPass(inst, "session start");
                    });
                }
                // ...then sample the expertise GLOBs, so the payout's rank
                // can be read on the session thread at song end. Queued
                // AFTER the clamp on purpose: game-thread tasks are FIFO, so
                // the sample is the POST-clamp value, which is the renown the
                // player actually performs at. It is also pre-promotion -
                // BeginPromotion runs later, at the deferred payout dispatch.
                // A practice run skips the clamp, so this samples the
                // unclamped value - harmless, because practice pays nothing
                // that could read it.
                SgtProgression::PostUiSample();
            }
            g_sgtGen.fetch_add(1);  // invalidate any in-flight keeper store
            g_sgtSeen.store(-1);
            g_sgtClipStopped.store(false);
            g_sgtPauseDeathLogged = false;
            if (Settings::GetSingleton().wholeSongPerform && AnySpell()) {
                g_sgtLogic.OnSessionStart(QpcSec());
                // every performance starts with clean SgtVm latches no
                // matter how the previous one ended (FIFO: runs before
                // this session's first keeper pass)
                SKSE::GetTaskInterface()->AddTask(
                    [] {
                        SgtVm::ResetPerformanceLatches();
                        SgtVm::ResetAudience();
                    });
            }
            // longest sustain span: computed outside the lock (only reads
            // g_s->song, which nothing else touches yet) to keep the
            // critical section tight
            double maxSus = 0.0;
            for (const auto& n : g_s->song.chart.notes) {
                for (int l = 0; l < bard::kLaneCount; ++l) {
                    if (n.sustainEnd[l] > 0.0) {
                        maxSus = std::max(maxSus, n.sustainEnd[l] - n.time);
                    }
                }
            }
            // publish the feed for the game-thread hook: pointers first
            // (under mx), gates after - the hook checks `active` before
            // dereferencing under the same mutex
            {
                auto&            feed = EngineFeed::GetSingleton();
                std::scoped_lock lk(feed.mx);
                feed.engine   = g_s->engine.get();
                feed.clock    = &g_s->clock;
                feed.counters = {};
                feed.song       = &g_s->song;
                feed.audio      = &g_s->audio;
                feed.songLen    = g_s->songLen;
                feed.maxSustainSec = maxSus;
                // solo-instrument songs (song.ini flag): the sole stem IS
                // the instrument - whole-track miss-mute is the right feel
                int gs = g_s->audio.FindStem("guitar");
                if (gs < 0 && g_s->song.ini.singleInstrument &&
                    stemCount == 1) {
                    gs = 0;
                }
                // Practice NEVER miss-mutes. Dropping out the instrument on
                // every fluff is the right punishment in a performance and
                // the wrong one when the whole point is drilling a passage
                // you cannot play yet: it removes the reference you are
                // trying to play along to, exactly when you need it most.
                // -1 is the "no stem to mute" value the teardown path
                // already uses, so this needs no new branch downstream.
                feed.guitarStem = g_s->practice ? -1 : gs;
            }
            EngineFeed::GetSingleton().active.store(true);
            EngineFeed::GetSingleton().engaged.store(true);
            // a previous session's camera/player placement may be a whole
            // cell away - drop it rather than attenuate the first 33ms
            // against it
            g_placementValid.store(false, std::memory_order_release);
            g_placementLogged.store(false, std::memory_order_release);
            // (The control-baseline capture that used to run here went with
            // the end-of-session recovery on 2026-07-22 - it sampled a field
            // that is not the control mask on this runtime. See SgtVm.h.)
            // Probe 0 is the session-START snapshot: before MovementGuard's
            // block and before SGT's disable (FIFO puts it ahead of both).
            // It is NOT a free-roam baseline and must not be read as one -
            // the field run of 2026-07-21 11:52 showed it firing with
            // engaged=true, i.e. our own hook already filtering, so its
            // moveInputVec is zero whatever the player is doing. The control
            // for "what does this read when the player CAN move" is the
            // on-demand probe on iProbeKey.
            SKSE::GetTaskInterface()->AddTask([] { LogControlProbe(0); });
            MovementGuard::Post(true);
            // the crowd's opinion is per-song: a window left over from the
            // last performance would commit a level before a note is played
            g_mood.Reset();
            g_failureGate.Reset();
            // Per-song, same as the mood window: a cloak left lit by the last
            // performance would have the player walk on already alight, and
            // would then never re-ignite because the state already said lit.
            // It also clears the refresh clock, which a new song rewinds.
            ResetStreakFire();
            g_streakFireParams.firstAt = st.streakFireHandsAt;
            // Reserved - the second tier is not wired. See Params::bigAt.
            g_streakFireParams.bigAt   = st.streakFireBlazeAt;
            UiBus::GetSingleton().glory.store(0.5f);
            UiBus::GetSingleton().gloryDanger.store(false);
            // A neutral SGT mood still qualifies dialogue. Begin at 0/0 so
            // the audience listens to the opening phrase before judging it.
            // This runs after ResetAudience's earlier game-thread task and
            // the 10 Hz feed reasserts it because SGT writes the same globals
            // during its delayed startup.
            SKSE::GetTaskInterface()->AddTask([] {
                MoodGlobals::Clear();
                SgtVm::ReleaseAudienceCelebration();
            });
            g_moodTally = payout::MoodTally{};  // ...and so is what it paid
            // ...and neither is its voice. The crowd owns its own audio
            // device and bank; Prepare opens them on the FIRST session only
            // and returns immediately after that. It sits here, at the end
            // of the start sequence, rather than beside the miss-sfx load:
            // opening a second WASAPI device earlier would spend part of the
            // 200ms budget the anchor wait above depends on, whereas a stall
            // here only delays the first tick of a lead-in countdown that is
            // anchored to audio frames, not to this thread.
            CrowdReactions::Prepare();
            CrowdReactions::Reset();
            // The UI SFX bank shares the crowd's timing rationale exactly
            // (its stings and results tick outlive the session engine),
            // so its once-per-process device open sits in the same slot.
            UiSfx::Prepare();
            g_lastCheerMilestone = 0;
            g_prevCombo          = 0;
            PostCaptureContext();   // main thread refills it + applies duck
            // The NEW badge survives browsing and load failures, then clears
            // only once this chart has actually reached a live session.
            // Practice is deliberately not that live session: MarkSeen is a
            // PERSISTED write, and the badge means "you have performed this",
            // which practising it has not made true (rules.clearNewTags).
            if (g_s->rules.clearNewTags) {
                StarLedger::GetSingleton().MarkSeen(g_s->chartKey);
            }
            // Remember what this session was, so the P2 practice debug key
            // can re-open the same song without a picker (see SessionThread).
            g_lastPicked            = picked;
            g_lastDifficulty        = difficulty;
            g_lastInstrumentContext = g_s->instrumentContext;
            spdlog::info(
                "[session] started: len={:.1f}s startFrame={} lead={:.2f}s "
                "weaponDrawn={} practice={} startAt={:.2f}s",
                g_s->songLen, X, lead, drawn, g_s->practice,
                g_s->startSongSec);
            // Third-party HUD widgets (STB etc.) go dark for the whole
            // session; every end path below restores them.
            WidgetMuffle::OnSessionStart();
            return true;
        }

        void PauseSession() {
            if (!g_s) return;
            g_s->resumeCountdown.Cancel();
            g_s->resumeAudioStarted = false;
            UiBus::GetSingleton().resumeCountdownActive.store(false);
            UiBus::GetSingleton().resumeCountdownCue.store(0);
            auto& feed = EngineFeed::GetSingleton();
            feed.engaged.store(false);  // routing off: keys reach the game
            MovementGuard::Post(false);
            const double raw = QpcSec();
            {
                std::scoped_lock lk(feed.mx);
                g_s->clock.Pause(raw);
            }
            g_s->audio.SetPaused(true);
            g_s->audio.SetRate(1.0);
            g_s->ctrl.Reset();
            g_state.store(State::kPaused);
            // pause UX (field round 6): menu window + frozen world
            UiBus::GetSingleton().pauseMenuOpen.store(true);
            PostWorldPause(true);
            spdlog::info("[session] paused @ {:.2f}s",
                         g_s->clock.SongTime(raw));
        }

        // Rebase a LIVE session onto a_songSec: audio, the anchor estimator,
        // the chart clock and the judgment engine, all together. The one
        // path practice's loop restart uses, and the only place in the
        // project that moves a running session backwards in time.
        //
        // Sequenced against the two hazards that make a naive seek a field
        // crash or a silent hang:
        //
        //  1. AnchorEstimator's monotonic clamp (AnchorEstimator.cpp:11).
        //     After a BACKWARDS seek Position() would return the stale
        //     pre-seek value forever, so the clock would never re-enter the
        //     range and the loop would look frozen with no error anywhere.
        //     SetStartFrame is the only thing that clears it, so the call
        //     below is not optional. It runs while audio is PAUSED, which is
        //     what makes the frame counter it is derived from non-stale.
        //
        //  2. HighwayWindow.cpp / InputHook.cpp index the ENGINE with note
        //     indices taken from feed.song->chart, and GuitarEngine's
        //     JudgmentOf is unchecked. feed.song and feed.engine must
        //     therefore be swapped inside ONE critical section - two locks
        //     would leave a window where the render thread reads a fresh
        //     engine against a stale chart, which is an out-of-bounds read
        //     on every note past the slice.
        //
        // Returns true when the session is playing again. On false the
        // audio device never came back; the session has been left in a
        // CONSISTENT kPaused state (engine and chart still agree, clock
        // rebased onto the target) rather than spinning, and the caller must
        // not re-engage the feed.
        [[nodiscard]] bool SeekTo(double a_songSec) {
            auto& feed = EngineFeed::GetSingleton();
            // 1-3: stop feeding, freeze audio, and drop drift engagement -
            // the controller's integral is meaningless across a
            // discontinuity and would fight the new anchor for seconds.
            feed.engaged.store(false);
            g_s->audio.SetPaused(true);
            g_s->ctrl.Reset();
            g_s->audio.SetRate(1.0);
            // SetPaused only sets a flag the audio thread reads at the TOP
            // of its callback, so a callback that started a moment earlier
            // is still inside ma_engine_read_pcm_frames. Seeking the stems
            // under it would race the mixer on the data source. A published
            // anchor with rate == 0 proves a callback has since run and
            // observed the pause, so the mixer is out of the sounds. Bounded
            // and best-effort: a device that never publishes has bigger
            // problems, and the 500ms live-wait below is what reports it.
            const double quiesceFrom = QpcSec();
            while (g_s->audio.ReadAnchor().rate != 0.0 &&
                   QpcSec() - quiesceFrom <= 0.1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // 4-5: seek, then clear the monotonic floor with the frame song
            // position 0 now plays at. Both while paused.
            //
            // SeekStems returns exactly that frame, and it is the tested
            // path (test_stemseek), so the arithmetic is not repeated here.
            //
            // Its base is ma_engine_get_time_in_pcm_frames while Position()
            // compares against the anchor's framesConsumed. Those are one
            // domain by construction: framesConsumed accumulates precisely
            // the frames read out of the engine (AudioEngine.cpp:268), and
            // the normal start path already feeds engine time straight into
            // SetStartFrame via ScheduleStart. The quiesce above froze both,
            // so it does not matter that SeekStems samples after the seek
            // and the old code sampled before it.
            //
            // The returned value is routinely a WRAPPED representation of a
            // negative frame - any range starting later in the song than
            // this AudioEngine has been alive produces one, which for a
            // short section late in a long song is every restart. That is
            // intended; Position's matching modular subtraction recovers it
            // exactly. Read StemSeekLogic.h before touching this.
            const std::uint64_t X = g_s->audio.SeekStems(a_songSec);
            g_s->est->SetStartFrame(X);
            // 6-7: let audio run and wait for a live callback, exactly as
            // the resume path does, with the same 500ms watchdog.
            g_s->audio.SetPaused(false);
            const double seekStartedAt = QpcSec();
            bard::AudioAnchor anchor{};
            bool              live = false;
            while (QpcSec() - seekStartedAt <= 0.5) {
                anchor = g_s->audio.ReadAnchor();
                if (anchor.rate > 0.0) {
                    live = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const double raw = QpcSec();
            // Position() is safe to call whether or not the device came
            // back: SetStartFrame above reset the clamp, so a dead device
            // simply reports the seek target.
            const double pos = live ? g_s->est->Position(anchor, raw)
                                    : a_songSec;
            // 8: one engine per run - GuitarEngine steps FORWARD only, so a
            // restart is a rebuild, not a rewind. Built and measured
            // outside the lock (it copies the chart by value) so the game
            // thread's input hook blocks for a pointer swap and nothing
            // more.
            auto rebuilt = std::make_unique<bard::GuitarEngine>(
                g_s->song.chart,
                difficulty::EngineParamsFor(
                    Settings::GetSingleton().tuning));
            double maxSus = 0.0;
            for (const auto& n : g_s->song.chart.notes) {
                for (int l = 0; l < bard::kLaneCount; ++l) {
                    if (n.sustainEnd[l] > 0.0) {
                        maxSus = std::max(maxSus, n.sustainEnd[l] - n.time);
                    }
                }
            }
            {
                std::scoped_lock lk(feed.mx);
                g_s->clock.ResumeSynced(raw, pos);
                g_s->engine        = std::move(rebuilt);
                feed.engine        = g_s->engine.get();
                feed.song          = &g_s->song;
                feed.maxSustainSec = maxSus;
                // Under the SAME lock as the engine swap, because it means
                // the same thing on the hook's side of the boundary: this
                // clock is a new run. AutoPlayBot's note cursor is the one
                // piece of per-run state that lives over there, and it is
                // monotonic - without this the cheat goes silent from the
                // second practice loop on.
                feed.clockGeneration.fetch_add(1, std::memory_order_relaxed);
            }
            // 9: per-run bookkeeping. The drift summary, the 5s window and
            // the mood/crowd edge detectors all describe ONE continuous run;
            // carrying them across a discontinuity would report a fictional
            // spike and attribute a negative interval to the crowd tally
            // (the -1e9 sentinels are what suppress that first sample).
            g_s->settleTicks   = 40;
            g_s->nextLog       = 0.0;
            g_s->nextGapLog    = 0.0;
            g_s->nextMoodFeed  = -1e9;
            g_s->nextMoodSync  = -1e9;
            g_s->lastMoodFeed  = -1e9;
            g_s->prevFrames    = 0;
            g_s->maxAbsDelta   = 0.0;
            g_s->maxDeltaAt    = 0.0;
            g_s->sumDelta      = 0.0;
            g_s->winMin        = 1e9;
            g_s->winMax        = -1e9;
            g_s->samples       = 0;
            g_prevCombo          = 0;
            g_lastCheerMilestone = 0;
            // A seek rebuilds the engine, so the combo restarts at 0. Reset
            // here too or the fire state still believes the old streak, and
            // the ignite that should follow the rebuilt one never fires.
            ResetStreakFire();
            // `refined` is deliberately NOT reset: the countdown
            // re-projection is a one-shot device-startup correction, and
            // ResumeSynced above has just re-anchored the clock onto live
            // audio far more directly than it could.
            if (!live) {
                spdlog::warn(
                    "[practice] seek to {:.2f}s: no live audio callback "
                    "within 500ms - pausing (loop {})",
                    a_songSec, g_s->loopCount);
                PauseSession();
                return false;
            }
            // 10
            feed.engaged.store(true);
            // The start frame prints SIGNED: it is legitimately negative for
            // any range that starts later in the song than the audio engine
            // has been alive, and the unsigned modular form is unreadable.
            spdlog::info(
                "[practice] seek to {:.2f}s (loop {}) - clock @ {:.2f}s, "
                "audio @ {:.2f}s, song 0 @ frame {}, {} notes",
                a_songSec, g_s->loopCount, g_s->clock.SongTime(QpcSec()), pos,
                static_cast<std::int64_t>(X), g_s->song.chart.notes.size());
            return true;
        }

        // Live practice playback speed (P6). Session thread only.
        //
        // Applying it means three things moving together, and all three are
        // required: the AUDIO ratio (through the time-stretch, so pitch is
        // preserved and SetRate is left to the drift corrector), the CLOCK
        // speed (MasterClock scales InputTime and rebases its offset so the
        // song position stays continuous across the change), and the HUD.
        //
        // The restart at the end is what "changing speed resets the practice
        // stats" means: SeekTo rebuilds the engine from the slice, so hit,
        // miss and combo all start clean, and the clock is re-anchored to
        // live audio at the new ratio through one already-tested path
        // instead of a second bespoke one.
        void ApplyPracticeSpeed(int a_delta) {
            if (!g_s || !g_s->practice) { return; }
            const double next =
                bard::practice::StepSpeed(g_s->speed, a_delta);
            if (next == g_s->speed) {
                spdlog::info("[practice] speed already at {:.2f}x", next);
                return;
            }
            g_s->speed = next;
            g_s->audio.SetSongSpeed(next);
            g_s->clock.SetSpeed(QpcSec(), next);
            UiBus::GetSingleton().practiceSpeed.store(
                static_cast<float>(next));
            spdlog::info("[practice] speed -> {:.2f}x (loop restarts, stats "
                         "reset)", next);
            (void)SeekTo(bard::practice::RestartTime(g_s->range));
        }

        // Pause-menu Restart, for both modes. Session thread only.
        void RestartSession() {
            if (!g_s) { return; }
            if (g_s->practice) {
                // Practice restarts its RANGE, which is precisely the loop's
                // own path: engine rebuilt from the slice (so accuracy and
                // combo clear), clock re-anchored to live audio. None of the
                // teardown below is needed or wanted.
                spdlog::info("[practice] restart requested at loop {}",
                             g_s->loopCount);
                g_s->loopCount = 0;
                UiBus::GetSingleton().practiceLoop.store(0);
                (void)SeekTo(bard::practice::RestartTime(g_s->range));
                return;
            }
            // A regular restart is a fresh session on the same pick.
            // Captured BEFORE EndSession, which deletes g_s.
            const auto entry = g_lastPicked;
            const int  diff  = g_lastDifficulty;
            const int  ctx   = g_lastInstrumentContext;
            if (!entry) {
                spdlog::warn(
                    "[session] restart ignored - no remembered pick");
                return;
            }
            // completed=false: an abandoned attempt records NOTHING, exactly
            // like an abort. Restarting must never bank a partial run's
            // stars, gold or expertise.
            EndSession("restarted", false);
            // Pushed AFTER EndSession, and that order is load-bearing:
            // EndSession DRAINS any pending start as stale by definition, so
            // pushing first would silently throw this away and leave the
            // player at the idle screen. The idle loop consumes it next tick
            // through the ordinary browser-pick path.
            UiBus::GetSingleton().PushStart(*entry, diff, ctx);
            spdlog::info("[session] restart: re-queued {}",
                         path_text::Utf8(entry->folder));
        }

        // Pause-menu practice toggle. Restarts the same pick in the OTHER
        // mode rather than converting the live session, because there is no
        // in-place conversion to make: practice runs on a SLICED chart and
        // the engine is built from it, so switching means a new session
        // either way. This reuses the restart path and changes only which
        // start it queues.
        void TogglePracticeMode() {
            if (!g_s) { return; }
            const bool toPractice = !g_s->practice;
            const auto entry = g_lastPicked;
            const int  diff  = g_lastDifficulty;
            const int  ctx   = g_lastInstrumentContext;
            if (!entry) {
                spdlog::warn(
                    "[practice] pause toggle ignored - no remembered pick");
                return;
            }
            // completed=false either way: leaving a normal run part-way to
            // practise it must not bank the partial attempt.
            EndSession(toPractice ? "switching to practice"
                                  : "leaving practice",
                       false);
            if (toPractice) {
                // Straight into the PICKER for this song, rather than
                // starting a whole-song practice run. Choosing WHICH part to
                // drill is the entire point of the mode, and a player who
                // pauses mid-song to practise has a specific passage in mind.
                //
                // Deliberately NOT paired with PostOpenBrowserAfterCameraPrep.
                // That posts its open through a game-thread task which can
                // land a frame or more later, and its Open() would then reset
                // the picker this request had already set up. The picker
                // request opens the window itself, so there is exactly one
                // Open() and no race.
                UiBus::GetSingleton().RequestPracticePicker(*entry, diff, ctx);
            } else {
                UiBus::GetSingleton().PushStart(*entry, diff, ctx);
            }
            spdlog::info("[practice] pause-menu toggle -> {}",
                         toPractice ? "practice picker" : "normal play");
        }

        void TickPlaying() {
            const double raw = QpcSec();
            const auto   a   = g_s->audio.ReadAnchor();
            if (g_s->est->Stale(a, raw)) {
                // diagnostics (field round 2 closed the round-1 mystery:
                // transient 27-38ms gaps from SGT's performance-start hitch,
                // device running - now ridden through below; the pause is
                // for SUSTAINED staleness only): the device state tells a
                // stopped/rerouting device (default-output switch) apart
                // from a starved callback; the notification log in
                // AudioEngine names reroutes explicitly
                const auto  ds = g_s->audio.DeviceState();
                const char* dn = ds == 1   ? "stopped"
                                 : ds == 2 ? "started"
                                 : ds == 3 ? "starting"
                                 : ds == 4 ? "stopping"
                                           : "uninitialized";
                spdlog::warn(
                    "[session] audio anchor stale - pausing (spec 6): "
                    "gap={:.1f}ms limit={:.1f}ms frames={} rate={:.2f} "
                    "dev={} fg={}",
                    (raw - a.qpc) * 1000.0,
                    g_s->est->StaleLimitSec() * 1000.0, a.frames, a.rate,
                    dn, GameHwndForeground());
                PauseSession();
                return;
            }
            // transient callback gap (>2x period but under the pause
            // limit): extrapolation rides through it and the settle logic
            // absorbs the catch-up burst - keep a field eye on frequency
            if (const double agap = raw - a.qpc;
                agap > 2.0 * g_s->est->Period() && raw >= g_s->nextGapLog) {
                g_s->nextGapLog = raw + 5.0;
                spdlog::info(
                    "[session] anchor gap {:.1f}ms (period {:.0f}ms) - "
                    "transient, riding through",
                    agap * 1000.0, g_s->est->Period() * 1000.0);
            }
            const double pos = g_s->est->Position(a, raw);

            const double it = g_s->clock.InputTime(raw);
            // The window is the last second of the START countdown. Anchored
            // to startSongSec rather than to a literal 0 because a practice
            // range's countdown lands on the RANGE, not on song 0;
            // startSongSec is 0.0 for every normal session, so this is the
            // original `it >= -1.0 && it < 0.0` unchanged there.
            if (!g_s->refined && it >= g_s->startSongSec - 1.0 &&
                it < g_s->startSongSec) {
                // the tX projection at ScheduleStart uses a device-startup
                // anchor whose frames<->qpc mapping can be off by 1-2 callback
                // periods (field run: a constant -6.5ms session); by mid-
                // countdown the cadence is steady, so re-anchor once. pos is
                // SONG-domain - valid as InputTime only while audioCal == 0
                // (same M5 caveat as the resume site).
                const double adjust = pos - g_s->clock.SongTime(raw);
                {
                    std::scoped_lock lk(EngineFeed::GetSingleton().mx);
                    g_s->clock.ResumeSynced(raw, pos);
                }
                g_s->refined = true;
                spdlog::info("[session] countdown re-projection: {:+.2f}ms",
                             adjust * 1000.0);
            }

            const double delta = g_s->clock.SongTime(raw) - pos;
            g_s->audio.SetRate(g_s->ctrl.Update(delta));
            // engine feeding happens on the game thread now (InputHook)
            // Whammy audio: publish the engine's whammy-recent state to
            // the vibrato node (atomic write; the audio thread smooths
            // it). Read under the feed lock like every engine read.
            {
                auto& feed = EngineFeed::GetSingleton();
                std::scoped_lock lk(feed.mx);
                if (feed.engine && feed.clock) {
                    g_s->audio.SetWhammy(feed.engine->WhammyRecentIn(
                        feed.clock->InputTime(raw)));
                    // Star Power flanger follows the engine's SP state the
                    // same way (atomic publish; the audio thread ramps the
                    // wet ~100ms both directions, so activation, spend-out
                    // and any abort path all disengage clicklessly).
                    g_s->audio.SetSpFilter(
                        Settings::GetSingleton().spFilter &&
                        feed.engine->Stats().spActive);
                }
            }

            // drift bookkeeping (the M2 exit criterion)
            const double t = g_s->clock.SongTime(raw);
            // a catch-up burst (>2 periods consumed in one gap) momentarily
            // shifts the frames<->qpc mapping; hold the summary stats for
            // ~200ms (field run: one 6.98ms spike after a stale-recovery
            // resume). prevFrames starts 0 so the first tick always settles.
            const auto gap = a.frames - g_s->prevFrames;
            g_s->prevFrames = a.frames;
            if (gap > 960) { g_s->settleTicks = 40; }
            // the summary stats must reflect the SONG under steady cadence
            // only (M2 exit criterion is "over a full song"): skip the lead-in
            // (it < 0) and any post-burst settle window.
            if (g_s->settleTicks == 0 && it >= 0.0) {
                if (std::abs(delta) > g_s->maxAbsDelta) {
                    g_s->maxAbsDelta = std::abs(delta);
                    g_s->maxDeltaAt  = t;
                }
                g_s->sumDelta += delta;
                ++g_s->samples;
            }
            if (g_s->settleTicks > 0) { --g_s->settleTicks; }
            // the 5s window diagnostics roll unconditionally (they surfaced
            // the transient); only the log line is verbose-gated (else a
            // non-verbose run reports one giant window)
            g_s->winMin = std::min(g_s->winMin, delta);
            g_s->winMax = std::max(g_s->winMax, delta);
            if (t >= g_s->nextLog) {
                if (Settings::GetSingleton().verboseLog) {
                    // engine stats are game-thread-written (InputHook) -
                    // copy under the feed lock before reading
                    bard::EngineStats es;
                    {
                        std::scoped_lock lk(EngineFeed::GetSingleton().mx);
                        es = g_s->engine->Stats();
                    }
                    spdlog::info(
                        "[session] t={:.1f}s delta={:+.2f}ms "
                        "win[{:+.2f},{:+.2f}] ctrl={} corr={} | score={} "
                        "combo={} hit={} miss={} over={}",
                        t, delta * 1000.0, g_s->winMin * 1000.0,
                        g_s->winMax * 1000.0,
                        g_s->ctrl.Engaged() ? "ON" : "off",
                        g_s->ctrl.Corrections(), es.score, es.combo,
                        es.notesHit, es.notesMissed, es.overstrums);
                }
                g_s->winMin  = 1e9;
                g_s->winMax  = -1e9;
                g_s->nextLog = t + 5.0;
            }
            // Live Guitar-Hero-style performance health. Deliberately
            // outside the 5s diagnostics block and its verbose gate: the
            // meter consumes judgment deltas and must remain responsive for
            // every player. ~10Hz catches dense-chart counter jumps while
            // keeping the engine-feed lock, also used by InputHook, off this
            // ~200Hz session tick.
            const auto& st = Settings::GetSingleton();
            // ONE 10Hz sampler serves the authoritative Glory/failure model
            // and both optional crowd integrations. It always runs because
            // the HUD and deterministic failure gate are gameplay state;
            // bLiveCrowdMood gates only SGT global writes and
            // bCrowdReactions gates only our own one-shots.
            if (t >= g_s->nextMoodFeed) {
                // engine stats are game-thread-written (InputHook) - copy
                // under the feed lock before reading, as the log site above
                bard::EngineStats mes;
                {
                    std::scoped_lock lk(EngineFeed::GetSingleton().mx);
                    mes = g_s->engine->Stats();
                }
                // Sized to THIS song's note count. A flawless short BA song
                // could not otherwise reach the green zone at all - the
                // meter's span is in notes, so the chart's LENGTH set the
                // ceiling instead of the playing (field 2026-07-26). In
                // practice this is the SLICED chart, which is correct and
                // deliberate: a drilled section should be able to please the
                // room on its own terms.
                const auto mp = difficulty::RockParamsFor(
                    st.tuning, static_cast<int>(g_s->song.chart.notes.size()));
                const auto fp = difficulty::FailureParamsFor(st.tuning);
                crowd::RockSample ms;
                ms.songSec     = t;
                ms.notesHit    = mes.notesHit;
                ms.notesMissed = mes.notesMissed;
                ms.overstrums  = mes.overstrums;
                ms.spActive    = mes.spActive;
                // Tally the interval that just ENDED before feeding,
                // because it belongs to the level the crowd was committed
                // to THROUGH it - Feed may commit a new one on this very
                // call. The payout reads the dominant level out of this,
                // never Committed() at the final note
                // (payout::MoodTally).
                if (g_s->lastMoodFeed > -1e8) {
                    g_moodTally.Add(static_cast<int>(g_mood.Committed()),
                                    t - g_s->lastMoodFeed);
                }
                g_s->lastMoodFeed = t;
                const bool moodChanged = g_mood.Feed(ms, mp);
                const float sentiment =
                    static_cast<float>(g_mood.Sentiment());
                UiBus::GetSingleton().glory.store(sentiment);
                // Short-circuited rather than fed-and-ignored: the gate is
                // STATEFUL - it integrates time spent below the danger line -
                // so running it through a practice session and discarding the
                // answer would leave it primed to fail the player moments
                // into their next real run (rules.allowFailure).
                const bool failed =
                    g_s->rules.allowFailure &&
                    g_failureGate.Feed(t, sentiment, mes.notesMissed,
                                       mes.overstrums, fp);
                UiBus::GetSingleton().gloryDanger.store(
                    g_failureGate.Dangerous());
                // rules.commitGloryGlobals covers BOTH branches below - the
                // corrective Clear() as well as Write() - because both are
                // writes to the SGT globals that drive NPC dialogue. The
                // Clear() at session START is deliberately outside this gate
                // and still runs unconditionally, or a stale Terrible from a
                // previous run would stay pinned through the practice run.
                if (g_s->rules.commitGloryGlobals && st.liveCrowdMood &&
                    (moodChanged || t >= g_s->nextMoodSync)) {
                    const bool commentsAllowed =
                        difficulty::AudienceCommentsAllowed(t, st.tuning);
                    if (!commentsAllowed) {
                        // SGT can set its own pair around three seconds into
                        // startup. Clear repeatedly until the listen-first
                        // interval ends so that write cannot leak an early
                        // line between ordinary mood commits.
                        SKSE::GetTaskInterface()->AddTask(
                            [] { MoodGlobals::Clear(); });
                    } else {
                        if (!g_s->audienceCommentsReleased) {
                            g_s->audienceCommentsReleased = true;
                            spdlog::info(
                                "[mood] normal audience comments enabled at "
                                "{:.2f}s",
                                t);
                        }
                        // Re-sync on a bounded cadence even if the meter
                        // stayed in the same zone, so comments cannot drift
                        // away from the visible red/yellow/green state.
                        const auto lv = g_mood.Committed();
                        SKSE::GetTaskInterface()->AddTask([lv] {
                            const bool corrected = MoodGlobals::Write(lv);
                            if (corrected &&
                                crowd::ReactionFor(lv).releaseCelebration) {
                                SgtVm::ReleaseAudienceCelebration();
                            }
                        });
                    }
                    g_s->nextMoodSync = t + 0.5;
                }
                // Streak fire (GH parity, user ask 2026-07-26): the hands
                // catch light as a streak builds, and the whole performer
                // does past the second tier. Stepped here because this block
                // already has the engine's combo in hand under the feed
                // lock's shadow, and the cadence (~10Hz) is finer than the
                // art's own refresh interval.
                //
                // Practice deliberately included: it is a presentation
                // flourish, not a recorded outcome, and drilling a hard run
                // clean is exactly when a player wants to see it.
                if (streakfire::Enabled(g_streakFireParams)) {
                    // Was it already burning BEFORE this step? That is the
                    // only way to tell an ignition from a refresh, and Step
                    // has already overwritten it by the time it returns.
                    const bool wasLit = g_streakFire.Lit();
                    if (g_streakFire.Step(mes.combo, t, g_streakFireParams) ==
                        streakfire::Action::kApply) {
                        PostStreakCloak(!wasLit);
                    }
                }
                if (failed) {
                    spdlog::info(
                        "[failure] crowd lost: Glory={:.3f}, below {:.3f} "
                        "for {:.1f}s with {} further misses/overstrums",
                        sentiment, fp.dangerBelow, fp.graceSec,
                        fp.furtherBadRequired);
                    // The electric fail sting belongs to THIS moment, not
                    // the completed-song verdict - a FAILED teardown never
                    // reaches that block (field 2026-07-25: "the fail sfx
                    // never plays when i fail a song"; three crowd-lost
                    // events in the log, zero sting lines). The 6s
                    // recording plays out under the boos and teardown on
                    // the bank's own engine.
                    // Gated at the site the rule NAMES, even though practice
                    // cannot reach here today (rules.allowFailure already
                    // makes `failed` false). PracticeLoop.h promises each
                    // field maps to the one site that commits it, and a
                    // reader grepping songEndStings has to find this.
                    if (g_s->rules.songEndStings &&
                        ui_sfx::ElectricSoundContext(
                            g_s->instrumentContext)) {
                        UiSfx::Fire(ui_sfx::Cue::kSongFailElectric);
                    }
                    // Staggered at the moment of the loss, not at teardown:
                    // the animation has to be underway before EndSession
                    // hands input back, or the jump it exists to suppress
                    // has already happened. Practice never reaches here -
                    // rules.allowFailure keeps `failed` false - which is
                    // correct: a practice run cannot fail.
                    PostPlayerStagger(0.5f);
                    EndSession("FAILED: the crowd lost patience", false, true);
                    return;
                }
                // Crowd punctuation, off the SAME stats copy - a second read
                // would mean a second grab of the feed lock the game thread's
                // InputHook also takes.
                //
                // Both tests are EDGE tests against the previous sample, not
                // value tests against the current one, because this site runs
                // at ~10Hz and the combo counter is written by the game
                // thread at note rate. On a dense chart combo can advance
                // several notes between samples, so `combo % N == 0` would
                // miss the milestone outright whenever the sample lands at
                // 24 and then 27, and `combo == 0` would miss the collapse
                // whenever the player has already re-hit a note by the time
                // we look. Crossing a multiple, and dropping at all, are both
                // observable at any sample rate.
                if (st.cheerEveryNotes > 0) {
                    const int milestone =
                        mes.combo / st.cheerEveryNotes * st.cheerEveryNotes;
                    if (milestone > 0 && milestone > g_lastCheerMilestone) {
                        // The milestone bookkeeping stays OUTSIDE the rule
                        // gate on purpose. It is an edge detector against the
                        // previous sample, so skipping the update would leave
                        // it re-detecting the same crossing on every tick.
                        // Only the sound is suppressed.
                        g_lastCheerMilestone = milestone;
                        if (g_s->rules.crowdReactions) {
                            CrowdReactions::Fire(CrowdReactions::Kind::kCheer,
                                                 QpcSec());
                        }
                    }
                }
                // combo only ever falls on a break, so any fall IS one
                if (mes.combo < g_prevCombo) {
                    // guarded like cheerEveryNotes above: at 0 every 1-to-0
                    // break would clear the threshold and groan
                    if (g_s->rules.crowdReactions &&
                        st.streakBreakNotes > 0 &&
                        g_prevCombo >= st.streakBreakNotes) {
                        CrowdReactions::Fire(CrowdReactions::Kind::kGroan,
                                             QpcSec());
                    }
                    // the next streak earns its milestones again from zero
                    g_lastCheerMilestone = 0;
                }
                g_prevCombo       = mes.combo;
                g_s->nextMoodFeed = t + 0.1;  // re-arm, as nextLog above
            }
            // Practice loop (spec 6.3). Tested on the CLOCK time `t`, never
            // on the audio `pos`: `t` is the clock the highway renders from
            // and the one PracticeLoop.h is written against, and after a
            // restart the two are re-synchronised anyway.
            //
            // It sits AHEAD of the end-of-song test below so a range that
            // reaches the end of the chart still loops. It cannot fire twice
            // for one crossing: ResolveRange guarantees endSec >= startSec +
            // kTailPadSec, so the rebased clock (~startSec) is always below
            // the threshold (endSec + restartDelay) on the very next tick.
            if (g_s->practice &&
                bard::practice::ShouldRestart(t, g_s->range.endSec,
                                              g_s->speed)) {
                if (!g_s->loopEnabled) {
                    // Looping off: the range gets ONE pass, then the summary.
                    //
                    // Published as its own tiny payload and NOT through
                    // StageResults. That path is gated on `completed`, and
                    // routing practice through it would walk straight back
                    // through every recording gate P3 built - stars, gold,
                    // expertise, the lot. Practice still records nothing.
                    auto& bus = UiBus::GetSingleton();
                    bard::EngineStats es{};
                    {
                        std::scoped_lock lk(EngineFeed::GetSingleton().mx);
                        if (g_s->engine) { es = g_s->engine->Stats(); }
                    }
                    bus.practiceSummaryHit.store(es.notesHit);
                    bus.practiceSummaryMissed.store(es.notesMissed);
                    bus.practiceSummaryTotal.store(
                        static_cast<int>(g_s->song.chart.notes.size()));
                    bus.practiceSummaryCombo.store(es.maxCombo);
                    bus.practiceSummarySpeed.store(
                        static_cast<float>(g_s->speed));
                    // STAGED, not published: EndSession below decides how
                    // long the dismissed band needs before a panel is
                    // allowed to freeze the world.
                    bus.StagePracticeSummary();
                    spdlog::info(
                        "[practice] range complete (no loop): {}/{} hit, "
                        "{} missed, best combo {}, speed {:.2f}x",
                        es.notesHit, g_s->song.chart.notes.size(),
                        es.notesMissed, es.maxCombo, g_s->speed);
                    EndSession("practice range complete", false);
                    return;
                }
                ++g_s->loopCount;
                UiBus::GetSingleton().practiceLoop.store(g_s->loopCount);
                // Everything computed above this line - pos, delta, the
                // stats copy - describes the run that just ended.
                (void)SeekTo(bard::practice::RestartTime(g_s->range));
                return;
            }
            // End of song. The AUDIO position against stem length, not the
            // clock, so a stalled clock still ends the session.
            //
            // Practice pushes the bar past its own decision point. A range
            // that reaches the end of the chart resolves endSec = songLen +
            // kTailPadSec and ShouldRestart waits restartDelaySec beyond
            // THAT, so the bare songLen + 0.25 test would end the session
            // ~1.75s before any restart could fire - and the whole-song
            // range (the common case, since most charts carry no section
            // markers) would never loop even once. It remains a backstop:
            // strictly later than the loop point, and still bounded.
            double endGuard = g_s->songLen + 0.25;
            if (g_s->practice) {
                const bard::practice::LoopParams lp;
                const double sp = g_s->speed > 0.0 ? g_s->speed : 1.0;
                endGuard = std::max(
                    endGuard, g_s->range.endSec + lp.restartDelaySec * sp +
                                  0.5);
            }
            if (pos >= endGuard) {
                // completed=false for practice, and that ONE argument is the
                // largest part of the no-recording rules. `completed` is what
                // opens the whole Results/ledger/gold block in EndSession, so
                // passing false suppresses, in one move: StarLedger::Record,
                // MarkNew, the results sting cue, StageResults, the
                // applause/awkward/swell trio, ArmDeferred, and with it the
                // deferred SGT expertise feed (BeginPromotion /
                // NoteSessionPayout / FinishPayout are reachable only from
                // the deferPayout branch, which requires completed).
                //
                // That is why rules.recordStars, payGold, feedExpertise and
                // the results half of songEndStings carry no separate test at
                // their own sites. GoldScale::OnSessionEnd still runs and
                // still clears its captured amount - it simply pays nothing
                // when completed is false - which is what keeps a stale
                // capture from a prior real run out of the next one.
                EndSession(g_s->practice ? "practice range ended"
                                         : "song complete",
                           !g_s->practice);
            }
        }

        void SessionThread() {
            const HRESULT hr =
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // own COM, spec 8
            spdlog::info("[session] thread up, com=0x{:X}",
                         static_cast<std::uint32_t>(hr));
            bool   startHeld = false, abortHeld = false, probeHeld = false;
            bool   practiceHeld = false;  // P2 practice debug key
            bool   speedUpHeld = false, speedDownHeld = false;  // P6
            double nextPoll  = 0.0;  // perform-ability presence poll
            double nextBrowseCamera = 0.0;
            double nextPlacement = 0.0;  // world-audio listener/source refresh
            double nextTeachPoll = 0.0;  // bard-lesson expertise edge
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                const auto& st    = Settings::GetSingleton();
                const auto  state = g_state.load();
                const bool startEdge =
                    KeyPressedEdge(st.debugStartKey, startHeld);
                const bool abortEdge =
                    KeyPressedEdge(st.debugAbortKey, abortHeld);
                // Practice field-test entry (P2). Consumed in kIdle only;
                // gated on the INI key being set at all, like iProbeKey.
                const bool practiceEdge =
                    st.debugPracticeKey &&
                    KeyPressedEdge(st.debugPracticeKey, practiceHeld);
                // Live practice speed (P6). Consumed ONLY inside a practice
                // run - the edges are still sampled every tick so the held
                // state cannot go stale and fire on entry to the next one.
                const bool speedDownEdge =
                    st.practiceSpeedDownKey &&
                    KeyPressedEdge(st.practiceSpeedDownKey, speedDownHeld);
                const bool speedUpEdge =
                    st.practiceSpeedUpKey &&
                    KeyPressedEdge(st.practiceSpeedUpKey, speedUpHeld);
                if ((speedDownEdge || speedUpEdge) &&
                    g_state.load() == State::kPlaying && g_s &&
                    g_s->practice && GameHwndForeground()) {
                    ApplyPracticeSpeed(speedUpEdge ? +1 : -1);
                    continue;
                }
                // On-demand probe. The CONTROL this diagnosis has been
                // missing: every stuck reading so far has been compared
                // against nothing, because the session-start probe fires
                // after the hook engages and the post-session ones only fire
                // when the player is already stuck. Held W in free roam must
                // show a non-zero moveInputVec; if it does not, the vector is
                // not how this load order moves the player and the whole
                // routing read is wrong.
                if (st.debugProbeKey &&
                    KeyPressedEdge(st.debugProbeKey, probeHeld)) {
                    SKSE::GetTaskInterface()->AddTask(
                        [] { LogControlProbe(99); });
                }

                GoldScale::TickDeferred(QpcSec());
                SgtProgression::Tick(QpcSec());
                // One gate, two panels: a session stages EITHER Results or
                // the practice summary, never both (practice ends with
                // completed=false, which suppresses the whole Results
                // block), so they share the hold rather than each inventing
                // one.
                if (bard::band::ResultsMayPublish(
                        QpcSec(), g_resultsPublishNotBefore)) {
                    auto& pub = UiBus::GetSingleton();
                    if (pub.TryPublishStagedResults()) {
                        g_resultsPublishNotBefore = 0.0;
                        spdlog::info(
                            "[results] published after input lookup and band "
                            "dismissal quiesced");
                    } else if (pub.TryPublishStagedPracticeSummary()) {
                        g_resultsPublishNotBefore = 0.0;
                        spdlog::info(
                            "[practice] summary published after band "
                            "dismissal quiesced");
                    }
                }

                // Bard teaching (spec 6.3). Sampled on a cadence rather than
                // at the clamp enforcement points, because those only fire at
                // a load or around a cast - a lesson lands while the player
                // is stood in the inn talking to a bard, and the payoff
                // notification has to arrive then, not at the next load. 1s
                // is comfortably inside one Papyrus fragment: the three
                // SetValue calls are consecutive statements.
                //
                // "busy" is only about whether the player could physically
                // be in a bard's dialogue: mid-song, or with the results box
                // still up. It is NOT the discriminator - the all-three rule
                // is - so it costs nothing if it is a little wide.
                if (st.bardTeachingUnlocks && QpcSec() >= nextTeachPoll) {
                    nextTeachPoll   = QpcSec() + 1.0;
                    const bool busy = state != State::kIdle ||
                                      g_pendingPayout.has_value() ||
                                      g_endStripSpell != 0;
                    SKSE::GetTaskInterface()->AddTask(
                        [busy] { TryBardTeaching(busy); });
                }

                // a save load invalidates a payout still waiting on
                // results-close (see g_pendingPayout)
                if (g_dropPendingPayout.exchange(false) && g_pendingPayout) {
                    g_pendingPayout.reset();
                    spdlog::info(
                        "[sgt] held payout dropped (save load - the loaded "
                        "save must not receive phantom XP/gold)");
                }
                if (g_dropFailureFeedback.exchange(false)) {
                    g_failureFeedback.Cancel();
                    g_failureFeedbackActive.store(false);
                    UiBus::GetSingleton().failureInputLockout.store(
                        false, std::memory_order_release);
                    spdlog::info(
                        "[failure] pending feedback teardown dropped "
                        "(save load)");
                }
                const auto failurePlan = g_failureFeedback.Poll(QpcSec());
                if (failurePlan.teardown) {
                    g_failureFeedbackActive.store(false);
                    UiBus::GetSingleton().failureInputLockout.store(
                        false, std::memory_order_release);
                    PostCrowdFailureTeardown(g_failureInstrumentContext,
                                             g_failureToken);
                }
                // delayed end-of-song strip (see g_endStripSpell comment):
                // MessageAndEXP has run by now - vanilla-order cleanup
                if (g_endStripSpell != 0 && QpcSec() >= g_endStripAt) {
                    const auto spell = g_endStripSpell;
                    g_endStripSpell  = 0;
                    SKSE::GetTaskInterface()->AddTask([spell] {
                        auto* pc = RE::PlayerCharacter::GetSingleton();
                        auto* sp =
                            RE::TESForm::LookupByID<RE::SpellItem>(spell);
                        if (pc && sp && pc->HasSpell(sp)) {
                            pc->RemoveSpell(sp);
                            spdlog::info(
                                "[sgt] performance ended (RemoveSpell 3s "
                                "after payout dispatch - MessageAndEXP ran "
                                "on a live effect)");
                            // the completed-song strip is the exact case
                            // where SGT's cleanup latents die (nothing
                            // references the effect) - field 2026-07-20
                            SgtVm::EnablePlayerControlsFallback();
                        }
                        // The deferred-payout path's mood clear; EndSession
                        // skips it so the applause window keeps the level
                        // the player earned. SGT's OnEffectFinish is what
                        // would zero the pair here, and this is the very
                        // case where its cleanup latents die with the
                        // dispelled effect (same field finding as the
                        // control fallback above), so we do it in its
                        // place. Outside the HasSpell guard on purpose: if
                        // SGT beat us to the removal its own cleanup wrote
                        // the same 0/0, so this cannot fight it either way.
                        MoodGlobals::Clear();
                    });
                }
                // Control-recovery retry (field 2026-07-20, and this is a bug
                // in the FIRST version of the recovery). The immediate
                // attempt at session end runs while OUR OWN world pause is
                // still up - the log showed `control recovery: no action
                // (... paused=true)` and the unpause landing in the very
                // next line, so the guard meant to avoid fighting a menu
                // stood down exactly when it was needed.
                //
                // So retry while the world is genuinely unpaused. The
                // recovery is idempotent - it logs "no action" and returns
                // when nothing is missing - so a few extra passes cost
                // nothing and the window closes on its own.
                if (g_recoverUntil > 0.0) {
                    if (QpcSec() >= g_recoverUntil) {
                        g_recoverUntil = 0.0;
                        g_poseReleasePending.store(false);  // window over
                    } else if (!g_uiFreeze && QpcSec() >= g_recoverNext) {
                        g_recoverNext = QpcSec() + 0.5;
                        // This window no longer touches controls at all. It
                        // used to retry EnablePlayerControlsFallback here on
                        // a timer - the commit the abort-path bisect
                        // convicted - and that dispatch has been dead since
                        // iControlRecoveryPasses defaulted to 0. The setting
                        // and the counter went with it on 2026-07-22; every
                        // teardown path calls the fallback directly. What
                        // the window still does is the one-shot pose release
                        // and camera restore below, which disarm themselves.
                        SKSE::GetTaskInterface()->AddTask([] {
                            // pose release: one-shot, and only into a LIVE
                            // graph - an event sent under the pause the
                            // abort menu holds could be dropped, which is
                            // the same trap the recovery gate fell into
                            auto* ui = RE::UI::GetSingleton();
                            if (ui && !ui->GameIsPaused() &&
                                g_poseReleasePending.exchange(false)) {
                                SgtVm::ReleasePerformPose();
                                // and hand the camera back - SGT's own
                                // restore is dead code in our flow (it sits
                                // after the latent its cleanup dies at)
                                if (g_wasFirstPerson.exchange(false)) {
                                    if (auto* cam =
                                            RE::PlayerCamera::GetSingleton()) {
                                        cam->ForceFirstPerson();
                                        spdlog::info(
                                            "[sgt] browse camera: first "
                                            "person restored (session end)");
                                    }
                                }
                            }
                        });
                    }
                }
                // (The always-on ControlMap edge watcher lived here until
                // 2026-07-22. It traced enabledControls/unk11C, which on AE
                // are the input context stack's size and its padding - so
                // every [ctlwatch] line ever captured was reporting a stack
                // depth as a control mask, and three sessions of diagnosis
                // were reasoned off it. Removed rather than repointed: see
                // MovementGuard.h before instrumenting that object again.)
                // post-session control probes (kProbeSchedule after end)
                if (g_probeIdx < kProbeCount &&
                    QpcSec() >= g_probeEndAt + kProbeSchedule[g_probeIdx]) {
                    const int no = g_probeIdx + 1;
                    ++g_probeIdx;
                    SKSE::GetTaskInterface()->AddTask(
                        [no] { LogControlProbe(no); });
                }

                if (state == State::kIdle) {
                    // The failed performer gets a short, voiced humiliation
                    // beat before SGT's stage and spell are torn down. Do not
                    // let a cast or stale browser pick start a new session
                    // during that hold: its delayed teardown would otherwise
                    // stop the new audience and remove the shared ability.
                    if (failurePlan.blockStarts) {
                        g_reqStart.exchange(false);
                        UiBus::GetSingleton().TakePendingStart();
                        continue;
                    }
                    // Browse/results world freeze (design 2026-07-20): the
                    // world pauses while the song browser or the results
                    // box is up. SGT's Papyrus start thread freezes BEFORE
                    // it can play anything (the browse-jitter root fix).
                    // WHAT THE FREEZE ACTUALLY STOPS (12:30 field log, and
                    // it is NOT what the 11:30 read concluded): SKSE tasks
                    // keep running - the 5s keeper logged three passes on
                    // cadence inside an 18.6s freeze. It is the PAPYRUS VM
                    // that stops. So a queued pass executes on time but its
                    // VM-side work (dispatches, and binding the script
                    // object of an effect whose OnEffectStart has not run)
                    // only completes after the unfreeze - which is what the
                    // 11:30 log's "task ran 200ms after unfreeze" really
                    // showed. Rule: pure C++ game-object work is safe under
                    // the freeze, anything VM-side is not.
                    // The freeze deliberately PERSISTS
                    // while kCloseOnGameMenu merely HIDES the window behind
                    // a native menu - unfreezing there would hand SGT's
                    // start thread its race back. Exits that lift it: pick
                    // (explicit, below), browser cancel and results close
                    // (falling edge here), save load (OnPreLoadGame closes
                    // both windows + unfreezes directly).
                    {
                        auto&      bus  = UiBus::GetSingleton();
                        // The practice summary joins the SAME freeze the
                        // browser and results already use, rather than
                        // inventing a post-session pause of its own. This
                        // watcher runs in kIdle, which is exactly where a
                        // finished practice run has just landed, so the
                        // summary gets the world freeze and its lift for
                        // free - and cannot leave the world frozen, because
                        // the same edge that raised it lowers it.
                        // The Songbook's freeze WAITS for the browse
                        // sheathe. A frozen world does not advance an
                        // animation, so freezing on the same frame the
                        // Songbook opens is what left the weapon pinned
                        // mid-sheathe for a whole performance (field
                        // 2026-07-26; the full account is at kSheatheTick).
                        // Bounded twice over - the watch releases
                        // g_weaponsSettled itself when it gives up, and this
                        // deadline is the belt in case that thread never
                        // runs - because an open Songbook over a LIVE world
                        // is the worse failure of the two.
                        const bool browserUp = bus.browserOpen.load();
                        // Forced dialogue (a guard's arrest) outranks the
                        // songbook: never engage the browse freeze under
                        // one, and close the browser - pending OR open -
                        // so the scene plays out in a live world; the
                        // player re-casts afterwards. Without this, the
                        // sheathe-hold window let the dialogue open
                        // mid-transition and the delayed freeze
                        // deadlocked the world (field 2026-07-27).
                        // Dialogue SPECIFICALLY - not the whole native
                        // tracker: the trigger flow's own inventory/
                        // favorites churn tripped a broader guard and
                        // killed every legitimate browse (the same day's
                        // regression).
                        const bool dialogueUp = g_dialogueOpen.load(
                            std::memory_order_acquire);
                        if (dialogueUp &&
                            (browserUp ||
                             bus.browserOpenRequest.load(
                                 std::memory_order_acquire))) {
                            CancelPendingBrowserOpen();
                            if (!bus.browserCloseRequest.exchange(true)) {
                                spdlog::info(
                                    "[browser] forced dialogue during "
                                    "browse - closing the songbook so "
                                    "the scene can play (recovery)");
                            }
                        }
                        const bool sheatheHolding =
                            browserUp &&
                            !g_weaponsSettled.load(
                                std::memory_order_acquire) &&
                            QpcSec() < g_sheatheHoldUntil.load(
                                           std::memory_order_acquire);
                        const bool uiUp =
                            FUCK::GetInterface() &&
                            ((browserUp && !sheatheHolding &&
                              !dialogueUp) ||
                             bus.resultsReady.load() ||
                             bus.practiceSummaryReady.load());
                        if (uiUp != g_uiFreeze) {
                            g_uiFreeze = uiUp;
                            PostWorldPause(uiUp);
                            if (!uiUp) {
                                DispatchPendingPayout("results closed");
                                // pick or cancel? BrowserWindow::Play pushes
                                // the selection BEFORE it closes, so the
                                // pick branch below sees it on this very
                                // iteration and clears this.
                                g_uiClosedEdge = true;
                            }
                        }
                    }
                    if (UiBus::GetSingleton().browserOpen.load()) {
                        if (QpcSec() >= nextBrowseCamera) {
                            nextBrowseCamera = QpcSec() + 0.25;
                            PostKeepThirdPersonForBrowse();
                        }
                    } else {
                        nextBrowseCamera = 0.0;
                    }
                    // primary SGT trigger: poll for the perform ability at
                    // idle (see PollPerformAbility - AddSpell fires no cast
                    // event); 500ms keeps the browser snappy and the task
                    // trivial
                    if (AnySpell() && QpcSec() >= nextPoll) {
                        nextPoll = QpcSec() + 0.5;
                        SKSE::GetTaskInterface()->AddTask(
                            [] { PollPerformAbility(); });
                    }
                    // browse standstill: consume the last pass observation,
                    // then maybe dispatch the next. Driven BEFORE the pick
                    // check so a strip task queued this iteration lands
                    // ahead of the pick's AddSpell task (game-thread FIFO).
                    {
                        const bool wasActive = g_standstill.Active();
                        g_standstill.Observe(
                            static_cast<sgtperform::Standstill::Pass>(
                                g_ssSeen.exchange(-1)));
                        if (wasActive && !g_standstill.Active()) {
                            // only Observe(kAbsent) ends it this way
                            spdlog::info(
                                "[sgt] standstill: effect gone before strip "
                                "(SGT self-exit)");
                        }
                        const auto plan =
                            g_standstill.NextTick(QpcSec(), g_uiFreeze);
                        if (plan.waitLog) {
                            spdlog::info(
                                "[sgt] standstill: SGT clip still not "
                                "started after ~10s - waiting (menu "
                                "holding its start thread?)");
                        }
                        if (plan.run) {
                            const auto spell = g_ssSpell;
                            const bool strip = plan.strip;
                            SKSE::GetTaskInterface()->AddTask([spell, strip] {
                                g_ssSeen.store(
                                    SgtVm::StandstillPass(spell, strip));
                            });
                        }
                    }
                    // Practice picker (P4): the Songbook has asked what
                    // sections this chart carries. Served HERE, on the
                    // session thread, because sections exist only in
                    // ParsedChart - LoadSong parses them off disk, and the
                    // render thread must never touch the filesystem mid-draw.
                    //
                    // Answered even when the parse FAILS, with an empty list.
                    // A picker left waiting forever on a broken chart is a
                    // dead surface the player cannot back out of; an empty
                    // list is the whole-song row, which is the honest answer
                    // and a shape ResolveRange already handles.
                    {
                        bard::SongEntry req;
                        unsigned        gen = 0;
                        if (UiBus::GetSingleton().TakePracticeSectionsRequest(
                                req, gen)) {
                            bard::LoadedSong probe;
                            std::string      err;
                            std::vector<UiBus::PracticeSection> out;
                            double endSec = 0.0;
                            if (bard::LoadSong(req, g_lastDifficulty, probe,
                                               &err)) {
                                out.reserve(probe.chart.sections.size());
                                for (const auto& sec : probe.chart.sections) {
                                    out.push_back({ sec.time, sec.name });
                                }
                                // The end of playable CONTENT, which is what
                                // ResolveRange documents this argument as.
                                // The live session uses the stem length
                                // instead (MaxStemLengthSec), but that needs
                                // an open audio device; this value only ever
                                // feeds the picker's displayed end time, and
                                // StartSession re-resolves the range against
                                // the real stem length before anything plays.
                                for (const auto& n : probe.chart.notes) {
                                    endSec = std::max(endSec, n.time);
                                    for (int l = 0; l < bard::kLaneCount;
                                         ++l) {
                                        endSec = std::max(endSec,
                                                          n.sustainEnd[l]);
                                    }
                                }
                            } else {
                                spdlog::warn(
                                    "[practice] section probe failed for {} "
                                    "- offering whole song ({})",
                                    path_text::Utf8(req.folder), err);
                            }
                            spdlog::info(
                                "[practice] picker: {} sections, content ends "
                                "{:.2f}s ({})",
                                out.size(), endSec,
                                path_text::Utf8(req.folder));
                            UiBus::GetSingleton().PublishPracticeSections(
                                std::move(out), endSec, gen);
                        }
                    }
                    // browser Play (UiBus) wins over a raw start edge
                    if (auto sel = UiBus::GetSingleton().TakePendingStart()) {
                        // a pick CONTINUES the trigger's effect instance
                        // (the freeze held its start thread) - the queued
                        // standstill passes that burst at unfreeze must NOT
                        // blank its idle properties (field 2026-07-20:
                        // invisible lute + end-of-song control lock).
                        // Synchronous atomic: cleared before any unfreeze
                        // task can run.
                        SgtVm::DisarmStandstillBlank();
                        g_uiClosedEdge = false;  // this close was a PICK
                        // lift the browse freeze BEFORE the session spins
                        // up (the render mirror lags a frame; the pick IS
                        // the exit). Game-thread FIFO: this unfreeze lands
                        // ahead of everything StartSession posts, so SGT's
                        // start thread resumes into a live world.
                        if (g_uiFreeze) {
                            g_uiFreeze = false;
                            PostWorldPause(false);
                        }
                        // rare corner: picking with the results box still
                        // up supersedes it - fire the held payout first
                        // (the continuing effect receives it; the new
                        // session's start cancels the strip)
                        DispatchPendingPayout(
                            "song picked with results still up");
                        if (songeligibility::IsBoundContext(
                                sel->instrumentContext)) {
                            // Reassert the frozen initiating instrument before
                            // StartSession snapshots it. Settings/debug state
                            // may have changed while the Songbook was open.
                            StarLedger::GetSingleton().SetActiveInstrument(
                                static_cast<stars::Instrument>(
                                    songeligibility::ProgressionContext(
                                        sel->instrumentContext)));
                        }
                        // A picker request that never got answered (or was
                        // answered for a song the player then changed their
                        // mind about) must not outlive the pick.
                        UiBus::GetSingleton().ClearPracticeSections();
                        PracticeRequest req;
                        req.practice     = sel->practice;
                        req.startSection = sel->startSection;
                        req.endSection   = sel->endSection;
                        req.loopEnabled  = sel->loopEnabled;
                        req.speed        = sel->speed;
                        if (StartSession(&sel->entry, sel->difficulty,
                                         sel->instrumentContext, req)) {
                            g_standstill.Cancel();
                            PostBeginSgtPerformance(
                                g_s->instrumentContext, g_s->instrument,
                                g_s->bandStems);
                            g_state.store(State::kPlaying);
                            PostBeginPerformanceVanity(
                                sel->practice ? "practice start"
                                              : "song start");
                        } else {
                            // failed load: reopen the browser instead of
                            // silent nothing (the error is already logged)
                            PostOpenBrowserAfterCameraPrep("load failure",
                                                          false,
                                                          sel->instrumentContext);
                        }
                        continue;
                    }
                    // Browse CANCEL: the window closed and no pick was
                    // consumed. The settle-then-strip still has to run (an
                    // early RemoveSpell leaves SGT's in-flight start thread
                    // with ghost idle + music), but the player should not
                    // have to stand through it - arm the pass-driven control
                    // release so control comes back sub-second instead of
                    // 3-4s later at the strip.
                    if (g_uiClosedEdge) {
                        g_uiClosedEdge = false;
                        // nothing started, so nothing owns the camera - put
                        // a first-person player straight back
                        PostRestoreFirstPerson("browse cancelled");
                        if (g_standstill.Active()) {
                            spdlog::info(
                                "[sgt] browse cancelled - instant control "
                                "release armed (strip still settles)");
                            SKSE::GetTaskInterface()->AddTask(
                                [] { SgtVm::ArmCancelControlRelease(); });
                        }
                    }
                    // Practice field-test entry (P2, temporary). Starts a
                    // practice run over the WHOLE song - startSection /
                    // endSection stay -1, which ResolveRange reads as "whole
                    // song" - on the last song this run started, or on the
                    // first scanned one if there has not been a session yet.
                    // It exists purely so the loop can be exercised in the
                    // field before P4's section picker; it deliberately
                    // mirrors the legacy direct-start branch below rather
                    // than inventing a second start shape.
                    if (practiceEdge && GameHwndForeground()) {
                        const bard::SongEntry* entry =
                            g_lastPicked ? &*g_lastPicked : nullptr;
                        spdlog::info(
                            "[practice] debug key: whole-song practice on {}",
                            entry ? path_text::Utf8(entry->folder)
                                  : std::string("the first scanned song"));
                        PracticeRequest req;
                        req.practice = true;
                        if (StartSession(entry, g_lastDifficulty,
                                         g_lastInstrumentContext, req)) {
                            g_standstill.Cancel();
                            PostBeginSgtPerformance(
                                g_s->instrumentContext, g_s->instrument,
                                g_s->bandStems);
                            g_state.store(State::kPlaying);
                            PostBeginPerformanceVanity("practice start");
                        }
                        continue;
                    }
                    // start key (foreground-gated) OR a perform-power cast
                    const bool castReq = g_reqStart.exchange(false);
                    if ((startEdge && GameHwndForeground()) || castReq) {
                        spdlog::info(
                            "[session] start request consumed ({}) - {}",
                            castReq ? "perform cast" : "start key",
                            FUCK::GetInterface() ? "opening browser"
                                                 : "legacy direct start");
                        if (!castReq) {
                            // start key = no instrument cast; sessions and
                            // browser display attribute the default lute
                            StarLedger::GetSingleton().SetActiveInstrument(
                                stars::Instrument::kLute);
                        }
                        if (FUCK::GetInterface()) {
                            const int instrumentContext =
                                castReq
                                  ? songeligibility::ContextForTrigger(
                                        g_lastTriggerInst.load())
                                  : songeligibility::kContextFree;
                            // Publish the render-thread open edge only from
                            // the game-thread camera task. For a perform cast,
                            // inventory/Tween must finish closing first; the
                            // start-key path has no such menu transition.
                            PostOpenBrowserAfterCameraPrep(
                                castReq ? "perform cast" : "start key",
                                castReq, instrumentContext);
                            // browse standstill: strip the trigger's SGT
                            // performance once it settles - the player just
                            // stands while browsing; the pick re-adds it
                            if (castReq &&
                                Settings::GetSingleton().sgtNativeStart) {
                                // Native start already stripped the ability
                                // at the hook, before OnEffectStart ran.
                                // There is nothing started, so nothing to
                                // settle and nothing to strip - the whole
                                // standstill machine is skipped.
                                spdlog::info(
                                    "[sgt] native start: no standstill needed "
                                    "(nothing was ever started)");
                            } else if (castReq &&
                                Settings::GetSingleton().wholeSongPerform) {
                                const int inst = g_lastTriggerInst.load();
                                const auto spell =
                                    g_performSpell[inst].load();
                                if (spell != 0) {
                                    g_ssSpell = spell;
                                    g_ssSeen.store(-1);
                                    g_standstill.Begin(QpcSec());
                                    SKSE::GetTaskInterface()->AddTask([] {
                                        SgtVm::ResetStandstillLatch();
                                    });
                                    spdlog::info(
                                        "[sgt] standstill: begun (inst={}) "
                                        "- strips once SGT's start settles",
                                        inst);
                                }
                            }
                        } else if (StartSession(
                                       nullptr,
                                       Settings::GetSingleton().difficulty,
                                       songeligibility::kLute)) {
                            g_standstill.Cancel();
                            PostBeginSgtPerformance(
                                g_s->instrumentContext, g_s->instrument,
                                g_s->bandStems);
                            g_state.store(State::kPlaying);
                            PostBeginPerformanceVanity("debug song start");
                        }
                    }
                    continue;
                }
                // ---- active session ----
                // a perform-power cast mid-session must not queue a phantom
                // start once we leave kIdle - consume and drop it here (the
                // kIdle branch already 'continue's above this point)
                g_reqStart.exchange(false);
                // world audio: keep the listener on the camera and the
                // source on the player. 30Hz is plenty - the player is
                // rooted for the whole performance, so only the camera
                // actually moves, and panning that smooth is inaudible from
                // a faster refresh.
                if (Settings::GetSingleton().worldAudio && g_s) {
                    if (QpcSec() >= nextPlacement) {
                        nextPlacement = QpcSec() + 0.033;
                        PostUpdateWorldAudioPlacement();
                    }
                    if (g_placementValid.load(std::memory_order_acquire)) {
                        g_s->audio.SetListener(
                            g_camPos[0].load(std::memory_order_relaxed),
                            g_camPos[1].load(std::memory_order_relaxed),
                            g_camPos[2].load(std::memory_order_relaxed),
                            g_camFwd[0].load(std::memory_order_relaxed),
                            g_camFwd[1].load(std::memory_order_relaxed),
                            g_camFwd[2].load(std::memory_order_relaxed),
                            g_camUp[0].load(std::memory_order_relaxed),
                            g_camUp[1].load(std::memory_order_relaxed),
                            g_camUp[2].load(std::memory_order_relaxed));
                        g_s->audio.SetSourcePosition(
                            g_srcPos[0].load(std::memory_order_relaxed),
                            g_srcPos[1].load(std::memory_order_relaxed),
                            g_srcPos[2].load(std::memory_order_relaxed));
                    }
                }
                // whole-song SGT keeper (plan 2026-07-19)
                {
                    const auto seen = static_cast<sgtperform::Seen>(
                        g_sgtSeen.exchange(-1));
                    const bool clipStopped =
                        g_sgtClipStopped.exchange(false);
                    g_sgtLogic.Observe(seen, clipStopped);
                    GoldScale::SetWholeSongLive(
                        g_sgtLogic.state() ==
                        sgtperform::Logic::State::kLive);
                    if (g_sgtLogic.Lost()) {
                        if (state == State::kPlaying) {
                            // the user took SGT's manual exit (Activate) -
                            // the performance and the session are one thing
                            // while actually playing
                            EndSession(
                                "aborted: SGT performance ended externally");
                            continue;
                        }
                        // paused/resuming: recoverable (design 2026-07-19).
                        // BardHero now owns all keyboard input in these
                        // states, but an external/scripted loss can still
                        // occur. The resume path re-adds and re-probes.
                        if (!g_sgtPauseDeathLogged) {
                            g_sgtPauseDeathLogged = true;
                            spdlog::info(
                                "[sgt] performance died while paused - "
                                "recoverable (re-AddSpell at resume)");
                        }
                    }
                    const auto plan = g_sgtLogic.NextTick(
                        QpcSec(), state == State::kPlaying,
                        st.sgtIdleKeepAlive);
                    if (plan.run) {
                        // the session's instrument is frozen at start; the
                        // live active instrument can drift via a mid-session
                        // cast (e.g. Magic menu) - probe the frozen one
                        const auto spell =
                            PerformSpellForContext(g_s->instrumentContext);
                        const bool keepIdle = plan.keepIdle;
                        const bool keepFollower =
                            Settings::GetSingleton().followerKeepAlive;
                        const auto gen      = g_sgtGen.load();
                        SKSE::GetTaskInterface()->AddTask([spell, keepIdle,
                                                           keepFollower, gen] {
                            // Gate the ACTION, not just the result. This
                            // guard used to sit only around the store below,
                            // so a pass posted just before a session ended
                            // still ran its side effects afterwards - and
                            // KeeperPass RE-SENDS the lute idle whenever
                            // bIdlePlaying reads false, which is exactly what
                            // releasing the pose makes it. That race re-arms
                            // the very idle we just ended, and nothing would
                            // ever take it off again.
                            if (g_sgtGen.load() != gen) { return; }
                            const auto r = SgtVm::KeeperPass(spell, keepIdle,
                                                            keepFollower);
                            if (g_sgtGen.load() == gen) {
                                // Publish the payload before the tri-state
                                // observation, which is the ready edge the
                                // session thread consumes.
                                g_sgtClipStopped.store(r.clipStopped);
                                g_sgtSeen.store(r.effectPresent ? 1 : 0);
                            }
                        });
                    }
                }
                // abort is deliberately NOT foreground-gated - global
                // emergency-out
                if (g_reqAbort.exchange(false) || abortEdge) {
                    EndSession("aborted");
                    continue;
                }
                // Restart. Sits beside abort because it is reachable from the
                // same places - the pause menu included - and both are
                // session-wide rather than tied to one state.
                if (g_reqRestart.exchange(false)) {
                    // Lift our own pause FIRST. EndSession does this for the
                    // regular path, but the practice path stays inside the
                    // session and would otherwise seek back into a frozen
                    // world with the pause menu still up.
                    if (g_s && g_s->practice) {
                        UiBus::GetSingleton().pauseMenuOpen.store(false);
                        PostWorldPause(false);
                        g_state.store(State::kPlaying);
                    }
                    RestartSession();
                    continue;
                }
                if (g_reqPracticeToggle.exchange(false)) {
                    // Always a full teardown, so the pause lift EndSession
                    // performs covers both directions - no manual unpause
                    // needed here, unlike the practice branch of restart.
                    TogglePracticeMode();
                    continue;
                }
                if (g_s && g_s->startCell != 0) {
                    const auto cell = g_playerCell.load();
                    const auto ws   = g_playerWs.load();
                    const bool interiorNow = g_playerInterior.load();
                    // spec 10 refined (deferred ledger): a load between two
                    // exterior cells of ONE worldspace is a boundary walk,
                    // not a scene change - stay paused instead of aborting
                    const bool wsChanged = ws != g_s->startWs;
                    const bool cellChanged =
                        cell != 0 && cell != g_s->startCell;
                    const bool interiorInvolved =
                        interiorNow || g_s->startInterior;
                    if (wsChanged || (cellChanged && interiorInvolved)) {
                        EndSession("aborted: cell/worldspace change (spec 10)");
                        continue;
                    }
                } else if (g_s && g_s->startCell == 0) {
                    g_s->startCell     = g_playerCell.load();  // late capture
                    g_s->startWs       = g_playerWs.load();
                    g_s->startInterior = g_playerInterior.load();
                }
                switch (state) {
                    case State::kPlaying: {
                        g_reqResume.store(false);  // stale toggle hygiene
                        const HWND hw = g_gameHwnd.load();
                        const bool focusLost =
                            hw && GetForegroundWindow() != hw;
                        if (g_reqPause.exchange(false) || focusLost ||
                            startEdge) {
                            PauseSession();
                        } else {
                            bard::BandStage::Tick(QpcSec());
                            TickPlaying();
                        }
                        break;
                    }
                    case State::kPaused:
                        g_reqPause.store(false);
                        if (startEdge || g_reqResume.exchange(false)) {
                            if (g_inLoadingMenu.load() || g_uiPaused.load() ||
                                !GameHwndForeground()) {
                                // a silently swallowed resume looked like a
                                // dead key in the field (deferred ledger)
                                spdlog::info(
                                    "[session] resume blocked (loading={} "
                                    "uiPaused={} foreground={})",
                                    g_inLoadingMenu.load(), g_uiPaused.load(),
                                    GameHwndForeground());
                            } else {
                                auto& bus = UiBus::GetSingleton();
                                bus.pauseMenuOpen.store(false);
                                PostWorldPause(resume_world::ShouldPause(
                                    resume_world::Phase::kCountdown));
                                const double now = QpcSec();
                                g_s->resumeCountdown.Cancel();
                                g_s->resumeCountdown.Start(now);
                                g_s->resumeAudioStarted = false;
                                bus.resumeCountdownCue.store(
                                    static_cast<int>(
                                        bard::ResumeCountdown::Cue::kThree));
                                bus.resumeCountdownActive.store(true);
                                // Audio, chart clock and rhythm input remain
                                // frozen for the whole 3,2,1,GO gate. Skyrim
                                // remains paused through the live-audio-anchor
                                // wait and resumes on the same boundary as
                                // chart/audio/gameplay below.
                                g_s->audio.SetPaused(true);
                                EngineFeed::GetSingleton().engaged.store(false);
                                MovementGuard::Post(true);
                                g_state.store(State::kResuming);
                                spdlog::info(
                                    "[session] resume countdown started "
                                    "(3,2,1,GO; {:.2f}s)",
                                    bard::ResumeCountdown::kDuration);
                            }
                        }
                        break;
                    case State::kResuming: {
                        const HWND hw = g_gameHwnd.load();
                        const bool focusLost =
                            hw && GetForegroundWindow() != hw;
                        if (g_reqPause.exchange(false) || focusLost ||
                            startEdge) {
                            g_s->resumeCountdown.Cancel();
                            g_s->resumeAudioStarted = false;
                            auto& bus = UiBus::GetSingleton();
                            bus.resumeCountdownActive.store(false);
                            bus.resumeCountdownCue.store(0);
                            g_s->audio.SetPaused(true);
                            EngineFeed::GetSingleton().engaged.store(false);
                            MovementGuard::Post(false);
                            g_state.store(State::kPaused);
                            bus.pauseMenuOpen.store(true);
                            PostWorldPause(true);
                            spdlog::info(
                                "[session] resume countdown cancelled; "
                                "returned to pause");
                            break;
                        }

                        const double countdownNow = QpcSec();
                        const auto cue =
                            g_s->resumeCountdown.CueAt(countdownNow);
                        UiBus::GetSingleton().resumeCountdownCue.store(
                            static_cast<int>(cue));
                        if (!g_s->resumeCountdown.Complete(countdownNow)) {
                            break;
                        }
                        if (!g_s->resumeAudioStarted) {
                            // GO has completed. Start audio first; the clock
                            // remains frozen until the first live callback is
                            // observed and then rebases onto that exact audio
                            // position below.
                            PostWorldPause(resume_world::ShouldPause(
                                resume_world::Phase::kAwaitingAudioAnchor));
                            g_s->audio.SetPaused(false);
                            g_s->resumeAudioStarted = true;
                            g_s->resumeStartedAt = countdownNow;
                            spdlog::info(
                                "[session] resume countdown complete; "
                                "starting audio");
                        }
                        // wait for a live anchor, then rebase the clock ONTO
                        // the audio position - pause cycles cannot accumulate
                        // skew (spec 6 rebase rule)
                        const auto a = g_s->audio.ReadAnchor();
                        if (a.rate > 0.0) {
                            const double raw = QpcSec();
                            const double pos = g_s->est->Position(a, raw);
                            // pos is SONG-domain; valid as an InputTime only
                            // while audioCal == 0 (all of M2). M5: rebase on
                            // pos - audioCal * speed once the calibrator
                            // lands (ledger entry in the M2 plan).
                            {
                                std::scoped_lock lk(
                                    EngineFeed::GetSingleton().mx);
                                g_s->clock.ResumeSynced(raw, pos);
                            }
                            g_s->resumeCountdown.Cancel();
                            g_s->resumeAudioStarted = false;
                            UiBus::GetSingleton().resumeCountdownActive.store(
                                false);
                            UiBus::GetSingleton().resumeCountdownCue.store(0);
                            EngineFeed::GetSingleton().engaged.store(true);
                            PostWorldPause(resume_world::ShouldPause(
                                resume_world::Phase::kPlaying));
                            // The pause menu's close re-showed menu-churn
                            // widgets (TrueHUD); the menu is long gone by
                            // countdown-complete, so re-assert now (field
                            // 2026-07-27).
                            WidgetMuffle::OnSessionResume();
                            // a stale-recovery / resume publishes a device
                            // catch-up burst; hold the summary stats through it
                            g_s->settleTicks = 40;
                            // SGT recovery (design 2026-07-19): re-add if an
                            // external/scripted path killed the performance
                            // while paused. This also covers a death the 5s
                            // keeper never observed (task lands before the
                            // next keeper pass, game-thread FIFO).
                            if (Settings::GetSingleton().wholeSongPerform &&
                                g_sgtLogic.Confirmed()) {
                                const auto inst = g_s->instrument;
                                const auto spell = PerformSpellForContext(
                                    g_s->instrumentContext);
                                if (spell != 0) {
                                    const bool seenDead = g_sgtLogic.Lost();
                                    SKSE::GetTaskInterface()->AddTask(
                                        [spell, inst, seenDead] {
                                        auto* pc = RE::PlayerCharacter::
                                            GetSingleton();
                                        auto* sp = RE::TESForm::LookupByID<
                                            RE::SpellItem>(spell);
                                        if (!pc || !sp ||
                                            pc->HasSpell(sp)) {
                                            return;
                                        }
                                        // new effect = new SongToPlay -
                                        // clean keeper latches first
                                        SgtVm::ResetPerformanceLatches();
                                        g_arming.NoteSelfAdd(
                                            static_cast<int>(inst));
                                        SgtProgression::NoteCast(inst,
                                                                 QpcSec());
                                        GoldScale::NotePerformCast();
                                        {
                                            SelfAddGuard g;  // not ours to strip
                                            pc->AddSpell(sp);
                                        }
                                        spdlog::info(
                                            "[sgt] performance re-added at "
                                            "resume ({})",
                                            seenDead
                                                ? "keeper saw the death"
                                                : "died unseen while "
                                                  "paused");
                                    });
                                    g_sgtLogic.Recover(QpcSec());
                                    g_sgtPauseDeathLogged = false;
                                }
                            }
                            g_state.store(State::kPlaying);
                            PostBeginPerformanceVanity("resume");
                            spdlog::info("[session] resumed @ {:.2f}s", pos);
                        } else if (QpcSec() - g_s->resumeStartedAt > 0.5) {
                            // a dead audio device must not spin the resume
                            // silently forever
                            spdlog::warn(
                                "[session] resume: no live audio callback "
                                "within 500ms - re-pausing");
                            g_s->audio.SetPaused(true);
                            g_s->resumeCountdown.Cancel();
                            g_s->resumeAudioStarted = false;
                            UiBus::GetSingleton().resumeCountdownActive.store(
                                false);
                            UiBus::GetSingleton().resumeCountdownCue.store(0);
                            EngineFeed::GetSingleton().engaged.store(false);
                            MovementGuard::Post(false);
                            g_state.store(State::kPaused);
                            UiBus::GetSingleton().pauseMenuOpen.store(true);
                            PostWorldPause(true);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }

    Session& Session::GetSingleton() {
        static Session instance;
        return instance;
    }

    void Session::RequestPause() { g_reqPause.store(true); }
    void Session::RequestResume() { g_reqResume.store(true); }
    void Session::RequestAbort() { g_reqAbort.store(true); }
    void Session::RequestRestart() { g_reqRestart.store(true); }
    void Session::RequestPracticeToggle() {
        g_reqPracticeToggle.store(true);
    }
    bool Session::NativeMenuOpen() {
        return g_nativeMenuOpen.load(std::memory_order_acquire);
    }

    bool Session::IsPerformSpell(RE::FormID a_id) {
        if (a_id == 0) { return false; }
        for (int i = 0;
             i < songeligibility::kInstrumentContextCount; ++i) {
            if (g_performSpell[i].load() == a_id) { return true; }
        }
        return false;
    }

    bool Session::IsSelfAddInFlight() { return g_selfAdd.load(); }

    void Session::NotePerformAbilityAdded(RE::FormID a_id) {
        if (a_id == 0) { return; }
        for (int i = 0;
             i < songeligibility::kInstrumentContextCount; ++i) {
            if (g_performSpell[i].load() == a_id) {
                if (g_arming.NoteHook(i)) { FirePerformTrigger(i, "hook"); }
                return;
            }
        }
    }

    void Session::Install() {
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
        }

        // Resolve the three native SGT start triggers plus the optional
        // Electric addon trigger. This runs on the game thread at
        // kDataLoaded (forms are loaded), so the RE:: lookup is legal;
        // g_performSpell[] is the only surface the session thread reads.
        auto resolveSpell = [](const std::string& a_spec,
                               const char* a_inst) -> RE::FormID {
            const auto bar = a_spec.find('|');
            if (bar == std::string::npos) { return 0; }
            const std::string plugin  = a_spec.substr(0, bar);
            RE::FormID        localId = 0;
            try {
                localId = static_cast<RE::FormID>(
                    std::stoul(a_spec.substr(bar + 1), nullptr, 16));
            } catch (const std::exception&) {
                localId = 0;
            }
            if (plugin.empty() || localId == 0) { return 0; }
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) { return 0; }
            if (auto* spell = dh->LookupForm<RE::SpellItem>(localId, plugin)) {
                spdlog::info(
                    "[session] perform trigger ({}): {} 0x{:X} -> "
                    "runtime 0x{:X}",
                    a_inst, plugin, localId, spell->GetFormID());
                return spell->GetFormID();
            }
            spdlog::warn(
                "[session] perform trigger ({}): {} 0x{:X} not found "
                "- disabled",
                a_inst, plugin, localId);
            return 0;
        };
        const auto& stg = Settings::GetSingleton();
        g_performSpell[static_cast<int>(stars::Instrument::kLute)].store(
            resolveSpell(stg.performSpell, "lute"));
        g_performSpell[static_cast<int>(stars::Instrument::kFlute)].store(
            resolveSpell(stg.performSpellFlute, "flute"));
        g_performSpell[static_cast<int>(stars::Instrument::kDrum)].store(
            resolveSpell(stg.performSpellDrum, "drum"));
        g_performSpell[songeligibility::kGuitar].store(
            resolveSpell(stg.performSpellGuitar, "guitar"));
        // resolve SGT's expertise GLOBs in the same kDataLoaded context
        SgtProgression::Install();
        ResolveLocTypeInn();  // ...and the payout's venue keyword
        // a stale electric-perform flag must never survive into this
        // process (the OAR player clip keys on it)
        SgtVm::ClearElectricPerformGlobal();

        if (AnySpell()) {
            RE::ScriptEventSourceHolder::GetSingleton()
                ->AddEventSink<RE::TESSpellCastEvent>(
                    SpellSink::GetSingleton());
            if (Settings::GetSingleton().goldScale) { GoldScale::Install(); }
        }

        std::thread(SessionThread).detach();
        spdlog::info(
            "[session] installed (start=VK 0x{:X}, abort=VK 0x{:X}, "
            "practice=VK 0x{:X})",
            Settings::GetSingleton().debugStartKey,
            Settings::GetSingleton().debugAbortKey,
            Settings::GetSingleton().debugPracticeKey);
    }

    void Session::OnPreLoadGame() {
        bard::BandStage::EndNow("pre-load game", true);
        // A director left engaged across a load would steer a session that
        // no longer exists, against a camera the new timeline owns.
        PerformanceCamera::Release();
        // A load can land mid-song, and the cloak is a live temp effect
        // rather than saved state - so put it out before the new timeline
        // inherits a fire nobody lit.
        StopStreakCloak();
        // Same shape: hidden widget menus are live UI state, not saved
        // state, so give them back before the new timeline arrives.
        WidgetMuffle::OnSessionEnd();
        if (g_state.load() != State::kIdle) {
            g_reqAbort.store(true);  // session thread tears down + restores
            // ONLY when a session actually ran, unlike everything else in
            // this function. The rest restores BardHero's OWN state, so it
            // is free to fire unconditionally; this one writes ANOTHER
            // mod's persistent data. kPreLoadGame has no filtering, so
            // unconditionally it would run on every quickload, manual load
            // and load-after-death: a player performing through SGT
            // DIRECTLY, with us not involved at all, who quicksaves mid-song
            // and reloads, gets Good=1 restored with the effect - and our
            // task would then stomp it to 0/0 on the next frame. SGT sets
            // the pair once, ~3s into OnEffectStart, and never re-
            // establishes it, so that would disqualify its crowd lines for
            // the rest of a performance BardHero never touched.
            SKSE::GetTaskInterface()->AddTask([] { MoodGlobals::Clear(); });
        }
        // belt and braces: restore ducking even if teardown races the load
        SKSE::GetTaskInterface()->AddTask(
            [] { Ducking::GetSingleton().Restore(); });
        MovementGuard::Post(false);
        UiBus::GetSingleton().pauseMenuOpen.store(false);
        UiBus::GetSingleton().resumeCountdownActive.store(false);
        UiBus::GetSingleton().resumeCountdownCue.store(0);
        // a load closes the browse/results UI and drops any held payout
        // (world-pause design 2026-07-20); the direct unfreeze below also
        // covers the browse/results freeze (ApplyWorldPause's held latch)
        CancelPendingBrowserOpen();
        UiBus::GetSingleton().browserCloseRequest.store(true);
        UiBus::GetSingleton().CloseResults();
        // The practice summary is the third panel this freeze covers, and it
        // is now STAGED as well as shown - a load must drop both, or one
        // queued inside the band-dismissal hold publishes itself into the
        // freshly loaded game and re-freezes the world there.
        UiBus::GetSingleton().ClearPracticeSummary();
        g_dropPendingPayout.store(true);
        // Invalidate even a teardown task already queued by the session
        // thread. kPreLoadGame runs on the game thread, so an old-timeline
        // task must be able to recognize the new timeline when it executes.
        g_failureEpoch.fetch_add(1);
        UiBus::GetSingleton().failureInputLockout.store(
            false, std::memory_order_release);
        if (g_failureFeedbackActive.exchange(false)) {
            g_dropFailureFeedback.store(true);
            SKSE::GetTaskInterface()->AddTask(
                [] { MoodGlobals::Clear(); });
        }
        // ...and a bard lesson still waiting on the library belongs to the
        // abandoned timeline. kPreLoadGame runs on the game thread, which
        // owns this flag, so clear it directly.
        g_teachPending = false;
        PostWorldPause(false);
    }

    void Session::OnPostLoadGame() {
        bard::BandStage::EndNow("post-load game", true);
        // kNewGame routes here WITHOUT passing through OnPreLoadGame (see
        // plugin.cpp), so a lesson held over quit-to-menu would otherwise
        // land on the new character. Game thread, same as OnPreLoadGame.
        CancelPendingBrowserOpen();
        g_teachPending = false;
        // the electric-perform global rides SAVES: a save written
        // mid-electric-perform restores as 1.0 and would leak the
        // electric body clip into lute performances on that timeline
        SgtVm::ClearElectricPerformGlobal();
    }
}
