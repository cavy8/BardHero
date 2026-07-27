// src/game/UiSfxLogic.h
#pragma once

#include "game/EndingLogic.h"
#include "game/SongEligibility.h"

#include <iterator>
#include <optional>

// PURE decisions for the P6 UI SFX set. The shipped contract is the
// 11-file final cut of 2026-07-25
// (`BardHero Electric\docs\request-to-main-2026-07-25-ui-sfx.md`):
// relative loudness is baked into the files, everything plays at ONE
// common gain (fUiSfxVolume), and the recorded guitar material fires only
// in the electric performance context. Runtime playback lives in
// UiSfx.cpp; this header stays headless so ResultsLogicTests can lock the
// contract without the game.
namespace SH::ui_sfx {

    // Fixed slot order: Cue IS the index into the one-shot bank and the
    // enumerator's name IS the file stem it loads (banner_pop.wav, ...).
    // ONE array spells both out - the same no-drift rule as
    // CrowdReactions::Kind. kScoreTick is the single LOOPED slot; every
    // other cue is a plain one-shot.
    enum class Cue {
        kBannerPop,
        kScoreTick,
        kSpGain,
        kSpActivate,
        kSongPassElectric,
        kSongFailElectric,
    };

    inline constexpr const char* kCueNames[] = {
        "banner_pop",         "score_tick",         "sp_gain",
        "sp_activate",        "song_pass_electric", "song_fail_electric",
    };
    static_assert(std::size(kCueNames) ==
                      static_cast<std::size_t>(Cue::kSongFailElectric) + 1,
                  "kCueNames must cover every Cue, in enumerator order");

    [[nodiscard]] constexpr const char* FileName(Cue a_cue) {
        const auto i = static_cast<std::size_t>(a_cue);
        return i < std::size(kCueNames) ? kCueNames[i] : "?";
    }

    // The recorded guitar cues (miss bank, song-end stings) must fire ONLY
    // in the electric performance context. Guitar aliases lute progression
    // (songeligibility::ProgressionContext), so this gate reads the exact
    // selection context and never the aliased instrument - a lute
    // performance by a player with guitar progression must stay lute.
    [[nodiscard]] constexpr bool ElectricSoundContext(int a_context) {
        return a_context == songeligibility::kGuitar;
    }

    // Slider calibration. Every volume setting is presented as PERCENT OF
    // A REFERENCE GAIN, never as a raw 0..1 value: the one-shot banks sit
    // near 0.02 raw against the song bed, so a raw slider idles with its
    // knob at 2% of the track, which reads broken (field 2026-07-25: "a
    // default of 0.02 is very odd"). The INI keeps storing raw gain, so
    // existing values and the engine API are untouched.
    //
    // 100% is the ORIGINAL reference, deliberately NOT the shipped
    // default: the field pass recalibrated the banks upward (miss to
    // 200%, UI to 350%), and keeping the reference fixed is what makes
    // those numbers mean what they say on the slider.
    inline constexpr float kOneShotDefaultGain = 0.02f;
    inline constexpr float kSongDefaultGain    = 0.15f;
    // Clamps widened 2026-07-25 with the recalibration: the UI bank alone
    // now idles at 350%, so the old 200% ceiling sat below its own
    // default.
    inline constexpr float kOneShotSliderMaxPercent = 1000.0f;
    // 700% of 0.15 is 1.05, so the top of the song slider lands ON the
    // unity clamp below - the slider can reach full volume rather than
    // stopping short of it.
    inline constexpr float kSongSliderMaxPercent    = 700.0f;

    [[nodiscard]] constexpr float GainFromPercent(float a_percent) noexcept {
        return a_percent * (kOneShotDefaultGain / 100.0f);
    }
    [[nodiscard]] constexpr float PercentFromGain(float a_gain) noexcept {
        return a_gain * (100.0f / kOneShotDefaultGain);
    }
    // Song volume rides the same percent presentation against its own
    // reference. Raw gain is clamped to 1.0: the song bus feeds the
    // engine node directly, so anything past unity clips rather than
    // getting louder.
    [[nodiscard]] constexpr float SongGainFromPercent(
        float a_percent) noexcept {
        const float gain = a_percent * (kSongDefaultGain / 100.0f);
        return gain < 0.0f ? 0.0f : (gain > 1.0f ? 1.0f : gain);
    }
    [[nodiscard]] constexpr float SongPercentFromGain(float a_gain) noexcept {
        return a_gain * (100.0f / kSongDefaultGain);
    }

    // Per-cue trim on top of the one common bank gain. The delivered set
    // bakes relative loudness into the files, so this table stays empty
    // by design - with ONE field-ordered exception (2026-07-25): the two
    // recorded Pixabay SP impacts are magic-spell hits mastered far
    // hotter than the synthesized cues around them, and the user asked
    // for them 25% down while the bank as a whole came up. Trimming here
    // rather than in the WAV keeps the sound session's file the master.
    inline constexpr float kSpCueTrim = 0.75f;

    [[nodiscard]] constexpr float CueGainScale(Cue a_cue) noexcept {
        return (a_cue == Cue::kSpGain || a_cue == Cue::kSpActivate)
                   ? kSpCueTrim
                   : 1.0f;
    }

    // Song-end sting pick. The caller's valence block already excludes
    // aborts (a quit has no verdict), so "pass" here covers positive AND
    // neutral: BardHero has no GH fail-out, and the negative
    // awkward-silence verdict is the fail analogue - the sting agrees with
    // what the crowd plays. Outside the electric context there is no sting
    // at all (lute song end keeps current behavior per the request).
    [[nodiscard]] constexpr std::optional<Cue> StingForEnding(
        int a_instrumentContext, ending::Valence a_valence) {
        if (!ElectricSoundContext(a_instrumentContext)) {
            return std::nullopt;
        }
        return a_valence == ending::Valence::kNegative
                   ? Cue::kSongFailElectric
                   : Cue::kSongPassElectric;
    }

    // The vanilla instruments' half of the same moment (user ask
    // 2026-07-26). The electric guitar gets the synthesized sting above;
    // lute, flute and drum had nothing, so a cleared song now lands
    // Skyrim's OWN level-up sound - the sting the player already reads as
    // "you got better at something", which is exactly what a cleared song
    // means here (it feeds SGT expertise).
    //
    // Deliberately the complement of StingForEnding, never both: the
    // electric context is excluded, so no instrument ever gets two stings
    // stacked on one song end. A negative verdict is excluded for the same
    // reason the electric path splits pass from fail - the awkward-silence
    // ending is the fail analogue, and congratulating it would read as a
    // bug. Aborts never reach here (no verdict, so the caller's valence
    // block does not run).
    [[nodiscard]] constexpr bool VanillaClearSting(
        int a_instrumentContext, ending::Valence a_valence) noexcept {
        return !ElectricSoundContext(a_instrumentContext) &&
               a_valence != ending::Valence::kNegative;
    }
}
