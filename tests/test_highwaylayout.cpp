#include "harness.h"

#include "chart/ChartTypes.h"
#include "chart/Normalize.h"
#include "chart/TempoMap.h"
#include "engine/EngineParams.h"
#include "engine/GuitarEngine.h"
#include "render/FlickWindowPolicy.h"
#include "render/HighwayLayout.h"
#include "render/BackdropLayout.h"
#include "render/IHighwayRenderer.h"

#include <cmath>
#include <vector>

using namespace SH::hw;

static bard::Note N(double time, std::uint8_t mask, bool hopo = false,
                    bool tap = false, std::int32_t sp = -1) {
    bard::Note n;
    n.time     = time;
    n.mask     = mask;
    n.isHopo   = hopo;
    n.isTap    = tap;
    n.spPhrase = sp;
    return n;
}

static void RunDepthTests() {
    // The overlay-only Highway still has a transparent ImGui host window.
    // Without kNoResize that empty default host exposes a lone resize-grip
    // triangle around its bottom-right corner.
    {
        constexpr unsigned noDecoration = 1u << 0;
        constexpr unsigned noBackground = 1u << 1;
        constexpr unsigned noMove       = 1u << 2;
        constexpr unsigned noResize     = 1u << 3;
        constexpr unsigned passInput    = 1u << 4;
        constexpr unsigned hideHud      = 1u << 5;
        constexpr unsigned blockVanity  = 1u << 6;
        constexpr unsigned renderTm     = 1u << 7;
        constexpr unsigned closeOnMenu  = 1u << 8;
        const unsigned orbitFlags = SH::flick_window_policy::HighwayHostFlags(
            noDecoration, noBackground, noMove, noResize, passInput,
            hideHud, blockVanity, true, renderTm, closeOnMenu);
        const unsigned fixedFlags = SH::flick_window_policy::HighwayHostFlags(
            noDecoration, noBackground, noMove, noResize, passInput,
            hideHud, blockVanity, false, renderTm, closeOnMenu);
        CHECK((orbitFlags & noResize) != 0);
        CHECK((orbitFlags & blockVanity) == 0);
        CHECK((fixedFlags & blockVanity) != 0);
    }

    const float k = Style::Default().depthGain;
    CHECK_NEAR(ZOf(0.0, k), 0.0, 1e-9);
    CHECK_NEAR(ZOf(1.0, k), 1.0, 1e-6);
    CHECK(ZOf(0.5, k) > 0.5f);  // perspective front-loads near motion
    float prev = -10.0f;
    for (double u = -0.5; u <= 1.0; u += 0.05) {  // monotonic
        const float z = ZOf(u, k);
        CHECK(z > prev);
        prev = z;
    }
    CHECK_NEAR(ZOf(-0.1, k), -k * 0.1, 1e-6);  // linear extension below

    // UOfZ inverts ZOf over the visible span.
    CHECK_NEAR(UOfZ(0.0f, k), 0.0, 1e-9);
    CHECK_NEAR(UOfZ(1.0f, k), 1.0, 1e-6);
    for (double u = 0.0; u <= 1.0; u += 0.05) {
        CHECK_NEAR(UOfZ(ZOf(u, k), k), u, 1e-5);
    }
}

static void RunLaneTests() {
    const Style s = Style::Default();
    const View  v{ 1920, 1080 };
    CHECK_NEAR(YOf(s, v, 0.0f), 1080.0 * s.strikeY, 1e-3);
    CHECK_NEAR(YOf(s, v, 1.0f), 1080.0 * s.horizonY, 1e-3);
    CHECK_NEAR(LaneX(s, v, 2, 0.0f), 960.0, 1e-3);  // center lane centered
    // symmetry + equal spacing at the strikeline
    CHECK_NEAR(LaneX(s, v, 0, 0.0f) + LaneX(s, v, 4, 0.0f), 1920.0, 1e-2);
    const float d01 = LaneX(s, v, 1, 0.0f) - LaneX(s, v, 0, 0.0f);
    const float d34 = LaneX(s, v, 4, 0.0f) - LaneX(s, v, 3, 0.0f);
    CHECK_NEAR(d01, d34, 1e-3);
    CHECK(HalfWOf(s, v, 1.0f) < HalfWOf(s, v, 0.0f));  // converges
}

