// InputMapperTests - the pure DI->engine mapper (spec 7.1). Covers the M0
// SPIKE-1 required design elements (seeding, plausibility, stale tolerance,
// buffer-full), bind mapping + edge semantics, the timeGetTime->QPC
// per-event mapping incl. wraparound, and the engage-diff reconciliation.
#include "harness.h"

#include "game/InputMapper.h"
#include "game/FlickOpenGate.h"
#include "game/ListNavigationLogic.h"
#include "game/NativeMenuLogic.h"
#include "game/PauseInputLogic.h"
#include "game/SessionInputLogic.h"

#include <vector>

using namespace SH;
using bard::InputAction;

namespace {
    constexpr std::uint32_t kT0 = 100000;  // ms; zeroed slots implausible
    constexpr double        kQ0 = 500.0;   // QPC seconds at tgt == kT0

    DiEvent Ev(std::uint32_t ofs, bool down, std::uint32_t ts,
               std::uint32_t seq) {
        return { ofs, down ? 0x80u : 0x00u, ts, seq, 0 };
    }

    struct Fix {
        InputMapper              m;
        Binds                    b;
        std::vector<MappedEvent> out;
        DiEvent                  buf[10]{};

        InputMapper::FeedStats Feed(std::uint32_t tgt = kT0,
                                    double qpc = kQ0, bool engaged = true) {
            return m.Feed(buf, 10, tgt, qpc, b, engaged, out);
        }
        void Seed() {  // first call after construction seeds, emits nothing
            Feed();
            out.clear();
        }
    };
}

static void TestSeedSwallowsPreexisting() {
    Fix f;
    f.buf[0] = Ev(0x02, true, kT0 - 50, 5);  // already sitting in the buffer
    f.Feed();  // seeding call
    CHECK(f.out.empty());
    const auto st = f.Feed();  // same buffer again: nothing is fresh
    CHECK(st.fresh == 0);
    CHECK(f.out.empty());
}

static void TestGarbageNeverPoisons() {
    Fix f;
    // heap junk (the M0 run-1 poison): implausible fields, huge sequence
    f.buf[0] = { 0x41424344u, 0x0D0A2020u, 0xDEADBEEFu, 99999u, 0 };
    f.Seed();
    f.buf[1] = Ev(0x02, true, kT0, 10);  // real event, LOW sequence
    const auto st = f.Feed();
    CHECK(st.fresh == 1);
    CHECK(f.out.size() == 1);
    CHECK(f.out[0].action == InputAction::kFret1);
}

static void TestFretEdgesAndTimestamp() {
    Fix f;
    f.Seed();
    f.buf[0] = Ev(0x02, true, kT0 - 15, 1);  // pressed 15ms before probe
    const auto st = f.Feed();
    CHECK(st.mapped == 1);
    CHECK(f.out.size() == 1);
    CHECK(f.out[0].action == InputAction::kFret1);
    CHECK(f.out[0].value == 1);
    CHECK(f.out[0].dik == 0x02);
    CHECK_NEAR(f.out[0].qpcSec, kQ0 - 0.015, 1e-9);
    f.out.clear();
    f.buf[0] = Ev(0x02, false, kT0 - 2, 2);
    f.Feed();
    CHECK(f.out.size() == 1);
    CHECK(f.out[0].value == 0);
}

static void TestFret5Maps() {
    Fix f;
    f.Seed();
    f.buf[0] = Ev(0x06, true, kT0, 1);
    f.Feed();
    CHECK(f.out.size() == 1);
    CHECK(f.out[0].action == InputAction::kFret5);
}

static void TestStrumPressOnly() {
    Fix f;
    f.Seed();
    f.buf[0] = Ev(0x39, true, kT0 - 10, 1);
    f.buf[1] = Ev(0x39, false, kT0 - 4, 2);
    f.Feed();
    CHECK(f.out.size() == 1);  // release must NOT strum (CH/YARG keyboard;
                               // field: release-strums doubled every tap
                               // into an overstrum and zeroed the combo)
    CHECK(f.out[0].action == InputAction::kStrum);
    CHECK(f.out[0].value == 1);
    CHECK_NEAR(f.out[0].qpcSec, kQ0 - 0.010, 1e-9);
}

