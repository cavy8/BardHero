// src/render/HighwayLayout.cpp
#include "render/HighwayLayout.h"

#include <algorithm>
#include <cmath>

namespace SH::hw {

    float ZOf(double u, float depthGain) {
        const double k = depthGain;
        if (u <= 0.0) return static_cast<float>(k * u);
        return static_cast<float>(k * u / (1.0 + (k - 1.0) * u));
    }

    float UOfZ(float z, float depthGain) {
        const double k = depthGain;
        if (z <= 0.0f) return static_cast<float>(z / k);  // ZOf's u<=0 leg
        // z = k*u / (1 + (k-1)*u)  =>  u = z / (k - (k-1)*z).
        // k > 1 keeps the denominator >= 1 over z in [0,1].
        return static_cast<float>(z / (k - (k - 1.0) * z));
    }

    float YOf(const Style& s, const View& v, float z) {
        return v.h * (s.strikeY + (s.horizonY - s.strikeY) * z);
    }

    float HalfWOf(const Style& s, const View& v, float z) {
        return v.w * (s.halfWStrike + (s.halfWHorizon - s.halfWStrike) * z);
    }

    float LaneSpacing(const Style& s, const View& v, float z) {
        return 2.0f * HalfWOf(s, v, z) / 5.0f;
    }

    float LaneX(const Style& s, const View& v, int lane, float z) {
        return v.w * 0.5f +
               (static_cast<float>(lane) - 2.0f) * LaneSpacing(s, v, z);
    }

    float HighwayBackgroundPhase(double visual, double lookahead) {
        if (lookahead <= 0.0) return 0.0f;
        float phase = static_cast<float>(std::fmod(visual / lookahead, 1.0));
        if (phase < 0.0f) phase += 1.0f;
        return phase;
    }

    SurfaceUv HighwayBackgroundUvAt(const Style& s, const View& v,
                                    const V2& pixel, float phase) {
        const float strikeY = YOf(s, v, 0.0f);
        const float horizonY = YOf(s, v, 1.0f);
        const float denom = horizonY - strikeY;
        const float z = denom == 0.0f ? 0.0f : std::clamp(
            (pixel.y - strikeY) / denom, 0.0f, 1.0f);
        const float halfW = HalfWOf(s, v, z);
        const float x = halfW > 0.0f
            ? 0.5f + (pixel.x - v.w * 0.5f) / (2.0f * halfW)
            : 0.5f;
        const float u = UOfZ(z, s.depthGain);
        return { x, 1.0f - u - phase };
    }

    NoteRange VisibleNotes(const std::vector<bard::Note>& notes,
                           double visualTime, double lookahead,
                           double tailSec, double maxSustainSec) {
        const double lo = visualTime - tailSec - maxSustainSec;
        const double hi = visualTime + lookahead * 1.05;
        auto cmp = [](const bard::Note& n, double t) { return n.time < t; };
        const auto b = std::lower_bound(notes.begin(), notes.end(), lo, cmp);
        const auto e = std::lower_bound(notes.begin(), notes.end(), hi, cmp);
        return { static_cast<std::size_t>(b - notes.begin()),
                 static_cast<std::size_t>(e - notes.begin()) };
    }

