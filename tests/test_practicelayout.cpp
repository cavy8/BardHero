// tests/test_practicelayout.cpp
#include "render/PracticeLayout.h"

#include "harness.h"

using namespace SH;
using practice_pick::Focus;
using practice_pick::State;

static void SplitColumnsTests() {
    // Two equal columns and one gap: 616 = 300 + 16 + 300.
    const auto c = practice_layout::SplitColumns(616.0f);
    CHECK(c.width == 300.0f);
    CHECK(c.leftX == 0.0f);
    CHECK(c.rightX == 316.0f);

    // A host window narrower than the gap must not produce a NEGATIVE width.
    // ImGui reads a negative child size as "fill the available region", which
    // would stack the two lists on top of each other rather than shrinking
    // them - a silent visual corruption with no error anywhere.
    CHECK(practice_layout::SplitColumns(8.0f).width == 0.0f);
    CHECK(practice_layout::SplitColumns(0.0f).width == 0.0f);
    CHECK(practice_layout::SplitColumns(-100.0f).width == 0.0f);
}

static void ListHeightTests() {
    // 400 content - 56 header - 132 footer = 212. The footer reserve covers
    // the range line, the speed presets and the Loop/Play row together.
    CHECK(practice_layout::ListHeight(400.0f) == 212.0f);
    // Derived, not hardcoded, so this keeps holding if a row is added.
    CHECK(practice_layout::ListHeight(400.0f) ==
          400.0f - practice_layout::kHeaderReserve -
              practice_layout::kFooterReserve);

    // Squeezed to nothing, the list floors at one row instead of collapsing:
    // an empty section list still has to draw its "Whole song" row.
    CHECK(practice_layout::ListHeight(100.0f) ==
          practice_layout::kRowHeight);
    CHECK(practice_layout::ListHeight(0.0f) == practice_layout::kRowHeight);

    // With no chrome and a bare row height this is the old plain division.
    CHECK(practice_layout::VisibleRows(300.0f, 0.0f,
                                       practice_layout::kRowHeight) == 11);
    CHECK(practice_layout::VisibleRows(26.0f, 0.0f,
                                       practice_layout::kRowHeight) == 1);
    // Never zero: a zero-row window makes FirstVisibleRow's arithmetic
    // meaningless and shows an empty panel.
    CHECK(practice_layout::VisibleRows(0.0f, 0.0f,
                                       practice_layout::kRowHeight) == 1);
    CHECK(practice_layout::VisibleRows(-5.0f, 0.0f,
                                       practice_layout::kRowHeight) == 1);

    // ---- THE CLIPPED-ROW BUG (field 2026-07-26) ----------------------
    // The rows do not get the whole list height. With a title line and the
    // child's padding taken out, and item spacing added to the row pitch,
    // FEWER rows fit - and the count must drop, or the last one is drawn
    // half outside the child.
    {
        const float listH  = 300.0f;
        const float chrome = 22.0f + 8.0f * 2.0f;   // title + padding x2
        const float pitch  = practice_layout::kRowHeight + 4.0f;
        const int   fitted = practice_layout::VisibleRows(listH, chrome,
                                                          pitch);
        CHECK(fitted == 8);                          // (300-38)/30
        // Strictly fewer than the naive answer that caused the bug.
        CHECK(fitted < practice_layout::VisibleRows(
                           listH, 0.0f, practice_layout::kRowHeight));
        // And what it reports must actually FIT, which is the whole point.
        CHECK(chrome + static_cast<float>(fitted) * pitch <= listH);
    }
    // Chrome taller than the list still floors at one row rather than
    // returning zero or a negative count.
    CHECK(practice_layout::VisibleRows(40.0f, 90.0f, 30.0f) == 1);
    // A nonsense pitch must not divide by zero.
    CHECK(practice_layout::VisibleRows(300.0f, 0.0f, 0.0f) == 11);
    CHECK(practice_layout::VisibleRows(300.0f, 0.0f, -3.0f) == 11);
}

