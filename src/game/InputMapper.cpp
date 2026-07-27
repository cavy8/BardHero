// src/game/InputMapper.cpp
#include "game/InputMapper.h"

#include <algorithm>

namespace SH {
    namespace {
        // bind slots: 0..4 frets, 5 SP, 6 strum, 7 whammy, 8 pause, -1 none
        // (primary and secondary binds land in the SAME slot - the engine
        // never knows which physical key produced an action)
        int SlotOf(const Binds& b, std::uint32_t dik) {
            if (dik == 0) return -1;
            const int d = static_cast<int>(dik);
            for (int i = 0; i < 5; ++i) {
                if (b.fret[i] == d || b.fret2[i] == d) return i;
            }
            if (b.sp == d || b.sp2 == d) return 5;
            if (b.strum == d || b.strum2 == d || b.strum3 == d) return 6;
            if (b.whammy == d || b.whammy2 == d) return 7;
            if (b.pause == d || b.pause2 == d) return 8;
            return -1;
        }

        int SlotOf(const GamepadBinds& b, std::uint32_t code) {
            if (code == 0) return -1;
            const int c = static_cast<int>(code);
            for (int i = 0; i < 5; ++i) {
                if (b.fret[i] == c) return i;
            }
            if (b.sp == c) return 5;
            if (b.strum[0] == c || b.strum[1] == c) return 6;
            if (b.whammy == c) return 7;
            if (b.pause == c) return 8;
            return -1;
        }

        bard::InputAction FretAction(int slot) {
            return static_cast<bard::InputAction>(
                static_cast<int>(bard::InputAction::kFret1) + slot);
        }

        bool HasStarPowerValue(const std::vector<MappedEvent>& a_events,
                               bool a_down) {
            return std::any_of(
                a_events.begin(), a_events.end(),
                [a_down](const MappedEvent& a_event) {
                    return a_event.action ==
                               bard::InputAction::kStarPower &&
                           a_event.value == (a_down ? 1 : 0);
                });
        }

        // ofs must be a DIK code, data a pure press/release bit, the stamp
        // within (-1s, +10min) of now. Never-written tail slots hold heap
        // garbage (M0 field finding: poisoned the run-1 high-water mark and
        // muted everything) - this filter is a REQUIRED design element. The
        // 10-minute bound (up from the spike's 60s) keeps long-alt-tab
        // releases, which are REAL events (M0 saw one 23.5s late); random
        // garbage stamps land in-window with P ~ 0.014% and would also need
        // plausible ofs+data plus a sequence above the mark.
        bool Plausible(const DiEvent& e, std::uint32_t tgtNow) {
            if (e.ofs >= 0x100) return false;
            if ((e.data & ~0x80u) != 0) return false;
            const auto age =
                static_cast<std::int32_t>(tgtNow - e.timeStamp);
            return age > -1000 && age < 600000;
        }
    }

    bool Binds::IsBound(std::uint32_t dik) const {
        return SlotOf(*this, dik) >= 0;
    }

    bool GamepadBinds::IsBound(std::uint32_t code) const {
        return SlotOf(*this, code) >= 0;
    }

    void KeyboardSpFallback::Reset() {
        *this = KeyboardSpFallback{};
    }

    void KeyboardSpFallback::FeedButton(
        std::uint32_t code, bool down, double qpcNowSec,
        const Binds& binds, bool engaged, std::vector<MappedEvent>& out) {
        if (binds.sp == 0 ||
            code != static_cast<std::uint32_t>(binds.sp)) {
            return;
        }
        _heldRaw = down;
        if (!engaged) { return; }
        if (!HasStarPowerValue(out, down)) {
            out.push_back({ qpcNowSec, bard::InputAction::kStarPower,
                            down ? 1 : 0, code });
        }
        _heldEngine = down;
    }

    void KeyboardSpFallback::EmitEngageDiff(
        double qpcNowSec, const Binds& binds,
        std::vector<MappedEvent>& out) {
        if (_heldRaw == _heldEngine) { return; }
        if (!HasStarPowerValue(out, _heldRaw)) {
            out.push_back(
                { qpcNowSec, bard::InputAction::kStarPower,
                  _heldRaw ? 1 : 0,
                  static_cast<std::uint32_t>(binds.sp) });
        }
        _heldEngine = _heldRaw;
    }

    void GamepadMapper::Reset() { *this = GamepadMapper{}; }

