// src/game/UnlockLogic.h
#pragma once

// PURE song-unlock gate (no RE/OS includes - headless-tested by
// UnlockTests). Mirrors SGT's own "each rank unlocks 3-8 songs" model over
// BardHero's chart library, which is currently gated by nothing.
//
// Fail-open by design: an unknown or malformed difficulty resolves to rank
// 1, so a chart can never become unreachable through bad metadata.
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "game/SongEligibility.h"

namespace SH::unlock {

    // SGT's paid-bard dialogue fragments emit this literal after taking the
    // lesson fee. BardHero turns that transaction into a song lesson, not a
    // generic skill-up, so the legacy sentence is hidden at the HUD boundary.
    // Exact matching is deliberate: no SGT assets are patched and no other
    // notification (including BardHero's learned-song payoff) is affected.
    [[nodiscard]] inline bool ShouldSuppressLessonNotification(
        std::string_view a_text) {
        return a_text == "Your musical talent increases";
    }

    [[nodiscard]] inline std::string LearnedSongNotification(
        std::string_view a_title, std::string_view a_instrument = {}) {
        if (const auto tagged =
                songeligibility::TaggedInstrument(a_instrument)) {
            const auto instrument =
                songeligibility::InstrumentName(*tagged);
            return "New " + std::string(instrument) +
                   " song learned: " + std::string(a_title);
        }
        return "New song learned: " + std::string(a_title);
    }

    // diffGuitar is song.ini's `diff_guitar` (-1 = absent).
    // overrideRank is `unlock_rank` (-1 = absent); 1..5 wins when valid.
    inline int RequiredRank(int diffGuitar, int overrideRank) {
        if (overrideRank >= 1 && overrideRank <= 5) { return overrideRank; }
        if (diffGuitar < 0) { return 1; }
        if (diffGuitar <= 1) { return 1; }
        return std::clamp(diffGuitar, 1, 5);
    }

    // playerRank floors at 1: SgtProgression::UiSampled() returns -1 when
    // nothing has been sampled yet, and SGT is a soft dependency, so a
    // player without it installed reads -1 for every instrument,
    // permanently. Without this floor, -1 >= requiredRank is false for
    // every chart in 1..5, so the whole library - including charts with no
    // diff_guitar tag at all, meant to always be available - would lock,
    // with the browser cheerfully printing "Rank 1 required" on a chart
    // that needs no rank. Fail-open in the same direction the header above
    // already promises for an absent diff_guitar.
    inline bool IsUnlocked(int requiredRank, int playerRank,
                           bool taughtByBard) {
        return taughtByBard || std::max(playerRank, 1) >= requiredRank;
    }

    // ---- bard-lesson edge (spec 6.3) --------------------------------------
    // The discriminator that separates a paid lesson from every other write
    // to SGT's expertise globals. A lesson (Bard_Fragment*.psc and its 12
    // siblings, 100 gold) raises ALL THREE in one dialogue fragment. Nothing
    // else can:
    //   - SGT's own per-song payout writes ONE global, and reaches +10 with
    //     speechcraft and rested, so no MAGNITUDE test can separate it.
    //   - Our promotion bracket and XP feed each write ONE global, by up to
    //     +66, and they land AFTER the session has gone idle.
    //   - Our clamp sweeps all three, but is strictly monotone DOWN, so it
    //     can never contribute a rise. That property is load-bearing here:
    //     a clamp that could ever RAISE a global would break this rule.
    // `step` is a noise floor only, not the discriminator - the Orc bard
    // (Bard_FragmentOrc.psc) charges the same 100 gold for +1 to all three,
    // so it wants to be 1. A negative sample means "never sampled" and can
    // never contribute a rise.
    inline bool IsLessonEdge(const int* prev, const int* now, int count,
                             int step) {
        if (!prev || !now || count <= 0 || step < 1) { return false; }
        for (int i = 0; i < count; ++i) {
            if (prev[i] < 0 || now[i] < 0) { return false; }
            if (now[i] - prev[i] < step) { return false; }
        }
        return true;
    }

    // Index of the lowest-tier chart that is still locked, or -1. Charts
    // already open by rank are never chosen - a lesson must buy something.
    // requiredRanks entries are expected to come from RequiredRank(), whose
    // own range is always 1..5; this function does not itself validate that.
    inline int PickTeachingTarget(const std::vector<int>&  requiredRanks,
                                  const std::vector<bool>& taught,
                                  int playerRank) {
        int best = -1, bestRank = 99;
        for (std::size_t i = 0; i < requiredRanks.size(); ++i) {
            const bool isTaught = i < taught.size() && taught[i];
            if (IsUnlocked(requiredRanks[i], playerRank, isTaught)) {
                continue;
            }
            if (requiredRanks[i] < bestRank) {
                bestRank = requiredRanks[i];
                best     = static_cast<int>(i);
            }
        }
        return best;
    }

    // Field-test lesson target: preserve the valuable locked-first rule,
    // then fall back to the lowest-tier untaught chart. A rank-5 player can
    // therefore still learn a named song and exercise the persisted NEW
    // flow instead of every paid lesson becoming an invisible no-op.
    inline int PickLessonTarget(const std::vector<int>& requiredRanks,
                                const std::vector<bool>& taught,
                                int playerRank) {
        const int locked =
            PickTeachingTarget(requiredRanks, taught, playerRank);
        if (locked >= 0) { return locked; }
        int best = -1, bestRank = 99;
        for (std::size_t i = 0; i < requiredRanks.size(); ++i) {
            if (i < taught.size() && taught[i]) { continue; }
            if (requiredRanks[i] < bestRank) {
                bestRank = requiredRanks[i];
                best = static_cast<int>(i);
            }
        }
        return best;
    }
}