static void RunBackgroundUvTests() {
    const Style s = Style::Default();
    const View  v{ 1920, 1080 };
    CHECK_NEAR(HighwayBackgroundPhase(2.5, 2.0), 0.25f, 1e-6);
    CHECK_NEAR(HighwayBackgroundPhase(-0.5, 2.0), 0.75f, 1e-6);
    CHECK_NEAR(HighwayBackgroundPhase(3.0, 0.0), 0.0f, 1e-6);

    const float phase = 0.25f;
    const float cx = v.w * 0.5f;
    const auto strikeCenter = HighwayBackgroundUvAt(
        s, v, { cx, YOf(s, v, 0.0f) }, phase);
    CHECK_NEAR(strikeCenter.x, 0.5f, 1e-6);
    CHECK_NEAR(strikeCenter.y, 0.75f, 1e-6);

    const auto horizonCenter = HighwayBackgroundUvAt(
        s, v, { cx, YOf(s, v, 1.0f) }, phase);
    CHECK_NEAR(horizonCenter.x, 0.5f, 1e-6);
    CHECK_NEAR(horizonCenter.y, -0.25f, 1e-6);

    for (const float z : { 0.0f, 0.5f, 1.0f }) {
        const float y = YOf(s, v, z);
        const float hw = HalfWOf(s, v, z);
        const auto left = HighwayBackgroundUvAt(
            s, v, { cx - hw, y }, phase);
        const auto right = HighwayBackgroundUvAt(
            s, v, { cx + hw, y }, phase);
        CHECK_NEAR(left.x, 0.0f, 1e-5);
        CHECK_NEAR(right.x, 1.0f, 1e-5);
        CHECK_NEAR(left.y, 1.0f - UOfZ(z, s.depthGain) - phase, 1e-5);
        CHECK_NEAR(right.y, left.y, 1e-6);
    }

    // Pixel-based UVs remain continuous across the quad diagonal.
    const V2 diagMid{
        (cx - HalfWOf(s, v, 1.0f) + cx + HalfWOf(s, v, 0.0f)) * 0.5f,
        (YOf(s, v, 1.0f) + YOf(s, v, 0.0f)) * 0.5f
    };
    const auto a = HighwayBackgroundUvAt(
        s, v, { diagMid.x - 0.1f, diagMid.y + 0.1f }, phase);
    const auto b = HighwayBackgroundUvAt(
        s, v, { diagMid.x + 0.1f, diagMid.y - 0.1f }, phase);
    CHECK(std::abs(a.x - b.x) < 0.002f);
    CHECK(std::abs(a.y - b.y) < 0.002f);
}

static void RunWindowTests() {
    std::vector<bard::Note> notes;
    for (int i = 0; i < 10; ++i) notes.push_back(N(1.0 * i, 0x01));
    // visual=5, lookahead=1, tail=0.25, maxSus=2 -> lo=2.75 (first idx 3),
    // hi=6.05 -> last exclusive idx 7
    const auto r = VisibleNotes(notes, 5.0, 1.0, 0.25, 2.0);
    CHECK(r.first == 3);
    CHECK(r.last == 7);
    const auto all = VisibleNotes(notes, 0.0, 100.0, 0.25, 0.0);
    CHECK(all.first == 0 && all.last == 10);
}

static void RunBeatLineTests() {
    bard::TempoMap tempo;  // default 120 BPM, res 192
    tempo.SetResolution(192);
    tempo.Finalize();
    std::vector<BeatLine> out;
    CollectBeatLines(tempo, {}, 0.0, 0.0, 2.1, out);
    // (0, 2.1] at 120 BPM: beats 0.5, 1.0, 1.5, 2.0
    CHECK(out.size() == 4);
    CHECK_NEAR(out[0].time, 0.5, 1e-9);
    CHECK_NEAR(out[3].time, 2.0, 1e-9);
    CHECK(!out[0].measure && !out[1].measure && !out[2].measure);
    CHECK(out[3].measure);  // tick 768 = 4/4 measure boundary

    // TimeSig pick must be order-INDEPENDENT (ParsedChart::timeSigs has no
    // sort guarantee): the later-tick sig listed FIRST must still only
    // apply from its tick on. 4/4 below tick 768 (t=2.0), 3/4 above.
    const std::vector<bard::TimeSig> sigs = { { 768, 3, 4 }, { 0, 4, 4 } };
    CollectBeatLines(tempo, sigs, 0.0, -0.1, 3.6, out);
    // beats: ticks 0,192,...,1344 -> t 0.0, 0.5, ..., 3.5
    CHECK(out.size() == 8);
    CHECK_NEAR(out[0].time, 0.0, 1e-9);
    CHECK_NEAR(out[7].time, 3.5, 1e-9);
    CHECK(out[0].measure);   // tick 0: the 4/4 sig at tick 0 applies below 768
    CHECK(!out[1].measure && !out[2].measure && !out[3].measure);
    CHECK(out[4].measure);   // tick 768: the 3/4 sig's own start
    CHECK(!out[5].measure && !out[6].measure);
    CHECK(out[7].measure);   // tick 1344 = 768+576: 3/4 won above 768 (an
                             // order-dependent last-in-list pick would use
                             // the tick-0 4/4 sig here and flag tick 1536
                             // instead)
}

