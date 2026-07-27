// src/game/GoldScaleMath.h
#pragma once

// PURE math for the SGT gold scaling (no RE/OS includes - headless-tested).
#include <algorithm>
#include <cmath>

namespace SH::goldscale {
    struct Params {
        double accMin      = 0.0;  // multiplier at 0% accuracy
        double accMax      = 3.0;  // multiplier at 100% accuracy
        double diffMult[4] = { 0.5, 0.75, 1.0, 1.5 };  // Easy..Expert
    };

    // Gold DELTA for a captured SGT payout: positive = bonus to add,
    // negative = take-back. mult clamps >= 0, so the take-back can never
    // exceed the captured amount.
    inline int Delta(int captured, int notesHit, int notesTotal,
                     int difficulty, const Params& p) {
        if (captured <= 0 || notesTotal <= 0) { return 0; }
        const double a = std::clamp(
            static_cast<double>(notesHit) / notesTotal, 0.0, 1.0);
        const int    d    = std::clamp(difficulty, 0, 3);
        const double mult = std::max(
            0.0, (p.accMin + (p.accMax - p.accMin) * a) * p.diffMult[d]);
        return static_cast<int>(std::lround(captured * (mult - 1.0)));
    }
}
