#pragma once
#include <cstdint>

namespace bard {
    enum class InputAction : std::uint8_t {
        kFret1, kFret2, kFret3, kFret4, kFret5,
        kStrum, kStarPower, kWhammy, kPause
    };
    // POD, 16 bytes, replay-serializable (spec section 3).
    struct NoteInput {
        double       time;    // song-relative seconds, calibration applied
        InputAction  action;
        std::int32_t value;   // bool pressed / axis for whammy
    };
    static_assert(sizeof(NoteInput) == 16);
}