static void RunCountdownTests() {
    CHECK(CountdownValue(-2.0) == 2);
    CHECK(CountdownValue(-0.01) == 1);
    CHECK(CountdownValue(0.0) == 0);
    CHECK(CountdownValue(10.0) == 0);
}

static void RunBackdropTests() {
    const auto integer = SH::backdrop::FullBleed(1920.0f, 1080.0f);
    CHECK(integer.loX < 0.0f);
    CHECK(integer.loY < 0.0f);
    CHECK(integer.hiX > 1920.0f);
    CHECK(integer.hiY > 1080.0f);

    // Fractional display sizes occur after FLICK resolution/user scaling.
    // Every edge must still be overscanned, with no rounded foreground rule.
    const auto scaled = SH::backdrop::FullBleed(1478.4f, 1439.6f);
    CHECK(scaled.loX <= -1.0f);
    CHECK(scaled.loY <= -1.0f);
    CHECK(scaled.hiX >= 1480.0f);
    CHECK(scaled.hiY >= 1441.0f);
    CHECK_NEAR(scaled.rounding, 0.0f, 1e-6);
}

static void RunTrailSegTests() {
    const float k = Style::Default().depthGain;
    TrailSeg segs[12];
    // held: base clamps to the strikeline
    int n = TrailSegments(-0.5, 0.5, true, k, segs, 12);
    CHECK(n >= 1);
    CHECK_NEAR(segs[0].z0, 0.0, 1e-6);
    // not held: base keeps its (negative) position
    n = TrailSegments(-0.25, 0.5, false, k, segs, 12);
    CHECK(n >= 1);
    CHECK(segs[0].z0 < 0.0f);
    // fully past: nothing
    CHECK(TrailSegments(-2.0, -1.0, false, k, segs, 12) == 0);
}

