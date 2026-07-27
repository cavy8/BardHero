// src/game/Session.h
#pragma once

namespace SH {

    // Phase-1 minimal session driver (spec 10): debug hotkey starts the
    // first scanned song; master clock + audio + slave controller + drift
    // log; auto-pause on menu open / focus loss / stale audio; auto-abort
    // on save load / cell change under a loading screen. Runs a 5ms session
    // thread (own COM MTA - never Skyrim's). M3: engine feeding lives on
    // the game thread (InputHook via EngineFeed); this thread keeps only
    // clock/controller/drift work.
    class Session {
    public:
        static Session& GetSingleton();
        void Install();        // event sink + session thread; once, kDataLoaded
        void OnPreLoadGame();  // save load mid-session -> abort (spec 10)
        // kPostLoadGame AND kNewGame: drop per-timeline state a load
        // invalidates. kNewGame never reaches OnPreLoadGame.
        void OnPostLoadGame();

        // Game-thread callers (the input hook's pause bind). Only requests -
        // the session thread owns every state transition.
        static void RequestPause();
        // Pause-key toggle (field round 5, guitar Plus): the hook captures
        // the pause binds while a session is PAUSED and requests the
        // resume; same guards as the start-key resume apply.
        static void RequestResume();
        // Pause-menu "Quit" (field round 6). Same as the abort key.
        static void RequestAbort();
        // Pause-menu "Restart". Regular play starts the same pick over and
        // records NOTHING for the abandoned attempt; practice restarts its
        // range through the loop's own path.
        static void RequestRestart();
        // Pause-menu practice toggle. Restarts the same pick in the other
        // mode - a live session cannot be converted in place, because
        // practice runs on a sliced chart.
        static void RequestPracticeToggle();

        // Input-hook recovery gate. True only for a non-passive native menu
        // that opened during the active BardHero session; while true, input
        // is yielded so the same vanilla shortcut can always close it.
        static bool NativeMenuOpen();

        // Is this FormID one of the three configured SGT perform abilities?
        // Safe from any thread and from hook context: it only reads the
        // resolved-once atomics. Used by the Phase 0 trigger spike.
        static bool IsPerformSpell(RE::FormID a_id);

        // Game thread, from the MagicTarget::AddTarget hook: an SGT
        // perform ability was just added to the player. Arms the session
        // start when this is a fresh arming - the 500ms poll stays as a
        // belt, and the shared latch means only one of them fires.
        static void NotePerformAbilityAdded(RE::FormID a_id);

        // True while BardHero is itself adding the perform ability (song
        // pick, pause-resume). Native start must not strip those - doing so
        // killed the performance in the same millisecond it started, and
        // the player never played the lute (field 2026-07-20).
        static bool IsSelfAddInFlight();
    };
}