    void CollectBeatLines(const bard::TempoMap& tempo,
                          const std::vector<bard::TimeSig>& sigs,
                          double offsetSeconds, double t0, double t1,
                          std::vector<BeatLine>& out) {
        out.clear();
        if (t1 <= t0) return;
        const double res = static_cast<double>(tempo.Resolution());
        // clamp BEFORE TickAt: negative seconds walk TempoMap's lookup off
        // the front of its marker array (UB), and t0 IS negative during the
        // kSongStartDelay lead-in. Ticks below 0 don't exist anyway.
        const double tick0 =
            std::max(0.0, tempo.TickAt(std::max(0.0, t0 - offsetSeconds)));
        // first beat tick strictly after tick0's floor
        std::int64_t beat =
            static_cast<std::int64_t>(std::floor(tick0 / res));
        for (;; ++beat) {
            const double tick = static_cast<double>(beat) * res;
            if (tick < 0.0) continue;
            const double t = tempo.SecondsAt(tick) + offsetSeconds;
            if (t <= t0) continue;
            if (t > t1) break;
            // measure grouping from the LATEST TimeSig at or before tick.
            // Order-independent: ParsedChart::timeSigs carries no sort
            // guarantee, so keep the max-tick eligible sig, not the last
            // one listed (ties keep the later listing, TempoMap-style).
            std::uint32_t sigTick = 0, num = 4, denom = 4;
            for (const auto& ts : sigs) {
                if (static_cast<double>(ts.tick) <= tick &&
                    ts.tick >= sigTick) {
                    sigTick = ts.tick;
                    num     = ts.num;
                    denom   = ts.denom ? ts.denom : 4;
                }
            }
            const double measureTicks =
                static_cast<double>(num) * res * (4.0 / denom);
            const double rel = tick - static_cast<double>(sigTick);
            const bool   measure =
                measureTicks > 0.0 &&
                std::fmod(rel, measureTicks) < 1e-6;
            out.push_back({ t, measure });
            // runaway guard: a frame's window never needs more lines than
            // this; defends against a caller passing a huge span.
            if (out.size() >= 512) break;
        }
    }

    int CountdownValue(double inputTime) {
        if (inputTime >= 0.0) return 0;
        return static_cast<int>(std::ceil(-inputTime));
    }

    int TrailSegments(double u0, double u1, bool held, float depthGain,
                      TrailSeg* out, int maxSegs) {
        double s0 = held ? std::max(u0, 0.0) : u0;
        double s1 = std::min(u1, 1.0);
        // s1<=0: the whole trail (head AND tail) has already scrolled past
        // the strikeline - nothing left ahead of it worth drawing. A
        // not-held trail with s0<0<s1 (partially past) still draws (the
        // ahead portion), only fully-past trails are dropped.
        if (s1 <= s0 || s1 <= 0.0 || maxSegs <= 0) return 0;
        const int n = std::clamp(
            static_cast<int>(std::ceil((s1 - s0) * 8.0)), 1, maxSegs);
        for (int i = 0; i < n; ++i) {
            const double a = s0 + (s1 - s0) * i / n;
            const double b = s0 + (s1 - s0) * (i + 1) / n;
            out[i] = { ZOf(a, depthGain), ZOf(b, depthGain),
                       static_cast<float>(a), static_cast<float>(b) };
        }
        return n;
    }

    namespace {
        void GemQuad(const Style& s, const View& v, int lane, float z,
                     Sprite spr, const RGBA& tint, IHighwayRenderer& r) {
            const float sp = LaneSpacing(s, v, z);
            const float hx = s.gemHalfW * sp;
            const float hy = hx * s.gemSquash;
            const float x  = LaneX(s, v, lane, z);
            const float y  = YOf(s, v, z);
            const V2 p[4] = { { x - hx, y - hy }, { x + hx, y - hy },
                              { x + hx, y + hy }, { x - hx, y + hy } };
            r.Quad(spr, p, tint);
        }
    }

    RGBA VisualNoteColor(const bard::Note& n, int lane, bool missed,
                          bool spActive, bool spPhraseAvailable) {
        if (missed) { return kMissGrey; }
        if (spActive) { return kSpActiveCyan; }
        if (n.spPhrase >= 0 && spPhraseAvailable) { return kSpActiveCyan; }
        return lane == bard::kOpenLane ? kOpenColor : kLaneColors[lane];
    }

