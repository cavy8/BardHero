// src/game/PerformanceCamera.cpp
#include "PCH.h"

#include "game/PerformanceCamera.h"

#include "QpcClock.h"
#include "Settings.h"
#include "chart/LoadSong.h"
#include "clock/MasterClock.h"
#include "engine/GuitarEngine.h"
#include "game/CameraDirectorLogic.h"
#include "game/EngineFeed.h"
#include "game/UiBus.h"

#include "RE/P/PlayerCamera.h"
#include "RE/T/ThirdPersonState.h"

// SmoothCam owns the FINAL camera position when it is active, so writing
// vanilla ThirdPersonState fields is not enough on a load order that has it
// (which is most of them, and certainly Nolvus). See the handshake below.
#define SMOOTHCAM_API_COMMONLIB
#include "SmoothCamAPI.h"

#include <cmath>
#include <vector>

namespace SH::PerformanceCamera {
    namespace {
        constexpr float kDegToRad = 0.01745329251994329577f;
        // How long after a cut before the zoom gap means anything. The
        // engine eases currentZoomOffset toward targetZoomOffset, so a fresh
        // cut always shows a gap; only a gap that OUTLASTS the ease is
        // geometry. Comfortably longer than the vanilla zoom ease.
        constexpr double kZoomSettleSec = 0.60;
        // Zoom units the engine has to be pulling the camera in by before it
        // counts as a clamp at all.
        constexpr float  kClampGap      = 0.12f;

        std::atomic<bool> g_hooked{ false };
        std::atomic<bool> g_directorOn{ false };
        // Cross-thread release request. Session teardown runs on the session
        // thread; the camera is only safe to touch on the game thread, so
        // the ending paths raise this and the next hook pass honours it.
        std::atomic<bool> g_releaseReq{ false };

        // ---- game thread only ------------------------------------------
        camdir::Director g_dir;
        bool   g_engaged   = false;
        double g_engagedAt = 0.0;

        // Per-song copy, taken ONCE at engage. The spec is explicit that the
        // step function must touch no shared container, so the tempo map and
        // the section times are held by value here rather than read out of
        // EngineFeed::song every frame under the lock.
        bard::TempoMap      g_tempo;
        std::vector<double> g_sectionSec;
        double              g_lastNoteSec = 0.0;
        double              g_beatsPerBar = 4.0;
        bool                g_songLoaded  = false;

        // Event edges the director wants as one-shots.
        bool g_prevSp        = false;
        int  g_lastMilestone = 0;

        // What the game had before we touched it. Restored on every release
        // path: zero is not neutral for these fields, it is just a different
        // camera, and a director that does not hand the view back is exactly
        // the defect the spike harness shipped with on 2026-07-26.
        float g_origYaw = 0.0f, g_origPitch = 0.0f, g_origZoom = 0.0f;
        float g_origFov = 0.0f;
        bool  g_origFree = false;
        bool  g_haveOrig = false;

        // Occlusion: set from the collision read at the END of the previous
        // frame, consumed by the next snapshot. g_lastCutAt gates it past
        // the zoom ease - see kZoomSettleSec.
        bool   g_clamped    = false;
        double g_lastCutAt  = 0.0;
        double g_lastDiagAt = 0.0;

        // ---- SmoothCam handshake ----------------------------------------
        //
        // WHY THIS EXISTS, because Spike 2 said it would not be needed and
        // Spike 2 was wrong. That spike checked whether our writes to
        // freeRotation and targetZoomOffset were still in the FIELDS on the
        // next frame. They were, and they still are - the 2026-07-26 field
        // log shows `pitch want=0.105 got=0.105` and `zoom want=0.34
        // cur=0.34`, exact, every sample. What it never checked was whether
        // the CAMERA obeyed those fields, and with SmoothCam installed it
        // does not: SmoothCam owns the final camera position, reads vanilla
        // state as an input, and applies its own interpolation, distance and
        // FOV on top. That is precisely the symptom set reported - cuts
        // taking about a second to catch up, an unchanging wide FOV, and a
        // camera parked far enough out to be in the walls.
        //
        // The spec called this outcome in advance and named the remedy:
        // request camera control for the performance, SendToGoalPosition
        // before handing back so the return does not snap.
        SmoothCamAPI::IVSmoothCam3* g_smoothCam = nullptr;
        bool g_haveCameraControl = false;

