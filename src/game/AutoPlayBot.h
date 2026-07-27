// src/game/AutoPlayBot.h
#pragma once

// PURE feeder-level autoplay bot (cheat, settings page 2026-07-20): when
// [Cheats] bAutoPlay is on, the input hook feeds the engine THESE synthetic
// inputs instead of the mapper's real ones. The engine stays untouched, so
// the determinism suites keep their meaning; this bot has its own suite
// (AutoPlayTests) proving 100% hits / 0 overstrums against the real engine.
//
// Per note at time t (all events stamped exactly t - the engine judges by
// event time, so frame quantization never costs accuracy):
//   releases of stale frets, then presses of the note's frets, then one
//   strum (queue order is preserved; strumming taps/HOPOs is always valid).
// Sustained lanes stay held to their sustain end; at a later note's tick an
// active sustain is extended BY DEFINITION, and the engine subtracts
// extended lanes from the fret test, so keeping them held is exact - this
// also makes open notes work mid-sustain (all other frets released).
// Times are InputTime-domain, equal to song time while audioCal == 0 (the
// standing M2/M5 caveat, same as the hook's real-event path).

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "chart/ChartTypes.h"
#include "engine/InputTypes.h"

namespace SH {
    class AutoPlayBot {
    public:
        void Reset() {
            _next   = 0;
            _held   = 0;
            _primed = false;
            _holdUntil.fill(0.0);
        }

        // Once per frame with monotonic a_now: appends perfect inputs for
        // every note that has come due. The first call fast-forwards past
        // already-elapsed notes (a mid-song toggle must not replay the
        // player's history as a burst of late hits).
        void Emit(const bard::ParsedChart& a_chart, double a_now,
                  std::vector<bard::NoteInput>& a_out) {
            const auto& notes = a_chart.notes;
            if (!_primed) {
                _primed = true;
                while (_next < notes.size() &&
                       notes[_next].time < a_now) {
                    ++_next;
                }
            }
            while (_next < notes.size() && notes[_next].time <= a_now) {
                const auto&        n    = notes[_next];
                const double       t    = n.time;
                const std::uint8_t want = n.mask & 0x1F;
                std::uint8_t       keep = 0;  // live sustains from earlier
                for (int l = 0; l < 5; ++l) {
                    if (_holdUntil[l] > t) {
                        keep |= static_cast<std::uint8_t>(1u << l);
                    }
                }
                const std::uint8_t target = want | keep;
                for (int l = 0; l < 5; ++l) {  // releases first
                    const auto bit = static_cast<std::uint8_t>(1u << l);
                    if ((_held & bit) && !(target & bit)) {
                        a_out.push_back({ t, FretAction(l), 0 });
                        _held &= static_cast<std::uint8_t>(~bit);
                    }
                }
                for (int l = 0; l < 5; ++l) {  // then presses
                    const auto bit = static_cast<std::uint8_t>(1u << l);
                    if (!(_held & bit) && (target & bit)) {
                        a_out.push_back({ t, FretAction(l), 1 });
                        _held |= bit;
                    }
                }
                a_out.push_back({ t, bard::InputAction::kStrum, 1 });
                for (int l = 0; l < 5; ++l) {
                    if (n.sustainEnd[l] > t) {
                        _holdUntil[l] =
                            std::max(_holdUntil[l], n.sustainEnd[l]);
                    }
                }
                ++_next;
            }
        }

        // fret bits 0-4 as the bot currently holds them (HUD fret glow)
        [[nodiscard]] std::uint8_t HeldMask() const { return _held; }

    private:
        static bard::InputAction FretAction(int a_lane) {
            return static_cast<bard::InputAction>(
                static_cast<int>(bard::InputAction::kFret1) + a_lane);
        }

        std::size_t           _next   = 0;
        std::uint8_t          _held   = 0;
        bool                  _primed = false;
        std::array<double, 5> _holdUntil{};
    };
}
