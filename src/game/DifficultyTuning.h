#pragma once

#include "../../bardcore/engine/EngineParams.h"
#include "game/CrowdMoodLogic.h"

#include <algorithm>
#include <cmath>

namespace SH::difficulty {
    // Player-facing difficulty controls. Defaults exactly reproduce the
    // field-tested 2026-07-24 timing, Glory, and failure policy.
    struct Tuning {
        double hitWindowScale          = 1.0;
        double strumLeniencySec        = 0.050;
        double earlyStrumLeniencySec   = 0.025;
        double hopoLeniencySec         = 0.080;
        double sustainDropLeniencySec  = 0.025;
        bool   infiniteFrontEnd        = false;
        bool   antiGhosting            = true;
        int    maxMultiplier           = 4;

        // GH-parity rebalance 2026-07-25 (user: "it's currently very easy
        // to fail, match Guitar Hero balance"). Guitar Hero's rock meter
        // is forgiving per mistake and unforgiving only about SUSTAINED
        // failure: a mistake costs a few notes of progress, every clean
        // hit refills immediately, and you die from a bad run, never from
        // a bad bar. The old numbers inverted that - one mistake cost
        // 7.5 hits AND froze recovery for the next 12 clean notes, so a
        // single fumbled phrase was most of the way to a fail.
        //
        // 1 mistake = 4 hits of progress (GH sits near 1:3-1:4). Meter
        // spans 300 hit-steps, so from the 0.5 start it takes ~26
        // unanswered mistakes to even reach the danger zone, and the
        // fail gate below still wants sustained danger on top of that.
        double gloryMeterSpanHits      = 300.0;
        double gloryBadWeight          = 4.0;
        double gloryStarPowerHitScale  = 1.25;
        double gloryStarPowerBadScale  = 0.80;
        double gloryOpeningSec         = 8.0;
        double gloryOpeningBadScale    = 0.25;
        // GH has NO recovery gate at all. Three is the smallest value
        // that still answers the field note this mechanic was added for
        // (raw note volume buying recovery on dense charts) without
        // eating a whole phrase after every mistake.
        int    gloryRecoveryHits       = 3;
        double gloryRedBelow           = 1.0 / 3.0;
        double gloryGreenAt            = 2.0 / 3.0;

        // Failure is the meter bottoming out and STAYING there, GH-style:
        // deeper danger line, longer grace, and more further mistakes
        // required before the crowd actually walks. Nothing can fail in
        // the first 8 seconds - that window belongs to input settle and
        // the animation start, not to judgment.
        double failureDangerBelow      = 0.15;
        double failureRecoverAt        = 0.30;
        double failureGraceSec         = 5.0;
        double failureStartSec         = 8.0;
        int    failureFurtherBad       = 4;

        double audienceCommentDelaySec = 6.0;
    };

    namespace detail {
        inline double FiniteOr(double a_value, double a_fallback) {
            return std::isfinite(a_value) ? a_value : a_fallback;
        }
    }

    inline void Normalize(Tuning& a_tuning) {
        const Tuning defaults;
        auto bounded = [&](double& a_value, double a_min, double a_max,
                           double a_fallback) {
            a_value = std::clamp(
                detail::FiniteOr(a_value, a_fallback), a_min, a_max);
        };

        bounded(a_tuning.hitWindowScale, 0.50, 2.00,
                defaults.hitWindowScale);
        bounded(a_tuning.strumLeniencySec, 0.0, 0.200,
                defaults.strumLeniencySec);
        bounded(a_tuning.earlyStrumLeniencySec, 0.0, 0.150,
                defaults.earlyStrumLeniencySec);
        bounded(a_tuning.hopoLeniencySec, 0.0, 0.250,
                defaults.hopoLeniencySec);
        bounded(a_tuning.sustainDropLeniencySec, 0.0, 0.150,
                defaults.sustainDropLeniencySec);
        a_tuning.maxMultiplier =
            std::clamp(a_tuning.maxMultiplier, 1, 8);

        bounded(a_tuning.gloryMeterSpanHits, 50.0, 1000.0,
                defaults.gloryMeterSpanHits);
        bounded(a_tuning.gloryBadWeight, 0.50, 30.0,
                defaults.gloryBadWeight);
        bounded(a_tuning.gloryStarPowerHitScale, 0.0, 3.0,
                defaults.gloryStarPowerHitScale);
        bounded(a_tuning.gloryStarPowerBadScale, 0.0, 2.0,
                defaults.gloryStarPowerBadScale);
        bounded(a_tuning.gloryOpeningSec, 0.0, 15.0,
                defaults.gloryOpeningSec);
        bounded(a_tuning.gloryOpeningBadScale, 0.0, 1.0,
                defaults.gloryOpeningBadScale);
        a_tuning.gloryRecoveryHits =
            std::clamp(a_tuning.gloryRecoveryHits, 0, 100);
        bounded(a_tuning.gloryRedBelow, 0.05, 0.90,
                defaults.gloryRedBelow);
        bounded(a_tuning.gloryGreenAt, a_tuning.gloryRedBelow, 0.95,
                defaults.gloryGreenAt);

        bounded(a_tuning.failureDangerBelow, 0.01, 0.90,
                defaults.failureDangerBelow);
        bounded(a_tuning.failureRecoverAt, a_tuning.failureDangerBelow, 1.0,
                defaults.failureRecoverAt);
        bounded(a_tuning.failureGraceSec, 0.0, 15.0,
                defaults.failureGraceSec);
        bounded(a_tuning.failureStartSec, 0.0, 30.0,
                defaults.failureStartSec);
        a_tuning.failureFurtherBad =
            std::clamp(a_tuning.failureFurtherBad, 1, 20);
        bounded(a_tuning.audienceCommentDelaySec, 0.0, 30.0,
                defaults.audienceCommentDelaySec);
    }

