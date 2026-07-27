#include "harness.h"
#include "chart/Normalize.h"
#include "engine/GuitarEngine.h"

#include <vector>

using namespace bard;

// 120 BPM, res 192 chart builder (same RawChord C() helper shape as
// test_normalize.cpp; tests stay self-contained per file, house pattern).
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

static ParsedChart Chart(std::vector<RawChord> chords,
                         std::vector<SpPhrase> sp    = {},
                         std::vector<SoloPhrase> solos = {}) {
    RawTrack t;
    t.chords             = std::move(chords);
    t.spPhrases          = std::move(sp);
    t.solos              = std::move(solos);
    t.hopoThresholdTicks = 65;
    TempoMap tempo;
    tempo.SetResolution(192);
    tempo.Finalize();
    return Normalize(t, tempo, 0.0);
}

// Input-script shorthand. Chords passed to Chart() must be tick-sorted;
// Normalize does not sort.
static NoteInput Fret(double t, int lane, bool down) {
    return { t, static_cast<InputAction>(lane), down ? 1 : 0 };
}
static NoteInput Strum(double t) { return { t, InputAction::kStrum, 1 }; }

// Drive helper: feed everything, settle far past the chart.
static EngineStats Run(const ParsedChart& c, std::vector<NoteInput> ins,
                       EngineParams p = EngineParams::Default()) {
    GuitarEngine e(c, p);
    for (const auto& i : ins) e.Queue(i);
    e.Update(1e6);
    return e.Stats();
}

static void RunSkeletonTests() {
    {   // Glory is presentation pressure, not a fail gate: neutral at the
        // start, gently rewarded by a hit, punished harder by mistakes.
        GuitarEngine e(Chart({ C(192, { 0 }), C(384, { 0 }) }),
                       EngineParams::Default());
        CHECK_NEAR(e.Stats().glory, 0.5, 1e-12);
        e.Queue(Fret(0.4, 0, true));
        e.Queue(Strum(0.5));  // hit: +0.0125
        e.Queue(Strum(0.7));  // expires mid-gap: overstrum -0.04
        e.Update(5.0);        // second note misses: -0.075
        CHECK_NEAR(e.Stats().glory, 0.3975, 1e-12);
        CHECK(e.Stats().notesHit == 1);
        CHECK(e.Stats().overstrums == 1);
        CHECK(e.Stats().notesMissed == 1);
    }
    {   // Both rails clamp through real engine events. This is deliberately
        // more than a test of std::clamp transcribed beside the feature.
        std::vector<RawChord> good;
        std::vector<NoteInput> inputs{ Fret(0.0, 0, true) };
        for (int i = 1; i <= 50; ++i) {
            good.push_back(C(192 * i, { 0 }));
            inputs.push_back(Strum(0.5 * i));
        }
        CHECK_NEAR(Run(Chart(good), inputs).glory, 1.0, 1e-12);

        std::vector<RawChord> bad;
        for (int i = 1; i <= 10; ++i) bad.push_back(C(192 * i, { 0 }));
        CHECK_NEAR(Run(Chart(bad), {}).glory, 0.0, 1e-12);
    }
    {   // untouched notes all miss at back end; combo stays 0
        auto s = Run(Chart({ C(0, { 0 }), C(192, { 1 }) }), {});
        CHECK(s.notesMissed == 2);
        CHECK(s.notesHit == 0 && s.score == 0);
    }
    {   // strum buffered inside StrumLeniency, fret arrives later -> HIT
        // (that IS the buffer's purpose, spec 5.1); also documents queue
        // order preservation under the monotonic clamp
        GuitarEngine e(Chart({ C(0, { 0 }) }), EngineParams::Default());
        e.Queue(Strum(0.010));
        e.Queue(Fret(0.005, 0, true));  // clamps forward to 0.010
        e.Update(1.0);
        CHECK(e.Stats().notesHit == 1);
    }
    {   // strum hit at exact window edges: +/-70ms at ratio 1.0
        auto mk = [&](double t) {
            return Run(Chart({ C(192, { 0 }) }),  // note at 0.5s
                       { Fret(0.0, 0, true), Strum(t) });
        };
        CHECK(mk(0.5 - 0.069).notesHit == 1);
        CHECK(mk(0.5 + 0.069).notesHit == 1);
        // strum at 0.400: small timer expires 0.425, window opens 0.430 ->
        // buffered strum dies before entry -> miss
        CHECK(mk(0.400).notesHit == 0);
        // strum at 0.415: timer alive at the 0.430 entry boundary -> hit
        CHECK(mk(0.415).notesHit == 1);
    }
}

