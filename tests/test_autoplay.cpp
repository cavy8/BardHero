#include "harness.h"
#include "chart/Normalize.h"
#include "engine/GuitarEngine.h"
#include "game/AutoPlayBot.h"

#include <vector>

using namespace bard;

// 120 BPM, res 192 chart builder (same RawChord C() helper shape as
// test_engine.cpp; tests stay self-contained per file, house pattern).
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
                         std::vector<SpPhrase> sp = {}) {
    RawTrack t;
    t.chords             = std::move(chords);
    t.spPhrases          = std::move(sp);
    t.hopoThresholdTicks = 65;
    TempoMap tempo;
    tempo.SetResolution(192);
    tempo.Finalize();
    return Normalize(t, tempo, 0.0);
}

// Frame-quantized drive, exactly like the hook: each 16ms frame the bot
// emits everything due, the engine judges at the event stamps.
static EngineStats Drive(const ParsedChart& c, double from, double to,
                         SH::AutoPlayBot& bot) {
    GuitarEngine            e(c, EngineParams::Default());
    std::vector<NoteInput>  evs;
    for (double now = from; now <= to; now += 0.016) {
        evs.clear();
        bot.Emit(c, now, evs);
        for (const auto& ev : evs) e.Queue(ev);
        e.Update(now);
    }
    return e.Stats();
}

static void RunTests() {
    {   // torture chart: singles, strum chord, sustain, note INSIDE the
        // sustain (extended lane), open note mid-sustain, tap, forced
        // HOPO, disjoint-sustain chord, dense 16ths - every note hit,
        // zero misses/overstrums/ghosts
        auto c = Chart({
            C(0, { 0 }),                              // single G
            C(192, { 1, 2 }),                         // strum chord RY
            C(384, { 0 }, Forcing::kNone, false, 384),  // 1s sustain G
            C(480, { 1 }),                            // inside G's sustain
            C(576, { 5 }),                            // open, mid-sustain
            C(672, { 2 }, Forcing::kNone, true),      // tap
            C(736, { 3 }, Forcing::kForceHopo),       // forced HOPO
            C(960, { 2, 4 }, Forcing::kNone, false, 192),  // chord + sus
            C(1104, { 0 }),  // 16th-ish run while chord sustain lives
            C(1152, { 1 }),
            C(1200, { 2 }),
            C(1248, { 3 }),
        });
        SH::AutoPlayBot bot;
        bot.Reset();
        const auto s = Drive(c, -0.2, 6.0, bot);
        CHECK(s.notesHit == 12);
        CHECK(s.notesMissed == 0);
        CHECK(s.overstrums == 0);
        CHECK(s.ghostInputs == 0);
        CHECK(s.maxCombo == 12);
        CHECK(s.sustainScore > 0);  // sustains actually held to their end
    }
    {   // SP phrase notes complete their phrase (the hook fires the SP
        // activation separately; here the gauge must at least accrue)
        auto c = Chart(
            {
                C(0, { 0 }),
                C(192, { 1 }),
                C(384, { 2 }),
            },
            { SpPhrase{ 0, 385, false, 0, -1 } });
        SH::AutoPlayBot bot;
        bot.Reset();
        const auto s = Drive(c, -0.2, 3.0, bot);
        CHECK(s.notesHit == 3);
        CHECK(s.spPhrasesCompleted == 1);
    }
    {   // mid-song prime: a bot switched on halfway must not replay the
        // elapsed half as a late-hit burst - earlier notes just miss,
        // later ones all hit, and nothing overstrums
        auto c = Chart({
            C(0, { 0 }),
            C(192, { 1 }),
            C(768, { 2 }),   // 2.0s
            C(960, { 3 }),   // 2.5s
        });
        SH::AutoPlayBot bot;
        bot.Reset();
        const auto s = Drive(c, 1.5, 4.0, bot);
        CHECK(s.notesHit == 2);
        CHECK(s.notesMissed == 2);
        CHECK(s.overstrums == 0);
    }
    {   // Reset() forgets held/sustain state between sessions
        auto c1 = Chart({ C(0, { 4 }, Forcing::kNone, false, 768) });
        SH::AutoPlayBot bot;
        bot.Reset();
        (void)Drive(c1, -0.2, 1.0, bot);  // ends mid-sustain, lane 4 held
        bot.Reset();
        CHECK(bot.HeldMask() == 0);
        auto c2 = Chart({ C(0, { 5 }) });  // open note: any held fret kills it
        const auto s = Drive(c2, -0.2, 1.0, bot);
        CHECK(s.notesHit == 1);
    }
}

TEST_MAIN("AutoPlay")
