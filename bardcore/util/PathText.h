#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace SH::path_text {
    // std::filesystem::path::string() uses the active Windows code page and
    // can throw for perfectly valid filenames on another player's locale.
    // Song/cache identifiers and logs are UTF-8 everywhere else.
    [[nodiscard]] inline std::string Utf8(const std::filesystem::path& path) {
        const auto value = path.u8string();
        return { reinterpret_cast<const char*>(value.data()), value.size() };
    }

    [[nodiscard]] inline std::filesystem::path FromUtf8(
        std::string_view value) {
        const auto* first =
            reinterpret_cast<const char8_t*>(value.data());
        return std::filesystem::path(
            std::u8string(first, first + value.size()));
    }
}
