// src/render/SettingsTool.cpp
#include "PCH.h"
#include "render/SettingsTool.h"

#include "Settings.h"
#include "audio/AudioEngine.h"
#include "game/AtronachRecipe.h"
#include "game/BindingEditLogic.h"
#include "game/EngineFeed.h"
#include "game/InputHook.h"
#include "game/KeyNamesLogic.h"
#include "game/PerformanceCamera.h"
#include "game/SongLibrary.h"
#include "game/SgtProgression.h"
#include "game/StarLedger.h"
#include "game/StarsLogic.h"
#include "game/UiSfx.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h (house ordering)

#include "FUCK_API.h"
#include "render/UiSound.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>

namespace SH {
    namespace {
        constexpr const char* kDiffNames[4] = { "Easy", "Medium", "Hard",
                                                "Expert" };
        constexpr const char* kInstNames[3] = { "Lute", "Flute", "Drum" };
        // SGT expertise tiers: Clueless <26 / Beginner <46 / Decent <66 /
        // Advanced <86 / Pro. Buttons jump to a mid-tier value.
        constexpr const char* kTierNames[5]  = { "Clueless", "Beginner",
                                                 "Decent", "Advanced",
                                                 "Pro" };
        constexpr int         kTierValues[5] = { 10, 35, 55, 75, 95 };

        const char* TierOf(int a_expertise) {
            if (a_expertise < 26) { return kTierNames[0]; }
            if (a_expertise < 46) { return kTierNames[1]; }
            if (a_expertise < 66) { return kTierNames[2]; }
            if (a_expertise < 86) { return kTierNames[3]; }
            return kTierNames[4];
        }

        bool SliderDouble(const char* a_label, double& a_value,
                          float a_min, float a_max, const char* a_format) {
            float value = static_cast<float>(a_value);
            if (!FUCK::SliderFloat(a_label, &value, a_min, a_max,
                                   a_format)) {
                return false;
            }
            a_value = value;
            return true;
        }

        bool SliderMilliseconds(const char* a_label, double& a_seconds,
                                float a_minMs, float a_maxMs) {
            float milliseconds = static_cast<float>(a_seconds * 1000.0);
            if (!FUCK::SliderFloat(a_label, &milliseconds, a_minMs, a_maxMs,
                                   "%.0f ms")) {
                return false;
            }
            a_seconds = static_cast<double>(milliseconds) / 1000.0;
            return true;
        }

        bool SliderPercent(const char* a_label, double& a_ratio,
                           float a_min = 0.0f, float a_max = 1.0f) {
            float percent = static_cast<float>(a_ratio * 100.0);
            if (!FUCK::SliderFloat(a_label, &percent, a_min * 100.0f,
                                   a_max * 100.0f, "%.0f%%")) {
                return false;
            }
            a_ratio = static_cast<double>(percent) / 100.0;
            return true;
        }

        // The FLICK SIDEBAR lists ITools, not IWindows (FUCK_API.h: "Implement
        // this to add a new Tool to the FUCK Sidebar") - the settings page
        // lives here so it shows up under BardHero in FLICK's own panel; the
        // in-game browse UI stays clean (user call 2026-07-20). FLICK owns
        // the panel chrome, cursor and close semantics - Draw() is content
        // only.
        class SettingsTool final : public FUCK::ITool {
        public:
            // the sidebar shows Name() as the list entry - it must identify
            // the MOD, not say "Settings" (user call 2026-07-20)
            const char* Name() const override { return "Bard Hero"; }

            void OnOpen() override {
                const auto& st = Settings::GetSingleton();
                // One-shot volumes display as percent of default (100% =
                // the field-tuned 0.02 gain) - a raw 0..1 slider idled
                // with its knob at 2% of the track (UiSfxLogic.h).
                _missPct = ui_sfx::PercentFromGain(
                    static_cast<float>(st.missSfxVolume));
                _uiPct = ui_sfx::PercentFromGain(
                    static_cast<float>(st.uiSfxVolume));
                _songPct = ui_sfx::SongPercentFromGain(
                    static_cast<float>(st.songVolume));
                _instSel       = static_cast<int>(
                    StarLedger::GetSingleton().ActiveInstrument());
                SgtProgression::PostUiSample();
                _nextSample = FUCK::GetTime() + 1.0;
            }

