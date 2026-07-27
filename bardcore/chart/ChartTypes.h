#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "chart/TempoMap.h"

namespace bard {

    // Lane indices 0..4 = GRYBO, 5 = open. Bitmasks use bit 6 for open to
    // match the engine's held-fret convention (spec 5.2: open = bit 6, set
    // only when nothing is held).
    inline constexpr int          kLaneCount = 6;
    inline constexpr int          kOpenLane  = 5;
    inline constexpr std::uint8_t kOpenBit   = 1 << 6;

    inline constexpr std::uint8_t LaneBit(int lane) {
        return lane == kOpenLane ? kOpenBit
                                 : static_cast<std::uint8_t>(1u << lane);
    }
    inline constexpr int LaneIndex(std::uint8_t bit) {
        if (bit == kOpenBit) return kOpenLane;
        for (int i = 0; i < 5; ++i)
            if (bit == (1u << i)) return i;
        return -1;
    }

    // ---- normalized IR ----------------------------------------------------

    // One chord event; single notes are 1-lane chords. Judged as ONE unit,
    // scored per lane (spec 5.3).
    struct Note {
        std::uint32_t tick = 0;
        double        time = 0.0;      // seconds, ALL offsets applied
        std::uint8_t  mask = 0;        // union of LaneBit()
        bool          isHopo = false;  // resolved: natural x forcing
        bool          isTap  = false;  // tap wins over hopo (spec 4.3)
        std::int32_t  spPhrase = -1;   // index into ParsedChart::spPhrases
        // Per-LANE sustain (chords may be disjoint, spec 4.3). 0 = none.
        std::array<std::uint32_t, kLaneCount> sustainTicks{};
        std::array<double, kLaneCount>        sustainEnd{};  // seconds
        // Lanes whose sustain overlaps a LATER note's tick (extended
        // sustains, spec 5.4) - engine subtracts these from fret tests.
        std::uint8_t extendedMask = 0;
    };

    struct SpPhrase {
        std::uint32_t startTick = 0, endTick = 0;  // end EXCLUSIVE...
        bool          zeroLen = false;             // ...except zero-length
        int           noteCount = 0;
        std::int32_t  lastNoteIndex = -1;          // award happens here
    };
    struct SoloPhrase {
        std::uint32_t startTick = 0, endTick = 0;  // end INCLUSIVE (spec 4.3)
        int           noteCount = 0;
    };
    // A practice-mode section marker (spec 2026-07-26-practice-mode 5.1).
    // Presentation and range selection only: nothing in judgment, scoring
    // or timing may read these.
    struct ChartSection {
        std::uint32_t tick = 0;    // section start, chart ticks
        double        time = 0.0;  // seconds, chart offset already applied
        std::string   name;        // "Intro", "Verse 1"; never empty
    };
    struct TimeSig {
        std::uint32_t tick = 0;
        std::uint32_t num = 4, denom = 4;  // display + SP only, never timing
    };

    struct SongMeta {
        std::string name, artist, album, charter, loadingPhrase;
        double      previewStartMs = -1.0;
        int         diffGuitar = -1;
        double      songLengthMs = -1.0;
    };

    struct ParsedChart {
        std::uint32_t           resolution = 192;
        TempoMap                tempo;
        std::vector<TimeSig>    timeSigs;
        std::vector<Note>       notes;  // sorted by tick, unique ticks
        std::vector<SpPhrase>   spPhrases;
        std::vector<SoloPhrase> solos;
        // Sorted by tick, unique ticks. EMPTY is normal - most charts
        // carry no markers, and practice covers the whole song then.
        std::vector<ChartSection> sections;
        double                  offsetSeconds = 0.0;  // already inside Note::time
        SongMeta                meta;
    };

    // ---- raw (pre-normalization) representation ---------------------------

    // .chart N5 is a strum/HOPO FLIP; .mid base+5/base+6 are true overrides
    // (spec 4.3/4.4). Front-ends map to this enum; Normalize applies it.
    enum class Forcing : std::uint8_t { kNone, kFlip, kForceHopo, kForceStrum };

    struct RawChord {
        std::uint32_t tick = 0;
        std::uint8_t  mask = 0;
        Forcing       forcing = Forcing::kNone;
        bool          tap = false;
        std::array<std::uint32_t, kLaneCount> sustainTicks{};
    };

    // One difficulty of one instrument, format-agnostic.
    struct RawTrack {
        std::vector<RawChord>   chords;     // sorted, one per tick
        std::vector<SpPhrase>   spPhrases;  // tick fields only
        std::vector<SoloPhrase> solos;      // tick fields only
        std::uint32_t hopoThresholdTicks = 0;  // format default; ini overrides
        bool          hasEnd = false;          // E end / [end] honored
        std::uint32_t endTick = 0;
    };
}
