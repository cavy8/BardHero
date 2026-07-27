#pragma once

// PURE composition of the public FLICK flag values supplied by the runtime
// call site. Keeping the arguments symbolic avoids duplicating FLICK's bit
// assignments while making the overlay host's chrome contract testable.
namespace SH::flick_window_policy {
    [[nodiscard]] constexpr unsigned HighwayHostFlags(
        unsigned noDecoration, unsigned noBackground, unsigned noMove,
        unsigned noResize, unsigned passInput, unsigned hideHud,
        unsigned blockVanity, bool allowPerformanceVanity,
        unsigned renderDuringTm,
        unsigned closeOnGameMenu) {
        return noDecoration | noBackground | noMove | noResize | passInput |
               hideHud |
               (allowPerformanceVanity ? 0u : blockVanity) |
               renderDuringTm | closeOnGameMenu;
    }
}