        void RequestSmoothCamControl() {
            if (!g_smoothCam || g_haveCameraControl) { return; }
            const auto r =
                g_smoothCam->RequestCameraControl(SKSE::GetPluginHandle());
            g_haveCameraControl = r == SmoothCamAPI::APIResult::OK ||
                                  r == SmoothCamAPI::APIResult::AlreadyGiven;
            spdlog::info("[camdir] SmoothCam camera control: {}",
                         g_haveCameraControl ? "GRANTED"
                                             : "refused (another owner or "
                                               "SmoothCam must keep it)");
        }

        void ReleaseSmoothCamControl() {
            if (!g_smoothCam || !g_haveCameraControl) { return; }
            const auto handle = SKSE::GetPluginHandle();
            // Move to SmoothCam's own goal as it takes over, so the handback
            // is a move rather than a snap from wherever our last shot left
            // the camera.
            (void)g_smoothCam->SendToGoalPosition(handle, true, false,
                                                  nullptr);
            (void)g_smoothCam->ReleaseCameraControl(handle);
            g_haveCameraControl = false;
            spdlog::info("[camdir] SmoothCam camera control released");
        }

        void ClearSong() {
            g_tempo = bard::TempoMap{ };
            g_sectionSec.clear();
            g_lastNoteSec = 0.0;
            g_beatsPerBar = 4.0;
            g_songLoaded  = false;
        }

        // One lock, once, at engage. Everything the director needs per frame
        // afterwards is either an atomic or a member of this copy.
        void LoadSongCopy() {
            auto& feed = EngineFeed::GetSingleton();
            std::scoped_lock lk(feed.mx);
            if (!feed.song) { return; }
            const auto& chart = feed.song->chart;
            g_tempo = chart.tempo;
            g_sectionSec.clear();
            g_sectionSec.reserve(chart.sections.size());
            for (const auto& sec : chart.sections) {
                g_sectionSec.push_back(sec.time);
            }
            g_lastNoteSec =
                chart.notes.empty() ? 0.0 : chart.notes.back().time;
            // Numerator only: the denominator is display/SP in this codebase
            // and never timing (ChartTypes.h), and a bar is what the cut
            // cadence counts. First marker wins - a mid-song meter change
            // would re-phase the grid, which is not worth the complexity for
            // a camera.
            g_beatsPerBar =
                chart.timeSigs.empty()
                    ? 4.0
                    : static_cast<double>(chart.timeSigs.front().num);
            if (g_beatsPerBar < 1.0) { g_beatsPerBar = 4.0; }
            g_songLoaded = true;
        }

        void CaptureOriginals(RE::ThirdPersonState* a_st,
                              RE::PlayerCamera* a_cam) {
            g_origYaw   = a_st->freeRotation.x;
            g_origPitch = a_st->freeRotation.y;
            g_origZoom  = a_st->targetZoomOffset;
            g_origFree  = a_st->freeRotationEnabled;
            g_origFov   = a_cam ? a_cam->worldFOV : 0.0f;
            g_haveOrig  = true;
        }

        void RestoreOriginals(RE::ThirdPersonState* a_st,
                              RE::PlayerCamera* a_cam) {
            if (!g_haveOrig) { return; }
            a_st->freeRotation.x      = g_origYaw;
            a_st->freeRotation.y      = g_origPitch;
            a_st->targetZoomOffset    = g_origZoom;
            a_st->freeRotationEnabled = g_origFree;
            if (a_cam && g_origFov > 0.0f) { a_cam->worldFOV = g_origFov; }
            g_haveOrig = false;
        }

