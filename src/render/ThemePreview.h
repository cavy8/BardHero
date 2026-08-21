#pragma once

// The editor publishes a preview target; the real BardHero windows render it.
// Keep() uses a heartbeat because FLICK has no universal page-close callback.

#include <atomic>

#include "QpcClock.h"

namespace SH::theme_preview {
    enum class Target : int {
        kNone = 0,
        kSongbook,
        kResults,
        kHighway
    };

    // Allow a brief frame stutter without leaving stale previews onscreen.
    inline constexpr double kHeartbeatSec = 0.5;

    namespace detail {
        inline std::atomic<int>    g_target{ 0 };
        inline std::atomic<double> g_stamp{ -1.0e9 };
    }

    // Called once per frame by the theme tool.
    inline void Keep(Target a_target) {
        detail::g_target.store(static_cast<int>(a_target),
                               std::memory_order_relaxed);
        detail::g_stamp.store(QpcSec(), std::memory_order_release);
    }

    inline Target Active() {
        const double stamp = detail::g_stamp.load(std::memory_order_acquire);
        if (QpcSec() - stamp > kHeartbeatSec) return Target::kNone;
        return static_cast<Target>(
            detail::g_target.load(std::memory_order_relaxed));
    }

    inline bool Is(Target a_target) {
        return a_target != Target::kNone && Active() == a_target;
    }
}
