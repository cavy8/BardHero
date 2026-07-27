#include "../src/game/BandLifecycleLogic.h"
#include "../src/game/BandFormationLogic.h"
#include "../src/game/BandPerformanceLogic.h"

#include <cassert>
#include <array>
#include <iostream>

int main() {
    using bard::band::BeginDecision;
    using bard::band::ResourceDecision;
    bard::band::Lifecycle lifecycle;

    assert(
        bard::band::DecideResources(true, true, true, true, true)
        == ResourceDecision::kSpawnWithDrum);
    assert(
        bard::band::DecideResources(true, true, true, true, false)
        == ResourceDecision::kSpawnWithoutDrum);
    assert(
        bard::band::DecideResources(false, true, true, true, true)
        == ResourceDecision::kAbort);
    assert(
        bard::band::DecideResources(true, false, true, true, true)
        == ResourceDecision::kAbort);
    assert(
        bard::band::DecideResources(true, true, false, true, true)
        == ResourceDecision::kAbort);
    assert(
        bard::band::DecideResources(true, true, true, false, true)
        == ResourceDecision::kAbort);

    // Still streaming in: hidden, nothing plays, warm-up not even counting
    // (the host only counts pulses WITH 3D).
    const auto waitingArrival =
        bard::band::DecideArrival(false, false, true, true);
    assert(waitingArrival.hide);
    assert(!waitingArrival.reveal);
    assert(!waitingArrival.playArt);
    assert(!waitingArrival.playShader);
    // 3D loaded but the warm-up still running: stay below the stage. This
    // gap is the whole mechanism - it outlasts the engine's spawn fade-in
    // (so the arrival's one SetAlpha sticks) and gives the graph time to
    // drive bones (so no bind-pose frame).
    const auto warmingUp =
        bard::band::DecideArrival(false, true, true, false);
    assert(warmingUp.hide);
    assert(!warmingUp.reveal);
    assert(!warmingUp.playArt);
    assert(!warmingUp.playShader);
    // The arrival beat: EVERYTHING on this one pulse, on a visible
    // performer. Attached art inherits actor alpha (field 2026-07-27
    // 05:59: "they all go invisible the effect too"), so art on a hidden
    // pulse is art that vanishes - there is no two-beat version of this.
    const auto arrivalBeat =
        bard::band::DecideArrival(false, true, true, true);
    assert(!arrivalBeat.hide);
    assert(arrivalBeat.reveal);
    assert(arrivalBeat.playArt);
    assert(arrivalBeat.playShader);

    // ---- CONCEALMENT MUST ACTUALLY CONCEAL ------------------------------
    //
    // Performers spawn below their stage mark because nothing alpha-based
    // survives the engine's spawn fade-in (an alpha-reassert poll was tried
    // and produced visible STUTTER - see kSpawnConcealmentDepth). The depth
    // only works if it clears the actor model: too shallow and a skull
    // pokes through a thin tavern floor, which ships looking fine on a
    // thick-floored test stage and is only caught by bounding it here.
    static_assert(
        bard::band::kSpawnConcealmentDepth >= 512.0f,
        "concealment must clear any playable-race model (~128u) with room "
        "for thin floors");
    static_assert(
        bard::band::kSpawnConcealmentDepth <= 4096.0f,
        "concealment must stay near the player's loaded space, or the "
        "actor's 3D may never stream in and the arrival never fires");

    // ---- NOTHING EVER PLAYS ON A HIDDEN PERFORMER -----------------------
    //
    // Two field runs proved the two failure shapes: effects on a hidden
    // pulse are invisible or vanish mid-burn (04:28, 05:59), and a reveal
    // before the warm-up is a fade-in war or a bind pose (05:20, 05:50).
    // Exhaustive over the whole input space, because a single hand-picked
    // case is what let the first shape through.
    for (int revealed = 0; revealed < 2; ++revealed) {
        for (int has3D = 0; has3D < 2; ++has3D) {
            for (int hasArt = 0; hasArt < 2; ++hasArt) {
                for (int due = 0; due < 2; ++due) {
                    const auto d = bard::band::DecideArrival(
                        revealed != 0, has3D != 0, hasArt != 0,
                        due != 0);
                    // Effects only ever land on the reveal pulse itself.
                    assert(!(d.playShader && d.hide));
                    assert(!(d.playArt && d.hide));
                    assert(d.playShader == d.reveal);
                    assert(!(d.playArt && !d.reveal));
                    // No reveal before 3D and the warm-up - that is the
                    // A-pose window again.
                    assert(!(d.reveal && due == 0));
                    assert(!(d.reveal && has3D == 0));
                    // Hide and reveal are exclusive, and a revealed
                    // performer is left entirely alone.
                    assert(!(d.hide && d.reveal));
                    if (revealed != 0) {
                        assert(!d.hide && !d.reveal && !d.playArt
                               && !d.playShader);
                    }
                }
            }
        }
    }

    // ---- THE TIMING KNOB ------------------------------------------------
    //
    // The arrival is due exactly at kArrivalWarmupPulses - never early,
    // and never never (an invisible band). Written to hold at ANY knob
    // value including zero, which is the current owner-chosen setting.
    assert(bard::band::ArrivalDue(bard::band::kArrivalWarmupPulses));
    assert(bard::band::ArrivalDue(bard::band::kArrivalWarmupPulses + 1));
    if constexpr (bard::band::kArrivalWarmupPulses > 0) {
        assert(!bard::band::ArrivalDue(0));
        assert(
            !bard::band::ArrivalDue(
                bard::band::kArrivalWarmupPulses - 1));
    } else {
        // Zero warm-up: due the moment the 3D exists, by the owner's
        // explicit call ("i don't want any delay", 2026-07-27). The
        // fade-in and bind-pose cosmetics this accepts are documented at
        // the constant - there is deliberately no lower bound here.
        assert(bard::band::ArrivalDue(0));
    }
    // Upper bound only: past ~2.8s the stage is dead air between the song
    // starting and anyone appearing on it.
    static_assert(
        bard::band::kArrivalWarmupPulses <= 8,
        "past ~2.8s of warm-up the stage is dead air");
    const auto noArtArrival =
        bard::band::DecideArrival(false, true, false, true);
    assert(noArtArrival.reveal);
    assert(!noArtArrival.playArt);
    assert(noArtArrival.playShader);
    const auto completedArrival =
        bard::band::DecideArrival(true, true, true, true);
    assert(!completedArrival.hide);
    assert(!completedArrival.reveal);
    assert(!completedArrival.playArt);

    assert(lifecycle.Begin(-1) == BeginDecision::kIgnore);
    assert(lifecycle.Begin(0) == BeginDecision::kIgnore);
    assert(lifecycle.Begin(1) == BeginDecision::kIgnore);
    assert(lifecycle.Begin(2) == BeginDecision::kIgnore);
    assert(!lifecycle.Active());
    assert(!lifecycle.End());

    assert(lifecycle.Begin(3) == BeginDecision::kSpawn);
    assert(lifecycle.Active());
    assert(lifecycle.Begin(3) == BeginDecision::kAlreadyActive);
    assert(lifecycle.Active());
    assert(lifecycle.End());
    assert(!lifecycle.Active());
    assert(!lifecycle.End());

    assert(lifecycle.Begin(3) == BeginDecision::kSpawn);
    assert(lifecycle.End());

    static_assert(bard::band::kFormation.size() == 4);
    assert(
        bard::band::kFormation[0].role
        == bard::band::Role::kBassist);
    assert(bard::band::kFormation[0].actorLocalId == 0x806);
    assert(
        bard::band::kFormation[1].role
        == bard::band::Role::kSinger);
    assert(bard::band::kFormation[1].actorLocalId == 0x809);
    assert(
        bard::band::kFormation[2].role
        == bard::band::Role::kRhythmGuitarist);
    assert(bard::band::kFormation[2].actorLocalId == 0x807);
    assert(
        bard::band::kFormation[3].role
        == bard::band::Role::kDrummer);
    assert(bard::band::kFormation[3].actorLocalId == 0x808);
    // The performers form one player-relative stage line. Two stand on
    // either side of the player, with no cardinal-point cross around them.
    for (const auto& performer : bard::band::kFormation) {
        assert(performer.forward == 0.0f);
        assert(performer.right != 0.0f);
    }
    assert(bard::band::kFormation[0].right < bard::band::kFormation[1].right);
    assert(bard::band::kFormation[1].right < 0.0f);
    assert(bard::band::kFormation[2].right > 0.0f);
    assert(bard::band::kFormation[2].right < bard::band::kFormation[3].right);
    assert(bard::band::kFormation[0].right == -105.0f);
    assert(bard::band::kFormation[1].right == -55.0f);
    assert(bard::band::kFormation[2].right == 55.0f);
    assert(bard::band::kFormation[3].right == 105.0f);

    // ---- HOW MANY PERFORMERS CARRY THE CONJURATION ----------------------
    //
    // SummonTargetFX's sample is baked into its NIF, so the count of art
    // objects IS the count of sounds - there is no volume field anywhere in
    // between. That makes this count the single most consequential number in
    // the band's presentation, and it must track the knob exactly: a bearer
    // role no performer holds means a SILENT, effect-less arrival, and a
    // count that disagrees with kArrivalArtOnEveryPerformer means the knob
    // has quietly stopped meaning what it says. Both ship looking fine.
    int bearers = 0;
    for (const auto& performer : bard::band::kFormation) {
        if (bard::band::BearsArrivalArt(performer.role)) { ++bearers; }
    }
    if (bard::band::kArrivalArtOnEveryPerformer) {
        assert(
            bearers == static_cast<int>(bard::band::kFormation.size()));
        assert(bard::band::BearsArrivalArt(bard::band::Role::kBassist));
        assert(bard::band::BearsArrivalArt(bard::band::Role::kDrummer));
        assert(
            bard::band::BearsArrivalArt(
                bard::band::Role::kRhythmGuitarist));
    } else {
        assert(bearers == 1);
        assert(!bard::band::BearsArrivalArt(bard::band::Role::kBassist));
        assert(!bard::band::BearsArrivalArt(bard::band::Role::kDrummer));
        assert(
            !bard::band::BearsArrivalArt(
                bard::band::Role::kRhythmGuitarist));
    }
    // True either way: the designated bearer always bears it, so flipping
    // the knob can never produce a band with no conjuration at all.
    assert(bard::band::BearsArrivalArt(bard::band::kArrivalArtBearer));
    assert(bearers >= 1);
    assert(bard::band::kArrivalArtBearer == bard::band::Role::kSinger);

    const auto bassist =
        bard::band::PerformanceEvents(bard::band::Role::kBassist, 0);
    const auto guitarist =
        bard::band::PerformanceEvents(
            bard::band::Role::kRhythmGuitarist, 1);
    const auto drummer =
        bard::band::PerformanceEvents(bard::band::Role::kDrummer, 0);
    const auto singer =
        bard::band::PerformanceEvents(bard::band::Role::kSinger, 1);
    assert(bassist[0] == "IdleLuteStart");
    assert(guitarist[0] == "IdleLuteStart");
    assert(drummer[0] == "IdleDrumStart");
    assert(singer[0] == "IdleDrinkPotion");
    assert(
        bard::band::PerformanceEvents(
            bard::band::Role::kSinger, 8)[0]
        == "IdleDrinkPotion");
    assert(
        bard::band::PerformanceEvents(
            bard::band::Role::kSinger, 12)[0]
        == "IdleCiceroDance1");
    assert(
        bard::band::PerformanceEvents(
            bard::band::Role::kSinger, 24)[0]
        == "IdleDrinkPotion");
    assert(
        bard::band::PerformanceEvents(
            bard::band::Role::kSinger, 36)[0]
        == "IdleCiceroDance2");
    static_assert(
        bard::band::kDismissDeleteDelaySeconds
        > bard::band::kDismissVisualSeconds);
    static_assert(
        bard::band::kResultsRevealDelaySeconds
        > bard::band::kDismissDeleteDelaySeconds);
    assert(!bard::band::ResultsMayPublish(10.0, 11.75));
    assert(bard::band::ResultsMayPublish(11.75, 11.75));
    // 0_master.hkx declares the booleans `bHeadTracking`/`bHeadTrackSpine`
    // (verified by string-scanning the live Nemesis-patched graph). The
    // `HeadTrackingOff` EVENT also exists there, but the 14:57 field run
    // proved every performer rejected it on every pulse while accepting
    // idle events. The graph variables are the working seam.
    assert(bard::band::kHeadTrackingVariable == "bHeadTracking");
    assert(bard::band::kHeadTrackSpineVariable == "bHeadTrackSpine");

    // The game thread drains queued pulses in bursts after loads and menu
    // churn: the 14:57 field run drained ~12 pulses within 3ms, collapsing
    // every "one pulse apart" sequence. Backlog pulses must be dropped.
    assert(!bard::band::PulseIsBacklog(false, 0.0));
    assert(bard::band::PulseIsBacklog(true, 0.0));
    assert(bard::band::PulseIsBacklog(true, 0.003));
    assert(!bard::band::PulseIsBacklog(
        true, bard::band::kMinPulseSpacingSeconds));
    assert(!bard::band::PulseIsBacklog(
        true, bard::band::kPerformancePulseSeconds));
    static_assert(
        bard::band::kMinPulseSpacingSeconds
        < bard::band::kPerformancePulseSeconds);

    // The shared AnimObjectLute capture window must be measured in real
    // seconds, not pulses: with a collapsed backlog the old one-pulse gate
    // re-pointed the model ~2ms after the bassist's idle, and whichever
    // model the graph loaded won nondeterministically (bass sometimes held
    // no instrument in the field). A dead bass role must not block the
    // rhythm guitarist forever.
    assert(bard::band::RhythmMustWaitForBass(true, false, 99.0));
    assert(bard::band::RhythmMustWaitForBass(true, true, 0.0));
    assert(bard::band::RhythmMustWaitForBass(true, true, 0.002));
    assert(!bard::band::RhythmMustWaitForBass(
        true, true, bard::band::kSharedPropCaptureSeconds));
    assert(!bard::band::RhythmMustWaitForBass(false, false, 0.0));
    assert(!bard::band::RhythmMustWaitForBass(false, true, 0.0));

    assert(!bard::band::ShouldAttemptPerformance(
        bard::band::Role::kSinger, true, 0));
    assert(bard::band::ShouldAttemptPerformance(
        bard::band::Role::kSinger, false, 0));
    assert(!bard::band::ShouldAttemptPerformance(
        bard::band::Role::kSinger, true, 1));
    assert(bard::band::ShouldAttemptPerformance(
        bard::band::Role::kSinger, true, 12));
    assert(!bard::band::ShouldAttemptPerformance(
        bard::band::Role::kSinger, true, 13));
    assert(bard::band::ShouldAttemptPerformance(
        bard::band::Role::kSinger, true, 24));
    assert(bard::band::ShouldAttemptPerformance(
        bard::band::Role::kSinger, true, 36));
    assert(!bard::band::ShouldAttemptPerformance(
        bard::band::Role::kRhythmGuitarist, true, 8));
    assert(bard::band::ShouldAttemptPerformance(
        bard::band::Role::kRhythmGuitarist, false, 8));

    bard::band::StemAvailability mixedTrack;
    assert(bard::band::RoleIsLive(
        bard::band::Role::kBassist, mixedTrack));
    assert(bard::band::RoleIsLive(
        bard::band::Role::kRhythmGuitarist, mixedTrack));
    assert(bard::band::RoleIsLive(
        bard::band::Role::kDrummer, mixedTrack));
    assert(bard::band::RoleIsLive(
        bard::band::Role::kSinger, mixedTrack));

    bard::band::StemAvailability splitTrack;
    splitTrack.Note("guitar");
    // A guitar-only Clone Hero import has no separable backing-role
    // metadata. It must fall back to the full ensemble, exactly like a
    // single mixed `song` stem.
    assert(bard::band::RoleIsLive(
        bard::band::Role::kBassist, splitTrack));
    assert(bard::band::RoleIsLive(
        bard::band::Role::kRhythmGuitarist, splitTrack));
    assert(bard::band::RoleIsLive(
        bard::band::Role::kDrummer, splitTrack));
    assert(bard::band::RoleIsLive(
        bard::band::Role::kSinger, splitTrack));
    splitTrack.Note("bass");
    splitTrack.Note("rhythm");
    splitTrack.Note("drums_2");
    splitTrack.Note("vocals_1");
    assert(bard::band::RoleIsLive(
        bard::band::Role::kBassist, splitTrack));
    assert(bard::band::RoleIsLive(
        bard::band::Role::kRhythmGuitarist, splitTrack));
    assert(bard::band::RoleIsLive(
        bard::band::Role::kDrummer, splitTrack));
    assert(bard::band::RoleIsLive(
        bard::band::Role::kSinger, splitTrack));

    // You Shook Me All Night Long (Mickelraven): bass + drums_1..4 +
    // guitar + song + vocals, no `rhythm` stem. The rhythm guitar is
    // audibly inside the `song` backing mix, so the rhythm guitarist must
    // play. The pre-2026-07-25 rule idled him.
    bard::band::StemAvailability ysmanl;
    ysmanl.Note("bass");
    ysmanl.Note("drums_1");
    ysmanl.Note("drums_4");
    ysmanl.Note("guitar");
    ysmanl.Note("song");
    ysmanl.Note("vocals");
    assert(bard::band::RoleIsLive(
        bard::band::Role::kRhythmGuitarist, ysmanl));
    assert(bard::band::RoleIsLive(
        bard::band::Role::kBassist, ysmanl));
    assert(bard::band::RoleIsLive(
        bard::band::Role::kDrummer, ysmanl));
    assert(bard::band::RoleIsLive(
        bard::band::Role::kSinger, ysmanl));

    // Harmonix Slipknot layout: drums_1..3 + guitar + rhythm + song +
    // vocals, no `bass` stem. The bass is audibly inside the `song`
    // backing mix, so the bassist must play. The pre-2026-07-25 rule
    // idled him.
    bard::band::StemAvailability harmonix;
    harmonix.Note("drums_1");
    harmonix.Note("drums_2");
    harmonix.Note("drums_3");
    harmonix.Note("guitar");
    harmonix.Note("rhythm");
    harmonix.Note("song");
    harmonix.Note("vocals");
    assert(bard::band::RoleIsLive(
        bard::band::Role::kBassist, harmonix));
    assert(bard::band::RoleIsLive(
        bard::band::Role::kRhythmGuitarist, harmonix));

    // COMPLETE separation (no `song` catch-all): a missing role stem now
    // honestly proves the role is silent. This strict branch had no
    // negative coverage before.
    bard::band::StemAvailability separated;
    separated.Note("bass");
    separated.Note("drums");
    separated.Note("vocals");
    separated.Note("guitar");
    assert(!bard::band::RoleIsLive(
        bard::band::Role::kRhythmGuitarist, separated));
    assert(bard::band::RoleIsLive(
        bard::band::Role::kBassist, separated));
    bard::band::StemAvailability separatedNoBass;
    separatedNoBass.Note("rhythm");
    separatedNoBass.Note("drums");
    separatedNoBass.Note("vocals");
    assert(!bard::band::RoleIsLive(
        bard::band::Role::kBassist, separatedNoBass));

    // Ghost membrane gating (field 2026-07-25: shader applied before 3D
    // existed attached to nothing and skeletons stayed opaque). It must
    // wait for 3D AND a settled instrument, and a dead role (never
    // starting) still counts as settled.
    assert(!bard::band::GhostShaderShouldApply(false, false, true));
    assert(!bard::band::GhostShaderShouldApply(false, true, false));
    assert(bard::band::GhostShaderShouldApply(false, true, true));
    assert(!bard::band::GhostShaderShouldApply(true, true, true));
    static_assert(
        bard::band::kApparitionAlpha > 0.0f
        && bard::band::kApparitionAlpha < 1.0f);

    // Headtracking clear logging (field 2026-07-25 21:36: pulse 0 runs
    // ~14ms after conjuring, before the graph loads, so the old
    // first-attempt latch always logged cleared=false and later silent
    // successes made the `cleared=true` field tell unobservable). Log
    // the first success; past the fallback pulse a still-failing clear
    // logs anyway so a real failure surfaces. Never log twice.
    assert(!bard::band::ShouldLogHeadTrackingClear(false, false, 0));
    assert(bard::band::ShouldLogHeadTrackingClear(false, true, 0));
    assert(bard::band::ShouldLogHeadTrackingClear(false, true, 3));
    assert(!bard::band::ShouldLogHeadTrackingClear(
        false, false, bard::band::kHeadTrackingLogFallbackPulse - 1));
    assert(bard::band::ShouldLogHeadTrackingClear(
        false, false, bard::band::kHeadTrackingLogFallbackPulse));
    assert(!bard::band::ShouldLogHeadTrackingClear(true, true, 3));
    assert(!bard::band::ShouldLogHeadTrackingClear(
        true, false, bard::band::kHeadTrackingLogFallbackPulse));

    // `keys`/`crowd` stems are reserved scanner names but prove neither
    // separation nor backing.
    bard::band::StemAvailability keysOnly;
    keysOnly.Note("keys");
    keysOnly.Note("crowd");
    assert(bard::band::RoleIsLive(
        bard::band::Role::kBassist, keysOnly));
    assert(!keysOnly.anyRoleDedicated);
    assert(!keysOnly.mixedBacking);
    using bard::band::PropKind;
    assert(
        bard::band::PerformanceProp(bard::band::Role::kBassist)
        == PropKind::kBassAnimationObject);
    assert(
        bard::band::PerformanceProp(
            bard::band::Role::kRhythmGuitarist)
        == PropKind::kGuitarAnimationObject);
    assert(
        bard::band::PerformanceProp(bard::band::Role::kDrummer)
        == PropKind::kNativeDrumAnimationObject);
    assert(
        bard::band::PerformanceProp(bard::band::Role::kSinger)
        == PropKind::kMicrophoneAnimationObject);
    for (const auto role : {
             bard::band::Role::kBassist,
             bard::band::Role::kRhythmGuitarist,
             bard::band::Role::kDrummer,
             bard::band::Role::kSinger }) {
        assert(!bard::band::UsesEquippedWeaponProp(role));
    }
    for (const auto role : {
             bard::band::Role::kBassist,
             bard::band::Role::kRhythmGuitarist,
             bard::band::Role::kDrummer,
             bard::band::Role::kSinger }) {
        for (std::uint64_t pulse = 0; pulse < 8; ++pulse) {
            for (const auto event :
                 bard::band::PerformanceEvents(role, pulse)) {
                assert(!event.empty());
                assert(event.find("attack") == std::string_view::npos);
                assert(event.find("Attack") == std::string_view::npos);
                assert(event.find("shout") == std::string_view::npos);
                assert(event.find("Shout") == std::string_view::npos);
            }
        }
    }

    std::cout << "BandLifecycleTests PASS\n";
    return 0;
}
