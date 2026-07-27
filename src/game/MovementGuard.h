// src/game/MovementGuard.h
#pragma once

namespace SH {
    // Blocks player movement-class controls (move, jump, sneak, POV switch)
    // while a session is actively engaged; restores them on pause / end /
    // abort / save load.
    //
    // ⚠ THIS NO LONGER TOUCHES ControlMap's CONTROL MASKS, AND MUST NOT ⚠
    //
    // Until 2026-07-22 it called ControlMap::ToggleControls. On AE 1.6.1170
    // the members past `controlMap[]` are shifted +8, so `enabledControls`
    // as CommonLibSSE-NG declares it is really `contextPriorityStack::_size`.
    // `ToggleControls(kMovement, false)` therefore cleared bit 0 of the
    // input context stack's SIZE - 1 -> 0, emptying the stack - and the
    // engine then resolved no input context at all. That was the post-song
    // input lock, the FUCK.dll CTD at the results box, and the reason
    // opening any menu "fixed" it (a push/pop rewrote _size). Full evidence
    // and the six confirmations in
    // `docs/evidence/2026-07-22-controlmap-offsets-are-wrong-on-ae.md`;
    // field-confirmed by bBlockMovement=0 clearing both symptoms in one run.
    //
    // The block now runs through our own input hook, which unlinks the
    // movement keys from the event list while a session is engaged. The
    // guard reads only the BINDING table (`GetMappedKey`, +0x60), the one
    // region of ControlMap that is verified correct on this runtime.
    //
    // Because the hook consults the set only inside its session-swallow
    // branch, a set left behind cannot outlive a session - "the restore
    // never ran and the player is frozen" is now structurally impossible
    // rather than merely guarded against.
    //
    // Post() is safe from any thread; the work happens in an SKSE task
    // (game thread, FIFO - the last posted request wins).
    class MovementGuard {
    public:
        static void Post(bool blocked);  // any thread; no-op if disabled in INI

        // Log the actual key BINDINGS (game thread only).
        //
        // Every probe in the 2026-07-20/21 hunt measured whether controls
        // were ENABLED; none asked whether W was still BOUND to Forward.
        // This line is what proved the binding table innocent - it read
        // fwd=0x11 back=0x1f left=0x1e right=0x20 jump=0x39 on every run,
        // correct throughout, including while the player was frozen.
        //
        // That is now doubly load-bearing: those reads come from
        // `controlMap[]` at +0x60, so they are also the evidence that the
        // ControlMap POINTER is sound and only the members past that array
        // are misplaced. Keep this logging.
        static void LogBinds(const char* a_when);
    };
}