        void Disengage(RE::ThirdPersonState* a_st, RE::PlayerCamera* a_cam,
                       const char* a_why) {
            if (!g_engaged) { return; }
            ReleaseSmoothCamControl();
            RestoreOriginals(a_st, a_cam);
            g_engaged    = false;
            g_clamped    = false;
            g_lastCutAt  = 0.0;
            g_lastDiagAt = 0.0;
            g_prevSp    = false;
            g_lastMilestone = 0;
            ClearSong();
            spdlog::info("[camdir] released ({}) after {} cuts", a_why,
                         g_dir.CutCount());
            g_dir.Reset();
        }

        camdir::Snapshot BuildSnapshot(double a_songSec,
                                       const bard::EngineStats& a_stats) {
            camdir::Snapshot s;
            s.songSec = a_songSec;
            if (g_songLoaded && g_tempo.Resolution() > 0) {
                const double tick  = g_tempo.TickAt(a_songSec);
                const double beats =
                    tick / static_cast<double>(g_tempo.Resolution());
                const double bars = beats / g_beatsPerBar;
                s.barIndex = static_cast<int>(bars < 0.0 ? 0.0 : bars);
                const double phase = beats - std::floor(beats);
                s.beatPhase = static_cast<float>(phase < 0.0 ? 0.0 : phase);
            }
            // Sections are sorted, and a song is a few dozen of them at
            // most, so a linear scan per frame is cheaper than the branch
            // needed to cache a cursor across a seek.
            s.sectionIndex = -1;
            for (std::size_t i = 0; i < g_sectionSec.size(); ++i) {
                if (a_songSec >= g_sectionSec[i]) {
                    s.sectionIndex = static_cast<int>(i);
                } else {
                    break;
                }
            }
            auto& bus  = UiBus::GetSingleton();
            s.glory    = bus.glory.load(std::memory_order_relaxed);
            s.streak   = a_stats.combo;
            s.spActive = a_stats.spActive;
            s.clamped  = g_clamped;

            s.spJustActivated = a_stats.spActive && !g_prevSp;
            g_prevSp          = a_stats.spActive;
            // Every 50 comboed notes, matching the streak banners the
            // highway already celebrates - the camera should answer the
            // moment the player is being told about, not a different one.
            const int milestone = a_stats.combo / 50;
            s.streakMilestone   = milestone > g_lastMilestone;
            if (milestone > g_lastMilestone) { g_lastMilestone = milestone; }
            // A window, not an instant: at 60fps an equality test on a
            // float second would miss most frames, and the director's own
            // minimum hold stops the repeat from chaining into a strobe.
            s.finalNote = g_lastNoteSec > 0.0 &&
                          a_songSec >= g_lastNoteSec - 0.10 &&
                          a_songSec <= g_lastNoteSec + 0.40;
            return s;
        }

        // The measurement spikes that used to live here are GONE, and
        // deliberately: both were answered on 2026-07-26 and their outcomes
        // are written into the spec, so a harness that only ever throws the
        // camera around is now pure liability. Git history has it if a
        // future runtime or camera mod ever needs re-measuring.