static void RunEmitTests() {
    const Style s = Style::Default();
    const View  v{ 1920, 1080 };
    {   // The highway floor uses one continuous shape per edge.
        RecordingRenderer r;
        EmitSurface(s, v, false, 0, r);
        CHECK(r.ops.size() == 3);
        CHECK(r.ops[0].textured);
        CHECK(r.ops[0].sprite ==
              static_cast<int>(Sprite::kHighwayFade));
        CHECK(!r.ops[1].textured);
        CHECK(!r.ops[2].textured);
        for (const auto& op : r.ops) {
            CHECK_NEAR(op.p[0].y, YOf(s, v, 1.0f), 1e-3);
            CHECK_NEAR(op.p[3].y, YOf(s, v, 0.0f), 1e-3);
        }
    }
    {   // Theme-driven surface colors.
        const RGBA oldGradient = kHighwayGradient;
        const RGBA oldRail = kHighwayBorderLine;
        kHighwayGradient = RGBA{ 0.12f, 0.18f, 0.24f, 0.66f };
        kHighwayBorderLine = RGBA{ 0.90f, 0.40f, 0.20f, 0.50f };

        RecordingRenderer r;
        EmitSurface(s, v, false, 0, r);
        CHECK(r.ops.size() == 3);
        CHECK_NEAR(r.ops[0].c.r, kHighwayGradient.r, 1e-6);
        CHECK_NEAR(r.ops[0].c.a, kHighwayGradient.a, 1e-6);
        CHECK_NEAR(r.ops[1].c.r, kHighwayBorderLine.r, 1e-6);
        CHECK_NEAR(r.ops[1].c.g, kHighwayBorderLine.g, 1e-6);
        CHECK_NEAR(r.ops[1].c.a, kHighwayBorderLine.a, 1e-6);

        kHighwayGradient = oldGradient;
        kHighwayBorderLine = oldRail;
    }
    {   // Beat lines derive from the measure-line theme color.
        const RGBA oldMeasure = kHighwayMeasureLine;
        kHighwayMeasureLine = RGBA{ 0.80f, 0.60f, 0.40f, 0.50f };

        const RGBA measure = BeatLineColor(true, false, 0.0f);
        const RGBA beat = BeatLineColor(false, false, 0.0f);
        CHECK_NEAR(measure.r, kHighwayMeasureLine.r, 1e-6);
        CHECK_NEAR(measure.g, kHighwayMeasureLine.g, 1e-6);
        CHECK_NEAR(measure.b, kHighwayMeasureLine.b, 1e-6);
        CHECK_NEAR(measure.a, 0.50f, 1e-6);
        CHECK_NEAR(beat.a, 0.50f * (0.18f / 0.34f), 1e-6);

        const RGBA oldSp = kSpActiveCyan;
        kSpActiveCyan = RGBA{ 0.10f, 0.90f, 1.00f, 1.0f };
        const RGBA spMeasure = BeatLineColor(true, true, 0.0f);
        CHECK_NEAR(spMeasure.r, kSpActiveCyan.r, 1e-6);
        CHECK_NEAR(spMeasure.a, kHighwayMeasureLine.a, 1e-6);
        kSpActiveCyan = oldSp;
        kHighwayMeasureLine = oldMeasure;
    }
    {   // pending green single at the strikeline
        RecordingRenderer r;
        EmitGem(s, v, N(0.0, 0x01), 0.0f, 0, false, false, r);
        CHECK(r.ops.size() == 1);
        CHECK(r.ops[0].textured &&
              r.ops[0].sprite == static_cast<int>(Sprite::kGem));
        const float cy = (r.ops[0].p[0].y + r.ops[0].p[2].y) * 0.5f;
        CHECK_NEAR(cy, 1080.0 * s.strikeY, 1e-2);
        CHECK_NEAR(r.ops[0].c.g, kLaneColors[0].g, 1e-6);
    }
    {   // hit: nothing; missed: grey
        RecordingRenderer r;
        EmitGem(s, v, N(0.0, 0x01), 0.0f, 1, false, false, r);
        CHECK(r.ops.empty());
        EmitGem(s, v, N(0.0, 0x01), 0.0f, 2, false, false, r);
        CHECK(r.ops.size() == 1);
        CHECK_NEAR(r.ops[0].c.r, kMissGrey.r, 1e-6);
    }
    {   // hopo adds a cap; tap swaps the sprite; chord emits per lane
        RecordingRenderer r;
        EmitGem(s, v, N(0.0, 0x01, true), 0.0f, 0, false, false, r);
        CHECK(r.ops.size() == 2);
        CHECK(r.ops[1].sprite == static_cast<int>(Sprite::kHopoCap));
        r.ops.clear();
        EmitGem(s, v, N(0.0, 0x01, false, true), 0.0f, 0, false, false, r);
        CHECK(r.ops.size() == 1);
        CHECK(r.ops[0].sprite == static_cast<int>(Sprite::kTapGem));
        r.ops.clear();
        EmitGem(s, v, N(0.0, 0x03), 0.0f, 0, false, false,
                r);  // G+R chord
        CHECK(r.ops.size() == 2);
    }
    {   // open note spans most of the highway; SP phrase goes cyan
        RecordingRenderer r;
        EmitGem(s, v, N(0.0, bard::kOpenBit), 0.0f, 0, false, false, r);
        CHECK(r.ops.size() == 1);
        const float w = r.ops[0].p[1].x - r.ops[0].p[0].x;
        CHECK(w > 3.0f * LaneSpacing(s, v, 0.0f));
        r.ops.clear();
        EmitGem(s, v, N(0.0, 0x01, false, false, 0), 0.0f, 0, false,
                true, r);
        CHECK_NEAR(r.ops[0].c.r, kSpActiveCyan.r, 1e-6);
        CHECK_NEAR(r.ops[0].c.g, kSpActiveCyan.g, 1e-6);
        CHECK_NEAR(r.ops[0].c.b, kSpActiveCyan.b, 1e-6);
        // The same visual-color seam drives long-note trails. A marked
        // sustain is cyan while charging, matching the active Star Power
        // treatment. Active power also turns ordinary/open notes cyan.
        const auto phrase = N(0.0, 0x01, false, false, 0);
        CHECK_NEAR(VisualNoteColor(phrase, 0, false, false, true).g,
                   kSpActiveCyan.g, 1e-6);
        CHECK_NEAR(VisualNoteColor(phrase, 0, false, true, true).g,
                   kSpActiveCyan.g, 1e-6);
        const auto open = N(0.0, bard::kOpenBit);
        CHECK_NEAR(VisualNoteColor(open, bard::kOpenLane, false, true, false).b,
                   kSpActiveCyan.b, 1e-6);
        CHECK_NEAR(VisualNoteColor(phrase, 0, true, true, true).r,
                   kMissGrey.r, 1e-6);
    }
    {   // held trail is anchored at the strikeline
        RecordingRenderer r;
        EmitTrail(s, v, 0, -0.5, 0.5, true, kLaneColors[0], r);
        CHECK(!r.ops.empty());
        float maxY = 0;
        for (const auto& op : r.ops) maxY = std::max(maxY, op.p[2].y);
        CHECK_NEAR(maxY, YOf(s, v, 0.0f), 1e-2);
    }
    {   // visible tail end gets a rounded cap (both layers), sitting on
        // the trail's far edge and extending toward the horizon
        RecordingRenderer r;
        EmitTrail(s, v, 2, -0.2, 0.5, true, kLaneColors[2], r);
        CHECK(r.ops.size() >= 4);
        const auto& coreCap = r.ops[r.ops.size() - 1];
        const auto& haloCap = r.ops[r.ops.size() - 2];
        CHECK(coreCap.sprite == static_cast<int>(Sprite::kTrailCap));
        CHECK(haloCap.sprite == static_cast<int>(Sprite::kTrailCap));
        const float yTail = YOf(s, v, ZOf(0.5, s.depthGain));
        CHECK_NEAR(coreCap.p[2].y, yTail, 1e-2);   // base = trail end
        CHECK(coreCap.p[0].y < coreCap.p[2].y);    // dome points away
        CHECK(haloCap.p[1].x - haloCap.p[0].x >
              coreCap.p[1].x - coreCap.p[0].x);    // halo wider than core
    }
    {   // tail clipped at the horizon: no cap to round
        RecordingRenderer r;
        EmitTrail(s, v, 2, 0.2, 2.0, false, kLaneColors[2], r);
        CHECK(!r.ops.empty());
        for (const auto& op : r.ops) {
            CHECK(op.sprite != static_cast<int>(Sprite::kTrailCap));
        }
    }
}

