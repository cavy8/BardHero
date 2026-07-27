#pragma once

// Practice section picker: geometry and selection decisions (spec
// 2026-07-26-practice-mode, plan P4). Pure - no ImGui, no FUCK, no RE - so
// it carries a headless suite (tests/test_practicelayout.cpp) in the
// PauseLayout.h mould.
//
// The picker is drawn as a SUB-VIEW of the Songbook rather than as its own
// FLICK window. That is a deliberate safety choice: a new window would need
// its own cursor refcount path, its own open-mirror flag, and a third
// `capture` value in InputHook.cpp - whose overlay selection is written as a
// BINARY ternary in two places (`capture == 1 ? kBrowser : kResults`), so a
// third overlay would silently inherit Results semantics. Hosting inside the
// Songbook means none of those exist to get wrong.

#include <iterator>

namespace SH::practice_layout {

    // Measured against the Songbook's content box, not the display: the
    // picker borrows its host's window.
    inline constexpr float kColumnGap    = 16.0f;
    inline constexpr float kHeaderReserve = 56.0f;
    // The Songbook footer has ZERO vertical budget and reserves its
    // header/body gutter by hand; a two-list child that assumes it can grow
    // reproduces the double-scrollbar bug. Reserve the footer explicitly and
    // measure the list zone from what is left.
    // Sized for EVERYTHING under the lists, not just the range line: the
    // range text, the speed-preset row, and the Loop/Play row. It was 44
    // when the footer was a single line, and the button row fell off the
    // bottom of the panel once the speed presets joined it (field
    // 2026-07-26). Grow this whenever a row is added below the lists.
    inline constexpr float kFooterReserve = 132.0f;
    inline constexpr float kRowHeight     = 26.0f;

    struct Columns {
        float width  = 0.0f;  // each list's width
        float leftX  = 0.0f;
        float rightX = 0.0f;
    };

    // Two equal columns with one gap. Never returns a negative width: a
    // host window narrower than the gap would otherwise ask ImGui for a
    // negative child size, which reads as "fill available" and overlaps the
    // two lists on top of each other.
    [[nodiscard]] constexpr Columns SplitColumns(float a_contentWidth) {
        const float raw = (a_contentWidth - kColumnGap) * 0.5f;
        const float w   = raw > 0.0f ? raw : 0.0f;
        return Columns{ w, 0.0f, w + kColumnGap };
    }

    // Height available to each list once the header and footer have taken
    // their reserved bands. Floors at one row so an empty section list still
    // draws its "Whole song" row instead of collapsing to nothing.
    [[nodiscard]] constexpr float ListHeight(float a_contentHeight) {
        const float h = a_contentHeight - kHeaderReserve - kFooterReserve;
        return h > kRowHeight ? h : kRowHeight;
    }

    // How many section rows actually FIT, given what the list spends on
    // things that are not rows.
    //
    // ⚠ THE ROWS DO NOT GET THE WHOLE LIST HEIGHT, and assuming they did is
    // what clipped the bottom row in the field (2026-07-26: "some items get
    // cut off"). Three things take space inside that child and every one of
    // them was unaccounted for:
    //
    //   - the list draws its own "From"/"To" TITLE inside the child;
    //   - the child carries the theme's WINDOW PADDING, top and bottom;
    //   - ImGui adds ITEM SPACING after each Selectable, so the row PITCH is
    //     kRowHeight + spacing, not kRowHeight.
    //
    // Both are passed in MEASURED (GetTextLineHeightWithSpacing, the style
    // vars) rather than guessed as constants, because they follow the host's
    // theme and font and a hard-coded number goes stale silently - the only
    // symptom is a half-drawn row that looks like a scrolling bug.
    //
    // a_chromeHeight is everything vertical that is not a row; a_rowPitch is
    // the real per-row advance. Flooring means a mismeasure costs a few
    // pixels of blank space at the bottom, never a clipped row.
    [[nodiscard]] constexpr int VisibleRows(float a_listHeight,
                                            float a_chromeHeight,
                                            float a_rowPitch) {
        const float pitch = a_rowPitch > 0.0f ? a_rowPitch : kRowHeight;
        const float rows  = a_listHeight - a_chromeHeight;
        if (!(rows > 0.0f)) { return 1; }
        const int n = static_cast<int>(rows / pitch);
        return n > 0 ? n : 1;
    }

    // Top row of the scroll window, computed HERE rather than by asking the
    // host to scroll to the selection. FUCK::SetScrollHereY is version-2
    // gated and silently no-ops on a v1 host, which walks keyboard selection
    // off-screen with no error and no way to see it went wrong. Owning the
    // window means the picker behaves identically on both hosts.
    //
    // a_current is the previous top row, so an already-visible selection
    // does not yank the view.
    [[nodiscard]] constexpr int FirstVisibleRow(int a_selected, int a_visible,
                                                int a_count, int a_current) {
        if (a_count <= 0 || a_visible <= 0) { return 0; }
        const int maxTop = a_count - a_visible > 0 ? a_count - a_visible : 0;
        int       top    = a_current < 0 ? 0 : (a_current > maxTop ? maxTop
                                                                  : a_current);
        if (a_selected < 0) { return top; }
        if (a_selected < top) { top = a_selected; }
        const int bottom = top + a_visible - 1;
        if (a_selected > bottom) { top = a_selected - a_visible + 1; }
        if (top > maxTop) { top = maxTop; }
        return top < 0 ? 0 : top;
    }
}

