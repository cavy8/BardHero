#pragma once

#include "util/PathText.h"

#include <filesystem>
#include <string>

namespace SH::song_identity {
    // Persistent score/unlock keys use the song folder's leaf name. Keep the
    // conversion locale-independent: Clone Hero libraries commonly contain
    // characters (for example AC／DC's full-width slash) that Windows cannot
    // represent through std::filesystem::path::string().
    [[nodiscard]] inline std::string ChartKey(
        const std::filesystem::path& songFolder) {
        return path_text::Utf8(songFolder.filename());
    }
}
