#pragma once

// PURE Songbook column sorting (no RE/OS/ImGui).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>
#include <vector>

namespace SH::song_sort {
    enum class Column : int {
        kSong   = 0,
        kArtist = 1,
        kLength = 2,
        kRating = 3
    };
    inline constexpr Column kDefaultColumn = Column::kRating;
    inline constexpr bool   kDefaultAscending = false;

    // Rating is a mixed display column. Ascending reads as the natural
    // progression path (lower-rank locks, unplayed songs, earned stars);
    // descending puts the player's best performances first.
    enum class RatingState : int {
        kLocked   = 0,
        kUnplayed = 1,
        kRated    = 2
    };

    struct Key {
        std::string_view song;
        std::string_view artist;
        int              stableIndex = 0;
        double           lengthMs   = 0.0;
        RatingState      ratingState = RatingState::kUnplayed;
        int              ratingValue = 0;  // required rank or earned stars
    };

    [[nodiscard]] inline int CompareFolded(std::string_view a_left,
                                           std::string_view a_right) {
        const auto count = std::min(a_left.size(), a_right.size());
        for (std::size_t i = 0; i < count; ++i) {
            const auto left = static_cast<unsigned char>(a_left[i]);
            const auto right = static_cast<unsigned char>(a_right[i]);
            const int l = std::tolower(left);
            const int r = std::tolower(right);
            if (l != r) { return l < r ? -1 : 1; }
        }
        if (a_left.size() == a_right.size()) { return 0; }
        return a_left.size() < a_right.size() ? -1 : 1;
    }

    template <class T>
    [[nodiscard]] inline int CompareValue(const T& a_left,
                                          const T& a_right) {
        if (a_left == a_right) { return 0; }
        return a_left < a_right ? -1 : 1;
    }

    [[nodiscard]] inline int Compare(const Key& a_left, const Key& a_right,
                                     Column a_column) {
        int result = 0;
        switch (a_column) {
            case Column::kArtist:
                result = CompareFolded(a_left.artist, a_right.artist);
                break;
            case Column::kLength: {
                const double left =
                    std::isfinite(a_left.lengthMs) ? a_left.lengthMs : 0.0;
                const double right =
                    std::isfinite(a_right.lengthMs) ? a_right.lengthMs : 0.0;
                result = CompareValue(left, right);
                break;
            }
            case Column::kRating:
                result = CompareValue(
                    static_cast<int>(a_left.ratingState),
                    static_cast<int>(a_right.ratingState));
                if (result == 0) {
                    result = CompareValue(a_left.ratingValue,
                                          a_right.ratingValue);
                }
                break;
            case Column::kSong:
            default:
                result = CompareFolded(a_left.song, a_right.song);
                break;
        }
        // Song title is the visible tiebreak for every other column; the
        // stable folder key resolves duplicate titles deterministically.
        if (result == 0 && a_column != Column::kSong) {
            result = CompareFolded(a_left.song, a_right.song);
        }
        if (result == 0) {
            result = CompareValue(a_left.stableIndex, a_right.stableIndex);
        }
        return result;
    }

    inline void Sort(std::vector<int>& a_order,
                     const std::vector<Key>& a_keys, Column a_column,
                     bool a_ascending) {
        std::stable_sort(
            a_order.begin(), a_order.end(), [&](int a_left, int a_right) {
                const bool leftValid =
                    a_left >= 0 &&
                    static_cast<std::size_t>(a_left) < a_keys.size();
                const bool rightValid =
                    a_right >= 0 &&
                    static_cast<std::size_t>(a_right) < a_keys.size();
                if (leftValid != rightValid) { return leftValid; }
                if (!leftValid) { return a_left < a_right; }
                const int cmp = Compare(a_keys[a_left], a_keys[a_right],
                                        a_column);
                // Rating-descending is deliberately mixed: earned stars are
                // high-to-low, but once the player reaches locked material
                // its learning path stays low-rank-to-high-rank. Applying
                // one global direction to both was the Rank 5, Rank 5,
                // Rank 4 ordering seen in the field.
                if (a_column == Column::kRating &&
                    a_keys[a_left].ratingState == RatingState::kLocked &&
                    a_keys[a_right].ratingState == RatingState::kLocked) {
                    return cmp < 0;
                }
                return a_ascending ? cmp < 0 : cmp > 0;
            });
    }
}
