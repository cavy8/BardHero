#pragma once

#include <cmath>

namespace SH::backdrop {
    struct Rect {
        float loX = 0.0f;
        float loY = 0.0f;
        float hiX = 0.0f;
        float hiY = 0.0f;
        float rounding = 0.0f;
    };

    // Screen-draw-list backdrops are deliberately overscanned. A quad whose
    // last vertex lands exactly on a fractional viewport edge can expose an
    // antialiased/rounded strip; ceil plus one pixel covers every supported
    // FLICK scale without changing any foreground/highway geometry.
    [[nodiscard]] inline Rect FullBleed(float width, float height) {
        return { -1.0f, -1.0f, std::ceil(width) + 1.0f,
                 std::ceil(height) + 1.0f, 0.0f };
    }
}
