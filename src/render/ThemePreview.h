#pragma once

// What the theme editor wants on screen RIGHT NOW.
//
// The editor does not draw a preview and it does not open one either. It
// publishes a target here once per drawn frame, and the real windows - the
// Songbook, the results panel, the highway - each decide for themselves
// whether they are the one being previewed. That inversion is the whole
// point: the preview IS the shipped menu, drawn by the shipped code at its
// real on-screen size, so there is no second copy of a panel to keep in
// sync with the one players actually see.
//
// The heartbeat is the lifetime model. FLICK gives a tool no reliable "my
// page went away" callback covering every route out of it (tab switch, tool
// switch, menu close, hotkey, load screen), and a latched flag that
// survives any one of them strands a preview on screen with nothing driving
// it. So Keep() stamps a time and Is() expires: stop drawing the editor for
// any reason at all and every preview tears itself down within a few
// frames, without a single teardown path having to be correct.

#include <atomic>

#include "QpcClock.h"

namespace SH::theme_preview {
    enum class Target : int {
        kNone = 0,
        kSongbook,  // SongBrowserV3, the whole panel visual language
        kResults,   // ResultsV13, the color-heavy one
        kHighway    // HighwayV2 + its background layer, statically posed
    };

    // Generous against a frame stutter, short enough that leaving the page
    // drops the preview before the player can wonder why the Songbook is
    // open in front of them.
    inline constexpr double kHeartbeatSec = 0.5;

    namespace detail {
        inline std::atomic<int>    g_target{ 0 };
        inline std::atomic<double> g_stamp{ -1.0e9 };
    }

    // Called once per drawn frame by the theme tool, with whatever the
    // visible tab wants shown.
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
