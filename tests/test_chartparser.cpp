#include "harness.h"
#include "chart/ChartParser.h"

using namespace bard;

// Minimal synthetic chart exercising every 4.3 rule. Raw string, parsed from
// memory - golden-file corpus tests with real CH charts stay LOCAL (spec 11).
static const char* kChart = R"([Song]
{
  Resolution = 192
  Offset = 1.5
  Name = "Test "Song""
}
[SyncTrack]
{
  0 = TS 4
  0 = B 120000
  384 = B 240000
  384 = TS 7 3
  500 = A 12345
}
[Events]
{
}
[ExpertSingle]
{
  0 = N 0 0
  100 = N 1 0
  100 = N 5 0
  200 = N 2 0
  200 = N 6 0
  300 = N 7 0
  400 = N 0 300
  400 = N 1 150
  700 = N 3 0
  700 = S 2 100
  800 = N 4 0
  800 = S 64 100
  900 = E solo
  960 = N 0 0
  1060 = N 1 0
  1100 = E soloend
}
)";

static void RunTests() {
    ChartFile cf;
    CHECK(ParseChartText(kChart, cf));

    {   // [Song]: resolution, offset (seconds), quote stripping (outer only)
        CHECK(cf.resolution == 192);
        CHECK_NEAR(cf.offsetSeconds, 1.5, 1e-9);
        CHECK(cf.meta.name == "Test \"Song\"");
    }
    {   // SyncTrack: B in millibpm; TS denom = power-of-2 exponent, default 2;
        // A anchors ignored at play time
        CHECK(cf.tempo.Resolution() == 192);
        CHECK_NEAR(cf.tempo.SecondsAt(384), 1.0, 1e-9);
        CHECK(cf.timeSigs.size() == 2);
        CHECK(cf.timeSigs[0].num == 4 && cf.timeSigs[0].denom == 4);
        CHECK(cf.timeSigs[1].num == 7 && cf.timeSigs[1].denom == 8);  // 2^3
    }

    RawTrack t;
    CHECK(BuildRawTrack(cf, "ExpertSingle", t));
    {   // note grouping + modifiers: N5 flip, N6 tap (overrides), N7 open
        CHECK(t.chords.size() == 9);
        CHECK(t.chords[0].mask == LaneBit(0));
        CHECK(t.chords[1].forcing == Forcing::kFlip);
        CHECK(t.chords[2].tap);
        CHECK(t.chords[3].mask == kOpenBit);
        // per-lane sustains, disjoint chord (400: G 300 ticks, R 150)
        CHECK(t.chords[4].sustainTicks[0] == 300);
        CHECK(t.chords[4].sustainTicks[1] == 150);
        // hopo threshold: floor(65/192 * 192) = 65 (spec 4.3)
        CHECK(t.hopoThresholdTicks == 65);
    }
    {   // S 2 = SP phrase; S 64 (drums activation) ignored on guitar
        CHECK(t.spPhrases.size() == 1);
        CHECK(t.spPhrases[0].startTick == 700 && t.spPhrases[0].endTick == 800);
    }
    {   // E solo / E soloend, end tick inclusive
        CHECK(t.solos.size() == 1);
        CHECK(t.solos[0].startTick == 900 && t.solos[0].endTick == 1100);
    }
    {   // 480-resolution chart: threshold floor(65/192*480) = 162
        ChartFile cf2;
        CHECK(ParseChartText("[Song]\n{\nResolution = 480\n}\n"
                             "[ExpertSingle]\n{\n0 = N 0 0\n}\n",
                             cf2));
        RawTrack t2;
        CHECK(BuildRawTrack(cf2, "ExpertSingle", t2));
        CHECK(t2.hopoThresholdTicks == 162);
    }
    {   // missing tempo marker at 0 -> 120 BPM
        ChartFile cf3;
        CHECK(ParseChartText(
            "[Song]\n{\n}\n[ExpertSingle]\n{\n192 = N 0 0\n}\n", cf3));
        CHECK_NEAR(cf3.tempo.SecondsAt(192), 0.5, 1e-9);
    }
    {   // E end honored: recorded for end_events truncation
        ChartFile cf4;
        CHECK(ParseChartText(
            "[Song]\n{\n}\n[ExpertSingle]\n{\n0 = N 0 0\n500 = E end\n}\n",
            cf4));
        RawTrack t4;
        CHECK(BuildRawTrack(cf4, "ExpertSingle", t4));
        CHECK(t4.hasEnd && t4.endTick == 500);
    }
}

TEST_MAIN("ChartParser")
