#pragma once

#include "game/ResultsLogic.h"

#include <cmath>

// Pure, absolute-time Results progression reveal. Draw() asks for the state
// at elapsed seconds, so frame cadence cannot change the XP count, meter fill
// or rank-up phase.
namespace SH::results_anim {
    struct Reveal {
        int   shownXp;
        int   displayedExpertise;
        float baseFrac;
        float meterFrac;
        bool  rankUpActive;
        float rankUpPulse;
        bool  showingNewRank;
    };

    struct StarReveal {
        float scale;
        float angle;
        float alpha;
        float glow;
    };

    struct StarShine {
        float center;
        float width;
        float alpha;
    };

    struct StarGlow {
        float radiusScale;
        float alpha;
    };

    inline float Clamp01(double value) {
        if (value <= 0.0) { return 0.0f; }
        if (value >= 1.0) { return 1.0f; }
        return static_cast<float>(value);
    }

    inline float EaseOut(float t) {
        const float inv = 1.0f - t;
        return 1.0f - inv * inv * inv;
    }

    inline int LerpInt(int from, int to, float t) {
        const int delta = to > from ? to - from : 0;
        return from + static_cast<int>(delta * t + 0.5f);
    }

    // Earned stars arrive one by one. Absolute time keeps this independent
    // from frame cadence; the brief scale overshoot is the landing impact.
    inline StarReveal StarAt(double elapsed, int index, bool earned) {
        if (!earned) { return { 1.0f, 0.0f, 0.0f, 0.0f }; }
        constexpr double kFirst = 0.18;
        constexpr double kStep = 0.17;
        constexpr double kLand = 0.46;
        const float t = Clamp01(
            (elapsed - kFirst - index * kStep) / kLand);
        const float ease = EaseOut(t);
        constexpr float kPi = 3.14159265358979323846f;
        const float impact = std::sin(kPi * t);
        return {
            0.20f + 0.80f * ease + 0.20f * impact,
            -2.35f * (1.0f - ease),
            t,
            impact,
        };
    }

    // After the last possible landing (t=1.32s), a narrow glint periodically
    // travels across the earned stars. This is a pure absolute-time state:
    // frame rate can skip across it, but can never move or accumulate it.
    inline StarShine ShineAt(double elapsed, int earnedStars) {
        if (earnedStars <= 0 || elapsed < 1.45) {
            return { -1.0f, 0.18f, 0.0f };
        }
        constexpr double kPeriod = 1.80;
        constexpr double kTravel = 0.70;
        const double phase = std::fmod(elapsed - 1.45, kPeriod);
        if (phase < 0.0 || phase >= kTravel) {
            return { -1.0f, 0.18f, 0.0f };
        }
        constexpr float kPi = 3.14159265358979323846f;
        const float t = Clamp01(phase / kTravel);
        return {
            -0.18f + 1.36f * t,
            0.18f,
            0.72f * std::sin(kPi * t),
        };
    }

    // A settled earned star keeps a slow breathing halo. Like every Results
    // animation this is absolute-time: reopening at the same elapsed time
    // gives the same pixels, independent of frame cadence.
    inline StarGlow SettledGlowAt(double elapsed, int index, bool earned) {
        if (!earned || index < 0) { return { 1.0f, 0.0f }; }
        constexpr double kFirst = 0.18;
        constexpr double kStep = 0.17;
        constexpr double kLand = 0.46;
        constexpr double kPeriod = 2.40;
        const double settledAt = kFirst + index * kStep + kLand;
        if (elapsed < settledAt) { return { 1.0f, 0.0f }; }
        const double phase = std::fmod(elapsed - settledAt, kPeriod) / kPeriod;
        constexpr double kTau = 6.28318530717958647692;
        const float wave =
            static_cast<float>(std::sin(kTau * phase) * 0.5 + 0.5);
        return { 1.20f + 0.12f * wave, 0.05f + 0.07f * wave };
    }

    // The five-star row uses gap/radius = 25/9. Keeping every halo at or
    // below 1.32 radii guarantees neighboring silhouettes never intersect.
    [[nodiscard]] inline float ImpactGlowScale(float glow) {
        return 1.18f + 0.14f * Clamp01(glow);
    }

    inline Reveal At(double elapsed, int before, int after, int xpGain,
                     bool rankUp) {
        const auto beforeProgress = results::ProgressFor(before);
        const auto afterProgress = results::ProgressFor(after);
        const double xpDuration = rankUp ? 1.95 : 1.25;
        const float xpT = EaseOut(Clamp01((elapsed - 0.15) / xpDuration));
        const int safeXp = xpGain > 0 ? xpGain : 0;
        const int shownXp =
            static_cast<int>(safeXp * xpT + 0.0001f);

        if (!rankUp) {
            const float beforeFrac =
                static_cast<float>(beforeProgress.frac);
            const float afterFrac =
                static_cast<float>(afterProgress.frac);
            return {
                shownXp,
                LerpInt(before, after, xpT),
                beforeFrac,
                beforeFrac + (afterFrac - beforeFrac) * xpT,
                false,
                0.0f,
                true,
            };
        }

        constexpr double kOldFillStart = 0.15;
        constexpr double kOldFillSec = 0.70;
        constexpr double kRankStart = 0.75;
        constexpr double kRankPeak = 1.15;
        constexpr double kRankEnd = 1.65;
        constexpr double kNewFillStart = 1.35;
        constexpr double kNewFillSec = 0.85;
        const float oldFill =
            EaseOut(Clamp01((elapsed - kOldFillStart) / kOldFillSec));
        const float newFill =
            EaseOut(Clamp01((elapsed - kNewFillStart) / kNewFillSec));
        const bool showingNewRank = elapsed >= kNewFillStart;
        const bool rankUpActive =
            elapsed >= kRankStart && elapsed < kRankEnd;
        float pulse = 0.0f;
        if (rankUpActive) {
            const double half =
                elapsed <= kRankPeak
                    ? (elapsed - kRankStart) / (kRankPeak - kRankStart)
                    : (kRankEnd - elapsed) / (kRankEnd - kRankPeak);
            pulse = Clamp01(half);
        }

        if (!showingNewRank) {
            const float beforeFrac =
                static_cast<float>(beforeProgress.frac);
            return {
                shownXp,
                before,
                beforeFrac,
                beforeFrac + (1.0f - beforeFrac) * oldFill,
                rankUpActive,
                pulse,
                false,
            };
        }

        const int newBandStart = afterProgress.lo;
        const float afterFrac =
            static_cast<float>(afterProgress.frac);
        return {
            shownXp,
            LerpInt(newBandStart, after, newFill),
            0.0f,
            afterFrac * newFill,
            rankUpActive,
            pulse,
            true,
        };
    }
}