    void GamepadMapper::FeedButton(
        std::uint32_t code, bool down, double qpcNowSec,
        const GamepadBinds& binds, bool engaged, bool gamepadMode,
        std::vector<MappedEvent>& out) {
        const int slot = SlotOf(binds, code);
        if (slot < 0) return;

        if (slot < 5) {
            const auto bit = static_cast<std::uint8_t>(1u << slot);
            _heldRaw = down ? static_cast<std::uint8_t>(_heldRaw | bit)
                            : static_cast<std::uint8_t>(_heldRaw & ~bit);
        } else if (slot == 5) {
            _heldRaw = down ? static_cast<std::uint8_t>(_heldRaw | 0x20)
                            : static_cast<std::uint8_t>(_heldRaw & ~0x20);
        } else if (slot == 7) {
            _whammyHeldRaw = down;
            _whammyEdge = true;
        }
        if (!engaged) return;

        if (slot < 5) {
            out.push_back(
                { qpcNowSec, FretAction(slot), down ? 1 : 0, code });
            const auto bit = static_cast<std::uint8_t>(1u << slot);
            _heldEngine =
                down ? static_cast<std::uint8_t>(_heldEngine | bit)
                     : static_cast<std::uint8_t>(_heldEngine & ~bit);
            if (down && gamepadMode) { _autoStrum = true; }
        } else if (slot == 5) {
            out.push_back({ qpcNowSec, bard::InputAction::kStarPower,
                            down ? 1 : 0, code });
            _heldEngine =
                down ? static_cast<std::uint8_t>(_heldEngine | 0x20)
                     : static_cast<std::uint8_t>(_heldEngine & ~0x20);
        } else if (slot == 6) {
            if (down) {
                out.push_back(
                    { qpcNowSec, bard::InputAction::kStrum, 1, code });
            }
        } else if (slot == 7) {
            out.push_back({ qpcNowSec, bard::InputAction::kWhammy,
                            down ? 1 : 0, code });
        } else if (slot == 8 && down) {
            out.push_back(
                { qpcNowSec, bard::InputAction::kPause, 1, code });
        }
    }

    void GamepadMapper::EndFrame(
        double qpcNowSec, const GamepadBinds& binds, bool engaged,
        bool gamepadMode, std::vector<MappedEvent>& out) {
        if (engaged && gamepadMode && _autoStrum) {
            out.push_back({ qpcNowSec, bard::InputAction::kStrum, 1,
                            static_cast<std::uint32_t>(binds.fret[0]) });
        }
        _autoStrum = false;
        if (engaged && _whammyHeldRaw && !_whammyEdge) {
            out.push_back({ qpcNowSec, bard::InputAction::kWhammy, 1,
                            static_cast<std::uint32_t>(binds.whammy) });
        }
        _whammyEdge = false;
    }

    void GamepadMapper::EmitEngageDiff(
        double qpcNowSec, const GamepadBinds& binds,
        std::vector<MappedEvent>& out) {
        const std::uint8_t diff = _heldRaw ^ _heldEngine;
        for (int i = 0; i < 5; ++i) {
            const auto bit = static_cast<std::uint8_t>(1u << i);
            if (diff & bit) {
                out.push_back({ qpcNowSec, FretAction(i),
                                (_heldRaw & bit) ? 1 : 0,
                                static_cast<std::uint32_t>(binds.fret[i]) });
            }
        }
        if (diff & 0x20) {
            out.push_back({ qpcNowSec, bard::InputAction::kStarPower,
                            (_heldRaw & 0x20) ? 1 : 0,
                            static_cast<std::uint32_t>(binds.sp) });
        }
        _heldEngine = _heldRaw;
    }

    void InputMapper::Reset() { *this = InputMapper{}; }

