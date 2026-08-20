#pragma once

#include "render/HighwayLayout.h"

#include <memory>
#include <string_view>

namespace SH::hw {
    class HighwaySurfaceD3D final {
    public:
        HighwaySurfaceD3D();
        ~HighwaySurfaceD3D();

        HighwaySurfaceD3D(const HighwaySurfaceD3D&) = delete;
        HighwaySurfaceD3D& operator=(const HighwaySurfaceD3D&) = delete;

        void Refresh();
        bool RenderBackground(std::string_view path, const RGBA& tint,
                              const Style& style, const View& view,
                              double visual, double lookahead);

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
