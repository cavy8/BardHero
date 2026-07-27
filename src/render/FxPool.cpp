// src/render/FxPool.cpp
#include "render/FxPool.h"

#include <algorithm>
#include <cmath>

namespace SH::hw {
    namespace {
        constexpr float kPi = 3.14159265358979f;
    }

    std::uint32_t FxPool::Rand() {
        // xorshift32 - deterministic, plenty for confetti
        _rng ^= _rng << 13;
        _rng ^= _rng >> 17;
        _rng ^= _rng << 5;
        return _rng;
    }
    float FxPool::Rand01() {
        return static_cast<float>(Rand() & 0xFFFFFF) / 16777215.0f;
    }

    FxParticle* FxPool::Alloc() {
        // ring allocation: overflow recycles the oldest slot by position -
        // bounded by construction, never grows
        FxParticle* slot = &_p[_next];
        _next            = (_next + 1) % kCap;
        return slot;
    }

    void FxPool::Update(double dt) {
        if (dt <= 0.0) { return; }  // paused: particles freeze with the song
        const float fdt = static_cast<float>(dt);
        for (auto& p : _p) {
            if (!p.alive) { continue; }
            p.age += fdt;
            if (p.age >= p.life) {
                p.alive = false;
                continue;
            }
            p.vy += p.gravity * fdt;
            const float k = std::max(0.0f, 1.0f - p.drag * fdt);
            p.vx *= k;
            p.vy *= k;
            p.x += p.vx * fdt;
            p.y += p.vy * fdt;
        }
    }

    void FxPool::Clear() {
        for (auto& p : _p) { p.alive = false; }
        _next = 0;
    }

    int FxPool::Alive() const {
        int n = 0;
        for (const auto& p : _p) {
            if (p.alive) { ++n; }
        }
        return n;
    }

    void FxPool::HitBurst(float x, float y, const RGBA& lane, float scale) {
        {   // expanding ring
            auto* p    = Alloc();
            *p         = {};
            p->x       = x;
            p->y       = y;
            p->size0   = 26.0f * scale;
            p->size1   = 84.0f * scale;
            p->life    = 0.28f;
            p->sprite  = Sprite::kRing;
            p->tint    = lane;
            p->alive   = true;
        }
        {   // hot core glow
            auto* p    = Alloc();
            *p         = {};
            p->x       = x;
            p->y       = y;
            p->size0   = 48.0f * scale;
            p->size1   = 12.0f * scale;
            p->life    = 0.18f;
            p->sprite  = Sprite::kGlowDot;
            p->tint    = { 1.0f, 1.0f, 1.0f, 1.0f };
            p->alive   = true;
        }
        for (int i = 0; i < 6; ++i) {  // sparks, upward cone
            auto*       p   = Alloc();
            *p              = {};
            const float ang = -kPi / 2.0f +
                              (Rand01() - 0.5f) * (110.0f * kPi / 180.0f);
            const float spd = (260.0f + 160.0f * Rand01()) * scale;
            p->x       = x;
            p->y       = y;
            p->vx      = std::cos(ang) * spd;
            p->vy      = std::sin(ang) * spd;
            p->rot     = ang;
            p->size0   = 10.0f * scale;
            p->size1   = 3.0f * scale;
            p->life    = 0.35f + 0.20f * Rand01();
            p->gravity = 900.0f;
            p->drag    = 2.2f;
            p->sprite  = Sprite::kSpark;
            p->tint    = lane;
            p->alive   = true;
        }
    }

    void FxPool::PressKick(float x, float y, const RGBA& lane,
                           float scale) {
        auto* p   = Alloc();
        *p        = {};
        p->x      = x;
        p->y      = y;
        p->size0  = 18.0f * scale;
        p->size1  = 46.0f * scale;
        p->life   = 0.16f;
        p->sprite = Sprite::kRing;
        p->tint   = lane;
        p->alive  = true;
    }

    void FxPool::SustainSpark(float x, float y, const RGBA& lane,
                              float scale) {
        auto*       p   = Alloc();
        *p              = {};
        const float ang = -kPi / 2.0f +
                          (Rand01() - 0.5f) * (50.0f * kPi / 180.0f);
        const float spd = (90.0f + 60.0f * Rand01()) * scale;
        p->x       = x;
        p->y       = y;
        p->vx      = std::cos(ang) * spd;
        p->vy      = std::sin(ang) * spd;
        p->rot     = ang;
        p->size0   = 7.0f * scale;
        p->size1   = 2.0f * scale;
        p->life    = 0.30f;
        p->gravity = 500.0f;
        p->drag    = 1.5f;
        p->sprite  = Sprite::kSpark;
        p->tint    = lane;
        p->alive   = true;
    }