static void RunFretPatternTests() {
    {   // single note allows anchoring BELOW (G held under R is legal)
        auto s = Run(Chart({ C(192, { 1 }) }),
                     { Fret(0.4, 0, true), Fret(0.45, 1, true), Strum(0.5) });
        CHECK(s.notesHit == 1);
    }
    {   // ...but a HIGHER extra fret blocks it
        auto s = Run(Chart({ C(192, { 1 }) }),
                     { Fret(0.4, 2, true), Fret(0.45, 1, true), Strum(0.5) });
        CHECK(s.notesHit == 0);
    }
    {   // strum chords need the exact mask
        auto s = Run(Chart({ C(192, { 0, 1 }) }),
                     { Fret(0.4, 0, true), Fret(0.42, 1, true),
                       Fret(0.44, 2, true), Strum(0.5) });
        CHECK(s.notesHit == 0);
        auto s2 = Run(Chart({ C(192, { 0, 1 }) }),
                      { Fret(0.4, 0, true), Fret(0.42, 1, true), Strum(0.5) });
        CHECK(s2.notesHit == 1);
    }
    {   // open note = strum with NOTHING held
        auto s = Run(Chart({ C(192, { kOpenLane }) }), { Strum(0.5) });
        CHECK(s.notesHit == 1);
        auto s2 = Run(Chart({ C(192, { kOpenLane }) }),
                      { Fret(0.4, 0, true), Strum(0.5) });
        CHECK(s2.notesHit == 0);
    }
}

