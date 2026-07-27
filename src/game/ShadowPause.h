#pragma once

#include <string_view>

namespace bard::ShadowPause {
    // A REAL pausing menu that neither Skyrim Souls nor FUCK knows about
    // (Menu Studio ShadowPause precedent, ported 2026-07-25).
    //
    // The session's world pause was a manual RE::UI::numPausesGame bump.
    // That freezes Main::Update and the Papyrus VM, but it never runs the
    // engine's MENU-pause bookkeeping - which is what pauses in-flight
    // voice lines and resumes them afterwards. Field report: NPC dialogue
    // uttered before the pause kept playing through it, while the Journal
    // and console (real kPausesGame menus) pause those lines mid-word.
    //
    // So we bring our own real menu: registered with kPausesGame and no
    // Scaleform movie, shown/hidden through UIMessageQueue. The ENGINE
    // then owns numPausesGame and all pause side effects, exactly like a
    // vanilla paused menu.
    //
    // Verified against the shipped binaries before porting:
    // - Skyrim Souls works from a fixed list of vanilla menu names in
    //   SkyrimSoulsRE.ini; it never touches an unknown menu.
    // - FUCK.dll's kCloseOnGameMenu works from a fixed whitelist of
    //   vanilla names (RaceSex/Book/Lockpicking/.../ContainerMenu cluster
    //   in the binary), so this menu does NOT hide our kCloseOnGameMenu
    //   FLICK windows (the pause menu window carries that flag).
    // - Session's MenuSink exempts this menu by name: it must never count
    //   as a native menu (g_nativeMenus) or trigger the pausing-menu
    //   session-pause request against itself.
    void Register();  // once, at kDataLoaded (needs RE::UI)
    void Show();      // idempotent; game thread
    void Hide();      // idempotent; game thread
    [[nodiscard]] bool IsUp();
    [[nodiscard]] bool IsAvailable();  // registration succeeded

    inline constexpr std::string_view kMenuName{ "BardHero_ShadowPause" };
}
