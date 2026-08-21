// src/render/FlickRenderer.cpp
#include "PCH.h"
#include "render/FlickRenderer.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h (house ordering)

#include "FUCK_API.h"

#include <algorithm>

namespace SH::hw {
    namespace {
        constexpr const char* kAtlasPath =
            "Data/SKSE/Plugins/BardHero/highway/atlas.png";
        constexpr const char* kHighwayFadePath =
            "Data/SKSE/Plugins/BardHero/highway/highway_fade.png";
        inline ImVec2 IV(const V2& p) { return ImVec2(p.x, p.y); }
        inline ImVec4 IC(const RGBA& c) { return ImVec4(c.r, c.g, c.b, c.a); }

    }


    bool FlickRenderer::Ready() {
        if (_atlas && _highwayFade) return true;
        auto* i = FUCK::GetInterface();
        if (!i) return false;
        if (!_atlas) {
            // Retry missing assets periodically instead of probing every frame.
            if (_loggedMissing) {
                if (++_retryCounter < 120) return false;
                _retryCounter = 0;
            }
            _atlas = i->LoadImage(kAtlasPath, false);
            if (!_atlas && !_loggedMissing) {
                spdlog::error("[render] atlas missing: {}", kAtlasPath);
                _loggedMissing = true;
            }
        }
        if (_atlas && !_highwayFade && !_fadeLoadAttempted) {
            _fadeLoadAttempted = true;
            _highwayFade = i->LoadImage(kHighwayFadePath, false);
            if (!_highwayFade) {
                spdlog::error("[render] highway fade missing: {}",
                              kHighwayFadePath);
            }
        }
        return _atlas != nullptr;
    }

    void FlickRenderer::Quad(Sprite spr, const V2 p[4], const RGBA& tint) {
        auto* i = FUCK::GetInterface();
        if (!i || !_atlas) return;
        if (spr == Sprite::kHighwayFade) {
            if (_highwayFade) {
                i->DrawImageQuad(
                    _highwayFade, IV(p[0]), IV(p[1]), IV(p[2]), IV(p[3]),
                    ImVec2(0.0f, 0.0f), ImVec2(1.0f, 0.0f),
                    ImVec2(1.0f, 1.0f), ImVec2(0.0f, 1.0f), IC(tint));
            } else {
                // Keep one shape when the optional asset is missing.
                RGBA solid = tint;
                solid.a *= 0.38f;
                i->DrawQuadFilled(IV(p[0]), IV(p[1]), IV(p[2]), IV(p[3]),
                                  IC(solid));
            }
            return;
        }
        const UvRect uv = UvOf(spr);
        i->DrawImageQuad(_atlas, IV(p[0]), IV(p[1]), IV(p[2]), IV(p[3]),
                         ImVec2(uv.u0, uv.v0), ImVec2(uv.u1, uv.v0),
                         ImVec2(uv.u1, uv.v1), ImVec2(uv.u0, uv.v1),
                         IC(tint));
    }

    void FlickRenderer::QuadUv(const UvRect& uv, const V2 p[4],
                               const RGBA& tint) {
        auto* i = FUCK::GetInterface();
        if (!i || !_atlas) return;
        i->DrawImageQuad(_atlas, IV(p[0]), IV(p[1]), IV(p[2]), IV(p[3]),
                         ImVec2(uv.u0, uv.v0), ImVec2(uv.u1, uv.v0),
                         ImVec2(uv.u1, uv.v1), ImVec2(uv.u0, uv.v1),
                         IC(tint));
    }

    void FlickRenderer::QuadFilled(const V2 p[4], const RGBA& c) {
        auto* i = FUCK::GetInterface();
        if (!i) return;
        i->DrawQuadFilled(IV(p[0]), IV(p[1]), IV(p[2]), IV(p[3]), IC(c));
    }

    void FlickRenderer::ScreenRectFilled(const V2& mn, const V2& mx,
                                         const RGBA& c) {
        auto* i = FUCK::GetInterface();
        if (!i) return;
        const auto u8 = [](float f) {
            return static_cast<unsigned>(
                std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        const ImU32 col = (u8(c.a) << 24) | (u8(c.b) << 16) |
                          (u8(c.g) << 8) | u8(c.r);
        i->DrawScreenRectFilled(IV(mn), IV(mx), col, 0.0f);
    }
}
