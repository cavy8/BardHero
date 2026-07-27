#pragma once

// Pure parsing for the widget-muffle list: third-party HUD widget menus
// hidden while a BardHero session runs. No RE, no OS - the game-thread work
// is WidgetMuffle.cpp glue.
//
// Menu names, not module names: STB Widgets and STB Active Effects register
// each widget as its OWN named IMenu whose name matches the SWF basename
// (read out of both DLLs' strings, 2026-07-27). Driving this from an INI
// list means a player can muffle any other widget mod's menus - or correct
// a name - without a rebuild.

#include <string>
#include <string_view>
#include <vector>

namespace SH::widget_muffle {

    // STB Widgets' eight menus, STB Active Effects, and TrueHUD (the
    // health/magicka/stamina bars - owner-approved default 2026-07-27).
    // The nine STB names are FIELD-CONFIRMED 2026-07-27 (probe logged
    // "hid [...]; absent [-]" on a rig running both 1.6 mods) and TrueHUD
    // appears verbatim in its DLL's strings. (TrueHUD_Widgets.swf inside
    // STB Widgets is not an STB menu - no such string in either STB DLL -
    // so it does not belong here.)
    inline constexpr std::string_view kDefaultMenus =
        "goldWidget,gametimeWidget,equipWidget_STB,lvlWidget,"
        "playtimeWidget,resistWidget,shoutWidget,weightWidget,"
        "STBActiveEffects,TrueHUD";

    // Inner-clip channels, for menus whose own machinery re-asserts
    // MOVIE-level visibility and which expose no widget.setVisible:
    //
    // - shoutWidget: a bare-timeline SWF (empty strings scan + field
    //   noapi probe agree); STB's own C++ drives it through this path.
    // - TrueHUD: UpdateVisibility() re-applies _view->SetVisible from an
    //   internal mode on pause/unpause events (source-read + field
    //   2026-07-27: a movie-level re-hide at countdown-complete was
    //   re-shown moments later). Its C++ writes the MOVIE, its AS writes
    //   the CHILD containers (trueHUDMain/PartialVisibilityContainer) -
    //   the stage clip _root.TrueHUD between them is written by nobody,
    //   the same blind spot the STB muffle rides.
    //
    // Menus not listed here use the AS API or the movie-level fallback.
    [[nodiscard]] inline constexpr std::string_view InnerClipFor(
        std::string_view a_menu) {
        if (a_menu == "shoutWidget") {
            return "_root.shoutWidget._visible";
        }
        if (a_menu == "TrueHUD") {
            return "_root.TrueHUD._visible";
        }
        return {};
    }

    // Where each menu's RESTORE target lives: the widget mod's own INI
    // visibility key. Restoring means re-applying THEIR config - exactly
    // what STB's post-loading config applier does - never blanket-true,
    // or a config-hidden widget (SetPlayTimeVisible=false in the field)
    // would be force-shown at song end. Unknown menus return {} and
    // restore to visible.
    inline constexpr std::string_view kWidgetsIni = "STB_Widgets.ini";
    inline constexpr std::string_view kActiveEffectsIni =
        "STB_ActiveEffects.ini";

    struct RestoreKey {
        std::string_view ini{};  // filename under Data/SKSE/Plugins/
        std::string_view key{};  // [Main] bool key in that file
    };

    [[nodiscard]] inline constexpr RestoreKey RestoreKeyFor(
        std::string_view a_menu) {
        if (a_menu == "goldWidget") {
            return { kWidgetsIni, "SetGoldVisible" };
        }
        if (a_menu == "gametimeWidget") {
            return { kWidgetsIni, "SetGameTimeVisible" };
        }
        if (a_menu == "equipWidget_STB") {
            return { kWidgetsIni, "SetEquipVisible" };
        }
        if (a_menu == "lvlWidget") {
            return { kWidgetsIni, "SetLvlVisible" };
        }
        if (a_menu == "playtimeWidget") {
            return { kWidgetsIni, "SetPlayTimeVisible" };
        }
        if (a_menu == "resistWidget") {
            return { kWidgetsIni, "SetResistVisible" };
        }
        if (a_menu == "shoutWidget") {
            return { kWidgetsIni, "SetShoutVisible" };
        }
        if (a_menu == "weightWidget") {
            return { kWidgetsIni, "SetWeightVisible" };
        }
        if (a_menu == "STBActiveEffects") {
            return { kActiveEffectsIni, "SetAeffVisible" };
        }
        return {};
    }

    // Comma-separated -> trimmed names, empties dropped. An all-empty or
    // blank string disables the feature (the host treats an empty list as
    // "do nothing").
    [[nodiscard]] inline std::vector<std::string> ParseMenuList(
        std::string_view a_csv) {
        std::vector<std::string> out;
        std::size_t pos = 0;
        while (pos <= a_csv.size()) {
            const std::size_t comma = a_csv.find(',', pos);
            const std::size_t end =
                comma == std::string_view::npos ? a_csv.size() : comma;
            std::size_t b = pos;
            std::size_t e = end;
            while (b < e && (a_csv[b] == ' ' || a_csv[b] == '\t')) { ++b; }
            while (e > b && (a_csv[e - 1] == ' ' || a_csv[e - 1] == '\t')) {
                --e;
            }
            if (e > b) {
                out.emplace_back(a_csv.substr(b, e - b));
            }
            if (comma == std::string_view::npos) { break; }
            pos = comma + 1;
        }
        return out;
    }
}
