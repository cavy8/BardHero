// src/game/CameraDirectorLogic.h
#pragma once

// PURE performance-camera director (no RE/OS includes - headless-tested by
// CameraDirectorTests). Spec:
// docs/superpowers/specs/2026-07-26-performance-camera-director-design.md
//
// Takes a per-frame snapshot the applier builds from state the session
// already publishes, and returns the camera values to write plus whether a
// cut happened. It owns the current shot, the cut clock and the per-song
// blacklist; it touches no shared container and does no I/O.
//
// ---- WHAT SPIKE 1 TOOK AWAY ---------------------------------------------
//
// The spec's shot model opened with a `Pivot` (kPlayer, kSinger, kBassist,
// kRhythm, kDrummer, kStageCentre) so the director could frame one band
// member. That is GONE, and its absence is the single most important thing
// to understand before extending this file.
//
// Moving the orbit pivot means writing ThirdPersonState::posOffsetExpected,
// and the 2026-07-26 spike proved that field is not a control input: the
// engine recomputes it inside Update, so a value written ahead of the
// original is discarded, and posOffsetActual glides toward its target on an
// exponential ease rather than snapping - so even if the write held, it
// could never express a CUT. Do not reintroduce per-member framing without
// re-running that spike; the measured outcome is recorded in the spec.
//
// Every shot here is therefore an orbit around the player, expressed as a
// yaw and pitch relative to the STAGE HEADING plus a camera distance. That
// still covers most of the Guitar Hero vocabulary, because the band is a
// fixed 210-unit line centred on the player (BandFormationLogic.h): a wide
// low yaw along that line frames the whole ensemble, and a tight yaw frames
// the player alone. The framing comes from the orbit, not from the pivot.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace SH::camdir {

    // ---- the shot library ------------------------------------------------
    //
    // Ids are stable and are what the cut log prints; the field gate asks
    // for "the director engaging with a shot identifier", so these names
    // are part of the diagnostic surface, not just an enum.
    // ---- WHICH WAY IS FRONT ----------------------------------------------
    //
    // yawDeg is the camera's orbit offset from the PLAYER'S FACING, so 0 is
    // the camera BEHIND the player looking at their back, and 180 is the
    // camera in front looking back at their face. The band is a fixed
    // 210-unit line beside the player, all facing the same way
    // (BandFormationLogic.h), so a shot only sees the ensemble's FACES from
    // somewhere near 180.
    //
    // The first library got this backwards without noticing: five of its six
    // shots sat within 52 degrees of 0, which is to say behind everyone.
    // Field 2026-07-26: "majority feel like they are from the back". The
    // table below is deliberately front-WEIGHTED - five front, three back -
    // because the front is where the performance is.
    enum class ShotId {
        // Behind the player, looking past them at the room.
        kWideStage = 0,   // the fallback - see kFallback below
        kOverShoulderLeft,
        kOverShoulderRight,
        // In front, looking back at the band's faces.
        kFrontCentre,
        kFrontLeft,
        kFrontRight,
        kFrontLow,        // low hero angle, looking up at the performers
        kPushIn,          // tightest, just off centre
        kCount
    };

    inline constexpr int kShotCount = static_cast<int>(ShotId::kCount);

    // The one shot that is never blacklisted, so the pool can never empty.
    // It is the WIDEST shot deliberately: whatever is occluding a tight
    // angle, backing off is the move most likely to clear it.
    inline constexpr ShotId kFallback = ShotId::kWideStage;

    struct Shot {
        ShotId id{ ShotId::kWideStage };
        float  yawDeg{ 0.0f };    // relative to the stage heading
        float  pitchDeg{ 0.0f };  // positive looks down
        float  distance{ 1.0f };  // maps to targetZoomOffset, 0..1
        float  fovDelta{ 0.0f };
        float  driftYawDegPerSec{ 0.0f };  // slow push within the shot
        // 0 = wide, slow, band-focused. 1 = tight, low, urgent. This is the
        // ONLY thing performance weighting reads, which keeps the weighting
        // table one number per shot instead of a matrix.
        float  energy{ 0.0f };
    };

    // ⚠ DISTANCES ARE targetZoomOffset, WHERE THE GAME'S OWN RESTING VALUE
    // IS 0.0, NOT 0.5. The 2026-07-26 spike logged `zoom target=0.0
    // current=0.0` as the vanilla third-person baseline, so this scale is
    // "extra zoom out beyond normal third person", not "distance from 0 to
    // max". The first cut of this table read it as the latter and ran
    // 0.55-1.00, which put even the TIGHT shot further out than the game
    // ever puts the camera - field 2026-07-26, "the camera is always way too
    // zoomed out". Anything above ~0.45 here is a very long lens.
    // FOV is per shot, and it is doing real work rather than decoration: a
    // wide establishing shot on a WIDER lens exaggerates the depth of the
    // stage, and a tight shot on a LONGER one (negative delta) compresses
    // it, which is what makes two shots at a similar angle still read as
    // two different shots. Deltas are degrees on top of the player's own
    // worldFOV, so they ride whatever the player has configured instead of
    // fighting it. Star Power's push adds on top of these.
    [[nodiscard]] constexpr Shot ShotFor(ShotId a_id) {
        switch (a_id) {
            // ---- behind (3) --------------------------------------------
            case ShotId::kWideStage:
                return { ShotId::kWideStage, 0.0f, 12.0f, 0.42f,
                         6.0f, 1.5f, 0.05f };
            case ShotId::kOverShoulderLeft:
                return { ShotId::kOverShoulderLeft, -52.0f, 8.0f, 0.26f,
                         0.0f, 2.0f, 0.45f };
            case ShotId::kOverShoulderRight:
                return { ShotId::kOverShoulderRight, 52.0f, 8.0f, 0.26f,
                         0.0f, -2.0f, 0.45f };
            // ---- front (5) ---------------------------------------------
            // Distances are deliberately varied so these read as different
            // shots even where two of them sit inside the 30-degree rule:
            // the size change is what carries a near-axis cut (CutIsLegible).
            // Yaws stay CANONICAL in [-180, 180]. 200 and -160 are the same
            // camera, but only one of them is readable next to its
            // neighbours, and the well-formedness check enforces it.
            case ShotId::kFrontCentre:
                return { ShotId::kFrontCentre, 180.0f, 9.0f, 0.34f,
                         4.0f, 1.2f, 0.25f };
            case ShotId::kFrontLeft:
                return { ShotId::kFrontLeft, 140.0f, 7.0f, 0.22f,
                         0.0f, -1.6f, 0.50f };
            case ShotId::kFrontRight:
                return { ShotId::kFrontRight, -140.0f, 7.0f, 0.22f,
                         0.0f, 1.6f, 0.50f };
            case ShotId::kFrontLow:
                return { ShotId::kFrontLow, -170.0f, -10.0f, 0.10f,
                         -8.0f, -1.0f, 0.85f };
            case ShotId::kPushIn:
                return { ShotId::kPushIn, 165.0f, 4.0f, 0.08f,
                         -12.0f, 0.6f, 0.70f };
            case ShotId::kCount: break;
        }
        return { };
    }

    // Does this shot see the band's faces? Everything past a quarter turn
    // from the player's back is looking inward at the line.
    [[nodiscard]] constexpr bool IsFrontShot(const Shot& a_shot) noexcept {
        const float y = a_shot.yawDeg < 0.0f ? -a_shot.yawDeg : a_shot.yawDeg;
        const float wrapped = y > 180.0f ? 360.0f - y : y;
        return wrapped > 90.0f;
    }

    // ---- cut legibility --------------------------------------------------
    //
    // THE 30-DEGREE RULE, which is the actual film convention behind "the
    // next angle must not be too close to the current one" (field
    // 2026-07-26). Cutting between two angles that are nearly the same reads
    // as a jump - a glitch rather than a decision - because the viewer sees
    // the frame twitch instead of seeing a new point of view.
    //
    // The rule has two halves, and BOTH are needed. Move the camera at least
    // 30 degrees around the subject, OR change the shot SIZE substantially:
    // a cut straight down the same axis from wide to tight is a cut-in, and
    // it is perfectly legible precisely because the size change carries it.
    // Requiring only the angle would forbid the cut-in; requiring only the
    // size would allow a 6-degree nudge at the same distance.
    inline constexpr float kMinYawSepDeg  = 30.0f;
    inline constexpr float kMinSizeChange = 0.12f;

    // Shortest angular distance in degrees, 0..180. The wrap matters: the
    // behind-the-band shot sits past 160, so a naive subtraction makes it
    // look far from everything when it can be close going the other way.
    [[nodiscard]] constexpr float YawSeparation(float a_from,
                                                float a_to) noexcept {
        float d = a_to - a_from;
        while (d > 180.0f) { d -= 360.0f; }
        while (d < -180.0f) { d += 360.0f; }
        return d < 0.0f ? -d : d;
    }

    [[nodiscard]] constexpr bool CutIsLegible(const Shot& a_from,
                                              const Shot& a_to) noexcept {
        if (a_from.id == a_to.id) { return false; }
        if (YawSeparation(a_from.yawDeg, a_to.yawDeg) >= kMinYawSepDeg) {
            return true;
        }
        const float ds = a_to.distance - a_from.distance;
        return (ds < 0.0f ? -ds : ds) >= kMinSizeChange;
    }

    // ---- cadence and hold ------------------------------------------------
    //
    // A cut every four bars is a phrase, which is the unit Guitar Hero's own
    // camera moves on. Sections, where a chart has them, override it.
    inline constexpr int    kBarsPerCut  = 4;
    // No cut may land within this of the previous one. This is what stops an
    // event override chaining into a strobe: SP activation on the bar after
    // a scheduled cut is EXACTLY the case the spec calls out.
    inline constexpr double kMinHoldSec  = 1.20;
    // A shot the engine has been hard-clamping for this long is occluded.
    inline constexpr double kClampCutSec = 0.30;

    // ---- shake -----------------------------------------------------------
    //
    // Halved-and-then-some from the first cut (field 2026-07-26, "the camera
    // shake is a little too strong"). The reference is a camera OPERATOR
    // reacting to the music, not a handheld camera in an earthquake: at a
    // real distance a fifth of a degree is visible motion, and the beat
    // pulse runs continuously so it accumulates in the eye in a way a
    // one-off impulse does not.
    inline constexpr float  kBeatShakeDeg   = 0.20f;  // at full glory
    inline constexpr float  kImpulseDeg     = 0.85f;
    inline constexpr double kImpulseTauSec  = 0.28;
    inline constexpr float  kMaxShakeDeg    = 1.40f;  // hard bound
    inline constexpr float  kSpFovDelta     = 6.0f;
    inline constexpr double kSpFovTauSec    = 0.90;
    // ⚠ Effect timers are NEVER 0.0. Zero is a real song time (the first
    // note), so a zero sentinel fires every effect once at song start. The
    // codebase convention is a large negative epoch, clamped on read.
    inline constexpr double kNoEvent = -1.0e9;

    struct Snapshot {
        double songSec{ 0.0 };
        // Bar index at songSec, from the tempo map. Monotonic; the director
        // only ever compares it for change.
        int    barIndex{ 0 };
        // 0..1 within the current beat, for the beat-locked pulse. Taken
        // from the tempo map rather than a free-running timer so the motion
        // sits on the actual beat.
        float  beatPhase{ 0.0f };
        // Chart section index, or -1 when the chart carries no markers. Not
        // every chart has them, which is why the bar cadence exists.
        int    sectionIndex{ -1 };
        float  glory{ 0.5f };
        int    streak{ 0 };
        bool   spActive{ false };
        // The engine is hard-clamping the current shot THIS frame (the
        // applier compares collision-corrected distance against requested).
        bool   clamped{ false };
        // One-shot events. The applier raises these on the frame they occur.
        bool   spJustActivated{ false };
        bool   streakMilestone{ false };
        bool   finalNote{ false };
    };

    struct Output {
        ShotId id{ ShotId::kWideStage };
        float  yawDeg{ 0.0f };
        float  pitchDeg{ 0.0f };
        float  distance{ 1.0f };
        float  fovDelta{ 0.0f };
        bool   cut{ false };  // a cut landed on this step (log it)
    };

    // Where the pool wants to sit for a given performance. 0 pulls toward
    // wide/slow/band-focused, 1 toward tight/low/urgent. Exposed so the
    // weighting can be asserted directly, without going through the
    // selection hash.
    [[nodiscard]] constexpr float EnergyTarget(float a_glory,
                                               int a_streak) noexcept {
        const float glory = a_glory < 0.0f ? 0.0f
                          : a_glory > 1.0f ? 1.0f : a_glory;
        // A long streak is its own evidence of a strong performance, worth
        // up to a fifth of the range on top of glory. Capped so streak can
        // never override a collapsing meter.
        const float streak =
            a_streak <= 0 ? 0.0f
                          : (a_streak >= 100 ? 0.20f
                                             : 0.20f * (a_streak / 100.0f));
        const float t = 0.10f + 0.70f * glory + streak;
        return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }

    // Selection weight for one shot. Peaks where the shot's energy matches
    // the target and falls off linearly, never to zero - every shot keeps a
    // floor so a strong run does not lock the camera onto one angle.
    [[nodiscard]] constexpr float ShotWeight(float a_energy,
                                             float a_target) noexcept {
        const float d = a_energy > a_target ? a_energy - a_target
                                            : a_target - a_energy;
        return 1.0f + 3.0f * (1.0f - d);
    }

    class Director {
    public:
        void Reset() {
            _shot        = ShotFor(kFallback);
            _lastCutAt   = kNoEvent;
            _lastBarCut  = -1;
            _lastSection = -1;
            _cutCount    = 0;
            _blacklist   = 0;
            _clampedFrom = kNoEvent;
            _impulseAt   = kNoEvent;
            _spFovAt     = kNoEvent;
            _heldSec     = 0.0;
            _started     = false;
        }

        Director() { Reset(); }

        [[nodiscard]] ShotId Current() const { return _shot.id; }
        [[nodiscard]] int    CutCount() const { return _cutCount; }
        [[nodiscard]] bool   Blacklisted(ShotId a_id) const {
            return (_blacklist & Bit(a_id)) != 0;
        }

        Output Step(const Snapshot& a_s) {
            Output out;
            if (!_started) {
                _started    = true;
                _lastCutAt  = a_s.songSec;
                _lastBarCut = a_s.barIndex;
                _lastSection = a_s.sectionIndex;
                _shot       = ShotFor(kFallback);
                out.cut     = true;
                _cutCount   = 1;
            }
            _heldSec = a_s.songSec - _lastCutAt;

            // ---- occlusion: learn the room by trying it -------------------
            // A sustained clamp means the engine is pushing the camera in to
            // avoid geometry. Blacklist and cut away. The fallback is never
            // blacklisted, so the pool cannot empty.
            if (a_s.clamped) {
                if (_clampedFrom <= kNoEvent / 2.0) {
                    _clampedFrom = a_s.songSec;
                }
            } else {
                _clampedFrom = kNoEvent;
            }
            const bool occluded =
                _clampedFrom > kNoEvent / 2.0 &&
                (a_s.songSec - _clampedFrom) >= kClampCutSec &&
                _shot.id != kFallback;

            // ---- when may a cut land? ------------------------------------
            // Sections take PRECEDENCE over the bar cadence: a section
            // boundary always wants a cut, and it re-phases the bar counter
            // so the four-bar grid restarts with the new section rather than
            // drifting against it. A chart with no markers carries
            // sectionIndex -1 forever and falls through to bars alone.
            const bool sectionEdge =
                a_s.sectionIndex >= 0 && a_s.sectionIndex != _lastSection;
            const bool barEdge =
                a_s.barIndex >= _lastBarCut + kBarsPerCut;
            const bool eventEdge =
                a_s.spJustActivated || a_s.streakMilestone || a_s.finalNote;

            // The minimum hold is checked ONCE, here, and applies to every
            // route in. That is the whole point: an event override that
            // skipped it could chain into a strobe of cuts.
            const bool mayCut = _heldSec >= kMinHoldSec;
            if (mayCut && (sectionEdge || barEdge || eventEdge || occluded)) {
                if (occluded) {
                    _blacklist |= Bit(_shot.id);
                    _clampedFrom = kNoEvent;
                }
                const Shot next = Select(a_s);
                // Re-phase the cadence whether or not the shot changed, so
                // an exhausted pool does not re-evaluate every single frame.
                _lastBarCut = a_s.barIndex;
                // A "cut" to the shot we are already on is not a cut. It can
                // only happen when everything else is blacklisted and we are
                // parked on the unblacklistable fallback - reporting it
                // would violate the never-repeat invariant and would spam
                // the cut log once per phrase for the rest of the song.
                if (next.id != _shot.id) {
                    _shot      = next;
                    _lastCutAt = a_s.songSec;
                    _heldSec   = 0.0;
                    ++_cutCount;
                    out.cut    = true;
                }
            }
            // Tracked even when the hold swallowed the cut, so one section
            // cannot queue a cut that fires late into the next.
            if (a_s.sectionIndex >= 0) { _lastSection = a_s.sectionIndex; }

            if (a_s.spJustActivated) {
                _spFovAt   = a_s.songSec;
                _impulseAt = a_s.songSec;
            }
            if (a_s.streakMilestone || a_s.finalNote) {
                _impulseAt = a_s.songSec;
            }

            // ---- compose the frame ---------------------------------------
            const float drift = static_cast<float>(_heldSec) *
                                _shot.driftYawDegPerSec;
            const float shake = Shake(a_s);
            out.id       = _shot.id;
            out.yawDeg   = _shot.yawDeg + drift + shake;
            out.pitchDeg = _shot.pitchDeg + shake * 0.35f;
            out.distance = _shot.distance;
            out.fovDelta = _shot.fovDelta + SpFov(a_s);
            return out;
        }

        // Total shake in degrees for this snapshot. Bounded by kMaxShakeDeg
        // and decays to exactly zero, so a paused or finished song leaves no
        // residual motion.
        [[nodiscard]] float Shake(const Snapshot& a_s) const {
            const float glory = a_s.glory < 0.0f ? 0.0f
                              : a_s.glory > 1.0f ? 1.0f : a_s.glory;
            // Beat-locked, from the tempo map's phase rather than a free
            // timer - so it sits ON the beat even after a seek.
            constexpr float kTwoPi = 6.28318530717958647692f;
            const float beat =
                kBeatShakeDeg * glory *
                std::sin(kTwoPi * a_s.beatPhase);
            float impulse = 0.0f;
            if (_impulseAt > kNoEvent / 2.0) {
                const double age = a_s.songSec - _impulseAt;
                if (age >= 0.0) {
                    impulse = kImpulseDeg *
                              static_cast<float>(
                                  std::exp(-age / kImpulseTauSec)) *
                              std::sin(static_cast<float>(age) * 46.0f);
                }
            }
            const float total = beat + impulse;
            return total > kMaxShakeDeg    ? kMaxShakeDeg
                   : total < -kMaxShakeDeg ? -kMaxShakeDeg
                                           : total;
        }

    private:
        static constexpr std::uint32_t Bit(ShotId a_id) {
            return 1u << static_cast<int>(a_id);
        }

        [[nodiscard]] float SpFov(const Snapshot& a_s) const {
            if (_spFovAt <= kNoEvent / 2.0) { return 0.0f; }
            const double age = a_s.songSec - _spFovAt;
            if (age < 0.0) { return 0.0f; }
            // Held while SP is actually up, then decayed out - a kick that
            // expired mid-phrase would read as the FOV sagging.
            const float decay =
                a_s.spActive
                    ? 1.0f
                    : static_cast<float>(std::exp(-age / kSpFovTauSec));
            return kSpFovDelta * decay;
        }

        // Deterministic, and deliberately so: the same song and the same
        // performance trace must produce the same cut schedule, or a field
        // report cannot be reproduced. The cut counter is the only entropy,
        // run through a integer hash - no clock, no RNG state.
        [[nodiscard]] Shot Select(const Snapshot& a_s) const {
            const float target = EnergyTarget(a_s.glory, a_s.streak);
            float  weights[kShotCount] = { };
            float  total = 0.0f;
            // Two passes, because legibility is a PREFERENCE that must never
            // empty the pool. The first pass keeps only cuts that read as
            // cuts; if a blacklist has eaten every legible option, the
            // second takes anything that is not the current shot. A slightly
            // jumpy cut is a much smaller defect than a camera that stops
            // cutting for the rest of the song.
            for (int pass = 0; pass < 2 && total <= 0.0f; ++pass) {
                const bool legibleOnly = pass == 0;
                for (int i = 0; i < kShotCount; ++i) {
                    const auto id = static_cast<ShotId>(i);
                    // Never repeat the current shot, and never pick one this
                    // song has blacklisted.
                    if (id == _shot.id || Blacklisted(id)) { continue; }
                    const Shot cand = ShotFor(id);
                    if (legibleOnly && !CutIsLegible(_shot, cand)) {
                        continue;
                    }
                    weights[i] = ShotWeight(cand.energy, target);
                    total += weights[i];
                }
            }
            if (total <= 0.0f) {
                // Everything else is blacklisted or is the current shot.
                // The fallback is unblacklistable, so this can only mean we
                // ARE the fallback - hold rather than invent a shot.
                return _shot.id == kFallback ? _shot : ShotFor(kFallback);
            }
            std::uint32_t h = static_cast<std::uint32_t>(_cutCount) * 2654435761u;
            h ^= h >> 15;
            h *= 2246822519u;
            h ^= h >> 13;
            const float pick =
                total * (static_cast<float>(h % 100000u) / 100000.0f);
            float acc = 0.0f;
            for (int i = 0; i < kShotCount; ++i) {
                if (weights[i] <= 0.0f) { continue; }
                acc += weights[i];
                if (pick < acc) { return ShotFor(static_cast<ShotId>(i)); }
            }
            // Float accumulation can land a hair past the last boundary.
            for (int i = kShotCount - 1; i >= 0; --i) {
                if (weights[i] > 0.0f) {
                    return ShotFor(static_cast<ShotId>(i));
                }
            }
            return ShotFor(kFallback);
        }

        Shot          _shot{ };
        double        _lastCutAt{ kNoEvent };
        double        _clampedFrom{ kNoEvent };
        double        _impulseAt{ kNoEvent };
        double        _spFovAt{ kNoEvent };
        double        _heldSec{ 0.0 };
        int           _lastBarCut{ -1 };
        int           _lastSection{ -1 };
        int           _cutCount{ 0 };
        std::uint32_t _blacklist{ 0 };
        bool          _started{ false };
    };
}
