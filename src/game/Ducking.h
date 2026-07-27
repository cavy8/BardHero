// src/game/Ducking.h
#pragma once

// No sibling header in this repo relies on the plugin PCH for RE types
// (AudioEngine.h keeps RE:: out of its interface via pimpl; the spike
// headers don't need RE:: at all) - forward-declare here so this header
// still stands alone for tooling that doesn't force-include PCH.h.
namespace RE {
    class BSIMusicType;
}

namespace SH {

    // Session ducking (spec 8): DoApplyDuckingAttenuation on the manager's
    // current music + SetCategoryVolume(0) on the MUS/AMB sound categories.
    // Cached + restored; idempotent; MAIN THREAD ONLY (call via
    // SKSE::GetTaskInterface()). Every session-terminal path AND
    // kPreLoadGame call Restore() - the crash-guard is that category
    // volumes are runtime-only state, so a hard crash resets them with the
    // process; within-process abandonment is covered by the terminal-path
    // calls. The MUS category is zeroed unconditionally - it is the primary
    // mechanism; duckCurrentMusic/duckAmbience layer the track-ducking and
    // ambience refinements on top.
    class Ducking {
    public:
        static Ducking& GetSingleton();
        void Apply(bool duckCurrentMusic, bool duckAmbience);
        void Restore();
        bool Applied() const { return _applied; }

    private:
        bool               _applied = false;
        float              _musVol = 1.0f, _ambVol = 1.0f;
        bool               _musDucked = false;
        bool               _ambDucked = false;
        RE::BSIMusicType*  _duckedMusic = nullptr;
    };
}
