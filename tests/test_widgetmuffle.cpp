// tests/test_widgetmuffle.cpp
#include "game/WidgetMuffleLogic.h"

#include "harness.h"

using namespace SH;

static void DefaultListTests() {
    const auto names =
        widget_muffle::ParseMenuList(widget_muffle::kDefaultMenus);
    // Ten menus: STB Widgets' eight, STB Active Effects, and TrueHUD. A
    // shrinking default is a widget that quietly stays on screen.
    CHECK(names.size() == 10);
    // The names read verbatim out of the DLLs must stay present - they are
    // the anchor for the swf-basename convention the rest lean on.
    bool equip = false, gameTime = false, gold = false, active = false,
         trueHud = false;
    for (const auto& n : names) {
        if (n == "equipWidget_STB") { equip = true; }
        if (n == "gametimeWidget") { gameTime = true; }
        if (n == "goldWidget") { gold = true; }
        if (n == "STBActiveEffects") { active = true; }
        if (n == "TrueHUD") { trueHud = true; }
        // Menu names never carry whitespace; one slipping in means the
        // parser stopped trimming.
        CHECK(n.find(' ') == std::string::npos);
        CHECK(!n.empty());
    }
    CHECK(equip);
    CHECK(gameTime);
    CHECK(gold);
    CHECK(active);
    CHECK(trueHud);
}

static void InnerClipTests() {
    // shoutWidget (no widget.setVisible - field noapi probe) and TrueHUD
    // (its own C++ re-asserts movie visibility on pause/unpause - field
    // 2026-07-27) are driven through inner-clip paths. Everyone else must
    // return empty so the ladder proceeds to API/movie.
    CHECK(widget_muffle::InnerClipFor("shoutWidget") ==
          "_root.shoutWidget._visible");
    CHECK(widget_muffle::InnerClipFor("TrueHUD") ==
          "_root.TrueHUD._visible");
    CHECK(widget_muffle::InnerClipFor("goldWidget").empty());
    CHECK(widget_muffle::InnerClipFor("STBActiveEffects").empty());
}

static void ParseTests() {
    using widget_muffle::ParseMenuList;

    // Trimming and empty-entry dropping - an INI hand-edit with spaces or
    // trailing commas must not produce phantom menu names.
    const auto spaced = ParseMenuList("  a , b\t, c  ");
    CHECK(spaced.size() == 3);
    CHECK(spaced[0] == "a");
    CHECK(spaced[1] == "b");
    CHECK(spaced[2] == "c");

    CHECK(ParseMenuList("one").size() == 1);
    CHECK(ParseMenuList("one,").size() == 1);
    CHECK(ParseMenuList(",two").size() == 1);
    CHECK(ParseMenuList(",,").empty());
    CHECK(ParseMenuList("   ").empty());
    // Blank = the feature's OFF switch: the host treats an empty list as
    // "do nothing", so this emptiness is load-bearing.
    CHECK(ParseMenuList("").empty());
}

static void RestoreKeyTests() {
    // Every STB-owned default menu maps to its mod's own INI visibility
    // key - the restore target; a missing mapping would silently restore
    // that widget to blanket-visible, force-showing a config-hidden
    // widget. TrueHUD is movie-level BY DESIGN and maps to nothing
    // (restore-to-visible is correct for it), as do unknown menus.
    const auto names =
        widget_muffle::ParseMenuList(widget_muffle::kDefaultMenus);
    for (const auto& n : names) {
        const auto rk = widget_muffle::RestoreKeyFor(n);
        if (n == "TrueHUD") {
            CHECK(rk.ini.empty());
            CHECK(rk.key.empty());
            continue;
        }
        CHECK(!rk.ini.empty());
        CHECK(!rk.key.empty());
    }
    CHECK(widget_muffle::RestoreKeyFor("goldWidget").key ==
          "SetGoldVisible");
    CHECK(widget_muffle::RestoreKeyFor("playtimeWidget").key ==
          "SetPlayTimeVisible");
    CHECK(widget_muffle::RestoreKeyFor("STBActiveEffects").ini ==
          widget_muffle::kActiveEffectsIni);
    // Unknown menus (a user's own INI additions) restore to visible.
    CHECK(widget_muffle::RestoreKeyFor("TrueHUD_Widgets").ini.empty());
}

void RunTests() {
    DefaultListTests();
    ParseTests();
    RestoreKeyTests();
    InnerClipTests();
}

TEST_MAIN("WidgetMuffle")
