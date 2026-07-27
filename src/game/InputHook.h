// src/game/InputHook.h
#pragma once

#include <cstdint>

namespace SH {
    // M3 input pipeline (spec 7, path 7.1): write_call<5> at the
    // PollInputDevices dispatch site. Each frame, on the game thread:
    // DI-buffer timestamp read -> InputMapper -> engine feed (under the
    // EngineFeed lock), plus selective unlinking of bound keys from the
    // InputEvent list while a session is engaged. Pass-through when idle.
    class InputHook {
    public:
        static void Install();  // kDataLoaded; byte-verifies the site (E8)

        // Hook-side counters for the post-session probe (2026-07-20). Every
        // probe so far has measured the GAME's state and every layer read
        // healthy, including in a standalone run where SGT's script never
        // ran at all - so the lock is not in anything the game owns. The one
        // component present in every locked run and never instrumented is
        // THIS hook, which rewires the engine's intrusive InputEvent list at
        // the same site FLICK and Fitting Room chain through.
        //
        // Passthrough = frames forwarded untouched. Filtered = frames that
        // took the unlink path. If `filtered` keeps climbing after a session
        // has ended, the hook never went back to pass-through and that is
        // the lock. If BOTH are frozen, the hook is not being called at all,
        // which points at the chain above us instead.
        static unsigned long PassthroughFrames();
        static unsigned long FilteredFrames();
        // last non-passthrough reason: -1 never / 0 none / 1 browser /
        // 2 results / 3 session swallow / 4 paused
        static int LastCapture();

        // Post-session pass-through EVENT trace (2026-07-20).
        //
        // The counters above proved the hook RUNS - passthrough climbs at a
        // steady frame rate after every session. But it counts FRAMES, and a
        // frame carrying zero input events increments it exactly like a frame
        // carrying a keypress. So "input is passing through" has never
        // actually been measured; only "the hook is being called" has. That
        // distinction is the last unexamined axis in the exit-lock hunt.
        //
        // Armed at session end with a budget of N events. NOTHING logged while
        // the player mashes keys means input dies ABOVE us - the device layer,
        // or a mod hooked ahead of us in the chain. Events logged while the
        // player is still frozen means the game receives input and declines to
        // act on it, and the fault is downstream of everything we own.
        static void ArmPassthroughTrace(int a_events);

        // Extra DIK scan codes to unlink while a session is engaged - the
        // movement keys, published by MovementGuard (2026-07-22).
        //
        // This replaced ControlMap::ToggleControls, which on AE 1.6.1170
        // writes contextPriorityStack::_size rather than enabledControls and
        // was emptying the engine's input context stack - see
        // `docs/evidence/2026-07-22-controlmap-offsets-are-wrong-on-ae.md`.
        //
        // The set is only ever consulted inside the session-swallow branch,
        // so a stale one cannot outlive a session. That makes the old
        // failure mode - a restore that never ran leaving the player frozen
        // - structurally impossible rather than merely guarded against.
        //
        // Callable from any thread; a_count 0 (or a_diks null) clears it.
        static void SetMovementSwallow(const std::uint32_t* a_diks,
                                       int                  a_count);

        // Bindings-tab live services (2026-07-27).
        //
        // Capture: BeginBindCapture arms the hook; the next down-edge on
        // the armed device is stored and the event swallowed, and while
        // armed EVERY event of that device is swallowed (a stray press
        // must not leak into the game - that is also what makes binding
        // Esc possible). Expiry is enforced HOOK-SIDE (5s): a poll-only
        // expiry would leave the hook eating the keyboard if the tool
        // window closed mid-listen. Mouse is untouched so the on-screen
        // Cancel stays clickable.
        enum class BindDevice : int { kNone = 0, kKeyboard = 1,
                                      kGamepad = 2 };
        static void BeginBindCapture(BindDevice a_device);
        static void CancelBindCapture();
        // Returns the captured code and clears it; -1 = nothing yet.
        static int  TakeCaptureResult();

        // Re-read every bind (both keyboard columns and gamepad) from
        // Settings on the hook's own thread at its next frame - nobody
        // else ever writes the bind state after install. Cheap; callable
        // from any thread. The movement-swallow mask needs no refresh:
        // MovementGuard publishes it per session (movement keys, not
        // bindings).
        static void RefreshBinds();
    };
}
