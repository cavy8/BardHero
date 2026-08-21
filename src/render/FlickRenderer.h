// src/render/FlickRenderer.h
#pragma once

// IHighwayRenderer -> FUCK ABI. Thin, untested glue (the pure side is
// HighwayLayout). No FUCK types in this header (include-order pitfall);
// Image handles are opaque pointers.

#include "render/IHighwayRenderer.h"

namespace SH::hw {
    class FlickRenderer final : public IHighwayRenderer {
    public:
        bool Ready();  // lazy-loads the atlas; false until it exists
        void Quad(Sprite spr, const V2 p[4], const RGBA& tint) override;
        void QuadFilled(const V2 p[4], const RGBA& c) override;
        void QuadUv(const UvRect& uv, const V2 p[4],
                    const RGBA& tint) override;
        void ScreenRectFilled(const V2& mn, const V2& mx,
                              const RGBA& c) override;

    private:
        void* _atlas             = nullptr;
        void* _highwayFade       = nullptr;
        bool  _loggedMissing     = false;
        bool  _fadeLoadAttempted = false;
        int   _retryCounter      = 0;
    };
}