static void RunStateMachineTests() {
    // -- deferred overstrum ------------------------------------------------
    {   // timer expiry is the ONLY overstrum source; leading suppression
        auto s = Run(Chart({ C(192, { 0 }), C(384, { 0 }) }),
                     { Fret(0.3, 0, true), Strum(0.5), Strum(0.7) });
        CHECK(s.notesHit == 1);
        CHECK(s.notesMissed == 1);
        CHECK(s.overstrums == 1);  // mid-gap strum expired
        auto s2 = Run(Chart({ C(1920, { 0 }) }), { Strum(0.1) });
        CHECK(s2.overstrums == 0);  // nothing resolved yet -> suppressed
        // ...but the host must still be ABLE to sound it: scoring stays
        // silent, presentation gets a counter (field 2026-07-25, a
        // lead-in strum that makes no sound reads as dropped input).
        CHECK(s2.unscoredStrums == 1);
        CHECK(s2.combo == 0);
        // The dud strum itself costs NOTHING: the only Glory movement in
        // this fixture is the chart's single note going unplayed
        // (-0.075), so an overstrum penalty on top would read as 0.385.
        CHECK(s2.notesMissed == 1);
        CHECK_NEAR(s2.glory, 0.5 - 0.075, 1e-12);
    }
    {   // The outro suppression is presentation-counted the same way, and
        // a REAL overstrum never lands in the unscored bucket - the two
        // must not double-count, or the miss cue would fire twice.
        auto s = Run(Chart({ C(192, { 0 }) }),
                     { Fret(0.4, 0, true), Strum(0.5), Strum(2.0) });
        CHECK(s.notesHit == 1);
        CHECK(s.overstrums == 0);      // past the last note, no sustains
        CHECK(s.unscoredStrums == 1);
        auto mid = Run(Chart({ C(192, { 0 }), C(1920, { 0 }) }),
                       { Fret(0.4, 0, true), Strum(0.5), Strum(0.9) });
        CHECK(mid.overstrums == 1);     // real overstrum, mid-song
        CHECK(mid.unscoredStrums == 0);
    }
    {   // double strum: wrong frets buffer the first strum; the second is an
        // immediate overstrum; the re-armed timer still hits once frets fix
        auto s = Run(Chart({ C(192, { 0 }), C(384, { 0 }) }),
                     { Fret(0.45, 0, true), Strum(0.5),   // hit note 1
                       Fret(0.9, 1, true),                // wrong top fret
                       Strum(0.95), Strum(0.96),          // buffer + double
                       Fret(0.97, 1, false) });           // frets fix -> hit
        CHECK(s.notesHit == 2);
        CHECK(s.overstrums == 1);
        CHECK(s.maxCombo == 1);
    }
    // -- HOPO / tap fret path ---------------------------------------------
    {   // hopo chain: strum first, hammer the rest; anchoring below is legal
        auto s = Run(Chart({ C(192, { 0 }), C(252, { 1 }), C(312, { 2 }) }),
                     { Fret(0.45, 0, true), Strum(0.5),
                       Fret(0.655, 1, true),      // note 2 at 0.65625
                       Fret(0.81, 2, true) });    // note 3 at 0.8125
        CHECK(s.notesHit == 3);
        CHECK(s.overstrums == 0);
    }
    {   // combo 0 mid-chart: hopo fret path refused, strum re-arms the chain
        auto s = Run(Chart({ C(0, { 0 }), C(60, { 1 }), C(120, { 2 }) }),
                     { /* note 1 missed */
                       Fret(0.15, 1, true), Strum(0.16),
                       Fret(0.31, 2, true) });
        CHECK(s.notesMissed == 1);
        CHECK(s.notesHit == 2);
    }
    {   // FIRST note of chart exception: a leading hopo is frettable
        auto s = Run(Chart({ C(192, { 0 }, Forcing::kForceHopo) }),
                     { Fret(0.5, 0, true) });
        CHECK(s.notesHit == 1);
    }
    {   // hopo leniency eats EXACTLY ONE strum; the eaten strum cannot hit a
        // later note
        auto s = Run(Chart({ C(192, { 0 }), C(252, { 1 }), C(384, { 2 }) }),
                     { Fret(0.45, 0, true), Strum(0.5),
                       Fret(0.655, 1, true),
                       Strum(0.66),                       // EATEN
                       Fret(0.99, 2, true), Strum(1.0) });
        CHECK(s.notesHit == 3);
        CHECK(s.overstrums == 0);
        // a SECOND redundant strum is not eaten -> deferred overstrum
        auto s2 = Run(Chart({ C(192, { 0 }), C(252, { 1 }), C(384, { 2 }) }),
                      { Fret(0.45, 0, true), Strum(0.5),
                        Fret(0.655, 1, true),
                        Strum(0.66), Strum(0.67),
                        Fret(0.99, 2, true), Strum(1.0) });
        CHECK(s2.notesHit == 3);
        CHECK(s2.overstrums == 1);
    }
    {   // tap: frettable as head even with combo > 0 requirements absent
        auto s = Run(Chart({ C(192, { 0 }, Forcing::kNone, true) }),
                     { Fret(0.5, 0, true) });
        CHECK(s.notesHit == 1);
    }
    {   // front-end credit expires; infinite front end (Casual) flips it
        auto c = Chart({ C(0, { 1 }), C(384, { 0 }, Forcing::kForceHopo) });
        std::vector<NoteInput> ins{ Fret(-0.05, 1, true), Strum(0.0),
                                    Fret(0.15, 1, false),
                                    Fret(0.2, 0, true) };
        CHECK(Run(c, ins).notesHit == 1);  // credit long dead at 0.93 entry
        CHECK(Run(c, ins, EngineParams::Casual()).notesHit == 2);
    }
    // -- skip-to-hit + force-miss -----------------------------------------
    {   // holding note-2's fret and strumming hits note 2 (combo 0 allows
        // off-head), force-missing note 1
        auto s = Run(Chart({ C(192, { 0 }), C(212, { 2 }) }),
                     { Fret(0.5, 2, true), Strum(0.55) });
        CHECK(s.notesHit == 1);
        CHECK(s.notesMissed == 1);
    }
    // -- ghosting ----------------------------------------------------------
    {   // press raising the top fret, absent from the note's mask -> latch
        // blocks the fret path; strum still hits; detection runs with the
        // setting off but the penalty does not
        auto c = Chart({ C(0, { 0 }), C(192, { 1 }, Forcing::kForceHopo) });
        std::vector<NoteInput> ghost{ Fret(-0.05, 0, true), Strum(0.0),
                                      Fret(0.45, 3, true),   // ghost press
                                      Fret(0.46, 3, false),
                                      Fret(0.48, 1, true) };
        auto s = Run(c, ghost);
        CHECK(s.notesHit == 1);  // hopo blocked by the latch
        CHECK(s.ghostInputs == 1);
        auto sOff = Run(c, ghost, EngineParams::Casual());
        CHECK(sOff.ghostInputs == 1);  // detection always runs...
        CHECK(sOff.notesHit == 2);     // ...penalty only when enabled
        auto strummed = ghost;
        strummed.push_back(Strum(0.5));
        CHECK(Run(c, strummed).notesHit == 2);  // still strummable
    }
}

