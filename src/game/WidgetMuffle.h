#pragma once

namespace SH::WidgetMuffle {
    // Hide the configured third-party widget menus for the session that is
    // starting. Posts to the game thread; safe from the session thread.
    void OnSessionStart();

    // Re-assert every active hide. Call when the resume countdown
    // completes: closing the pause menu makes menu-churn widget mods
    // (TrueHUD) re-show their movies, and by countdown-complete the menu
    // is long closed so the re-hide cannot lose that race. Idempotent
    // over the hidden set; no-op outside a session.
    void OnSessionResume();

    // Restore exactly what OnSessionStart hid, to each widget's own INI
    // config state. Idempotent - every session end path and OnPreLoadGame
    // may call it freely.
    void OnSessionEnd();
}
