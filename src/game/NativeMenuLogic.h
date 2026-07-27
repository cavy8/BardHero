#pragma once

// PURE tracker for native menus that can temporarily take ownership away
// from BardHero. Passive/cursor layers do not accept a vanilla close hotkey;
// every other menu does and therefore activates recovery pass-through.

#include <string>
#include <string_view>
#include <unordered_set>

namespace SH::native_menu {
    [[nodiscard]] inline bool IsPassive(std::string_view name) {
        return name == "Cursor Menu" || name == "HUD Menu" ||
               name == "TrueHUD" || name == "Fader Menu" ||
               name == "Mist Menu";
    }

    class Tracker {
    public:
        void Observe(std::string_view name, bool opening) {
            if (IsPassive(name)) return;
            if (opening) {
                _open.emplace(name);
            } else {
                _open.erase(std::string(name));
            }
        }
        void Clear() { _open.clear(); }
        [[nodiscard]] bool Open() const { return !_open.empty(); }

    private:
        std::unordered_set<std::string> _open;
    };
}
