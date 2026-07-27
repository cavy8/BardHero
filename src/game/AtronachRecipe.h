// src/game/AtronachRecipe.h
#pragma once

namespace SH::AtronachRecipe {
    // Registers the Doom Lute's Atronach Forge recipe by APPENDING to the
    // vanilla forge lists at runtime. Safe to call when the Electric addon
    // is absent - it no-ops. Call at kDataLoaded.
    void Install();

    // True when the addon is installed and its Doom Lute record resolves.
    // A hash lookup, so calling it once per UI frame is fine.
    bool DoomLuteAvailable();

    // Cheat: puts one Doom Lute straight into the player's inventory,
    // skipping the forge. No-ops when the addon is absent.
    //
    // Safe to call from the UI/render thread - the inventory change itself
    // is deferred to an SKSE task, the same way GoldScale defers gold.
    void GiveDoomLuteToPlayer();
}