        struct UpdateHook {
            static void thunk(RE::ThirdPersonState* a_this,
                              RE::BSTSmartPointer<RE::TESCameraState>& a_next) {
                if (!a_this) {
                    func(a_this, a_next);
                    return;
                }
                auto* cam = RE::PlayerCamera::GetSingleton();
                auto& feed = EngineFeed::GetSingleton();
                auto& bus  = UiBus::GetSingleton();
                const bool engaged =
                    feed.engaged.load(std::memory_order_acquire);
                const bool paused =
                    bus.worldPaused.load(std::memory_order_relaxed) ||
                    bus.pauseMenuOpen.load(std::memory_order_relaxed);

                if (g_releaseReq.exchange(false)) {
                    Disengage(a_this, cam, "session teardown");
                }

                // ⚠ PAUSE IS TESTED FIRST, AND THE ORDER IS THE WHOLE FIX.
                //
                // EngineFeed::engaged means kPlaying/kResuming, so it goes
                // FALSE while the session is paused. With the !engaged test
                // first, every pause took the session-ended path: the field
                // log for 2026-07-26 shows "released (session ended)"
                // followed by a fresh "engaged" on every single unpause, so
                // the director lost its held shot, its blacklist and its cut
                // count each time, and re-captured the camera as if a new
                // song had started. The spec asks for exactly the opposite -
                // freeze on pause, and resume by snapping back to the held
                // shot.
                //
                // Freezing IS the snap-back: the director is not stepped, so
                // no time passes for it, and our last write stands.
                //
                // Quitting FROM the pause menu still releases, because
                // EndSession raises the release request that is handled
                // above this - it does not depend on reaching the branch
                // below.
                if (g_engaged && paused) {
                    func(a_this, a_next);
                    return;
                }
                if (!g_directorOn.load(std::memory_order_relaxed) ||
                    !engaged) {
                    Disengage(a_this, cam, "session ended");
                    func(a_this, a_next);
                    return;
                }

                if (!g_engaged) {
                    g_engaged   = true;
                    g_engagedAt = QpcSec();
                    // Before anything is written: with SmoothCam active our
                    // vanilla-field writes are only an input to its solver,
                    // not the camera.
                    RequestSmoothCamControl();
                    CaptureOriginals(a_this, cam);
                    g_dir.Reset();
                    ClearSong();
                    LoadSongCopy();
                    spdlog::info(
                        "[camdir] engaged - {} section marker(s), {:.1f} "
                        "beats/bar, last note {:.1f}s",
                        g_sectionSec.size(), g_beatsPerBar, g_lastNoteSec);
                }

                // One lock per frame, on the game thread, exactly as the
                // input hook already does. Nothing else is read from the
                // feed - the tempo map and sections were copied at engage.
                double            songSec = 0.0;
                bard::EngineStats stats{ };
                {
                    std::scoped_lock lk(feed.mx);
                    if (!feed.clock || !feed.engine) {
                        func(a_this, a_next);
                        return;
                    }
                    songSec = feed.clock->VisualTime(QpcSec());
                    stats   = feed.engine->Stats();
                }

                const auto snap = BuildSnapshot(songSec, stats);
                const auto out  = g_dir.Step(snap);
                if (out.cut) {
                    spdlog::info(
                        "[camdir] cut #{} -> shot {} at {:.2f}s (bar {}, "
                        "section {}, glory {:.2f})",
                        g_dir.CutCount(), static_cast<int>(out.id), songSec,
                        snap.barIndex, snap.sectionIndex, snap.glory);
                }

                // BEFORE the original: the engine's collision and smoothing
                // pass runs inside it, so this is what it solves against.
                a_this->freeRotationEnabled = true;
                a_this->freeRotation.x      = out.yawDeg * kDegToRad;
                a_this->freeRotation.y      = out.pitchDeg * kDegToRad;
                a_this->targetZoomOffset    = out.distance;
                // A CUT IS INSTANT, so snap the current zoom too. Writing
                // only the TARGET leaves the engine easing currentZoomOffset
                // toward it, which is visible as the camera sliding into
                // place for a beat or so after every cut instead of simply
                // being there (field 2026-07-26, "the camera can have this
                // interpolation before it settles"). That ease belongs to
                // player-driven zoom, not to direction - Guitar Hero cuts,
                // it does not dolly. The drift within a shot is what the
                // director expresses smoothly, and that still works because
                // it moves the target a little each frame.
                if (out.cut) { a_this->currentZoomOffset = out.distance; }
                if (cam && g_origFov > 0.0f) {
                    cam->worldFOV = g_origFov + out.fovDelta;
                }

                func(a_this, a_next);

                // AFTER: the collision-corrected position the engine just
                // solved. A shot the engine is pulling in hard is occluded;
                // the director turns a sustained clamp into a cut and a
                // blacklist entry. Reading it here is the only place it
                // exists - which is why the write order above is not a
                // style choice.
                //
                // ⚠ THE SETTLE WINDOW IS LOAD-BEARING, not caution.
                // currentZoomOffset CHASES targetZoomOffset through the
                // engine's own smoothing, so for a few hundred ms after any
                // cut to a wider shot the gap is large for a reason that has
                // nothing to do with geometry. Without this gate every such
                // cut would read as an occlusion and blacklist a perfectly
                // good shot - and because blacklisting is permanent for the
                // song, the pool would collapse to the fallback within the
                // first few phrases of every performance.
                //
                // ⚠ UNVERIFIED IN THE FIELD. The 2026-07-26 spike never
                // observed an actual clamp, so both the comparison and the
                // 0.12 threshold are the spec's design rather than measured
                // values. If the blacklist never fires in a tight interior,
                // or fires constantly, this is the first line to instrument.
                if (out.cut) { g_lastCutAt = songSec; }
                g_clamped = false;
                if (!std::isnan(a_this->collisionPosValid) &&
                    (songSec - g_lastCutAt) >= kZoomSettleSec) {
                    const float want = out.distance;
                    const float got  = a_this->currentZoomOffset;
                    g_clamped = (want - got) > kClampGap;
                }

                // ---- diagnosis: "pushed by an actor and the camera ends
                // up constantly looking up" (field 2026-07-26) ------------
                //
                // No fix here yet, deliberately. At Tier 2 we do not own
                // camera collision, so every candidate fix is a guess until
                // we know WHICH field is moving. Reading freeRotation back
                // after the original is the only way to see whether the
                // engine (or another mod) is overwriting the pitch we
                // asked for, or whether the pitch is ours and the camera is
                // being displaced some other way - pitchZoomOffset and the
                // collision position separate those two cases.
                //
                // Logged on a 1s cadence, plus immediately whenever the
                // pitch we get back differs from the pitch we wrote, which
                // is the event actually worth catching.
                {
                    const float wantPitch = out.pitchDeg * kDegToRad;
                    const float gotPitch  = a_this->freeRotation.y;
                    const bool  drifted =
                        std::fabs(gotPitch - wantPitch) > 0.02f;
                    // FOV READBACK. "I don't really notice changes in FOV"
                    // (field 2026-07-26) has two possible causes that need
                    // opposite fixes - the deltas are too small to see, or
                    // something downstream is overwriting worldFOV every
                    // frame the way SmoothCam was overriding position. This
                    // settles it: wantFov is what we just wrote, gotFov is
                    // what is actually in the field after the engine's pass.
                    // Equal but ineffective means the deltas need to grow;
                    // unequal means we are being overwritten and the answer
                    // is upstream, not in the table.
                    const float wantFov =
                        (cam && g_origFov > 0.0f) ? g_origFov + out.fovDelta
                                                  : 0.0f;
                    const float gotFov = cam ? cam->worldFOV : 0.0f;
                    const bool  fovLost =
                        wantFov > 0.0f && std::fabs(gotFov - wantFov) > 0.5f;
                    if (drifted || fovLost || songSec - g_lastDiagAt >= 1.0) {
                        g_lastDiagAt = songSec;
                        spdlog::info(
                            "[camdir] state t={:.1f} pitch want={:.3f} "
                            "got={:.3f}{} | yaw want={:.3f} got={:.3f} | "
                            "zoom want={:.2f} cur={:.2f} | fov base={:.1f} "
                            "want={:.1f} got={:.1f}{} | pitchZoom={:.1f} "
                            "collValid={} clamped={}",
                            songSec, wantPitch, gotPitch,
                            drifted ? " DRIFTED" : "",
                            out.yawDeg * kDegToRad, a_this->freeRotation.x,
                            out.distance, a_this->currentZoomOffset,
                            g_origFov, wantFov, gotFov,
                            fovLost ? " FOV-OVERWRITTEN" : "",
                            a_this->pitchZoomOffset,
                            std::isnan(a_this->collisionPosValid) ? "NaN"
                                                                  : "num",
                            g_clamped);
                    }
                }
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };
    }

