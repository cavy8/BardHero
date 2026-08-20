// src/render/AtlasUv.h
#pragma once

// Sprite cells of the highway atlas (tools/gen_atlas.py writes the PNG -
// keep the two in lockstep). 1024x1280 atlas, 8 cols x 10 rows of 128px
// cells (rows grew 8->10 for the P5 win strips; cell indices did NOT
// renumber - only the v scale changed), 6px UV inset against bleed. All
// sprites are white + alpha: tint at draw time.

namespace SH::hw {
    enum class Sprite : int {
        kGem          = 0,   // round gem puck, shaded bevel (tint = lane)
        kHopoCap      = 1,   // white inner-dot overlay on a gem
        kTapGem       = 2,   // flat bar gem (replaces kGem for taps)
        kOpenBar      = 3,   // full-width open-note bar (tint purple)
        kFretRing     = 4,   // strikeline fret, idle
        kFretPressed  = 5,   // strikeline fret, held
        kFlash        = 6,   // hit flash burst (6-arm star)
        kTrail        = 7,   // sustain trail strip (vertical band)
        kShimmer      = 8,   // SP streak
        // juice pack v1 (2026-07-19)
        kRing         = 9,   // thin expanding hit-burst ring
        kSpark        = 10,  // teardrop spark streak (rot = velocity angle)
        kGlowDot      = 11,  // soft radial glow (underglow/pulse/miss flash)
        kStarGlint    = 12,  // 4-point sparkle (SP ambience)
        kFlame        = 13,  // abstract energy plume (sustain contact)
        kGemUnderGlow = 14,  // wide soft halo beneath near gems
        // GH-feel spec P1.5 (2026-07-25)
        kSolid        = 15,  // uniform fill (pause dim, flashes)
        // GH-feel spec P3 (2026-07-25): lightning bolt segments (jagged
        // bright line along +x, drawn as oriented chained quads)
        kBolt0        = 30,
        kBolt1        = 31,
        kBolt2        = 38,  // NOT contiguous with the others - no math
        kTrailCap     = 39,  // rounded sustain-trail tail tip
        kDigit0       = 40,  // digits 0-9 occupy cells 40-49
        // GH-feel spec P2 (2026-07-25): hit/sustain flames
        kFlameFb0     = 50,  // flame flipbook, kFlameFbFrames cells 50-53
        kEmber        = 54,  // irregular ember blob (fountain)
        kNeedle       = 55,  // long thin streak (rot = velocity angle)
        // Highway floor depth-fade. FlickRenderer special-cases this sprite
        // to the full-resolution highway_fade.png; the atlas-sized version
        // quantizes into fixed horizontal bands when stretched.
        kHighwayFade  = 56,
    };

    inline constexpr int kFlameFbFrames = 4;

    // Bolt segment variant for chain index i (cells are NOT contiguous).
    inline Sprite BoltSprite(int i) {
        switch (i % 3) {
        case 1:  return Sprite::kBolt1;
        case 2:  return Sprite::kBolt2;
        default: return Sprite::kBolt0;
        }
    }

    struct UvRect { float u0, v0, u1, v1; };

    // Multi-cell banner text strips (whole-cell rows; see gen_atlas.py
    // STRIPS). No inset: strip edges are transparent by construction.
    // v units are ROW/10 since the P5 atlas growth.
    inline UvRect BannerReadyUv()  { return { 0.0f, 0.20f, 1.00f, 0.30f }; }
    inline UvRect BannerActiveUv() { return { 0.0f, 0.30f, 0.75f, 0.40f }; }
    inline UvRect BannerStreakUv() { return { 0.0f, 0.40f, 0.75f, 0.50f }; }
    // P5 win-screen catchphrases (full rows 8/9)
    inline UvRect WinGloriousUv()  { return { 0.0f, 0.80f, 1.00f, 0.90f }; }
    inline UvRect WinFlawlessUv()  { return { 0.0f, 0.90f, 1.00f, 1.00f }; }

    inline UvRect UvOf(Sprite s) {
        const int   i      = static_cast<int>(s);
        const float cellU  = 0.125f;          // 8 columns
        const float cellV  = 0.1f;            // 10 rows
        const float insetU = 6.0f / 1024.0f;
        const float insetV = 6.0f / 1280.0f;
        const float u      = static_cast<float>(i % 8) * cellU;
        const float v      = static_cast<float>(i / 8) * cellV;
        return { u + insetU, v + insetV,
                 u + cellU - insetU, v + cellV - insetV };
    }
}
