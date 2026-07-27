#pragma once
#include "chart/ChartTypes.h"
#include "chart/Smf.h"

namespace bard {

    struct MidOptions {
        // song.ini overrides (SongIni task wires them); <0 = format default.
        std::int64_t sustainCutoffTicks = -1;  // default: division/3
        std::int64_t hopoThresholdTicks = -1;  // default: division/3 + 1
        int          spNote = -1;              // multiplier_note: 103/116 only
    };

    // difficulty: 0=Easy 1=Medium 2=Hard 3=Expert (base = 60 + 12*d).
    // Tempo/TS come from the SmfFile's file-wide vectors; the caller builds
    // the TempoMap (division = resolution).
    bool BuildRawTrackFromMid(const SmfFile& f, int difficulty,
                              const MidOptions& opt, RawTrack& out);
}
