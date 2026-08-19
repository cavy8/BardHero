// src/render/RenderUi.h
#pragma once

// Registration + shared UI helpers for the M4 FLICK windows. Cursor
// acquire/release is refcounted so results + browser can't fight over
// ForceCursor.

namespace SH::RenderUi {
    void Register();  // all M4 windows; call after FUCK::Connect succeeds

    // Re-apply the mutable theme palette to BardHero-owned UI/gameplay
    // colors. reloadHighwayImage releases/reloads the optional background
    // texture, so the FLICK theme editor can switch paths without a restart.
    void ApplyTheme(bool reloadHighwayImage = false);

    void AcquireCursor();
    void ReleaseCursor();
    // Live ForceCursor refcount, for the post-session lock probe. A latch
    // that never returns to 0 leaves the cursor forced, which routes input
    // to the UI and reads as a total input lock while every ControlMap flag,
    // handler and actor bit stays healthy.
    int CursorRefs();
}
