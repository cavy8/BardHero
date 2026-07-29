// src/game/InputHook.cpp
#include "PCH.h"
#include "game/InputHook.h"

#include "QpcClock.h"
#include "Settings.h"
#include "audio/AudioEngine.h"
#include "game/AutoPlayBot.h"
#include "game/DifficultyTuning.h"
#include "game/EngineFeed.h"
#include "game/InputMapper.h"
#include "game/PauseInputLogic.h"
#include "game/SessionInputLogic.h"
#include "game/Session.h"
#include "game/UiBus.h"

#include "chart/LoadSong.h"  // LoadedSong::chart (autoplay bot feed)
#include "clock/MasterClock.h"
#include "engine/GuitarEngine.h"

#include "RE/C/Console.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h (house ordering)

#include "FUCK_API.h"   // IsMenuOpen: an open FLICK menu releases the keyboard

#include <Windows.h>

#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace SH {
    namespace {
        // Research offsets into BSWin32KeyboardDevice (CommonLib names them
        // unk070/unk078): +0x78 = DIDEVICEOBJECTDATA[10], stride 24.
        // M0-verified live on 1.6.1170; layout identical on 1.5.97.
        constexpr std::uintptr_t kDiBufferOfs = 0x78;
        constexpr int            kDiBufferLen = 10;
        // Never swallowed, even if a user binds them (spec 7).
        constexpr std::uint32_t kDikConsole     = 0x29;  // grave/tilde
        constexpr std::uint32_t kDikPrintScreen = 0xB7;  // screenshot
        // Tab opens the TweenMenu. It is not a session bind, so it fell
        // through to the game and the player could open the menu mid-song
        // (field 2026-07-20). Swallowed while a session is engaged; the
        // pause key is still the way out, and this does not touch the
        // paused or browse states where the menu is legitimate.
        constexpr std::uint32_t kDikTweenMenu = 0x0F;
        // Browse-time navigation extras (not session binds, swallowed and
        // harvested only while the browser is open).
        constexpr std::uint32_t kDikEnter    = 0x1C;
        constexpr std::uint32_t kDikNumEnter = 0x9C;
        // Songbook difficulty (2026-07-26). DIK arrow codes, not VK.
        constexpr std::uint32_t kDikLeft     = 0xCB;
        constexpr std::uint32_t kDikRight    = 0xCD;

        Binds                    g_binds;   // set once at Install
        GamepadBinds             g_padBinds;  // set once at Install
        bool                     g_controllerEnabled = true;
        bool                     g_gamepadMode       = true;
        // Keyboard twin of gamepad mode: a fret press strums by itself.
        // Refreshed by RefreshBinds so the settings toggle is live.
        bool                     g_fretsOnly         = false;
        // The chord-join window for both mappers' auto-strum, and it must
        // EQUAL the engine's live strum leniency: shorter re-strums into a
        // pending strum (immediate overstrum), longer suppresses when
        // nothing is pending. RefreshBinds computes it through the same
        // difficulty::EngineParamsFor the session uses, so the two windows
        // cannot drift.
        double                   g_strumGraceSec     = 0.050;
        // Isolation kill-switch, set once at Install from
        // Settings::hookNeverFilter. When true, DispatchHook::thunk short-
        // circuits to a pure pass-through and never touches the engine's
        // intrusive InputEvent list (2026-07-20 walk-lock isolation; NOT a
        // ship path - default 0).
        bool                     g_neverFilter = false;  // set once at Install
        int                      g_hookMode    = 0;      // set once at Install

        // Bindings-tab capture + refresh (2026-07-27). g_captureMode holds
        // an InputHook::BindDevice as int; g_captureResult the grabbed
        // code, -1 = none. The thunk consumes g_bindsDirty at frame start,
        // so bind state stays hook-thread-owned after install.
        std::atomic<int>          g_captureMode{ 0 };
        std::atomic<int>          g_captureResult{ -1 };
        std::atomic<std::int64_t> g_captureArmedMs{ 0 };  // steady ms
        std::atomic<bool>         g_bindsDirty{ false };
        constexpr std::int64_t    kCaptureExpiryMs = 5000;

        [[nodiscard]] std::int64_t SteadyMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now()
                           .time_since_epoch())
                .count();
        }

        void LoadBindsFromSettings();  // defined with Install() below

        // Movement keys to unlink while a session is engaged, as a 256-bit
        // map over the DIK space. Written on the game thread by
        // MovementGuard, read on the input thread; per-word atomics, so a
        // publish can never be seen half-applied within a word.
        std::atomic<std::uint64_t> g_moveSwallow[4]{};

        bool MoveSwallowed(std::uint32_t dik) {
            if (dik > 0xFF) { return false; }
            return ((g_moveSwallow[dik >> 6].load(
                         std::memory_order_relaxed) >>
                     (dik & 63)) &
                    1u) != 0;
        }
        InputMapper              g_mapper;  // game thread only
        KeyboardSpFallback       g_keyboardSpFallback;  // game thread only
        GamepadMapper            g_padMapper;  // game thread only
        std::vector<MappedEvent> g_events;  // game thread only, reused
        // Intrusive-link slots rewired by the current dispatch, with the
        // value each held before we touched it. Restored the moment the game
        // has consumed the filtered list, so no engine-owned event object
        // outlives this call in a modified state. Game thread only, reused.
        struct LinkPatch {
            RE::InputEvent** slot;
            RE::InputEvent*  orig;
        };
        std::vector<LinkPatch> g_patches;
        AutoPlayBot              g_bot;     // game thread only (cheat)
        std::vector<bard::NoteInput> g_botEvents;  // game thread, reused
        // Last EngineFeed::clockGeneration the bot was seeded against. A
        // practice loop restart rebases the clock without ending the
        // session, and the bot's cursor is monotonic, so it needs its own
        // re-seed edge (see EngineFeed::clockGeneration).
        std::uint64_t g_botGeneration = 0;
        bool g_wasActive  = false;          // mapper-reset edge
        bool g_wasFeeding = false;          // engage-diff edge
        // Last clockGeneration the MAPPERS were reconciled against. The
        // bot is not the only per-run state on this side of the boundary:
        // a practice-loop SeekTo rebuilds the engine (fret mask zero)
        // inside one session-thread pass, and whether this hook ever SEES
        // feed.engaged go false across that gap is a race it usually
        // loses - the whole seek takes well under a second. Lose it and no
        // engage edge fires; win it and the engage diff still emits
        // nothing, because the mapper diffs against what it told the OLD
        // engine. Either way a fret held across the wrap is dead until
        // physically lifted and re-pressed, with the strike-line pad lit
        // over notes that refuse to register (field 2026-07-28: "you have
        // to lift your finger off the note for it to register", "the red
        // pad color got stuck"). The generation edge catches both: forget
        // the engine state, then diff.
        std::uint64_t g_mapperGeneration = 0;
        long g_prevMissish = 0;  // notesMissed + overstrums at last frame
        int  g_prevHits    = 0;

        // Hook-side probe counters (see InputHook.h). Written on the game
        // thread from the dispatch thunk, read from the post-session probe
        // task on the same thread; relaxed atomics are for tearing safety
        // only, no ordering is implied or needed.
        std::atomic<unsigned long> g_passthroughFrames{ 0 };
        std::atomic<unsigned long> g_filteredFrames{ 0 };
        // 0 none / 1 browser / 2 results / 3 session swallow / 4 paused
        std::atomic<int> g_lastCapture{ -1 };

        bool ConsoleOpen() {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(RE::Console::MENU_NAME);
        }

        // Remaining events to log on the pass-through path; see
        // InputHook::ArmPassthroughTrace.
        std::atomic<int> g_traceBudget{ 0 };

        // Read-only walk of the same intrusive list the filter path already
        // traverses, so it is no more hazardous than the existing code.
        void TracePassthrough(RE::InputEvent* const* a_events) {
            if (g_traceBudget.load(std::memory_order_relaxed) <= 0 ||
                !a_events) {
                return;
            }
            for (RE::InputEvent* e = *a_events; e; e = e->next) {
                if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) {
                    continue;
                }
                const auto* be = static_cast<const RE::ButtonEvent*>(e);
                // EDGES ONLY. A held key emits one event per FRAME, so the
                // first trace burned its whole 60-event budget on two presses
                // (Enter held 0.12s, E held 0.13s) and never got to the keys
                // that matter. IsDown/IsUp are true only on the transition
                // frame, so the budget now buys distinct presses instead of
                // frames of one press.
                if (!be->IsDown() && !be->IsUp()) { continue; }
                if (g_traceBudget.fetch_sub(1, std::memory_order_relaxed) <=
                    0) {
                    return;
                }
                spdlog::info(
                    "[probe] passthru event: dev={} dik={:#04x} {} held={:.2f}",
                    static_cast<int>(e->GetDevice()), be->GetIDCode(),
                    be->IsDown() ? "DOWN" : "UP", be->HeldDuration());
            }
        }

        // Browse-time navigation harvest (field round 4): FLICK's ImGui
        // never sees swallowed or GlovePIE-injected keys, so the intents
        // are read HERE, from the events being swallowed, and posted to
        // UiBus for the windows. gh3.PIE reality: strum bar = Up/Down
        // arrows, Plus = Enter, frets = A..L. Difficulty is now picked HERE
        // too (2026-07-26, GH convention: difficulty belongs to song
        // select), on the yellow/blue frets and the Left/Right arrows - it
        // is still mirrored into the FLICK Settings value, so the two
        // surfaces always agree.
        void HarvestBrowserNav(std::uint32_t dik) {
            auto& bus = UiBus::GetSingleton();
            if (dik == static_cast<std::uint32_t>(g_binds.strum2)) {
                bus.navMove.fetch_add(-1);
            } else if (dik == static_cast<std::uint32_t>(g_binds.strum3)) {
                bus.navMove.fetch_add(1);
            } else if (dik == static_cast<std::uint32_t>(g_binds.fret[0]) ||
                       dik ==
                           static_cast<std::uint32_t>(g_binds.fret2[0]) ||
                       dik == kDikEnter || dik == kDikNumEnter) {
                bus.navConfirm.store(true);
            } else if (dik == static_cast<std::uint32_t>(g_binds.fret[1]) ||
                       dik ==
                           static_cast<std::uint32_t>(g_binds.fret2[1]) ||
                       dik == static_cast<std::uint32_t>(g_binds.pause) ||
                       dik == static_cast<std::uint32_t>(g_binds.pause2)) {
                // red fret = back (CH convention), Esc = close
                bus.navClose.store(true);
            } else if (dik == static_cast<std::uint32_t>(g_binds.fret[2]) ||
                       dik ==
                           static_cast<std::uint32_t>(g_binds.fret2[2]) ||
                       dik == kDikLeft) {
                bus.navDiff.fetch_add(-1);  // yellow fret / Left = easier
            } else if (dik == static_cast<std::uint32_t>(g_binds.fret[3]) ||
                       dik ==
                           static_cast<std::uint32_t>(g_binds.fret2[3]) ||
                       dik == kDikRight) {
                bus.navDiff.fetch_add(1);   // blue fret / Right = harder
            } else if (dik == static_cast<std::uint32_t>(g_binds.fret[4]) ||
                       dik ==
                           static_cast<std::uint32_t>(g_binds.fret2[4])) {
                // orange fret = toggle practice arming. The only fret this
                // function had not already spent, which is why practice got
                // it rather than a new binding the player has to discover.
                bus.navToggle.store(true);
            }
        }

        std::uint32_t GamepadCode(const RE::ButtonEvent* event) {
            if (!event) { return 282; }
            auto mask = event->GetIDCode();
            const auto* controls = RE::ControlMap::GetSingleton();
            if (controls &&
                controls->gamePadMapType == RE::PC_GAMEPAD_TYPE::kOrbis) {
                // Skyrim reports Orbis face/directional buttons in Sony's
                // mask domain. Convert to the XInput positions used by the
                // normalized 266..281 macro-code table. Trigger IDs remain
                // the game-defined 0x9/0xA on both controller types.
                switch (mask) {
                    case 0x00000010: mask = 0x0001; break;  // up
                    case 0x00000040: mask = 0x0002; break;  // down
                    case 0x00000080: mask = 0x0004; break;  // left
                    case 0x00000020: mask = 0x0008; break;  // right
                    case 0x00000008: mask = 0x0010; break;  // options
                    case 0x00100000: mask = 0x0020; break;  // touch pad
                    case 0x00000002: mask = 0x0040; break;  // L3
                    case 0x00000004: mask = 0x0080; break;  // R3
                    case 0x00000400: mask = 0x0100; break;  // L1
                    case 0x00000800: mask = 0x0200; break;  // R1
                    case 0x00004000: mask = 0x1000; break;  // cross
                    case 0x00002000: mask = 0x2000; break;  // circle
                    case 0x00008000: mask = 0x4000; break;  // square
                    case 0x00001000: mask = 0x8000; break;  // triangle
                    default: break;
                }
            }
            switch (mask) {
                case 0x0001: return 266;  // D-pad up
                case 0x0002: return 267;  // D-pad down
                case 0x0004: return 268;  // D-pad left
                case 0x0008: return 269;  // D-pad right
                case 0x0010: return 270;  // Start / Options
                case 0x0020: return 271;  // Back / touch pad
                case 0x0040: return 272;  // L3
                case 0x0080: return 273;  // R3
                case 0x0100: return 274;  // LB / L1
                case 0x0200: return 275;  // RB / R1
                case 0x1000: return 276;  // A / Cross
                case 0x2000: return 277;  // B / Circle
                case 0x4000: return 278;  // X / Square
                case 0x8000: return 279;  // Y / Triangle
                case 0x0009: return 280;  // LT / L2
                case 0x000A: return 281;  // RT / R2
                default: return 282;       // invalid / unbound
            }
        }

        void PublishNavHold(pause_input::Action a_action,
                            const RE::ButtonEvent* a_event) {
            if (!a_event) { return; }
            const int direction =
                a_action == pause_input::Action::kMoveUp   ? -1
              : a_action == pause_input::Action::kMoveDown ? 1
                                                           : 0;
            if (direction == 0) { return; }

            auto& held = UiBus::GetSingleton().navHeldMove;
            if (a_event->IsUp()) {
                int expected = direction;
                held.compare_exchange_strong(expected, 0);
            } else {
                // CommonLib supplies a ButtonEvent every frame while held.
                // IsDown is only the edge, so preserve physical state here
                // and let the render surface apply its repeat cadence.
                held.store(direction);
            }
        }

        session_input::GamepadKeys PadNavigationKeys() {
            session_input::GamepadKeys keys;
            keys.moveUp   = static_cast<std::uint32_t>(g_padBinds.strum[0]);
            keys.moveDown = static_cast<std::uint32_t>(g_padBinds.strum[1]);
            keys.confirm  = static_cast<std::uint32_t>(g_padBinds.confirm);
            keys.cancel   = static_cast<std::uint32_t>(g_padBinds.cancel);
            keys.pause    = static_cast<std::uint32_t>(g_padBinds.pause);
            return keys;
        }

        // Once per frame while a session exists: DI read -> mapper ->
        // engine, all under the feed lock. Game thread; Session's g_s stays
        // session-thread-only - everything crosses via EngineFeed.
        void FeedEngine(EngineFeed& feed,
                        RE::InputEvent* const* a_events) {
            auto* mgr = RE::BSInputDeviceManager::GetSingleton();
            auto* kb  = mgr ? mgr->GetKeyboard() : nullptr;
            if (!kb) return;
            if (!g_wasActive) {  // new session: fresh seed (M0 rule)
                g_mapper.Reset();
                g_keyboardSpFallback.Reset();
                g_padMapper.Reset();
                g_bot.Reset();
                g_botGeneration = feed.clockGeneration.load(
                    std::memory_order_acquire);
                g_wasActive  = true;
                g_wasFeeding = false;
                g_prevMissish = 0;
                g_prevHits    = 0;
            }
            const auto* buf = reinterpret_cast<const DiEvent*>(
                reinterpret_cast<std::uintptr_t>(kb) + kDiBufferOfs);
            const std::uint32_t tgtNow = timeGetTime();
            const double        qpcNow = QpcSec();

            std::scoped_lock lk(feed.mx);
            if (!feed.engine || !feed.clock) return;  // teardown race
            // feed only while the clock runs: during kResuming the keys are
            // already routed away but a strum must not land at frozen time
            const bool feeding =
                feed.engaged.load(std::memory_order_relaxed) &&
                !feed.clock->Paused();
            g_events.clear();
            const auto st = g_mapper.Feed(buf, kDiBufferLen, tgtNow, qpcNow,
                                          g_binds, feeding, g_fretsOnly,
                                          g_strumGraceSec, g_events);
            if (a_events) {
                for (auto* e = *a_events; e; e = e->next) {
                    if (e->GetDevice() != RE::INPUT_DEVICE::kKeyboard ||
                        e->GetEventType() !=
                            RE::INPUT_EVENT_TYPE::kButton) {
                        continue;
                    }
                    const auto* be =
                        static_cast<const RE::ButtonEvent*>(e);
                    if (!be->IsDown() && !be->IsUp()) { continue; }
                    g_keyboardSpFallback.FeedButton(
                        be->GetIDCode(), be->IsDown(), qpcNow, g_binds,
                        feeding, g_events);
                }
            }
            if (g_controllerEnabled && a_events) {
                for (auto* e = *a_events; e; e = e->next) {
                    if (e->GetDevice() != RE::INPUT_DEVICE::kGamepad ||
                        e->GetEventType() !=
                            RE::INPUT_EVENT_TYPE::kButton) {
                        continue;
                    }
                    const auto* be =
                        static_cast<const RE::ButtonEvent*>(e);
                    if (!be->IsDown() && !be->IsUp()) { continue; }
                    g_padMapper.FeedButton(
                        GamepadCode(be), be->IsDown(), qpcNow, g_padBinds,
                        feeding, g_gamepadMode, g_events);
                }
            }
            const std::uint64_t mapperGen =
                feed.clockGeneration.load(std::memory_order_relaxed);
            const bool engineRebuilt = mapperGen != g_mapperGeneration;
            g_mapperGeneration = mapperGen;
            if (engineRebuilt) {
                // A rebuilt engine has been told nothing, whatever the
                // mappers remember telling the old one (see the comment at
                // g_mapperGeneration). Forget unconditionally - if the
                // session is not feeding right now, the next engage edge
                // performs the same diff and re-emits then.
                g_mapper.ForgetEngineState();
                g_keyboardSpFallback.ForgetEngineState();
                if (g_controllerEnabled) {
                    g_padMapper.ForgetEngineState();
                }
            }
            if (feeding && (!g_wasFeeding || engineRebuilt)) {
                // resume reconciliation - after Feed, so the diff (stamped
                // now) follows this frame's real events monotonically
                g_mapper.EmitEngageDiff(qpcNow, g_binds, g_events);
                g_keyboardSpFallback.EmitEngageDiff(
                    qpcNow, g_binds, g_events);
                if (g_controllerEnabled) {
                    g_padMapper.EmitEngageDiff(qpcNow, g_padBinds,
                                               g_events);
                }
                if (engineRebuilt) {
                    spdlog::info(
                        "[input] mapper reconciled for clock generation {} "
                        "(held frets 0x{:02X})",
                        mapperGen, g_mapper.HeldFretMask());
                }
            }
            if (g_controllerEnabled) {
                g_padMapper.EndFrame(qpcNow, g_padBinds, feeding,
                                     g_gamepadMode, g_events,
                                     g_strumGraceSec);
            }
            g_wasFeeding = feeding;
            feed.heldFrets.store(
                static_cast<std::uint8_t>(
                    g_mapper.HeldFretMask() |
                    (g_controllerEnabled ? g_padMapper.HeldFretMask() : 0)),
                                 std::memory_order_relaxed);

            const bool verbose  = Settings::GetSingleton().verboseLog;
            // Autoplay cheat (settings page 2026-07-20): the bot owns the
            // engine while on - real events still pump the mapper (held
            // state + pause detection stay live) but never reach the
            // engine. Known corner: toggling OFF mid-song leaves the
            // engine's fret state the bot's until the next engage diff
            // (pause/resume) reconciles it.
            const bool autoplay = Settings::GetSingleton().autoPlay;
            for (const auto& ev : g_events) {
                if (ev.action == bard::InputAction::kPause && ev.value) {
                    Session::RequestPause();
                }
                if (autoplay) { continue; }
                const double ageMs = (qpcNow - ev.qpcSec) * 1000.0;
                feed.counters.fed += 1;
                feed.counters.sumAgeMs += ageMs;
                feed.counters.maxAgeMs =
                    std::max(feed.counters.maxAgeMs, ageMs);
                feed.engine->Queue({ feed.clock->InputTime(ev.qpcSec),
                                     ev.action, ev.value });
                if (verbose) {
                    spdlog::info("[input] dik=0x{:02X} act={} v={} "
                                 "ageMs={:.1f}",
                                 ev.dik, static_cast<int>(ev.action),
                                 ev.value, ageMs);
                }
            }
            feed.counters.stale += st.stale;
            if (st.bufferFull) {
                feed.counters.bufferFull += 1;
                spdlog::warn(
                    "[input] DI buffer full (10 events in one frame)");
            }
            if (autoplay && feeding && feed.song) {
                // A practice loop restart rebases the clock WITHOUT ending
                // the session, so the new-session seed above never fires for
                // it - and the bot's note cursor is MONOTONIC, so from the
                // second loop on it sits past the end of the range and emits
                // nothing at all (field 2026-07-26). Re-seed on the
                // generation edge. Read inside the feed lock, which is what
                // SeekTo bumps it under, so the cursor can never be reset
                // one frame out of step with the rebuilt engine.
                const std::uint64_t gen = feed.clockGeneration.load(
                    std::memory_order_relaxed);
                if (gen != g_botGeneration) {
                    g_botGeneration = gen;
                    g_bot.Reset();
                    spdlog::info(
                        "[autoplay] bot re-seeded for clock generation {}",
                        gen);
                }
                // perfect inputs stamped at exact note times (see
                // AutoPlayBot.h); queued before Update so this frame
                // judges them at those stamps
                const double nowIn = feed.clock->InputTime(qpcNow);
                g_botEvents.clear();
                g_bot.Emit(feed.song->chart, nowIn, g_botEvents);
                for (const auto& bev : g_botEvents) {
                    feed.engine->Queue(bev);
                }
                // fire SP the moment the half-bar activation gate opens
                if (!feed.engine->Stats().spActive &&
                    feed.engine->SpGaugeFraction(nowIn) >= 0.5) {
                    feed.engine->Queue(
                        { nowIn, bard::InputAction::kStarPower, 1 });
                }
                feed.heldFrets.store(g_bot.HeldMask(),
                                     std::memory_order_relaxed);
            }
            if (feeding) {
                feed.engine->Update(feed.clock->InputTime(qpcNow));
            }
            if (feeding && feed.audio) {
                const auto& es = feed.engine->Stats();
                // unscoredStrums included so a strum with nothing to hit
                // still SOUNDS like a miss in the lead-in and the outro,
                // where scoring stays silent by design (field
                // 2026-07-25: a silent strum reads as dropped input).
                // Presentation only - the stem duck below keys on the
                // same edge, which is correct: a dud strum should duck
                // the guitar exactly like any other miss.
                const long missish =
                    es.notesMissed + es.overstrums + es.unscoredStrums;
                const bool newMiss = missish > g_prevMissish;
                const auto& set     = Settings::GetSingleton();
                if (feed.guitarStem >= 0 && set.missMutesGuitar) {
                    if (es.notesHit > g_prevHits) {
                        // 1.0 = fader fully OPEN, which is not the same thing
                        // as "full volume". miniaudio's fader and the sound's
                        // own volume are two independent multiplicative
                        // stages of ma_engine_node (fader at step 2, volume
                        // at step 4), so fading back to SongVolume() while
                        // ma_sound_set_volume already holds SongVolume()
                        // squares it: 0.45 * 0.45 = 0.20, a 14 dB hole. The
                        // song level lives on the sound, and the settings
                        // slider still applies live through SetSongVolume.
                        feed.audio->FadeStem(feed.guitarStem, 1.0f, 40);
                    }
                    if (newMiss) {  // miss wins a same-frame tie
                        feed.audio->FadeStem(feed.guitarStem, 0.0f, 40);
                    }
                }
                if (newMiss && set.missSfx) { feed.audio->PlayMissSfx(); }
                g_prevMissish = missish;
                g_prevHits    = es.notesHit;
            }
        }

        // write_call<5> at the canonical dispatch site inside
        // BSInputDeviceManager::PollInputDevices (same site FLICK + Fitting
        // Room chain through). While engaged, bound keys are UNLINKED from
        // the intrusive InputEvent list before forwarding - the objects
        // passed down are always engine-owned events (chained-hook rule).
        // The unlink is a LOAN, not an edit: every rewired next pointer is
        // put back the instant the game has consumed the filtered list, so
        // no engine event object is left modified past this call. Keeping
        // the rewire was the post-session input lock (see the restore loop).
        // Print Screen always passes. Console is owned for every BardHero
        // surface, but an already-open console still triggers the recovery
        // pass-through above the filter loop.
        struct DispatchHook {
            static void thunk(
                RE::BSTEventSource<RE::InputEvent*>* a_dispatcher,
                RE::InputEvent* const*               a_events) {
                auto& uiBus = UiBus::GetSingleton();
                if (!uiBus.TryEnterFlickInput()) {
                    // Results is crossing its first host draw. Do not enter
                    // FUCK's unsynchronised WindowState lookup during the
                    // render-thread mutation. EngineFeed is already inactive
                    // before this gate can arm.
                    return;
                }
                struct FlickInputLease {
                    UiBus& bus;
                    ~FlickInputLease() { bus.LeaveFlickInput(); }
                } flickInputLease{ uiBus };
                // AN OPEN FLICK MENU IS A MENU, so the session stops owning
                // the keyboard while it is up.
                //
                // Playing phase otherwise swallows the ENTIRE keyboard on
                // purpose (see session_input::ForKeyboard) so no game hotkey
                // can fire mid-song. But FLICK chains the very same dispatch
                // site we do, so that also ate FLICK's own toggle key: the
                // settings page was unreachable during a performance, which
                // is exactly when a player wants to change how the highway
                // looks (field 2026-07-26).
                //
                // Fed into the `nativeMenuOpen` parameter that both pure
                // predicates have always taken and that both call sites have
                // always passed `false` for. Nothing new to reason about -
                // it is the same "a menu is open, hands off" rule the native
                // menus were meant to get.
                //
                // Read INSIDE the lease: that gate exists precisely to keep
                // us out of FUCK while the render thread is mutating its
                // window state.
                const bool flickMenuOpen = FUCK::IsMenuOpen();
                // ISOLATION KILL-SWITCH (bHookNeverFilter). Pure no-op pass-
                // through: forward the ORIGINAL, untouched a_events and return
                // BEFORE FeedEngine, any capture decision, and the
                // `*link = e->next` unlink below - so the engine's intrusive
                // InputEvent list is never walked or mis-linked and no bound
                // key is ever unlinked. Answers one question in one field run:
                // does the post-session walking lock survive with our list
                // splice gone? Frets will not register while on - expected.
                if (g_hookMode == 1) {
                    g_passthroughFrames.fetch_add(1,
                                                  std::memory_order_relaxed);
                    func(a_dispatcher, a_events);
                    return;
                }
                auto&      feed = EngineFeed::GetSingleton();
                const bool active =
                    feed.active.load(std::memory_order_acquire);
                if (active) {
                    FeedEngine(feed, a_events);
                } else {
                    g_wasActive = false;  // arm the next session's reset
                }
                // MODE 2: the engine has been fed (so notes still score),
                // now hand the game its own untouched list and stop. No
                // capture decision, nothing swallowed. Splits "the feed" off
                // from "the swallow" - which mode 1 could never tell apart.
                if (g_hookMode == 2) {
                    g_passthroughFrames.fetch_add(1,
                                                  std::memory_order_relaxed);
                    func(a_dispatcher, a_events);
                    return;
                }
                auto& bus = UiBus::GetSingleton();
                session_input::Phase sessionPhase =
                    session_input::Phase::kNone;
                if (active) {
                    if (feed.engaged.load(std::memory_order_acquire)) {
                        sessionPhase = session_input::Phase::kPlaying;
                    } else if (bus.pauseMenuOpen.load(
                                   std::memory_order_acquire)) {
                        sessionPhase = session_input::Phase::kPaused;
                    } else if (bus.resumeCountdownActive.load(
                                   std::memory_order_acquire)) {
                        sessionPhase =
                            session_input::Phase::kResumeCountdown;
                    }
                }
                // Every active-session phase owns the keyboard. The old
                // engaged path swallowed only rhythm binds plus Tab, so M
                // was explicitly handed to Skyrim and could open Map.
                //
                // Recovery wins over capture: MenuSink tracks any non-passive
                // native menu that opens during the session, and while one is
                // up this hook yields the untouched list. That guarantees the
                // same vanilla shortcut can close an unexpected menu.
                const bool nativeMenuOpen = Session::NativeMenuOpen();
                const bool sessionCapture =
                    sessionPhase != session_input::Phase::kNone &&
                    !nativeMenuOpen;
                // Non-session captures (field 2026-07-19):
                //  - browser open: nav keys become UiBus intents and are
                //    swallowed so they do not steer the character
                //  - results box only: a NARROW set (green fret / Enter)
                //    closes it
                int  capture       = 0;  // 0 none / 1 browser / 2 results
                if (!active) {
                    auto* ui = RE::UI::GetSingleton();
                    // GameIsPaused is exempt when the pause is OUR OWN
                    // world freeze (field round 6) - the pause menu must
                    // stay navigable; a MessageBox always suspends capture
                    // (its dismiss keys must never be eaten - round-5
                    // wedge suspect)
                    const bool ourPause =
                        UiBus::GetSingleton().worldPaused.load(
                            std::memory_order_relaxed);
                    const bool uiClear =
                        ui && (!ui->GameIsPaused() || ourPause) &&
                        !ui->IsApplicationMenuOpen() &&
                        !ui->IsItemMenuOpen() &&
                        !ui->IsMenuOpen("MessageBoxMenu") &&
                        !ui->IsMenuOpen("Dialogue Menu");
                    if (uiClear) {
                        if (bus.browserOpen.load(
                                std::memory_order_relaxed)) {
                            capture = 1;
                        } else if (bus.resultsReady.load(
                                       std::memory_order_relaxed) ||
                                   bus.practiceSummaryReady.load(
                                       std::memory_order_relaxed)) {
                            // The practice summary reuses capture 2 - the
                            // RESULTS semantics - deliberately. Its needs are
                            // identical: confirm-only, no navigation, green
                            // fret / Enter closes.
                            //
                            // This is why it is NOT capture 3. The overlay
                            // selection below is written as a BINARY ternary
                            // in two places (`capture == 1 ? kBrowser :
                            // kResults`), so a third value would silently
                            // inherit these same semantics anyway - while
                            // looking like it had its own. Sharing the value
                            // keeps both ternaries correct AND untouched.
                            capture = 2;
                        }
                    }
                }
                // Bindings-tab services (2026-07-27). First: consume a
                // pending refresh on the hook's own thread - nobody else
                // ever writes g_binds after install.
                if (g_bindsDirty.exchange(false,
                                          std::memory_order_acq_rel)) {
                    LoadBindsFromSettings();
                }
                // Second: bind capture. Placed BEFORE the pass-through
                // return below - with the settings tool open and no
                // session, that branch would otherwise hand the list to
                // the game untouched and the capture would never see a
                // key (and the pressed key would leak into the game).
                if (const auto capMode = g_captureMode.load(
                        std::memory_order_acquire);
                    capMode != 0) {
                    if (SteadyMs() -
                            g_captureArmedMs.load(
                                std::memory_order_relaxed) >
                        kCaptureExpiryMs) {
                        g_captureMode.store(0, std::memory_order_release);
                    } else {
                        RE::InputEvent*  head =
                            a_events ? *a_events : nullptr;
                        RE::InputEvent** link = &head;
                        while (*link) {
                            RE::InputEvent* e       = *link;
                            bool            swallow = false;
                            if (capMode == 1 &&
                                e->GetDevice() ==
                                    RE::INPUT_DEVICE::kKeyboard) {
                                swallow = true;
                                if (e->GetEventType() ==
                                        RE::INPUT_EVENT_TYPE::kButton) {
                                    const auto* be = static_cast<
                                        const RE::ButtonEvent*>(e);
                                    if (be->IsDown()) {
                                        g_captureResult.store(
                                            static_cast<int>(
                                                be->GetIDCode()),
                                            std::memory_order_relaxed);
                                        g_captureMode.store(
                                            0,
                                            std::memory_order_release);
                                    }
                                }
                            } else if (capMode == 2 &&
                                       e->GetDevice() ==
                                           RE::INPUT_DEVICE::kGamepad) {
                                swallow = true;
                                if (e->GetEventType() ==
                                        RE::INPUT_EVENT_TYPE::kButton) {
                                    const auto* be = static_cast<
                                        const RE::ButtonEvent*>(e);
                                    if (be->IsDown()) {
                                        g_captureResult.store(
                                            static_cast<int>(
                                                GamepadCode(be)),
                                            std::memory_order_relaxed);
                                        g_captureMode.store(
                                            0,
                                            std::memory_order_release);
                                    }
                                }
                            }
                            if (swallow) {
                                *link = e->next;
                            } else {
                                link = &e->next;
                            }
                        }
                        RE::InputEvent* filtered = head;
                        g_filteredFrames.fetch_add(
                            1, std::memory_order_relaxed);
                        func(a_dispatcher, &filtered);
                        return;
                    }
                }
                if ((!sessionCapture && capture == 0) ||
                    ConsoleOpen()) {
                    // Crowd-failure jump lockout. The ONLY thing that makes
                    // this hook act on the pass-through path, so it is kept
                    // as narrow as possible: one bounded flag, keyboard
                    // button events only, and only the strum binds - which
                    // default to Space, i.e. Jump (InputMapper.h:42). That
                    // is the key that let a failed song end in a leap.
                    //
                    // Never while the console is open: its keys must never
                    // be eaten, and ConsoleOpen() is already one of the two
                    // reasons we are on this path at all.
                    if (!ConsoleOpen() &&
                        bus.failureInputLockout.load(
                            std::memory_order_acquire)) {
                        RE::InputEvent*  head = a_events ? *a_events : nullptr;
                        RE::InputEvent** link = &head;
                        while (*link) {
                            RE::InputEvent* e       = *link;
                            bool            swallow = false;
                            if (e->GetDevice() ==
                                    RE::INPUT_DEVICE::kKeyboard &&
                                e->GetEventType() ==
                                    RE::INPUT_EVENT_TYPE::kButton) {
                                const auto dik =
                                    static_cast<const RE::ButtonEvent*>(e)
                                        ->GetIDCode();
                                swallow =
                                    dik == static_cast<std::uint32_t>(
                                               g_binds.strum) ||
                                    dik == static_cast<std::uint32_t>(
                                               g_binds.strum2) ||
                                    dik == static_cast<std::uint32_t>(
                                               g_binds.strum3);
                            }
                            if (swallow) {
                                *link = e->next;
                            } else {
                                link = &e->next;
                            }
                        }
                        g_passthroughFrames.fetch_add(
                            1, std::memory_order_relaxed);
                        RE::InputEvent* filtered = head;
                        func(a_dispatcher, &filtered);
                        return;
                    }
                    g_passthroughFrames.fetch_add(
                        1, std::memory_order_relaxed);
                    TracePassthrough(a_events);
                    func(a_dispatcher, a_events);  // pass-through (spec 7)
                    return;
                }
                // Which capture is holding us off pass-through, for the
                // post-session probe: a stale browserOpen or resultsReady
                // latch would keep us here forever after a session ended.
                g_filteredFrames.fetch_add(1, std::memory_order_relaxed);
                g_lastCapture.store(
                    sessionPhase == session_input::Phase::kPlaying ? 3
                    : sessionPhase == session_input::Phase::kPaused ? 4
                    : sessionPhase ==
                              session_input::Phase::kResumeCountdown
                        ? 5
                        : capture,
                                     std::memory_order_relaxed);
                RE::InputEvent*  head = a_events ? *a_events : nullptr;
                RE::InputEvent** link = &head;
                while (*link) {
                    RE::InputEvent* e       = *link;
                    bool            swallow = false;
                    if (e->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
                        e->GetEventType() ==
                            RE::INPUT_EVENT_TYPE::kButton) {
                        const auto* be =
                            static_cast<const RE::ButtonEvent*>(e);
                        const auto dik = be->GetIDCode();
                        if (dik != kDikPrintScreen) {
                            pause_input::Keys keys;
                            keys.console     = kDikConsole;
                            keys.printScreen = kDikPrintScreen;
                            keys.moveUp = static_cast<std::uint32_t>(
                                g_binds.strum2);
                            keys.moveDown = static_cast<std::uint32_t>(
                                g_binds.strum3);
                            keys.confirm1 = static_cast<std::uint32_t>(
                                g_binds.fret[0]);
                            keys.confirm2 = static_cast<std::uint32_t>(
                                g_binds.fret2[0]);
                            keys.enter    = kDikEnter;
                            keys.numEnter = kDikNumEnter;
                            keys.pause1 = static_cast<std::uint32_t>(
                                g_binds.pause);
                            keys.pause2 = static_cast<std::uint32_t>(
                                g_binds.pause2);
                            if (sessionCapture) {
                                const auto action =
                                    session_input::ForKeyboard(
                                        sessionPhase, flickMenuOpen, dik,
                                        keys);
                                swallow =
                                    action != pause_input::Action::kPass;
                                if (sessionPhase ==
                                    session_input::Phase::kPaused) {
                                    PublishNavHold(action, be);
                                }
                                if (be->IsDown()) {
                                    if (sessionPhase ==
                                            session_input::Phase::kPaused &&
                                        action ==
                                        pause_input::Action::kMoveUp) {
                                        bus.navMove.fetch_add(-1);
                                    } else if (
                                        sessionPhase ==
                                            session_input::Phase::kPaused &&
                                        action ==
                                        pause_input::Action::kMoveDown) {
                                        bus.navMove.fetch_add(1);
                                    } else if (
                                        sessionPhase ==
                                            session_input::Phase::kPaused &&
                                        action ==
                                        pause_input::Action::kConfirm) {
                                        bus.navConfirm.store(true);
                                    } else if (
                                        sessionPhase ==
                                            session_input::Phase::kPaused &&
                                        action ==
                                        pause_input::Action::kResume) {
                                        Session::RequestResume();
                                    } else if (
                                        sessionPhase ==
                                            session_input::Phase::
                                                kResumeCountdown &&
                                        action ==
                                            pause_input::Action::kPause) {
                                        Session::RequestPause();
                                    }
                                }
                            } else {
                                const auto overlay =
                                    capture == 1
                                      ? session_input::Overlay::kBrowser
                                      : session_input::Overlay::kResults;
                                const auto action =
                                    session_input::ForOverlayKeyboard(
                                        overlay, dik, keys,
                                        g_binds.IsBound(dik));
                                swallow =
                                    action != pause_input::Action::kPass;
                                if (capture == 1) {
                                    PublishNavHold(action, be);
                                }
                                if (swallow && be->IsDown()) {
                                    if (capture == 1) {
                                        HarvestBrowserNav(dik);
                                    } else if (
                                        action ==
                                        pause_input::Action::kConfirm) {
                                        UiBus::GetSingleton()
                                            .navConfirm.store(true);
                                    }
                                }
                            }
                            if (swallow && dik == kDikConsole &&
                                be->IsDown()) {
                                spdlog::info(
                                    "[input] console key suppressed "
                                    "(capture={})",
                                    g_lastCapture.load(
                                        std::memory_order_relaxed));
                            }
                        }
                    } else if (
                        g_controllerEnabled &&
                        e->GetDevice() == RE::INPUT_DEVICE::kGamepad) {
                        // BardHero owns the whole pad while one of its
                        // surfaces is active. Button edges become UI intents;
                        // thumbsticks and any unbound buttons are swallowed
                        // so Skyrim cannot move the player or open a menu
                        // behind the minigame.
                        swallow = true;
                        if (e->GetEventType() ==
                            RE::INPUT_EVENT_TYPE::kButton) {
                            const auto* be =
                                static_cast<const RE::ButtonEvent*>(e);
                            const auto code = GamepadCode(be);
                            const auto keys = PadNavigationKeys();
                            pause_input::Action action =
                                pause_input::Action::kSwallow;
                            if (sessionCapture) {
                                action = session_input::ForGamepad(
                                    sessionPhase, flickMenuOpen, code, keys);
                            } else {
                                action =
                                    session_input::ForOverlayGamepad(
                                        capture == 1
                                          ? session_input::Overlay::kBrowser
                                          : session_input::Overlay::kResults,
                                        code, keys);
                            }
                            swallow =
                                action != pause_input::Action::kPass;
                            if ((sessionCapture &&
                                 sessionPhase ==
                                     session_input::Phase::kPaused) ||
                                capture == 1) {
                                PublishNavHold(action, be);
                            }
                            if (swallow && be->IsDown()) {
                                if (action ==
                                    pause_input::Action::kMoveUp) {
                                    bus.navMove.fetch_add(-1);
                                } else if (
                                    action ==
                                    pause_input::Action::kMoveDown) {
                                    bus.navMove.fetch_add(1);
                                } else if (
                                    action ==
                                    pause_input::Action::kConfirm) {
                                    bus.navConfirm.store(true);
                                } else if (
                                    action ==
                                    pause_input::Action::kResume) {
                                    if (sessionPhase ==
                                        session_input::Phase::kPaused) {
                                        Session::RequestResume();
                                    } else {
                                        bus.navClose.store(true);
                                    }
                                } else if (
                                    action ==
                                    pause_input::Action::kPause) {
                                    Session::RequestPause();
                                }
                            }
                        }
                    }
                    if (swallow) {
                        // Unlinking writes through `link`, and once we are
                        // past the first event that slot is the PREVIOUS
                        // event's `next` field - engine-owned memory. The
                        // engine reuses those event objects frame after
                        // frame, so a rewire left behind is not confined to
                        // this dispatch: it corrupts the list for the rest of
                        // the process. That is the post-session input lock -
                        // every input dead, every game-side control still
                        // reading healthy, a save-reload no help and only a
                        // restart clearing it (proved 2026-07-21 by
                        // bHookNeverFilter=1 making it vanish).
                        //
                        // So borrow the list, do not keep it: remember each
                        // slot with its original value and put them all back
                        // once the game has consumed the filtered view.
                        // `&head` is our own local and needs no restoring.
                        if (link != &head) {
                            g_patches.push_back({ link, *link });
                        }
                        *link = e->next;  // the game never sees it
                    } else {
                        link = &e->next;
                    }
                }
                // MODE 3: hand the game ITS OWN array with the head swapped,
                // then put the head back - so nothing but engine memory ever
                // travels down a chain that FLICK and Fitting Room also hook
                // ([[chained-hook-pitfalls]]). Mode 0 passes `filtered`, a
                // pointer to OUR stack array; if anything downstream keeps
                // that pointer past the call it is reading a dead frame.
                if (g_hookMode == 3 && a_events) {
                    auto** slot = const_cast<RE::InputEvent**>(a_events);
                    RE::InputEvent* const origHead = *slot;
                    *slot = head;
                    func(a_dispatcher, a_events);
                    *slot = origHead;
                } else {
                    RE::InputEvent* const filtered[1] = { head };
                    func(a_dispatcher, filtered);
                }
                // Reverse order: a slot unlinked more than once (consecutive
                // swallowed events) is walked back to the value it had before
                // the FIRST write.
                for (auto it = g_patches.rbegin(); it != g_patches.rend();
                     ++it) {
                    *it->slot = it->orig;
                }
                g_patches.clear();
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };
    }

    unsigned long InputHook::PassthroughFrames() {
        return g_passthroughFrames.load(std::memory_order_relaxed);
    }
    unsigned long InputHook::FilteredFrames() {
        return g_filteredFrames.load(std::memory_order_relaxed);
    }
    int InputHook::LastCapture() {
        return g_lastCapture.load(std::memory_order_relaxed);
    }
    void InputHook::ArmPassthroughTrace(int a_events) {
        g_traceBudget.store(a_events, std::memory_order_relaxed);
    }

    void InputHook::SetMovementSwallow(const std::uint32_t* a_diks,
                                       int                  a_count) {
        std::uint64_t w[4]{};
        if (a_diks) {
            for (int i = 0; i < a_count; ++i) {
                const auto d = a_diks[i];
                // 0 is "no bind" and 0xFF is the engine's UNBOUND marker;
                // neither is a key anyone can press, and mapping 0xFF would
                // swallow a real scan code's slot.
                if (d != 0 && d < 0xFF) {
                    w[d >> 6] |= 1ull << (d & 63);
                }
            }
        }
        for (int i = 0; i < 4; ++i) {
            g_moveSwallow[i].store(w[i], std::memory_order_relaxed);
        }
    }

    namespace {
        // The whole bind surface, re-readable: Install() runs it once at
        // startup, and the thunk re-runs it on the hook thread whenever
        // the Bindings tab flags g_bindsDirty (RefreshBinds). Keeping the
        // writes on the hook thread is what lets g_binds/g_padBinds stay
        // plain structs.
        void LoadBindsFromSettings() {
            const auto& st  = Settings::GetSingleton();
            g_binds.fret[0] = st.fret1Key;
            g_binds.fret[1] = st.fret2Key;
            g_binds.fret[2] = st.fret3Key;
            g_binds.fret[3] = st.fret4Key;
            g_binds.fret[4] = st.fret5Key;
            g_binds.strum   = st.strumKey;
            g_binds.sp      = st.spKey;
            g_binds.whammy  = st.whammyKey;
            g_binds.pause   = st.pauseKey;
            g_binds.fret2[0] = st.fret1Key2;
            g_binds.fret2[1] = st.fret2Key2;
            g_binds.fret2[2] = st.fret3Key2;
            g_binds.fret2[3] = st.fret4Key2;
            g_binds.fret2[4] = st.fret5Key2;
            g_binds.strum2   = st.strumKey2;
            g_binds.strum3   = st.strumKey3;
            g_binds.sp2      = st.spKey2;
            g_binds.whammy2  = st.whammyKey2;
            g_binds.pause2   = st.pauseKey2;
            g_controllerEnabled = st.controllerEnabled;
            g_gamepadMode       = st.gamepadMode;
            g_fretsOnly         = st.fretsOnly;
            g_strumGraceSec =
                difficulty::EngineParamsFor(st.tuning).strumLeniency;
            g_padBinds.fret[0]  = st.gamepadFret1;
            g_padBinds.fret[1]  = st.gamepadFret2;
            g_padBinds.fret[2]  = st.gamepadFret3;
            g_padBinds.fret[3]  = st.gamepadFret4;
            g_padBinds.fret[4]  = st.gamepadFret5;
            g_padBinds.strum[0] = st.gamepadStrum1;
            g_padBinds.strum[1] = st.gamepadStrum2;
            g_padBinds.sp       = st.gamepadSp;
            g_padBinds.whammy   = st.gamepadWhammy;
            g_padBinds.pause    = st.gamepadPause;
            g_padBinds.confirm  = st.gamepadConfirm;
            g_padBinds.cancel   = st.gamepadCancel;
        }
    }

    void InputHook::BeginBindCapture(BindDevice a_device) {
        g_captureResult.store(-1, std::memory_order_relaxed);
        g_captureArmedMs.store(SteadyMs(), std::memory_order_relaxed);
        g_captureMode.store(static_cast<int>(a_device),
                            std::memory_order_release);
    }

    void InputHook::CancelBindCapture() {
        g_captureMode.store(0, std::memory_order_release);
        g_captureResult.store(-1, std::memory_order_relaxed);
    }

    int InputHook::TakeCaptureResult() {
        return g_captureResult.exchange(-1, std::memory_order_acq_rel);
    }

    void InputHook::RefreshBinds() {
        g_bindsDirty.store(true, std::memory_order_release);
    }

    void InputHook::Install() {
        LoadBindsFromSettings();
        const auto& st = Settings::GetSingleton();

        // Read the isolation kill-switch ONCE here (never on the game thread).
        g_neverFilter = st.hookNeverFilter;
        g_hookMode    = st.hookMode;
        if (g_neverFilter) {
            spdlog::warn(
                "[input] NEVER-FILTER mode ON (bHookNeverFilter=1): the hook "
                "is a no-op pass-through, bound keys will NOT be unlinked and "
                "the engine is NOT fed. Diagnostic isolation only.");
        }

        // 1ms multimedia timer resolution: DI stamps live in the
        // timeGetTime domain (M0; the OS restores it at process exit).
        timeBeginPeriod(1);

        const REL::Relocation<std::uintptr_t> target{
            REL::RelocationID(67315, 68617), 0x7B
        };
        // Byte-verify per-runtime before trusting the site (AE inlined-
        // hook-sites pitfall). M0 verified 1.6.1170 live; the check stays
        // for every future runtime.
        if (*reinterpret_cast<std::uint8_t*>(target.address()) != 0xE8) {
            spdlog::error(
                "[input] dispatch site byte != E8 - hook NOT installed; "
                "sessions will run without input.");
            return;
        }
        DispatchHook::func = SKSE::GetTrampoline().write_call<5>(
            target.address(), DispatchHook::thunk);
        spdlog::info(
            "[input] hook installed (67315/68617+0x7B, linkrestore=ON, "
            "mode={}, recovery=capped); binds frets="
            "0x{:02X},0x{:02X},0x{:02X},0x{:02X},0x{:02X} strum=0x{:02X} "
            "sp=0x{:02X} whammy=0x{:02X} pause=0x{:02X}",
            g_hookMode, g_binds.fret[0], g_binds.fret[1], g_binds.fret[2],
            g_binds.fret[3], g_binds.fret[4], g_binds.strum, g_binds.sp,
            g_binds.whammy, g_binds.pause);
        spdlog::info(
            "[input] secondary binds (controller bridge): frets="
            "0x{:02X},0x{:02X},0x{:02X},0x{:02X},0x{:02X} strum=0x{:02X}/"
            "0x{:02X} sp=0x{:02X} whammy=0x{:02X} pause=0x{:02X}",
            g_binds.fret2[0], g_binds.fret2[1], g_binds.fret2[2],
            g_binds.fret2[3], g_binds.fret2[4], g_binds.strum2,
            g_binds.strum3, g_binds.sp2, g_binds.whammy2, g_binds.pause2);
        spdlog::info(
            "[input] native controller enabled={} gamepadMode={} frets="
            "{},{},{},{},{} strum={}/{} sp={} whammy={} pause={} "
            "confirm={} cancel={}",
            g_controllerEnabled, g_gamepadMode, g_padBinds.fret[0],
            g_padBinds.fret[1], g_padBinds.fret[2], g_padBinds.fret[3],
            g_padBinds.fret[4], g_padBinds.strum[0],
            g_padBinds.strum[1], g_padBinds.sp, g_padBinds.whammy,
            g_padBinds.pause, g_padBinds.confirm, g_padBinds.cancel);
    }
}
