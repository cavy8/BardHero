#pragma once

#include "game/BandPerformanceLogic.h"

#include <string_view>

namespace bard::BandStage {
    void Begin(
        int a_instrumentContext,
        const band::StemAvailability& a_stems);
    bool Active();
    void Tick(double a_now);
    void EndAsync(std::string_view a_reason);
    void EndNow(std::string_view a_reason, bool a_immediate = false);
}
