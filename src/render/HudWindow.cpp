#include "PCH.h"
#include "render/HudWindow.h"

#include "QpcClock.h"
#include "game/EngineFeed.h"
#include "game/UiBus.h"

#include "chart/LoadSong.h"  // bard::LoadedSong - the practice strip reads
                             // feed.song->chart.sections for its range names
#include "clock/MasterClock.h"
#include "engine/GuitarEngine.h"
#include "render/HighwayLayout.h"  // CountdownValue, highway proportions
#include "render/HudLayout.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h (house ordering)

#include "FUCK_API.h"
#include "render/PanelStyle.h"  // shared modal surface + controls (summary)
#include "render/RenderUi.h"    // cursor refcount, summary modal only
#include "render/UiSound.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace SH {
    namespace {
        struct HudSnap {
            bool      valid = false, spActive = false, gloryDanger = false;
            long long score = 0;
            int       mult = 1, countdown = 0, combo = 0;
            int       spPhrases = 0;  // P3 meter zap watermark
            float     glory = 0.5f, sp = 0.0f;
        };

        HudSnap Snapshot() {
            HudSnap s;
            auto&   feed = EngineFeed::GetSingleton();
            std::scoped_lock lk(feed.mx);
            if (!feed.clock || !feed.engine) return s;
            const double raw   = QpcSec();
            const double input = feed.clock->InputTime(raw);
            const auto&  st    = feed.engine->Stats();
            s.valid     = true;
            s.score     = st.score;
            s.combo     = st.combo;
            s.mult      = st.multiplier * (st.spActive ? 2 : 1);
            s.countdown = hw::CountdownValue(input);
            auto& bus = UiBus::GetSingleton();
            s.glory = std::clamp(bus.glory.load(), 0.0f, 1.0f);
            s.gloryDanger = bus.gloryDanger.load();
            s.sp        = std::clamp(
                static_cast<float>(feed.engine->SpGaugeFraction(input)),
                0.0f, 1.0f);
            s.spActive  = st.spActive;
            s.spPhrases = st.spPhrasesCompleted;
            return s;
        }

        FUCK::WindowFlags HudFlags() {
            using F = FUCK::WindowFlags;
            // kPassInputToGame remains load-bearing: these windows are
            // decorative displays and must never become input surfaces.
            return static_cast<F>(
                static_cast<unsigned>(F::kNoDecoration) |
                static_cast<unsigned>(F::kNoBackground) |
                static_cast<unsigned>(F::kNoMove) |
                static_cast<unsigned>(F::kNoResize) |
                static_cast<unsigned>(F::kPassInputToGame) |
                static_cast<unsigned>(F::kRenderDuringTM) |
                static_cast<unsigned>(F::kCloseOnGameMenu));
        }

        bool HudOpen() {
            return EngineFeed::GetSingleton().active.load(
                std::memory_order_acquire);
        }

        // Practice records nothing, so the surfaces that exist to report what
        // a run EARNED are noise there: score, multiplier, Glory, Star Power
        // and the streak banners all describe a performance that is not
        // happening. The practice strip replaces them.
        bool PracticeHudActive() {
            return UiBus::GetSingleton().practiceActive.load(
                std::memory_order_acquire);
        }

        // ...which makes this the gate for every scoring-facing HUD window.
        bool ScoringHudOpen() { return HudOpen() && !PracticeHudActive(); }

        // FLICK windows float above the highway-list pause dim, so the
        // score/glory panels stayed bright while everything else went
        // dark (field 2026-07-25). Each panel dims ITSELF: a full-window
        // black rect as the last draw, same strength as the world dim.
        void DimIfPaused(const ImVec2& a_origin, const ImVec2& a_avail) {
            if (!UiBus::GetSingleton().worldPaused.load()) { return; }
            FUCK::DrawRectFilled(
                ImVec2(a_origin.x - 8.0f, a_origin.y - 8.0f),
                ImVec2(a_origin.x + a_avail.x + 8.0f,
                       a_origin.y + a_avail.y + 8.0f),
                ImVec4(0.0f, 0.0f, 0.0f, 0.86f), 0.0f);
        }

        void CenteredAt(float centerX, float y, const char* text,
                        const ImVec4& color) {
            const float t = FUCK::CalcTextSize(text).x;
            FUCK::SetCursorScreenPos(ImVec2(centerX - t * 0.5f, y));
            FUCK::TextColored(color, "%s", text);
        }

        // P6 enter motion. FLICK's IsWindowAppearing never fires for
        // host-managed IWindows (known pitfall), so "fresh open" = first
        // Draw after a >0.4s gap; a kCloseOnGameMenu hide/re-show replays
        // the rise, which reads as intended. CONTENT offsets only - the
        // window itself never moves (in-Draw moves are no-ops); content
        // offset past the window rect clips at the window edge, which is
        // exactly the wipe-reveal look.
        struct EnterMotion {
            double lastDraw = -1.0, enterAt = 0.0;
            float Rise(double now, double delay, float amp) {
                if (lastDraw < 0.0 || now - lastDraw > 0.4) {
                    enterAt = now + delay;
                }
                lastDraw = now;
                const float t =
                    static_cast<float>((now - enterAt) / 0.28);
                if (t <= 0.0f) { return amp; }
                if (t >= 1.0f) { return 0.0f; }
                const float e = t * t * (3.0f - 2.0f * t);
                return (1.0f - e) * amp;
            }
        };

        // Both side widgets use the same quiet plate and footprint. This is
        // deliberately simple: the previous open-corner treatment left four
        // unrelated hairlines floating in the world, and its bottom strokes
        // landed directly on the host child's clip edge.
        void DrawPlate(const ImVec4& accent, float offY = 0.0f) {
            const ImVec2 p0 = FUCK::GetCursorScreenPos();
            const ImVec2 a = FUCK::GetContentRegionAvail();
            const ImVec2 p(p0.x, p0.y + offY);
            const float  s = FUCK::Scale(1.0f);
            const ImVec2 lo(p.x + 4 * s, p.y + 4 * s);
            const ImVec2 hi(p.x + a.x - 6 * s, p.y + a.y - 6 * s);
            FUCK::DrawRectFilled(ImVec2(lo.x + 4 * s, lo.y + 5 * s),
                                 ImVec2(hi.x + 4 * s, hi.y + 5 * s),
                                 ImVec4(0.0f, 0.0f, 0.0f, 0.24f), 7 * s);
            FUCK::DrawRectFilled(lo, hi,
                                 ImVec4(0.040f, 0.040f, 0.038f, 0.40f),
                                 7 * s);
            FUCK::DrawRect(lo, hi, ImVec4(0.46f, 0.46f, 0.43f, 0.78f),
                           7 * s, 2 * s);
            FUCK::DrawLine(ImVec2(lo.x + 12 * s, lo.y + 5 * s),
                           ImVec2(hi.x - 12 * s, lo.y + 5 * s), accent,
                           2 * s);
        }

        ImVec4 GloryColor(float f) {
            if (f < 0.34f) return ImVec4(0.74f, 0.18f, 0.12f, 0.96f);
            if (f < 0.67f) return ImVec4(0.82f, 0.57f, 0.16f, 0.96f);
            return ImVec4(0.36f, 0.68f, 0.30f, 0.96f);
        }

        class ScoreWindow final : public FUCK::IWindow {
        public:
            // V5 separates Star Power from score/multiplier and scales the
            // physical host window with FLICK's resolution scale.
            const char* Id() const override { return "HudScoreV5"; }
            const char* Title() const override { return "BardHero Score"; }
            // Hidden in practice: score and multiplier report a run that is
            // deliberately not being recorded.
            bool IsOpen() const override { return ScoringHudOpen(); }
            void SetOpen(bool) override {}
            FUCK::WindowFlags GetFlags() const override { return HudFlags(); }
            ImVec2 GetDefaultSize() const override {
                const float z = FUCK::Scale(1.0f);
                return ImVec2(hud_layout::PanelWidth(z),
                              hud_layout::PanelHeight(z));
            }
            ImVec2 GetDefaultPos() const override {
                const ImVec2 d = FUCK::GetDisplaySize();
                const ImVec2 s = GetDefaultSize();
                const float highwayEdge = d.x * (0.5f - 0.19f);
                return ImVec2(highwayEdge - s.x - d.x * 0.016f, d.y * 0.64f);
            }

            void Draw() override {
                const HudSnap s = Snapshot();
                if (!s.valid) return;
                const float z = FUCK::Scale(1.0f);
                const float offY =
                    _enter.Rise(FUCK::GetTime(), 0.0, 46.0f * z);
                DrawPlate(ImVec4(0.48f, 0.48f, 0.45f, 0.88f), offY);
                const ImVec2 origin0 = FUCK::GetCursorScreenPos();
                const ImVec2 avail = FUCK::GetContentRegionAvail();
                const ImVec2 origin(origin0.x, origin0.y + offY);
                const float cx = origin.x + avail.x * 0.5f;

                char text[64];
                const char* title = nullptr;
                if (s.countdown > 0) {
                    title = "READY";
                    std::snprintf(text, sizeof(text), "%d", s.countdown);
                } else {
                    title = "SCORE";
                    std::snprintf(text, sizeof(text), "%lld", s.score);
                }
                const float titleH = FUCK::CalcTextSize(title).y;
                FUCK::PushFontScaled(FUCK::GetFont(FUCK::Font::kLarge), 1.12f);
                const float valueH = FUCK::CalcTextSize(text).y;
                FUCK::PopFont();
                char mult[32];
                std::snprintf(mult, sizeof(mult), "x%d", s.mult);
                FUCK::PushFontScaled(FUCK::GetFont(FUCK::Font::kLarge), 1.26f);
                const float multH = FUCK::CalcTextSize(mult).y;
                FUCK::PopFont();
                const auto layout = hud_layout::MakeScore(
                    avail.y, z, titleH, valueH, multH);
                CenteredAt(cx, origin.y + layout.titleY, title,
                           ImVec4(0.66f, 0.66f, 0.63f, 0.95f));
                FUCK::PushFontScaled(FUCK::GetFont(FUCK::Font::kLarge), 1.12f);
                CenteredAt(cx, origin.y + layout.valueY, text,
                           ImVec4(0.92f, 0.92f, 0.88f, 1.0f));
                FUCK::PopFont();
                FUCK::PushFontScaled(FUCK::GetFont(FUCK::Font::kLarge), 1.26f);
                CenteredAt(cx, origin.y + layout.multiplierY, mult,
                           ImVec4(0.92f, 0.92f, 0.88f, 1.0f));
                FUCK::PopFont();
                // small pending (2026-07-25): in-panel streak readout.
                // Sits between score and multiplier; skipped when the
                // layout's fallback branch leaves no room. Gold from the
                // first banner milestone.
                if (s.countdown == 0 && s.combo >= 2) {
                    char streak[32];
                    std::snprintf(streak, sizeof(streak), "STREAK %d",
                                  s.combo);
                    const float sh = FUCK::CalcTextSize(streak).y;
                    const float sy = layout.valueY + valueH + 4.0f * z;
                    if (sy + sh + 2.0f * z < layout.multiplierY) {
                        const ImVec4 c = s.combo >= 50
                            ? ImVec4(0.98f, 0.80f, 0.24f, 0.95f)
                            : ImVec4(0.62f, 0.62f, 0.59f, 0.90f);
                        CenteredAt(cx, origin.y + sy, streak, c);
                    }
                }
                DimIfPaused(origin0, avail);
            }

        private:
            EnterMotion _enter;
        };

        class GloryWindow final : public FUCK::IWindow {
        public:
            // V4 owns both crowd sentiment and Star Power on the right.
            const char* Id() const override { return "HudGloryV4"; }
            const char* Title() const override { return "BardHero Glory"; }
            // Hidden in practice, and this window owns BOTH Glory and Star
            // Power - the crowd is not judging a practice run and its
            // reactions are gated off anyway (rules.crowdReactions).
            bool IsOpen() const override { return ScoringHudOpen(); }
            void SetOpen(bool) override {}
            FUCK::WindowFlags GetFlags() const override { return HudFlags(); }
            ImVec2 GetDefaultSize() const override {
                const float z = FUCK::Scale(1.0f);
                return ImVec2(hud_layout::PanelWidth(z),
                              hud_layout::PanelHeight(z));
            }
            ImVec2 GetDefaultPos() const override {
                const ImVec2 d = FUCK::GetDisplaySize();
                const float highwayEdge = d.x * (0.5f + 0.19f);
                return ImVec2(highwayEdge + d.x * 0.016f, d.y * 0.64f);
            }

            void Draw() override {
                const HudSnap s = Snapshot();
                if (!s.valid) return;
                const float dangerWave = static_cast<float>(
                    std::sin(FUCK::GetTime() * 10.0) * 0.5 + 0.5);
                const ImVec4 danger{ 0.96f, 0.08f, 0.035f,
                                     0.52f + 0.48f * dangerWave };
                const ImVec4 color =
                    s.gloryDanger ? danger : GloryColor(s.glory);
                const float z = FUCK::Scale(1.0f);
                // staggered ~120ms behind the score panel (spec P6)
                const float offY =
                    _enter.Rise(FUCK::GetTime(), 0.12, 46.0f * z);
                DrawPlate(color, offY);
                const ImVec2 origin0 = FUCK::GetCursorScreenPos();
                const ImVec2 avail = FUCK::GetContentRegionAvail();
                const ImVec2 origin(origin0.x, origin0.y + offY);
                const float cx = origin.x + avail.x * 0.5f;
                FUCK::PushFont(FUCK::GetFont(FUCK::Font::kLarge));
                const float titleH = FUCK::CalcTextSize("GLORY").y;
                FUCK::PopFont();
                const float labelH = FUCK::CalcTextSize("STAR POWER").y;
                const auto layout = hud_layout::MakeGlory(
                    avail.y, z, titleH, labelH);
                FUCK::PushFont(FUCK::GetFont(FUCK::Font::kLarge));
                CenteredAt(cx, origin.y + layout.titleY, "GLORY",
                           s.gloryDanger ? danger
                                         : ImVec4(0.92f, 0.92f, 0.88f, 1.0f));
                FUCK::PopFont();

                const float  gw = std::max(80.0f * z, avail.x - 42.0f * z);
                const float  gh = 28.0f * z;
                const float  x = origin.x + (avail.x - gw) * 0.5f;
                const float  y = origin.y + layout.meterY;
                FUCK::DrawRectFilled(ImVec2(x, y), ImVec2(x + gw, y + gh),
                                     ImVec4(0.025f, 0.022f, 0.018f, 0.62f),
                                     5.0f * z);
                FUCK::DrawRect(ImVec2(x, y), ImVec2(x + gw, y + gh),
                               ImVec4(0.50f, 0.48f, 0.43f, 0.94f),
                               5.0f * z, 2.0f * z);
                // Guitar Hero's Rock Meter is a fixed red/yellow/green
                // gauge with a moving needle, not a progress bar whose
                // entire color changes. Keep all three outcomes visible so
                // one judgment reads as direction and danger at a glance.
                const float inset = 5.0f * z;
                const float innerX = x + inset;
                const float innerY = y + inset;
                const float innerW = gw - 2 * inset;
                const float innerH = gh - 2 * inset;
                const ImVec4 zones[3] = {
                    ImVec4(0.66f, 0.055f, 0.025f, 0.88f),
                    ImVec4(0.78f, 0.48f, 0.035f, 0.84f),
                    ImVec4(0.20f, 0.67f, 0.18f, 0.84f)
                };
                for (int i = 0; i < 3; ++i) {
                    const float lo = innerX + innerW * (i / 3.0f);
                    const float hi = innerX + innerW * ((i + 1) / 3.0f);
                    FUCK::DrawRectFilled(
                        ImVec2(lo, innerY), ImVec2(hi, innerY + innerH),
                        zones[i], i == 0 || i == 2 ? 2.0f * z : 0.0f);
                }
                for (int i = 1; i < 3; ++i) {
                    const float lx =
                        innerX + innerW * (static_cast<float>(i) / 3.0f);
                    FUCK::DrawLine(ImVec2(lx, y + 3 * z),
                                   ImVec2(lx, y + gh - 3 * z),
                                   ImVec4(0.06f, 0.05f, 0.04f, 0.78f),
                                   2.0f * z);
                }
                const float needleX = innerX + innerW * s.glory;
                FUCK::DrawLine(
                    ImVec2(needleX, y + 1.5f * z),
                    ImVec2(needleX, y + gh - 1.5f * z),
                    ImVec4(0.98f, 0.96f, 0.88f, 1.0f), 3.0f * z);
                FUCK::DrawRectFilled(
                    ImVec2(needleX - 4.0f * z, y),
                    ImVec2(needleX + 4.0f * z, y + 4.0f * z),
                    ImVec4(0.98f, 0.96f, 0.88f, 1.0f), 1.0f * z);

                // Star Power lives below Glory, conventionally paired on
                // the right and independent of the left score font flow.
                const ImVec4 cyan{ 0.10f, 0.92f, 1.0f, 1.0f };
                const float pulse = s.spActive
                    ? 0.78f + 0.22f * static_cast<float>(
                          std::sin(FUCK::GetTime() * 7.0) * 0.5 + 0.5)
                    : 0.82f;
                // P3 meter zap: a completed phrase flashes the gauge
                // white-hot for ~0.45s (the highway bolt fires off the
                // same engine counter). Sentinel -1 = no zap when
                // joining a session already in progress.
                const double nowT = FUCK::GetTime();
                if (_prevPhrases >= 0 && s.spPhrases > _prevPhrases) {
                    _zapUntil = nowT + 0.45;
                }
                _prevPhrases = s.spPhrases;
                const float zap =
                    _zapUntil > nowT
                        ? static_cast<float>((_zapUntil - nowT) / 0.45)
                        : 0.0f;
                const ImVec4 power{ cyan.x + (1.0f - cyan.x) * zap,
                                    cyan.y + (1.0f - cyan.y) * zap,
                                    cyan.z, pulse };
                const char* state = s.spActive
                    ? "ACTIVE"
                    : (s.sp >= 0.5f ? "READY" : "");
                const float pw = avail.x - 36.0f * z;
                const float ph = 11.0f * z;
                const float px = origin.x + 18.0f * z;
                const float py = origin.y + layout.powerMeterY;
                FUCK::SetCursorScreenPos(
                    ImVec2(px, origin.y + layout.powerLabelY));
                FUCK::TextColored(ImVec4(0.62f, 0.62f, 0.59f, 0.96f),
                                  "STAR POWER");
                if (*state) {
                    const float sw = FUCK::CalcTextSize(state).x;
                    FUCK::SetCursorScreenPos(
                        ImVec2(px + pw - sw,
                               origin.y + layout.powerLabelY));
                    FUCK::TextColored(power, "%s", state);
                }
                FUCK::DrawRectFilled(
                    ImVec2(px, py), ImVec2(px + pw, py + ph),
                    ImVec4(0.02f, 0.025f, 0.03f, 0.62f), 2.0f * z);
                constexpr int kCells = 8;
                const float gap = 2.0f * z;
                const float inset2 = 2.0f * z;
                const float cellW =
                    (pw - 2 * inset2 - gap * (kCells - 1)) / kCells;
                const int filled = std::clamp(
                    static_cast<int>(std::ceil(s.sp * kCells)), 0, kCells);
                for (int i = 0; i < kCells; ++i) {
                    const float cellX = px + inset2 + i * (cellW + gap);
                    FUCK::DrawRectFilled(
                        ImVec2(cellX, py + inset2),
                        ImVec2(cellX + cellW, py + ph - inset2),
                        i < filled ? power
                                   : ImVec4(0.10f, 0.14f, 0.15f, 0.72f),
                        z);
                }
                FUCK::DrawRect(
                    ImVec2(px, py), ImVec2(px + pw, py + ph),
                    s.spActive || zap > 0.0f
                        ? power
                        : ImVec4(0.35f, 0.42f, 0.43f, 1.0f),
                    2.0f * z,
                    s.spActive || zap > 0.0f ? 2.0f * z : z);
                if (zap > 0.0f) {  // electric glow halo around the bar
                    FUCK::DrawRectFilled(
                        ImVec2(px - 5.0f * z, py - 5.0f * z),
                        ImVec2(px + pw + 5.0f * z, py + ph + 5.0f * z),
                        ImVec4(cyan.x, cyan.y, cyan.z, 0.30f * zap),
                        4.0f * z);
                }
                DimIfPaused(origin0, avail);
            }

        private:
            int         _prevPhrases = -1;
            double      _zapUntil    = 0.0;
            EnterMotion _enter;
        };

        class ResumeCountdownWindow final : public FUCK::IWindow {
        public:
            const char* Id() const override { return "ResumeCountdownV1"; }
            const char* Title() const override {
                return "BardHero Resume Countdown";
            }
            bool IsOpen() const override {
                return UiBus::GetSingleton().resumeCountdownActive.load(
                    std::memory_order_acquire);
            }
            void SetOpen(bool) override {}
            FUCK::WindowFlags GetFlags() const override { return HudFlags(); }
            ImVec2 GetDefaultSize() const override {
                return FUCK::Scale(260.0f, 150.0f);
            }
            ImVec2 GetDefaultPos() const override {
                const ImVec2 d = FUCK::GetDisplaySize();
                const ImVec2 s = GetDefaultSize();
                return ImVec2((d.x - s.x) * 0.5f, d.y * 0.36f);
            }

            void Draw() override {
                const int cue =
                    UiBus::GetSingleton().resumeCountdownCue.load(
                        std::memory_order_acquire);
                const char* text = cue == 3   ? "3"
                                   : cue == 2 ? "2"
                                   : cue == 1 ? "1"
                                   : cue == 4 ? "GO!"
                                              : "";
                if (!*text) return;

                DrawPlate(ImVec4(0.88f, 0.70f, 0.32f, 0.95f));
                const ImVec2 origin = FUCK::GetCursorScreenPos();
                const ImVec2 avail = FUCK::GetContentRegionAvail();
                const float cx = origin.x + avail.x * 0.5f;
                CenteredAt(cx, origin.y + avail.y * 0.18f, "GET READY",
                           ImVec4(0.72f, 0.72f, 0.68f, 0.98f));
                FUCK::PushFontScaled(FUCK::GetFont(FUCK::Font::kLarge),
                                     2.2f);
                const float h = FUCK::CalcTextSize(text).y;
                CenteredAt(cx, origin.y + (avail.y - h) * 0.58f, text,
                           cue == 4
                               ? ImVec4(0.96f, 0.78f, 0.30f, 1.0f)
                               : ImVec4(0.94f, 0.94f, 0.90f, 1.0f));
                FUCK::PopFont();
            }
        };

        // Transient over-highway banners (GH-feel spec P1): streak
        // milestones every 50 comboed notes, the pulsing STAR POWER READY
        // call-to-action, and a short activation flash. One always-open
        // window whose content appears transiently - windows are never
        // opened mid-session (FUCK WindowState race), and all motion is
        // CONTENT animation (in-Draw window moves are no-ops).
        class JuiceBannerWindow final : public FUCK::IWindow {
        public:
            const char* Id() const override { return "HudJuiceBannerV1"; }
            const char* Title() const override {
                return "BardHero Banners";
            }
            bool IsOpen() const override { return HudOpen(); }
            void SetOpen(bool) override {}
            FUCK::WindowFlags GetFlags() const override { return HudFlags(); }
            ImVec2 GetDefaultSize() const override {
                return FUCK::Scale(920.0f, 210.0f);
            }
            ImVec2 GetDefaultPos() const override {
                const ImVec2 d = FUCK::GetDisplaySize();
                const ImVec2 s = GetDefaultSize();
                // Above the resume countdown slot, clear of the gems.
                return ImVec2((d.x - s.x) * 0.5f, d.y * 0.12f);
            }

            void Draw() override {
                const HudSnap s = Snapshot();
                if (!s.valid) return;
                const double now = FUCK::GetTime();
                // A countdown means song start or resume: drop any stale
                // banner state from the previous run.
                if (s.countdown > 0) {
                    _lastCombo = 0;
                    _milestoneUntil = -1.0;
                    _wasReady = false;
                    _wasActive = s.spActive;
                    return;
                }
                // Streak milestone: fires on the upward crossing of each
                // 50-combo boundary. Combo only steps by one, so a single
                // comparison of the 50-buckets is exact.
                if (s.combo >= 50 && s.combo / 50 > _lastCombo / 50) {
                    _milestone = (s.combo / 50) * 50;
                    _milestoneUntil = now + 2.3;
                    spdlog::info("[fx] streak banner {}", _milestone);
                }
                _lastCombo = s.combo;
                const bool ready = !s.spActive && s.sp >= 0.5f;
                if (ready && !_wasReady) {
                    _readySince = now;
                    spdlog::info("[fx] sp ready banner shown");
                }
                _wasReady = ready;
                if (s.spActive && !_wasActive) {
                    _activeFlashUntil = now + 1.5;
                    spdlog::info("[fx] sp activation flash");
                }
                _wasActive = s.spActive;

                if (now < _milestoneUntil) {
                    char text[48];
                    std::snprintf(text, sizeof(text), "%d NOTE STREAK",
                                  _milestone);
                    // Slam-in: overshoot scale decaying to rest, a short
                    // rocking wobble, ray burst, fade in the final 0.4s.
                    const double t = now - (_milestoneUntil - 2.3);
                    const float scale = 2.0f
                        + 0.9f * static_cast<float>(std::exp(-t * 14.0));
                    const float alpha = _milestoneUntil - now < 0.4
                        ? static_cast<float>((_milestoneUntil - now) / 0.4)
                        : 1.0f;
                    const float bob = 6.0f * static_cast<float>(
                        std::exp(-t * 3.0) * std::sin(t * 21.0));
                    BannerText(text, scale, bob,
                               ImVec4(0.98f, 0.82f, 0.26f, alpha),
                               ImVec4(0.46f, 0.28f, 0.04f, 0.85f * alpha));
                } else if (now < _activeFlashUntil) {
                    const double t = now - (_activeFlashUntil - 1.5);
                    const float a = static_cast<float>(
                        (_activeFlashUntil - now) / 1.5);
                    const float scale = 2.6f
                        + 1.0f * static_cast<float>(std::exp(-t * 12.0));
                    BannerText("STAR POWER!", scale, 0.0f,
                               ImVec4(0.78f, 0.99f, 1.0f, a),
                               ImVec4(0.05f, 0.38f, 0.48f, 0.85f * a));
                } else if (ready) {
                    // Settle from an entrance overshoot into a ~1.1Hz
                    // pulse with a slow rocking bob and a lazy ray spin.
                    const double t = now - _readySince;
                    const float entrance =
                        0.7f * static_cast<float>(std::exp(-t * 8.0));
                    const float pulse = 0.10f * static_cast<float>(
                        std::sin(t * 6.9));
                    const float bob = 4.0f * static_cast<float>(
                        std::sin(t * 2.2));
                    BannerText("STAR POWER READY",
                               1.9f + entrance + pulse, bob,
                               ImVec4(0.58f, 0.96f, 1.0f, 0.96f),
                               ImVec4(0.03f, 0.30f, 0.42f, 0.80f));
                }
            }

        private:

            // Layered text: soft glow pass, hard shadow, then the core.
            // Alpha layering stands in for additive bloom (no additive
            // blend in this renderer).
            void BannerText(const char* a_text, float a_scale, float a_bob,
                            const ImVec4& a_core, const ImVec4& a_glow) {
                const ImVec2 origin = FUCK::GetCursorScreenPos();
                const ImVec2 avail = FUCK::GetContentRegionAvail();
                const float z = FUCK::Scale(1.0f);
                FUCK::PushFontScaled(FUCK::GetFont(FUCK::Font::kLarge),
                                     a_scale);
                const ImVec2 ts = FUCK::CalcTextSize(a_text);
                const float x = origin.x + (avail.x - ts.x) * 0.5f;
                const float y = origin.y + (avail.y - ts.y) * 0.5f
                    + a_bob * z;
                const ImVec4 glowSoft{ a_glow.x, a_glow.y, a_glow.z,
                                       a_glow.w * 0.45f };
                for (const auto& off :
                     { ImVec2(-2, 0), ImVec2(2, 0), ImVec2(0, -2),
                       ImVec2(0, 2) }) {
                    FUCK::SetCursorScreenPos(
                        ImVec2(x + off.x * z, y + off.y * z));
                    FUCK::TextColored(glowSoft, "%s", a_text);
                }
                FUCK::SetCursorScreenPos(
                    ImVec2(x + 3.0f * z, y + 3.0f * z));
                FUCK::TextColored(
                    ImVec4(0.0f, 0.0f, 0.0f, 0.55f * a_core.w), "%s",
                    a_text);
                FUCK::SetCursorScreenPos(ImVec2(x, y));
                FUCK::TextColored(a_core, "%s", a_text);
                FUCK::PopFont();
            }

            int    _lastCombo = 0;
            int    _milestone = 0;
            double _milestoneUntil = -1.0;
            double _readySince = 0.0;
            double _activeFlashUntil = -1.0;
            bool   _wasReady = false;
            bool   _wasActive = false;
        };

        // ---- practice strip (plan P5) -----------------------------------
        struct PracticeSnap {
            bool        valid = false;
            int         loop = 0;
            float       speed = 1.0f;
            float       accuracy = 0.0f;
            int         attempted = 0;
            std::string fromName, toName;
        };

        PracticeSnap PracticeSnapshot() {
            PracticeSnap p;
            auto&        bus = UiBus::GetSingleton();
            if (!bus.practiceActive.load(std::memory_order_acquire)) {
                return p;
            }
            // Every bus read happens BEFORE feed.mx is taken. These are
            // atomics precisely so this never needs UiBus::mx, which must
            // never be held together with EngineFeed::mx (UiBus.h:4-6).
            p.loop            = bus.practiceLoop.load();
            p.speed           = bus.practiceSpeed.load();
            const int fromIdx = bus.practiceStartSection.load();
            const int toIdx   = bus.practiceEndSection.load();
            auto&     feed    = EngineFeed::GetSingleton();
            std::scoped_lock lk(feed.mx);
            if (!feed.engine || !feed.song) { return p; }
            const auto& st = feed.engine->Stats();
            // Accuracy over notes ATTEMPTED, not over the whole slice: the
            // player wants "how am I doing", and hit/total reads near zero
            // for most of every loop no matter how well it is going.
            p.attempted = st.notesHit + st.notesMissed;
            p.accuracy  = p.attempted > 0
                              ? static_cast<float>(st.notesHit) / p.attempted
                              : 0.0f;
            // The names come from the SLICE's own section list, which
            // SliceChart copies verbatim from the full chart - so the
            // published indices still address it correctly. Bounds-checked
            // because a republished chart could be shorter than the indices
            // that were current when they were stored.
            const auto& secs = feed.song->chart.sections;
            const int   n    = static_cast<int>(secs.size());
            if (fromIdx >= 0 && fromIdx < n) {
                p.fromName = secs[static_cast<std::size_t>(fromIdx)].name;
            }
            if (toIdx >= 0 && toIdx < n) {
                p.toName = secs[static_cast<std::size_t>(toIdx)].name;
            }
            p.valid = true;
            return p;
        }

        class PracticeStripWindow final : public FUCK::IWindow {
        public:
            const char* Id() const override { return "HudPracticeV1"; }
            const char* Title() const override { return "BardHero Practice"; }
            bool        IsOpen() const override {
                return HudOpen() &&
                       UiBus::GetSingleton().practiceActive.load(
                           std::memory_order_acquire);
            }
            void SetOpen(bool) override {}
            // HudFlags carries kPassInputToGame, which is MANDATORY here:
            // this strip sits over live gameplay, and an input-capturing
            // window there reads as a total input lock. It is the exact
            // opposite of the picker, which deliberately omits it.
            FUCK::WindowFlags GetFlags() const override { return HudFlags(); }
            ImVec2 GetDefaultSize() const override {
                const float z = FUCK::Scale(1.0f);
                // Deliberately large. With score, Glory, Star Power and the
                // streak banners all hidden in practice, this is the ONLY
                // chrome on screen, so it can afford the room - and the
                // range name and loop counter are what the player is
                // actually glancing at between attempts.
                const ImVec2 d = FUCK::GetDisplaySize();
                return ImVec2(std::clamp(d.x * 0.56f, 640.0f, 1180.0f),
                              118.0f * z);
            }
            ImVec2 GetDefaultPos() const override {
                const ImVec2 d = FUCK::GetDisplaySize();
                const ImVec2 s = GetDefaultSize();
                // Top centre. Nothing competes with it now.
                return ImVec2((d.x - s.x) * 0.5f, d.y * 0.030f);
            }

            void Draw() override {
                const PracticeSnap p = PracticeSnapshot();
                if (!p.valid) { return; }
                const float  z = FUCK::Scale(1.0f);
                DrawPlate(ImVec4(0.88f, 0.70f, 0.32f, 0.88f));
                const ImVec2 origin = FUCK::GetCursorScreenPos();
                const ImVec2 avail  = FUCK::GetContentRegionAvail();
                const float  cx     = origin.x + avail.x * 0.5f;
                char         range[128];
                if (p.fromName.empty() && p.toName.empty()) {
                    // -1/-1 is whole song, which is the COMMON case: most
                    // charts carry no section markers at all.
                    std::snprintf(range, sizeof(range), "WHOLE SONG");
                } else if (p.fromName == p.toName || p.toName.empty()) {
                    std::snprintf(range, sizeof(range), "%s",
                                  p.fromName.c_str());
                } else {
                    std::snprintf(range, sizeof(range), "%s  >  %s",
                                  p.fromName.c_str(), p.toName.c_str());
                }
                FUCK::PushFont(FUCK::GetFont(FUCK::Font::kLarge));
                CenteredAt(cx, origin.y + 12.0f * z, range,
                           ImVec4(0.88f, 0.70f, 0.32f, 1.0f));
                FUCK::PopFont();
                char line[160];
                if (p.attempted > 0) {
                    std::snprintf(line, sizeof(line),
                                  "PRACTICE   LOOP %d   SPEED %d%%   ACC %d%%",
                                  p.loop, static_cast<int>(p.speed * 100.0f +
                                                           0.5f),
                                  static_cast<int>(p.accuracy * 100.0f + 0.5f));
                } else {
                    // No notes judged yet this loop - "ACC 0%" would read as
                    // a failing run rather than an empty one.
                    std::snprintf(line, sizeof(line),
                                  "PRACTICE   LOOP %d   SPEED %d%%   ACC --",
                                  p.loop, static_cast<int>(p.speed * 100.0f +
                                                           0.5f));
                }
                FUCK::PushFont(FUCK::GetFont(FUCK::Font::kLarge));
                CenteredAt(cx, origin.y + 62.0f * z, line,
                           ImVec4(0.82f, 0.82f, 0.78f, 1.0f));
                FUCK::PopFont();
                DimIfPaused(origin, avail);
            }
        };

        // ---- shared control hint bar -------------------------------------
        // ONE bar at the bottom centre of the SCREEN rather than a legend
        // inside every panel. The panels stay clean, and the player learns
        // one place to look instead of hunting a different footer on each
        // surface.
        //
        // It lives in the HUD file because it IS a HUD element: a decorative,
        // input-transparent overlay sharing HudFlags, whose kPassInputToGame
        // is what stops it ever becoming an input surface. It is the one
        // window here that is not gated on a live session.
        //
        // Order is most-specific-first. The practice picker must be tested
        // BEFORE the Songbook: the picker is a sub-view, so browserOpen is
        // true for both.
        const char* ActiveHints() {
            auto& bus = UiBus::GetSingleton();
            if (bus.resultsReady.load(std::memory_order_relaxed) ||
                bus.practiceSummaryReady.load(std::memory_order_relaxed)) {
                return "Green fret / Enter   Continue";
            }
            if (bus.pauseMenuOpen.load(std::memory_order_relaxed)) {
                return "Strum / Up-Down   Move       "
                       "Green fret / Enter   Select";
            }
            if (bus.practicePickerOpen.load(std::memory_order_relaxed)) {
                // "Adjust" covers the two non-list stops: the strum steps
                // the speed presets and flips the loop once Left/Right has
                // carried focus there.
                return "Strum / Up-Down   Move - Adjust       "
                       "Yellow-Blue / Left-Right   Focus       "
                       "Green / Enter   Start       Red / Esc   Back";
            }
            if (bus.browserOpen.load(std::memory_order_relaxed)) {
                return "Strum / Up-Down   Song       "
                       "Yellow-Blue / Left-Right   Difficulty       "
                       "Orange   Practice       Green / Enter   Play       "
                       "Red / Esc   Close";
            }
            if (bus.practiceActive.load(std::memory_order_relaxed)) {
                return "-   Slower       =   Faster       Esc   Pause";
            }
            return nullptr;
        }

        class HintBarWindow final : public FUCK::IWindow {
        public:
            const char* Id() const override { return "HudHintBarV1"; }
            const char* Title() const override { return "BardHero Hints"; }
            bool IsOpen() const override { return ActiveHints() != nullptr; }
            void SetOpen(bool) override {}
            FUCK::WindowFlags GetFlags() const override { return HudFlags(); }
            ImVec2 GetDefaultSize() const override {
                const ImVec2 d = FUCK::GetDisplaySize();
                return ImVec2(d.x, 40.0f * FUCK::Scale(1.0f));
            }
            ImVec2 GetDefaultPos() const override {
                const ImVec2 d = FUCK::GetDisplaySize();
                const ImVec2 s = GetDefaultSize();
                // Full width and bottom-anchored, so a long hint line is
                // centred on the SCREEN rather than on whatever panel
                // happens to be up.
                return ImVec2(0.0f, d.y - s.y - d.y * 0.020f);
            }
            void Draw() override {
                const char* hints = ActiveHints();
                // Re-read rather than trusting IsOpen: the surface can close
                // between the two calls, and CenteredAt would dereference a
                // null.
                if (!hints) { return; }
                const ImVec2 origin = FUCK::GetCursorScreenPos();
                const ImVec2 avail  = FUCK::GetContentRegionAvail();
                CenteredAt(origin.x + avail.x * 0.5f, origin.y, hints,
                           ImVec4(0.74f, 0.74f, 0.70f, 0.94f));
            }
        };

        // Practice summary, shown when looping is off and the range has had
        // its single pass. Reads the tiny atomic payload the session
        // published - it never goes near UiBus::Results, so it cannot drag
        // practice through the recording path.
        //
        // It captures NO input and has no buttons. It dismisses itself on a
        // timer and on the next session start. That is deliberate: giving it
        // a confirm would mean a new `capture` value in InputHook.cpp and a
        // cursor refcount to balance, which is the machinery this project has
        // repeatedly turned into a total input lock. A read-only scoreboard
        // does not need any of it.
        class PracticeSummaryWindow final : public FUCK::IWindow {
        public:
            // A SAFETY NET, not a dismissal mechanism. At 9s it was doing
            // the dismissing - the panel closed itself out from under the
            // player before they had read it (field 2026-07-26). The reason
            // to keep any timer is unchanged: a modal that cannot be closed
            // is an input lock with no way out, so if the confirm path ever
            // breaks, something has to give the player their game back. So
            // it stays, moved well past any plausible reading time. The
            // results panel has no timer at all and does not need one -
            // it is not the panel that has repeatedly lost its cursor.
            static constexpr double kHoldSec = 120.0;
            // Confirm dead-time after the panel appears. Long enough to
            // outlast the trailing strum of the run that just ended, short
            // enough that a player reaching for Continue never notices it.
            static constexpr double kArmSec = 0.6;

            const char* Id() const override { return "HudPracticeSummaryV1"; }
            const char* Title() const override {
                return "BardHero Practice Summary";
            }
            bool IsOpen() const override {
                const bool open =
                    UiBus::GetSingleton().practiceSummaryReady.load(
                        std::memory_order_acquire);
                // Cursor released HERE, not only in Draw. The session thread
                // can clear the summary (a new run starting) and then Draw
                // simply stops being called - a refcount leaked that way
                // reads as a TOTAL INPUT LOCK while every control flag looks
                // healthy. Copied from PauseMenuWindow's force-close path.
                if (!open) {
                    auto* self = const_cast<PracticeSummaryWindow*>(this);
                    self->_drawHeld = false;
                    self->_shownAt  = 0.0;
                    if (_cursorHeld) {
                        RenderUi::ReleaseCursor();
                        self->_cursorHeld = false;
                    }
                }
                return open;
            }
            void SetOpen(bool) override {}
            // NOT HudFlags: this one is a modal, so it must NOT carry
            // kPassInputToGame. It is the opposite case from the practice
            // strip, which is decorative and must.
            FUCK::WindowFlags GetFlags() const override {
                using F = FUCK::WindowFlags;
                return static_cast<F>(
                    static_cast<unsigned>(F::kNoDecoration) |
                    static_cast<unsigned>(F::kNoBackground) |
                    static_cast<unsigned>(F::kNoMove) |
                    static_cast<unsigned>(F::kNoResize) |
                    static_cast<unsigned>(F::kHideHUD) |
                    static_cast<unsigned>(F::kCloseOnGameMenu));
            }
            ImVec2 GetDefaultSize() const override {
                const float z = FUCK::Scale(1.0f);
                // Tall enough for the Continue button at +192, not just the
                // text above it.
                return ImVec2(520.0f * z, 268.0f * z);
            }
            ImVec2 GetDefaultPos() const override {
                const ImVec2 d = FUCK::GetDisplaySize();
                const ImVec2 s = GetDefaultSize();
                return ImVec2((d.x - s.x) * 0.5f, (d.y - s.y) * 0.42f);
            }

            void Draw() override {
                auto&        bus = UiBus::GetSingleton();
                const double now = FUCK::GetTime();
                if (!_drawHeld) {
                    _drawHeld = true;
                    _shownAt  = now;
                    RenderUi::AcquireCursor();
                    _cursorHeld = true;
                    // The confirm that ENDED the run may still be in flight;
                    // without this it dismisses the summary on frame one.
                    bus.DrainNav();
                }
                // The confirm is ARMED late, and one drain is not enough.
                //
                // A practice range ends on its own while the player is still
                // playing, and the green fret IS the confirm bind - so the
                // last notes of the run arrive as confirms. DrainNav clears
                // what had already accumulated by frame one; a green press
                // landing on frame two is the player's trailing input for a
                // run that is over, and it dismissed the panel before it
                // could be read (field 2026-07-26 "it can exit without me
                // pressing continue"). Swallow rather than queue: a confirm
                // arriving inside the window means the run, not the panel.
                if (now - _shownAt < kArmSec) {
                    bus.navConfirm.store(false);
                } else {
                    const bool confirm = bus.navConfirm.exchange(false);
                    if (confirm) {
                        ui_sound::Play(ui_sound::Event::kConfirm);
                        bus.ClearPracticeSummary();
                        return;
                    }
                }
                if (now - _shownAt > kHoldSec) {
                    spdlog::warn(
                        "[practice] summary auto-dismissed after {:.0f}s - "
                        "the confirm path never fired",
                        kHoldSec);
                    bus.ClearPracticeSummary();
                    return;
                }
                const int hit    = bus.practiceSummaryHit.load();
                const int missed = bus.practiceSummaryMissed.load();
                const int total  = bus.practiceSummaryTotal.load();
                const int combo  = bus.practiceSummaryCombo.load();
                const float spd  = bus.practiceSummarySpeed.load();
                const int attempted = hit + missed;
                const float z = FUCK::Scale(1.0f);
                // The shared MODAL surface, not DrawPlate. DrawPlate is the
                // HUD widget plate - a 0.40-alpha wash with one hairline
                // border, sized to sit unobtrusively over the highway
                // mid-song. As a panel it reads as no background at all
                // (field 2026-07-26). This is a modal, so it gets what every
                // other BardHero modal gets: the drop shadow, the 0.86 fill
                // and the double border. Gold stays as the inner accent -
                // the practice identity the plate call carried.
                panel::DrawCurrent(ImVec4(0.88f, 0.70f, 0.32f, 0.90f));
                const ImVec2 origin = FUCK::GetCursorScreenPos();
                const ImVec2 avail  = FUCK::GetContentRegionAvail();
                const float  cx     = origin.x + avail.x * 0.5f;
                FUCK::PushFont(FUCK::GetFont(FUCK::Font::kLarge));
                CenteredAt(cx, origin.y + 14.0f * z, "PRACTICE COMPLETE",
                           ImVec4(0.88f, 0.70f, 0.32f, 1.0f));
                FUCK::PopFont();
                char line[192];
                std::snprintf(line, sizeof(line), "%d / %d notes    %d%%",
                              hit, total,
                              attempted > 0
                                  ? static_cast<int>(100.0f * hit / attempted +
                                                     0.5f)
                                  : 0);
                FUCK::PushFont(FUCK::GetFont(FUCK::Font::kLarge));
                CenteredAt(cx, origin.y + 74.0f * z, line,
                           ImVec4(0.86f, 0.86f, 0.82f, 1.0f));
                FUCK::PopFont();
                char sub[192];
                std::snprintf(sub, sizeof(sub),
                              "Best streak %d      Missed %d      Speed %d%%",
                              combo, missed,
                              static_cast<int>(spd * 100.0f + 0.5f));
                CenteredAt(cx, origin.y + 128.0f * z, sub,
                           ImVec4(0.72f, 0.72f, 0.68f, 0.96f));
                // Says it out loud, because a scoreboard that looks like the
                // results screen invites the assumption that it banked
                // something.
                CenteredAt(cx, origin.y + 160.0f * z,
                           "Practice runs are not recorded",
                           ImVec4(0.62f, 0.62f, 0.58f, 0.92f));
                // Continue, matching the results panel. Mouse or green fret
                // / Enter; both land on the same clear.
                panel::PushControls();
                const float btnW = FUCK::CalcTextSize("Continue").x +
                                   36.0f * z;
                FUCK::SetCursorScreenPos(
                    ImVec2(cx - btnW * 0.5f, origin.y + 192.0f * z));
                if (FUCK::Button("Continue")) {
                    ui_sound::Play(ui_sound::Event::kConfirm);
                    bus.ClearPracticeSummary();
                }
                panel::PopControls();
            }

        private:
            double _shownAt   = 0.0;
            bool   _drawHeld  = false;
            bool   _cursorHeld = false;
        };

        ScoreWindow           g_score;
        GloryWindow           g_glory;
        ResumeCountdownWindow g_resumeCountdown;
        PracticeStripWindow   g_practiceStrip;
        HintBarWindow         g_hintBar;
        PracticeSummaryWindow g_practiceSummary;
        // JuiceBannerWindow retired 2026-07-25: banners moved into the
        // highway overlay as rotated atlas-strip quads (HighwayWindow::
        // DrawBanners) - FLICK text cannot rotate, sprite strips can.
    }

    void RegisterHudWindow() {
        FUCK::RegisterWindow(&g_score);
        FUCK::RegisterWindow(&g_glory);
        FUCK::RegisterWindow(&g_resumeCountdown);
        FUCK::RegisterWindow(&g_practiceStrip);
        FUCK::RegisterWindow(&g_hintBar);
        FUCK::RegisterWindow(&g_practiceSummary);
    }
}
