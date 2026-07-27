#pragma once

// Human names for the two binding code spaces the Bindings tab shows:
// DirectInput scan codes (keyboard column, what the DI hook reads) and
// SKSE's normalized gamepad codes 266..281 (what GamepadCode() yields).
// Pure - no RE, no OS - so suite 40 can pin every name.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace SH::key_names {

    // Returns "" for codes the table does not know (caller falls back to
    // DikLabel). Covers every key the DI keyboard device can report that a
    // player could plausibly bind.
    [[nodiscard]] inline constexpr std::string_view Dik(
        std::uint32_t a_code) {
        switch (a_code) {
            case 0x01: return "Escape";
            case 0x02: return "1";
            case 0x03: return "2";
            case 0x04: return "3";
            case 0x05: return "4";
            case 0x06: return "5";
            case 0x07: return "6";
            case 0x08: return "7";
            case 0x09: return "8";
            case 0x0A: return "9";
            case 0x0B: return "0";
            case 0x0C: return "-";
            case 0x0D: return "=";
            case 0x0E: return "Backspace";
            case 0x0F: return "Tab";
            case 0x10: return "Q";
            case 0x11: return "W";
            case 0x12: return "E";
            case 0x13: return "R";
            case 0x14: return "T";
            case 0x15: return "Y";
            case 0x16: return "U";
            case 0x17: return "I";
            case 0x18: return "O";
            case 0x19: return "P";
            case 0x1A: return "[";
            case 0x1B: return "]";
            case 0x1C: return "Enter";
            case 0x1D: return "Left Ctrl";
            case 0x1E: return "A";
            case 0x1F: return "S";
            case 0x20: return "D";
            case 0x21: return "F";
            case 0x22: return "G";
            case 0x23: return "H";
            case 0x24: return "J";
            case 0x25: return "K";
            case 0x26: return "L";
            case 0x27: return ";";
            case 0x28: return "'";
            case 0x29: return "`";
            case 0x2A: return "Left Shift";
            case 0x2B: return "\\";
            case 0x2C: return "Z";
            case 0x2D: return "X";
            case 0x2E: return "C";
            case 0x2F: return "V";
            case 0x30: return "B";
            case 0x31: return "N";
            case 0x32: return "M";
            case 0x33: return ",";
            case 0x34: return ".";
            case 0x35: return "/";
            case 0x36: return "Right Shift";
            case 0x37: return "Numpad *";
            case 0x38: return "Left Alt";
            case 0x39: return "Space";
            case 0x3A: return "Caps Lock";
            case 0x3B: return "F1";
            case 0x3C: return "F2";
            case 0x3D: return "F3";
            case 0x3E: return "F4";
            case 0x3F: return "F5";
            case 0x40: return "F6";
            case 0x41: return "F7";
            case 0x42: return "F8";
            case 0x43: return "F9";
            case 0x44: return "F10";
            case 0x45: return "Num Lock";
            case 0x46: return "Scroll Lock";
            case 0x47: return "Numpad 7";
            case 0x48: return "Numpad 8";
            case 0x49: return "Numpad 9";
            case 0x4A: return "Numpad -";
            case 0x4B: return "Numpad 4";
            case 0x4C: return "Numpad 5";
            case 0x4D: return "Numpad 6";
            case 0x4E: return "Numpad +";
            case 0x4F: return "Numpad 1";
            case 0x50: return "Numpad 2";
            case 0x51: return "Numpad 3";
            case 0x52: return "Numpad 0";
            case 0x53: return "Numpad .";
            case 0x57: return "F11";
            case 0x58: return "F12";
            case 0x9C: return "Numpad Enter";
            case 0x9D: return "Right Ctrl";
            case 0xB5: return "Numpad /";
            case 0xB7: return "Print Screen";
            case 0xB8: return "Right Alt";
            case 0xC5: return "Pause/Break";
            case 0xC7: return "Home";
            case 0xC8: return "Up Arrow";
            case 0xC9: return "Page Up";
            case 0xCB: return "Left Arrow";
            case 0xCD: return "Right Arrow";
            case 0xCF: return "End";
            case 0xD0: return "Down Arrow";
            case 0xD1: return "Page Down";
            case 0xD2: return "Insert";
            case 0xD3: return "Delete";
            default:   return "";
        }
    }

    // SKSE normalized gamepad codes (see InputMapper.h GamepadBinds).
    [[nodiscard]] inline constexpr std::string_view Pad(
        std::uint32_t a_code) {
        switch (a_code) {
            case 266: return "D-pad Up";
            case 267: return "D-pad Down";
            case 268: return "D-pad Left";
            case 269: return "D-pad Right";
            case 270: return "Start";
            case 271: return "Back / Select";
            case 272: return "Left Stick Click";
            case 273: return "Right Stick Click";
            case 274: return "LB / L1";
            case 275: return "RB / R1";
            case 276: return "A / Cross";
            case 277: return "B / Circle";
            case 278: return "X / Square";
            case 279: return "Y / Triangle";
            case 280: return "LT / L2";
            case 281: return "RT / R2";
            default:  return "";
        }
    }

    [[nodiscard]] inline std::string DikLabel(std::uint32_t a_code) {
        const auto fixed = Dik(a_code);
        if (!fixed.empty()) { return std::string{ fixed }; }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "Key 0x%02X", a_code);
        return std::string{ buf };
    }

    [[nodiscard]] inline std::string PadLabel(std::uint32_t a_code) {
        const auto fixed = Pad(a_code);
        if (!fixed.empty()) { return std::string{ fixed }; }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "Button %u", a_code);
        return std::string{ buf };
    }
}
