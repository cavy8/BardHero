#pragma once
#include "chart/ChartTypes.h"

namespace bard {
    // RawTrack -> ParsedChart notes/phrases/solos. THE single home of the
    // natural-HOPO rule, forcing semantics, tap override, time resolution,
    // sustain-end resolution and extended-sustain detection. offsetSeconds =
    // chart offset + ini delay (already combined by the caller).
    ParsedChart Normalize(const RawTrack& track, const TempoMap& tempo,
                          double offsetSeconds);

    // Shared post-pass for the ini sustain-cutoff override (spec 4.2): zero
    // every lane sustain shorter than cutoffTicks. Called by BOTH front-end
    // callers before Normalize.
    void ApplySustainCutoff(RawTrack& track, std::int64_t cutoffTicks);
}
