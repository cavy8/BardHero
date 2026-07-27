#include "harness.h"
#include "chart/Normalize.h"
#include "replay/Replay.h"

#include <cstdio>
#include <vector>

using namespace bard;

// Same builder shorthand as the other engine test files (house pattern).
static RawChord C(std::uint32_t tick, std::initializer_list<int> lanes,
                  Forcing f = Forcing::kNone, bool tap = false,
                  std::uint32_t sus = 0) {
    RawChord c;
    c.tick    = tick;
    c.forcing = f;
    c.tap     = tap;
    for (int l : lanes) {
        c.mask |= LaneBit(l);
        c.sustainTicks[l] = sus;
    }
    return c;
}

// Torture chart: every mechanic at once, 120 BPM, res 192 (tick t -> t/384 s).
// 18 notes; the input script hits 16, misses 2, lands 1 overstrum, 1 ghost,
// completes 2 SP phrases + strips 1, whammies an SP sustain, activates SP,
// drains it, and finishes a 2/3 solo.
static ParsedChart BuildTortureChart() {
    RawTrack t;
    t.hopoThresholdTicks = 65;
    t.chords             = {
        // A: strums + chord
        C(192, { 0 }), C(384, { 1 }), C(576, { 0, 1 }), C(768, { 2 }),
        // B: hopo chain (gaps 60)
        C(960, { 0 }), C(1020, { 1 }), C(1080, { 2 }), C(1140, { 3 }),
        // C: tap
        C(1440, { 4 }, Forcing::kNone, true),
        // D: sustain + note over it (extended)
        C(1632, { 0 }, Forcing::kNone, false, 384), C(1824, { 1 }),
        // E: SP phrases
        C(2112, { 0 }, Forcing::kNone, false, 300), C(2304, { 1 }),  // P1
        C(2688, { 2 }),                                              // P2
        C(3264, { 3 }),                                              // P3 (missed)
        // F: solo (2 of 3 hit)
        C(3456, { 0 }), C(3648, { 1 }), C(3840, { 2 }),
    };
    t.spPhrases = { { 2112, 2500 }, { 2688, 2750 }, { 3264, 3300 } };
    t.solos     = { { 3456, 3840, 0 } };
    TempoMap tempo;
    tempo.SetResolution(192);
    tempo.Finalize();
    return Normalize(t, tempo, 0.0);
}

static std::vector<NoteInput> BuildTortureInputs() {
    auto F = [](double t, int lane, bool down) {
        return NoteInput{ t, static_cast<InputAction>(lane), down ? 1 : 0 };
    };
    auto S = [](double t) { return NoteInput{ t, InputAction::kStrum, 1 }; };
    auto W = [](double t) { return NoteInput{ t, InputAction::kWhammy, 1 }; };
    return {
        // A: four strums
        F(0.45, 0, true), S(0.5),
        F(0.9, 0, false), F(0.95, 1, true), S(1.0),
        F(1.44, 0, true), S(1.5),                       // GR chord, exact
        F(1.9, 0, false), F(1.92, 1, false), F(1.95, 2, true), S(2.0),
        S(2.2),                                         // deferred overstrum
        // B: strum the chain head, hammer the rest; ghost press mid-chain
        F(2.4, 2, false), F(2.45, 0, true), S(2.5),
        F(2.6, 4, true),                                // GHOST vs R@2.65625
        F(2.62, 4, false),
        F(2.63, 1, true), S(2.66),                      // R only via strum
        F(2.79, 2, true),                               // hopo Y
        F(2.95, 3, true),                               // hopo B
        S(2.98),                                        // EATEN by leniency
        F(3.1, 3, false), F(3.12, 2, false), F(3.14, 1, false),
        F(3.16, 0, false),
        // C: tap
        F(3.74, 4, true), F(3.9, 4, false),
        // D: sustain G, R over it
        F(4.2, 0, true), S(4.25),
        F(4.7, 1, true), S(4.75), F(4.9, 1, false),
        // E: SP phrase 1 (G sus + whammy, then R), phrase 2 (Y).
        // Kept time-sorted: a real input stream is non-decreasing, and the
        // clamp is for OS jitter, not a licence for unordered scripts.
        S(5.5), W(5.6), W(5.8),
        F(5.95, 1, true),
        W(6.0), S(6.0),
        F(6.4, 1, false), F(6.45, 0, false),
        F(6.95, 2, true), S(7.0),
        NoteInput{ 7.2, InputAction::kStarPower, 1 },   // activate
        F(7.4, 2, false),
        // F: solo, 2 of 3
        F(8.95, 0, true), S(9.0),
        F(9.45, 1, true), S(9.5),
    };
}

static void RunTests() {
    const auto chart  = BuildTortureChart();
    const auto inputs = BuildTortureInputs();

    const auto ref =
        RunReplay(chart, EngineParams::Default(), inputs, 60.0, 20.0);
    // the script really plays the chart
    CHECK(ref.stats.notesHit == 16);
    CHECK(ref.stats.notesMissed == 2);
    CHECK(ref.stats.overstrums == 1);
    CHECK(ref.stats.ghostInputs == 1);
    CHECK(ref.stats.spPhrasesCompleted == 2);
    CHECK(ref.stats.spActive == false);  // drained by t=20
    CHECK(ref.stats.sustainScore > 0);

    for (double hz : { 1.0, 2.0, 3.7, 10.0, 24.0, 59.94, 90.0, 144.0, 240.0,
                       977.0 }) {
        const auto r =
            RunReplay(chart, EngineParams::Default(), inputs, hz, 20.0);
        CHECK(r.stats.score == ref.stats.score);
        CHECK(r.stats.sustainScore == ref.stats.sustainScore);
        CHECK(r.stats.soloBonus == ref.stats.soloBonus);
        CHECK(r.stats.notesHit == ref.stats.notesHit);
        CHECK(r.stats.notesMissed == ref.stats.notesMissed);
        CHECK(r.stats.overstrums == ref.stats.overstrums);
        CHECK(r.stats.ghostInputs == ref.stats.ghostInputs);
        CHECK(r.stats.maxCombo == ref.stats.maxCombo);
        CHECK(r.stats.spPhrasesCompleted == ref.stats.spPhrasesCompleted);
        CHECK(r.stats.glory == ref.stats.glory);
        CHECK(r.judgments == ref.judgments);
        if (g_failures) {
            std::printf("  ^ first divergence at cadence %f Hz\n", hz);
            break;
        }
    }

    // Precision preset: dynamic windows must also be cadence-stable
    const auto refP =
        RunReplay(chart, EngineParams::Precision(), inputs, 60.0, 20.0);
    for (double hz : { 3.7, 144.0 }) {
        const auto r =
            RunReplay(chart, EngineParams::Precision(), inputs, hz, 20.0);
        CHECK(r.stats.score == refP.stats.score);
        CHECK(r.judgments == refP.judgments);
    }

    // replay round-trip
    const std::string tmp = "skyhero_replay_test.shrp";
    CHECK(SaveReplay(tmp, inputs));
    std::vector<NoteInput> loaded;
    CHECK(LoadReplay(tmp, loaded));
    CHECK(loaded.size() == inputs.size());
    CHECK(loaded.size() > 0 && loaded.back().time == inputs.back().time);
    std::remove(tmp.c_str());
}

TEST_MAIN("Determinism")