static void TestSpEdges() {
    Fix f;
    f.Seed();
    f.buf[0] = Ev(0x2A, true, kT0, 1);
    f.buf[1] = Ev(0x2A, false, kT0, 2);
    f.Feed();
    CHECK(f.out.size() == 2);
    CHECK(f.out[0].action == InputAction::kStarPower);
    CHECK(f.out[0].value == 1);
    CHECK(f.out[1].value == 0);
}

static void TestPrimarySpButtonFallback() {
    Binds binds;
    KeyboardSpFallback fallback;
    std::vector<MappedEvent> out;

    // Field regression: this runtime omitted DIK_LSHIFT from the raw
    // DirectInput buffer while still presenting it as a Skyrim ButtonEvent.
    // The fallback must produce the same Star Power action as the raw H path.
    fallback.FeedButton(0x2A, true, kQ0, binds, true, out);
    CHECK(out.size() == 1);
    CHECK(out[0].dik == 0x2A);
    CHECK(out[0].action == InputAction::kStarPower);
    CHECK(out[0].value == 1);
    fallback.FeedButton(0x2A, false, kQ0 + 0.1, binds, true, out);
    CHECK(out.size() == 2);
    CHECK(out[1].action == InputAction::kStarPower);
    CHECK(out[1].value == 0);

    // H remains owned by the existing raw secondary-binding path.
    fallback.FeedButton(0x23, true, kQ0 + 0.2, binds, true, out);
    CHECK(out.size() == 2);

    // If a machine reports Left Shift through both paths in one frame, the
    // equivalent raw event already in the queue suppresses the fallback.
    fallback.Reset();
    out.clear();
    out.push_back(
        { kQ0, InputAction::kStarPower, 1, 0x2A });
    fallback.FeedButton(0x2A, true, kQ0, binds, true, out);
    CHECK(out.size() == 1);

    // A release while paused is remembered and reconciled on resume so Star
    // Power cannot remain logically held.
    out.clear();
    fallback.FeedButton(0x2A, false, kQ0 + 1.0, binds, false, out);
    CHECK(out.empty());
    fallback.EmitEngageDiff(kQ0 + 2.0, binds, out);
    CHECK(out.size() == 1);
    CHECK(out[0].action == InputAction::kStarPower);
    CHECK(out[0].value == 0);
}

static void TestPausePressOnly() {
    Fix f;
    f.Seed();
    f.buf[0] = Ev(0x01, true, kT0, 1);
    f.buf[1] = Ev(0x01, false, kT0, 2);
    f.Feed();
    CHECK(f.out.size() == 1);
    CHECK(f.out[0].action == InputAction::kPause);
    CHECK(f.out[0].value == 1);
}

static void TestUnboundIgnored() {
    Fix f;
    f.Seed();
    f.buf[0] = Ev(0x10, true, kT0, 1);  // Q - not bound
    const auto st = f.Feed();
    CHECK(st.fresh == 1);
    CHECK(st.mapped == 0);
    CHECK(f.out.empty());
}

static void TestTimeGetTimeWraparound() {
    Fix f;
    f.Feed(5, kQ0);  // seed near the 49.7-day wrap
    f.out.clear();
    f.buf[0] = Ev(0x39, true, 0xFFFFFFFBu, 1);  // stamped 10ms before tgt=5
    f.Feed(5, kQ0);
    CHECK(f.out.size() == 1);
    CHECK_NEAR(f.out[0].qpcSec, kQ0 - 0.010, 1e-9);
}

static void TestStaleMappedAndCounted() {
    Fix f;
    f.Seed();
    // the M0 alt-tab case: a release delivered 23.5s late on focus regain
    f.buf[0] = Ev(0x02, false, kT0 - 23500, 1);
    const auto st = f.Feed();
    CHECK(st.stale == 1);
    CHECK(st.mapped == 1);  // still mapped: engine's monotonic clamp
    CHECK(f.out.size() == 1);
    CHECK(f.out[0].value == 0);
}

static void TestBufferFull() {
    Fix f;
    f.Seed();
    for (int i = 0; i < 10; ++i) {
        f.buf[i] = Ev(0x02, (i % 2) == 0, kT0 - 9 + i,
                      static_cast<std::uint32_t>(i + 1));
    }
    const auto st = f.Feed();
    CHECK(st.bufferFull);
    CHECK(st.fresh == 10);
}

static void TestOrderBySequence() {
    Fix f;
    f.Seed();
    f.buf[0] = Ev(0x03, false, kT0 - 2, 2);  // later event in earlier slot
    f.buf[1] = Ev(0x03, true, kT0 - 9, 1);
    f.Feed();
    CHECK(f.out.size() == 2);
    CHECK(f.out[0].value == 1);  // press (seq 1) first
    CHECK(f.out[1].value == 0);
}

