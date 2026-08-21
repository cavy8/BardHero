// src/render/HighwayLayout.h
#pragma once

// PURE highway geometry (spec 9): no FUCK, no RE, no OS - headless-tested
// (HighwayLayoutTests, same pattern as InputMapper). The window composes
// these into draw calls; everything here is a function of (style, view,
// times) only, so the highway is time-linear by construction (SPIKE-3
// field rule: never per-frame increments).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "chart/ChartTypes.h"
#include "chart/TempoMap.h"
#include "render/IHighwayRenderer.h"

namespace SH::hw {

    // Fractions of screen W/H so every resolution lays out identically.
    struct Style {
        float strikeY      = 0.86f;   // strikeline y / screen h
        float horizonY     = 0.30f;   // highway top y / screen h
        float halfWStrike  = 0.19f;   // highway half-width at strikeline / w
        float halfWHorizon = 0.055f;  // ... at horizon / w
        float depthGain    = 2.2f;    // perspective strength (>1)
        float tailSec      = 0.25f;   // missed gems live this long past the line
        float gemHalfW     = 0.40f;   // gem half-width / lane spacing
        float gemSquash    = 0.62f;   // gem height / width (perspective ellipse)
        static Style Default() { return {}; }
    };

    struct View { float w = 1920.0f, h = 1080.0f; };

    // Perspective depth. u = secondsAhead / lookahead: z(0)=0 strikeline,
    // z(1)=1 horizon; near notes move depthGain x faster. u<0 (past the
    // line) extends linearly with the strikeline slope so misses slide off
    // at constant speed.
    float ZOf(double u, float depthGain);

    // Inverse of ZOf over the visible span.
    float UOfZ(float z, float depthGain);

    float YOf(const Style& s, const View& v, float z);
    float HalfWOf(const Style& s, const View& v, float z);      // px
    float LaneSpacing(const Style& s, const View& v, float z);  // center-to-center px
    float LaneX(const Style& s, const View& v, int lane, float z);  // lane 0..4

    struct SurfaceUv { float x = 0.0f, y = 0.0f; };
    float     HighwayBackgroundPhase(double visual, double lookahead);
    SurfaceUv HighwayBackgroundUvAt(const Style& s, const View& v,
                                    const V2& pixel, float phase);

    // Chart notes worth considering around visualTime: [first, last).
    // Backs up by maxSustainSec so a running trail's head note stays in.
    struct NoteRange { std::size_t first = 0, last = 0; };
    NoteRange VisibleNotes(const std::vector<bard::Note>& notes,
                           double visualTime, double lookahead,
                           double tailSec, double maxSustainSec);

    // Quarter-note lines (measure=false) and measure starts (measure=true)
    // with song-domain time in (t0, t1]. TimeSigs pick the grouping
    // (display-only, spec 4.5); offsetSeconds converts tempo<->song domain.
    struct BeatLine {
        double time    = 0.0;
        bool   measure = false;
    };
    void CollectBeatLines(const bard::TempoMap& tempo,
                          const std::vector<bard::TimeSig>& sigs,
                          double offsetSeconds, double t0, double t1,
                          std::vector<BeatLine>& out);

    // Countdown from InputTime during the kSongStartDelay lead-in:
    // ceil(-t), 0 once the song runs.
    int CountdownValue(double inputTime);

    // Juice helpers (2026-07-19). LastBeatTime: the newest quarter-note
    // time <= visual (song domain), or a far-past sentinel during the
    // lead-in - callers turn it into a pulse via exp decay. ApproachGlow:
    // 1 at/behind the strikeline fading to 0 by u = 0.12 (smoothstep).
    double LastBeatTime(const bard::TempoMap& tempo, double offsetSeconds,
                        double visual);
    float  ApproachGlow(double u);

    // Sustain trail from u0 (head) to u1, u = ahead/lookahead. held anchors
    // the base at the strikeline. Clipped to [held?0:u0, 1], subdivided for
    // the perspective bend. Returns segments written (<= maxSegs).
    struct TrailSeg { float z0 = 0, z1 = 0; float u0 = 0, u1 = 0; };
    int TrailSegments(double u0, double u1, bool held, float depthGain,
                      TrailSeg* out, int maxSegs);

    // Pause dim tiling (2026-07-25 field fix). The FLICK overlay host is
    // begun with zero window padding, so its draw-list clip insets from
    // the screen by the theme's WindowBorderSize - a single full-screen
    // window-list quad leaves a bright rim at the edges. Tile instead:
    // out[0] = inner rect for the window list (safely inside the clip),
    // out[1..4] = top/bottom/left/right strips for the UNCLIPPED
    // foreground list. Exact abutment, zero overlap: one alpha reads as
    // one uniform dim. margin clamps to [0, min(w,h)/4].
    struct DimRect { V2 mn, mx; };
    void PauseDimRects(const View& v, float margin, DimRect out[5]);

    // P3 lightning polyline: lateral node offsets in [-amp, amp],
    // deterministic in seed (re-seed per crackle tick for flicker).
    // First and last nodes pinned to 0 so a strike lands on target and
    // an edge arc rejoins its rail. n >= 2.
    void BoltOffsets(std::uint32_t seed, float amp, float* out, int n);

    // ---- emission helpers (RecordingRenderer-tested) --------------------
    // judgment: 0 pending, 1 hit (nothing drawn - the flash handles it),
    // 2 missed (grey, slides past the line).
    // One source of truth for gems, open bars and sustain trails. Misses
    // remain grey; active Star Power overrides every playable color; a
    // phrase-marked note (including its trail) uses that same cyan only while
    // its engine-owned phrase remains available. Once stripped, pending notes
    // return to their ordinary lane colors.
    RGBA VisualNoteColor(const bard::Note& n, int lane, bool missed,
                          bool spActive, bool spPhraseAvailable);
    RGBA BeatLineColor(bool measure, bool spActive, float z);
    // One gradient quad and one continuous rail quad per edge.
    void EmitSurface(const Style& s, const View& v, bool spActive, int combo,
                     IHighwayRenderer& r);
    void EmitGem(const Style& s, const View& v, const bard::Note& n, float z,
                  int judgment, bool spActive, bool spPhraseAvailable,
                  IHighwayRenderer& r);
    // One lane's trail (lane 5 = open: wide centered band). tint resolved
    // by the caller; per-segment horizon fade applied here.
    void EmitTrail(const Style& s, const View& v, int lane, double u0,
                   double u1, bool held, const RGBA& tint,
                   IHighwayRenderer& r, float wobblePhase = 0.0f,
                   float wobbleAmp = 0.0f);
}