    void RegisterSmoothCam() {
        // kPostLoad: register the callback that will receive the interface.
        // Must be exactly once, and before the request below.
        const auto* msg = SKSE::GetMessagingInterface();
        if (!msg) { return; }
        const bool ok = SmoothCamAPI::RegisterInterfaceLoaderCallback(
            msg, [](void* a_instance,
                    SmoothCamAPI::InterfaceVersion a_version) {
                if (a_version >= SmoothCamAPI::InterfaceVersion::V3) {
                    g_smoothCam =
                        static_cast<SmoothCamAPI::IVSmoothCam3*>(a_instance);
                    spdlog::info(
                        "[camdir] SmoothCam API acquired (interface V{})",
                        static_cast<int>(a_version) + 1);
                } else {
                    spdlog::warn(
                        "[camdir] SmoothCam offered interface V{}, which is "
                        "older than the V3 this needs - the director will "
                        "run without the handshake and SmoothCam will keep "
                        "overriding it",
                        static_cast<int>(a_version) + 1);
                }
            });
        if (!ok) {
            spdlog::info(
                "[camdir] SmoothCam interface callback not registered - "
                "SmoothCam is probably not installed, which is fine: the "
                "vanilla field writes are the camera on their own then");
        }
    }

    void RequestSmoothCam() {
        // kPostPostLoad: SmoothCam answers this by invoking the callback
        // registered above. Silence here just means it is not installed.
        if (const auto* msg = SKSE::GetMessagingInterface()) {
            (void)SmoothCamAPI::RequestInterface(msg);
        }
    }