    void EmitSurface(const Style& s, const View& v, bool spActive, int combo,
                     IHighwayRenderer& r) {
        const float cx = v.w * 0.5f;
        const V2 floor[4] = {
            { cx - HalfWOf(s, v, 1.0f), YOf(s, v, 1.0f) },
            { cx + HalfWOf(s, v, 1.0f), YOf(s, v, 1.0f) },
            { cx + HalfWOf(s, v, 0.0f), YOf(s, v, 0.0f) },
            { cx - HalfWOf(s, v, 0.0f), YOf(s, v, 0.0f) },
        };
        r.Quad(Sprite::kHighwayFade, floor,
               spActive ? RGBA{ 0.02f, 0.14f, 0.17f, 1.0f }
                        : RGBA{ 0.05f, 0.05f, 0.09f, 1.0f });

        const float streak =
            0.6f * (std::min(combo, 50) / 50.0f);
        RGBA rail = spActive ? RGBA{ 0.10f, 0.92f, 1.00f, 0.92f }
                             : RGBA{ 0.55f, 0.75f, 0.95f, 0.75f };
        rail.r += (1.0f - rail.r) * streak;
        rail.g += (1.0f - rail.g) * streak;
        rail.b += (1.0f - rail.b) * streak;

        for (int side = -1; side <= 1; side += 2) {
            const float nearEdge = cx + side * HalfWOf(s, v, 0.0f);
            const float farEdge  = cx + side * HalfWOf(s, v, 1.0f);
            const float nearHalfWidth =
                std::max(2.0f, 0.03f * HalfWOf(s, v, 0.0f));
            const float farHalfWidth =
                std::max(2.0f, 0.03f * HalfWOf(s, v, 1.0f));
            const V2 edge[4] = {
                { farEdge - farHalfWidth, YOf(s, v, 1.0f) },
                { farEdge + farHalfWidth, YOf(s, v, 1.0f) },
                { nearEdge + nearHalfWidth, YOf(s, v, 0.0f) },
                { nearEdge - nearHalfWidth, YOf(s, v, 0.0f) },
            };
            r.QuadFilled(edge, rail);
        }
    }

    void EmitGem(const Style& s, const View& v, const bard::Note& n, float z,
                  int judgment, bool spActive, bool spPhraseAvailable,
                  IHighwayRenderer& r) {
        if (judgment == 1) return;  // hit: gone (flash covers the moment)
        const bool missed = judgment == 2;
        if (n.mask & bard::kOpenBit) {
            // full-width open bar
            const RGBA tint =
                VisualNoteColor(n, bard::kOpenLane, missed, spActive,
                                spPhraseAvailable);
            const float y   = YOf(s, v, z);
            const float hw  = HalfWOf(s, v, z) * 0.92f;
            const float hy  = 0.22f * LaneSpacing(s, v, z);
            const float cx  = v.w * 0.5f;
            const V2 p[4] = { { cx - hw, y - hy }, { cx + hw, y - hy },
                              { cx + hw, y + hy }, { cx - hw, y + hy } };
            r.Quad(Sprite::kOpenBar, p, tint);
            return;
        }
        for (int lane = 0; lane < 5; ++lane) {
            if (!(n.mask & bard::LaneBit(lane))) continue;
            const RGBA tint = VisualNoteColor(
                n, lane, missed, spActive, spPhraseAvailable);
            const Sprite base = n.isTap ? Sprite::kTapGem : Sprite::kGem;
            GemQuad(s, v, lane, z, base, tint, r);
            if (n.isHopo && !n.isTap && !missed) {
                GemQuad(s, v, lane, z, Sprite::kHopoCap,
                        spActive ? kSpActiveCyan : RGBA{ 1, 1, 1, 1 }, r);
            }
        }
    }

