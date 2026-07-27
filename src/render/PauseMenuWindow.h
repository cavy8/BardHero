// src/render/PauseMenuWindow.h
#pragma once

namespace SH {
    // Session pause menu (field round 6): shown while the session is
    // paused (UiBus::pauseMenuOpen, session-thread owned). Resume / Quit
    // song, navigable by strum + green fret / Plus (UiBus nav intents
    // harvested by the InputHook) or the mouse.
    void RegisterPauseMenuWindow();
}