namespace SH::practice_pick {

    // Four focus stops, ordered as the panel is laid out top to bottom.
    // Speed and Loop joined on 2026-07-27; as mouse-only buttons they were
    // unreachable from a Guitar Hero controller.
    enum class Focus {
        kStart = 0,
        kEnd   = 1,
        kSpeed = 2,
        kLoop  = 3,
    };

    inline constexpr int kFocusCount = 4;

    struct State {
        Focus focus    = Focus::kStart;
        int   startIdx = 0;
        int   endIdx   = 0;
    };

    // The practice tempo table, HERE so the strum stepping and the mouse
    // buttons draw from the same list and cannot drift. Named rather than
    // numbered - "Slow" beats doing arithmetic on "70%".
    struct SpeedPreset {
        int         pct;
        const char* name;
    };

    inline constexpr SpeedPreset kSpeedPresets[] = {
        { 50, "Very Slow" }, { 60, "Slow" },   { 70, "Steady" },
        { 80, "Medium" },    { 90, "Fast" },   { 100, "Full Speed" },
    };
    inline constexpr int kSpeedPresetCount =
        static_cast<int>(std::size(kSpeedPresets));

    // Strum-steps the tempo through the presets. Only the SIGN of a_move
    // (+down / -up) is honoured, up-strum raises the tempo, and it CLAMPS
    // at both ends - holding past Full Speed sits at Full Speed.
    // DIRECTIONAL on purpose: faster is the smallest preset strictly above
    // the current speed, slower the largest strictly below, so a non-preset
    // value (the -/= keys move in 5% steps mid-run and persist) lands on
    // its visual neighbour instead of double-stepping.
    [[nodiscard]] constexpr double StepSpeedPreset(double a_current,
                                                   int a_move) {
        const int pct =
            static_cast<int>(a_current * 100.0 + (a_current >= 0 ? 0.5 : -0.5));
        if (a_move < 0) {
            for (int i = 0; i < kSpeedPresetCount; ++i) {
                if (kSpeedPresets[i].pct > pct) {
                    return kSpeedPresets[i].pct / 100.0;
                }
            }
            return kSpeedPresets[kSpeedPresetCount - 1].pct / 100.0;
        }
        if (a_move > 0) {
            for (int i = kSpeedPresetCount - 1; i >= 0; --i) {
                if (kSpeedPresets[i].pct < pct) {
                    return kSpeedPresets[i].pct / 100.0;
                }
            }
            return kSpeedPresets[0].pct / 100.0;
        }
        return a_current;
    }

    // One frame of navigation. a_move steps within the focused LIST and
    // WRAPS (list_navigation convention); on the kSpeed/kLoop stops it is
    // NOT applied here - the window routes it to StepSpeedPreset / its
    // loop flag, since tempo and loop persist on the window. a_diff moves
    // focus across the four stops and CLAMPS. start and end stay
    // INDEPENDENT: ResolveRange swaps a reversed pick, and the picker
    // displays what ResolveRange returns rather than second-guessing it.
    [[nodiscard]] constexpr State Step(State a_state, int a_move, int a_diff,
                                       int a_count) {
        State s = a_state;
        if (a_diff != 0) {
            int f = static_cast<int>(s.focus) + a_diff;
            if (f < 0) { f = 0; }
            if (f > kFocusCount - 1) { f = kFocusCount - 1; }
            s.focus = static_cast<Focus>(f);
        }
        if (a_count <= 0) {
            // No markers is the COMMON case. Pin the INDICES so a stray
            // strum cannot select a row that is not there - but keep the
            // focus: a markerless chart still has a tempo and a loop to
            // set, and a per-frame focus reset made both unreachable on a
            // controller.
            s.startIdx = 0;
            s.endIdx   = 0;
            return s;
        }

        if (a_move != 0
            && (s.focus == Focus::kStart || s.focus == Focus::kEnd)) {
            int& idx = s.focus == Focus::kStart ? s.startIdx : s.endIdx;
            const int base = idx >= 0 && idx < a_count
                                 ? idx
                                 : (a_move > 0 ? -1 : 0);
            const int moved = (base + a_move) % a_count;
            idx = moved < 0 ? moved + a_count : moved;
        }
        // Clamp both, not only the focused one: the section list is
        // republished whenever a different song is picked, and a shorter
        // chart must not leave a stale out-of-range index behind.
        if (s.startIdx < 0) { s.startIdx = 0; }
        if (s.startIdx >= a_count) { s.startIdx = a_count - 1; }
        if (s.endIdx < 0) { s.endIdx = 0; }
        if (s.endIdx >= a_count) { s.endIdx = a_count - 1; }
        return s;
    }
}