static void FirstVisibleRowTests() {
    // This is the hazard-10 replacement for FUCK::SetScrollHereY, which is
    // version-2 gated and silently no-ops on a v1 host. Every case below is
    // one the host would have handled invisibly.
    const int vis = 5;

    // Already visible: the view does NOT move.
    CHECK(practice_layout::FirstVisibleRow(2, vis, 20, 0) == 0);
    CHECK(practice_layout::FirstVisibleRow(4, vis, 20, 0) == 0);

    // Walking off the bottom scrolls by exactly one row at a time.
    CHECK(practice_layout::FirstVisibleRow(5, vis, 20, 0) == 1);
    CHECK(practice_layout::FirstVisibleRow(6, vis, 20, 1) == 2);

    // Walking off the top.
    CHECK(practice_layout::FirstVisibleRow(3, vis, 20, 6) == 3);
    CHECK(practice_layout::FirstVisibleRow(0, vis, 20, 6) == 0);

    // Wrapping from row 0 to the last row (the strum-up case) must land on
    // the LAST window, not scroll one row at a time through the whole list.
    CHECK(practice_layout::FirstVisibleRow(19, vis, 20, 0) == 15);

    // The top can never exceed the last full window...
    CHECK(practice_layout::FirstVisibleRow(19, vis, 20, 99) == 15);
    // ...and a list shorter than the window always starts at 0.
    CHECK(practice_layout::FirstVisibleRow(1, vis, 3, 0) == 0);
    CHECK(practice_layout::FirstVisibleRow(2, vis, 3, 2) == 0);

    // Degenerate inputs stay in range rather than indexing off the vector.
    CHECK(practice_layout::FirstVisibleRow(0, vis, 0, 0) == 0);
    CHECK(practice_layout::FirstVisibleRow(-1, vis, 20, 4) == 4);
    CHECK(practice_layout::FirstVisibleRow(3, 0, 20, 0) == 0);
}

static void StepFocusTests() {
    State s;
    CHECK(s.focus == Focus::kStart);

    // navDiff walks focus across all FOUR stops - the two lists, then
    // Speed, then Loop (2026-07-27: both were mouse-only, which on a GH
    // controller meant unreachable) - and CLAMPS at both ends: focus
    // wrapping around would read as the selection teleporting across the
    // panel with no input to explain it.
    s = practice_pick::Step(s, 0, +1, 8);
    CHECK(s.focus == Focus::kEnd);
    s = practice_pick::Step(s, 0, +1, 8);
    CHECK(s.focus == Focus::kSpeed);
    s = practice_pick::Step(s, 0, +1, 8);
    CHECK(s.focus == Focus::kLoop);
    s = practice_pick::Step(s, 0, +1, 8);
    CHECK(s.focus == Focus::kLoop);
    s = practice_pick::Step(s, 0, -1, 8);
    CHECK(s.focus == Focus::kSpeed);
    s = practice_pick::Step(s, 0, -1, 8);
    CHECK(s.focus == Focus::kEnd);
    s = practice_pick::Step(s, 0, -1, 8);
    CHECK(s.focus == Focus::kStart);
    s = practice_pick::Step(s, 0, -1, 8);
    CHECK(s.focus == Focus::kStart);
}

static void StepMoveTests() {
    // Movement applies to the FOCUSED list only.
    State s;
    s = practice_pick::Step(s, +2, 0, 8);
    CHECK(s.startIdx == 2);
    CHECK(s.endIdx == 0);

    s = practice_pick::Step(s, 0, +1, 8);  // focus the end list
    s = practice_pick::Step(s, +3, 0, 8);
    CHECK(s.startIdx == 2);  // untouched
    CHECK(s.endIdx == 3);

    // Within-list movement WRAPS, matching the Songbook and pause menu.
    s = practice_pick::Step(s, +5, 0, 8);
    CHECK(s.endIdx == 0);
    s = practice_pick::Step(s, -1, 0, 8);
    CHECK(s.endIdx == 7);

    // An end BEFORE the start is deliberately allowed. ResolveRange swaps
    // them, and the picker displays what ResolveRange returns rather than
    // re-deriving the rule - which is also how it inherits the lead-in
    // clamp and last-section-to-song-end behaviour for free.
    State t{ Focus::kEnd, 6, 6 };
    t = practice_pick::Step(t, -4, 0, 8);
    CHECK(t.startIdx == 6);
    CHECK(t.endIdx == 2);

    // On the Speed and Loop stops the strum must leave BOTH lists alone -
    // the window routes it to the tempo/loop instead. A strum that also
    // moved a hidden list would silently change the practice range.
    State u{ Focus::kSpeed, 3, 5 };
    u = practice_pick::Step(u, +2, 0, 8);
    CHECK(u.startIdx == 3);
    CHECK(u.endIdx == 5);
    State v{ Focus::kLoop, 3, 5 };
    v = practice_pick::Step(v, -2, 0, 8);
    CHECK(v.startIdx == 3);
    CHECK(v.endIdx == 5);
}

