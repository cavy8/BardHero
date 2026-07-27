#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace bard {

    // song.ini values honored in v1 (spec 4.2). Sentinels mark absence.
    // delayMs already has the seconds heuristic applied.
    struct SongIniValues {
        std::string name, artist, album, charter, loadingPhrase;
        double      delayMs  = 0.0;
        bool        hasDelay = false;
        std::int64_t hopoFrequency  = -1;  // ticks
        bool         eighthNoteHopo = false;
        std::int64_t sustainCutoff  = -1;  // ticks (CH's typo key accepted)
        int          multiplierNote = -1;  // 103/116 only
        bool         endEvents      = true;
        int          diffGuitar     = -1;
        double       previewStartMs = -1.0;
        double       songLengthMs   = -1.0;
        // Solo-instrument song (bard imports): the sole stem IS the
        // instrument, so it is a valid miss-mute target.
        bool singleInstrument = false;
        // Optional instrument tag for instrument-bound Songbook eligibility.
        // Normalized lowercase "lute"|"flute"|"drum"|"guitar"; empty means
        // legacy/untagged and is visible only in the context-free Songbook.
        std::string instrument;
        // Optional unlock gate override (spec 2026-07-21 section 6.1).
        // -1 = absent, derive from diffGuitar instead.
        int unlockRank = -1;
    };

    bool ParseSongIniText(std::string_view text, SongIniValues& out);

    // Resolved parse-affecting settings for the front-ends:
    //   hopo_frequency (ticks) wins over eighthnote_hopo over the format
    //   default (.chart floor(65/192*res); .mid floor(res/3)+1).
    //   PINNED: eighthnote_hopo maps to floor(res/2) (YARG behavior); the
    //   M5 corpus pass validates.
    std::int64_t ResolveHopoThreshold(const SongIniValues& ini,
                                      std::uint32_t resolution, bool isMid);
    // sustain cutoff: ini wins (both formats); else .mid res/3, .chart none.
    std::int64_t ResolveSustainCutoff(const SongIniValues& ini,
                                      std::uint32_t resolution, bool isMid);
}