    void FxPool::FlameBurst(float x, float y, float gemW, const RGBA& lane,
                            float scale) {
        // GH look (field 2026-07-25): one central tongue spans ~1.25x
        // the gem face, two shoulder tongues fill the sides; every base
        // is anchored AT the strikeline (the drawer bottom-anchors
        // flipbook flames) so fire rises from the fret, not through it.
        {   // central tongue, gem-covering, plays in place
            auto* p     = Alloc();
            *p          = {};
            p->x        = x;
            p->y        = y;
            p->size0    = 0.62f * gemW * scale;
            p->size1    = 0.70f * gemW * scale;  // slight growth, no travel
            p->life     = 0.20f + 0.05f * Rand01();
            p->sprite   = Sprite::kFlameFb0;
            p->fbFrames = static_cast<std::uint8_t>(kFlameFbFrames);
            p->tint     = lane;
            p->alive    = true;
        }
        for (int i = 0; i < 2; ++i) {  // shoulder tongues, leaning out
            auto*       p    = Alloc();
            *p               = {};
            const float side = i == 0 ? -1.0f : 1.0f;
            p->x        = x + side * (0.30f + 0.06f * Rand01()) * gemW;
            p->y        = y;
            p->vx       = side * 30.0f * scale;
            p->vy       = -40.0f * scale;
            p->size0    = (0.30f + 0.08f * Rand01()) * gemW * scale;
            p->size1    = 0.12f * gemW * scale;
            p->life     = 0.15f + 0.06f * Rand01();
            p->sprite   = Sprite::kFlameFb0;
            p->fbFrames = static_cast<std::uint8_t>(kFlameFbFrames);
            p->tint     = lane;
            p->alive    = true;
        }
        for (int i = 0; i < 3; ++i) {  // embers popping upward
            auto*       p   = Alloc();
            *p              = {};
            const float ang = -kPi / 2.0f +
                              (Rand01() - 0.5f) * (80.0f * kPi / 180.0f);
            const float spd = (180.0f + 140.0f * Rand01()) * scale;
            p->x       = x;
            p->y       = y;
            p->vx      = std::cos(ang) * spd;
            p->vy      = std::sin(ang) * spd;
            p->size0   = (0.035f + 0.020f * Rand01()) * gemW * scale;
            p->size1   = 0.010f * gemW * scale;
            p->life    = 0.30f + 0.15f * Rand01();
            p->gravity = 750.0f;
            p->drag    = 1.8f;
            p->sprite  = Sprite::kEmber;
            p->tint    = lane;
            p->alive   = true;
        }
    }

    void FxPool::SustainFountain(float x, float y, const RGBA& lane,
                                 float scale) {
        // Field 2026-07-25 round 3: NO rising flame tongues here - they
        // read as "mini thin flames" climbing the trail center. GH holds
        // are ONE continuous fret flame (the window's flipbook contact
        // plume) plus a spark fountain; this recipe is the sparks only.
        for (int i = 0; i < 2; ++i) {  // arcing embers
            auto*       p   = Alloc();
            *p              = {};
            const float ang = -kPi / 2.0f +
                              (Rand01() - 0.5f) * (95.0f * kPi / 180.0f);
            const float spd = (120.0f + 110.0f * Rand01()) * scale;
            p->x       = x + (Rand01() - 0.5f) * 10.0f * scale;
            p->y       = y;
            p->vx      = std::cos(ang) * spd;
            p->vy      = std::sin(ang) * spd;
            p->size0   = (6.0f + 4.0f * Rand01()) * scale;
            p->size1   = 1.5f * scale;
            p->life    = 0.35f + 0.20f * Rand01();
            p->gravity = 650.0f;
            p->drag    = 1.4f;
            p->sprite  = Sprite::kEmber;
            p->tint    = lane;
            p->alive   = true;
        }
        if (Rand01() < 0.35f) {  // occasional fast needle
            auto*       p   = Alloc();
            *p              = {};
            const float ang = -kPi / 2.0f +
                              (Rand01() - 0.5f) * (40.0f * kPi / 180.0f);
            const float spd = (320.0f + 180.0f * Rand01()) * scale;
            p->x       = x;
            p->y       = y;
            p->vx      = std::cos(ang) * spd;
            p->vy      = std::sin(ang) * spd;
            p->rot     = ang;
            p->size0   = 9.0f * scale;
            p->size1   = 3.0f * scale;
            p->life    = 0.28f;
            p->gravity = 500.0f;
            p->drag    = 1.2f;
            p->sprite  = Sprite::kNeedle;
            p->tint    = lane;
            p->alive   = true;
        }
    }

    void FxPool::ConfettiBurst(float x, float y, float spread,
                               float scale) {
        // lane palette + gold + white, upward fan under gravity
        static constexpr RGBA kPal[7] = {
            kLaneColors[0], kLaneColors[1], kLaneColors[2],
            kLaneColors[3], kLaneColors[4],
            { 0.96f, 0.73f, 0.20f, 1.0f },  // gold
            { 0.97f, 0.97f, 0.94f, 1.0f },  // white
        };
        for (int i = 0; i < 26; ++i) {
            auto*       p   = Alloc();
            *p              = {};
            const float ang = -kPi / 2.0f +
                              (Rand01() - 0.5f) * (150.0f * kPi / 180.0f);
            const float spd = (240.0f + 240.0f * Rand01()) * scale;
            p->x       = x + (Rand01() - 0.5f) * spread;
            p->y       = y;
            p->vx      = std::cos(ang) * spd;
            p->vy      = std::sin(ang) * spd;
            p->rot     = Rand01() * 6.2831853f;
            p->size0   = (7.0f + 5.0f * Rand01()) * scale;
            p->size1   = (6.0f + 4.0f * Rand01()) * scale;
            p->life    = 1.1f + 0.6f * Rand01();
            p->gravity = 420.0f;
            p->drag    = 0.9f;
            p->sprite  = Sprite::kSolid;
            p->tint    = kPal[Rand() % 7];
            p->alive   = true;
        }
    }

    void FxPool::SpGlint(float x, float y, float scale) {
        auto* p    = Alloc();
        *p         = {};
        p->x       = x;
        p->y       = y;
        p->vy      = -30.0f - 25.0f * Rand01();
        p->size0   = 6.0f * scale;
        p->size1   = 20.0f * scale;
        p->life    = 0.90f;
        p->sprite  = Sprite::kStarGlint;
        p->tint    = kSpActiveCyan;
        p->tint.a  = 0.9f;
        p->alive   = true;
    }
}