static void RunJuiceHelperTests() {
    bard::TempoMap tempo;  // default 120 BPM, res 192 -> beats every 0.5s
    tempo.SetResolution(192);
    tempo.Finalize();
    // newest beat at or before visual
    CHECK_NEAR(LastBeatTime(tempo, 0.0, 1.1), 1.0, 1e-9);
    CHECK_NEAR(LastBeatTime(tempo, 0.0, 0.5), 0.5, 1e-9);
    CHECK_NEAR(LastBeatTime(tempo, 0.0, 0.0), 0.0, 1e-9);
    // lead-in (before the first beat): far-past sentinel = pulse decayed
    CHECK(LastBeatTime(tempo, 0.0, -0.3) < -1e6);
    // offset shifts the song domain
    CHECK_NEAR(LastBeatTime(tempo, 0.25, 1.30), 1.25, 1e-9);

    // approach glow: 1 at the strikeline, 0 outside 0.12, monotonic
    CHECK_NEAR(ApproachGlow(0.0), 1.0f, 1e-6);
    CHECK_NEAR(ApproachGlow(-0.5), 1.0f, 1e-6);
    CHECK_NEAR(ApproachGlow(0.12), 0.0f, 1e-6);
    CHECK_NEAR(ApproachGlow(0.5), 0.0f, 1e-6);
    const float a = ApproachGlow(0.03), b = ApproachGlow(0.06),
                c = ApproachGlow(0.09);
    CHECK(1.0f > a && a > b && b > c && c > 0.0f);
}

