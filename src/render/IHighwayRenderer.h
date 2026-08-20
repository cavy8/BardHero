#pragma once

// The spec-9 renderer seam. PURE: no FUCK, no imgui, no OS - the layout
// module and its tests speak only this. FlickRenderer maps it to the FUCK
// ABI in game; RecordingRenderer backs the headless render tests.

#include <vector>

#include "render/AtlasUv.h"

namespace SH::hw {
    struct V2   { float x = 0, y = 0; };
    struct RGBA { float r = 1, g = 1, b = 1, a = 1; };

    // Runtime palette values. Defaults preserve the established BardHero
    // colors, while the single theme.ini can override them at UI startup.
    inline RGBA kLaneColors[5] = {
        { 0.22f, 0.80f, 0.28f, 1.0f },
        { 0.90f, 0.22f, 0.20f, 1.0f },
        { 0.95f, 0.83f, 0.18f, 1.0f },
        { 0.25f, 0.55f, 0.95f, 1.0f },
        { 0.95f, 0.55f, 0.15f, 1.0f },
    };
    inline RGBA kOpenColor    = { 0.72f, 0.40f, 0.95f, 1.0f };
    inline RGBA kMissGrey     = { 0.45f, 0.45f, 0.45f, 0.85f };
    inline RGBA kSpActiveCyan = { 0.10f, 0.92f, 1.00f, 1.0f };
    inline RGBA kHighwayGradient   = { 0.05f, 0.05f, 0.09f, 1.0f };
    inline RGBA kHighwayBorderLine = { 0.55f, 0.75f, 0.95f, 0.75f };
    inline RGBA kHighwayStrikeline = { 1.00f, 1.00f, 1.00f, 1.0f };
    inline RGBA kHighwayMeasureLine = { 1.00f, 1.00f, 1.00f, 0.34f };

    // Corner order everywhere: TL, TR, BR, BL (FUCK::DrawImageQuad order).
    class IHighwayRenderer {
    public:
        virtual ~IHighwayRenderer() = default;
        virtual void Quad(Sprite spr, const V2 p[4], const RGBA& tint) = 0;
        virtual void QuadFilled(const V2 p[4], const RGBA& c)          = 0;
        virtual void QuadUv(const UvRect& uv, const V2 p[4],
                            const RGBA& tint) = 0;
        virtual void ScreenRectFilled(const V2& mn, const V2& mx,
                                      const RGBA& c) = 0;
    };

    class RecordingRenderer final : public IHighwayRenderer {
    public:
        struct Op {
            bool textured = false;
            bool screen   = false;
            int  sprite   = -1;
            V2   p[4];
            RGBA c;
        };
        std::vector<Op> ops;

        void Quad(Sprite s, const V2 p[4], const RGBA& t) override {
            Op o;
            o.textured = true;
            o.sprite   = static_cast<int>(s);
            for (int i = 0; i < 4; ++i) o.p[i] = p[i];
            o.c = t;
            ops.push_back(o);
        }
        void QuadFilled(const V2 p[4], const RGBA& c) override {
            Op o;
            for (int i = 0; i < 4; ++i) o.p[i] = p[i];
            o.c = c;
            ops.push_back(o);
        }
        void QuadUv(const UvRect&, const V2 p[4],
                    const RGBA& t) override {
            Op o;
            o.textured = true;
            o.sprite   = -2;
            for (int i = 0; i < 4; ++i) o.p[i] = p[i];
            o.c = t;
            ops.push_back(o);
        }
        void ScreenRectFilled(const V2& mn, const V2& mx,
                              const RGBA& c) override {
            Op o;
            o.screen = true;
            o.p[0] = mn;
            o.p[1] = { mx.x, mn.y };
            o.p[2] = mx;
            o.p[3] = { mn.x, mx.y };
            o.c = c;
            ops.push_back(o);
        }
    };
}
