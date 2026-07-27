#pragma once

namespace SH::pause_layout {
    inline constexpr float kDefaultWidth       = 380.0f;
    // Four rows since 2026-07-26: Resume / Restart / Practice / Quit.
    //
    // A SEED for the first frame, not the answer.
    //
    // This constant has been hand-tuned twice now and been wrong twice: 440
    // clipped the Quit button, 380 left a dead band under it (both field
    // 2026-07-26). It cannot be got right from here, because the real drawn
    // height depends on ImGui's runtime ItemSpacing.y and on the measured
    // width of the header font's text - neither of which a pure header can
    // see. A third guess would just buy a third screenshot.
    //
    // So the panel MEASURES ITSELF instead: it reads the layout cursor after
    // the last row and feeds that through WindowHeightForContent below.
    // The window carries kNoMove|kNoResize, which makes the host re-read
    // GetDefaultSize every frame, so the correction lands the frame after
    // the first draw and then holds exactly.
    //
    // The seed still has to be big enough not to CLIP on frame one, though,
    // which is what the Measure() slack test guards - a seed that is too
    // small shows a clipped panel for one frame before correcting itself.
    // Too LARGE is harmless, because the measurement shrinks it. So this
    // errs high: 400 clears five rows with room, where 380 did not (it was
    // tuned for four).
    inline constexpr float kDefaultHeight      = 400.0f;
    // Rows the menu draws. Measure() and the window both read it, so adding
    // an entry cannot leave the panel sized for the old count - which is how
    // a button ends up clipped below the bounded content. Since the panel
    // measures itself this is no longer load-bearing for the HEIGHT, but it
    // still drives navigation wrap.
    //
    // Five since 2026-07-26: Resume / Restart / Practice / Settings / Quit.
    inline constexpr int   kButtonCount        = 5;
    inline constexpr float kContentInset       = 20.0f;
    inline constexpr float kHostVerticalBudget = 32.0f;
    inline constexpr float kHeaderReserve      = 60.0f;
    inline constexpr float kTopGap             = 8.0f;
    inline constexpr float kButtonHeight       = 42.0f;
    inline constexpr float kButtonGap          = 8.0f;

    struct Fit {
        float contentHeight;
        float contentBottom;
    };

    // NOTE: contentBottom is the sum of the pieces this header knows about.
    // It deliberately does NOT include ImGui's per-element ItemSpacing, which
    // is a runtime theme value - so it is a LOWER BOUND on the real drawn
    // height, not the height itself. Size the window off kDefaultHeight,
    // which carries the slack for it.
    [[nodiscard]] constexpr Fit Measure(float a_scale) {
        return {
            (kDefaultHeight - kHostVerticalBudget -
             2.0f * kContentInset) * a_scale,
            (kHeaderReserve + kTopGap +
             static_cast<float>(kButtonCount) * kButtonHeight +
             static_cast<float>(kButtonCount - 1) * kButtonGap) * a_scale,
        };
    }

    // The inverse of Measure(): given the content height the panel ACTUALLY
    // drew (the layout cursor after the last row, in pixels), the window
    // height that fits it exactly. This is what closes the loop - the caller
    // measures, this converts, GetDefaultSize returns it next frame.
    //
    // a_drawnContent is already scaled, because it came from the live
    // layout; only the chrome terms take a_scale here. Clamped to the seed
    // as a floor so a measurement taken on a frame that drew nothing (a
    // hidden window, a torn-down session) cannot collapse the panel to
    // nothing on the frame after.
    [[nodiscard]] constexpr float WindowHeightForContent(
        float a_drawnContent, float a_scale) {
        const float chrome =
            (kHostVerticalBudget + 2.0f * kContentInset) * a_scale;
        const float wanted = a_drawnContent + chrome;
        const float floorH = (kHeaderReserve + kTopGap +
                              static_cast<float>(kButtonCount) *
                                  kButtonHeight) * a_scale;
        return wanted > floorH ? wanted : floorH;
    }

    // Runtime maps this policy to ImGuiWindowFlags_NoScrollbar and
    // ImGuiWindowFlags_NoScrollWithMouse. Keep the ImGui constants out of
    // this pure header so ResultsLogicTests stays platform-independent.
    [[nodiscard]] constexpr bool SuppressScrollbar() { return true; }
}
