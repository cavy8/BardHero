// src/game/GuitarPropLogic.h
#pragma once

// PURE constants and validation for Skyrim animation-object model paths.
// TESModel::SetModel receives a path relative to Data\Meshes at runtime.

#include <string_view>

namespace SH::guitarprop {
    inline constexpr std::string_view kAnimationObjectModel =
        "BardHeroElectric\\GuitarAnimObject.nif";

    [[nodiscard]] constexpr bool IsMeshesRelativePath(
        std::string_view a_path) {
        if (a_path.empty() || a_path.front() == '\\' ||
            a_path.front() == '/') {
            return false;
        }
        if (a_path.size() >= 3 && a_path[1] == ':') { return false; }

        constexpr std::string_view forbidden = "meshes\\";
        if (a_path.size() < forbidden.size()) { return true; }
        for (std::size_t i = 0; i < forbidden.size(); ++i) {
            char ch = a_path[i];
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch + ('a' - 'A'));
            }
            if (ch != forbidden[i]) { return true; }
        }
        return false;
    }

    static_assert(IsMeshesRelativePath(kAnimationObjectModel));
}
