#pragma once

namespace SH {
    // The ONLY place the display name exists (spec sections 2 and 12: a forced
    // rename must be a one-commit operation). Everything user-visible - FLICK
    // sidebar, log lines, future FOMOD - reads this.
    inline constexpr const char* kDisplayName = "BardHero";
    // Keep in sync with CMakeLists project(VERSION) and vcpkg.json.
    inline constexpr const char* kVersion = "0.1.0";
}
