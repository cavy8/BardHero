#pragma once

// Decode a WIC image into a top-down RGBA8 buffer for the D3D highway layers.

#include <cstdint>
#include <string_view>
#include <vector>

namespace SH::hw {
    bool LoadImageRGBA8(std::string_view path,
                        std::vector<std::uint8_t>& outPixels,
                        std::uint32_t& outWidth, std::uint32_t& outHeight);
}
