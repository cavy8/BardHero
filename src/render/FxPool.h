// src/render/FxPool.h
#pragma once

// PURE particle pool for the highway juice pack (no RE/FUCK/OS -
// headless-tested, suite FxPoolTests). Screen-space px; +y is DOWN, so
// "up" spawns use negative vy and gravity is positive. Deterministic:
// fixed-seed xorshift - two pools fed identical calls stay identical.

#include <cstdint>

#include "render/AtlasUv.h"
#include "render/IHighwayRenderer.h"

namespace SH::hw {
    struct FxParticle {
        float  x = 0, y = 0, vx = 0, vy = 0;
        float  size0 = 0, size1 = 0;   // px at birth/death (lerp by age)
        float  rot = 0;                // radians (spark orientation)
        float  age = 0, life = 1;
        float  gravity = 0, drag = 0;  // px/s^2, 1/s
        Sprite sprite = Sprite::kGlowDot;
        // >1: sprite is the FIRST cell of a flipbook this many frames
        // long; the drawer advances by normalized age (FlipbookFrame)
        std::uint8_t fbFrames = 0;
        RGBA   tint;
        bool   alive = false;
    };

    // Flipbook cell for normalized age t in [0,1]: 0..frames-1, clamped
    // so t = 1 never reads past the last frame.
    inline int FlipbookFrame(float t, int frames) {
        if (frames <= 1) { return 0; }
        const int f = static_cast<int>(t * static_cast<float>(frames));
        return f < frames ? (f > 0 ? f : 0) : frames - 1;
    }

    class FxPool {
    public:
        static constexpr int kCap = 256;

        void Update(double dt);  // caller clamps; <= 0 freezes (pause)
        void Clear();

        // spawn recipes
        void HitBurst(float x, float y, const RGBA& lane, float scale);
        void PressKick(float x, float y, const RGBA& lane, float scale);
        void SustainSpark(float x, float y, const RGBA& lane, float scale);
        void SpGlint(float x, float y, float scale);
        // GH-feel spec P2: lane-tinted flame burst on every hit (flipbook
        // tongues + embers), and one fountain tick at a held sustain's
        // contact point (rising tongue + arcing ember + chance needle).
        // gemW = the gem's full drawn width in px: the burst must cover
        // the whole note face (field 2026-07-25 - fixed px read as thin
        // wisps at high resolutions).
        void FlameBurst(float x, float y, float gemW, const RGBA& lane,
                        float scale);
        void SustainFountain(float x, float y, const RGBA& lane,
                             float scale);
        // GH-feel spec P5: festive kSolid confetti fan (results window).
        // spread = horizontal emitter width in px; drawers spin by age.
        void ConfettiBurst(float x, float y, float spread, float scale);

        template <typename Fn> void ForEach(Fn&& fn) const {
            for (const auto& p : _p) {
                if (p.alive) { fn(p); }
            }
        }
        int Alive() const;

    private:
        FxParticle*   Alloc();
        std::uint32_t Rand();
        float         Rand01();

        FxParticle    _p[kCap]{};
        int           _next = 0;
        std::uint32_t _rng  = 0x9E3779B9u;
    };
}
