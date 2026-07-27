// tests/test_practice.cpp - suite: practice mode core
#include "harness.h"

#include "chart/ChartTypes.h"
#include "practice/PracticeLoop.h"
#include "practice/PracticeRange.h"
#include "practice/PracticeSections.h"
#include "practice/PracticeSlice.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

static void StepSpeedTests() {
    using bard::practice::StepSpeed;
    using bard::practice::kSpeedMax;
    using bard::practice::kSpeedMin;

    // 5% steps, both directions.
    CHECK_NEAR(StepSpeed(1.0, -1), 0.95, 1e-9);
    CHECK_NEAR(StepSpeed(1.0, +1), 1.05, 1e-9);
    CHECK_NEAR(StepSpeed(0.95, -1), 0.90, 1e-9);

    // CLAMPS at both ends rather than wrapping: holding the key past the
    // limit must sit there, not jump to the opposite extreme.
    CHECK_NEAR(StepSpeed(kSpeedMin, -1), kSpeedMin, 1e-9);
    CHECK_NEAR(StepSpeed(kSpeedMin, -50), kSpeedMin, 1e-9);
    CHECK_NEAR(StepSpeed(kSpeedMax, +1), kSpeedMax, 1e-9);
    CHECK_NEAR(StepSpeed(kSpeedMax, +50), kSpeedMax, 1e-9);

    // Stepping down then back up RETURNS EXACTLY to 1.0. This is the
    // property the grid snap exists for: the audio callback's bypass is an
    // exact `speed == 1.0` test, so a value that drifted to 0.99999 would
    // silently keep the whole time-stretch pipeline running - and its
    // latency - through what the player was told is normal speed.
    double v = 1.0;
    for (int i = 0; i < 6; ++i) { v = StepSpeed(v, -1); }
    for (int i = 0; i < 6; ++i) { v = StepSpeed(v, +1); }
    CHECK(v == 1.0);

    // A round trip elsewhere on the grid returns to the same value to well
    // within a step. NOT asserted exactly, and the asymmetry is the point:
    // 20 * 0.05 is exactly 1.0 in binary but 14 * 0.05 is 0.7000000000000001,
    // so only the 1.0 case above can be an exact test - and 1.0 is the only
    // one that has to be, because it is the value the audio callback's
    // bypass compares against.
    CHECK_NEAR(StepSpeed(StepSpeed(0.70, +1), -1), 0.70, 1e-9);

    // Zero delta is a no-op that still snaps a nudged value onto the grid.
    CHECK_NEAR(StepSpeed(0.7499, 0), 0.75, 1e-9);

    // A nonsense current speed degrades to 1.0 rather than to 0 or a
    // negative ratio, which the audio path cannot represent at all.
    CHECK_NEAR(StepSpeed(0.0, 0), 1.0, 1e-9);
    CHECK_NEAR(StepSpeed(-2.0, 0), 1.0, 1e-9);
}

