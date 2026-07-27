// src/render/IHighwayRenderer.h
#pragma once

// The spec-9 renderer seam. PURE: no FUCK, no imgui, no OS - the layout
// module and its tests speak only this. FlickRenderer maps it to the FUCK
// ABI in game; RecordingRenderer backs the headless render tests.

#include <vector>

#include "render/AtlasUv.h"

namespace SH::hw {
    struct V2   { float x = 0, y = 0; };
    struct RGBA { float r = 1, g = 1, b = 1, a = 1; };

    inline constexpr RGBA kLaneColors[5] = {
        { 0.22f, 0.80f, 0.28f, 1.0f },  // green
        { 0.90f, 0.22f, 0.20f, 1.0f },  // red
        { 0.95f, 0.83f, 0.18f, 1.0f },  // yellow
        { 0.25f, 0.55f, 0.95f, 1.0f },  // blue
        { 0.95f, 0.55f, 0.15f, 1.0f },  // orange
    };
    inline constexpr RGBA kOpenColor = { 0.72f, 0.40f, 0.95f, 1.0f };
    inline constexpr RGBA kMissGrey  = { 0.45f, 0.45f, 0.45f, 0.85f };
    inline constexpr RGBA kSpActiveCyan = { 0.10f, 0.92f, 1.00f, 1.0f };

    // Corner order everywhere: TL, TR, BR, BL (FUCK::DrawImageQuad order).
    class IHighwayRenderer {
    public:
        virtual ~IHighwayRenderer() = default;
        virtual void Quad(Sprite spr, const V2 p[4], const RGBA& tint) = 0;
        virtual void QuadFilled(const V2 p[4], const RGBA& c)          = 0;
        // Arbitrary atlas region (multi-cell banner strips).
        virtual void QuadUv(const UvRect& uv, const V2 p[4],
                            const RGBA& tint) = 0;
        // Axis-aligned rect on the UNCLIPPED screen-space list. In game
        // this is the FUCK foreground list: it escapes the overlay host's
        // border clip but also draws ABOVE every FLICK panel - reach for
        // it only where panel overlap cannot matter (pause-dim edge
        // strips within px of the screen border).
        virtual void ScreenRectFilled(const V2& mn, const V2& mx,
                                      const RGBA& c) = 0;
    };

    // Test double (spec 9): records calls for assertions.
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
            o.sprite   = -2;  // raw-uv marker
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
