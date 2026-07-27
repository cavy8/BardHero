// src/render/ResultsWindow.cpp
#include "PCH.h"
#include "render/ResultsWindow.h"

#include "game/EngineFeed.h"
#include "game/ResultsLogic.h"
#include "game/UiBus.h"
#include "game/UiSfx.h"
#include "render/RenderUi.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h (house ordering)

#include "FUCK_API.h"
#include "render/FlickRenderer.h"
#include "render/FxPool.h"
#include "render/PanelStyle.h"
#include "render/ResultsAnimation.h"
#include "render/ResultsLayout.h"
#include "render/UiSound.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace SH {
    namespace {
        constexpr ImVec4 kEarnedGold{ 0.96f, 0.73f, 0.20f, 1.0f };

        void TextAt(const ImVec2& p, const char* text, const ImVec4& color) {
            FUCK::SetCursorScreenPos(p);
            FUCK::TextColored(color, "%s", text);
        }

        void CenterAt(float x, float y, const char* text,
                      const ImVec4& color) {
            const float w = FUCK::CalcTextSize(text).x;
            TextAt(ImVec2(x - w * 0.5f, y), text, color);
        }

        void CenterPairAt(float x, float y,
                          const char* left, const ImVec4& leftColor,
                          const char* right, const ImVec4& rightColor,
                          float gap) {
            const float leftW = FUCK::CalcTextSize(left).x;
            const float rightW = FUCK::CalcTextSize(right).x;
            const float start = x - (leftW + gap + rightW) * 0.5f;
            TextAt(ImVec2(start, y), left, leftColor);
            TextAt(ImVec2(start + leftW + gap, y), right, rightColor);
        }

        ImVec4 Mix(const ImVec4& from, const ImVec4& to, float t) {
            return ImVec4(from.x + (to.x - from.x) * t,
                          from.y + (to.y - from.y) * t,
                          from.z + (to.z - from.z) * t,
                          from.w + (to.w - from.w) * t);
        }

        std::string FitText(std::string text, float maxWidth) {
            if (FUCK::CalcTextSize(text.c_str()).x <= maxWidth) { return text; }
            while (text.size() > 4) {
                text.pop_back();
                const std::string candidate = text + "...";
                if (FUCK::CalcTextSize(candidate.c_str()).x <= maxWidth) {
                    return candidate;
                }
            }
            return "...";
        }

        void DrawStar(const ImVec2& center, float outer, float angle,
                      const ImVec4& fill, const ImVec4& edge, float lineW) {
            constexpr float kPi = 3.14159265358979323846f;
            ImVec2 points[10];
            for (int i = 0; i < 10; ++i) {
                const float radius = (i % 2 == 0) ? outer : outer * 0.45f;
                const float a = -kPi * 0.5f + angle + i * kPi / 5.0f;
                points[i] = ImVec2(center.x + std::cos(a) * radius,
                                   center.y + std::sin(a) * radius);
            }
            for (int i = 0; i < 10; ++i) {
                FUCK::DrawTriangleFilled(
                    center, points[i], points[(i + 1) % 10], fill);
                if (lineW > 0.0f && edge.w > 0.0f) {
                    FUCK::DrawLine(points[i], points[(i + 1) % 10],
                                   edge, lineW);
                }
            }
        }

        // Feather a star-shaped halo with nested low-alpha silhouettes.
        // Outer-to-inner layering gives a soft falloff without the hard
        // circular borders that intersected neighboring stars.
        void DrawStarHalo(const ImVec2& center, float radius, float scale,
                          const ImVec4& color, float alpha) {
            constexpr int kLayers = 5;
            for (int layer = kLayers; layer >= 1; --layer) {
                const float t = static_cast<float>(layer) / kLayers;
                const float layerScale = 1.0f + (scale - 1.0f) * t;
                const float layerAlpha =
                    alpha * (0.10f + 0.20f * (1.0f - t));
                DrawStar(center, radius * layerScale, 0.0f,
                         ImVec4(color.x, color.y, color.z, layerAlpha),
                         ImVec4(0, 0, 0, 0), 0.0f);
            }
        }

        void RatingStars(const ImVec2& center, int stars, double elapsed) {
            const float s = FUCK::Scale(1.0f);
            const float gap = 25.0f * s, r = 9.0f * s;
            const float x0 = center.x - 2.0f * gap;
            for (int i = 0; i < 5; ++i) {
                const ImVec2 c(x0 + i * gap, center.y);
                DrawStar(c, r, 0.0f,
                         ImVec4(0.10f, 0.095f, 0.08f, 0.90f),
                         ImVec4(0.46f, 0.43f, 0.36f, 0.88f), s);
                const auto settled =
                    results_anim::SettledGlowAt(elapsed, i, i < stars);
                if (settled.alpha > 0.0f) {
                    DrawStarHalo(c, r, settled.radiusScale, kEarnedGold,
                                 settled.alpha);
                }
                const auto reveal =
                    results_anim::StarAt(elapsed, i, i < stars);
                if (reveal.alpha <= 0.0f) { continue; }
                if (reveal.glow > 0.0f) {
                    DrawStarHalo(
                        c, r,
                        results_anim::ImpactGlowScale(reveal.glow),
                        panel::kGold, 0.24f * reveal.glow);
                }
                DrawStar(
                    c, r * reveal.scale, reveal.angle,
                    ImVec4(kEarnedGold.x, kEarnedGold.y, kEarnedGold.z,
                           reveal.alpha),
                    ImVec4(1.0f, 0.90f, 0.48f, reveal.alpha),
                    1.4f * s);
            }
            const auto shine = results_anim::ShineAt(elapsed, stars);
            if (shine.alpha > 0.0f) {
                for (int i = 0; i < stars; ++i) {
                    const float at = static_cast<float>(i) / 4.0f;
                    const float local = std::max(
                        0.0f, 1.0f - std::abs(at - shine.center) /
                                           shine.width);
                    if (local <= 0.0f) { continue; }
                    const ImVec2 c(x0 + i * gap, center.y);
                    const float alpha = shine.alpha * local;
                    DrawStar(c, r * 0.72f, 0.0f,
                             ImVec4(1.0f, 0.98f, 0.78f, alpha),
                             ImVec4(1.0f, 0.94f, 0.58f, alpha),
                             0.8f * s);
                }
            }
        }

        class ResultsWindow final : public FUCK::IWindow {
        public:
            // V13 replaces intersecting circular halos with bounded,
            // feathered star silhouettes.
            const char* Id() const override { return "ResultsV13"; }
            const char* Title() const override { return "Results"; }
            bool        IsOpen() const override {
                const bool open = UiBus::GetSingleton().resultsReady.load();
                // resultsReady can now be cleared EXTERNALLY (OnPreLoadGame:
                // a load closes the box) - Draw then stops being called, so
                // the cursor must be released here (pause-menu precedent)
                if (!open && _cursorHeld) {
                    RenderUi::ReleaseCursor();
                    const_cast<ResultsWindow*>(this)->_cursorHeld = false;
                }
                return open;
            }
            void SetOpen(bool open) override {
                if (!open) Close();
            }
            FUCK::WindowFlags GetFlags() const override {
                using F = FUCK::WindowFlags;
                // kNoMove/kNoResize pin geometry to GetDefaultPos/Size every
                // frame (host mechanics - see BrowserWindow::GetFlags);
                // kAutoResize collapses (the host's ##Content child is
                // stretch-sized) and kCustomPosition strands the window at
                // the ImGui default - both field-found, avoid.
                return static_cast<F>(
                    static_cast<unsigned>(F::kNoDecoration) |
                    static_cast<unsigned>(F::kNoBackground) |
                    static_cast<unsigned>(F::kNoMove) |
                    static_cast<unsigned>(F::kNoResize) |
                    static_cast<unsigned>(F::kHideHUD) |
                    // kPassInputToGame is load-bearing here for the same
                    // reason it is on the highway (SPIKE-3: without it FLICK
                    // blocks ALL game input while the window is open). This
                    // window is the only one that appears exclusively on the
                    // COMPLETED-song path - the exact path that lost every
                    // input - and the block outlived the close. Taking the
                    // block at all is the bug: closing then has to hand it
                    // back, and it did not. Now it is never taken, so there
                    // is nothing to hand back. Dismissal does not need it:
                    // the confirm key is harvested by our own InputHook
                    // (ImGui never sees those keys) and the mouse works off
                    // the forced cursor.
                    static_cast<unsigned>(F::kPassInputToGame) |
                    static_cast<unsigned>(F::kCloseOnEsc) |
                    // hide behind native menus like the siblings
                    static_cast<unsigned>(F::kCloseOnGameMenu));
            }
            ImVec2 GetDefaultSize() const override {
                const ImVec2 d = FUCK::GetDisplaySize();
                return ImVec2(std::clamp(d.x * 0.44f, 740.0f, 860.0f),
                              std::clamp(d.y * 0.57f, 540.0f, 620.0f));
            }
            ImVec2 GetDefaultPos() const override {
                const ImVec2 d = FUCK::GetDisplaySize();
                const ImVec2 s = GetDefaultSize();
                return ImVec2((d.x - s.x) * 0.5f, (d.y - s.y) * 0.5f);
            }

            void Draw() override {
                auto& bus = UiBus::GetSingleton();
                const bool settlingHost = bus.ResultsHostOpenBlocking();
                struct HostSettle {
                    UiBus& bus;
                    bool   active;
                    ~HostSettle() {
                        if (active) {
                            bus.CompleteResultsFirstDraw();
                            spdlog::info(
                                "[results] first host draw settled; FUCK "
                                "input resumed");
                        }
                    }
                } hostSettle{ bus, settlingHost };
                if (EngineFeed::GetSingleton().active.load(
                        std::memory_order_acquire)) {
                    Close();  // a new session started before we were dismissed
                    return;
                }
                // FLICK's IsWindowAppearing never fires for host-managed
                // IWindows (field-found on the browser) - the cursor latch
                // doubles as the once-per-open gate: snapshot + acquire on
                // first Draw, held across kCloseOnGameMenu hide/re-show,
                // reset by Close() for the next open.
                if (!_snapHeld) {
                    _snap = bus.ReadResults();
                    _snapHeld = true;
                    _openedAt = FUCK::GetTime();
                    RenderUi::AcquireCursor();
                    _cursorHeld = true;
                    // a stray fret/Enter press between session end and this
                    // first draw must not insta-close the box
                    bus.navConfirm.store(false);
                    // P5 celebration state
                    _fx.Clear();
                    _prevFxT         = _openedAt;
                    _nextHoldFlameAt = 0.0;
                    _lastPingedStar  = -1;
                    _burstsFired     = 0;
                    spdlog::info("[results] celebration phrase={}",
                                 _snap.fullCombo ? "FLAWLESS" : "GLORIOUS");
                    // The electric song-end sting pops WITH the menu
                    // (field 2026-07-25: fired at verdict time it landed
                    // ~1.75s before this first draw). Session stashed the
                    // cue index; -1 = no sting.
                    if (_snap.stingCue >= 0) {
                        UiSfx::Fire(
                            static_cast<ui_sfx::Cue>(_snap.stingCue));
                    }
                    // Vanilla instruments: Skyrim's own level-up sting, on
                    // the same first draw and for the same reason. Mutually
                    // exclusive with the electric cue above by construction,
                    // so this is an independent `if` rather than an `else` -
                    // if that ever stops holding, the log will show two
                    // stings and say so, instead of one silently winning.
                    if (_snap.clearSting) {
                        ui_sound::Play(ui_sound::Event::kLevelUp);
                    }
                }
                // Draw into a fixed visual budget instead of stacking rows.
                // Every item stays inside `hi`, so the host child has no
                // reason to create either scrollbar.
                const float  s = FUCK::Scale(1.0f);
                const ImVec2 origin = FUCK::GetCursorScreenPos();
                const ImVec2 avail = FUCK::GetContentRegionAvail();
                // P5 celebration timeline: phrase slam owns the panel
                // until kContentAt, then the upper results block slides
                // up under the phrase, which holds until ~2.1 and fades
                // by 2.7 (field round 2: the first cut's 1.45s read too
                // short). Any confirm/click during the celebration SKIPS
                // it (GH convention) instead of closing the box.
                constexpr double kContentAt = 1.4;
                {
                    const double raw = FUCK::GetTime() - _openedAt;
                    if (raw < kContentAt &&
                        (FUCK::IsMouseClicked(0, false) ||
                         bus.navConfirm.exchange(false))) {
                        _openedAt -= kContentAt;  // jump past the slam
                    }
                }
                const double elapsed = FUCK::GetTime() - _openedAt;
                const bool contentVisible = elapsed >= kContentAt;
                float shakeX = 0.0f, shakeY = 0.0f;
                if (elapsed > 0.12 && elapsed < 0.7) {
                    const float amp = 7.0f * s * static_cast<float>(
                        std::exp(-(elapsed - 0.12) * 5.5));
                    shakeX = amp * static_cast<float>(
                        std::sin(elapsed * 67.0));
                    shakeY = amp * static_cast<float>(
                        std::cos(elapsed * 53.0));
                }
                const ImVec2 lo(origin.x + 4 * s + shakeX,
                                origin.y + 4 * s + shakeY);
                const ImVec2 hi(origin.x + avail.x - 10 * s + shakeX,
                                origin.y + avail.y - 10 * s + shakeY);
                const float cx = (lo.x + hi.x) * 0.5f;
                const float innerL = lo.x + 24 * s;
                const float innerR = hi.x - 24 * s;
                const ImVec4 text = panel::kText;
                const ImVec4 quiet = panel::kQuiet;
                const ImVec4 blue = panel::kBlue;
                // split/lower anchor stays fixed; the sliding copy only
                // moves the upper block (title through stats)
                const auto upper = results_layout::MakeUpper(lo.y, s);
                const float slideT = contentVisible
                    ? std::min(1.0f, static_cast<float>(
                          (elapsed - kContentAt) / 0.35))
                    : 0.0f;
                const float slideE = slideT * slideT * (3.0f - 2.0f * slideT);
                const float slide  = (1.0f - slideE) * 44.0f * s;
                const float cTop   = lo.y + slide;
                const auto upperSlide = results_layout::MakeUpper(cTop, s);
                panel::Draw(lo, hi);

                const double pct =
                    _snap.notesTotal > 0
                        ? 100.0 * _snap.notesHit / _snap.notesTotal
                        : 0.0;
                const int stars = std::clamp(_snap.stars, 0, 5);
                if (contentVisible) {
                FUCK::PushFont(FUCK::GetFont(FUCK::Font::kLarge));
                // "<song> on EASY" - the difficulty rides the title rather
                // than costing a stat column (a fifth column collided
                // OVERSTRUMS into POWER PHRASES, field 2026-07-26).
                //
                // The NAME is fitted to the width left over AFTER the
                // suffix, never the whole string: fitting the combined
                // text would truncate the difficulty off the end, which is
                // the one part that must always survive.
                static constexpr const char* kDiffNames[4] = {
                    "EASY", "MEDIUM", "HARD", "EXPERT"
                };
                const std::string rawName =
                    _snap.songName.empty() ? "SONG COMPLETE"
                                           : _snap.songName;
                std::string suffix;
                if (_snap.difficulty >= 0 && _snap.difficulty < 4) {
                    suffix = std::string(" on ") +
                             kDiffNames[_snap.difficulty];
                }
                const float suffixW =
                    suffix.empty()
                        ? 0.0f
                        : FUCK::CalcTextSize(suffix.c_str()).x;
                const std::string title =
                    FitText(rawName,
                            std::max(1.0f, (innerR - innerL) - suffixW)) +
                    suffix;
                CenterAt(cx, cTop + 14 * s, title.c_str(), text);
                FUCK::PopFont();
                if (!_snap.artist.empty()) {
                    const std::string artist = FitText(_snap.artist,
                                                       innerR - innerL);
                    CenterAt(cx, cTop + 47 * s, artist.c_str(), quiet);
                }
                FUCK::DrawLine(ImVec2(innerL, cTop + 76 * s),
                               ImVec2(innerR, cTop + 76 * s),
                               ImVec4(0.46f, 0.46f, 0.43f, 0.70f), s);

                CenterAt(cx, upperSlide.scoreTitleY, "SCORE", quiet);
                // score ticks up over ~1.2s once the content lands
                const double tickT = std::clamp(
                    (elapsed - kContentAt) / 1.2, 0.0, 1.0);
                const double tickE = tickT * tickT * (3.0 - 2.0 * tickT);
                // P6 SFX: the looped score_tick under the count-up -
                // start on the first counting frame, clickless-stop when
                // the score lands. Frame-driven, so a native menu hiding
                // this window mid-count lets the loop run until the next
                // draw or Close() - accepted; Close() stops it on every
                // dismissal path and StopScoreTick is idempotent.
                if (tickT < 1.0 && !_tickLoopOn) {
                    _tickLoopOn = true;
                    UiSfx::StartScoreTick();
                } else if (tickT >= 1.0 && _tickLoopOn) {
                    _tickLoopOn = false;
                    UiSfx::StopScoreTick();
                }
                char big[64];
                std::snprintf(big, sizeof(big), "%lld",
                              static_cast<long long>(
                                  static_cast<double>(_snap.score) *
                                  tickE + 0.5));
                FUCK::PushFontScaled(FUCK::GetFont(FUCK::Font::kLarge), 1.30f);
                CenterAt(cx, upperSlide.scoreValueY, big, text);
                FUCK::PopFont();
                const double starT = std::max(0.0, elapsed - kContentAt);
                RatingStars(ImVec2(cx, upperSlide.ratingY), stars, starT);
                // star-pip stingers, one as each pip lands
                for (int i = _lastPingedStar + 1; i < stars; ++i) {
                    if (results_anim::StarAt(starT, i, true).alpha >
                        0.0f) {
                        ui_sound::Play(ui_sound::Event::kStarPing);
                        _lastPingedStar = i;
                    } else {
                        break;
                    }
                }
                const char* clearBadge = _snap.newBest
                    ? (_snap.prevBest > 0 ? "NEW BEST" : "FIRST CLEAR")
                    : "";
                const char* comboBadge =
                    _snap.fullCombo ? "FULL COMBO" : "";
                if (*clearBadge || *comboBadge) {
                    const float shine = 0.25f + 0.75f * static_cast<float>(
                        std::sin(elapsed * 4.2) * 0.5 + 0.5);
                    const ImVec4 clearColor = Mix(
                        panel::kGold, ImVec4(1, 1, 0.82f, 1), shine);
                    const ImVec4 comboColor = Mix(
                        kEarnedGold, ImVec4(1.0f, 0.94f, 0.58f, 1), shine);
                    const float gap = 18.0f * s;
                    if (*clearBadge && *comboBadge) {
                        CenterPairAt(cx, upperSlide.badgeY, clearBadge,
                                     clearColor, comboBadge, comboColor,
                                     gap);
                    } else {
                        CenterAt(cx, upperSlide.badgeY,
                                 *clearBadge ? clearBadge : comboBadge,
                                 *clearBadge ? clearColor : comboColor);
                    }
                }

                // Four stats, as before. A fifth column collided
                // "OVERSTRUMS" into "POWER PHRASES" at real widths (field
                // 2026-07-26) - the difficulty now rides the title line
                // instead, where it costs no horizontal budget.
                const char* labels[4] = { "NOTES", "MAX COMBO",
                                          "OVERSTRUMS", "POWER PHRASES" };
                char values[4][48];
                std::snprintf(values[0], sizeof(values[0]), "%d/%d  %.0f%%",
                              _snap.notesHit, _snap.notesTotal, pct);
                std::snprintf(values[1], sizeof(values[1]), "%d",
                              _snap.maxCombo);
                std::snprintf(values[2], sizeof(values[2]), "%d",
                              _snap.overstrums);
                std::snprintf(values[3], sizeof(values[3]), "%d",
                              _snap.spPhrases);
                const float statW = (innerR - innerL) / 4.0f;
                for (int i = 0; i < 4; ++i) {
                    const float x = innerL + statW * (i + 0.5f);
                    CenterAt(x, upperSlide.statsTitleY, labels[i], quiet);
                    CenterAt(x, upperSlide.statsValueY, values[i], text);
                }

                const float splitY = upper.splitY;
                const float footerY = hi.y - 74 * s;
                const auto lower =
                    results_layout::MakeLower(splitY, footerY, s);
                FUCK::DrawLine(ImVec2(innerL, splitY), ImVec2(innerR, splitY),
                               ImVec4(0.46f, 0.46f, 0.43f, 0.70f), s);
                const float midX = cx;
                FUCK::DrawLine(ImVec2(midX, splitY + 12 * s),
                               ImVec2(midX, footerY - 12 * s),
                               ImVec4(0.40f, 0.40f, 0.38f, 0.55f), s);

                char rewardSummary[64];
                if (_snap.goldKnown) {
                    std::snprintf(
                        rewardSummary, sizeof(rewardSummary),
                        _snap.gold > 0 ? "REWARD  %d GOLD"
                                       : "REWARD  NO PURSE",
                        _snap.gold);
                    CenterAt((innerL + midX) * 0.5f,
                             lower.rewardSummaryY, rewardSummary,
                             _snap.gold > 0 ? text : quiet);
                    const std::string reason =
                        results::CompactGoldReason(_snap.goldReason);
                    CenterAt((innerL + midX) * 0.5f,
                             lower.rewardDetailY, reason.c_str(), quiet);
                } else {
                    CenterAt((innerL + midX) * 0.5f,
                             lower.rewardSummaryY,
                             "REWARD  PENDING", quiet);
                }

                CenterAt((midX + innerR) * 0.5f, lower.progressTitleY,
                         results::ProficiencyHeading(_snap.instrument), quiet);
                if (_snap.rankKnown) {
                    const float rcx = (midX + innerR) * 0.5f;
                    const bool rankUp =
                        _snap.rankAfter > _snap.rankBefore;
                    const auto reveal = results_anim::At(
                        FUCK::GetTime() - _openedAt,
                        _snap.expertiseBefore, _snap.expertiseAfter,
                        _snap.xpGain, rankUp);
                    const auto pr =
                        results::ProgressFor(reveal.displayedExpertise);
                    char standing[80];
                    if (pr.maxed) {
                        std::snprintf(standing, sizeof(standing), "%s",
                                      results::MaxStandingLabel());
                    } else if (reveal.rankUpActive) {
                        std::snprintf(
                            standing, sizeof(standing), "RANK UP  %s",
                            results::StandingName(_snap.rankAfter));
                    } else {
                        std::snprintf(
                            standing, sizeof(standing), "%s  %d/%d",
                            results::StandingName(pr.rank),
                            reveal.displayedExpertise - pr.lo,
                            pr.next - pr.lo);
                    }
                    const ImVec4 standingColor = pr.maxed
                        ? kEarnedGold
                        : reveal.rankUpActive
                              ? Mix(text, panel::kGold, reveal.rankUpPulse)
                              : text;
                    if (results::ShowXpGain(reveal.displayedExpertise)) {
                        char xpText[32];
                        std::snprintf(xpText, sizeof(xpText), "+%d XP",
                                      reveal.shownXp);
                        CenterPairAt(rcx, lower.progressSummaryY,
                                     standing, standingColor,
                                     xpText, kEarnedGold, 10.0f * s);
                    } else {
                        CenterAt(rcx, lower.progressSummaryY,
                                 standing, standingColor);
                    }
                    if (!pr.maxed) {
                        const float w = (innerR - midX) - 42 * s;
                        const float h = lower.progressBarH;
                        const ImVec2 c(midX + 21 * s, lower.progressBarY);
                        FUCK::DrawRectFilled(
                            ImVec2(c.x, c.y), ImVec2(c.x + w, c.y + h),
                            ImVec4(0.12f, 0.12f, 0.11f, 0.90f), 2 * s);
                        const float base =
                            std::clamp(reveal.baseFrac, 0.0f, 1.0f);
                        const float meter =
                            std::clamp(reveal.meterFrac, base, 1.0f);
                        if (base > 0.0f) {
                            FUCK::DrawRectFilled(
                                ImVec2(c.x, c.y),
                                ImVec2(c.x + w * base, c.y + h),
                                Mix(blue, quiet, 0.42f), 2 * s);
                        }
                        if (meter > base) {
                            FUCK::DrawRectFilled(
                                ImVec2(c.x + w * base, c.y),
                                ImVec2(c.x + w * meter, c.y + h),
                                kEarnedGold, 2 * s);
                        }
                        if (reveal.rankUpPulse > 0.0f) {
                            const ImVec4 glow(
                                panel::kGold.x, panel::kGold.y,
                                panel::kGold.z,
                                0.28f + 0.58f * reveal.rankUpPulse);
                            FUCK::DrawRect(
                                ImVec2(midX + 8 * s, splitY + 4 * s),
                                ImVec2(innerR, footerY - 8 * s),
                                glow, 4 * s,
                                (1.0f + reveal.rankUpPulse) * s);
                        }
                    }
                }

                FUCK::DrawLine(ImVec2(innerL, footerY),
                               ImVec2(innerR, footerY),
                               ImVec4(0.46f, 0.46f, 0.43f, 0.70f), s);
                const ImVec2 buttonLo(cx - 78 * s, footerY + 18 * s);
                const ImVec2 buttonSize(156 * s, 34 * s);
                const ImVec2 mouse = FUCK::GetMousePos();
                const ImVec2 buttonHi(buttonLo.x + buttonSize.x,
                                      buttonLo.y + buttonSize.y);
                const bool hovered = _cursorHeld &&
                                     panel::Contains(mouse, buttonLo, buttonHi);
                const bool clicked = hovered && FUCK::IsMouseClicked(0, false);
                panel::ButtonFrame(buttonLo, buttonSize, hovered, true);
                CenterAt(cx, buttonLo.y + 5 * s, "CONTINUE", text);
                if (clicked || bus.navConfirm.exchange(false)) {
                    ui_sound::Play(ui_sound::Event::kConfirm);
                    Close();
                    return;
                }
                }  // contentVisible

                // celebration layers float over the plate (and over the
                // content once it slides in)
                UpdateAndDrawFlames(lo, hi, cx, s, elapsed);
                DrawPhrase(lo, hi, cx, elapsed);
            }

        private:
            // P5: GLORIOUS!/FLAWLESS! atlas strip slam-in. FLICK text
            // cannot rotate; the pre-rendered strip rotates like any
            // quad (banner precedent).
            void DrawPhrase(const ImVec2& lo, const ImVec2& hi, float cx,
                            double elapsed) {
                if (elapsed > 2.7 || !_hwR.Ready()) { return; }
                const float t  = static_cast<float>(elapsed);
                const float in = std::min(1.0f, t / 0.12f);
                const float inE = in * in * (3.0f - 2.0f * in);
                float scale = 2.4f - 1.4f * inE;
                float alpha = inE;
                if (t > 0.12f) {
                    // settle wobble, then a slow proud breathe while it
                    // holds (field round 2: longer presence)
                    scale = 1.0f + 0.05f *
                        static_cast<float>(std::exp(-(t - 0.12) * 6.0) *
                                           std::sin((t - 0.12) * 28.0)) +
                        0.02f * static_cast<float>(
                            std::sin((t - 0.12) * 2.6));
                }
                if (t > 2.1f) {
                    const float f = (t - 2.1f) / 0.6f;
                    alpha = 1.0f - f;
                    scale += 0.15f * f;
                }
                const float rot = -0.045f +
                    0.05f * static_cast<float>(std::exp(-t * 5.0) *
                                               std::sin(t * 40.0));
                const float H =
                    (hi.x - lo.x) * 0.115f * scale;   // strip is 8 cells
                const float W  = H * 8.0f;
                // glide up out of the score's way as the content slides
                // in (1.4 = kContentAt)
                const float up = std::clamp(
                    (t - 1.4f) / 0.35f, 0.0f, 1.0f);
                const float upE = up * up * (3.0f - 2.0f * up);
                const float py  = lo.y + (hi.y - lo.y) *
                                     (0.42f - 0.24f * upE);
                const auto  uv = _snap.fullCombo ? hw::WinFlawlessUv()
                                                 : hw::WinGloriousUv();
                const hw::RGBA tint = _snap.fullCombo
                    ? hw::RGBA{ 0.72f, 0.96f, 1.00f, alpha }
                    : hw::RGBA{ 0.98f, 0.80f, 0.30f, alpha };
                const float ca = std::cos(rot), sa = std::sin(rot);
                auto pt = [&](float lx, float ly) {
                    return hw::V2{ cx + lx * ca - ly * sa,
                                   py + lx * sa + ly * ca };
                };
                const hw::V2 p[4] = { pt(-W * 0.5f, -H * 0.5f),
                                      pt(W * 0.5f, -H * 0.5f),
                                      pt(W * 0.5f, H * 0.5f),
                                      pt(-W * 0.5f, H * 0.5f) };
                _hwR.QuadUv(uv, p, tint);
            }

            // Field round 2: FLAMES, not confetti - the celebration
            // borrows the highway's fire language. Gold fire for
            // GLORIOUS!, cold fire for FLAWLESS!.
            void UpdateAndDrawFlames(const ImVec2& lo, const ImVec2& hi,
                                     float cx, float s, double elapsed) {
                if (!_hwR.Ready()) { return; }
                const hw::RGBA fire = _snap.fullCombo
                    ? hw::RGBA{ 0.55f, 0.92f, 1.00f, 1.0f }
                    : hw::RGBA{ 1.00f, 0.62f, 0.18f, 1.0f };
                const float gw = (hi.x - lo.x) * 0.17f;
                const float phraseBase =
                    lo.y + (hi.y - lo.y) * 0.42f + gw * 0.35f;
                if (elapsed >= 0.12 && _burstsFired == 0) {
                    // impact: a row of fire under the slammed phrase
                    for (int i = -1; i <= 1; ++i) {
                        _fx.FlameBurst(cx + i * gw * 1.6f, phraseBase,
                                       gw, fire, 1.0f);
                    }
                    _burstsFired = 1;
                }
                if (elapsed >= 0.30 && _burstsFired == 1) {
                    // panel bottom corners answer
                    _fx.FlameBurst(lo.x + gw, hi.y - 8.0f * s,
                                   gw * 0.8f, fire, 1.0f);
                    _fx.FlameBurst(hi.x - gw, hi.y - 8.0f * s,
                                   gw * 0.8f, fire, 1.0f);
                    _burstsFired = 2;
                }
                // a simmer at the phrase base while it holds
                const double nowT = FUCK::GetTime();
                if (elapsed > 0.5 && elapsed < 2.1 &&
                    nowT >= _nextHoldFlameAt) {
                    const float jitter = static_cast<float>(
                        std::sin(elapsed * 9.7));
                    _fx.FlameBurst(cx + jitter * gw * 1.2f, phraseBase,
                                   gw * 0.55f, fire, 1.0f);
                    _nextHoldFlameAt = nowT + 0.14;
                }
                _fx.Update(std::clamp(nowT - _prevFxT, 0.0, 0.1));
                _prevFxT = nowT;
                _fx.ForEach([&](const hw::FxParticle& q) {
                    const float t2 = q.age / q.life;
                    const float sz =
                        q.size0 + (q.size1 - q.size0) * t2;
                    hw::RGBA c = q.tint;
                    c.a *= 1.0f - t2;
                    hw::Sprite spr = q.sprite;
                    if (q.fbFrames > 1) {
                        spr = static_cast<hw::Sprite>(
                            static_cast<int>(q.sprite) +
                            hw::FlipbookFrame(t2, q.fbFrames));
                        // upright plume, base anchored (highway drawer
                        // convention)
                        const float ex = sz, eh = sz * 3.4f;
                        const hw::V2 p[4] = { { q.x - ex, q.y - eh },
                                              { q.x + ex, q.y - eh },
                                              { q.x + ex, q.y },
                                              { q.x - ex, q.y } };
                        _hwR.Quad(spr, p, c);
                    } else {
                        const hw::V2 p[4] = { { q.x - sz, q.y - sz },
                                              { q.x + sz, q.y - sz },
                                              { q.x + sz, q.y + sz },
                                              { q.x - sz, q.y + sz } };
                        _hwR.Quad(spr, p, c);
                    }
                });
            }

            void Close() {
                UiBus::GetSingleton().CloseResults();
                _snapHeld = false;
                _openedAt = 0.0;
                // Every dismissal path, including the new-session bail at
                // the top of Draw - a confirm mid-count-up must not leave
                // the tick looping. Silent when the loop already ended.
                _tickLoopOn = false;
                UiSfx::StopScoreTick();
                if (_cursorHeld) {
                    RenderUi::ReleaseCursor();
                    _cursorHeld = false;
                }
            }
            UiBus::Results _snap;
            bool           _snapHeld = false;
            bool           _cursorHeld = false;
            double         _openedAt = 0.0;
            // P5 celebration (render-thread only)
            hw::FlickRenderer _hwR;
            hw::FxPool        _fx;
            double            _prevFxT         = 0.0;
            double            _nextHoldFlameAt = 0.0;
            int               _lastPingedStar  = -1;
            int               _burstsFired     = 0;
            bool              _tickLoopOn      = false;
        };
        ResultsWindow g_results;
    }

    void RegisterResultsWindow() { FUCK::RegisterWindow(&g_results); }
}
