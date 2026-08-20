#pragma once

#include "render/HighwayLayout.h"

#include <memory>
#include <string_view>

namespace SH::hw {
    // Draws one image scaled (never stretched) to fit the current screen,
    // centered, with nothing drawn in the leftover margin - whatever sits
    // beneath just shows through. Manual D3D11, same reasoning as
    // HighwaySurfaceD3D: a raw draw issued from a FUCK::IWindow's
    // RenderOverlay() always lands on the render target before FLICK's own
    // ImGui frame is flushed that frame, so two manual-D3D layers drawn in
    // sequence stack the same way relative to each other and to the
    // (also manual-D3D) highway background image, and both stay under
    // every FLICK-drawn (ImGui) highway visual - gems, HUD, banners.
    //
    // Backs the underlay and midlayer theme layers (see RenderUi.cpp); the
    // overlay layer needs to sit ABOVE that ImGui content instead, which a
    // manual draw can't reach, so it goes through FLICK's DrawImageQuad.
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