static void TestSequenceGapsAreNotLoss() {
    Fix f;
    f.Seed();
    f.buf[0] = Ev(0x02, true, kT0, 5);
    f.Feed();
    f.out.clear();
    f.buf[0] = Ev(0x02, false, kT0 + 30, 9);  // gap 6..8 = mouse events
    const auto st = f.Feed(kT0 + 30, kQ0 + 0.030);
    CHECK(st.mapped == 1);
    CHECK(st.stale == 0);
}

static void TestEngageDiffRelease() {
    Fix f;
    f.Seed();
    f.buf[0] = Ev(0x03, true, kT0, 1);
    f.Feed();  // engaged press - the engine knows fret2 is down
    f.out.clear();
    f.buf[0] = Ev(0x03, false, kT0 + 100, 2);
    f.Feed(kT0 + 100, kQ0 + 0.1, false);  // released while disengaged
    CHECK(f.out.empty());                 // tracked, not emitted
    f.m.EmitEngageDiff(kQ0 + 0.2, f.b, f.out);
    CHECK(f.out.size() == 1);
    CHECK(f.out[0].action == InputAction::kFret2);
    CHECK(f.out[0].value == 0);
    CHECK_NEAR(f.out[0].qpcSec, kQ0 + 0.2, 1e-9);
    f.out.clear();
    f.m.EmitEngageDiff(kQ0 + 0.3, f.b, f.out);  // idempotent once synced
    CHECK(f.out.empty());
}

static void TestEngageDiffPress() {
    Fix f;
    f.Seed();
    f.buf[0] = Ev(0x02, true, kT0, 1);
    f.Feed(kT0, kQ0, false);  // pressed while disengaged
    CHECK(f.out.empty());
    f.m.EmitEngageDiff(kQ0 + 0.05, f.b, f.out);
    CHECK(f.out.size() == 1);
    CHECK(f.out[0].action == InputAction::kFret1);
    CHECK(f.out[0].value == 1);
}

static void TestWhammyRefire() {
    Fix f;
    f.b.whammy = 0x2C;  // bind Z
    f.Seed();
    f.buf[0] = Ev(0x2C, true, kT0 - 3, 1);
    f.Feed();
    CHECK(f.out.size() == 1);  // press keeps its DI stamp, no double-fire
    CHECK(f.out[0].action == InputAction::kWhammy);
    CHECK_NEAR(f.out[0].qpcSec, kQ0 - 0.003, 1e-9);
    f.out.clear();
    f.buf[0] = {};  // no events; key still held
    f.Feed(kT0 + 16, kQ0 + 0.016);
    CHECK(f.out.size() == 1);  // refire at probe time
    CHECK_NEAR(f.out[0].qpcSec, kQ0 + 0.016, 1e-9);
    f.out.clear();
    f.Feed(kT0 + 32, kQ0 + 0.032, false);  // disengaged: no refire
    CHECK(f.out.empty());
    f.buf[0] = Ev(0x2C, false, kT0 + 48, 2);  // release while disengaged
    f.Feed(kT0 + 48, kQ0 + 0.048, false);
    f.out.clear();
    f.buf[0] = {};
    f.Feed(kT0 + 64, kQ0 + 0.064, true);  // re-engaged: held no more
    CHECK(f.out.empty());
}

static void TestResetReseeds() {
    Fix f;
    f.Seed();
    f.buf[0] = Ev(0x02, true, kT0, 1);
    f.Feed();
    CHECK(f.out.size() == 1);
    f.out.clear();
    f.m.Reset();
    f.buf[0] = Ev(0x02, false, kT0 + 10, 2);
    f.Feed(kT0 + 10, kQ0 + 0.010);  // first call after Reset seeds only
    CHECK(f.out.empty());
}

