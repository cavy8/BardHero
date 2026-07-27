// src/game/StreakFireLogic.h
#pragma once

// PURE streak-fire model (no RE/OS includes - headless-tested by
// StreakFireTests). Guitar Hero lights the player up as a note streak
// builds. This owns WHEN; Session.cpp owns the art.
//
// ---- A SUSTAINED CLOAK, ON AN EFFECT SHADER ----------------------------
//
// The player wears fire for as long as the streak holds, and it goes out
// when the streak breaks. That is the feature as asked for, twice, and it
// is what this file now models.
//
// It took six field rounds to get here, and the reason is worth keeping
// because it constrains anything built on top:
//
//  1. ART OBJECT (ARTO `FireCloakHandEffects`), sustained on a 0.8s loop.
//     Reported as visible, and it made a looping fire sound.
//  2. Effect shaders, same cadence. Reported as nothing visible.
//  3. ARTO, one burst per milestone, attached to the HAND nodes. Nothing,
//     with the log proving `2/2` nodes attached - so the attach worked and
//     drew nothing anyway.
//  4. ARTO, one burst, actor root. Nothing.
//  5. ARTO, 15 applications over 5 milestones. Nothing. This refuted the
//     whole "one instantiation is not enough" theory outright, and with it
//     the attach node, the cadence and the count.
//  6. EFFECT SHADER `FireCloakFXShader`, plus a control that fired
//     SummonTargetFX (art known to render on the band) at the PLAYER.
//     Field: the swirl AND the flames were both visible. That settled two
//     things at once - effects do render on the player, and the shader is
//     the mechanism that works here. The art object never was.
//
// So the ARTO is gone for good, and with it the sound problem: an EFSH
// record carries ICON/ICO2/NAM7-9 texture paths and a DATA blob and NOTHING
// ELSE. There is no sound field on an effect shader - verified by dumping
// both record types, not assumed. The cloak is SILENT BY CONSTRUCTION,
// which is what makes a sustained design safe now when it was not before.
//
// ---- THE ONE INVARIANT THAT MATTERS ------------------------------------
//
// The shader is handed a DURATION and re-applied before it expires. If the
// refresh interval is ever >= the hold duration, a held streak GAPS on
// every cycle - the cloak visibly stutters. That exact bug shipped once,
// with a 1.60 refresh against a 1.20 duration, because the two numbers
// lived in different files. They live in one struct here so the
// relationship is assertable, and StreakFireTests asserts it.
//
// The fire is not stopped, it is allowed to LAPSE. The only cancel also
// kills every other shader on the actor, which on the player could be an
// active spell. A break stops the refresh and the flames die within one
// hold duration.

namespace SH::streakfire {

    enum class Action {
        kNone,
        // Apply the cloak shader now. Sent both when the cloak first lights
        // and on every refresh while it holds - the caller does the same
        // thing either way, so there is no reason to distinguish them.
        kApply,
    };

    struct Params {
        // Combo at which the cloak lights. 30 is roughly two bars of steady
        // play at moderate density - earned, but seen more than once a song.
        int firstAt = 30;
        // Reserved for a second, fiercer tier. NOT WIRED: only one fire
        // shader has ever been confirmed to render in the field, and
        // shipping an unproven second record would risk the 30-89 band
        // showing nothing at all. Kept because the setting exists and the
        // two-tier design is written down; wire it when a second record has
        // been SEEN, not before.
        int bigAt = 90;
        // Seconds handed to the shader, and how often it is re-applied.
        // refreshSec MUST stay meaningfully below holdSec - see the header
        // note. The margin absorbs a poll that lands late.
        double holdSec    = 1.60;
        double refreshSec = 0.80;
    };

    // 0 disables the feature outright. It cannot mean "light at a combo of
    // zero", which is what a bare threshold test would make it - the player
    // would catch light on the first note of every song.
    [[nodiscard]] constexpr bool Enabled(const Params& a_p) noexcept {
        return a_p.firstAt > 0;
    }

    // Should the cloak be burning at this combo?
    [[nodiscard]] constexpr bool LitAt(int a_combo,
                                       const Params& a_p) noexcept {
        return Enabled(a_p) && a_combo >= a_p.firstAt;
    }

    class State {
    public:
        void Reset() {
            _lit       = false;
            _nextApply = 0.0;
        }

        State() { Reset(); }

        [[nodiscard]] bool Lit() const { return _lit; }

        // a_t is the session clock. Poll as often as you like - the cadence
        // is decided here, not by the caller's poll rate.
        Action Step(int a_combo, double a_t, const Params& a_p) {
            const int combo = a_combo < 0 ? 0 : a_combo;
            if (!LitAt(combo, a_p)) {
                // Out. No stop call - the shader lapses within one hold.
                _lit = false;
                return Action::kNone;
            }
            if (!_lit) {
                _lit       = true;
                _nextApply = a_t + a_p.refreshSec;
                return Action::kApply;
            }
            // A seek rewinds the session clock, and practice loops seek
            // constantly. Without this the next refresh sits in a future
            // that the rewound clock has to climb back to, and the cloak
            // gaps for exactly as long as the seek jumped back.
            const bool clockWentBack = a_t + a_p.refreshSec < _nextApply;
            if (a_t >= _nextApply || clockWentBack) {
                _nextApply = a_t + a_p.refreshSec;
                return Action::kApply;
            }
            return Action::kNone;
        }

    private:
        bool   _lit{ false };
        double _nextApply{ 0.0 };
    };
}