static void RunFailedSpPhraseColorTests() {
    bard::RawTrack track;
    for (int i = 0; i < 4; ++i) {
        bard::RawChord note;
        note.tick = static_cast<std::uint32_t>(i * 192);
        note.mask = bard::LaneBit(i);
        track.chords.push_back(note);
    }
    track.spPhrases.push_back({ 0, 576 });
    track.spPhrases.push_back({ 576, 768 });
    bard::TempoMap tempo;
    tempo.SetResolution(192);
    tempo.Finalize();
    const auto chart = bard::Normalize(track, tempo, 0.0);

    bard::GuitarEngine engine(chart, bard::EngineParams::Default());
    CHECK(engine.SpPhraseAvailableFor(1));

    // Let the first phrase note pass its back window. Notes 2 and 3 are
    // still future/pending, but their phrase can no longer be earned.
    engine.Update(0.20);
    CHECK(engine.JudgmentOf(0) == bard::Judgment::kMissed);
    CHECK(engine.JudgmentOf(1) == bard::Judgment::kPending);
    CHECK(!engine.SpPhraseAvailableFor(1));

    const auto tint = VisualNoteColor(
        chart.notes[1], 1, false, false, engine.SpPhraseAvailableFor(1));
    CHECK_NEAR(tint.r, kLaneColors[1].r, 1e-6);
    CHECK_NEAR(tint.g, kLaneColors[1].g, 1e-6);
    CHECK_NEAR(tint.b, kLaneColors[1].b, 1e-6);
    const auto activeTint = VisualNoteColor(
        chart.notes[1], 1, false, true, engine.SpPhraseAvailableFor(1));
    CHECK_NEAR(activeTint.r, kSpActiveCyan.r, 1e-6);

    // Failure is phrase-local. A later phrase still advertises as cyan.
    CHECK(engine.SpPhraseAvailableFor(3));
    const auto laterTint = VisualNoteColor(
        chart.notes[3], 3, false, false, engine.SpPhraseAvailableFor(3));
    CHECK_NEAR(laterTint.r, kSpActiveCyan.r, 1e-6);
}

static void RunPauseDimTests() {
    // The five rects must tile the screen exactly: no overlap, no gap.
    // Sum-of-areas == screen area catches both at once given each rect
    // stays inside the screen.
    const auto area = [](const DimRect& r) {
        return (r.mx.x - r.mn.x) * (r.mx.y - r.mn.y);
    };
    for (const View v : { View{ 1920.0f, 1080.0f },
                          View{ 3440.0f, 1440.0f },
                          View{ 800.0f, 600.0f } }) {
        DimRect r[5];
        PauseDimRects(v, 16.0f, r);
        float total = 0.0f;
        for (int i = 0; i < 5; ++i) {
            CHECK(r[i].mn.x >= 0.0f && r[i].mn.y >= 0.0f);
            CHECK(r[i].mx.x <= v.w && r[i].mx.y <= v.h);
            CHECK(r[i].mx.x >= r[i].mn.x && r[i].mx.y >= r[i].mn.y);
            total += area(r[i]);
        }
        CHECK_NEAR(total, v.w * v.h, 0.5f);
        // strips abut the inner rect exactly (same alpha = no seam)
        CHECK_NEAR(r[1].mx.y, r[0].mn.y, 1e-6);  // top strip bottom
        CHECK_NEAR(r[2].mn.y, r[0].mx.y, 1e-6);  // bottom strip top
        CHECK_NEAR(r[3].mx.x, r[0].mn.x, 1e-6);  // left strip right
        CHECK_NEAR(r[4].mn.x, r[0].mx.x, 1e-6);  // right strip left
    }

    // An absurd margin clamps instead of inverting the inner rect.
    {
        DimRect r[5];
        const View v{ 1920.0f, 1080.0f };
        PauseDimRects(v, 10000.0f, r);
        CHECK(r[0].mx.x > r[0].mn.x);
        CHECK(r[0].mx.y > r[0].mn.y);
        float total = 0.0f;
        for (int i = 0; i < 5; ++i) total += area(r[i]);
        CHECK_NEAR(total, v.w * v.h, 0.5f);
    }

    // RecordingRenderer sees ScreenRectFilled as a screen-space op with
    // the rect's four corners in TL,TR,BR,BL order.
    {
        RecordingRenderer rec;
        rec.ScreenRectFilled({ 1.0f, 2.0f }, { 5.0f, 7.0f },
                             RGBA{ 0, 0, 0, 0.86f });
        CHECK(rec.ops.size() == 1);
        CHECK(rec.ops[0].screen);
        CHECK(!rec.ops[0].textured);
        CHECK_NEAR(rec.ops[0].p[1].x, 5.0f, 1e-6);
        CHECK_NEAR(rec.ops[0].p[3].y, 7.0f, 1e-6);
        CHECK_NEAR(rec.ops[0].c.a, 0.86f, 1e-6);
    }
}

