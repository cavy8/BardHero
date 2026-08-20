#pragma once

// WIC file -> top-down RGBA8 pixel buffer. The one place this decode runs;
// every manual-D3D highway layer (background, underlay, midlayer) calls
// through here instead of carrying its own copy of the WIC boilerplate.

#include <cstdint>
#include <string_view>
#include <vector>

namespace SH::hw {
    bool LoadImageRGBA8(std::string_view path,
                        std::vector<std::uint8_t>& outPixels,
                        std::uint32_t& outWidth, std::uint32_t& outHeight);
}