static void TestIsBound() {
    Binds b;
    CHECK(b.IsBound(0x02));   // fret 1
    CHECK(b.IsBound(0x39));   // strum
    CHECK(b.IsBound(0x2A));   // SP
    CHECK(b.IsBound(0x01));   // pause
    CHECK(!b.IsBound(0x29));  // console key is not a default bind
    CHECK(!b.IsBound(0x00));  // dik 0 never matches an unbound slot
    CHECK(!b.IsBound(0x10));
    // secondary defaults (Wii GH bridge layout)
    CHECK(b.IsBound(0x1E));   // A = fret 1 (2nd)
    CHECK(b.IsBound(0x26));   // L = fret 5 (2nd)
    CHECK(b.IsBound(0xC8));   // Up arrow = strum (2nd)
    CHECK(b.IsBound(0xD0));   // Down arrow = strum (3rd)
    CHECK(b.IsBound(0x27));   // ';' = whammy (2nd)
    CHECK(b.IsBound(0x23));   // H = star power (2nd, gh3.PIE Minus/tilt)
    CHECK(!b.IsBound(0x1D));  // LCtrl unbound
}

static void TestPausedKeyboardCapture() {
    using SH::pause_input::Action;
    SH::pause_input::Keys keys;
    keys.console     = 0x29;
    keys.printScreen = 0xB7;
    keys.moveUp      = 0xC8;
    keys.moveDown    = 0xD0;
    keys.confirm1    = 0x02;
    keys.confirm2    = 0x1E;
    keys.enter       = 0x1C;
    keys.numEnter    = 0x9C;
    keys.pause1      = 0x01;
    keys.pause2      = 0x1C;

    CHECK(SH::pause_input::ForKeyboard(0x32, keys) ==
          Action::kSwallow);  // M / map may not reach Skyrim
    CHECK(SH::pause_input::ForKeyboard(0x0F, keys) ==
          Action::kSwallow);  // Tab / journal likewise
    CHECK(SH::pause_input::ForKeyboard(0x10, keys) ==
          Action::kSwallow);  // arbitrary remapped menu/game key
    CHECK(SH::pause_input::ForKeyboard(0xC8, keys) == Action::kMoveUp);
    CHECK(SH::pause_input::ForKeyboard(0xD0, keys) == Action::kMoveDown);
    CHECK(SH::pause_input::ForKeyboard(0x01, keys) == Action::kResume);
    CHECK(SH::pause_input::ForKeyboard(0x1C, keys) == Action::kConfirm);
    CHECK(SH::pause_input::ForKeyboard(0x29, keys) == Action::kSwallow);
    CHECK(SH::pause_input::ForKeyboard(0xB7, keys) == Action::kPass);
}