    void EmitTrail(const Style& s, const View& v, int lane, double u0,
                   double u1, bool held, const RGBA& tint,
                   IHighwayRenderer& r, float wobblePhase,
                   float wobbleAmp) {
        TrailSeg segs[12];
        const int n = TrailSegments(u0, u1, held, s.depthGain, segs, 12);
        for (int i = 0; i < n; ++i) {
            const auto& sg = segs[i];
            float x0, x1, w0, w1;
            if (lane == bard::kOpenLane) {
                x0 = x1 = v.w * 0.5f;
                w0 = 2.0f * LaneSpacing(s, v, sg.z0);
                w1 = 2.0f * LaneSpacing(s, v, sg.z1);
            } else {
                x0 = LaneX(s, v, lane, sg.z0);
                x1 = LaneX(s, v, lane, sg.z1);
                w0 = 0.35f * LaneSpacing(s, v, sg.z0);
                w1 = 0.35f * LaneSpacing(s, v, sg.z1);
            }
            // Whammy shiver, second field correction 2026-07-25: in GH
            // the CORE of a long note never moves - the outer flesh
            // bulges SYMMETRICALLY around it, like a string's standing-
            // wave envelope. So whammy modulates halo WIDTH only, never
            // lateral position: fast phase, short ripples, strongest at
            // the strikeline, damped to nothing at the horizon, per-lane
            // phase offsets so chords do not breathe in lockstep.
            // Third field correction: no strikeline damping (GH breathes
            // over the WHOLE trail), and a long wavelength (z*12, about
            // half the highway) - the old z*40 ripples aliased against
            // the 12-segment subdivision near the strikeline, which read
            // as the flesh leaning to one side.
            float bulge0 = 1.0f, bulge1 = 1.0f;
            if (wobbleAmp > 0.0f) {
                const float lanePhase = static_cast<float>(lane) * 1.7f;
                bulge0 += wobbleAmp * 0.85f
                    * (0.5f + 0.5f * std::sin(
                           wobblePhase + sg.z0 * 12.0f + lanePhase));
                bulge1 += wobbleAmp * 0.85f
                    * (0.5f + 0.5f * std::sin(
                           wobblePhase + sg.z1 * 12.0f + lanePhase));
            }
            const float y0 = YOf(s, v, sg.z0), y1 = YOf(s, v, sg.z1);
            RGBA c = tint;
            c.a *= 1.0f - 0.45f * sg.z0;  // horizon fade
            // Two layers replace the old flat band (it read as a plain
            // ribbon): a wide soft halo plus a hot narrow core. Layered
            // alpha stands in for additive bloom in this renderer.
            RGBA halo = c;
            halo.a *= held ? 0.40f : 0.26f;
            const float hw0 = w0 * 1.75f * bulge0;
            const float hw1 = w1 * 1.75f * bulge1;
            const V2 ph[4] = { { x1 - hw1, y1 }, { x1 + hw1, y1 },
                               { x0 + hw0, y0 }, { x0 - hw0, y0 } };
            r.Quad(Sprite::kTrail, ph, halo);
            RGBA core = c;
            if (held) {
                core.r += (1.0f - core.r) * 0.55f;
                core.g += (1.0f - core.g) * 0.55f;
                core.b += (1.0f - core.b) * 0.55f;
                core.a = std::min(1.0f, core.a * 1.25f);
            } else {
                core.a *= 0.85f;
            }
            const float cw0 = w0 * 0.55f, cw1 = w1 * 0.55f;
            const V2 p[4] = { { x1 - cw1, y1 }, { x1 + cw1, y1 },
                              { x0 + cw0, y0 }, { x0 - cw0, y0 } };
            r.Quad(Sprite::kTrail, p, core);
        }

        // Rounded tail tip (field 2026-07-25: the far end cut off hard).
        // Emitted only when the tail itself is on the highway - a tail
        // clipped at the horizon (u1 > 1) has no visible end to round.
        // Both layers get a kTrailCap dome whose base width matches the
        // trail end exactly (cap cell shares the trail's horizontal
        // profile, so the seam is alpha-continuous). The head end stays
        // square: the gem or the hit flames cover it.
        if (n > 0 && u1 <= 1.0) {
            const auto& sg = segs[n - 1];
            const float sp1 = LaneSpacing(s, v, sg.z1);
            float x1c, w1c;
            if (lane == bard::kOpenLane) {
                x1c = v.w * 0.5f;
                w1c = 2.0f * sp1;
            } else {
                x1c = LaneX(s, v, lane, sg.z1);
                w1c = 0.35f * sp1;
            }
            float bulge1 = 1.0f;
            if (wobbleAmp > 0.0f) {
                const float lanePhase = static_cast<float>(lane) * 1.7f;
                bulge1 += wobbleAmp * 0.85f
                    * (0.5f + 0.5f * std::sin(
                           wobblePhase + sg.z1 * 12.0f + lanePhase));
            }
            const float y1 = YOf(s, v, sg.z1);
            RGBA c = tint;
            c.a *= 1.0f - 0.45f * sg.z1;
            RGBA halo = c;
            halo.a *= held ? 0.40f : 0.26f;
            const float hw1  = w1c * 1.75f * bulge1;
            // dome height: proportionate for lane trails, clamped so the
            // wide open-note bar gets gently rounded corners instead of
            // a huge dome
            const float capH = std::min(hw1 * 0.7f, 1.2f * sp1);
            const V2 ph[4] = { { x1c - hw1, y1 - capH },
                               { x1c + hw1, y1 - capH },
                               { x1c + hw1, y1 }, { x1c - hw1, y1 } };
            r.Quad(Sprite::kTrailCap, ph, halo);
            RGBA core = c;
            if (held) {
                core.r += (1.0f - core.r) * 0.55f;
                core.g += (1.0f - core.g) * 0.55f;
                core.b += (1.0f - core.b) * 0.55f;
                core.a = std::min(1.0f, core.a * 1.25f);
            } else {
                core.a *= 0.85f;
            }
            const float cw1c  = w1c * 0.55f;
            const float capHc = std::min(cw1c * 0.7f, 1.2f * sp1);
            const V2 pc[4] = { { x1c - cw1c, y1 - capHc },
                               { x1c + cw1c, y1 - capHc },
                               { x1c + cw1c, y1 }, { x1c - cw1c, y1 } };
            r.Quad(Sprite::kTrailCap, pc, core);
        }
    }
}

