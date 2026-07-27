#pragma once

#include "practice/PracticeRange.h"

// Practice loop decisions + the no-recording rules (spec
// 2026-07-26-practice-mode 6.3 / 6.4).
namespace bard::practice {

    struct LoopParams {
        // Breath between the last note of the range and the snap back.
        double restartDelaySec = 1.5;
    };

    // a_songSec is the same clock the highway renders from.
    // a_speed is the playback multiplier (1.0 until Phase 2 ships speed).
    [[nodiscard]] inline bool ShouldRestart(double a_songSec,
                                            double a_rangeEndSec,
                                            double a_speed,
                                            const LoopParams& a_params = {}) {
        // A zero or negative speed is a caller bug. Left alone it drags
        // the threshold down to a_rangeEndSec (a restart with no breath
        // at all) or below it (a restart that fires BEFORE the range
        // ends - a per-frame spin on any range shorter than the delay).
        // Substituting 1.0 degrades to normal-speed looping instead.
        const double speed = a_speed > 0.0 ? a_speed : 1.0;
        const double delay =
            a_params.restartDelaySec > 0.0 ? a_params.restartDelaySec : 0.0;
        return a_songSec >= a_rangeEndSec + delay * speed;
    }

    // ---- playback speed (spec 2, Clone Hero / YARG convention) --------
    inline constexpr double kSpeedStep = 0.05;
    inline constexpr double kSpeedMin  = 0.50;
    inline constexpr double kSpeedMax  = 1.50;

    // CLAMPS rather than wraps, for the same reason difficulty stepping
    // does: holding the key past the limit must sit at the limit, not drop
    // the player from half speed to fastest without noticing.
    //
    // Snapping to the 5% grid before stepping is what keeps repeated
    // adjustments on the grid. Stepping the raw value instead would let
    // floating-point residue accumulate until "1.00x" on the HUD was really
    // 0.9999, and the speed==1.0 fast path in the audio callback - which is
    // an exact comparison on purpose - would silently stop being taken.
    [[nodiscard]] inline constexpr double StepSpeed(double a_current,
                                                    int    a_delta) {
        const double base = a_current > 0.0 ? a_current : 1.0;
        const long   grid =
            static_cast<long>(base / kSpeedStep + 0.5) + a_delta;
        double out = static_cast<double>(grid) * kSpeedStep;
        if (out < kSpeedMin) { out = kSpeedMin; }
        if (out > kSpeedMax) { out = kSpeedMax; }
        return out;
    }

    // Where a restart seeks to. Its own function so the host cannot
    // accidentally seek to the section's raw time and skip the lead-in.
    [[nodiscard]] inline constexpr double RestartTime(
        const PracticeRange& a_range) {
        return a_range.startSec;
    }

    // What a session is allowed to touch. Practice sets every field
    // false BY DESIGN - this exists so the host reads one value instead
    // of scattering `if (practice)` branches, and so a future field
    // cannot default to "on" by omission.
    //
    // Each field names the ONE site that commits it, found by auditing
    // the live session. Keep these comments accurate: they are the map
    // a future reader uses to check nothing new slipped past the gate.
    struct PracticeSessionRules {
        bool recordStars    = false;  // StarLedger::Record, song end
        bool payGold        = false;  // GoldScale arm/pay, song end
        bool feedExpertise  = false;  // SgtProgression::FinishPayout
        bool allowFailure   = false;  // the crowd failure gate
        bool crowdReactions = false;  // our own cheer/groan one-shots
        bool songEndStings  = false;  // the pass/fail sting cue
        bool clearNewTags   = false;  // MarkSeen, song START not end

        // The two below are NOT redundant with the seven above; each
        // covers a commit the original list missed.

        // SGT mood globals. Separate axis from crowdReactions on
        // purpose: crowdReactions gates OUR one-shot wavs, this gates
        // the SGT global writes that drive NPC dialogue and the Glory
        // commit. The engine treats them as independent settings and so
        // must we, or practice goes silent but still moves the world.
        bool commitGloryGlobals = false;

        // The rank-gate enforcement pass at session START. It mutates
        // the persisted lifted mask and can write the expertise global
        // DOWNWARD, so it is a real write even though nothing about it
        // looks like scoring. "Records nothing" has to include it.
        bool enforceRankGate = false;
    };

    [[nodiscard]] inline constexpr PracticeSessionRules PracticeRules() {
        return PracticeSessionRules{};
    }

    // The counterpart a normal performance reads. Its existence is what
    // lets the host resolve the mode ONCE - `practice ? PracticeRules()
    // : PerformanceRules()` - and then branch on named fields instead of
    // re-testing `if (practice)` at every one of the nine sites.
    [[nodiscard]] inline constexpr PracticeSessionRules PerformanceRules() {
        return PracticeSessionRules{ true, true, true, true, true,
                                     true, true, true, true };
    }
}
