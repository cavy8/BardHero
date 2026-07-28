#include "harness.h"
#include "game/ResultsLogic.h"
#include "render/HudLayout.h"
#include "render/PanelStyleLogic.h"
#include "render/PauseLayout.h"
#include "render/ResultsAnimation.h"
#include "render/ResultsLayout.h"
#include "game/UiSfxLogic.h"
#include "render/UiSoundLogic.h"

#include <cstring>
#include <string>

using namespace SH::results;

static bool Has(const std::string& a_hay, const char* a_needle) {
    return a_hay.find(a_needle) != std::string::npos;
}

// A whole-chart denominator - the thing the spec forbids - kept here as the
// FOIL. Several checks below assert the real function disagrees with it
// early in a run and agrees with it at the end; that pair is the property,
// and neither half alone would catch a regression back to it.
static double WholeChartAccuracy(int a_hit, int a_total) {
    return a_total > 0 ? static_cast<double>(a_hit) / a_total : 0.0;
}

static void RunTests() {
    // The field screenshot showed ImGui creating a vertical scrollbar and
    // clipping Quit. The shipping pause geometry must leave explicit slack
    // after host padding, the bounded-content inset, header and both rows at
    // every supported FLICK scale. Scale cancels in logical space, but run
    // the physical calculation too so a future mixed-unit regression fails.
    for (const float scale : { 0.75f, 1.0f, 1.15f, 1.5f }) {
        const auto fit = SH::pause_layout::Measure(scale);
        CHECK(fit.contentBottom <= fit.contentHeight);
        CHECK(fit.contentHeight - fit.contentBottom >= 8.0f * scale);
        CHECK(SH::pause_layout::SuppressScrollbar());
    }

    // The screenshot regression: READY/countdown, multiplier and STAR POWER
    // shared one vertical flow. Star Power now belongs to the independent
    // right panel, and both panels scale their physical footprint with FLICK.
    for (const float scale : { 0.75f, 1.0f, 1.15f, 1.5f }) {
        const float h = SH::hud_layout::PanelHeight(scale);
        const auto score = SH::hud_layout::MakeScore(
            h, scale, 18.0f * scale, 38.0f * scale, 42.0f * scale);
        CHECK(score.valueY >= score.titleY + 18.0f * scale);
        CHECK(score.multiplierY >= score.valueY + 38.0f * scale);
        CHECK(score.multiplierY + 42.0f * scale <= h);

        const auto glory = SH::hud_layout::MakeGlory(
            h, scale, 30.0f * scale, 18.0f * scale);
        CHECK(glory.meterY >= glory.titleY + 30.0f * scale);
        CHECK(glory.meterY + 28.0f * scale <= glory.powerLabelY);
        CHECK(glory.powerLabelY + 18.0f * scale <= glory.powerMeterY);
        CHECK(glory.powerMeterY + 11.0f * scale <= h);
        CHECK_NEAR(SH::hud_layout::PanelWidth(scale), 220.0f * scale,
                   1e-5);
    }

    // Earned stars land one at a time, rotate home, then settle exactly.
    {
        const auto dark = SH::results_anim::StarAt(0.0, 0, false);
        CHECK_NEAR(dark.scale, 1.0f, 1e-6);
        CHECK_NEAR(dark.alpha, 0.0f, 1e-6);
        const auto before = SH::results_anim::StarAt(0.17, 0, true);
        CHECK_NEAR(before.alpha, 0.0f, 1e-6);
        const auto first = SH::results_anim::StarAt(0.42, 0, true);
        const auto second = SH::results_anim::StarAt(0.42, 1, true);
        CHECK(first.alpha > second.alpha);
        CHECK(first.scale > 0.2f);
        CHECK(first.angle < 0.0f);
        const auto landed = SH::results_anim::StarAt(2.0, 4, true);
        CHECK_NEAR(landed.scale, 1.0f, 1e-6);
        CHECK_NEAR(landed.angle, 0.0f, 1e-6);
        CHECK_NEAR(landed.alpha, 1.0f, 1e-6);
        CHECK_NEAR(landed.glow, 0.0f, 1e-5);
    }

    // The eye-candy pass begins only after every possible earned star has
    // settled, loops from absolute time, and has a real dark interval.
    {
        const auto early = SH::results_anim::ShineAt(1.44, 5);
        CHECK_NEAR(early.alpha, 0.0f, 1e-6);
        const auto travel = SH::results_anim::ShineAt(1.80, 5);
        CHECK(travel.alpha > 0.70f);
        CHECK(travel.center > 0.0f && travel.center < 1.0f);
        const auto idle = SH::results_anim::ShineAt(2.30, 5);
        CHECK_NEAR(idle.alpha, 0.0f, 1e-6);
        const auto loop = SH::results_anim::ShineAt(1.80 + 1.80, 5);
        CHECK_NEAR(loop.center, travel.center, 1e-5);
        CHECK_NEAR(loop.alpha, travel.alpha, 1e-5);
        CHECK_NEAR(SH::results_anim::ShineAt(4.0, 0).alpha, 0.0f, 1e-6);

        const auto beforeGlow =
            SH::results_anim::SettledGlowAt(0.63, 0, true);
        CHECK_NEAR(beforeGlow.alpha, 0.0f, 1e-6);
        const auto settledGlow =
            SH::results_anim::SettledGlowAt(2.0, 0, true);
        CHECK(settledGlow.alpha > 0.0f);
        CHECK(settledGlow.radiusScale > 1.0f);
        // Five stars are 25 px apart at radius 9. Their halos must remain
        // disjoint even at the breathing peak.
        for (int tick = 0; tick <= 240; ++tick) {
            const auto glow = SH::results_anim::SettledGlowAt(
                tick / 20.0, 0, true);
            CHECK(glow.radiusScale * 2.0f < 25.0f / 9.0f);
        }
        const auto sameGlow =
            SH::results_anim::SettledGlowAt(2.0 + 2.4, 0, true);
        CHECK_NEAR(settledGlow.alpha, sameGlow.alpha, 1e-5);
        CHECK_NEAR(settledGlow.radiusScale, sameGlow.radiusScale, 1e-5);
        CHECK_NEAR(
            SH::results_anim::SettledGlowAt(3.0, 0, false).alpha,
            0.0f, 1e-6);
        for (int tick = 0; tick <= 100; ++tick) {
            CHECK(SH::results_anim::ImpactGlowScale(tick / 100.0f) *
                      2.0f < 25.0f / 9.0f);
        }

        // The triangle was the transparent Highway host's resize grip, not
        // the Pause panel. Pause therefore keeps the established modal
        // radius/inset/shadow instead of inventing a square visual language.
        const auto pause = SH::panel_style::PauseSurface();
        const auto modal = SH::panel_style::ModalSurface();
        CHECK_NEAR(pause.rounding, modal.rounding, 1e-6);
        CHECK_NEAR(pause.shadowOffset, modal.shadowOffset, 1e-6);
        CHECK_NEAR(pause.nearInset, modal.nearInset, 1e-6);
        CHECK_NEAR(pause.farInset, modal.farInset, 1e-6);

        using SH::ui_sound::Event;
        CHECK(std::string(SH::ui_sound::EditorId(Event::kFocus)) ==
              "UIMenuFocus");
        CHECK(std::string(SH::ui_sound::EditorId(Event::kConfirm)) ==
              "UIMenuOK");
        CHECK(std::string(SH::ui_sound::EditorId(Event::kCancel)) ==
              "UIMenuCancel");

        // P6 UI SFX bank (final 11-file cut 2026-07-25): the slot order
        // IS the bank index and the name IS the file stem - the same
        // one-array contract the crowd bank locks. A drift here plays
        // the wrong recording at the right moment, which no compile
        // error catches.
        using SH::ui_sfx::Cue;
        CHECK(std::string(SH::ui_sfx::FileName(Cue::kBannerPop)) ==
              "banner_pop");
        CHECK(std::string(SH::ui_sfx::FileName(Cue::kScoreTick)) ==
              "score_tick");
        CHECK(std::string(SH::ui_sfx::FileName(Cue::kSpGain)) ==
              "sp_gain");
        CHECK(std::string(SH::ui_sfx::FileName(Cue::kSpActivate)) ==
              "sp_activate");
        CHECK(std::string(SH::ui_sfx::FileName(Cue::kSongPassElectric)) ==
              "song_pass_electric");
        CHECK(std::string(SH::ui_sfx::FileName(Cue::kSongFailElectric)) ==
              "song_fail_electric");

        // The recorded guitar material is ELECTRIC-ONLY: the gate reads
        // the exact selection context, never the lute-aliased progression
        // (a lute perform by a guitar-ranked player must stay lute).
        CHECK(SH::ui_sfx::ElectricSoundContext(SH::songeligibility::kGuitar));
        CHECK(!SH::ui_sfx::ElectricSoundContext(SH::songeligibility::kLute));
        CHECK(!SH::ui_sfx::ElectricSoundContext(SH::songeligibility::kFlute));
        CHECK(!SH::ui_sfx::ElectricSoundContext(SH::songeligibility::kDrum));

        // Sting pick: pass covers positive AND neutral (BardHero has no
        // fail-out; the negative awkward-silence verdict is the fail
        // analogue), and a non-electric context never stings at all.
        using SH::ending::Valence;
        CHECK(SH::ui_sfx::StingForEnding(SH::songeligibility::kGuitar,
                                         Valence::kPositive) ==
              Cue::kSongPassElectric);
        CHECK(SH::ui_sfx::StingForEnding(SH::songeligibility::kGuitar,
                                         Valence::kNeutral) ==
              Cue::kSongPassElectric);
        CHECK(SH::ui_sfx::StingForEnding(SH::songeligibility::kGuitar,
                                         Valence::kNegative) ==
              Cue::kSongFailElectric);
        CHECK(!SH::ui_sfx::StingForEnding(SH::songeligibility::kLute,
                                          Valence::kNegative)
                   .has_value());
        CHECK(!SH::ui_sfx::StingForEnding(SH::songeligibility::kDrum,
                                          Valence::kPositive)
                   .has_value());

        // ...and the vanilla half of that same moment (user ask
        // 2026-07-26): lute/flute/drum clear on Skyrim's own level-up
        // sound. `UILevelUp` is the SOUN record, matching the record TYPE
        // of the entries above that are known to play - the SNDR beside it
        // (`UILevelUpSD`) would resolve to nothing.
        CHECK(std::string(SH::ui_sound::EditorId(Event::kLevelUp)) ==
              "UILevelUp");
        CHECK(SH::ui_sfx::VanillaClearSting(SH::songeligibility::kLute,
                                            Valence::kPositive));
        CHECK(SH::ui_sfx::VanillaClearSting(SH::songeligibility::kFlute,
                                            Valence::kNeutral));
        CHECK(SH::ui_sfx::VanillaClearSting(SH::songeligibility::kDrum,
                                            Valence::kPositive));
        // A poor performance is the fail analogue - congratulating it would
        // read as a bug.
        CHECK(!SH::ui_sfx::VanillaClearSting(SH::songeligibility::kLute,
                                             Valence::kNegative));
        // THE INVARIANT: never two stings on one song end. Exactly one of
        // the two paths may fire for any (context, valence) pair.
        for (const int ctx : { SH::songeligibility::kLute,
                               SH::songeligibility::kFlute,
                               SH::songeligibility::kDrum,
                               SH::songeligibility::kGuitar }) {
            for (const auto v : { Valence::kPositive, Valence::kNeutral,
                                  Valence::kNegative }) {
                CHECK(!(SH::ui_sfx::StingForEnding(ctx, v).has_value() &&
                        SH::ui_sfx::VanillaClearSting(ctx, v)));
            }
        }

        // One-shot volume slider calibration: 100% must be exactly the
        // field-tuned 0.02 gain, and the mapping must round-trip - the
        // INI stores raw gain while the tool shows percent, so a drift
        // here silently rescales every saved volume.
        CHECK_NEAR(SH::ui_sfx::GainFromPercent(100.0f), 0.02f, 1e-7);
        CHECK_NEAR(SH::ui_sfx::PercentFromGain(0.02f), 100.0f, 1e-4);
        CHECK_NEAR(SH::ui_sfx::GainFromPercent(0.0f), 0.0f, 1e-9);
        CHECK_NEAR(SH::ui_sfx::PercentFromGain(
                       SH::ui_sfx::GainFromPercent(137.0f)),
                   137.0f, 1e-3);
        // The recalibrated defaults must READ as the numbers the field
        // pass asked for: miss 200%, UI 350%. If the reference gain ever
        // moves, these are what catch it.
        CHECK_NEAR(SH::ui_sfx::PercentFromGain(0.04f), 200.0f, 1e-3);
        CHECK_NEAR(SH::ui_sfx::PercentFromGain(0.07f), 350.0f, 1e-3);
        // ...and the clamp must sit ABOVE both, or a slider would idle
        // pinned at its own ceiling.
        CHECK(SH::ui_sfx::kOneShotSliderMaxPercent > 350.0f);
        CHECK(SH::ui_sfx::kSongSliderMaxPercent > 100.0f);

        // Song volume rides the same presentation against its own
        // reference, and clamps to unity: past 1.0 the song bus clips
        // instead of getting louder.
        CHECK_NEAR(SH::ui_sfx::SongGainFromPercent(100.0f), 0.15f, 1e-7);
        CHECK_NEAR(SH::ui_sfx::SongPercentFromGain(0.15f), 100.0f, 1e-3);
        CHECK_NEAR(SH::ui_sfx::SongGainFromPercent(0.0f), 0.0f, 1e-9);
        CHECK_NEAR(SH::ui_sfx::SongGainFromPercent(
                       SH::ui_sfx::kSongSliderMaxPercent),
                   1.0f, 1e-6);

        // SP impacts carry a 25% trim under the common bank gain; every
        // other cue plays flat (relative loudness is baked into the WAVs).
        using SH::ui_sfx::Cue;
        CHECK_NEAR(SH::ui_sfx::CueGainScale(Cue::kSpGain), 0.75f, 1e-7);
        CHECK_NEAR(SH::ui_sfx::CueGainScale(Cue::kSpActivate), 0.75f, 1e-7);
        CHECK_NEAR(SH::ui_sfx::CueGainScale(Cue::kBannerPop), 1.0f, 1e-7);
        CHECK_NEAR(SH::ui_sfx::CueGainScale(Cue::kScoreTick), 1.0f, 1e-7);
        CHECK_NEAR(SH::ui_sfx::CueGainScale(Cue::kSongPassElectric), 1.0f,
                   1e-7);
    }

    // The screenshot uses tall Skyrim fonts: start-position deltas must
    // reserve the rendered line, not merely keep baselines in order.
    for (float scale : { 0.75f, 1.0f, 1.15f, 1.5f }) {
        const auto upper = SH::results_layout::MakeUpper(20.0f, scale);
        CHECK(upper.scoreValueY - upper.scoreTitleY >= 28.0f * scale);
        CHECK(upper.statsValueY - upper.statsTitleY >= 32.0f * scale);
        CHECK(upper.ratingY - upper.scoreValueY >= 50.0f * scale);
        CHECK(upper.splitY - upper.statsValueY >= 34.0f * scale);
    }

    // The lower Results section must fit at every shipped UI scale. This
    // The newest field frame reaches about 100 logical pixels at its active
    // FLICK scale. Fixed split-relative offsets then pull capped rows into
    // their siblings or put the progress bar below the footer.
    for (float scale : { 0.75f, 1.0f, 1.15f, 1.5f }) {
        for (float logicalHeight : { 100.0f, 116.0f, 130.0f, 180.0f }) {
            const float splitY = 40.0f * scale;
            const float footerY = splitY + logicalHeight * scale;
            const auto lower = SH::results_layout::MakeLower(
                splitY, footerY, scale);
            CHECK(SH::results_layout::ContentBottom(lower, scale) <=
                  footerY - 12.0f * scale);
            CHECK(lower.rewardDetailY - lower.rewardSummaryY >=
                  32.0f * scale);
            CHECK(lower.progressSummaryY - lower.progressTitleY >=
                  28.0f * scale);
            CHECK(lower.progressBarY -
                      (lower.progressSummaryY + 20.0f * scale) >=
                  8.0f * scale);

            // ⚠ The rows must stay a BLOCK, not spread to fill the frame.
            // Every check above still passes with the meter pinned to the
            // footer, because they only ever asserted that nothing OVERRUNS.
            // So the suite stayed green while a tall frame opened a hole down
            // the middle of the panel, between the reward text and the meter
            // (field 2026-07-27). Surplus height belongs BELOW the block.
            CHECK(lower.progressBarY - lower.progressSummaryY <=
                  40.0f * scale);
            CHECK(lower.progressBarY - lower.progressTitleY <=
                  80.0f * scale);
        }
    }

    // XP count-up and meter fill are deterministic functions of open time.
    {
        const auto start = SH::results_anim::At(0.0, 19, 25, 6, false);
        CHECK(start.shownXp == 0);
        CHECK(start.displayedExpertise == 19);
        CHECK_NEAR(start.meterFrac,
                   SH::results::ProgressFor(19).frac, 1e-6);

        const auto middle = SH::results_anim::At(0.9, 19, 25, 6, false);
        CHECK(middle.shownXp > 0 && middle.shownXp < 6);
        CHECK(middle.displayedExpertise > 19);
        CHECK(middle.meterFrac > start.meterFrac);

        const auto done = SH::results_anim::At(3.0, 19, 25, 6, false);
        CHECK(done.shownXp == 6);
        CHECK(done.displayedExpertise == 25);
        CHECK_NEAR(done.meterFrac,
                   SH::results::ProgressFor(25).frac, 1e-6);
        CHECK_NEAR(done.baseFrac,
                   SH::results::ProgressFor(19).frac, 1e-6);
    }

    // Rank-up first fills the old band, pulses the new standing, then fills
    // the newly entered band. The final state is exact, not animation drift.
    {
        const auto start = SH::results_anim::At(0.0, 25, 31, 6, true);
        CHECK(!start.rankUpActive);
        CHECK(!start.showingNewRank);
        CHECK(start.displayedExpertise == 25);

        const auto crossing = SH::results_anim::At(1.0, 25, 31, 6, true);
        CHECK(crossing.rankUpActive);
        CHECK(crossing.rankUpPulse > 0.0f);
        CHECK(crossing.meterFrac >= 0.99f);

        const auto newBand = SH::results_anim::At(1.7, 25, 31, 6, true);
        CHECK(newBand.showingNewRank);
        CHECK(newBand.displayedExpertise >= 26);
        CHECK(newBand.meterFrac > 0.0f);
        CHECK(newBand.meterFrac <
              SH::results::ProgressFor(31).frac);

        const auto done = SH::results_anim::At(3.0, 25, 31, 6, true);
        CHECK(!done.rankUpActive);
        CHECK(done.showingNewRank);
        CHECK(done.shownXp == 6);
        CHECK(done.displayedExpertise == 31);
        CHECK_NEAR(done.meterFrac,
                   SH::results::ProgressFor(31).frac, 1e-6);
        CHECK_NEAR(done.baseFrac, 0.0, 1e-6);
    }

    // ---- acceptance criterion 2 ---------------------------------------
    // A perfect run reads 100% at the first note, and keeps reading 100%.
    // The foil reads ~0.3% on a 300-note chart at that same moment.
    CHECK_NEAR(LiveAccuracy(1, 0), 1.0, 1e-12);
    CHECK(LiveAccuracy(1, 0) > WholeChartAccuracy(1, 300) + 0.9);
    for (int n = 1; n <= 300; ++n) { CHECK_NEAR(LiveAccuracy(n, 0), 1.0, 1e-12); }

    // Nothing resolved yet (lead-in / countdown): not zero.
    CHECK_NEAR(LiveAccuracy(0, 0), 1.0, 1e-12);
    // A run that has only missed reads zero, not one.
    CHECK_NEAR(LiveAccuracy(0, 1), 0.0, 1e-12);

    // ---- acceptance criterion 1 ----------------------------------------
    // Once every window has closed (hit + missed == total, which is where
    // the session always ends) the live figure IS the results figure.
    for (int total = 1; total <= 200; total += 7) {
        for (int hit = 0; hit <= total; hit += 3) {
            CHECK_NEAR(LiveAccuracy(hit, total - hit),
                       WholeChartAccuracy(hit, total), 1e-12);
        }
    }

    // Mid-run the two genuinely differ, and the live one is the higher of
    // the pair for any run that is not yet finished and not all misses.
    CHECK(LiveAccuracy(50, 10) > WholeChartAccuracy(50, 286));

    // Negative counters (never expected, but Draw() must not divide by a
    // negative) behave as zero.
    CHECK_NEAR(LiveAccuracy(-5, -5), 1.0, 1e-12);
    CHECK_NEAR(LiveAccuracy(4, -1), 1.0, 1e-12);

    // ---- acceptance criterion 3 ----------------------------------------
    // The HUD's colour band and the final star count must be the SAME
    // function of the SAME thresholds. Walk custom thresholds so a hidden
    // second set of defaults inside BandFor would show up.
    {
        SH::stars::StarParams p;
        p.t[0] = 0.10; p.t[1] = 0.20; p.t[2] = 0.30;
        p.t[3] = 0.40; p.t[4] = 0.50;
        for (int i = 0; i <= 1000; ++i) {
            const double a = i / 1000.0;
            CHECK(BandFor(a, p) == SH::stars::StarsFromAccuracy(a, p));
        }
        // ...and it really tracks THOSE thresholds, not the defaults.
        CHECK(BandFor(0.55, p) == 5);
        CHECK(BandFor(0.09, p) == 0);
        // exactly-on a threshold counts (thresholds are inclusive)
        CHECK(BandFor(0.30, p) == 3);
    }
    {
        SH::stars::StarParams d;  // shipped defaults
        CHECK(BandFor(0.0, d) == 0);
        CHECK(BandFor(0.50, d) == 1);
        CHECK(BandFor(1.0, d) == 5);
        // monotonic: more accuracy never means fewer stars
        int prev = 0;
        for (int i = 0; i <= 1000; ++i) {
            const int b = BandFor(i / 1000.0, d);
            CHECK(b >= prev);
            prev = b;
        }
    }

    // ---- pips ----------------------------------------------------------
    for (int s = 0; s <= 5; ++s) {
        const char* p = PipString(s);
        CHECK(std::strlen(p) == 5);
        int filled = 0;
        for (int i = 0; i < 5; ++i) {
            if (p[i] == '*') { ++filled; }
        }
        CHECK(filled == s);
    }
    // clamped, not indexed off the end
    CHECK(std::strcmp(PipString(-3), PipString(0)) == 0);
    CHECK(std::strcmp(PipString(99), PipString(5)) == 0);

    // ---- standings -----------------------------------------------------
    // SGT's own five, in SGT's own words, matching SettingsTool::TierOf.
    CHECK(std::strcmp(StandingName(1), "Clueless") == 0);
    CHECK(std::strcmp(StandingName(2), "Beginner") == 0);
    CHECK(std::strcmp(StandingName(3), "Decent") == 0);
    CHECK(std::strcmp(StandingName(4), "Advanced") == 0);
    CHECK(std::strcmp(StandingName(5), "Pro") == 0);
    CHECK(std::strcmp(StandingName(0), "Clueless") == 0);
    CHECK(std::strcmp(StandingName(9), "Pro") == 0);

    // Results progression names the instrument whose frozen session
    // snapshot earned the XP. At max rank the payoff copy becomes a single
    // gold MAX: no redundant "Pro" prefix and no meaningless +XP counter.
    CHECK(std::strcmp(ProficiencyHeading(0), "LUTE PROFICIENCY") == 0);
    CHECK(std::strcmp(ProficiencyHeading(1), "FLUTE PROFICIENCY") == 0);
    CHECK(std::strcmp(ProficiencyHeading(2), "DRUM PROFICIENCY") == 0);
    CHECK(std::strcmp(ProficiencyHeading(-1), "LUTE PROFICIENCY") == 0);
    CHECK(std::strcmp(ProficiencyHeading(99), "LUTE PROFICIENCY") == 0);
    CHECK(std::strcmp(MaxStandingLabel(), "MAX") == 0);
    CHECK(ShowXpGain(85));
    CHECK(!ShowXpGain(86));
    CHECK(!ShowXpGain(200));

    // ---- rank progress -------------------------------------------------
    // The rank this reports must agree with the ladder the rest of the mod
    // uses (SgtProgression::RankFromExpertise, boundaries 26/46/66/86) at
    // EVERY value, not just at the edges - a rank-up line that disagrees
    // with the browser's lock display is the bug this guards.
    for (int e = -10; e <= 140; ++e) {
        const int expect = e < 26 ? 1 : e < 46 ? 2 : e < 66 ? 3 : e < 86 ? 4 : 5;
        CHECK(ProgressFor(e).rank == expect);
    }
    // The bar fills as expertise climbs and RESETS at each edge - it must
    // never read nearly-full immediately after a rank-up.
    CHECK_NEAR(ProgressFor(0).frac, 0.0, 1e-12);
    CHECK_NEAR(ProgressFor(25).frac, 25.0 / 26.0, 1e-12);
    CHECK_NEAR(ProgressFor(26).frac, 0.0, 1e-12);
    CHECK(ProgressFor(45).frac > 0.9);
    CHECK_NEAR(ProgressFor(46).frac, 0.0, 1e-12);
    CHECK(ProgressFor(65).frac > 0.9);
    CHECK_NEAR(ProgressFor(66).frac, 0.0, 1e-12);
    // ...and within a band it is monotonic
    for (int e = 66; e < 85; ++e) {
        CHECK(ProgressFor(e + 1).frac > ProgressFor(e).frac);
    }
    // the top band is full, not empty, and names itself as such
    CHECK(ProgressFor(86).maxed);
    CHECK_NEAR(ProgressFor(86).frac, 1.0, 1e-12);
    CHECK(ProgressFor(200).maxed);
    CHECK(!ProgressFor(85).maxed);
    // the target is always the next edge, never the current one
    CHECK(ProgressFor(0).next == 26);
    CHECK(ProgressFor(30).next == 46);
    CHECK(ProgressFor(70).next == 86);
    // a negative sample (SGT absent) falls to rank 1, the same way
    // RankFromExpertise does
    CHECK(ProgressFor(-1).rank == 1);

    // ---- gold reasons ---------------------------------------------------
    {
        GoldFacts f;  // 38 gold, 4 stars, long set, warm inn
        f.gold = 38; f.stars = 4; f.lengthMult = 2.0; f.moodLevel = 2;
        const std::string r = GoldReason(f);
        CHECK(Has(r, "38 gold"));
        CHECK(Has(r, "long set"));
        CHECK(Has(r, "well played"));
        CHECK(Has(r, "warm room"));
        CHECK(r.back() == '.');
    }
    {
        // Acceptance criterion 6 is "names the gold AND at least one
        // reason", so every non-zero purse must carry a clause. Sweep the
        // whole input space rather than spot-checking.
        for (int stars = 0; stars <= 5; ++stars) {
            for (int mood = 0; mood <= 2; ++mood) {
                for (int inn = 0; inn <= 1; ++inn) {
                    for (double len : { 0.5, 1.0, 2.0 }) {
                        GoldFacts f;
                        f.gold = 12; f.stars = stars; f.moodLevel = mood;
                        f.atInn = inn != 0; f.lengthMult = len;
                        const std::string r = GoldReason(f);
                        CHECK(Has(r, "12 gold"));
                        // " - " plus at least one clause plus a full stop
                        CHECK(r.size() > std::strlen("12 gold - ") + 4);
                        CHECK(r.back() == '.');
                        // never a dangling or doubled separator
                        CHECK(!Has(r, ", ."));
                        CHECK(!Has(r, ",,"));
                        CHECK(!Has(r, "- ,"));
                    }
                }
            }
        }
    }
    {
        // A zero purse must say WHICH of the two causes it was.
        GoldFacts cold;
        cold.gold = 0; cold.stars = 5; cold.moodLevel = 0;
        CHECK(Has(GoldReason(cold), "lost the room"));

        GoldFacts rough;
        rough.gold = 0; rough.stars = 1; rough.minStars = 2; rough.moodLevel = 2;
        CHECK(Has(GoldReason(rough), "will not pay"));
        CHECK(!Has(GoldReason(rough), "lost the room"));

        // minStars is honoured, not hard-coded at 2: a 3-star run under a
        // 4-star bar is a rough night too.
        GoldFacts strict;
        strict.gold = 0; strict.stars = 3; strict.minStars = 4;
        strict.moodLevel = 2;
        CHECK(Has(GoldReason(strict), "will not pay"));

        // ...and a run that cleared the bar but rounded to nothing gets
        // neither of the two wrong explanations.
        GoldFacts thin;
        thin.gold = 0; thin.stars = 4; thin.minStars = 2; thin.moodLevel = 2;
        const std::string r = GoldReason(thin);
        CHECK(!Has(r, "lost the room"));
        CHECK(!Has(r, "will not pay"));
    }
    {
        // The room clause must not claim an inn the player was not in.
        GoldFacts street;
        street.gold = 5; street.stars = 4; street.atInn = false;
        street.moodLevel = 2;
        const std::string r = GoldReason(street);
        CHECK(Has(r, "street"));
        CHECK(!Has(r, "room"));
    }
    {
        // Nobody around outranks every other zero-purse cause: a mood level
        // computed with no listeners is the model talking to itself, and
        // "lost the room" must never blame a room that never existed
        // (field 2026-07-28).
        GoldFacts nobody;
        nobody.gold = 0; nobody.stars = 5; nobody.moodLevel = 0;
        nobody.audienceMult = 0.0;
        const std::string r = GoldReason(nobody);
        CHECK(Has(r, "nobody was around"));
        CHECK(!Has(r, "lost the room"));
        CHECK(Has(CompactGoldReason(r), "Nobody"));

        // a thin crowd is named on a PAID purse...
        GoldFacts sparse;
        sparse.gold = 5; sparse.stars = 4; sparse.moodLevel = 2;
        sparse.audienceMult = 0.4;
        CHECK(Has(GoldReason(sparse), "handful of listeners"));
        // ...and a full room stays silent about itself
        GoldFacts full;
        full.gold = 30; full.stars = 5; full.moodLevel = 2;
        CHECK(!Has(GoldReason(full), "handful"));
    }
    {
        // A middling length says nothing about length at all - the clause
        // exists to explain a swing, and 1.0x is not one.
        GoldFacts mid;
        mid.gold = 20; mid.stars = 3; mid.lengthMult = 1.0; mid.moodLevel = 2;
        const std::string r = GoldReason(mid);
        CHECK(!Has(r, "long set"));
        CHECK(!Has(r, "short set"));
        CHECK(Has(r, "solidly played"));
    }

    // Compact payout copy preserves the reason without truncation marks.
    for (int gold : { 0, 12 }) {
        for (int stars = 0; stars <= 5; ++stars) {
            for (int mood = 0; mood <= 2; ++mood) {
                GoldFacts f;
                f.gold = gold; f.stars = stars; f.moodLevel = mood;
                f.lengthMult = stars % 2 ? 0.5 : 2.0;
                const std::string compact = CompactGoldReason(GoldReason(f));
                CHECK(!compact.empty());
                CHECK(!Has(compact, "..."));
                CHECK(compact.size() <= 40);
                CHECK(compact.back() == '.');
            }
        }
    }
}

TEST_MAIN("ResultsLogic")
