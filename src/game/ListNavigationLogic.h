#pragma once

#include <algorithm>

namespace SH::list_navigation {
    // Move within a circular list. An unselected list behaves as though its
    // cursor sits just before the first row for Down, or just after the last
    // row for Up, so the first input is symmetric.
    [[nodiscard]] constexpr int WrapIndex(int a_current, int a_delta,
                                          int a_count) {
        if (a_count <= 0) { return -1; }
        if (a_delta == 0) {
            return a_current >= 0 && a_current < a_count ? a_current : -1;
        }
        const int base =
            a_current >= 0 && a_current < a_count
                ? a_current
                : (a_delta > 0 ? -1 : 0);
        const int moved = (base + a_delta) % a_count;
        return moved < 0 ? moved + a_count : moved;
    }

    // Songbook difficulty stepping (GH convention 2026-07-26: difficulty
    // is picked at song select). CLAMPS rather than wraps - holding the
    // step key past Expert must sit at Expert, not drop the player back to
    // Easy without noticing. Deliberately does NOT skip difficulties a
    // given chart lacks: the selection is global to the list, and each row
    // reports its own fallback in the Diff column instead.
    inline constexpr int kDifficultyCount = 4;

    [[nodiscard]] constexpr int StepDifficulty(int a_current,
                                               int a_delta) noexcept {
        const int base = a_current < 0
                             ? 0
                             : (a_current > kDifficultyCount - 1
                                    ? kDifficultyCount - 1
                                    : a_current);
        const int stepped = base + a_delta;
        return stepped < 0 ? 0
                           : (stepped > kDifficultyCount - 1
                                  ? kDifficultyCount - 1
                                  : stepped);
    }

    struct RepeatParams {
        double initialDelaySec = 0.350;
        double intervalSec     = 0.080;
    };

    // The input hook owns physical held-state and supplies press edges.
    // Render surfaces own one repeater each, keeping frame timing out of the
    // game-thread hook and ensuring a newly opened list starts clean.
    class HeldRepeat {
    public:
        [[nodiscard]] int Step(int a_edgeDelta, int a_heldDirection,
                               double a_now,
                               const RepeatParams& a_params = {}) {
            const int held = (a_heldDirection > 0) -
                             (a_heldDirection < 0);
            const double delay = std::max(0.0, a_params.initialDelaySec);
            const double interval = std::max(0.001, a_params.intervalSec);

            if (a_edgeDelta != 0) {
                _direction = held;
                _nextAt = held == 0 ? 0.0 : a_now + delay;
                return a_edgeDelta;
            }
            if (held != _direction) {
                _direction = held;
                _nextAt = held == 0 ? 0.0 : a_now + delay;
                return 0;
            }
            if (held == 0 || a_now + 1.0e-9 < _nextAt) { return 0; }

            // At most one repeat per render frame. A frame hitch should not
            // fling the selection through several unseen rows.
            _nextAt = a_now + interval;
            return held;
        }

        void Reset() {
            _direction = 0;
            _nextAt = 0.0;
        }

    private:
        int    _direction = 0;
        double _nextAt = 0.0;
    };
}
