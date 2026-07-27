#pragma once

#include "game/BandFormationLogic.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace bard::band {
    inline constexpr double kFirstPerformancePulseDelay = 0.0;
    inline constexpr double kPerformancePulseSeconds = 0.35;
    inline constexpr std::uint64_t kSingerCuePulseInterval = 12;
    // 0_master.hkx declares the graph booleans `bHeadTracking` and
    // `bHeadTrackSpine` (verified 2026-07-25 by string-scanning the live
    // Nemesis-patched graph; `bHeadTrackingOn` does NOT exist). The
    // `HeadTrackingOff` EVENT is also declared there, but the 14:57 field
    // run rejected it on every performer on every pulse while idle events
    // were accepted, so the variables are the seam that works. Dialogue can
    // re-enable the modifier, so the host re-clears them on every pulse.
    inline constexpr std::string_view kHeadTrackingVariable =
        "bHeadTracking";
    inline constexpr std::string_view kHeadTrackSpineVariable =
        "bHeadTrackSpine";
    // Field 2026-07-25 (21:36 log): the first clear attempt runs ~14ms
    // after conjuring, before the behavior graph is loaded, so it always
    // reports false; latching the log on attempt 1 made the
    // `cleared=true` field tell unobservable while later pulses silently
    // succeeded. Log the first SUCCESS instead, with a fallback at this
    // pulse (~3s in at the 0.35s cadence) so a genuinely failing clear
    // still surfaces as `cleared=false`.
    inline constexpr std::uint64_t kHeadTrackingLogFallbackPulse = 8;

    [[nodiscard]] inline constexpr bool ShouldLogHeadTrackingClear(
        bool a_alreadyLogged,
        bool a_cleared,
        std::uint64_t a_pulse) noexcept {
        return !a_alreadyLogged
            && (a_cleared || a_pulse >= kHeadTrackingLogFallbackPulse);
    }
    // Pulses are queued as game-thread tasks. After a load or menu churn the
    // task queue drains its backlog in one frame (the 14:57 field run ran
    // ~12 pulses within 3ms), which collapses every "one pulse later"
    // sequence: portal-before-reveal and the bassist's capture window on the
    // shared AnimObjectLute. A pulse arriving sooner than this spacing is
    // backlog, not cadence, and is dropped; the next scheduled pulse is at
    // most kPerformancePulseSeconds away.
    inline constexpr double kMinPulseSpacingSeconds = 0.25;

    // ---- WHY PERFORMERS SPAWN BELOW THE STAGE ---------------------------
    //
    // The engine fades a freshly spawned actor IN, overriding any alpha we
    // set until it finishes - measured twice on 2026-07-27 (visible
    // skeletons at spawn; a 50ms alpha re-assert poll read as stutter). So
    // visibility is solved with GEOMETRY: performers spawn this far below
    // their stage mark, where nothing renders whoever wins the alpha
    // fight, and are lifted on the arrival beat (kArrivalWarmupPulses).
    // Depth clears any playable-race model plus thin floors.
    inline constexpr float kSpawnConcealmentDepth = 1000.0f;

    // ---- WHY THE BAND'S ARRIVAL IS LOUD, AND WHY IT STAYS THAT WAY -------
    //
    // All four performers reach the portal step in lockstep, so all four
    // vanilla conjuration sounds start in the SAME frame - four copies of one
    // sample, perfectly in phase, summing to roughly +12 dB over a single
    // one. It is a STACKING effect, not a volume one: no individual summon is
    // too loud, which is why turning one down is the wrong lever.
    //
    // A one-summon-per-pulse cascade was built and REVERTED on 2026-07-27 at
    // the owner's call: the band arriving together is the look that is
    // wanted, and it is worth the noise. Do not "fix" this back into a
    // stagger without asking.
    //
    // If the volume needs solving again, the levers are: duck a sound
    // category across the arrival window (blunt - it takes other SFX with
    // it), or use a quieter art object. The sound is baked into the art
    // object's NIF, so unlike the fire cloak's `ambientSound` there is no
    // form field to null.

    // Real time the bassist must hold the shared AnimObjectLute before the
    // rhythm guitarist may re-point its model at the V guitar. Measured in
    // seconds because a pulse count proved meaningless under backlog: the
    // old one-pulse gate re-pointed the model ~2ms after the bassist's
    // idle, and the bassist's captured instrument became a coin flip.
    inline constexpr double kSharedPropCaptureSeconds = 0.30;

    [[nodiscard]] inline constexpr bool PulseIsBacklog(
        bool a_anyPulseExecuted,
        double a_secondsSinceLastPulse) noexcept {
        return a_anyPulseExecuted
            && a_secondsSinceLastPulse < kMinPulseSpacingSeconds;
    }

    // Field 2026-07-25: the ghost shader was instantiated on the first
    // pulse, ~4 seconds BEFORE the actor's 3D existed, so it attached to
    // nothing and every skeleton stayed bone-opaque (two screenshots
    // prove it; the "spectral shader applied" log line only proved the
    // call ran). The membrane must wait for loaded 3D AND the started
    // performance, so the attached instrument is covered by the same
    // shader as its player.
    // a_instrumentSettled: the performance started (instrument attached)
    // OR the role is not live and never will attach one - a dead role
    // must still look like an apparition, not opaque bone.
    [[nodiscard]] inline constexpr bool GhostShaderShouldApply(
        bool a_alreadyApplied,
        bool a_has3d,
        bool a_instrumentSettled) noexcept {
        return !a_alreadyApplied && a_has3d && a_instrumentSettled;
    }

    // Apparition translucency at reveal. 1.0 was fully opaque bone; the
    // ghost look needs real alpha in addition to the membrane shader.
    inline constexpr float kApparitionAlpha = 0.8f;

    // How long the arrival effects run on a performer - the vanilla portal
    // art and the wisp shader both use this. Extracted from a literal so
    // the reveal fade below can be PROVEN to fit inside it.
    inline constexpr float kArrivalEffectSeconds = 2.0f;

    // ---- THE TIMING KNOB: WARM-UP PULSES BEFORE THE ARRIVAL BEAT --------
    //
    // Pulses (~0.35s each) a performer waits below the stage after its 3D
    // loads before appearing in ONE beat - position, alpha, art, shader
    // and sound on the same pulse, the way vanilla presents a summon.
    //
    // ONE beat because two is impossible: attached hit art INHERITS the
    // actor's alpha (measured 2026-07-27 - hiding the actor mid-portal
    // vanished the portal with it), and alpha set during the engine's
    // spawn fade-in does not hold anyway. Do not rebuild "portal first,
    // performer second", a fade ramp, or a graph-readiness gate; all
    // three were built and reverted that night.
    //
    // ZERO is the owner's decision ("i don't want any delay", 2026-07-27),
    // accepting two cosmetic risks: a possible bind-pose flash, and a
    // solid-then-ghostly beat until the ghost-membrane pulse re-pins the
    // alpha. Do not raise it without the owner asking. The ~0.3s the
    // engine takes to stream the actor's 3D is the only remaining latency
    // and is not removable from plugin code.
    inline constexpr std::uint32_t kArrivalWarmupPulses = 0;

    [[nodiscard]] inline constexpr bool ArrivalDue(
        std::uint32_t a_warmupPulses) noexcept {
        return a_warmupPulses >= kArrivalWarmupPulses;
    }

    [[nodiscard]] inline constexpr bool RhythmMustWaitForBass(
        bool a_bassLive,
        bool a_bassStarted,
        double a_secondsSinceBassStart) noexcept {
        // A bass role that will never start (no dedicated bass stem) must
        // not block the rhythm guitarist forever.
        if (!a_bassLive) { return false; }
        if (!a_bassStarted) { return true; }
        return a_secondsSinceBassStart < kSharedPropCaptureSeconds;
    }

    using EventCandidates = std::array<std::string_view, 3>;

    enum class PropKind {
        kNone,
        kBassAnimationObject,
        kGuitarAnimationObject,
        kNativeDrumAnimationObject,
        kMicrophoneAnimationObject,
    };

    struct StemAvailability {
        bool anyRoleDedicated{ false };
        bool mixedBacking{ false };
        bool bass{ false };
        bool rhythm{ false };
        bool drums{ false };
        bool vocals{ false };

        constexpr void Note(std::string_view a_name) noexcept {
            const bool isBass = a_name == "bass";
            const bool isRhythm = a_name == "rhythm";
            const bool isDrums = a_name == "drums"
                || a_name.starts_with("drums_");
            const bool isVocals = a_name == "vocals"
                || a_name.starts_with("vocals_");
            bass = bass || isBass;
            rhythm = rhythm || isRhythm;
            drums = drums || isDrums;
            vocals = vocals || isVocals;
            // Guitar-only Clone Hero imports still contain a mixed backing
            // arrangement. Only stems that name one of our backing roles
            // prove that role-level separation exists.
            anyRoleDedicated = anyRoleDedicated || isBass || isRhythm
                || isDrums || isVocals;
            // Clone Hero's `song` stem is the catch-all backing mix:
            // everything WITHOUT a dedicated stem lands in it. Its
            // presence means an uncovered role may still be audible.
            mixedBacking = mixedBacking || a_name == "song";
        }
    };

    [[nodiscard]] inline constexpr bool RoleIsLive(
        Role a_role,
        const StemAvailability& a_stems) noexcept {
        // A single mixed stem contains no trustworthy separation metadata.
        // Preserve the full ensemble rather than inventing which musicians
        // are absent.
        if (!a_stems.anyRoleDedicated) { return true; }
        switch (a_role) {
        case Role::kBassist:
            if (a_stems.bass) { return true; }
            break;
        case Role::kRhythmGuitarist:
            if (a_stems.rhythm) { return true; }
            break;
        case Role::kDrummer:
            if (a_stems.drums) { return true; }
            break;
        case Role::kSinger:
            if (a_stems.vocals) { return true; }
            break;
        }
        // Strictness is only honest when the separation is COMPLETE. Field
        // 2026-07-25: every Bridge download that idled a musician shipped a
        // `song` backing mix that audibly contained the "missing" role
        // (You Shook Me All Night Long has no `rhythm` stem but Malcolm is
        // in song.opus; the Harmonix Slipknot stems have no `bass` but the
        // bass is in song.opus). Without a dedicated stem, a role is dead
        // only when there is no backing mix it could hide in.
        return a_stems.mixedBacking;
    }

    [[nodiscard]] inline constexpr PropKind PerformanceProp(Role a_role) {
        switch (a_role) {
        case Role::kBassist:
            return PropKind::kBassAnimationObject;
        case Role::kRhythmGuitarist:
            return PropKind::kGuitarAnimationObject;
        case Role::kDrummer:
            return PropKind::kNativeDrumAnimationObject;
        case Role::kSinger:
            return PropKind::kMicrophoneAnimationObject;
        }
        return PropKind::kNone;
    }

    [[nodiscard]] inline constexpr bool UsesEquippedWeaponProp(Role) {
        return false;
    }

    [[nodiscard]] inline constexpr bool ShouldAttemptPerformance(
        Role a_role,
        bool a_alreadyStarted,
        std::uint64_t a_pulse) {
        if (!a_alreadyStarted) { return true; }
        return a_role == Role::kSinger
            && a_pulse > 0
            && a_pulse % kSingerCuePulseInterval == 0;
    }

    [[nodiscard]] inline constexpr EventCandidates PerformanceEvents(
        Role a_role,
        std::uint64_t a_pulse) {
        switch (a_role) {
        case Role::kBassist:
            return {
                "IdleLuteStart",
                "IdleDefaultStart",
                "IdleForceDefaultState" };
        case Role::kRhythmGuitarist:
            return {
                "IdleLuteStart",
                "IdleDefaultStart",
                "IdleForceDefaultState" };
        case Role::kDrummer:
            return {
                "IdleDrumStart",
                "IdleDefaultStart",
                "IdleForceDefaultState" };
        case Role::kSinger:
            switch (
                (a_pulse / kSingerCuePulseInterval) % 4) {
            case 0:
            case 2:
                return {
                    "IdleDrinkPotion",
                    "DialogueHappy",
                    "DialogueNeutral" };
            case 1:
                return {
                    "IdleCiceroDance1",
                    "DialogueHappy",
                    "DialogueNeutral" };
            default:
                return {
                    "IdleCiceroDance2",
                    "DialogueNeutral",
                    "DialogueHappy" };
            }
        }
        return {};
    }
}