static void RunSustainTests() {
    // spacing = 192/25 = 7.68 ticks/point; L ticks -> floor(L/7.68) points.
    {   // full hold: 192-tick sustain = 25 points on top of the 50
        auto s = Run(Chart({ C(192, { 0 }, Forcing::kNone, false, 192) }),
                     { Fret(0.45, 0, true), Strum(0.5) });
        CHECK(s.score == 50 + 25);
        CHECK(s.sustainScore == 25);
    }
    {   // early release: grace accrues, then the drop commits the score
        // (384-tick sustain from 0.5s; release 1.0, deadline 1.025 ->
        // tickAt(1.025)=393.6 -> floor(201.6/7.68) = 26 points)
        auto s = Run(Chart({ C(192, { 0 }, Forcing::kNone, false, 384) }),
                     { Fret(0.45, 0, true), Strum(0.5),
                       Fret(1.0, 0, false) });
        CHECK(s.sustainScore == 26);
    }
    {   // re-grip inside the grace fully resets it -> full 50 points
        auto s = Run(Chart({ C(192, { 0 }, Forcing::kNone, false, 384) }),
                     { Fret(0.45, 0, true), Strum(0.5),
                       Fret(1.0, 0, false), Fret(1.01, 0, true) });
        CHECK(s.sustainScore == 50);
    }
    {   // burst: remainder awards at res/4 before the notated end; a release
        // inside the burst zone loses nothing
        auto s = Run(Chart({ C(192, { 0 }, Forcing::kNone, false, 384) }),
                     { Fret(0.45, 0, true), Strum(0.5),
                       Fret(1.4, 0, false) });  // burst was at 1.375
        CHECK(s.sustainScore == 50);
    }
    {   // multiplier rebase: 8 quick notes, sustain note 9 at 1x, note 10
        // lands mid-sustain -> old points keep 1x, later points 2x.
        // Sustain G@192 len 768 (100 pts): [0.5,1.5] = 50 pts at 1x;
        // (1.5,burst 2.375] = 43 pts at 2x; burst remainder 7 at 2x.
        // Notes: 8*50 + 50 + 100(mult 2) = 550. Sustain = 50+86+14 = 150.
        std::vector<RawChord> ch;
        for (int i = 0; i < 8; ++i)
            ch.push_back(C(i * 24, { 0 }));
        ch.push_back(C(192, { 0 }, Forcing::kNone, false, 768));
        ch.push_back(C(576, { 1 }));
        auto                   c = Chart(ch);
        std::vector<NoteInput> ins{ Fret(-0.05, 0, true) };
        for (int i = 0; i < 8; ++i) ins.push_back(Strum(i * 0.0625));
        ins.push_back(Strum(0.5));
        ins.push_back(Fret(1.45, 1, true));
        ins.push_back(Strum(1.5));
        auto s = Run(c, ins);
        CHECK(s.notesHit == 10);
        CHECK(s.sustainScore == 150);
        CHECK(s.score == 550 + 150);
    }
    {   // disjoint chord: independent per-lane objects (G 384 -> 50, R 96 ->
        // 12, R ends naturally before its release)
        RawChord c;
        c.tick            = 192;
        c.mask            = LaneBit(0) | LaneBit(1);
        c.sustainTicks[0] = 384;
        c.sustainTicks[1] = 96;
        auto chart = Chart({ c });
        auto s = Run(chart, { Fret(0.4, 0, true), Fret(0.42, 1, true),
                              Strum(0.5), Fret(0.85, 1, false) });
        CHECK(s.notesHit == 1);
        CHECK(s.sustainScore == 50 + 12);
    }
    {   // uniform chord: ONE object, scored per lane (2 lanes x 25)
        RawChord c;
        c.tick            = 192;
        c.mask            = LaneBit(0) | LaneBit(1);
        c.sustainTicks[0] = 192;
        c.sustainTicks[1] = 192;
        auto s = Run(Chart({ c }), { Fret(0.4, 0, true), Fret(0.42, 1, true),
                                     Strum(0.5) });
        CHECK(s.sustainScore == 50);
    }
    {   // extended sustain: same-lane repeat stays hittable; hitting it
        // drops the sustain exactly at the hit (384-tick sustain broken at
        // its halfway point = exactly 25 of 50 points)
        auto s = Run(Chart({ C(0, { 0 }, Forcing::kNone, false, 384),
                             C(192, { 0 }) }),
                     { Fret(-0.05, 0, true), Strum(0.0), Strum(0.5) });
        CHECK(s.notesHit == 2);
        CHECK(s.sustainScore == 25);
    }
    {   // extended sustain on another lane: fret subtraction lets the next
        // note be fret-path hit while G keeps ringing to its end
        auto s = Run(Chart({ C(0, { 0 }, Forcing::kNone, false, 384),
                             C(192, { 1 }, Forcing::kForceHopo) }),
                     { Fret(-0.05, 0, true), Strum(0.0),
                       Fret(0.5, 1, true) });
        CHECK(s.notesHit == 2);
        CHECK(s.sustainScore == 50);  // G sustain untouched, full points
    }
    {   // overstrum breaks all active sustains, score committed
        // (768-tick sustain from 0.0; small timer expires 0.525 ->
        // tickAt(0.525)=201.6 -> 26 points)
        auto s = Run(Chart({ C(0, { 0 }, Forcing::kNone, false, 768) }),
                     { Fret(-0.05, 0, true), Strum(0.0), Strum(0.5) });
        CHECK(s.overstrums == 1);
        CHECK(s.sustainScore == 26);
    }
}

