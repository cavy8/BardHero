#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "chart/ChartTypes.h"

namespace bard {

    // Raw .chart file: [Song] metadata + finalized TempoMap + per-section
    // entry lists. BuildRawTrack maps one difficulty section to a RawTrack.
    struct ChartFile {
        std::uint32_t resolution    = 192;
        double        offsetSeconds = 0.0;  // .chart Offset IS seconds
        SongMeta      meta;
        TempoMap      tempo;  // finalized
        std::vector<TimeSig> timeSigs;
        // section name -> entries (tick, rest-of-line), both trimmed
        std::unordered_map<std::string,
                           std::vector<std::pair<std::uint32_t, std::string>>>
            sections;
    };

    bool ParseChartText(std::string_view text, ChartFile& out);
    bool BuildRawTrack(const ChartFile& cf, const std::string& section,
                       RawTrack& out);
}