            void Draw() override {
                auto& st   = Settings::GetSingleton();
                bool  save = false;

                const auto drawGeneral = [&] {
                    FUCK::SeparatorText("Session");
                    int diff = std::clamp(st.difficulty, 0, 3);
                    FUCK::SetNextItemWidth(FUCK::Scale(140.0f));
                    if (FUCK::Combo("Default chart difficulty", &diff,
                                    kDiffNames, 4)) {
                        ui_sound::Play(ui_sound::Event::kConfirm);
                        st.difficulty = diff;
                        save = true;
                    }
                    if (FUCK::Checkbox("Pause world (menus + browse)",
                                       &st.pauseWorld)) {
                        ui_sound::Play(ui_sound::Event::kConfirm);
                        save = true;
                    }

                    // Everything here was INI-only until 2026-07-26. These
                    // are the settings a player actually wants to reach -
                    // how the performance LOOKS - and having them only in a
                    // text file while hit-window microseconds had sliders
                    // was the wrong way round.
                    FUCK::SeparatorText("Presentation");
                    if (FUCK::Checkbox("Cinematic performance camera",
                                       &st.performanceCameraDirector)) {
                        ui_sound::Play(ui_sound::Event::kConfirm);
                        PerformanceCamera::SetEnabled(
                            st.performanceCameraDirector);
                        save = true;
                    }
                    FUCK::TextDisabled(
                        "Cuts between angles on the beat, shakes with the "
                        "music, pushes in on Star Power");
                    // The one caveat that would otherwise read as a broken
                    // toggle: the camera hook is installed at load, so the
                    // FIRST time this is switched on it cannot take effect
                    // until the game restarts. Say so where it is switched,
                    // not in a log file.
                    if (st.performanceCameraDirector &&
                        !PerformanceCamera::Hooked()) {
                        FUCK::TextDisabled(
                            "Takes effect after a game restart");
                    }
                    // Mutually exclusive in effect - the director takes
                    // precedence - so the fallback is disabled rather than
                    // left looking like it still does something.
                    FUCK::BeginDisabled(st.performanceCameraDirector);
                    if (FUCK::Checkbox("Slow vanity orbit (simpler)",
                                       &st.performanceVanityCamera)) {
                        ui_sound::Play(ui_sound::Event::kConfirm);
                        save = true;
                    }
                    FUCK::EndDisabled();
                    if (FUCK::Checkbox("Rich note effects",
                                       &st.richFx)) {
                        ui_sound::Play(ui_sound::Event::kConfirm);
                        save = true;
                    }
                    FUCK::TextDisabled(
                        "Hit bursts, sustain flames, fret kicks, beat pulse");
                    {
                        // The threshold IS the toggle - streakfire::Enabled
                        // reads 0 as off, so a second bool would be a second
                        // source of truth for the same fact. The last
                        // non-zero value is remembered so a player who set
                        // 45 in the INI gets 45 back rather than the default.
                        static int lastOn = 0;
                        if (st.streakFireHandsAt > 0) {
                            lastOn = st.streakFireHandsAt;
                        }
                        bool cloak = st.streakFireHandsAt > 0;
                        if (FUCK::Checkbox("Streak flame cloak", &cloak)) {
                            st.streakFireHandsAt =
                                cloak ? (lastOn > 0 ? lastOn : 30) : 0;
                            ui_sound::Play(ui_sound::Event::kConfirm);
                            save = true;
                        }
                        FUCK::TextDisabled(
                            "Catch fire while a note streak holds. Silent");
                    }
                    {
                        // Seconds of lookahead is the honest unit and is what
                        // the highway is written against, but nobody thinks
                        // in it. Show the Clone Hero style number and convert.
                        float speed = static_cast<float>(
                            st.highwayLookaheadSec);
                        FUCK::SetNextItemWidth(FUCK::Scale(140.0f));
                        if (FUCK::SliderFloat("Note speed", &speed, 0.55f,
                                              2.20f, "%.2fs")) {
                            st.highwayLookaheadSec = speed;
                            save = true;
                        }
                        FUCK::TextDisabled(
                            "How far ahead notes appear. Lower is faster "
                            "and reads tighter");
                    }

                    FUCK::SeparatorText("Song library");
                    auto& library = SongLibrary::GetSingleton();
                    FUCK::BeginDisabled(library.Scanning());
                    if (FUCK::Button("Rescan songs")) {
                        ui_sound::Play(ui_sound::Event::kConfirm);
                        library.Rescan();
                    }
                    FUCK::EndDisabled();
                    if (library.Scanning()) {
                        FUCK::SameLine();
                        FUCK::Spinner("##settings_scan", FUCK::Scale(8.0f),
                                      FUCK::Scale(2.0f),
                                      ImVec4(0.70f, 0.70f, 0.67f, 1.0f));
                        FUCK::SameLine();
                        FUCK::TextDisabled("Scanning...");
                    } else if (library.BadCount() > 0) {
                        FUCK::SameLine();
                        FUCK::TextDisabled("%d rejected", library.BadCount());
                    }

                    FUCK::SeparatorText("Audio");
                    if (FUCK::SliderFloat("Song volume", &_songPct, 0.0f,
                                          ui_sfx::kSongSliderMaxPercent,
                                          "%.0f%%")) {
                        const float gain =
                            ui_sfx::SongGainFromPercent(_songPct);
                        st.songVolume = gain;
                        save = true;
                        auto& feed = EngineFeed::GetSingleton();
                        std::scoped_lock lk(feed.mx);
                        if (feed.audio) {
                            feed.audio->SetSongVolume(gain);
                        }
                    }
                    FUCK::TextDisabled("Applies immediately, even mid-song");
                    if (FUCK::SliderFloat("Miss SFX volume", &_missPct, 0.0f,
                                          ui_sfx::kOneShotSliderMaxPercent,
                                          "%.0f%%")) {
                        st.missSfxVolume = ui_sfx::GainFromPercent(_missPct);
                        save = true;
                    }
                    FUCK::TextDisabled(
                        "Miss SFX level applies from the next song");
                    if (FUCK::SliderFloat("UI SFX volume", &_uiPct, 0.0f,
                                          ui_sfx::kOneShotSliderMaxPercent,
                                          "%.0f%%")) {
                        const float gain = ui_sfx::GainFromPercent(_uiPct);
                        st.uiSfxVolume = gain;
                        save = true;
                        // Live: this bank loads once per process, so there
                        // is no next-session reload to pick the value up.
                        UiSfx::SetVolume(gain);
                    }
                    FUCK::TextDisabled(
                        "Banners, score tick, star power and song-end "
                        "stings; applies immediately");

                    FUCK::SeparatorText("SGT");
                    if (FUCK::Checkbox("Play-idle keep-alive",
                                       &st.sgtIdleKeepAlive)) {
                        ui_sound::Play(ui_sound::Event::kConfirm);
                        save = true;
                    }

                };

                const auto drawDifficulty = [&] {
                    auto& tuning = st.tuning;
                    FUCK::TextDisabled(
                        "Timing changes apply when the next song starts");

                    FUCK::SeparatorText("Notes and timing");
                    save |= SliderDouble("Hit window scale",
                                         tuning.hitWindowScale, 0.50f, 2.00f,
                                         "%.2fx");
                    save |= SliderMilliseconds("Strum leniency",
                                               tuning.strumLeniencySec,
                                               0.0f, 200.0f);
                    save |= SliderMilliseconds("Early strum leniency",
                                               tuning.earlyStrumLeniencySec,
                                               0.0f, 150.0f);
                    save |= SliderMilliseconds("HOPO leniency",
                                               tuning.hopoLeniencySec,
                                               0.0f, 250.0f);
                    save |= SliderMilliseconds("Sustain drop leniency",
                                               tuning.sustainDropLeniencySec,
                                               0.0f, 150.0f);
                    if (FUCK::Checkbox("Accept arbitrarily early strums",
                                       &tuning.infiniteFrontEnd)) {
                        save = true;
                    }
                    if (FUCK::Checkbox("Anti-ghosting",
                                       &tuning.antiGhosting)) {
                        save = true;
                    }
                    save |= FUCK::SliderInt("Maximum multiplier",
                                            &tuning.maxMultiplier, 1, 8);

                    FUCK::SeparatorText("Glory meter");
                    save |= SliderDouble("Hits for a full meter",
                                         tuning.gloryMeterSpanHits, 50.0f,
                                         1000.0f, "%.0f");
                    save |= SliderDouble("Mistake weight",
                                         tuning.gloryBadWeight, 0.50f, 30.0f,
                                         "%.2f hits");
                    save |= SliderDouble("Star Power recovery",
                                         tuning.gloryStarPowerHitScale, 0.0f,
                                         3.0f, "%.2fx");
                    save |= SliderDouble("Star Power damage",
                                         tuning.gloryStarPowerBadScale, 0.0f,
                                         2.0f, "%.2fx");
                    save |= SliderDouble("Opening protection",
                                         tuning.gloryOpeningSec, 0.0f, 15.0f,
                                         "%.1f s");
                    save |= SliderPercent("Opening damage",
                                          tuning.gloryOpeningBadScale);
                    save |= FUCK::SliderInt("Clean hits before recovery",
                                            &tuning.gloryRecoveryHits, 0, 100);
                    save |= SliderPercent("Red below",
                                          tuning.gloryRedBelow, 0.05f, 0.90f);
                    save |= SliderPercent("Green at",
                                          tuning.gloryGreenAt, 0.05f, 0.95f);

                    FUCK::SeparatorText("Failure and audience");
                    save |= SliderPercent("Failure danger below",
                                          tuning.failureDangerBelow, 0.01f,
                                          0.90f);
                    save |= SliderPercent("Danger clears at",
                                          tuning.failureRecoverAt, 0.01f,
                                          1.0f);
                    save |= SliderDouble("Failure grace",
                                         tuning.failureGraceSec, 0.0f, 15.0f,
                                         "%.1f s");
                    save |= SliderDouble("Failure enabled after",
                                         tuning.failureStartSec, 0.0f, 30.0f,
                                         "%.1f s");
                    save |= FUCK::SliderInt("Further mistakes to fail",
                                            &tuning.failureFurtherBad, 1, 20);
                    save |= SliderDouble("NPC comment delay",
                                         tuning.audienceCommentDelaySec, 0.0f,
                                         30.0f, "%.1f s");
                    FUCK::TextDisabled(
                        "Failure and ending reactions remain immediate");

                    if (FUCK::Button("Restore recommended difficulty")) {
                        ui_sound::Play(ui_sound::Event::kConfirm);
                        tuning = difficulty::Tuning{};
                        save = true;
                    }
                    if (save) { difficulty::Normalize(tuning); }
                };

                const auto drawProgression = [&] {
                    FUCK::SeparatorText("Star ratings");
                    save |= SliderPercent("1 star", st.star1);
                    save |= SliderPercent("2 stars", st.star2);
                    save |= SliderPercent("3 stars", st.star3);
                    save |= SliderPercent("4 stars", st.star4);
                    save |= SliderPercent("5 stars", st.star5);
                    st.star1 = std::clamp(st.star1, 0.0, 1.0);
                    st.star2 = std::clamp(st.star2, st.star1, 1.0);
                    st.star3 = std::clamp(st.star3, st.star2, 1.0);
                    st.star4 = std::clamp(st.star4, st.star3, 1.0);
                    st.star5 = std::clamp(st.star5, st.star4, 1.0);

                    FUCK::SeparatorText("Rewards and reactions");
                    save |= FUCK::SliderInt("XP per star", &st.xpPerStar,
                                            0, 20);
                    save |= FUCK::SliderInt("Five-star XP bonus",
                                            &st.xpBonus5, 0, 20);
                    save |= FUCK::SliderInt("Positive reaction from",
                                            &st.reactionPositiveStars, 0, 5,
                                            "%d stars");
                    save |= FUCK::SliderInt("Neutral reaction from",
                                            &st.reactionNeutralStars, 0, 5,
                                            "%d stars");
                    st.reactionPositiveStars =
                        std::max(st.reactionPositiveStars,
                                 st.reactionNeutralStars);
                    save |= FUCK::SliderInt("Payout from",
                                            &st.payoutMinStars, 1, 5,
                                            "%d stars");

                    FUCK::SeparatorText("Rank gates");
                    if (FUCK::Checkbox("Require proven songs for ranks",
                                       &st.rankGate)) {
                        save = true;
                    }
                    save |= FUCK::SliderInt("Songs for rank 2",
                                            &st.gateSongs2, 1, 20);
                    save |= FUCK::SliderInt("Songs for rank 3",
                                            &st.gateSongs3, 1, 20);
                    save |= FUCK::SliderInt("Songs for rank 4",
                                            &st.gateSongs4, 1, 20);
                    save |= FUCK::SliderInt("Songs for rank 5",
                                            &st.gateSongs5, 1, 20);
                    if (FUCK::Checkbox("Rank 5 requires a five-star song",
                                       &st.gate5NeedsFiveStar)) {
                        save = true;
                    }

                };

                // Its own tab rather than a section at the bottom of General.
                // These change what the game is rather than how it is set up,
                // so they do not belong in the same scroll as audio sliders -
                // and a player has to go looking for them instead of meeting
                // Autoplay on the way past the volume controls.
                const auto drawCheats = [&] {
                    FUCK::TextDisabled(
                        "Shortcuts around the normal rules. Nothing here is "
                        "needed to play.");

                    FUCK::SeparatorText("Performance");
                    if (FUCK::Checkbox("Autoplay (auto-hit all notes)",
                                       &st.autoPlay)) {
                        ui_sound::Play(ui_sound::Event::kConfirm);
                        save = true;
                    }
                    FUCK::TextDisabled(
                        "Frets/strum are ignored while on; pause still works. "
                        "Score, stars, XP and gold all still count.");

                    FUCK::SeparatorText("Items");
                    // The Doom Lute is normally earned at the Atronach Forge.
                    // Greyed out rather than hidden when the addon is absent,
                    // so it reads as "not installed" instead of the button
                    // silently doing nothing.
                    const bool haveLute =
                        SH::AtronachRecipe::DoomLuteAvailable();
                    FUCK::BeginDisabled(!haveLute);
                    if (FUCK::Button("Give me the Doom Lute")) {
                        ui_sound::Play(ui_sound::Event::kConfirm);
                        SH::AtronachRecipe::GiveDoomLuteToPlayer();
                    }
                    FUCK::EndDisabled();
                    FUCK::TextDisabled(
                        haveLute
                            ? "Puts one in your inventory, skipping the "
                              "Atronach Forge recipe"
                            : "Needs the Doom Lute addon, which is not "
                              "installed");

                    // Moved here from Progression, where it sat behind a
                    // "(dev)" label. It calls CheatSetExpertise and hands the
                    // player a bard rank they did not earn - that is a cheat
                    // whoever is reading the label, and Progression is for
                    // tuning how progress is EARNED, not for skipping it.
                    FUCK::SeparatorText("Bard rank");
                    if (!SgtProgression::Available()) {
                        FUCK::TextDisabled(
                            "Skyrim's Got Talent expertise globals not found");
                    } else {
                        if (FUCK::GetTime() >= _nextSample) {
                            SgtProgression::PostUiSample();
                            _nextSample = FUCK::GetTime() + 1.0;
                        }
                        for (int i = 0; i < 3; ++i) {
                            const int v = SgtProgression::UiSampled(
                                static_cast<stars::Instrument>(i));
                            if (v < 0) {
                                FUCK::Text("%s  -", kInstNames[i]);
                            } else {
                                FUCK::Text("%s  %d  (%s)", kInstNames[i], v,
                                           TierOf(v));
                            }
                        }
                        FUCK::SetNextItemWidth(FUCK::Scale(120.0f));
                        if (FUCK::Combo("Set instrument", &_instSel,
                                        kInstNames, 3)) {
                            ui_sound::Play(ui_sound::Event::kConfirm);
                        }
                        for (int t = 0; t < 5; ++t) {
                            if (t > 0) { FUCK::SameLine(); }
                            char lbl[32];
                            std::snprintf(lbl, sizeof(lbl), "%s##tier%d",
                                          kTierNames[t], t);
                            if (FUCK::Button(lbl)) {
                                ui_sound::Play(ui_sound::Event::kConfirm);
                                SgtProgression::CheatSetExpertise(
                                    static_cast<stars::Instrument>(_instSel),
                                    kTierValues[t]);
                            }
                        }
                        if (SgtProgression::ClampSuspended()) {
                            FUCK::TextColored(
                                ImVec4(0.95f, 0.75f, 0.30f, 1.0f),
                                "Rank-gate clamp SUSPENDED");
                            FUCK::SameLine();
                            if (FUCK::Button("Re-arm clamp")) {
                                ui_sound::Play(ui_sound::Event::kConfirm);
                                SgtProgression::SetClampSuspended(false);
                            }
                        } else {
                            FUCK::TextDisabled(
                                "Setting a tier suspends the rank-gate clamp");
                        }
                    }
                };

                const auto drawBindings = [&] {
                    FUCK::TextDisabled(
                        "Click a binding, then press the key or button. "
                        "Changes apply immediately.");

                    // Poll the hook once per frame for a capture result.
                    if (_bindArmedSlot >= 0) {
                        const int got = InputHook::TakeCaptureResult();
                        if (got >= 0) {
                            int* kb[] = { &st.fret1Key, &st.fret2Key,
                                          &st.fret3Key, &st.fret4Key,
                                          &st.fret5Key, &st.strumKey,
                                          &st.spKey,    &st.whammyKey,
                                          &st.pauseKey };
                            int* gp[] = { &st.gamepadFret1,
                                          &st.gamepadFret2,
                                          &st.gamepadFret3,
                                          &st.gamepadFret4,
                                          &st.gamepadFret5,
                                          &st.gamepadStrum1,
                                          &st.gamepadStrum2,
                                          &st.gamepadSp,
                                          &st.gamepadWhammy,
                                          &st.gamepadPause };
                            const bool pad = _bindArmedSlot >= 100;
                            const std::size_t idx = static_cast<
                                std::size_t>(pad ? _bindArmedSlot - 100
                                                 : _bindArmedSlot);
                            const int loser = pad
                                ? binding_edit::Assign(gp, 10, idx, got)
                                : binding_edit::Assign(kb, 9, idx, got);
                            _bindStolenSlot =
                                loser < 0 ? -1
                                          : (pad ? 100 + loser : loser);
                            _bindArmedSlot = -1;
                            ui_sound::Play(ui_sound::Event::kConfirm);
                            save = true;
                            InputHook::RefreshBinds();
                        }
                    }

                    static constexpr const char* kRow[9] = {
                        "Fret 1", "Fret 2", "Fret 3", "Fret 4", "Fret 5",
                        "Strum",  "Star Power", "Whammy", "Pause",
                    };
                    static constexpr const char* kPadRow[10] = {
                        "Fret 1", "Fret 2", "Fret 3", "Fret 4", "Fret 5",
                        "Strum up", "Strum down", "Star Power", "Whammy",
                        "Pause",
                    };

                    // One row: label, bind button, small clear button.
                    // IDs use the ##suffix idiom (the tier buttons in
                    // drawCheats) - FUCK exposes no ID stack.
                    const auto bindRow = [&](const char* label, int slotId,
                                             int* value, bool padCol,
                                             bool warnUnbound) {
                        const bool armed = _bindArmedSlot == slotId;
                        std::string name;
                        if (armed) {
                            name = "press a key...";
                        } else if (*value == 0) {
                            name = "Unbound";
                        } else {
                            name = padCol
                                ? key_names::PadLabel(
                                      static_cast<std::uint32_t>(*value))
                                : key_names::DikLabel(
                                      static_cast<std::uint32_t>(*value));
                        }
                        const bool tint =
                            !armed && *value == 0 &&
                            (warnUnbound || _bindStolenSlot == slotId);
                        if (tint) {
                            FUCK::TextColored(
                                ImVec4(0.95f, 0.75f, 0.30f, 1.0f), "%s",
                                label);
                        } else {
                            FUCK::Text("%s", label);
                        }
                        FUCK::SameLine(FUCK::Scale(130.0f));
                        char btn[80];
                        std::snprintf(btn, sizeof(btn), "%s##bind%d",
                                      name.c_str(), slotId);
                        if (FUCK::Button(btn)) {
                            ui_sound::Play(ui_sound::Event::kConfirm);
                            if (armed) {
                                InputHook::CancelBindCapture();
                                _bindArmedSlot = -1;
                            } else {
                                _bindArmedSlot = slotId;
                                InputHook::BeginBindCapture(
                                    padCol
                                        ? InputHook::BindDevice::kGamepad
                                        : InputHook::BindDevice::
                                              kKeyboard);
                            }
                        }
                        FUCK::SameLine();
                        char small[32];
                        if (armed) {
                            std::snprintf(small, sizeof(small),
                                          "Cancel##c%d", slotId);
                            if (FUCK::Button(small)) {
                                InputHook::CancelBindCapture();
                                _bindArmedSlot = -1;
                            }
                        } else if (*value != 0) {
                            std::snprintf(small, sizeof(small), "x##x%d",
                                          slotId);
                            if (FUCK::Button(small)) {
                                *value = 0;
                                save   = true;
                                InputHook::RefreshBinds();
                            }
                        }
                    };

                    FUCK::SeparatorText("Keyboard");
                    {
                        int* kb[] = { &st.fret1Key, &st.fret2Key,
                                      &st.fret3Key, &st.fret4Key,
                                      &st.fret5Key, &st.strumKey,
                                      &st.spKey,    &st.whammyKey,
                                      &st.pauseKey };
                        for (int i = 0; i < 9; ++i) {
                            // Frets and strum unbound = unplayable; tint.
                            bindRow(kRow[i], i, kb[i], false, i <= 5);
                        }
                        if (FUCK::Button("Reset keyboard to defaults")) {
                            const auto d =
                                binding_edit::KeyboardDefaults();
                            for (int i = 0; i < 9; ++i) { *kb[i] = d[i]; }
                            _bindStolenSlot = -1;
                            ui_sound::Play(ui_sound::Event::kConfirm);
                            save = true;
                            InputHook::RefreshBinds();
                        }
                        FUCK::TextDisabled(
                            "The bridge layout (A,S,J,K,L / arrows) is a "
                            "second, fixed column - edit the INI to "
                            "change it");
                    }

                    FUCK::SeparatorText("Gamepad");
                    {
                        int* gp[] = { &st.gamepadFret1, &st.gamepadFret2,
                                      &st.gamepadFret3, &st.gamepadFret4,
                                      &st.gamepadFret5, &st.gamepadStrum1,
                                      &st.gamepadStrum2, &st.gamepadSp,
                                      &st.gamepadWhammy,
                                      &st.gamepadPause };
                        for (int i = 0; i < 10; ++i) {
                            bindRow(kPadRow[i], 100 + i, gp[i], true,
                                    i <= 6);
                        }
                        if (FUCK::Button("Reset gamepad to defaults")) {
                            const auto d = binding_edit::GamepadDefaults();
                            for (int i = 0; i < 10; ++i) {
                                *gp[i] = d[i];
                            }
                            _bindStolenSlot = -1;
                            ui_sound::Play(ui_sound::Event::kConfirm);
                            save = true;
                            InputHook::RefreshBinds();
                        }
                        FUCK::TextDisabled(
                            "A / Cross doubling as a fret and menu-"
                            "confirm is by design");
                    }
                };

                if (FUCK::BeginTabBar("##BardHeroSettings")) {
                    if (FUCK::BeginTabItem("General")) {
                        drawGeneral();
                        FUCK::EndTabItem();
                    }
                    if (FUCK::BeginTabItem("Difficulty")) {
                        drawDifficulty();
                        FUCK::EndTabItem();
                    }
                    if (FUCK::BeginTabItem("Progression")) {
                        drawProgression();
                        FUCK::EndTabItem();
                    }
                    if (FUCK::BeginTabItem("Bindings")) {
                        drawBindings();
                        FUCK::EndTabItem();
                    } else if (_bindArmedSlot >= 0) {
                        // Tab not drawn this frame while armed: the player
                        // switched tabs mid-listen. Disarm - the hook-side
                        // expiry would catch it anyway, but not for 5s.
                        InputHook::CancelBindCapture();
                        _bindArmedSlot = -1;
                    }
                    if (FUCK::BeginTabItem("Cheats")) {
                        drawCheats();
                        FUCK::EndTabItem();
                    }
                    FUCK::EndTabBar();
                }

                if (save) { st.Save(); }
            }

        private:
            float  _missPct = 200.0f;
            float  _uiPct   = 350.0f;
            float  _songPct = 100.0f;
            int    _instSel    = 0;
            double _nextSample = 0.0;
            // Bindings tab: 0..8 = keyboard rows, 100+i = gamepad rows.
            int _bindArmedSlot  = -1;  // row listening for a press
            int _bindStolenSlot = -1;  // last steal loser, tinted
        };
        SettingsTool g_settingsTool;
    }

    void RegisterSettingsTool() { FUCK::RegisterTool(&g_settingsTool); }
}
