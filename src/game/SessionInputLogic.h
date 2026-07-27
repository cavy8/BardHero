#pragma once

// PURE keyboard ownership for every active-session phase. InputHook feeds the
// rhythm engine from DirectInput before Skyrim sees its InputEvent list, so
// swallowing here prevents native actions without losing note input.
//
// Recovery is intentionally stronger than ownership: if a vanilla menu
// somehow opened despite the filter, every key passes until that menu closes.
// The player must always be able to use its native close shortcut.

#include "game/PauseInputLogic.h"

#include <cstdint>

namespace SH::session_input {
    enum class Phase {
        kNone,
        kPlaying,
        kPaused,
        kResumeCountdown,
    };

    enum class Overlay {
        kNone,
        kBrowser,
        kResults,
    };

    struct GamepadKeys {
        std::uint32_t moveUp   = 0;
        std::uint32_t moveDown = 0;
        std::uint32_t confirm  = 0;
        std::uint32_t cancel   = 0;
        std::uint32_t pause    = 0;
    };

    [[nodiscard]] constexpr pause_input::Action ForKeyboard(
        Phase phase, bool nativeMenuOpen, std::uint32_t dik,
        const pause_input::Keys& keys) {
        if (phase == Phase::kNone || nativeMenuOpen) {
            return pause_input::Action::kPass;
        }
        if (dik == keys.printScreen) {
            return pause_input::Action::kPass;
        }
        if (phase == Phase::kPaused) {
            return pause_input::ForKeyboard(dik, keys);
        }
        if (phase == Phase::kResumeCountdown &&
            ((keys.pause1 != 0 && dik == keys.pause1) ||
             (keys.pause2 != 0 && dik == keys.pause2))) {
            return pause_input::Action::kPause;
        }
        // Playing and resume-countdown phases own the whole keyboard. The
        // raw DI buffer has already fed mapped rhythm actions; Skyrim must
        // receive none of the gameplay/menu hotkeys.
        return pause_input::Action::kSwallow;
    }

    // Songbook and Results exist outside EngineFeed::active, but they are
    // still owned minigame surfaces. Keep their narrow existing navigation
    // capture while making console suppression explicit and headless-testable.
    [[nodiscard]] constexpr pause_input::Action ForOverlayKeyboard(
        Overlay overlay, std::uint32_t dik, const pause_input::Keys& keys,
        bool isBound) {
        if (overlay == Overlay::kNone) {
            return pause_input::Action::kPass;
        }
        if (dik == keys.printScreen) {
            return pause_input::Action::kPass;
        }
        if (dik == keys.console) {
            return pause_input::Action::kSwallow;
        }
        if (overlay == Overlay::kBrowser) {
            // NAMED move actions for the strum keys, not a blanket swallow:
            // the name carries the direction to PublishNavHold, which is
            // what makes HOLDING the strum scroll (a GH controller through
            // GlovePIE is the keyboard). Swallow behaviour is unchanged.
            if (dik == keys.moveUp) { return pause_input::Action::kMoveUp; }
            if (dik == keys.moveDown) {
                return pause_input::Action::kMoveDown;
            }
            return isBound || dik == keys.enter || dik == keys.numEnter
                     ? pause_input::Action::kSwallow
                     : pause_input::Action::kPass;
        }
        return dik == keys.confirm1 || dik == keys.confirm2 ||
                       dik == keys.enter || dik == keys.numEnter
                 ? pause_input::Action::kConfirm
                 : pause_input::Action::kPass;
    }

    [[nodiscard]] constexpr pause_input::Action ForGamepad(
        Phase phase, bool nativeMenuOpen, std::uint32_t code,
        const GamepadKeys& keys) {
        if (phase == Phase::kNone || nativeMenuOpen) {
            return pause_input::Action::kPass;
        }
        if (phase == Phase::kPaused) {
            if (code == keys.moveUp) {
                return pause_input::Action::kMoveUp;
            }
            if (code == keys.moveDown) {
                return pause_input::Action::kMoveDown;
            }
            if (code == keys.confirm) {
                return pause_input::Action::kConfirm;
            }
            if (code == keys.cancel || code == keys.pause) {
                return pause_input::Action::kResume;
            }
            return pause_input::Action::kSwallow;
        }
        if (phase == Phase::kResumeCountdown && code == keys.pause) {
            return pause_input::Action::kPause;
        }
        return pause_input::Action::kSwallow;
    }

    [[nodiscard]] constexpr pause_input::Action ForOverlayGamepad(
        Overlay overlay, std::uint32_t code, const GamepadKeys& keys) {
        if (overlay == Overlay::kNone) {
            return pause_input::Action::kPass;
        }
        if (overlay == Overlay::kBrowser) {
            if (code == keys.moveUp) {
                return pause_input::Action::kMoveUp;
            }
            if (code == keys.moveDown) {
                return pause_input::Action::kMoveDown;
            }
            if (code == keys.confirm) {
                return pause_input::Action::kConfirm;
            }
            if (code == keys.cancel || code == keys.pause) {
                return pause_input::Action::kResume;
            }
            return pause_input::Action::kSwallow;
        }
        return code == keys.confirm || code == keys.cancel ||
                       code == keys.pause
                 ? pause_input::Action::kConfirm
                 : pause_input::Action::kSwallow;
    }
}