static void TestSessionKeyboardOwnershipAndRecovery() {
    using SH::session_input::Phase;
    using SH::session_input::Overlay;
    using SH::pause_input::Action;
    SH::pause_input::Keys keys;
    keys.console     = 0x29;
    keys.printScreen = 0xB7;
    keys.moveUp      = 0xC8;
    keys.moveDown    = 0xD0;
    keys.confirm1    = 0x02;
    keys.confirm2    = 0x1E;
    keys.enter       = 0x1C;
    keys.numEnter    = 0x9C;
    keys.pause1      = 0x01;
    keys.pause2      = 0x1C;

    // The old runtime only swallowed session binds plus Tab while engaged.
    // M was neither, so it could still generate Skyrim's Map action.
    CHECK(SH::session_input::ForKeyboard(
              Phase::kPlaying, false, 0x32, keys) == Action::kSwallow);
    CHECK(SH::session_input::ForKeyboard(
              Phase::kPlaying, false, 0x0F, keys) == Action::kSwallow);
    CHECK(SH::session_input::ForKeyboard(
              Phase::kResumeCountdown, false, 0x32, keys) ==
          Action::kSwallow);
    CHECK(SH::session_input::ForKeyboard(
              Phase::kResumeCountdown, false, 0x01, keys) ==
          Action::kPause);

    // Once an unexpected vanilla menu is already open, BardHero must yield
    // every key so its native close shortcut can recover the player.
    //
    // Since 2026-07-26 this flag ALSO carries an open FLICK menu, which is
    // why the parameter is worth more than it looks. Playing phase swallows
    // the whole keyboard so no game hotkey can fire mid-song - but FLICK
    // chains the same input dispatch we do, so that was also eating FLICK's
    // own toggle key and the settings page could not be opened during a
    // performance. An open FLICK menu is a menu; the session lets go.
    CHECK(SH::session_input::ForKeyboard(
              Phase::kPlaying, true, 0x32, keys) == Action::kPass);
    // Including the keys the session would otherwise claim hardest: its own
    // pause bind and a fret. Inside a menu they belong to the menu.
    CHECK(SH::session_input::ForKeyboard(
              Phase::kPlaying, true, keys.pause1, keys) == Action::kPass);
    CHECK(SH::session_input::ForKeyboard(
              Phase::kPlaying, true, keys.confirm1, keys) == Action::kPass);
    CHECK(SH::session_input::ForKeyboard(
              Phase::kPlaying, true, keys.console, keys) == Action::kPass);
    CHECK(SH::session_input::ForKeyboard(
              Phase::kPaused, true, 0x32, keys) == Action::kPass);
    CHECK(SH::session_input::ForKeyboard(
              Phase::kResumeCountdown, true, 0x01, keys) == Action::kPass);

    // Console is part of BardHero's keyboard ownership in every minigame
    // phase. Print Screen remains the sole always-available exception.
    CHECK(SH::session_input::ForKeyboard(
              Phase::kPlaying, false, 0x29, keys) == Action::kSwallow);
    CHECK(SH::session_input::ForKeyboard(
              Phase::kPaused, false, 0x29, keys) == Action::kSwallow);
    CHECK(SH::session_input::ForKeyboard(
              Phase::kResumeCountdown, false, 0x29, keys) ==
          Action::kSwallow);
    CHECK(SH::session_input::ForKeyboard(
              Phase::kPaused, false, 0xB7, keys) == Action::kPass);
    // Outside the minigame, and during recovery from an already-open native
    // menu, BardHero must not trap the console key.
    CHECK(SH::session_input::ForKeyboard(
              Phase::kNone, false, 0x29, keys) == Action::kPass);
    CHECK(SH::session_input::ForKeyboard(
              Phase::kPlaying, true, 0x29, keys) == Action::kPass);

    // Songbook and Results are outside EngineFeed::active but are still
    // minigame-owned surfaces. Both suppress console while preserving their
    // existing narrow navigation capture and Print Screen exception.
    CHECK(SH::session_input::ForOverlayKeyboard(
              Overlay::kBrowser, 0x29, keys, false) == Action::kSwallow);
    CHECK(SH::session_input::ForOverlayKeyboard(
              Overlay::kResults, 0x29, keys, false) == Action::kSwallow);
    CHECK(SH::session_input::ForOverlayKeyboard(
              Overlay::kBrowser, 0xB7, keys, false) == Action::kPass);
    CHECK(SH::session_input::ForOverlayKeyboard(
              Overlay::kResults, 0x10, keys, false) == Action::kPass);
    CHECK(SH::session_input::ForOverlayKeyboard(
              Overlay::kResults, 0x02, keys, true) == Action::kConfirm);
    CHECK(SH::session_input::ForOverlayKeyboard(
              Overlay::kBrowser, 0x10, keys, true) == Action::kSwallow);

    // The browser's strum keys are NAMED move actions, never a blanket
    // swallow. The name is what carries the direction to PublishNavHold,
    // which feeds the Songbook's HeldRepeat - without it, holding the strum
    // moved one row per press (field 2026-07-27: "Make sure holding down
    // arrow let's you scroll"). The gamepad path always returned these; a
    // GH controller through GlovePIE IS the keyboard, so keyboard must too.
    CHECK(SH::session_input::ForOverlayKeyboard(
              Overlay::kBrowser, 0xC8, keys, true) == Action::kMoveUp);
    CHECK(SH::session_input::ForOverlayKeyboard(
              Overlay::kBrowser, 0xD0, keys, true) == Action::kMoveDown);
    // Results has no list: its strum keys stay non-moves, so a held strum
    // across the results screen cannot pre-scroll anything.
    CHECK(SH::session_input::ForOverlayKeyboard(
              Overlay::kResults, 0xC8, keys, false) == Action::kPass);
    CHECK(SH::session_input::ForOverlayKeyboard(
              Overlay::kResults, 0xD0, keys, false) == Action::kPass);
}

static void TestNativeMenuRecoveryTracking() {
    SH::native_menu::Tracker menus;
    menus.Observe("Cursor Menu", true);  // BardHero/FLICK cursor
    menus.Observe("TrueHUD", true);      // passive overlay
    CHECK(!menus.Open());

    menus.Observe("MapMenu", true);
    CHECK(menus.Open());
    menus.Observe("Journal Menu", true);  // nested transition
    menus.Observe("MapMenu", false);
    CHECK(menus.Open());
    menus.Observe("Journal Menu", false);
    CHECK(!menus.Open());

    // Duplicate edges cannot create a depth mismatch or a trapped recovery.
    menus.Observe("InventoryMenu", true);
    menus.Observe("InventoryMenu", true);
    menus.Observe("InventoryMenu", false);
    CHECK(!menus.Open());
    menus.Clear();
    CHECK(!menus.Open());
}

