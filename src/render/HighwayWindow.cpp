// src/render/HighwayWindow.cpp
#include "PCH.h"
#include "render/HighwayWindow.h"

#include "QpcClock.h"
#include "Settings.h"
#include "game/EngineFeed.h"
#include "game/UiBus.h"  // practiceActive - banners are hidden in practice
#include "game/UiSfx.h"

#include "chart/LoadSong.h"
#include "engine/GuitarEngine.h"
#include "clock/MasterClock.h"

#include "render/FlickRenderer.h"
#include "render/FlickWindowPolicy.h"
#include "render/BackdropLayout.h"
#include "render/FxPool.h"
#include "render/HighwayLayout.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h (house ordering)

#include "FUCK_API.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace SH {
    namespace {
        namespace hw = SH::hw;

        class HighwayWindow final : public FUCK::IWindow {
        public:
            const char* Id() const override { return "HighwayV2"; }
            const char* Title() const override { return "BardHero Highway"; }
            bool        IsOpen() const override {
                return EngineFeed::GetSingleton().active.load(
                    std::memory_order_acquire);
            }
            void SetOpen(bool) override {}  // session-driven only
            void Draw() override {}         // everything is overlay (spec 9)
            FUCK::WindowFlags GetFlags() const override {
                using F = FUCK::WindowFlags;
                // kPassInputToGame is load-bearing (SPIKE-3 field bug:
                // without it FLICK blocks ALL game input while open).
                // Draw() is empty but FLICK still creates an ImGui host for
                // RenderOverlay. Missing kNoResize exposed that transparent
                // host's lone resize-grip triangle around (400,300).
                return static_cast<F>(
                    flick_window_policy::HighwayHostFlags(
                        static_cast<unsigned>(F::kNoDecoration),
                        static_cast<unsigned>(F::kNoBackground),
                        static_cast<unsigned>(F::kNoMove),
                        static_cast<unsigned>(F::kNoResize),
                        static_cast<unsigned>(F::kPassInputToGame),
                        static_cast<unsigned>(F::kHideHUD),
                        static_cast<unsigned>(F::kBlockVanity),
                        Settings::GetSingleton().performanceVanityCamera,
                        static_cast<unsigned>(F::kRenderDuringTM),
                        static_cast<unsigned>(F::kCloseOnGameMenu)));
            }

            void RenderOverlay() override {
                auto& feed = EngineFeed::GetSingleton();
                if (!feed.active.load(std::memory_order_acquire)) return;
                if (!_r.Ready()) return;
                const ImVec2 disp = FUCK::GetDisplaySize();
                if (disp.x <= 0.0f || disp.y <= 0.0f) return;
                const hw::View  view{ disp.x, disp.y };
                const hw::Style st = hw::Style::Default();
                const double    lookahead = std::clamp(
                    Settings::GetSingleton().highwayLookaheadSec, 0.3, 5.0);

                // Whole-frame read under the feed lock (plan decision 2);
                // emission is CPU draw-list appends, ~tens of us total.
                std::scoped_lock lk(feed.mx);
                if (!feed.song || !feed.clock || !feed.engine) return;
                const double raw    = QpcSec();
                const double visual = feed.clock->VisualTime(raw);
                const auto&  chart  = feed.song->chart;
                const auto&  stats  = feed.engine->Stats();
                const bool   paused = feed.clock->Paused();
                const bool   rich   = Settings::GetSingleton().richFx;

                // particle clock: visual-time deltas so pause freezes fx
                // with the song; a backwards jump = new session/rebase
                double dt = visual - _prevVisual;
                if (dt < 0.0 || _prevVisual < -1e8) dt = 0.0;
                _prevVisual = visual;
                _fx.Update(paused ? 0.0 : std::min(dt, 0.1));
                _beatPulse = 0.0f;
                if (rich) {
                    const double tb = hw::LastBeatTime(
                        chart.tempo, chart.offsetSeconds, visual);
                    _beatPulse = static_cast<float>(
                        std::exp(-(visual - tb) / 0.25));
                }

                DrawSurface(st, view, stats.spActive,
                            rich ? stats.combo : 0);
                DrawBeatLines(st, view, chart, visual, lookahead,
                              stats.spActive);
                DrawTrails(st, view, feed, chart, visual, lookahead,
                           stats.spActive);
                if (rich) {
                    DrawApproachGlows(st, view, feed, chart, visual,
                                      lookahead, stats.spActive);
                }
                DrawStrikeline(st, view, feed, rich, stats.spActive);
                DrawGems(st, view, feed, chart, visual, lookahead,
                         stats.spActive);
                DrawFlashes(st, view, feed, chart, visual, lookahead, rich);
                if (rich) {
                    DrawSustainFlames(st, view, feed, chart, visual,
                                      stats.spActive);
                    UpdateMissFlash(st, view, feed, visual);
                    UpdateSpGlints(st, view, stats.spActive, visual);
                    UpdateEdgeArcs(st, view, stats.spActive, visual);
                    DrawParticles();
                }
                DrawBoltStrike(st, view, visual);
                // Streak banners are suppressed in practice along with the
                // score and Glory panels: they celebrate a run that is
                // deliberately not being recorded, and the practice strip is
                // the one piece of chrome the mode wants.
                if (!UiBus::GetSingleton().practiceActive.load(
                        std::memory_order_acquire)) {
                    DrawBanners(view, stats.combo, stats.spActive, feed,
                                visual, paused);
                }
                if (paused) DrawPauseDim(st, view);
            }

        private:
            void DrawSurface(const hw::Style& st, const hw::View& v,
                             bool sp, int combo) {
                const float cx = v.w * 0.5f;
                // combo streak: rails lerp toward white as the streak grows
                const float streak =
                    0.6f * (std::min(combo, 50) / 50.0f);
                for (int i = 0; i < st.strips; ++i) {
                    const float z0 = static_cast<float>(i) / st.strips;
                    const float z1 = static_cast<float>(i + 1) / st.strips;
                    const float a  = 0.10f + 0.55f * (1.0f - z0);
                    const hw::V2 p[4] = {
                        { cx - hw::HalfWOf(st, v, z1), hw::YOf(st, v, z1) },
                        { cx + hw::HalfWOf(st, v, z1), hw::YOf(st, v, z1) },
                        { cx + hw::HalfWOf(st, v, z0), hw::YOf(st, v, z0) },
                        { cx - hw::HalfWOf(st, v, z0), hw::YOf(st, v, z0) },
                    };
                    // Active Star Power floods the surface cyan.
                    _r.QuadFilled(p, sp ? hw::RGBA{ 0.02f, 0.14f, 0.17f, a }
                                        : hw::RGBA{ 0.05f, 0.05f, 0.09f, a });
                    // Edge rails share the same cyan activation language.
                    hw::RGBA rail =
                        sp ? hw::RGBA{ 0.10f, 0.92f, 1.00f, 0.92f }
                           : hw::RGBA{ 0.55f, 0.75f, 0.95f, 0.75f };
                    rail.r += (1.0f - rail.r) * streak;
                    rail.g += (1.0f - rail.g) * streak;
                    rail.b += (1.0f - rail.b) * streak;
                    for (int side = -1; side <= 1; side += 2) {
                        const float e0 =
                            cx + side * hw::HalfWOf(st, v, z0);
                        const float e1 =
                            cx + side * hw::HalfWOf(st, v, z1);
                        const float w0 = std::max(
                            2.0f, 0.03f * hw::HalfWOf(st, v, z0));
                        const float w1 = std::max(
                            2.0f, 0.03f * hw::HalfWOf(st, v, z1));
                        const hw::V2 rp[4] = {
                            { e1 - w1, hw::YOf(st, v, z1) },
                            { e1 + w1, hw::YOf(st, v, z1) },
                            { e0 + w0, hw::YOf(st, v, z0) },
                            { e0 - w0, hw::YOf(st, v, z0) },
                        };
                        hw::RGBA c = rail;
                        c.a *= 1.0f - 0.5f * z0;
                        _r.QuadFilled(rp, c);
                    }
                }
            }

            void DrawBeatLines(const hw::Style& st, const hw::View& v,
                               const bard::ParsedChart& chart, double visual,
                               double lookahead, bool spActive) {
                hw::CollectBeatLines(chart.tempo, chart.timeSigs,
                                     chart.offsetSeconds, visual,
                                     visual + lookahead, _beats);
                const float cx = v.w * 0.5f;
                for (const auto& bl : _beats) {
                    const float z = hw::ZOf((bl.time - visual) / lookahead,
                                            st.depthGain);
                    const float y  = hw::YOf(st, v, z);
                    const float hwd = hw::HalfWOf(st, v, z);
                    const float th = bl.measure ? 3.0f : 1.5f;
                    const hw::V2 p[4] = { { cx - hwd, y - th },
                                          { cx + hwd, y - th },
                                          { cx + hwd, y + th },
                                          { cx - hwd, y + th } };
                    hw::RGBA c = spActive
                        ? hw::kSpActiveCyan
                        : hw::RGBA{ 1.0f, 1.0f, 1.0f, 1.0f };
                    c.a = (bl.measure ? 0.34f : 0.18f) *
                          (1.0f - 0.5f * z);
                    _r.QuadFilled(p, c);
                }
            }

            void DrawTrails(const hw::Style& st, const hw::View& v,
                            EngineFeed& feed, const bard::ParsedChart& chart,
                            double visual, double lookahead, bool spActive) {
                const auto range = hw::VisibleNotes(
                    chart.notes, visual, lookahead, st.tailSec,
                    feed.maxSustainSec);
                for (std::size_t i = range.first; i < range.last; ++i) {
                    const auto& n = chart.notes[i];
                    std::uint8_t act = 0, drop = 0;
                    feed.engine->SustainMasks(i, act, drop);
                    const auto j = feed.engine->JudgmentOf(i);
                    const bool phraseAvailable =
                        feed.engine->SpPhraseAvailableFor(i);
                    for (int lane = 0; lane < bard::kLaneCount; ++lane) {
                        if (n.sustainTicks[lane] == 0) continue;
                        const double endT = n.sustainEnd[lane];
                        if (endT <= visual - st.tailSec) continue;
                        const std::uint8_t bit  = bard::LaneBit(lane);
                        const bool         held = (act & bit) != 0;
                        const bool dead =
                            j == bard::Judgment::kMissed ||
                            ((drop & bit) && !held && endT > visual);
                        hw::RGBA tint = hw::VisualNoteColor(
                            n, lane, dead, spActive, phraseAvailable);
                        tint.a = held ? 0.95f : 0.70f;
                        // Whammy feedback: held trails breathe while the
                        // whammy is fresh. Visual time stands in for
                        // input time (a few ms against a 250ms window).
                        // The envelope interpolates so pressing and
                        // releasing swells and settles instead of
                        // snapping (field ask): ~70ms attack, ~200ms
                        // release. Statics are render-thread-only, one
                        // update per frame via the visual-time guard.
                        static float  s_whammyEnv  = 0.0f;
                        static double s_whammyPrev = -1.0;
                        if (visual != s_whammyPrev) {
                            const float target =
                                feed.engine->WhammyRecentIn(visual)
                                    ? 1.0f : 0.0f;
                            const float dt = static_cast<float>(
                                std::clamp(visual - s_whammyPrev,
                                           0.0, 0.1));
                            const float k =
                                target > s_whammyEnv ? 14.0f : 5.0f;
                            s_whammyEnv += (target - s_whammyEnv)
                                * (1.0f - std::exp(-k * dt));
                            s_whammyPrev = visual;
                        }
                        const float wob = held ? s_whammyEnv : 0.0f;
                        hw::EmitTrail(st, v, lane,
                                      (n.time - visual) / lookahead,
                                      (endT - visual) / lookahead, held,
                                      tint, _r,
                                      static_cast<float>(visual * 60.0),
                                      wob);
                    }
                }
            }

            void DrawStrikeline(const hw::Style& st, const hw::View& v,
                                EngineFeed& feed, bool rich, bool spActive) {
                const float cx = v.w * 0.5f;
                const float y  = hw::YOf(st, v, 0.0f);
                const float hwd = hw::HalfWOf(st, v, 0.0f);
                const hw::V2 bar[4] = { { cx - hwd, y - 2.0f },
                                        { cx + hwd, y - 2.0f },
                                        { cx + hwd, y + 2.0f },
                                        { cx - hwd, y + 2.0f } };
                // the strikeline breathes with the song (beat pulse)
                hw::RGBA line = spActive
                    ? hw::kSpActiveCyan
                    : hw::RGBA{ 1.0f, 1.0f, 1.0f, 1.0f };
                line.a = 0.45f + 0.25f * _beatPulse;
                _r.QuadFilled(bar, line);
                const std::uint8_t held = feed.heldFrets.load(
                    std::memory_order_relaxed);
                const float sp = hw::LaneSpacing(st, v, 0.0f);
                for (int lane = 0; lane < 5; ++lane) {
                    const float x  = hw::LaneX(st, v, lane, 0.0f);
                    const bool  down = (held >> lane) & 1;
                    // press kick: a small ring on the make edge
                    if (rich && down && !((_prevHeldBits >> lane) & 1)) {
                        _fx.PressKick(
                            x, y,
                            spActive ? hw::kSpActiveCyan
                                     : hw::kLaneColors[lane],
                            1.0f);
                    }
                    // pressed frets depress slightly; idle frets get the
                    // beat pulse in their alpha
                    const float press = down ? 0.93f : 1.0f;
                    const float hx = st.gemHalfW * sp * 1.15f * press;
                    const float hy = hx * st.gemSquash;
                    if (rich && down) {  // under-glow while held
                        const float gx = hx * 1.9f;
                        const hw::V2 gp[4] = { { x - gx, y - gx },
                                               { x + gx, y - gx },
                                               { x + gx, y + gx },
                                               { x - gx, y + gx } };
                        hw::RGBA g = spActive ? hw::kSpActiveCyan
                                              : hw::kLaneColors[lane];
                        g.a        = 0.35f;
                        _r.Quad(hw::Sprite::kGlowDot, gp, g);
                    }
                    const hw::V2 p[4] = { { x - hx, y - hy },
                                          { x + hx, y - hy },
                                          { x + hx, y + hy },
                                          { x - hx, y + hy } };
                    hw::RGBA c = spActive ? hw::kSpActiveCyan
                                          : hw::kLaneColors[lane];
                    c.a        = down ? 1.0f
                                      : 0.8f + 0.15f * _beatPulse;
                    _r.Quad(down ? hw::Sprite::kFretPressed
                                 : hw::Sprite::kFretRing,
                            p, c);
                }
                _prevHeldBits = held;
            }

            void DrawGems(const hw::Style& st, const hw::View& v,
                          EngineFeed& feed, const bard::ParsedChart& chart,
                          double visual, double lookahead, bool spActive) {
                const auto range = hw::VisibleNotes(
                    chart.notes, visual, lookahead, st.tailSec,
                    feed.maxSustainSec);
                // far-to-near so near gems paint on top
                for (std::size_t i = range.last; i-- > range.first;) {
                    const auto&  n = chart.notes[i];
                    const double u = (n.time - visual) / lookahead;
                    // VisibleNotes' 1.05 spawn margin reaches past the
                    // horizon (ZOf ~1.022); skip - don't clamp, clamping
                    // would pile dense passages onto the horizon line.
                    if (u > 1.0) continue;
                    if (n.time - visual < -st.tailSec) continue;
                    const auto j = feed.engine->JudgmentOf(i);
                    const bool phraseAvailable =
                        feed.engine->SpPhraseAvailableFor(i);
                    hw::EmitGem(st, v, n, hw::ZOf(u, st.depthGain),
                                static_cast<int>(j), spActive,
                                phraseAvailable, _r);
                }
            }

            void DrawFlashes(const hw::Style& st, const hw::View& v,
                             EngineFeed& feed,
                             const bard::ParsedChart& chart, double visual,
                             double lookahead, bool rich) {
                const auto& stats = feed.engine->Stats();
                if (stats.notesHit < _prevHit) {  // new session
                    _prevHit        = stats.notesHit;
                    _lastFlashedIdx = -1;
                    // -1e9, NOT 0: lead-in visual time is NEGATIVE, so a
                    // 0.0 "inactive" timer reads as a live effect with a
                    // huge unclamped envelope for the whole countdown
                    // (field 2026-07-25: solid-white highway + phantom
                    // bolt at song start)
                    for (auto& f : _flashUntil) f = -1e9;
                    _fx.Clear();
                    _prevMissish    = 0;
                    _missFlashUntil = -1e9;
                    _prevHeldBits   = 0;
                    _prevVisual     = -1e9;
                    _nextGlintAt    = 0.0;
                    for (auto& s : _sustainSpawnAt) s = 0.0;
                    _loggedFlameBurst = false;
                    _loggedFountain   = false;
                    _boltUntil        = -1e9;
                    _boltFloodUntil   = -1e9;
                    for (int k = 0; k < 2; ++k) {
                        _arcUntil[k]  = -1e9;
                        _nextArcAt[k] = 0.0;
                    }
                }
                const float yStrike = hw::YOf(st, v, 0.0f);
                const float spScale =
                    stats.spActive ? 1.35f : 1.0f;
                // burst flames size from the gem face, not fixed px
                // (field 2026-07-25: px constants read thin at high res)
                const float gemW = 2.0f * st.gemHalfW *
                                   hw::LaneSpacing(st, v, 0.0f);
                // P3: phrase completion fires the lightning strike +
                // white flood + strikeline flash (the meter zap lives in
                // the HUD, watermarking the same counter). Assignment
                // (not ratchet) so a new session's reset counter simply
                // re-arms the watermark.
                if (stats.spPhrasesCompleted > _prevPhrases &&
                    _prevPhrases >= 0) {
                    _boltUntil      = visual + 0.40;
                    _boltFloodUntil = visual + 0.12;
                    _boltSeedBase   = static_cast<std::uint32_t>(
                                          stats.spPhrasesCompleted) *
                                      7919u;
                    for (auto& f : _flashUntil) f = visual + 0.14;
                    spdlog::info("[fx] bolt phrase={}",
                                 stats.spPhrasesCompleted);
                    // P6 SFX: SP earned rides the same watermark as the
                    // bolt (all instruments - the magic pair is not
                    // electric-gated).
                    UiSfx::Fire(ui_sfx::Cue::kSpGain);
                }
                _prevPhrases = stats.spPhrasesCompleted;
                if (stats.notesHit > _prevHit) {
                    _prevHit = stats.notesHit;
                    // Watermark scan: light EVERY newly-hit note near the
                    // line exactly once (engine Update() can judge several
                    // notes between two render frames - chords, fast runs).
                    const auto range = hw::VisibleNotes(
                        chart.notes, visual, lookahead, 0.35,
                        feed.maxSustainSec);
                    for (std::size_t i = range.first; i < range.last; ++i) {
                        if (static_cast<std::int64_t>(i) <= _lastFlashedIdx)
                            continue;
                        if (feed.engine->JudgmentOf(i) !=
                            bard::Judgment::kHit) continue;
                        const double t = chart.notes[i].time;
                        if (t <= visual - 0.35 || t > visual + 0.2) continue;
                        const auto mask = chart.notes[i].mask;
                        // Burst color = the gem's own color at hit time
                        // (field 2026-07-25): a star-power phrase note
                        // burns CYAN even outside active SP, exactly as
                        // its gem rendered. VisualNoteColor is the one
                        // source of truth for that mapping.
                        const bool phraseAvail =
                            feed.engine->SpPhraseAvailableFor(i);
                        if (mask & bard::kOpenBit) {
                            // open note: classic full-strikeline flash
                            for (auto& f : _flashUntil) f = visual + 0.12;
                            if (rich) {  // a burst on EVERY lane
                                const hw::RGBA c = hw::VisualNoteColor(
                                    chart.notes[i], bard::kOpenLane,
                                    false, stats.spActive, phraseAvail);
                                // Lanes 0..4, not 1..3. The old loop bursted
                                // on the middle three only, which does not
                                // read as "three bursts across the width" -
                                // it reads as the green and orange lanes
                                // being BROKEN, because the strikeline flash
                                // above lights all five and only those two
                                // lanes have no particles over them (field
                                // 2026-07-26). An open note is a full-width
                                // event; every lane it spans should answer.
                                for (int l = 0; l < 5; ++l) {
                                    const float x =
                                        hw::LaneX(st, v, l, 0.0f);
                                    _fx.HitBurst(x, yStrike, c, spScale);
                                    _fx.FlameBurst(x, yStrike, gemW, c,
                                                   spScale);
                                }
                                LogFlameBurstOnce();
                            }
                        } else {
                            for (int l = 0; l < 5; ++l) {
                                if (mask & bard::LaneBit(l)) {
                                    _flashUntil[l] = visual + 0.12;
                                    if (rich) {
                                        const hw::RGBA c =
                                            hw::VisualNoteColor(
                                                chart.notes[i], l, false,
                                                stats.spActive,
                                                phraseAvail);
                                        const float x =
                                            hw::LaneX(st, v, l, 0.0f);
                                        _fx.HitBurst(x, yStrike, c,
                                                     spScale);
                                        _fx.FlameBurst(x, yStrike, gemW,
                                                       c, spScale);
                                        LogFlameBurstOnce();
                                    }
                                }
                            }
                        }
                        _lastFlashedIdx = static_cast<std::int64_t>(i);
                    }
                }
                const float y  = hw::YOf(st, v, 0.0f);
                const float sp = hw::LaneSpacing(st, v, 0.0f);
                for (int l = 0; l < 5; ++l) {
                    if (_flashUntil[l] <= visual) continue;
                    const float a = std::clamp(static_cast<float>(
                        (_flashUntil[l] - visual) / 0.12), 0.0f, 1.0f);
                    const float x  = hw::LaneX(st, v, l, 0.0f);
                    const float hx = st.gemHalfW * sp * 2.0f;
                    const hw::V2 p[4] = { { x - hx, y - hx },
                                          { x + hx, y - hx },
                                          { x + hx, y + hx },
                                          { x - hx, y + hx } };
                    _r.Quad(hw::Sprite::kFlash, p,
                            { 1.0f, 1.0f, 1.0f, 0.85f * a });
                }
                // SP shimmer: a streak sweeping horizon -> strikeline
                if (stats.spActive) {
                    const float zs = 1.0f - static_cast<float>(
                        std::fmod(visual * 0.6, 1.0));
                    const float yz  = hw::YOf(st, v, zs);
                    const float hwd = hw::HalfWOf(st, v, zs);
                    const float cx  = v.w * 0.5f;
                    const float th  = 14.0f * (1.0f - 0.5f * zs);
                    const hw::V2 p[4] = { { cx - hwd, yz - th },
                                          { cx + hwd, yz - th },
                                          { cx + hwd, yz + th },
                                          { cx - hwd, yz + th } };
                    _r.Quad(hw::Sprite::kShimmer, p,
                            { 0.10f, 0.92f, 1.00f, 0.38f });
                }
            }

            // soft lane-colored halo under gems about to reach the line
            void DrawApproachGlows(const hw::Style& st, const hw::View& v,
                                   EngineFeed& feed,
                                   const bard::ParsedChart& chart,
                                   double visual, double lookahead,
                                   bool spActive) {
                const auto range = hw::VisibleNotes(
                    chart.notes, visual, lookahead, 0.0,
                    feed.maxSustainSec);
                const float sp = hw::LaneSpacing(st, v, 0.0f);
                for (std::size_t i = range.first; i < range.last; ++i) {
                    const auto&  n = chart.notes[i];
                    const double u = (n.time - visual) / lookahead;
                    const float  g = hw::ApproachGlow(u);
                    if (g <= 0.0f || u < 0.0) continue;
                    if (feed.engine->JudgmentOf(i) !=
                        bard::Judgment::kPending) continue;
                    const float z = hw::ZOf(u, st.depthGain);
                    const float y = hw::YOf(st, v, z);
                    const bool phraseAvailable =
                        feed.engine->SpPhraseAvailableFor(i);
                    for (int l = 0; l < 5; ++l) {
                        if (!(n.mask & bard::LaneBit(l))) continue;
                        const float x  = hw::LaneX(st, v, l, z);
                        const float hx = st.gemHalfW * sp * 1.6f;
                        const hw::V2 p[4] = { { x - hx, y - hx },
                                              { x + hx, y - hx },
                                              { x + hx, y + hx },
                                              { x - hx, y + hx } };
                        hw::RGBA c = hw::VisualNoteColor(
                            n, l, false, spActive, phraseAvailable);
                        c.a        = 0.5f * g;
                        _r.Quad(hw::Sprite::kGemUnderGlow, p, c);
                    }
                }
            }

            // energy plume at the contact point of every held sustain,
            // plus a trickle of sparks
            void DrawSustainFlames(const hw::Style& st, const hw::View& v,
                                   EngineFeed& feed,
                                   const bard::ParsedChart& chart,
                                   double visual, bool spActive) {
                const auto range = hw::VisibleNotes(
                    chart.notes, visual, 0.0, st.tailSec,
                    feed.maxSustainSec);
                const float y  = hw::YOf(st, v, 0.0f);
                const float sp = hw::LaneSpacing(st, v, 0.0f);
                for (std::size_t i = range.first; i < range.last; ++i) {
                    const auto& n = chart.notes[i];
                    if (n.time > visual) continue;
                    std::uint8_t act = 0, drop = 0;
                    feed.engine->SustainMasks(i, act, drop);
                    if (!act) continue;
                    const bool phraseAvailable =
                        feed.engine->SpPhraseAvailableFor(i);
                    for (int l = 0; l < bard::kLaneCount; ++l) {
                        if (!(act & bard::LaneBit(l))) continue;
                        if (n.sustainEnd[l] <= visual) continue;
                        const bool  open = l == bard::kOpenLane;
                        const float x =
                            open ? v.w * 0.5f : hw::LaneX(st, v, l, 0.0f);
                        const hw::RGBA c = hw::VisualNoteColor(
                            n, open ? bard::kOpenLane : l, false, spActive,
                            phraseAvailable);
                        // wobbling contact plume, now flipbook-cycled
                        // (spec P2): ~13fps with a per-lane phase so
                        // chords don't lick in lockstep. 1.4x face
                        // (field 2026-07-25: right look, too small).
                        const float w =
                            st.gemHalfW * sp * 1.4f *
                            (1.0f + 0.12f * static_cast<float>(std::sin(
                                                visual * 24.0 + l * 1.7)));
                        const float h = w * 2.6f;
                        const hw::V2 p[4] = { { x - w, y - h },
                                              { x + w, y - h },
                                              { x + w, y + w * 0.4f },
                                              { x - w, y + w * 0.4f } };
                        hw::RGBA fc = c;
                        fc.a        = 0.85f;
                        const int frame = static_cast<int>(
                            visual * 13.0 + l * 1.3) % hw::kFlameFbFrames;
                        _r.Quad(static_cast<hw::Sprite>(
                                    static_cast<int>(
                                        hw::Sprite::kFlameFb0) + frame),
                                p, fc);
                        if (visual >= _sustainSpawnAt[l]) {
                            _fx.SustainFountain(x, y, c, 1.0f);
                            _sustainSpawnAt[l] = visual + 0.045;
                            LogFountainOnce();
                        }
                    }
                }
            }

            // brief red under-flash on any new miss/overstrum (the audio
            // buzz carries the punch; this is the visual echo)
            void UpdateMissFlash(const hw::Style& st, const hw::View& v,
                                 EngineFeed& feed, double visual) {
                const auto& stats = feed.engine->Stats();
                const long  missish =
                    stats.notesMissed + stats.overstrums;
                if (missish > _prevMissish) {
                    _missFlashUntil = visual + 0.15;
                }
                _prevMissish = missish;  // decrease = new session, benign
                if (_missFlashUntil <= visual) return;
                const float a = std::clamp(static_cast<float>(
                    (_missFlashUntil - visual) / 0.15), 0.0f, 1.0f);
                const float cx  = v.w * 0.5f;
                const float y   = hw::YOf(st, v, 0.0f);
                const float hwd = hw::HalfWOf(st, v, 0.0f) * 1.15f;
                const float hy  = hwd * 0.30f;
                const hw::V2 p[4] = { { cx - hwd, y - hy },
                                      { cx + hwd, y - hy },
                                      { cx + hwd, y + hy },
                                      { cx - hwd, y + hy } };
                _r.Quad(hw::Sprite::kGlowDot, p,
                        { 0.90f, 0.15f, 0.10f, 0.40f * a });
            }

            void UpdateSpGlints(const hw::Style& st, const hw::View& v,
                                bool spActive, double visual) {
                if (!spActive || visual < _nextGlintAt) return;
                // deterministic drift across the near half of the highway
                const float t =
                    static_cast<float>(std::fmod(visual * 7.31, 1.0));
                const float z   = 0.15f + 0.45f * t;
                const float cx  = v.w * 0.5f;
                const float hwd = hw::HalfWOf(st, v, z);
                const float x   = cx - hwd +
                                2.0f * hwd *
                                    static_cast<float>(
                                        std::fmod(visual * 3.17, 1.0));
                _fx.SpGlint(x, hw::YOf(st, v, z), 1.0f);
                _nextGlintAt = visual + 0.22;
            }

            void DrawParticles() {
                _fx.ForEach([this](const hw::FxParticle& q) {
                    const float t = q.age / q.life;
                    const float s =
                        q.size0 + (q.size1 - q.size0) * t;
                    hw::RGBA c = q.tint;
                    c.a *= 1.0f - t;
                    hw::Sprite spr = q.sprite;
                    if (q.fbFrames > 1) {
                        spr = static_cast<hw::Sprite>(
                            static_cast<int>(q.sprite) +
                            hw::FlipbookFrame(t, q.fbFrames));
                    }
                    if (q.sprite == hw::Sprite::kSpark ||
                        q.sprite == hw::Sprite::kNeedle) {
                        // oriented quad: +x of the sprite = velocity dir
                        const float ca = std::cos(q.rot), sa = std::sin(q.rot);
                        const float el =
                            q.sprite == hw::Sprite::kNeedle ? 2.4f : 1.6f;
                        const float ex = s * el, ey = s * 0.5f;
                        const hw::V2 p[4] = {
                            { q.x - ex * ca + ey * sa, q.y - ex * sa - ey * ca },
                            { q.x + ex * ca + ey * sa, q.y + ex * sa - ey * ca },
                            { q.x + ex * ca - ey * sa, q.y + ex * sa + ey * ca },
                            { q.x - ex * ca - ey * sa, q.y - ex * sa + ey * ca },
                        };
                        _r.Quad(spr, p, c);
                    } else if (q.fbFrames > 1) {
                        // flame tongues: upright plumes with the BASE
                        // anchored at q.y - GH flames rise from the
                        // fret, they do not straddle it
                        const float ex = s, eh = s * 3.4f;
                        const hw::V2 p[4] = { { q.x - ex, q.y - eh },
                                              { q.x + ex, q.y - eh },
                                              { q.x + ex, q.y },
                                              { q.x - ex, q.y } };
                        _r.Quad(spr, p, c);
                    } else {
                        const hw::V2 p[4] = { { q.x - s, q.y - s },
                                              { q.x + s, q.y - s },
                                              { q.x + s, q.y + s },
                                              { q.x - s, q.y + s } };
                        _r.Quad(spr, p, c);
                    }
                });
            }

            // GH-feel P1.5: banner strips as center-pivot rotated quads.
            // FLICK cannot rotate live text, but pre-rendered atlas strips
            // rotate like any quad. Envelope: scale 0->overshoot->rest
            // with a spin settling level, brief wiggle, spin-shrink out.
            // Replaces the retired HudJuiceBanner text window.
            static void BannerEnvelope(double t, double life, float& scale,
                                       float& rot, float& alpha) {
                const float in =
                    std::min(1.0f, static_cast<float>(t / 0.13));
                const float ease = in * in * (3 - 2 * in);
                scale = ease * (1.0f + 0.35f * static_cast<float>(
                    std::exp(-t * 9.0)));
                rot = -0.9f * (1.0f - ease)
                    + 0.07f * static_cast<float>(
                          std::exp(-t * 3.5) * std::sin(t * 24.0));
                alpha = 1.0f;
                const double left = life - t;
                if (left < 0.3) {
                    // Outro is a plain fade (field 2026-07-25): the
                    // spin-shrink exit read as fussy next to the strong
                    // entrance.
                    alpha = static_cast<float>(left / 0.3);
                }
            }

            void DrawStrip(const hw::UvRect& uv, float cellsWide, float cx,
                           float cy, float h, float rot,
                           const hw::RGBA& tint) {
                if (h <= 0.5f) return;
                const float w = h * cellsWide;
                const float ca = std::cos(rot), sa = std::sin(rot);
                auto pt = [&](float lx, float ly) {
                    return hw::V2{ cx + lx * ca - ly * sa,
                                   cy + lx * sa + ly * ca };
                };
                const hw::V2 p[4] = { pt(-w * 0.5f, -h * 0.5f),
                                      pt(w * 0.5f, -h * 0.5f),
                                      pt(w * 0.5f, h * 0.5f),
                                      pt(-w * 0.5f, h * 0.5f) };
                _r.QuadUv(uv, p, tint);
            }

            void DrawNumber(int value, float cx, float cy, float h,
                            float rot, const hw::RGBA& tint) {
                char buf[12];
                const int len =
                    std::snprintf(buf, sizeof(buf), "%d", value);
                if (len <= 0) return;
                const float adv = h * 0.60f;
                const float total = adv * static_cast<float>(len - 1);
                const float ca = std::cos(rot), sa = std::sin(rot);
                for (int i = 0; i < len; ++i) {
                    const float lx = -total * 0.5f
                        + static_cast<float>(i) * adv;
                    // digit slots rotate around the shared pivot
                    const auto uv = hw::UvOf(static_cast<hw::Sprite>(
                        static_cast<int>(hw::Sprite::kDigit0)
                        + (buf[i] - '0')));
                    DrawStrip(uv, 1.0f, cx + lx * ca, cy + lx * sa, h,
                              rot, tint);
                }
            }

            void DrawBanners(const hw::View& v, int combo, bool spActive,
                             EngineFeed& feed, double visual,
                             bool paused) {
                // Banner clocks run on REAL time, so a pause used to
                // either tick them away behind the dim or (with the old
                // !paused gate) hide them outright - the field-reported
                // "elements hidden on pause". Freeze the clock instead:
                // while paused, "now" pins to the pause moment; on
                // resume every armed timestamp shifts by the paused
                // span. Banners then render frozen and dim with the
                // world.
                double now = FUCK::GetTime();
                if (paused) {
                    if (_bPausedAt < 0.0) { _bPausedAt = now; }
                    now = _bPausedAt;
                } else if (_bPausedAt >= 0.0) {
                    const double d = now - _bPausedAt;
                    if (_bMilestoneAt > 0.0) { _bMilestoneAt += d; }
                    if (_bActiveAt > 0.0) { _bActiveAt += d; }
                    if (_bReadySince > 0.0) { _bReadySince += d; }
                    _bPausedAt = -1.0;
                }
                if (combo >= 50 && combo / 50 > _bLastCombo / 50) {
                    _bMilestone   = (combo / 50) * 50;
                    _bMilestoneAt = now;
                    spdlog::info("[fx] streak banner {}", _bMilestone);
                    // P6 SFX: the whoosh+thump on every streak slam-in
                    // (the request folds the cut milestone cue into
                    // banner_pop). The SP banners stay silent here: the
                    // activation moment belongs to sp_activate below,
                    // and the ready banner eases in rather than slamming
                    // (its cue was cut).
                    UiSfx::Fire(ui_sfx::Cue::kBannerPop);
                }
                _bLastCombo = combo;
                const bool ready = !spActive
                    && feed.engine->SpGaugeFraction(visual) >= 0.5;
                if (ready && !_bWasReady) {
                    _bReadySince = now;
                    spdlog::info("[fx] sp ready banner shown");
                }
                _bWasReady = ready;
                if (spActive && !_bWasActive) {
                    _bActiveAt = now;
                    spdlog::info("[fx] sp activation flash");
                    // P6 SFX: SP fired (all instruments). 2.6s recorded
                    // impact - deliberately the biggest cue in the bank,
                    // and it also covers the activation banner's slam.
                    UiSfx::Fire(ui_sfx::Cue::kSpActivate);
                }
                _bWasActive = spActive;

                const float H  = v.h * 0.075f;
                const float cx = v.w * 0.5f;
                const float cy = v.h * 0.185f;
                const double tm = now - _bMilestoneAt;
                if (_bMilestoneAt > 0.0 && tm < 2.2) {
                    float scale, rot, alpha;
                    BannerEnvelope(tm, 2.2, scale, rot, alpha);
                    const hw::RGBA gold{ 0.98f, 0.80f, 0.24f, alpha };
                    DrawNumber(_bMilestone, cx, cy - H * 0.55f,
                               H * 1.15f * scale, rot, gold);
                    DrawStrip(hw::BannerStreakUv(), 6.0f, cx,
                              cy + H * 0.62f, H * scale, rot, gold);
                    return;
                }
                const double ta = now - _bActiveAt;
                if (_bActiveAt > 0.0 && ta < 1.5) {
                    float scale, rot, alpha;
                    BannerEnvelope(ta, 1.5, scale, rot, alpha);
                    DrawStrip(hw::BannerActiveUv(), 6.0f, cx, cy,
                              H * 1.25f * scale, rot,
                              hw::RGBA{ 0.62f, 0.97f, 1.0f, alpha });
                    return;
                }
                if (ready) {
                    const double t = now - _bReadySince;
                    const float in =
                        std::min(1.0f, static_cast<float>(t / 0.14));
                    const float ease = in * in * (3 - 2 * in);
                    const float scale = ease
                        * (1.0f + 0.05f * static_cast<float>(
                               std::sin(t * 6.9)));
                    const float rot =
                        0.05f * static_cast<float>(std::sin(t * 2.2))
                        + 0.5f * static_cast<float>(std::exp(-t * 7.0));
                    DrawStrip(hw::BannerReadyUv(), 8.0f, cx, cy,
                              H * scale, rot,
                              hw::RGBA{ 0.55f, 0.95f, 1.0f, 0.95f });
                }
            }

            // P3: chained oriented bolt quads through the given nodes.
            // THREE layers per segment (field round 2 - two thin layers
            // read cheap): a wide faint outer bloom, a mid pale-cyan
            // glow, and the white-hot core. w0/w1 = core half-width at
            // the first/last node. Slight along-axis overlap hides the
            // joints; the cell art's end fade blends them.
            void DrawBoltChain(const hw::V2* nd, int count, float w0,
                               float w1, float alpha) {
                for (int i = 0; i + 1 < count; ++i) {
                    const float dx  = nd[i + 1].x - nd[i].x;
                    const float dy  = nd[i + 1].y - nd[i].y;
                    const float len = std::sqrt(dx * dx + dy * dy);
                    if (len < 1e-3f) { continue; }
                    const float ux = dx / len, uy = dy / len;
                    const float mx = (nd[i].x + nd[i + 1].x) * 0.5f;
                    const float my = (nd[i].y + nd[i + 1].y) * 0.5f;
                    const float ex = len * 0.5f * 1.15f;
                    const float t  = (static_cast<float>(i) + 0.5f) /
                                     static_cast<float>(count - 1);
                    const float w  = w0 + (w1 - w0) * t;
                    // round 4: fatter core layer - with the cell's core
                    // at 16% of cell height the white line is now
                    // ~0.3x the layer thickness, so 1.4x here lands a
                    // solid multi-pixel core at every distance
                    constexpr float kLayerW[3] = { 6.0f, 3.0f, 1.4f };
                    const hw::RGBA kLayerC[3] = {
                        { 0.30f, 0.65f, 1.00f, 0.16f * alpha },
                        { 0.55f, 0.90f, 1.00f, 0.45f * alpha },
                        { 1.00f, 1.00f, 1.00f, 0.98f * alpha },
                    };
                    for (int layer = 0; layer < 3; ++layer) {
                        const float ey = w * kLayerW[layer];
                        const hw::V2 p[4] = {
                            { mx - ex * ux + ey * uy,
                              my - ex * uy - ey * ux },
                            { mx + ex * ux + ey * uy,
                              my + ex * uy - ey * ux },
                            { mx + ex * ux - ey * uy,
                              my + ex * uy + ey * ux },
                            { mx - ex * ux - ey * uy,
                              my - ex * uy + ey * ux },
                        };
                        _r.Quad(hw::BoltSprite(i), p, kLayerC[layer]);
                    }
                }
            }

            // P3: phrase-complete strike - a crackling bolt down the
            // highway center (nodes re-jitter every 60ms) with branch
            // forks and a landing impact, plus a brief white flood over
            // the surface. Field round 2: thicker 3-layer rendering,
            // finer jags, forks - the thin 2-layer bolt read cheap.
            void DrawBoltStrike(const hw::Style& st, const hw::View& v,
                                double visual) {
                if (_boltFloodUntil > visual) {
                    const float fa = std::clamp(static_cast<float>(
                        (_boltFloodUntil - visual) / 0.12), 0.0f, 1.0f);
                    const float cx = v.w * 0.5f;
                    const hw::V2 p[4] = {
                        { cx - hw::HalfWOf(st, v, 1.0f),
                          hw::YOf(st, v, 1.0f) },
                        { cx + hw::HalfWOf(st, v, 1.0f),
                          hw::YOf(st, v, 1.0f) },
                        { cx + hw::HalfWOf(st, v, 0.0f),
                          hw::YOf(st, v, 0.0f) },
                        { cx - hw::HalfWOf(st, v, 0.0f),
                          hw::YOf(st, v, 0.0f) },
                    };
                    _r.QuadFilled(p,
                                  { 1.0f, 1.0f, 1.0f, 0.30f * fa });
                }
                if (_boltUntil <= visual) { return; }
                const float rem = std::clamp(static_cast<float>(
                    (_boltUntil - visual) / 0.40), 0.0f, 1.0f);
                const std::uint32_t tick = static_cast<std::uint32_t>(
                    std::max(0.0, _boltUntil - visual) / 0.06);
                float alpha = std::min(1.0f, rem * 2.2f);
                if (tick % 2 == 1) { alpha *= 0.80f; }  // crackle
                constexpr int kNodes = 13;
                float offs[kNodes];
                hw::BoltOffsets(_boltSeedBase + tick * 101u, 1.0f, offs,
                                kNodes);
                const float cx = v.w * 0.5f;
                hw::V2 nd[kNodes];
                for (int i = 0; i < kNodes; ++i) {
                    const float z = 1.0f - static_cast<float>(i) /
                                               (kNodes - 1);
                    nd[i] = { cx + offs[i] * 0.55f *
                                       hw::HalfWOf(st, v, z),
                              hw::YOf(st, v, z) };
                }
                DrawBoltChain(nd, kNodes, 6.0f, 14.0f, alpha);
                // branch forks: short thinner chains peeling off two
                // mid-nodes, re-rolled with the same crackle tick
                for (int b = 0; b < 2; ++b) {
                    const int at = 3 + b * 4;  // nodes 3 and 7
                    float boffs[4];
                    hw::BoltOffsets(
                        _boltSeedBase + tick * 101u + 31u * (b + 1),
                        1.0f, boffs, 4);
                    const float side = b == 0 ? -1.0f : 1.0f;
                    const float z0 = 1.0f - static_cast<float>(at) /
                                                (kNodes - 1);
                    hw::V2 bn[4];
                    for (int i = 0; i < 4; ++i) {
                        const float f = static_cast<float>(i) / 3.0f;
                        const float z = z0 - 0.10f * f;
                        const float reach =
                            side * 0.45f * f * hw::HalfWOf(st, v, z);
                        bn[i] = { nd[at].x + reach +
                                      boffs[i] * 0.18f *
                                          hw::HalfWOf(st, v, z),
                                  hw::YOf(st, v, std::max(0.0f, z)) };
                    }
                    DrawBoltChain(bn, 4, 3.0f, 5.5f, alpha * 0.65f);
                }
                // landing impact at the strikeline: hot core + ring
                {
                    const float y  = hw::YOf(st, v, 0.0f);
                    const float ir = (30.0f + 90.0f * (1.0f - rem)) *
                                     (v.h / 1440.0f);
                    const float g  = ir * 2.2f;
                    const hw::V2 pg[4] = { { cx - g, y - g },
                                           { cx + g, y - g },
                                           { cx + g, y + g },
                                           { cx - g, y + g } };
                    _r.Quad(hw::Sprite::kGlowDot, pg,
                            { 0.75f, 0.95f, 1.0f, 0.55f * alpha });
                    const float rr = ir * 3.0f;
                    const hw::V2 pr[4] = { { cx - rr, y - rr },
                                           { cx + rr, y - rr },
                                           { cx + rr, y + rr },
                                           { cx - rr, y + rr } };
                    _r.Quad(hw::Sprite::kRing, pr,
                            { 0.55f, 0.90f, 1.0f, 0.50f * alpha });
                }
            }

            // P3: SP-active ambience - short crackling arcs riding the
            // highway edge rails on a random 0.4-1.2s cadence per side
            // (deterministic time hashing, SpGlints precedent).
            void UpdateEdgeArcs(const hw::Style& st, const hw::View& v,
                                bool spActive, double visual) {
                if (!spActive) {
                    for (int k = 0; k < 2; ++k) {
                        _arcUntil[k]  = 0.0;
                        _nextArcAt[k] = 0.0;
                    }
                    return;
                }
                const float cx = v.w * 0.5f;
                for (int k = 0; k < 2; ++k) {
                    if (visual >= _nextArcAt[k]) {
                        const float h1 = static_cast<float>(std::fmod(
                            visual * 5.13 + k * 0.37, 1.0));
                        const float h2 = static_cast<float>(std::fmod(
                            visual * 9.71 + k * 0.61, 1.0));
                        _arcZ0[k]   = 0.05f + 0.55f * h1;
                        _arcLen[k]  = 0.12f + 0.18f * h2;
                        _arcSeed[k] = static_cast<std::uint32_t>(
                                          visual * 997.0) +
                                      static_cast<std::uint32_t>(k) *
                                          7919u;
                        _arcUntil[k]  = visual + 0.16;
                        _nextArcAt[k] = visual + 0.4 + 0.8 * h2;
                    }
                    if (_arcUntil[k] <= visual) { continue; }
                    const float a = std::clamp(static_cast<float>(
                        (_arcUntil[k] - visual) / 0.16), 0.0f, 1.0f);
                    constexpr int kN = 5;
                    float offs[kN];
                    const std::uint32_t tick =
                        static_cast<std::uint32_t>(
                            (_arcUntil[k] - visual) / 0.05);
                    hw::BoltOffsets(_arcSeed[k] + tick * 53u, 1.0f, offs,
                                    kN);
                    const float side = k == 0 ? -1.0f : 1.0f;
                    hw::V2 nd[kN];
                    for (int i = 0; i < kN; ++i) {
                        const float z =
                            _arcZ0[k] +
                            _arcLen[k] * (static_cast<float>(i) /
                                          (kN - 1));
                        nd[i] = { cx + side * (hw::HalfWOf(st, v, z) +
                                               6.0f + offs[i] * 14.0f),
                                  hw::YOf(st, v, z) };
                    }
                    DrawBoltChain(nd, kN, 2.8f, 4.5f, 0.8f * a);
                }
            }

            // Field tells for the P2 gate (once per session - per-hit
            // logging would spam the log at chart density)
            void LogFlameBurstOnce() {
                if (_loggedFlameBurst) { return; }
                _loggedFlameBurst = true;
                spdlog::info("[fx] flame bursts live (first hit)");
            }
            void LogFountainOnce() {
                if (_loggedFountain) { return; }
                _loggedFountain = true;
                spdlog::info("[fx] sustain fountain live (first hold)");
            }

            void DrawPauseDim(const hw::Style& st, const hw::View& v) {
                (void)st;
                // Field 2026-07-25: the old background-list stroke died
                // the frame the ShadowPause menu opened - FLICK stops
                // compositing its background list while a game menu is on
                // the stack, so the dim flashed once and vanished. The
                // highway quad list is the surviving z-plane that still
                // sits UNDER the FLICK panels: emit the dim there as the
                // kSolid cell. Drawn last in RenderOverlay, so it covers
                // world, highway, and every effect below the panels.
                //
                // Field 2026-07-25 (second report): that window list is
                // clipped a few px inside the screen (the overlay host is
                // begun with zero padding, so the theme's WindowBorderSize
                // sets the clip inset - verified in the FUCK PDB), which
                // left a bright rim at the edges. PauseDimRects tiles the
                // dim: inner rect stays on the window list, four edge
                // strips go to the unclipped foreground list. The strips
                // draw above panels, but only within `margin` px of the
                // screen border where no readable panel content lives.
                const hw::RGBA dim{ 0.0f, 0.0f, 0.0f, 0.86f };
                // Comfortably past any sane theme border (a few px, DPI-
                // scaled) while staying a thin cosmetic ring.
                const float margin =
                    std::max(12.0f, v.h * (16.0f / 1440.0f));
                hw::DimRect rects[5];
                hw::PauseDimRects(v, margin, rects);
                const hw::V2 p[4] = {
                    rects[0].mn,
                    { rects[0].mx.x, rects[0].mn.y },
                    rects[0].mx,
                    { rects[0].mn.x, rects[0].mx.y }
                };
                _r.Quad(hw::Sprite::kSolid, p, dim);
                for (int i = 1; i < 5; ++i) {
                    _r.ScreenRectFilled(rects[i].mn, rects[i].mx, dim);
                }
            }

            hw::FlickRenderer         _r;
            std::vector<hw::BeatLine> _beats;
            int          _prevHit        = 0;
            std::int64_t _lastFlashedIdx = -1;
            // effect timers: -1e9 = inactive. NEVER 0.0 - lead-in visual
            // time is negative and 0.0 reads as a live effect there.
            double       _flashUntil[5]  = { -1e9, -1e9, -1e9, -1e9,
                                             -1e9 };
            // juice pack (render-thread only, like everything above)
            hw::FxPool   _fx;
            double       _prevVisual   = -1e9;
            float        _beatPulse    = 0.0f;
            long         _prevMissish  = 0;
            double       _missFlashUntil = -1e9;
            std::uint8_t _prevHeldBits = 0;
            double       _sustainSpawnAt[bard::kLaneCount]{};
            double       _nextGlintAt  = 0.0;
            bool         _loggedFlameBurst = false;
            bool         _loggedFountain   = false;
            // P3 bolt strike + edge arcs (render-thread only)
            int           _prevPhrases    = -1;  // -1 = arm on first read
            double        _boltUntil      = -1e9;
            double        _boltFloodUntil = -1e9;
            std::uint32_t _boltSeedBase   = 0;
            double        _nextArcAt[2]{};
            double        _arcUntil[2]    = { -1e9, -1e9 };
            float         _arcZ0[2]{};
            float         _arcLen[2]{};
            std::uint32_t _arcSeed[2]{};
            // banner state (render-thread only)
            int    _bLastCombo   = 0;
            int    _bMilestone   = 0;
            double _bMilestoneAt = -1.0;
            double _bReadySince  = 0.0;
            double _bActiveAt    = -1.0;
            double _bPausedAt    = -1.0;  // >=0 while paused (real time)
            bool   _bWasReady    = false;
            bool   _bWasActive   = false;
        };
        HighwayWindow g_highway;
    }

    void RegisterHighwayWindow() { FUCK::RegisterWindow(&g_highway); }
}
