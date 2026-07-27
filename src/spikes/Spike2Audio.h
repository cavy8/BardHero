#pragma once

namespace SH::Spike2 {
    // Arms the audio spike (bSpike2Audio=1): inits miniaudio (WASAPI shared,
    // own thread), sample-sync-starts 4 ogg stems, logs callback-period +
    // anchor-jitter stats, then tears down. Runs ONCE per process via
    // whichever trigger fires first.
    void OnSaveLoaded();  // 5s after kPostLoadGame / kNewGame
    void ArmAtMenu();     // 20s after kDataLoaded - no save needed; the main
                          // menu's own music makes a fine coexistence test
}