static void TestResultsHostOpenGate() {
    SH::flick_open::Gate gate;

    // An input dispatch already inside FUCK must finish before Results may
    // become visible. Once opening begins, later input dispatches are held
    // outside the unsafe WindowState lookup.
    CHECK(gate.TryEnterInput());
    gate.BeginOpen();
    CHECK(!gate.CanPublish());
    CHECK(!gate.TryEnterInput());
    gate.LeaveInput();
    CHECK(gate.CanPublish());

    // The first completed render creates/settles the host state. Input may
    // enter FUCK again only after that boundary.
    gate.CompleteFirstDraw();
    CHECK(!gate.BlockingInput());
    CHECK(gate.TryEnterInput());
    gate.LeaveInput();

    // Load/teardown cancellation must never leave input suppressed.
    gate.BeginOpen();
    CHECK(gate.BlockingInput());
    gate.Cancel();
    CHECK(!gate.BlockingInput());
    CHECK(gate.TryEnterInput());
    gate.LeaveInput();
}

// Secondary binds land in the SAME action slots (controller-as-keyboard
// bridge, field 2026-07-19): A..L frets, Up/Down-arrow strum bar (press
// only, both switches), ';' whammy.
static void TestSecondaryBinds() {
    Fix f;
    f.Seed();
    f.buf[0] = Ev(0x1E, true, kT0, 10);   // A -> fret 1 press
    f.buf[1] = Ev(0xC8, true, kT0, 11);   // strum bar up -> strum
    f.buf[2] = Ev(0xC8, false, kT0, 12);  // bar release: NO strum
    f.buf[3] = Ev(0xD0, true, kT0, 13);   // strum bar down -> strum
    f.buf[4] = Ev(0x27, true, kT0, 14);   // ';' -> whammy press
    f.buf[5] = Ev(0x1E, false, kT0, 15);  // A release -> fret 1 release
    const auto st = f.Feed();
    CHECK(st.mapped == 5);
    CHECK(f.out.size() == 5);
    CHECK(f.out[0].action == InputAction::kFret1 && f.out[0].value == 1);
    CHECK(f.out[1].action == InputAction::kStrum && f.out[1].value == 1);
    CHECK(f.out[2].action == InputAction::kStrum && f.out[2].value == 1);
    CHECK(f.out[3].action == InputAction::kWhammy && f.out[3].value == 1);
    CHECK(f.out[4].action == InputAction::kFret1 && f.out[4].value == 0);
}

// Primary and secondary keys mix freely mid-song; unbinding a secondary
// slot (0) never matches anything.
static void TestSecondaryMixAndUnbind() {
    Fix f;
    f.Seed();
    f.buf[0] = Ev(0x02, true, kT0, 10);  // keyboard 1 -> fret 1
    f.buf[1] = Ev(0x1F, true, kT0, 11);  // guitar S -> fret 2
    auto st = f.Feed();
    CHECK(st.mapped == 2);
    CHECK(f.out[0].action == InputAction::kFret1);
    CHECK(f.out[1].action == InputAction::kFret2);
    CHECK(f.m.HeldFretMask() == 0x03);

    Fix g;
    g.b.fret2[0] = 0;  // secondary unbound
    g.b.strum2 = g.b.strum3 = 0;
    g.Seed();
    g.buf[0] = Ev(0x1E, true, kT0, 10);  // A: now nothing
    g.buf[1] = Ev(0xC8, true, kT0, 11);  // Up arrow: now nothing
    st = g.Feed();
    CHECK(st.mapped == 0);
    CHECK(g.out.empty());
}

static void TestGamepadRecommendedLayout() {
    GamepadBinds b;
    CHECK(b.fret[0] == 280);  // LT
    CHECK(b.fret[1] == 274);  // LB
    CHECK(b.fret[2] == 275);  // RB
    CHECK(b.fret[3] == 281);  // RT
    CHECK(b.fret[4] == 276);  // A
    CHECK(b.strum[0] == 266); // D-pad up
    CHECK(b.strum[1] == 267); // D-pad down
    CHECK(b.sp == 278);       // X
    CHECK(b.whammy == 279);   // Y
    CHECK(b.pause == 270);    // Start
}