static void RunSpSoloTests() {
    // res 192: full bar 6144 mticks, half bar 3072, phrase award 1536.
    auto mkSp = [&]() {
        std::vector<RawChord> ch;
        for (int i = 0; i < 8; ++i) ch.push_back(C(i * 192, { 0 }));
        std::vector<SpPhrase> sp{ { 0, 700 }, { 700, 1400 } };
        return Chart(ch, sp);  // 4 notes per phrase
    };
    {   // phrase completion awards 2 measures on the phrase's LAST note
        auto  c = mkSp();
        GuitarEngine e(c, EngineParams::Default());
        e.Queue(Fret(-0.1, 0, true));
        for (int i = 0; i < 4; ++i) e.Queue(Strum(i * 0.5));
        e.Update(2.0);
        CHECK_NEAR(e.Stats().spGaugeMeasureTicks, 1536.0, 1e-6);
        CHECK(e.Stats().spPhrasesCompleted == 1);
    }
    {   // a miss inside phrase 1 strips it; phrase 2 still completes
        auto  c = mkSp();
        GuitarEngine e(c, EngineParams::Default());
        e.Queue(Fret(-0.1, 0, true));
        e.Queue(Strum(0.0));
        // note 2 (t=0.5) missed
        for (double t : { 1.0, 1.5, 2.0, 2.5, 3.0, 3.5 }) e.Queue(Strum(t));
        e.Update(10.0);
        CHECK(e.Stats().spPhrasesCompleted == 1);
        CHECK_NEAR(e.Stats().spGaugeMeasureTicks, 1536.0, 1e-6);
    }
    {   // activation >= half bar, SP doubles the multiplier, drain ends at
        // zero. 17 G notes; 4 full phrases = 6144 (cap); activate at 7.9;
        // note 17 scores 50 * (mult 2 * 2) = 200.
        // Notes 1-9 at 1x = 450, 10-16 at 2x = 700, 17 at 4x = 200.
        std::vector<RawChord> ch;
        for (int i = 0; i < 17; ++i) ch.push_back(C(i * 192, { 0 }));
        std::vector<SpPhrase> sp;
        for (int i = 0; i < 4; ++i) {
            sp.push_back({ static_cast<std::uint32_t>(i * 4 * 192),
                           static_cast<std::uint32_t>((i * 4 + 3) * 192 + 1) });
        }
        auto         c = Chart(ch, sp);
        GuitarEngine e(c, EngineParams::Default());
        e.Queue(Fret(-0.1, 0, true));
        for (int i = 0; i < 16; ++i) e.Queue(Strum(i * 0.5));
        e.Queue(NoteInput{ 7.9, InputAction::kStarPower, 1 });
        e.Queue(Strum(8.0));
        e.Update(30.0);
        CHECK(e.Stats().spPhrasesCompleted == 4);
        CHECK(e.Stats().notesHit == 17);
        CHECK(e.Stats().spActive == false);  // drained (zero ~t=23.9)
        CHECK(e.Stats().score == 450 + 700 + 200);
    }
    {   // activation below half bar refused
        auto  c = mkSp();
        GuitarEngine e(c, EngineParams::Default());
        e.Queue(Fret(-0.1, 0, true));
        for (int i = 0; i < 4; ++i) e.Queue(Strum(i * 0.5));
        e.Queue(NoteInput{ 2.2, InputAction::kStarPower, 1 });
        e.Update(2.3);
        CHECK(!e.Stats().spActive);  // 1536 < 3072
    }
    {   // whammy on an SP sustain: +32/30 per tick inside the 250ms window.
        // Phrase note G@0 sus 768 (ends t=2.0); whammy 0.1..1.9 every 0.2s
        // keeps the window open to the sustain's end:
        // gain = (768 - 38.4) * 32/30 = 778.24 on top of the 1536 award.
        std::vector<RawChord> ch{ C(0, { 0 }, Forcing::kNone, false, 768) };
        std::vector<SpPhrase> sp{ { 0, 1 } };
        auto         c = Chart(ch, sp);
        GuitarEngine e(c, EngineParams::Default());
        e.Queue(Fret(-0.05, 0, true));
        e.Queue(Strum(0.0));
        for (int i = 0; i < 10; ++i) {
            e.Queue(NoteInput{ 0.1 + i * 0.2, InputAction::kWhammy, 1 });
        }
        e.Update(5.0);
        CHECK_NEAR(e.Stats().spGaugeMeasureTicks, 1536.0 + 778.24, 1e-6);
    }
    // -- solo bonus (PINNED formula) --------------------------------------
    {
        std::vector<RawChord> ch;
        for (int i = 0; i < 4; ++i) ch.push_back(C(i * 192, { 0 }));
        std::vector<SoloPhrase> solos{ { 0, 3 * 192, 0 } };
        auto c = Chart(ch, {}, solos);
        auto hitN = [&](int n) {
            std::vector<NoteInput> ins{ Fret(-0.1, 0, true) };
            for (int i = 0; i < n; ++i) ins.push_back(Strum(i * 0.5));
            return Run(c, ins);
        };
        CHECK(hitN(4).soloBonus == 400);  // 100% -> 100/note
        CHECK(hitN(3).soloBonus == 150);  // 75% -> scale .375 -> 150
        CHECK(hitN(2).soloBonus == 0);    // 50% < 60% -> nothing
    }
}

