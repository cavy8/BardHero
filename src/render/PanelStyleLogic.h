#pragma once

// PURE modal-surface policy. The FLICK-facing drawing adapter lives in
// PanelStyle.h; this tiny value object keeps geometry choices headless-tested.
namespace SH::panel_style {
    struct SurfaceStyle {
        float nearInset    = 4.0f;
        float farInset     = 10.0f;
        float rounding     = 9.0f;
        float shadowOffset = 5.0f;
    };

    [[nodiscard]] constexpr SurfaceStyle ModalSurface() { return {}; }

    // The field triangle belonged to Highway's transparent host, not this
    // panel. Pause therefore uses the same radius/insets/shadow as every
    // other BardHero modal.
    [[nodiscard]] constexpr SurfaceStyle PauseSurface() {
        return ModalSurface();
    }
}
