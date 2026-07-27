// tests/test_nativemenu.cpp
#include "game/NativeMenuLogic.h"

#include "harness.h"

using namespace SH;

static void PassiveSetTests() {
    // Passive = display/cursor layers that cannot trap input. Everything
    // else activates the SESSION's native-menu recovery. (The browse
    // phase's dialogue guard is separate and name-specific - "Dialogue
    // Menu" in Session.cpp's MenuSink - because a broader tracker-based
    // guard at idle read the trigger flow's own inventory churn as
    // ownership loss and killed every browse; field 2026-07-27.)
    CHECK(native_menu::IsPassive("Cursor Menu"));
    CHECK(native_menu::IsPassive("HUD Menu"));
    CHECK(native_menu::IsPassive("TrueHUD"));
    CHECK(native_menu::IsPassive("Fader Menu"));
    CHECK(native_menu::IsPassive("Mist Menu"));
    CHECK(!native_menu::IsPassive("Dialogue Menu"));
    CHECK(!native_menu::IsPassive("MessageBoxMenu"));
    CHECK(!native_menu::IsPassive("InventoryMenu"));
    CHECK(!native_menu::IsPassive("Journal Menu"));
}

static void TrackerTests() {
    native_menu::Tracker t;
    CHECK(!t.Open());
    // Passive layers never count.
    t.Observe("Cursor Menu", true);
    CHECK(!t.Open());
    t.Observe("Dialogue Menu", true);
    CHECK(t.Open());
    // Closing a menu the tracker never saw open is a no-op.
    t.Observe("InventoryMenu", false);
    CHECK(t.Open());
    t.Observe("Dialogue Menu", false);
    CHECK(!t.Open());
    // Clear drops everything (session start/teardown semantics).
    t.Observe("Dialogue Menu", true);
    t.Clear();
    CHECK(!t.Open());
}

void RunTests() {
    PassiveSetTests();
    TrackerTests();
}

TEST_MAIN("NativeMenu")