    void Release() {
        g_releaseReq.store(true, std::memory_order_release);
    }

    bool Hooked() { return g_hooked.load(std::memory_order_acquire); }

    void Install() {
        // Installed ONCE and never removed, but the per-frame path is gated
        // on the live setting - so the FLICK toggle can turn the director on
        // and off mid-session without ever touching a vtable at runtime,
        // which is not a thing to be doing while frames are in flight.
        //
        // The hook is only installed at all if the setting was on at load,
        // so a player who has never enabled it carries no camera hook.
        const auto& st = Settings::GetSingleton();
        g_directorOn.store(st.performanceCameraDirector,
                           std::memory_order_release);
        if (!st.performanceCameraDirector) { return; }
        // ThirdPersonState has a single vtable; Update is slot 03 per
        // RE/T/ThirdPersonState.h. write_vfunc, never write_branch<5> - a
        // non-branch entry returns a garbage original.
        REL::Relocation<std::uintptr_t> vtbl{ RE::ThirdPersonState::VTABLE[0] };
        UpdateHook::func = vtbl.write_vfunc(0x03, UpdateHook::thunk);
        g_hooked.store(true, std::memory_order_release);
        spdlog::info("[camdir] ThirdPersonState::Update hooked - the "
                     "performance camera director is active");
    }

    void SetEnabled(bool a_on) {
        // Live toggle from the settings page. Turning it ON only takes
        // effect if the hook was installed at load: a vtable write mid-frame
        // is not worth the risk, and the log says so rather than silently
        // doing nothing.
        const bool hooked = g_hooked.load(std::memory_order_acquire);
        g_directorOn.store(a_on && hooked, std::memory_order_release);
        if (a_on && !hooked) {
            spdlog::info(
                "[camdir] enabled in settings, but the camera hook was not "
                "installed at load - it takes effect on the next game start");
        } else {
            spdlog::info("[camdir] {} from settings",
                         a_on ? "enabled" : "disabled");
        }
        if (!a_on) { Release(); }
    }
}
