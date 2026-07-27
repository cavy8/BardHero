#include "harness.h"
#include "chart/Normalize.h"

using namespace bard;

// Builder shorthand used by every parser/engine test from here on.
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

static ParsedChart Norm(std::vector<RawChord> chords,
                        std::uint32_t threshold = 65,  // floor(65/192*192)
                        double offset = 0.0) {
    RawTrack t;
    t.chords             = std::move(chords);
    t.hopoThresholdTicks = threshold;
    TempoMap tempo;
    tempo.SetResolution(192);
    tempo.Finalize();  // 120 BPM
    return Normalize(t, tempo, offset);
}

static void RunTests() {
    {   // natural HOPO: close gap + different mask; chords and repeats never
        auto ch = Norm({ C(0, { 0 }), C(60, { 1 }), C(120, { 1 }),
                         C(180, { 2, 3 }), C(240, { 4 }) });
        CHECK(!ch.notes[0].isHopo);  // first note
        CHECK(ch.notes[1].isHopo);   // gap 60 < 65, G->R
        CHECK(!ch.notes[2].isHopo);  // same mask as previous
        CHECK(!ch.notes[3].isHopo);  // chord: never natural
        CHECK(ch.notes[4].isHopo);   // single after chord, differs, gap 60
    }
    {   // threshold boundary: gap == threshold is NOT a hopo (strict <)
        auto ch = Norm({ C(0, { 0 }), C(65, { 1 }), C(65 + 64, { 2 }) });
        CHECK(!ch.notes[1].isHopo);
        CHECK(ch.notes[2].isHopo);
    }
    {   // flip semantics (.chart N5)
        auto ch = Norm({ C(0, { 0 }), C(60, { 1 }, Forcing::kFlip),
                         C(400, { 2 }, Forcing::kFlip),
                         C(460, { 2, 3 }, Forcing::kFlip) });
        CHECK(!ch.notes[1].isHopo);  // natural true -> flipped off
        CHECK(ch.notes[2].isHopo);   // natural false (gap) -> flipped on
        CHECK(ch.notes[3].isHopo);   // chord natural false -> flipped HOPO chord
    }
    {   // true overrides (.mid)
        auto ch = Norm({ C(0, { 0 }), C(60, { 1 }, Forcing::kForceStrum),
                         C(400, { 2 }, Forcing::kForceHopo) });
        CHECK(!ch.notes[1].isHopo);
        CHECK(ch.notes[2].isHopo);
    }
    {   // tap overrides flip; tap never hopo
        auto ch = Norm({ C(0, { 0 }), C(60, { 1 }, Forcing::kFlip, true) });
        CHECK(ch.notes[1].isTap);
        CHECK(!ch.notes[1].isHopo);
    }
    {   // times: 120 BPM, offset applied
        auto ch = Norm({ C(192, { 0 }) }, 65, 1.5);
        CHECK_NEAR(ch.notes[0].time, 0.5 + 1.5, 1e-9);
    }
    {   // sustains: per-lane ends resolved; extended flagged
        auto ch = Norm({ C(0, { 0 }, Forcing::kNone, false, 300),
                         C(192, { 1 }) });
        CHECK(ch.notes[0].sustainTicks[0] == 300);
        CHECK_NEAR(ch.notes[0].sustainEnd[0], 300.0 / 192.0 * 0.5, 1e-9);
        CHECK(ch.notes[0].extendedMask == LaneBit(0));  // overlaps tick 192
        CHECK(ch.notes[1].extendedMask == 0);
    }
    {   // SP phrase membership: [start, end) exclusive, zero-length special
        RawTrack t;
        t.chords             = { C(0, { 0 }), C(100, { 1 }), C(200, { 2 }),
                                 C(300, { 3 }) };
        t.hopoThresholdTicks = 65;
        t.spPhrases.push_back({ 100, 200 });        // covers tick 100 only
        t.spPhrases.push_back({ 300, 300, true });  // zero-len covers 300
        TempoMap tempo;
        tempo.SetResolution(192);
        tempo.Finalize();
        auto ch = Normalize(t, tempo, 0.0);
        CHECK(ch.notes[0].spPhrase == -1);
        CHECK(ch.notes[1].spPhrase == 0);
        CHECK(ch.notes[2].spPhrase == -1);  // end exclusive
        CHECK(ch.notes[3].spPhrase == 1);   // zero-length includes its tick
        CHECK(ch.spPhrases[0].noteCount == 1);
        CHECK(ch.spPhrases[0].lastNoteIndex == 1);
    }
    {   // end_events truncation: notes at/after endTick dropped when hasEnd
        RawTrack t;
        t.chords             = { C(0, { 0 }), C(500, { 1 }) };
        t.hopoThresholdTicks = 65;
        t.hasEnd             = true;
        t.endTick            = 400;
        TempoMap tempo;
        tempo.SetResolution(192);
        tempo.Finalize();
        auto ch = Normalize(t, tempo, 0.0);
        CHECK(ch.notes.size() == 1);
    }
    {   // sustain-cutoff post-pass zeroes short lanes only
        RawTrack t;
        t.chords = { C(0, { 0 }, Forcing::kNone, false, 100),
                     C(500, { 1 }, Forcing::kNone, false, 200) };
        ApplySustainCutoff(t, 160);
        CHECK(t.chords[0].sustainTicks[0] == 0);
        CHECK(t.chords[1].sustainTicks[1] == 200);
    }
}

TEST_MAIN("Normalize")