    InputMapper::FeedStats InputMapper::Feed(const DiEvent* buf, int len,
                                             std::uint32_t tgtNow,
                                             double qpcNowSec,
                                             const Binds& binds, bool engaged,
                                             std::vector<MappedEvent>& out) {
        FeedStats st;
        if (!_seeded) {
            // whatever already sits in the buffer predates this session and
            // must never replay (M0 seeding rule)
            for (int i = 0; i < len; ++i) {
                if (Plausible(buf[i], tgtNow)) {
                    _lastSeq = std::max(_lastSeq, buf[i].sequence);
                }
            }
            _seeded = true;
            return st;
        }

        constexpr int  kMax = 16;  // production len is 10
        const DiEvent* fresh[kMax];
        for (int i = 0; i < len && st.fresh < kMax; ++i) {
            if (Plausible(buf[i], tgtNow) && buf[i].sequence > _lastSeq) {
                fresh[st.fresh++] = &buf[i];
            }
        }
        st.bufferFull = (st.fresh >= len);
        std::sort(fresh, fresh + st.fresh,
                  [](const DiEvent* a, const DiEvent* b) {
                      return a->sequence < b->sequence;
                  });

        bool whammyEdge = false;
        for (int i = 0; i < st.fresh; ++i) {
            const auto& e = *fresh[i];
            _lastSeq      = e.sequence;
            const auto ageMs =
                static_cast<std::int32_t>(tgtNow - e.timeStamp);
            if (ageMs > kStaleMs || ageMs < -kStaleMs) {
                ++st.stale;  // alt-tab backlog: still mapped below - the
                             // engine's monotonic queue clamp applies it now
            }
            const int slot = SlotOf(binds, e.ofs);
            if (slot < 0) continue;  // unbound key
            const bool   down = (e.data & 0x80u) != 0;
            const double qpc =
                qpcNowSec - static_cast<double>(ageMs) / 1000.0;

            // raw physical state tracks on EVERY call - the engage diff
            // reconciles whatever happened while disengaged
            if (slot < 5) {
                const auto bit = static_cast<std::uint8_t>(1u << slot);
                _heldRaw = down ? (_heldRaw | bit)
                                : static_cast<std::uint8_t>(_heldRaw & ~bit);
            } else if (slot == 5) {
                _heldRaw = down ? (_heldRaw | 0x20)
                                : static_cast<std::uint8_t>(_heldRaw & ~0x20);
            } else if (slot == 7) {
                _whammyHeldRaw = down;
            }
            if (!engaged) continue;

            switch (slot) {
                case 0:
                case 1:
                case 2:
                case 3:
                case 4: {
                    out.push_back({ qpc, FretAction(slot), down ? 1 : 0,
                                    e.ofs });
                    const auto bit = static_cast<std::uint8_t>(1u << slot);
                    _heldEngine =
                        down ? (_heldEngine | bit)
                             : static_cast<std::uint8_t>(_heldEngine & ~bit);
                    ++st.mapped;
                    break;
                }
                case 5:
                    out.push_back({ qpc, bard::InputAction::kStarPower,
                                    down ? 1 : 0, e.ofs });
                    _heldEngine =
                        down ? (_heldEngine | 0x20)
                             : static_cast<std::uint8_t>(_heldEngine & ~0x20);
                    ++st.mapped;
                    break;
                case 6:
                    // CH/YARG keyboard strum: PRESS only. The spec-7 "both
                    // edges" reading emulated the physical bar on one key;
                    // field run 2026-07-18: every tap's release fired a
                    // second strum into empty air (36 overstrums vs 31
                    // hits) and pinned the combo at 0.
                    if (down) {
                        out.push_back(
                            { qpc, bard::InputAction::kStrum, 1, e.ofs });
                        ++st.mapped;
                    }
                    break;
                case 7:
                    out.push_back({ qpc, bard::InputAction::kWhammy,
                                    down ? 1 : 0, e.ofs });
                    whammyEdge = true;
                    ++st.mapped;
                    break;
                case 8:
                    if (down) {
                        out.push_back(
                            { qpc, bard::InputAction::kPause, 1, e.ofs });
                        ++st.mapped;
                    }
                    break;
                default:
                    break;
            }
        }

        // DI delivers edges only; the engine's 250ms whammy window
        // (spec 5.6) needs a live signal while the key stays held
        if (engaged && _whammyHeldRaw && !whammyEdge) {
            out.push_back({ qpcNowSec, bard::InputAction::kWhammy, 1,
                            static_cast<std::uint32_t>(binds.whammy) });
            ++st.mapped;
        }
        return st;
    }

    void InputMapper::EmitEngageDiff(double qpcNowSec, const Binds& binds,
                                     std::vector<MappedEvent>& out) {
        const std::uint8_t diff = _heldRaw ^ _heldEngine;
        for (int i = 0; i < 5; ++i) {
            const auto bit = static_cast<std::uint8_t>(1u << i);
            if (diff & bit) {
                out.push_back({ qpcNowSec, FretAction(i),
                                (_heldRaw & bit) ? 1 : 0,
                                static_cast<std::uint32_t>(binds.fret[i]) });
            }
        }
        if (diff & 0x20) {
            out.push_back({ qpcNowSec, bard::InputAction::kStarPower,
                            (_heldRaw & 0x20) ? 1 : 0,
                            static_cast<std::uint32_t>(binds.sp) });
        }
        _heldEngine = _heldRaw;
    }
}
