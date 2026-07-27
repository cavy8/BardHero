// tests/test_bindings.cpp
#include "game/BindingEditLogic.h"
#include "game/KeyNamesLogic.h"

#include "harness.h"

using namespace SH;

static void DikNameTests() {
    // Spot checks against the DirectInput scan codes the shipped defaults
    // use - a wrong name here means the tab lies about a real binding.
    CHECK(key_names::Dik(0x2A) == "Left Shift");
    CHECK(key_names::Dik(0x39) == "Space");
    CHECK(key_names::Dik(0x01) == "Escape");
    CHECK(key_names::Dik(0x02) == "1");
    CHECK(key_names::Dik(0x1E) == "A");
    CHECK(key_names::Dik(0xC8) == "Up Arrow");
    CHECK(key_names::Dik(0x23) == "H");
    // Unknown scan codes fall back to a hex label, never an empty string.
    CHECK(key_names::DikLabel(0x2A) == "Left Shift");
    CHECK(key_names::DikLabel(0xE7) == "Key 0xE7");
    // 0 is the unbound sentinel and is the CALLER's job to special-case;
    // the table still answers something printable.
    CHECK(!key_names::DikLabel(0).empty());
}

static void PadNameTests() {
    CHECK(key_names::Pad(266) == "D-pad Up");
    CHECK(key_names::Pad(267) == "D-pad Down");
    CHECK(key_names::Pad(270) == "Start");
    CHECK(key_names::Pad(276) == "A / Cross");
    CHECK(key_names::Pad(280) == "LT / L2");
    CHECK(key_names::Pad(281) == "RT / R2");
    CHECK(key_names::PadLabel(299) == "Button 299");
    CHECK(!key_names::PadLabel(0).empty());
}

static void StealTests() {
    // A column is an array of slot pointers in UI order. Assign writes the
    // code, clears every OTHER slot holding it, and reports the first
    // loser so the UI can tint it.
    int a = 10, b = 20, c = 30;
    int* col[] = { &a, &b, &c };
    // Plain assign, no conflict.
    CHECK(binding_edit::Assign(col, 3, 0, 40) == -1);
    CHECK(a == 40);
    // Steal: give b's code to c; b goes unbound, index 1 reported.
    CHECK(binding_edit::Assign(col, 3, 2, 20) == 1);
    CHECK(c == 20);
    CHECK(b == 0);
    // Self-rebind is a no-op, never a steal.
    CHECK(binding_edit::Assign(col, 3, 2, 20) == -1);
    CHECK(c == 20);
    // Untouched slots stay untouched throughout.
    CHECK(a == 40);
}

static void DefaultsTests() {
    // Reset values come from the mapper's own struct defaults - the one
    // source of truth. Field-for-field, both columns.
    const auto kb = binding_edit::KeyboardDefaults();
    const Binds ref{};
    CHECK(kb.size() == 9);
    for (int i = 0; i < 5; ++i) { CHECK(kb[i] == ref.fret[i]); }
    CHECK(kb[5] == ref.strum);
    CHECK(kb[6] == ref.sp);
    CHECK(kb[7] == ref.whammy);
    CHECK(kb[8] == ref.pause);

    const auto gp = binding_edit::GamepadDefaults();
    const GamepadBinds gref{};
    CHECK(gp.size() == 10);
    for (int i = 0; i < 5; ++i) { CHECK(gp[i] == gref.fret[i]); }
    CHECK(gp[5] == gref.strum[0]);
    CHECK(gp[6] == gref.strum[1]);
    CHECK(gp[7] == gref.sp);
    CHECK(gp[8] == gref.whammy);
    CHECK(gp[9] == gref.pause);
}

void RunTests() {
    DikNameTests();
    PadNameTests();
    StealTests();
    DefaultsTests();
}

TEST_MAIN("Bindings")
