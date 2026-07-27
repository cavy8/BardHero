#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "chart/ChartParser.h"
#include "chart/ChartTypes.h"
#include "chart/Smf.h"
#include "chart/TempoMap.h"

// Section-marker extraction for practice mode (spec
// 2026-07-26-practice-mode 5.1). Pure: no filesystem, no engine.
namespace bard::practice {

    // Parses ONE .chart [Events] value, e.g. `E "section Intro"`.
    // Returns false for every event that is not a section marker - the
    // same list also carries solo/soloend/end and arbitrary charter
    // notes. a_name is only written on success and is never empty.
    bool SectionNameFromChartEvent(std::string_view a_value,
                                   std::string& a_name);

    // Parses ONE .mid text event, e.g. `[section Intro]`.
    bool SectionNameFromMidText(std::string_view a_text,
                                std::string& a_name);

    // Every section marker in a parsed .chart, sorted by tick, unique
    // ticks (last wins at a tie). Times come from the chart's own tempo
    // map plus a_offsetSeconds, so they land in the same domain as
    // Note::time.
    std::vector<ChartSection> SectionsFromChart(const ChartFile& a_chart,
                                                double a_offsetSeconds);

    // Same, from a parsed .mid. Only the EVENTS track is read.
    // a_tempo must ALREADY be finalized: SecondsAt only asserts it, and
    // asserts compile out of the release preset.
    std::vector<ChartSection> SectionsFromMid(const SmfFile& a_file,
                                              const TempoMap& a_tempo,  // finalized
                                              double a_offsetSeconds);
}
