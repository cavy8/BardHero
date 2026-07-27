// src/game/StarsLogic.h
#pragma once

// PURE star/progression math for the SGT integration (no RE/OS includes -
// suite StarsTests). Spec: docs/specs/2026-07-19-sgt-integration.md.
#include <cmath>
#include <cstdint>

namespace SH::stars {

    // The three SGT instruments; values are the ledger/serialization codes.
    enum class Instrument : std::uint8_t { kLute = 0, kFlute = 1, kDrum = 2 };
    inline constexpr int kInstrumentCount = 3;

    // ---- stars from final accuracy (thresholds inclusive) ----------------
    struct StarParams {
        double t[5] = { 0.50, 0.65, 0.80, 0.90, 0.96 };
    };
    inline int StarsFromAccuracy(double a_acc, const StarParams& a_p) {
        int s = 0;
        for (int i = 0; i < 5; ++i) {
            if (a_acc >= a_p.t[i]) { s = i + 1; }
        }
        return s;
    }

    // ---- long-performance expertise feed --------------------------------
    // Short BardHero songs already receive the skill-weighted completion XP
    // in EndingLogic. Only a set longer than the reference earns this extra
    // endurance bonus; the old 35s parity formula made one long song jump a
    // substantial fraction of an expertise band.
    struct FeedParams {
        double base   = 3.0;  // approximates SGT's average roll
        double minAcc = 0.5;  // below this: no bonus
        double referenceSec = 120.0;
    };
    inline int FeedBonus(double a_acc, double a_songSec,
                         const FeedParams& a_p) {
        const double reference =
            a_p.referenceSec > 0.0 ? a_p.referenceSec : 120.0;
        if (a_acc < a_p.minAcc || a_songSec <= reference) { return 0; }
        return static_cast<int>(
            std::lround(a_acc * (a_songSec / reference - 1.0) * a_p.base));
    }

    // ---- effective-tier promotion ----------------------------------------
    // A 5* run evaluates SGT's payout as at least Pretty Good (66), a 4*
    // run as at least OK (46). Deliberately never reaches Master (86).
    struct PromotionParams {
        int floor4 = 46;
        int floor5 = 66;
    };
    inline int PromotionFloor(int a_stars, const PromotionParams& a_p) {
        if (a_stars >= 5) { return a_p.floor5; }
        if (a_stars == 4) { return a_p.floor4; }
        return 0;
    }
    // Restore after the payout window: keep SGT's own grant (cur - promoted)
    // on top of the pre-promotion value. Never negative-grants.
    inline int RestoredValue(int a_actualBefore, int a_promoted, int a_cur) {
        const int grant = a_cur - a_promoted;
        return a_actualBefore + (grant > 0 ? grant : 0);
    }

    // ---- star-clamped rank gate ------------------------------------------
    // SGT tiers flip at 26/46/66/86; expertise clamps at boundary-1 until
    // the star gate is met. Lifted bits are MONOTONIC (persisted): charts
    // uninstalled later never re-clamp a lifted boundary.
    struct GateParams {
        int  need3[4]   = { 2, 3, 4, 5 };  // distinct charts at >=3*
        bool need5At85  = true;            // boundary 85 also needs one 5*
    };
    struct GateCounts {
        int chartsAt3 = 0;  // distinct charts with best >= 3*
        int chartsAt5 = 0;  // distinct charts with best >= 5*
    };
    inline constexpr int kBoundary[4] = { 25, 45, 65, 85 };

    inline std::uint8_t UpdateLifted(std::uint8_t a_lifted,
                                     const GateCounts& a_c,
                                     const GateParams& a_p) {
        for (int i = 0; i < 4; ++i) {
            const bool met =
                a_c.chartsAt3 >= a_p.need3[i] &&
                (i < 3 || !a_p.need5At85 || a_c.chartsAt5 >= 1);
            if (met) { a_lifted |= static_cast<std::uint8_t>(1u << i); }
        }
        return a_lifted;
    }
    inline int ClampCeiling(std::uint8_t a_lifted) {
        for (int i = 0; i < 4; ++i) {
            if (!(a_lifted & (1u << i))) { return kBoundary[i]; }
        }
        return 100;
    }
}
