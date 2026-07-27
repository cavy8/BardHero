#pragma once

// Steal-on-conflict assignment and factory defaults for the Bindings tab.
// Pure - the tab passes pointers at the Settings fields in UI order; the
// suite passes locals. Defaults come from the InputMapper structs so reset
// can never drift from what the mapper itself boots with.

#include <array>
#include <cstddef>

#include "game/InputMapper.h"

namespace SH::binding_edit {

    // Assigns a_code to a_slots[a_idx]; clears every OTHER slot in the
    // column holding a_code (0 = unbound). Returns the first cleared
    // index, or -1 (also -1 for a self-rebind). The owner-approved policy
    // is steal, never block: no dead-end states, the tinted loser row
    // shows what needs rebinding.
    inline int Assign(int* const* a_slots, std::size_t a_count,
                      std::size_t a_idx, int a_code) {
        if (a_idx >= a_count || !a_slots[a_idx]) { return -1; }
        int stolen = -1;
        if (a_code != 0) {
            for (std::size_t i = 0; i < a_count; ++i) {
                if (i == a_idx || !a_slots[i]) { continue; }
                if (*a_slots[i] == a_code) {
                    *a_slots[i] = 0;
                    if (stolen < 0) {
                        stolen = static_cast<int>(i);
                    }
                }
            }
        }
        *a_slots[a_idx] = a_code;
        return stolen;
    }

    // UI order: fret 1-5, strum, star power, whammy, pause.
    [[nodiscard]] inline std::array<int, 9> KeyboardDefaults() {
        const Binds b{};
        return { b.fret[0], b.fret[1], b.fret[2], b.fret[3], b.fret[4],
                 b.strum,   b.sp,      b.whammy,  b.pause };
    }

    // UI order: fret 1-5, strum up, strum down, star power, whammy, pause.
    [[nodiscard]] inline std::array<int, 10> GamepadDefaults() {
        const GamepadBinds g{};
        return { g.fret[0], g.fret[1], g.fret[2], g.fret[3], g.fret[4],
                 g.strum[0], g.strum[1], g.sp, g.whammy, g.pause };
    }
}