static void RunBoltTests() {
    // ends pinned so strikes land on target / arcs rejoin their rail
    float a[9], b[9];
    BoltOffsets(1234u, 40.0f, a, 9);
    CHECK_NEAR(a[0], 0.0f, 1e-9);
    CHECK_NEAR(a[8], 0.0f, 1e-9);
    bool nonzero = false;
    for (int i = 1; i < 8; ++i) {
        CHECK(a[i] >= -40.0f && a[i] <= 40.0f);
        if (a[i] != 0.0f) nonzero = true;
    }
    CHECK(nonzero);
    // deterministic per seed, different across seeds
    BoltOffsets(1234u, 40.0f, b, 9);
    for (int i = 0; i < 9; ++i) CHECK(a[i] == b[i]);
    BoltOffsets(1235u, 40.0f, b, 9);
    bool differs = false;
    for (int i = 1; i < 8; ++i) {
        if (a[i] != b[i]) differs = true;
    }
    CHECK(differs);
    // degenerate sizes stay in bounds
    float one[1] = { 99.0f };
    BoltOffsets(7u, 40.0f, one, 1);
    CHECK_NEAR(one[0], 0.0f, 1e-9);
    // a zero seed still produces jitter (xorshift fixpoint guard)
    BoltOffsets(0u, 40.0f, b, 9);
    nonzero = false;
    for (int i = 1; i < 8; ++i) {
        if (b[i] != 0.0f) nonzero = true;
    }
    CHECK(nonzero);

    // bolt segment cells are non-contiguous: the variant helper must
    // cycle 30/31/38 and never do arithmetic past kBolt1
    CHECK(BoltSprite(0) == Sprite::kBolt0);
    CHECK(BoltSprite(1) == Sprite::kBolt1);
    CHECK(BoltSprite(2) == Sprite::kBolt2);
    CHECK(BoltSprite(3) == Sprite::kBolt0);
    CHECK(static_cast<int>(BoltSprite(2)) == 38);
}

static void RunAtlasUvTests() {
    // The P5 atlas growth (8 -> 10 rows) rescaled every v: cell v = 0.1,
    // inset v = 6/1280. Indices did not renumber - lock the mapping.
    const auto solid = UvOf(Sprite::kSolid);  // cell 15 = col 7, row 1
    CHECK_NEAR(solid.u0, 0.875f + 6.0f / 1024.0f, 1e-6);
    CHECK_NEAR(solid.v0, 0.1f + 6.0f / 1280.0f, 1e-6);
    CHECK_NEAR(solid.v1, 0.2f - 6.0f / 1280.0f, 1e-6);
    const auto d9 = UvOf(static_cast<Sprite>(49));  // digit 9: col 1 row 6
    CHECK_NEAR(d9.u0, 0.125f + 6.0f / 1024.0f, 1e-6);
    CHECK_NEAR(d9.v0, 0.6f + 6.0f / 1280.0f, 1e-6);
    CHECK_NEAR(BannerReadyUv().v0, 0.20f, 1e-6);
    CHECK_NEAR(BannerActiveUv().v1, 0.40f, 1e-6);
    CHECK_NEAR(BannerStreakUv().v1, 0.50f, 1e-6);
    CHECK_NEAR(WinGloriousUv().v0, 0.80f, 1e-6);
    CHECK_NEAR(WinGloriousUv().v1, 0.90f, 1e-6);
    CHECK_NEAR(WinFlawlessUv().v1, 1.00f, 1e-6);
}

static void RunTests() {
    RunDepthTests();
    RunLaneTests();
    RunBackgroundUvTests();
    RunWindowTests();
    RunBeatLineTests();
    RunCountdownTests();
    RunBackdropTests();
    RunTrailSegTests();
    RunEmitTests();
    RunJuiceHelperTests();
    RunFailedSpPhraseColorTests();
    RunPauseDimTests();
    RunBoltTests();
    RunAtlasUvTests();
}

TEST_MAIN("HighwayLayout")