static void TestGamepadModeCoalescesChordStrum() {
    GamepadBinds             b;
    GamepadMapper            m;
    std::vector<MappedEvent> out;
    m.FeedButton(b.fret[0], true, kQ0, b, true, true, out);
    m.FeedButton(b.fret[1], true, kQ0, b, true, true, out);
    CHECK(out.size() == 2);
    CHECK(out[0].action == InputAction::kFret1);
    CHECK(out[1].action == InputAction::kFret2);
    m.EndFrame(kQ0, b, true, true, out);
    CHECK(out.size() == 3);
    CHECK(out[2].action == InputAction::kStrum);
    CHECK(m.HeldFretMask() == 0x03);
}

static void TestGamepadExplicitStrumAndClassicMode() {
    GamepadBinds             b;
    GamepadMapper            m;
    std::vector<MappedEvent> out;

    // Classic mode keeps fret and strum separate.
    m.FeedButton(b.fret[4], true, kQ0, b, true, false, out);
    m.EndFrame(kQ0, b, true, false, out);
    CHECK(out.size() == 1);
    CHECK(out[0].action == InputAction::kFret5);

    // D-pad strum works in either mode and is required for open notes.
    m.FeedButton(b.strum[0], true, kQ0, b, true, false, out);
    m.FeedButton(b.strum[0], false, kQ0, b, true, false, out);
    CHECK(out.size() == 2);
    CHECK(out[1].action == InputAction::kStrum);
}

static void TestGamepadUtilityActionsAndEngageDiff() {
    GamepadBinds             b;
    GamepadMapper            m;
    std::vector<MappedEvent> out;

    m.FeedButton(b.fret[2], true, kQ0, b, false, true, out);
    CHECK(out.empty());
    m.EmitEngageDiff(kQ0 + 0.1, b, out);
    CHECK(out.size() == 1);
    CHECK(out[0].action == InputAction::kFret3);
    CHECK(out[0].value == 1);

    out.clear();
    m.FeedButton(b.sp, true, kQ0, b, true, true, out);
    m.FeedButton(b.whammy, true, kQ0, b, true, true, out);
    m.FeedButton(b.pause, true, kQ0, b, true, true, out);
    CHECK(out.size() == 3);
    CHECK(out[0].action == InputAction::kStarPower);
    CHECK(out[1].action == InputAction::kWhammy);
    CHECK(out[2].action == InputAction::kPause);
}

static void TestGamepadSurfaceOwnership() {
    using SH::pause_input::Action;
    using SH::session_input::GamepadKeys;
    using SH::session_input::Overlay;
    using SH::session_input::Phase;

    GamepadKeys keys;
    keys.moveUp = 266;
    keys.moveDown = 267;
    keys.confirm = 276;
    keys.cancel = 277;
    keys.pause = 270;

    CHECK(SH::session_input::ForGamepad(
              Phase::kPlaying, false, 274, keys) == Action::kSwallow);
    CHECK(SH::session_input::ForGamepad(
              Phase::kPlaying, true, 274, keys) == Action::kPass);
    CHECK(SH::session_input::ForGamepad(
              Phase::kPaused, false, 266, keys) == Action::kMoveUp);
    CHECK(SH::session_input::ForGamepad(
              Phase::kPaused, false, 267, keys) == Action::kMoveDown);
    CHECK(SH::session_input::ForGamepad(
              Phase::kPaused, false, 276, keys) == Action::kConfirm);
    CHECK(SH::session_input::ForGamepad(
              Phase::kPaused, false, 277, keys) == Action::kResume);
    CHECK(SH::session_input::ForGamepad(
              Phase::kPaused, false, 270, keys) == Action::kResume);

    CHECK(SH::session_input::ForOverlayGamepad(
              Overlay::kBrowser, 266, keys) == Action::kMoveUp);
    CHECK(SH::session_input::ForOverlayGamepad(
              Overlay::kBrowser, 267, keys) == Action::kMoveDown);
    CHECK(SH::session_input::ForOverlayGamepad(
              Overlay::kBrowser, 276, keys) == Action::kConfirm);
    CHECK(SH::session_input::ForOverlayGamepad(
              Overlay::kBrowser, 277, keys) == Action::kResume);
    CHECK(SH::session_input::ForOverlayGamepad(
              Overlay::kBrowser, 274, keys) == Action::kSwallow);
    CHECK(SH::session_input::ForOverlayGamepad(
              Overlay::kResults, 276, keys) == Action::kConfirm);
    CHECK(SH::session_input::ForOverlayGamepad(
              Overlay::kResults, 277, keys) == Action::kConfirm);
}