namespace SH::hw {
    double LastBeatTime(const bard::TempoMap& tempo, double offsetSeconds,
                        double visual) {
        const double songT = visual - offsetSeconds;
        if (songT < 0.0) { return -1.0e9; }  // lead-in: no beat yet
        const double res  = static_cast<double>(tempo.Resolution());
        const double tick = std::max(0.0, tempo.TickAt(songT));
        std::int64_t beat =
            static_cast<std::int64_t>(std::floor(tick / res));
        // TickAt/SecondsAt rounding can land the floored beat just past
        // visual - walk down until it fits
        for (; beat >= 0; --beat) {
            const double t =
                tempo.SecondsAt(static_cast<double>(beat) * res) +
                offsetSeconds;
            if (t <= visual + 1e-9) { return t; }
        }
        return -1.0e9;
    }

    float ApproachGlow(double u) {
        if (u <= 0.0) { return 1.0f; }
        if (u >= 0.12) { return 0.0f; }
        const double t = 1.0 - u / 0.12;
        return static_cast<float>(t * t * (3.0 - 2.0 * t));
    }

    void BoltOffsets(std::uint32_t seed, float amp, float* out, int n) {
        if (n <= 0) { return; }
        std::uint32_t s = seed ? seed : 0x9E3779B9u;
        for (int i = 0; i < n; ++i) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            const float r01 =
                static_cast<float>(s & 0xFFFFFF) / 16777215.0f;
            out[i] = (r01 * 2.0f - 1.0f) * amp;
        }
        out[0] = 0.0f;
        if (n > 1) { out[n - 1] = 0.0f; }
    }

    void PauseDimRects(const View& v, float margin, DimRect out[5]) {
        const float m = std::clamp(margin, 0.0f,
                                   std::min(v.w, v.h) * 0.25f);
        // inner rect (window list)
        out[0] = { { m, m }, { v.w - m, v.h - m } };
        // strips (foreground list): full-width top/bottom, the side
        // strips fill exactly between them
        out[1] = { { 0.0f, 0.0f }, { v.w, m } };            // top
        out[2] = { { 0.0f, v.h - m }, { v.w, v.h } };       // bottom
        out[3] = { { 0.0f, m }, { m, v.h - m } };           // left
        out[4] = { { v.w - m, m }, { v.w, v.h - m } };      // right
    }
}
