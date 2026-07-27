#pragma once

// PURE chart-challenge and natural unlock-rank policy.
//
// Measured charts are ranked from what the player must perform, not from the
// blanket diff_guitar/unlock_rank metadata found in generated libraries.
// Every instrument keeps its own two easiest measured songs at rank 1. A
// chart that cannot be measured retains its legacy metadata-derived fallback.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace SH::song_challenge {
    struct Metrics {
        double bpm         = 0.0;
        double averageNps  = 0.0;
        double peakNps     = 0.0;
        double durationSec = 0.0;
    };

    [[nodiscard]] inline double Unit(double a_value, double a_low,
                                     double a_high) {
        if (!std::isfinite(a_value) || a_high <= a_low) { return 0.0; }
        return std::clamp((a_value - a_low) / (a_high - a_low), 0.0, 1.0);
    }

    // 0..100. Density owns most of the score because it is the direct input
    // burden. Tempo describes how tightly those notes are phrased, peak NPS
    // catches short bursts hidden by an easy average, and length measures
    // how long the player must sustain concentration.
    [[nodiscard]] inline double Score(const Metrics& a_m) {
        const double tempo   = Unit(a_m.bpm, 60.0, 180.0);
        const double density = Unit(a_m.averageNps, 0.5, 4.0);
        const double peak    = Unit(a_m.peakNps, 1.0, 6.0);
        const double length  = Unit(a_m.durationSec, 30.0, 240.0);
        return 100.0 * (0.25 * tempo + 0.40 * density +
                        0.20 * peak + 0.15 * length);
    }

    // Rank 1 is reserved for the guaranteed starters selected below.
    [[nodiscard]] inline int NaturalRequiredRank(double a_score) {
        if (!std::isfinite(a_score)) { return 1; }
        if (a_score < 40.0) { return 2; }
        if (a_score < 55.0) { return 3; }
        if (a_score < 70.0) { return 4; }
        return 5;
    }

    struct RankInput {
        int         instrument  = -1;  // lute/flute/drum/guitar = 0/1/2/3
        double      score       = 0.0;
        int         fallbackRank = 1;
        std::string key;
        bool        measured    = true;
    };

    [[nodiscard]] inline std::vector<int> RankLibrary(
        const std::vector<RankInput>& a_songs, int a_startersPerInstrument) {
        std::vector<int> out;
        out.reserve(a_songs.size());
        for (const auto& song : a_songs) {
            out.push_back(std::clamp(song.fallbackRank, 1, 5));
        }

        const int starterCount = std::max(a_startersPerInstrument, 0);
        for (int instrument = 0; instrument < 4; ++instrument) {
            std::vector<std::size_t> measured;
            for (std::size_t i = 0; i < a_songs.size(); ++i) {
                const auto& song = a_songs[i];
                if (song.instrument == instrument && song.measured &&
                    std::isfinite(song.score)) {
                    measured.push_back(i);
                }
            }
            std::sort(measured.begin(), measured.end(),
                      [&](std::size_t a_left, std::size_t a_right) {
                          const auto& left  = a_songs[a_left];
                          const auto& right = a_songs[a_right];
                          if (left.score != right.score) {
                              return left.score < right.score;
                          }
                          return left.key < right.key;
                      });
            for (std::size_t order = 0; order < measured.size(); ++order) {
                const auto index = measured[order];
                if (order < static_cast<std::size_t>(starterCount)) {
                    out[index] = 1;
                    continue;
                }
                // Repertoire-relative bands keep every sufficiently large
                // instrument library playable as a progression ladder. The
                // absolute score is a cap: a uniformly easy library is never
                // forced into rank 5 merely because one song is its hardest.
                const double position =
                    measured.size() <= 1
                        ? 1.0
                        : static_cast<double>(order) /
                              static_cast<double>(measured.size() - 1);
                const int relativeRank =
                    position < 0.35 ? 2
                    : position < 0.60 ? 3
                    : position < 0.82 ? 4
                                      : 5;
                out[index] = std::min(
                    NaturalRequiredRank(a_songs[index].score),
                    relativeRank);
            }
        }
        return out;
    }
}