static void SpeedPresetTests() {
    using practice_pick::StepSpeedPreset;
    using practice_pick::kSpeedPresets;
    using practice_pick::kSpeedPresetCount;

    // The table the buttons and the strum both draw from: ascending, ends
    // pinned at the two speeds that define the feature.
    CHECK(kSpeedPresetCount == 6);
    CHECK(kSpeedPresets[0].pct == 50);
    CHECK(kSpeedPresets[kSpeedPresetCount - 1].pct == 100);
    for (int i = 1; i < kSpeedPresetCount; ++i) {
        CHECK(kSpeedPresets[i].pct > kSpeedPresets[i - 1].pct);
    }

    // Up-strum (negative move) raises the tempo; down-strum lowers it.
    CHECK(StepSpeedPreset(0.9, -1) == 1.0);
    CHECK(StepSpeedPreset(0.9, +1) == 0.8);
    // CLAMPS at both ends, same rationale as difficulty stepping: holding
    // past Full Speed sits at Full Speed.
    CHECK(StepSpeedPreset(1.0, -1) == 1.0);
    CHECK(StepSpeedPreset(0.5, +1) == 0.5);
    // A mid-run -/= value that is not a preset steps DIRECTIONALLY to the
    // adjacent preset, so the row behaves the way it looks: 85 sits
    // between Medium and Fast, and one strum either way lands on exactly
    // its neighbour. (The first cut snapped to nearest before stepping and
    // double-stepped this case: 85 -> 80 -> 70.)
    CHECK(StepSpeedPreset(0.85, -1) == 0.9);
    CHECK(StepSpeedPreset(0.85, +1) == 0.8);
    CHECK(StepSpeedPreset(0.55, -1) == 0.6);
    CHECK(StepSpeedPreset(0.55, +1) == 0.5);
    // Values outside the table step back INTO it rather than escaping it.
    CHECK(StepSpeedPreset(0.30, -1) == 0.5);
    CHECK(StepSpeedPreset(0.30, +1) == 0.5);
    CHECK(StepSpeedPreset(1.40, +1) == 1.0);
    CHECK(StepSpeedPreset(1.40, -1) == 1.0);
    // A zero move is a no-op, not a snap.
    CHECK(StepSpeedPreset(0.85, 0) == 0.85);
}

static void StepEmptyAndStaleTests() {
    // No section markers is the COMMON case - most charts carry none. The
    // picker offers whole song as its only row and must never let a stray
    // strum select a row that is not there.
    State s{ Focus::kEnd, 5, 9 };
    s = practice_pick::Step(s, +3, +1, 0);
    CHECK(s.startIdx == 0);
    CHECK(s.endIdx == 0);
    // Focus is NOT reset with the indices: a markerless chart still has a
    // tempo and a loop to set, and the old per-frame reset to kStart made
    // both permanently unreachable on a controller.
    CHECK(s.focus == Focus::kSpeed);
    // ...and from there the remaining stops still answer.
    s = practice_pick::Step(s, 0, +1, 0);
    CHECK(s.focus == Focus::kLoop);

    // A shorter chart must not leave a stale out-of-range index behind when
    // the section list is republished for a different song. BOTH indices are
    // clamped, not only the focused one.
    State stale{ Focus::kStart, 40, 37 };
    stale = practice_pick::Step(stale, 0, 0, 4);
    CHECK(stale.startIdx == 3);
    CHECK(stale.endIdx == 3);

    // Negative indices cannot survive either.
    State neg{ Focus::kStart, -5, -2 };
    neg = practice_pick::Step(neg, 0, 0, 4);
    CHECK(neg.startIdx == 0);
    CHECK(neg.endIdx == 0);
}

void RunTests() {
    SplitColumnsTests();
    ListHeightTests();
    FirstVisibleRowTests();
    StepFocusTests();
    StepMoveTests();
    StepEmptyAndStaleTests();
    SpeedPresetTests();
}

TEST_MAIN("PracticeLayout")
