#pragma once
#include <string>
#include <vector>

#include "chart/ChartTypes.h"
#include "engine/GuitarEngine.h"

namespace bard {

    // Binary replay: "SHRP" magic, u32 version=1, u32 count, NoteInput[].
    bool SaveReplay(const std::string& path, const std::vector<NoteInput>& in);
    bool LoadReplay(const std::string& path, std::vector<NoteInput>& out);

    struct ReplayResult {
        EngineStats           stats;
        std::vector<Judgment> judgments;
    };

    // Drive one replay at a fixed update cadence: inputs are delivered in
    // the first Update whose time >= input.time (frame chunking, the YARG
    // model). Identical results across cadences is THE acceptance test for
    // the sub-step scheduler (spec 11).
    ReplayResult RunReplay(const ParsedChart& chart, const EngineParams& p,
                           const std::vector<NoteInput>& inputs,
                           double cadenceHz, double endTime);
}
