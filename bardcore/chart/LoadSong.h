#pragma once
#include <cstdint>
#include <string>

#include "chart/ChartTypes.h"
#include "chart/Scan.h"
#include "chart/SongIni.h"

namespace bard {

    struct LoadedSong {
        ParsedChart   chart;
        SongIniValues ini;  // raw values; host uses name/length for display+end
        // Which difficulties this chart actually carries (bit d set =
        // Easy..Expert d has a non-empty track) and which one the fallback
        // below actually built. Guitar-Hero-style per-difficulty records
        // key on resolvedDifficulty, never on the requested one - playing
        // "Expert" on a Hard-only chart is a Hard record.
        std::uint8_t difficultyMask     = 0;
        int          resolvedDifficulty = -1;
    };

    // The difficulty the fallback picks from an availability mask: the
    // requested one, then downward, then upward (spec 4.1: play what the
    // chart has). -1 when the mask is empty. Shared by LoadSong, the
    // Songbook (per-row stars + Diff display) and the session record, so
    // all three always agree on what a pick would play.
    [[nodiscard]] constexpr int ResolveDifficulty(std::uint8_t mask,
                                                  int want) noexcept {
        if (want < 0 || want > 3) want = 3;
        for (int d = want; d >= 0; --d) {
            if (mask & (1u << d)) return d;
        }
        for (int d = want + 1; d < 4; ++d) {
            if (mask & (1u << d)) return d;
        }
        return -1;
    }

    // SongEntry -> ParsedChart: read chart + song.ini, resolve the hopo /
    // sustain-cutoff / end-events overlays (spec 4.2), front-end to RawTrack,
    // Normalize. difficulty 0..3 (Easy..Expert); a missing difficulty falls
    // back per ResolveDifficulty above, and out.resolvedDifficulty /
    // out.difficultyMask report what happened. Returns false with a
    // one-line reason in *error.
    bool LoadSong(const SongEntry& entry, int difficulty, LoadedSong& out,
                  std::string* error = nullptr);
}
