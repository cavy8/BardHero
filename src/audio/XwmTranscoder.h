#pragma once

#include <filesystem>
#include <string>

namespace SH {
    // Decode Skyrim's xWMA/RIFF audio with the Windows WMA codec and encode a
    // compact Ogg Opus cache that the existing miniaudio backend can load.
    // The caller owns atomic publication (pass a temporary output path, then
    // rename it only after this returns true).
    bool TranscodeXwmToOpus(const std::filesystem::path& source,
                            const std::filesystem::path& output,
                            std::string& error);
}