static void RunRenderReadApiTests() {
    {   // SP gauge: one completed phrase = 1/4 bar; value is LIVE (drains
        // while active), not the anchor-only Stats() field
        std::vector<RawChord> ch;
        for (int i = 0; i < 16; ++i) ch.push_back(C(192u * i, { 0 }));
        SpPhrase p1, p2;
        p1.startTick = 0;
        p1.endTick   = 192u * 7 + 1;   // covers notes 0..7
        p2.startTick = 192u * 8;
        p2.endTick   = 192u * 15 + 1;  // covers notes 8..15
        auto         chart = Chart(std::move(ch), { p1, p2 });
        GuitarEngine e(chart, EngineParams::Default());
        // All inputs queued up front (monotonic order, as required): a
        // split queue-then-Update(4.2)-then-queue-more sequence would let
        // Update(4.2)'s unconditional AdvanceTo walk the clock past note
        // 8's hit window (front 3.93s) before its strum ever entered the
        // queue, missing it and stripping phrase 2 via StripPhraseOf.
        e.Queue(Fret(0.0, 0, true));
        for (int i = 0; i < 16; ++i) e.Queue(Strum(0.5 * i));
        e.Queue({ 7.6, InputAction::kStarPower, 1 });
        e.Update(4.2);  // drains notes 0..8 (t<=4.2); phrase 1 done, phrase
                        // 2 only 1/8 in -> still exactly one award
        CHECK_NEAR(e.SpGaugeFraction(4.2), 0.25, 1e-9);
        // second phrase completes -> 0.5 bar -> activation allowed
        e.Update(7.6);
        CHECK(e.Stats().spActive);
        const double f0 = e.SpGaugeFraction(7.6);
        const double f1 = e.SpGaugeFraction(8.6);
        // drain: 1 gauge tick per chart tick; 120 BPM res 192 -> 384
        // ticks/s; full bar = 32*192
        CHECK_NEAR(f0 - f1, 384.0 / (32.0 * 192.0), 1e-6);
    }
    {   // sustain masks: active while held, dropped after early release
        auto chart = Chart(
            { C(192, { 0 }, Forcing::kNone, false, 384) });  // 1s sustain
        GuitarEngine e(chart, EngineParams::Default());
        e.Queue(Fret(0.45, 0, true));
        e.Queue(Strum(0.5));
        e.Update(0.8);
        std::uint8_t a = 0xFF, d = 0xFF;  // poison: prove the zero-writes
        e.SustainMasks(0, a, d);
        CHECK(a == LaneBit(0) && d == 0);
        e.Queue(Fret(0.9, 0, false));  // release mid-sustain
        e.Update(1.2);                 // far past sustainDropLeniency (25ms)
        e.SustainMasks(0, a, d);
        CHECK(a == 0 && d == LaneBit(0));
    }
    {   // never-hit note: no started sustain -> both masks empty
        auto chart = Chart(
            { C(192, { 0 }, Forcing::kNone, false, 384) });
        GuitarEngine e(chart, EngineParams::Default());
        e.Update(5.0);
        std::uint8_t a = 0xFF, d = 0xFF;
        e.SustainMasks(0, a, d);
        CHECK(a == 0 && d == 0);
    }
}

static void RunTests() {
    RunSkeletonTests();
    RunFretPatternTests();
    RunStateMachineTests();
    RunSustainTests();
    RunSpSoloTests();
    RunRenderReadApiTests();
}

TEST_MAIN("Engine")
