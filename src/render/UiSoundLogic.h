#pragma once

// Pure mapping for BardHero's custom panels. Runtime plays Skyrim's own
// descriptors by editor ID; tests lock the semantic event -> vanilla sound
// contract without needing the game.
namespace SH::ui_sound {
    enum class Event { kFocus, kConfirm, kCancel, kStarPing, kLevelUp };

    [[nodiscard]] constexpr const char* EditorId(Event a_event) {
        switch (a_event) {
            case Event::kFocus: return "UIMenuFocus";
            case Event::kConfirm: return "UIMenuOK";
            case Event::kCancel: return "UIMenuCancel";
            // P5 star-pip stinger. Placeholder vanilla tick until P6's
            // original synthesized SFX set replaces it.
            case Event::kStarPing: return "UIMenuFocus";
            // Song clear on a VANILLA instrument - the electric guitar has
            // its own synthesized sting (ui_sfx::StingForEnding), so these
            // never stack. `UILevelUp` is Skyrim.esm's SOUN record, the same
            // record TYPE as UIMenuOK/UIMenuFocus above, which is what makes
            // it reachable through BuildSoundDataFromEditorID. Checked
            // against the ESM rather than assumed, because the descriptor
            // sitting beside it (`UILevelUpSD`, an SNDR) is the tempting
            // wrong answer and would silently play nothing at all.
            case Event::kLevelUp: return "UILevelUp";
        }
        return "UIMenuFocus";
    }
}