static void TestListNavigationWrapsBothWays() {
    using SH::list_navigation::WrapIndex;

    CHECK(WrapIndex(-1, 1, 5) == 0);
    CHECK(WrapIndex(-1, -1, 5) == 4);
    CHECK(WrapIndex(4, 1, 5) == 0);
    CHECK(WrapIndex(0, -1, 5) == 4);
    CHECK(WrapIndex(3, 3, 5) == 1);
    CHECK(WrapIndex(1, -3, 5) == 3);
    CHECK(WrapIndex(0, 1, 1) == 0);
    CHECK(WrapIndex(0, 1, 0) == -1);
    CHECK(WrapIndex(-1, 0, 5) == -1);
}

static void TestSongbookDifficultyStepsAndClamps() {
    using SH::list_navigation::StepDifficulty;
    using SH::list_navigation::kDifficultyCount;

    CHECK(kDifficultyCount == 4);
    CHECK(StepDifficulty(0, 1) == 1);
    CHECK(StepDifficulty(3, -1) == 2);
    CHECK(StepDifficulty(1, 2) == 3);
    // CLAMPS, never wraps: the list-selection cursor wraps (WrapIndex
    // above) but difficulty must not, or holding the step key past Expert
    // silently drops the player to Easy.
    CHECK(StepDifficulty(3, 1) == 3);
    CHECK(StepDifficulty(0, -1) == 0);
    CHECK(StepDifficulty(3, 99) == 3);
    CHECK(StepDifficulty(0, -99) == 0);
    // A garbage stored setting is clamped into range rather than
    // propagated: this value indexes a name array and feeds LoadSong.
    CHECK(StepDifficulty(-7, 0) == 0);
    CHECK(StepDifficulty(42, 0) == 3);
    CHECK(StepDifficulty(2, 0) == 2);
}

static void TestHeldListNavigationRepeatsAfterDelay() {
    using SH::list_navigation::HeldRepeat;
    HeldRepeat repeat;

    // The press edge moves once immediately. Holding does not double-move
    // that frame, waits 350 ms, then repeats at an 80 ms cadence.
    CHECK(repeat.Step(1, 1, 10.0) == 1);
    CHECK(repeat.Step(0, 1, 10.349) == 0);
    CHECK(repeat.Step(0, 1, 10.350) == 1);
    CHECK(repeat.Step(0, 1, 10.429) == 0);
    CHECK(repeat.Step(0, 1, 10.430) == 1);

    // Releasing cancels repeat. A new press, including the opposite
    // direction, moves once and earns a fresh initial delay.
    CHECK(repeat.Step(0, 0, 10.500) == 0);
    CHECK(repeat.Step(0, 0, 20.000) == 0);
    CHECK(repeat.Step(-1, -1, 20.100) == -1);
    CHECK(repeat.Step(0, -1, 20.449) == 0);
    CHECK(repeat.Step(0, -1, 20.450) == -1);
}

static void RunTests() {
    TestSeedSwallowsPreexisting();
    TestGarbageNeverPoisons();
    TestFretEdgesAndTimestamp();
    TestFret5Maps();
    TestStrumPressOnly();
    TestSpEdges();
    TestPrimarySpButtonFallback();
    TestPausePressOnly();
    TestUnboundIgnored();
    TestTimeGetTimeWraparound();
    TestStaleMappedAndCounted();
    TestBufferFull();
    TestOrderBySequence();
    TestSequenceGapsAreNotLoss();
    TestEngageDiffRelease();
    TestEngageDiffPress();
    TestWhammyRefire();
    TestResetReseeds();
    TestIsBound();
    TestPausedKeyboardCapture();
    TestSessionKeyboardOwnershipAndRecovery();
    TestNativeMenuRecoveryTracking();
    TestResultsHostOpenGate();
    TestSecondaryBinds();
    TestSecondaryMixAndUnbind();
    TestGamepadRecommendedLayout();
    TestGamepadModeCoalescesChordStrum();
    TestGamepadExplicitStrumAndClassicMode();
    TestGamepadUtilityActionsAndEngageDiff();
    TestGamepadSurfaceOwnership();
    TestListNavigationWrapsBothWays();
    TestSongbookDifficultyStepsAndClamps();
    TestHeldListNavigationRepeatsAfterDelay();
}

TEST_MAIN("InputMapper")
