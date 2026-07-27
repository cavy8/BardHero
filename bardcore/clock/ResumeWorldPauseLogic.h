#pragma once

// Pure ownership policy for Skyrim's world-pause latch during a custom
// pause/resume cycle. The session runtime consumes this at both edges; the
// Clock suite protects the boundary independently of RE/SKSE.
namespace SH::resume_world {
    enum class Phase {
        kPauseMenu,
        kCountdown,
        kAwaitingAudioAnchor,
        kPlaying,
        kTeardown,
    };

    [[nodiscard]] constexpr bool ShouldPause(Phase a_phase) {
        return a_phase == Phase::kPauseMenu ||
               a_phase == Phase::kCountdown ||
               a_phase == Phase::kAwaitingAudioAnchor;
    }
}
