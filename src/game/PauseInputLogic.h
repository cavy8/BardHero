#pragma once

#include <cstdint>

// PURE keyboard policy for BardHero's custom pause state. The native game
// must not receive arbitrary keys while our menu owns the session, because
// any remapped key could open Map/Journal/inventory behind it.
namespace SH::pause_input {
    enum class Action {
        kPass,
        kSwallow,
        kMoveUp,
        kMoveDown,
        kConfirm,
        kResume,
        kPause,
    };

    struct Keys {
        std::uint32_t console     = 0;
        std::uint32_t printScreen = 0;
        std::uint32_t moveUp      = 0;
        std::uint32_t moveDown    = 0;
        std::uint32_t confirm1    = 0;
        std::uint32_t confirm2    = 0;
        std::uint32_t enter       = 0;
        std::uint32_t numEnter    = 0;
        std::uint32_t pause1      = 0;
        std::uint32_t pause2      = 0;
    };

    [[nodiscard]] constexpr Action ForKeyboard(
        std::uint32_t dik, const Keys& keys) {
        if (dik == keys.printScreen) {
            return Action::kPass;
        }
        if (dik == keys.moveUp) { return Action::kMoveUp; }
        if (dik == keys.moveDown) { return Action::kMoveDown; }
        // The primary pause key is a toggle: Escape resumes directly and
        // can never accidentally confirm "Quit" after moving selection.
        if (keys.pause1 != 0 && dik == keys.pause1) {
            return Action::kResume;
        }
        if (dik == keys.confirm1 || dik == keys.confirm2 ||
            dik == keys.enter || dik == keys.numEnter ||
            (keys.pause2 != 0 && dik == keys.pause2)) {
            return Action::kConfirm;
        }
        return Action::kSwallow;
    }
}