static void RunTests() {
    StepSpeedTests();
    // A chart carries its section markers; no markers is a valid state,
    // not an error - practice must still open over the whole song.
    bard::ParsedChart chart;
    CHECK(chart.sections.empty());

    bard::ChartSection s;
    s.tick = 768;
    s.time = 2.0;
    s.name = "Intro";
    chart.sections.push_back(s);
    CHECK(chart.sections.size() == 1);
    CHECK(chart.sections[0].name == "Intro");
    CHECK(chart.sections[0].tick == 768);
    CHECK_NEAR(chart.sections[0].time, 2.0, 1e-12);

    // .chart markers: the three spellings seen in the wild, plus the
    // non-section events that share [Events] and must be ignored.
    {
        using bard::practice::SectionNameFromChartEvent;
        std::string name;
        CHECK(SectionNameFromChartEvent("E \"section Intro\"", name));
        CHECK(name == "Intro");
        CHECK(SectionNameFromChartEvent("E \"prc_verse_1\"", name));
        CHECK(name == "verse_1");
        CHECK(SectionNameFromChartEvent("section Chorus", name));
        CHECK(name == "Chorus");
        // Case-insensitive keyword, preserved name casing.
        CHECK(SectionNameFromChartEvent("E \"SECTION Solo A\"", name));
        CHECK(name == "Solo A");
        // Not sections:
        CHECK(!SectionNameFromChartEvent("E \"solo\"", name));
        CHECK(!SectionNameFromChartEvent("E \"soloend\"", name));
        CHECK(!SectionNameFromChartEvent("E \"end\"", name));
        CHECK(!SectionNameFromChartEvent("", name));
        // A keyword with no name is not a section (would render blank).
        CHECK(!SectionNameFromChartEvent("E \"section \"", name));
        CHECK(!SectionNameFromChartEvent("E \"section\"", name));
    }

    // .mid markers are bracketed text events.
    {
        using bard::practice::SectionNameFromMidText;
        std::string name;
        CHECK(SectionNameFromMidText("[section Intro]", name));
        CHECK(name == "Intro");
        CHECK(SectionNameFromMidText("[prc_chorus]", name));
        CHECK(name == "chorus");
        CHECK(SectionNameFromMidText("  [section Verse 2]  ", name));
        CHECK(name == "Verse 2");
        // A NUL tail must not hide the closing bracket: SMF strings are
        // length-prefixed, so a writer that counts the terminator hands
        // us one, and the bracket check runs after the trim.
        CHECK(SectionNameFromMidText(
            std::string_view("[section Intro]\0", 16), name));
        CHECK(name == "Intro");
        // Brackets are required in .mid, and non-section text is ignored.
        CHECK(!SectionNameFromMidText("section Intro", name));
        CHECK(!SectionNameFromMidText("[solo]", name));
        CHECK(!SectionNameFromMidText("[]", name));
        CHECK(!SectionNameFromMidText("", name));
        CHECK(!SectionNameFromMidText("[section ]", name));
    }

    // Whole-file collection: sorted by tick, ties resolved last-wins,
    // non-section events dropped, times from the tempo map + offset.
    {
        bard::ChartFile cf;
        cf.resolution = 192;
        cf.tempo.SetResolution(192);
        cf.tempo.AddBpm(0, 120.0);
        cf.tempo.Finalize();
        cf.sections["Events"] = {
            { 384, "E \"section Verse 1\"" },
            { 0,   "E \"section Intro\"" },
            { 192, "E \"solo\"" },              // dropped
            { 384, "E \"section Verse One\"" }, // same tick: last wins
        };
        const auto out = bard::practice::SectionsFromChart(cf, 0.5);
        CHECK(out.size() == 2);
        CHECK(out[0].tick == 0);
        CHECK(out[0].name == "Intro");
        // 120bpm at res 192: one beat = 0.5s, and the 0.5 offset applies.
        CHECK_NEAR(out[0].time, 0.5, 1e-9);
        CHECK(out[1].tick == 384);
        CHECK(out[1].name == "Verse One");
        CHECK_NEAR(out[1].time, 0.5 + 1.0, 1e-9);

        // A chart with no [Events] section at all is fine.
        bard::ChartFile empty;
        empty.tempo.SetResolution(192);
        empty.tempo.Finalize();
        CHECK(bard::practice::SectionsFromChart(empty, 0.0).empty());
    }

    // Same collection from a .mid, where the EVENTS track is the only
    // track a section may come from - a PART track's section-shaped text
    // is a charter's private note and must not reach the practice menu.
    {
        bard::TempoMap tempo;
        tempo.SetResolution(192);
        tempo.AddBpm(0, 120.0);
        tempo.Finalize();

        bard::SmfFile file;
        bard::SmfTrack events;
        events.name  = "EVENTS";
        events.texts = {
            { 384, "[section Verse 1]" },
            { 0, "[section Intro]" },
            { 192, "[lyric la]" },        // dropped
            { 384, "[section Verse One]" },  // same tick: last wins
        };
        bard::SmfTrack part;
        part.name  = "PART GUITAR";
        part.texts = { { 96, "[section Not Mine]" } };
        file.tracks.push_back(part);
        file.tracks.push_back(events);

        const auto out = bard::practice::SectionsFromMid(file, tempo, 0.5);
        CHECK(out.size() == 2);
        CHECK(out[0].tick == 0);
        CHECK(out[0].name == "Intro");
        CHECK_NEAR(out[0].time, 0.5, 1e-9);
        CHECK(out[1].tick == 384);
        CHECK(out[1].name == "Verse One");
        CHECK_NEAR(out[1].time, 0.5 + 1.0, 1e-9);

        // No EVENTS track at all is a valid song, not an error.
        bard::SmfFile partOnly;
        partOnly.tracks.push_back(part);
        CHECK(bard::practice::SectionsFromMid(partOnly, tempo, 0.0).empty());

        // Track-name variants. Smf.h assigns the meta-0x03 payload byte
        // for byte, so real files carry padded, differently-cased and
        // NUL-terminated spellings. Missing one is SILENT (zero sections,
        // no error), hence the tolerance - but it stops at equality.
        const auto named = [&](const std::string& n) {
            bard::SmfFile f;
            bard::SmfTrack t = events;
            t.name = n;
            f.tracks.push_back(t);
            return bard::practice::SectionsFromMid(f, tempo, 0.0);
        };
        CHECK(named("EVENTS").size() == 2);
        CHECK(named("EVENTS ").size() == 2);            // trailing space
        CHECK(named("  EVENTS\t").size() == 2);         // padded both ends
        CHECK(named("Events").size() == 2);             // charter casing
        CHECK(named(std::string("EVENTS\0", 7)).size() == 2);  // NUL tail
        // Equality, not prefix: these are OTHER tracks.
        CHECK(named("EVENTS2").empty());
        CHECK(named("EVENTS TRK").empty());
        CHECK(named("EVENT").empty());
    }

    // A section pair resolves to the seconds range practice plays.
    {
        using namespace bard::practice;
        std::vector<bard::ChartSection> secs;
        auto add = [&](std::uint32_t tick, double time, const char* n) {
            bard::ChartSection s;
            s.tick = tick; s.time = time; s.name = n;
            secs.push_back(s);
        };
        add(0, 0.0, "Intro");
        add(384, 10.0, "Verse");
        add(768, 20.0, "Chorus");
        const double songEnd = 30.0;

        // One section: starts a lead-in early, ends at the NEXT section.
        auto r = ResolveRange(secs, 1, 1, songEnd);
        CHECK(r.startSection == 1);
        CHECK(r.endSection == 1);
        CHECK_NEAR(r.startSec, 10.0 - kLeadInSec, 1e-9);
        CHECK_NEAR(r.endSec, 20.0 + kTailPadSec, 1e-9);
        // THE EASE-IN. Playback seeks a lead-in early, but the NOTES start
        // at the section itself - so the previous section's notes are not
        // thrown at the player during the approach window. Clone Hero's
        // practice behaviour, and the fix for the jarring loop restart.
        CHECK_NEAR(r.notesFromSec, 10.0, 1e-9);
        CHECK(r.notesFromSec > r.startSec);

        // A span of sections.
        r = ResolveRange(secs, 0, 1, songEnd);
        CHECK_NEAR(r.startSec, 0.0, 1e-9);   // clamped, never negative
        CHECK_NEAR(r.endSec, 20.0 + kTailPadSec, 1e-9);
        // A section at time 0 has nothing before it to omit, so the two
        // collapse and there is no ease-in to give.
        CHECK_NEAR(r.notesFromSec, 0.0, 1e-9);

        // A section INSIDE the first kLeadInSec seconds: startSec clamps to
        // 0 but notesFromSec must stay at the section, or the clamp would
        // quietly hand back the very notes the ease-in exists to drop.
        {
            std::vector<bard::ChartSection> early;
            bard::ChartSection e0, e1;
            e0.tick = 0;   e0.time = 0.0; e0.name = "Intro";
            e1.tick = 40;  e1.time = 1.0; e1.name = "Verse";
            early.push_back(e0);
            early.push_back(e1);
            const auto er = ResolveRange(early, 1, 1, 30.0);
            CHECK_NEAR(er.startSec, 0.0, 1e-9);      // clamped
            CHECK_NEAR(er.notesFromSec, 1.0, 1e-9);  // NOT clamped with it
            CHECK(er.notesFromSec >= er.startSec);
        }

        // Reversed picks are swapped rather than producing a dead range.
        r = ResolveRange(secs, 2, 0, songEnd);
        CHECK(r.startSection == 0);
        CHECK(r.endSection == 2);
        CHECK_NEAR(r.endSec, songEnd + kTailPadSec, 1e-9);

        // The LAST section runs to the song end, not to a section after it.
        r = ResolveRange(secs, 2, 2, songEnd);
        CHECK_NEAR(r.endSec, songEnd + kTailPadSec, 1e-9);

        // A songEnd that lands BEFORE the chosen section - a short
        // song_length, or an Outro marker sitting past the last note -
        // must not invert the range. A loop that ends before it starts
        // is worse than a short one.
        r = ResolveRange(secs, 2, 2, 5.0);
        CHECK(r.startSec <= r.endSec);
        CHECK_NEAR(r.startSec, 20.0 - kLeadInSec, 1e-9);
        CHECK_NEAR(r.endSec, 20.0 - kLeadInSec + kTailPadSec, 1e-9);

        // No markers, or a bad index, means the WHOLE song - practice
        // must never refuse to open.
        const std::vector<bard::ChartSection> none;
        r = ResolveRange(none, 0, 0, songEnd);
        CHECK(r.startSection == -1);
        CHECK(r.endSection == -1);
        CHECK_NEAR(r.startSec, 0.0, 1e-9);
        CHECK_NEAR(r.endSec, songEnd + kTailPadSec, 1e-9);
        r = ResolveRange(secs, -3, 99, songEnd);
        CHECK(r.startSection == -1);
        CHECK_NEAR(r.startSec, 0.0, 1e-9);

        // ...and the whole-song branch clamps the same way the section
        // branch does. a_songEndSec comes from unclamped, user-editable
        // ini fields, so a pathological delay can make it negative; an
        // inverted range would make a "restart when past endSec" caller
        // restart forever.
        r = ResolveRange(none, 0, 0, -10.0);
        CHECK(r.startSec <= r.endSec);
        CHECK_NEAR(r.startSec, 0.0, 1e-9);
        CHECK_NEAR(r.endSec, kTailPadSec, 1e-9);
    }

    {
        using namespace bard::practice;
        bard::ParsedChart chart;
        chart.resolution = 192;
        chart.offsetSeconds = 0.25;
        chart.tempo.SetResolution(192);
        chart.tempo.AddBpm(0, 120.0);
        chart.tempo.Finalize();
        chart.meta.name = "Song";

        auto note = [&](std::uint32_t tick, double time) {
            bard::Note n;
            n.tick = tick;
            n.time = time;
            n.mask = 1;
            chart.notes.push_back(n);
        };
        note(0, 1.0);     // before range
        note(192, 5.0);   // inside
        note(384, 6.0);   // inside
        note(768, 20.0);  // after range

        bard::SpPhrase inside;
        inside.startTick = 192; inside.endTick = 400;
        inside.noteCount = 2; inside.lastNoteIndex = 2;
        bard::SpPhrase straddling;
        straddling.startTick = 600; straddling.endTick = 900;
        straddling.noteCount = 1; straddling.lastNoteIndex = 3;
        chart.spPhrases = { inside, straddling };

        bard::SoloPhrase solo;
        solo.startTick = 192; solo.endTick = 384; solo.noteCount = 2;
        chart.solos = { solo };

        const auto out = SliceChart(chart, 4.0, 7.0);

        // Only the two notes inside the window survive.
        CHECK(out.notes.size() == 2);
        CHECK_NEAR(out.notes[0].time, 5.0, 1e-12);
        CHECK_NEAR(out.notes[1].time, 6.0, 1e-12);
        // Timing identity is copied, NEVER recomputed.
        CHECK(out.resolution == 192);
        CHECK_NEAR(out.offsetSeconds, 0.25, 1e-12);
        CHECK(out.meta.name == "Song");
        CHECK_NEAR(out.tempo.SecondsAt(192.0), 0.5, 1e-9);
        // A phrase fully inside is kept; a straddling one is dropped,
        // because awarding SP for a phrase whose start was never played
        // is a scoring lie.
        CHECK(out.spPhrases.size() == 1);
        CHECK(out.spPhrases[0].startTick == 192);
        CHECK(out.solos.size() == 1);
        // Sections are COPIED verbatim, which is what lets a practice HUD
        // still name them. This fixture declares none, so what is checked
        // here is that the copy of an empty vector is empty.
        CHECK(out.sections.empty());

        // A sustain crossing the end is KEPT, not truncated.
        bard::ParsedChart sus;
        sus.tempo.SetResolution(192);
        sus.tempo.AddBpm(0, 120.0);
        sus.tempo.Finalize();
        bard::Note held;
        held.tick = 192; held.time = 5.0; held.mask = 1;
        held.sustainTicks[0] = 960;
        held.sustainEnd[0] = 9.0;   // past the 7.0 end
        sus.notes.push_back(held);
        const auto susOut = SliceChart(sus, 4.0, 7.0);
        CHECK(susOut.notes.size() == 1);
        CHECK(susOut.notes[0].sustainTicks[0] == 960);
        CHECK_NEAR(susOut.notes[0].sustainEnd[0], 9.0, 1e-12);

        // An empty range yields an empty chart, not a crash.
        const auto none = SliceChart(chart, 100.0, 101.0);
        CHECK(none.notes.empty());
        CHECK(none.spPhrases.empty());
    }

    {
        // SP phrase note indices must be re-based onto the sliced note
        // vector: GuitarEngine awards the phrase AT lastNoteIndex, so a
        // stale index awards on the wrong note or reads out of bounds.
        using namespace bard::practice;
        bard::ParsedChart chart;
        chart.tempo.SetResolution(192);
        chart.tempo.AddBpm(0, 120.0);
        chart.tempo.Finalize();
        auto note = [&](std::uint32_t tick, double time) {
            bard::Note n;
            n.tick = tick; n.time = time; n.mask = 1;
            chart.notes.push_back(n);
        };
        note(0, 1.0);      // index 0, dropped by the slice
        note(192, 5.0);    // index 1 -> becomes 0
        note(384, 6.0);    // index 2 -> becomes 1
        bard::SpPhrase p;
        p.startTick = 192; p.endTick = 400;
        p.noteCount = 2; p.lastNoteIndex = 2;
        chart.spPhrases = { p };

        const auto out = SliceChart(chart, 4.0, 7.0);
        CHECK(out.spPhrases.size() == 1);
        CHECK(out.spPhrases[0].lastNoteIndex == 1);
        CHECK(out.spPhrases[0].noteCount == 2);
        CHECK(out.spPhrases[0].lastNoteIndex <
              static_cast<std::int32_t>(out.notes.size()));
    }

    {
        // The OTHER cross-reference: Note::spPhrase indexes spPhrases, so
        // dropping a phrase shifts every later note's index. GuitarEngine
        // sizes _phrases to spPhrases.size() and then indexes it with the
        // note's stored value UNCHECKED, so a stale index is an
        // out-of-bounds read AND write on the first SP note of the range.
        using namespace bard::practice;
        bard::ParsedChart chart;
        chart.tempo.SetResolution(192);
        chart.tempo.AddBpm(0, 120.0);
        chart.tempo.Finalize();
        // spPhrase is set exactly as Normalize sets it: the index of the
        // owning phrase in the ORIGINAL spPhrases, or -1 for none. It
        // defaults to -1, so it must be set explicitly or the test passes
        // for the wrong reason.
        auto note = [&](std::uint32_t tick, double time,
                        std::int32_t phrase) {
            bard::Note n;
            n.tick = tick; n.time = time; n.mask = 1;
            n.spPhrase = phrase;
            chart.notes.push_back(n);
        };
        note(0,   1.0, 0);   // index 0: OUTSIDE the window -> drops phrase 0
        note(192, 5.0, 0);   // index 1: survives, but its phrase does not
        note(384, 6.0, -1);  // index 2: no phrase at all
        note(576, 6.2, 1);   // index 3: survives, phrase 1 survives
        note(768, 6.5, 1);   // index 4: award note of phrase 1

        bard::SpPhrase dropped;   // original index 0
        dropped.startTick = 0; dropped.endTick = 200;
        dropped.noteCount = 2; dropped.lastNoteIndex = 1;
        bard::SpPhrase kept;      // original index 1 -> becomes 0
        kept.startTick = 576; kept.endTick = 800;
        kept.noteCount = 2; kept.lastNoteIndex = 4;
        chart.spPhrases = { dropped, kept };

        const auto out = SliceChart(chart, 4.0, 7.0);
        CHECK(out.notes.size() == 4);
        // Phrase 0 covers a note that fell outside, so it goes; phrase 1
        // is fully inside and survives at its new index 0.
        CHECK(out.spPhrases.size() == 1);
        CHECK(out.spPhrases[0].startTick == 576);
        CHECK(out.spPhrases[0].lastNoteIndex == 3);

        // ORPHAN: this note survived, its phrase did not. Left alone it
        // would still read 0 - now a VALID index that points at the wrong
        // phrase, silently awarding SP the player never earned.
        CHECK(out.notes[0].tick == 192);
        CHECK(out.notes[0].spPhrase == -1);
        // A note with no phrase keeps the sentinel; remapping must not
        // run -1 through the table.
        CHECK(out.notes[1].tick == 384);
        CHECK(out.notes[1].spPhrase == -1);
        // REMAP: 1 -> 0, and in range for the SLICED phrase vector. Left
        // alone these read 1, which is out of bounds for a 1-phrase chart.
        CHECK(out.notes[2].tick == 576);
        CHECK(out.notes[2].spPhrase == 0);
        CHECK(out.notes[2].spPhrase <
              static_cast<std::int32_t>(out.spPhrases.size()));
        CHECK(out.notes[3].tick == 768);
        CHECK(out.notes[3].spPhrase == 0);
        CHECK(out.notes[3].spPhrase <
              static_cast<std::int32_t>(out.spPhrases.size()));
    }

    {
        // Containment and the award index disagreeing is a corrupt chart,
        // not a judgement call: award on the wrong note and the player
        // gets Star Power from a phrase they never finished. Drop it - and
        // the notes that pointed at it must fall back to -1, not to the
        // index the phrase would have had.
        using namespace bard::practice;
        bard::ParsedChart chart;
        chart.tempo.SetResolution(192);
        chart.tempo.AddBpm(0, 120.0);
        chart.tempo.Finalize();
        auto note = [&](std::uint32_t tick, double time,
                        std::int32_t phrase) {
            bard::Note n;
            n.tick = tick; n.time = time; n.mask = 1;
            n.spPhrase = phrase;
            chart.notes.push_back(n);
        };
        note(0,   1.0, -1);  // outside the window
        note(192, 5.0, 0);   // inside
        bard::SpPhrase p;
        // Covers only tick 192, so containment KEEPS it...
        p.startTick = 192; p.endTick = 400; p.noteCount = 1;
        p.lastNoteIndex = 0;  // ...but awards on a note the slice dropped.
        chart.spPhrases = { p };

        auto out = SliceChart(chart, 4.0, 7.0);
        CHECK(out.notes.size() == 1);
        CHECK(out.spPhrases.empty());
        CHECK(out.notes[0].spPhrase == -1);

        // Same for an index that is not a note at all.
        chart.spPhrases[0].lastNoteIndex = 99;
        out = SliceChart(chart, 4.0, 7.0);
        CHECK(out.spPhrases.empty());
        CHECK(out.notes[0].spPhrase == -1);
    }

    {
        using namespace bard::practice;
        LoopParams lp;   // restartDelaySec defaults to 1.5

        // Not yet past the end: no restart.
        CHECK(!ShouldRestart(9.0, 10.0, 1.0, lp));
        // Past the end but still inside the delay.
        CHECK(!ShouldRestart(10.5, 10.0, 1.0, lp));
        // Past end + delay.
        CHECK(ShouldRestart(11.5, 10.0, 1.0, lp));
        // The delay SCALES with speed, so a slowed loop still gets a
        // proportional breath (YARG does the same). At 0.5x the delay is
        // 1.5 * 0.5 = 0.75 SONG-seconds - still 1.5s of wall time - so
        // the threshold moves from 11.5 down to 10.75.
        CHECK(!ShouldRestart(10.7, 10.0, 0.5, lp));
        CHECK(ShouldRestart(10.8, 10.0, 0.5, lp));
        // Same clock, different verdict at a different speed. Without
        // this the pair above would also hold for a fixed delay, and the
        // scaling would go untested.
        CHECK(!ShouldRestart(10.8, 10.0, 1.0, lp));
        // A nonsense speed must not corrupt the restart threshold. Far
        // past the end it restarts, as it would at any speed:
        CHECK(ShouldRestart(1000.0, 10.0, 0.0, lp));
        CHECK(ShouldRestart(1000.0, 10.0, -2.0, lp));
        // ...but THESE are the two that actually pin the guard. The pair
        // above passes with or without it; these fail without it, when
        // the threshold collapses to 10.0 (speed 0) or 7.0 (speed -2)
        // instead of holding at the normal-speed 11.5.
        CHECK(!ShouldRestart(10.5, 10.0, 0.0, lp));
        CHECK(!ShouldRestart(8.0, 10.0, -2.0, lp));

        // The seek target is the range start, never the raw section time.
        CHECK_NEAR(RestartTime(PracticeRange{ 4.0, 10.0, 1, 1 }), 4.0,
                   1e-12);

        // Practice records NOTHING. These are invariants, not defaults:
        // a practice run writing the ledger would corrupt the
        // per-difficulty records.
        constexpr auto rules = PracticeRules();
        static_assert(!rules.recordStars);
        static_assert(!rules.payGold);
        static_assert(!rules.feedExpertise);
        static_assert(!rules.allowFailure);
        static_assert(!rules.crowdReactions);
        static_assert(!rules.songEndStings);
        static_assert(!rules.clearNewTags);
        // Two commits the original seven missed, both found by auditing
        // the live session: the SGT mood globals that drive NPC
        // dialogue, and the rank-gate pass at session START that
        // mutates the persisted lifted mask.
        static_assert(!rules.commitGloryGlobals);
        static_assert(!rules.enforceRankGate);
        CHECK(!rules.recordStars);

        // The performance counterpart must be all-true, or a host that
        // resolves the mode once as `practice ? PracticeRules() :
        // PerformanceRules()` would silently disable a real feature. A
        // field added to the struct but forgotten in PerformanceRules
        // fails here immediately, which is the whole point of pinning
        // both directions rather than just the false one.
        constexpr auto live = PerformanceRules();
        static_assert(live.recordStars);
        static_assert(live.payGold);
        static_assert(live.feedExpertise);
        static_assert(live.allowFailure);
        static_assert(live.crowdReactions);
        static_assert(live.songEndStings);
        static_assert(live.clearNewTags);
        static_assert(live.commitGloryGlobals);
        static_assert(live.enforceRankGate);
        CHECK(live.recordStars);
    }
}

TEST_MAIN("Practice")
