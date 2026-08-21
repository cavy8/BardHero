#pragma once

#include "render/HighwayLayout.h"

#include <memory>
#include <string_view>

namespace SH::hw {
    // Render an image scaled to fit the screen. Manual D3D draws land below
    // FLICK's ImGui content, so this is used for underlay and midlayer only.
    class HighwayFullscreenLayerD3D final {
    public:
        HighwayFullscreenLayerD3D();
        ~HighwayFullscreenLayerD3D();

        HighwayFullscreenLayerD3D(const HighwayFullscreenLayerD3D&) = delete;
        HighwayFullscreenLayerD3D& operator=(
            const HighwayFullscreenLayerD3D&) = delete;

        void Refresh();
        bool Render(std::string_view path, const RGBA& tint,
                    const View& view);

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