    [[nodiscard]] inline bard::EngineParams EngineParamsFor(
        Tuning a_tuning) {
        Normalize(a_tuning);
        auto params = bard::EngineParams::Default();
        params.maxWindow *= a_tuning.hitWindowScale;
        params.minWindow *= a_tuning.hitWindowScale;
        params.strumLeniency = a_tuning.strumLeniencySec;
        params.strumLeniencySmall = a_tuning.earlyStrumLeniencySec;
        params.hopoLeniency = a_tuning.hopoLeniencySec;
        params.sustainDropLeniency = a_tuning.sustainDropLeniencySec;
        params.infiniteFrontEnd = a_tuning.infiniteFrontEnd;
        params.antiGhosting = a_tuning.antiGhosting;
        params.maxMultiplier = a_tuning.maxMultiplier;
        return params;
    }

    // a_songNotes sizes the meter to THIS SONG rather than to an absolute
    // note count - see crowd::MeterSpanForSong for why a flawless short
    // song could not reach the green zone at all. 0 means "the caller has
    // no chart", which keeps the tuned span exactly as configured.
    [[nodiscard]] inline crowd::RockParams RockParamsFor(
        Tuning a_tuning, int a_songNotes = 0) {
        Normalize(a_tuning);
        crowd::RockParams params;
        const double span = crowd::MeterSpanForSong(
            a_tuning.gloryMeterSpanHits, a_songNotes);
        params.hitGain = 1.0 / span;
        // The bad:good RATIO is the tuned quantity and survives the
        // rescale - a short song is not a more forgiving song, it is one
        // where every note is a larger share of the performance.
        params.badLoss = a_tuning.gloryBadWeight / span;
        params.spHitScale = a_tuning.gloryStarPowerHitScale;
        params.spBadScale = a_tuning.gloryStarPowerBadScale;
        params.openingSec = a_tuning.gloryOpeningSec;
        params.openingBadScale = a_tuning.gloryOpeningBadScale;
        params.recoveryHitsRequired = crowd::RecoveryHitsForSpan(
            a_tuning.gloryRecoveryHits, span,
            a_tuning.gloryMeterSpanHits);
        params.terribleBelow = a_tuning.gloryRedBelow;
        params.greatAt = a_tuning.gloryGreenAt;
        return params;
    }

    [[nodiscard]] inline crowd::FailureParams FailureParamsFor(
        Tuning a_tuning) {
        Normalize(a_tuning);
        crowd::FailureParams params;
        params.dangerBelow = a_tuning.failureDangerBelow;
        params.recoverAt = a_tuning.failureRecoverAt;
        params.graceSec = a_tuning.failureGraceSec;
        params.startSec = a_tuning.failureStartSec;
        params.furtherBadRequired = a_tuning.failureFurtherBad;
        return params;
    }

    [[nodiscard]] inline bool AudienceCommentsAllowed(
        double a_songSec, Tuning a_tuning) {
        Normalize(a_tuning);
        return a_songSec >= 0.0 &&
               a_songSec >= a_tuning.audienceCommentDelaySec;
    }
}
