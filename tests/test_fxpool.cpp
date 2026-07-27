#include "harness.h"
#include "render/FxPool.h"

#include <vector>

using namespace SH::hw;

static int Count(const FxPool& p) {
    int n = 0;
    p.ForEach([&](const FxParticle&) { ++n; });
    return n;
}

static void RunTests() {
    {   // spawn recipes produce bounded, known counts
        FxPool p;
        p.HitBurst(100.0f, 500.0f, kLaneColors[0], 1.0f);
        // 1 ring + 1 glow + 6 sparks
        CHECK(Count(p) == 8);
        CHECK(p.Alive() == 8);
        p.SustainSpark(100.0f, 500.0f, kLaneColors[1], 1.0f);
        CHECK(Count(p) == 9);
        p.SpGlint(200.0f, 300.0f, 1.0f);
        CHECK(Count(p) == 10);
        p.PressKick(100.0f, 500.0f, kLaneColors[2], 1.0f);
        CHECK(Count(p) == 11);  // ring only
    }
    {   // aging kills; dt<=0 freezes
        FxPool p;
        p.SustainSpark(0.0f, 0.0f, kLaneColors[0], 1.0f);
        p.Update(0.0);
        CHECK(Count(p) == 1);  // frozen, still alive
        for (int i = 0; i < 200; ++i) p.Update(0.016);  // 3.2s >> any life
        CHECK(Count(p) == 0);
        CHECK(p.Alive() == 0);
    }
    {   // pool cap: overflow recycles, never exceeds kCap
        FxPool p;
        for (int i = 0; i < 60; ++i) {
            p.HitBurst(0.0f, 0.0f, kLaneColors[0], 1.0f);  // 60*8 = 480
        }
        CHECK(p.Alive() <= FxPool::kCap);
        CHECK(p.Alive() == FxPool::kCap);  // saturated
    }
    {   // gravity pulls sparks downward over time (vy increases)
        FxPool p;
        p.SustainSpark(0.0f, 0.0f, kLaneColors[0], 1.0f);
        float vy0 = 0;
        p.ForEach([&](const FxParticle& q) { vy0 = q.vy; });
        p.Update(0.1);
        float vy1 = 0;
        p.ForEach([&](const FxParticle& q) { vy1 = q.vy; });
        CHECK(vy1 > vy0);  // screen-space: +y is down
    }
    {   // determinism: same seed + same calls -> identical state
        FxPool a, b;
        a.HitBurst(10.0f, 20.0f, kLaneColors[2], 1.0f);
        b.HitBurst(10.0f, 20.0f, kLaneColors[2], 1.0f);
        a.Update(0.05);
        b.Update(0.05);
        bool same = true;
        std::vector<FxParticle> va, vb;
        a.ForEach([&](const FxParticle& q) { va.push_back(q); });
        b.ForEach([&](const FxParticle& q) { vb.push_back(q); });
        CHECK(va.size() == vb.size());
        for (std::size_t i = 0; i < va.size() && same; ++i) {
            same = va[i].x == vb[i].x && va[i].y == vb[i].y &&
                   va[i].vx == vb[i].vx && va[i].age == vb[i].age;
        }
        CHECK(same);
    }
    {   // Clear empties
        FxPool p;
        p.HitBurst(0.0f, 0.0f, kLaneColors[0], 1.0f);
        p.Clear();
        CHECK(Count(p) == 0);
    }
    {   // P2 FlameBurst: 3 flipbook tongues + 3 embers, lane-tinted,
        // and the central tongue covers the gem face (field fix: the
        // burst sizes from gemW, not fixed px)
        FxPool p;
        const float gemW = 200.0f;
        p.FlameBurst(100.0f, 500.0f, gemW, kLaneColors[3], 1.0f);
        CHECK(Count(p) == 6);
        int tongues = 0, embers = 0;
        bool centralCovers = false;
        p.ForEach([&](const FxParticle& q) {
            if (q.sprite == Sprite::kFlameFb0) {
                ++tongues;
                CHECK(q.fbFrames == kFlameFbFrames);
                if (q.x == 100.0f) {
                    // central tongue: drawn width 2*size >= gem width
                    CHECK(q.size0 * 2.0f >= gemW);
                    centralCovers = true;
                }
            } else if (q.sprite == Sprite::kEmber) {
                ++embers;
                CHECK(q.gravity > 0.0f);
            }
            CHECK(q.tint.b == kLaneColors[3].b);
        });
        CHECK(tongues == 3);
        CHECK(embers == 3);
        CHECK(centralCovers);
    }
    {   // P2 SustainFountain round 3: sparks ONLY - two embers per
        // tick, needle sometimes, and NO flame tongues (field: they
        // read as mini thin flames climbing the trail; the window's
        // contact plume owns the fire). Deterministic across the roll.
        FxPool a, b;
        for (int i = 0; i < 40; ++i) {
            a.SustainFountain(50.0f, 400.0f, kLaneColors[1], 1.0f);
            b.SustainFountain(50.0f, 400.0f, kLaneColors[1], 1.0f);
        }
        CHECK(a.Alive() == b.Alive());
        int needles = 0, tongues = 0, embers = 0;
        a.ForEach([&](const FxParticle& q) {
            if (q.sprite == Sprite::kNeedle) ++needles;
            if (q.sprite == Sprite::kFlameFb0) ++tongues;
            if (q.sprite == Sprite::kEmber) ++embers;
        });
        CHECK(tongues == 0);
        CHECK(embers == 80);
        CHECK(needles > 0);       // ~35% of 40 rolls
        CHECK(needles < 40);
    }
    {   // FlipbookFrame: clamped stepping, degenerate frames
        CHECK(FlipbookFrame(0.0f, 4) == 0);
        CHECK(FlipbookFrame(0.24f, 4) == 0);
        CHECK(FlipbookFrame(0.26f, 4) == 1);
        CHECK(FlipbookFrame(0.99f, 4) == 3);
        CHECK(FlipbookFrame(1.0f, 4) == 3);   // never past the last cell
        CHECK(FlipbookFrame(0.9f, 1) == 0);
        CHECK(FlipbookFrame(0.9f, 0) == 0);
    }
}

TEST_MAIN("FxPool")
