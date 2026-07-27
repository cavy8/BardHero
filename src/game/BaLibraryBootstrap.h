#pragma once

#include <filesystem>
#include <string>

namespace SH {
    struct BaPrepareStats {
        int templates = 0;
        int sourcesFound = 0;
        int generated = 0;
        int ready = 0;
        int pending = 0;
        int customSkipped = 0;
        int failed = 0;
    };

    // Materialize the chart-only BA compatibility pack into the normal song
    // library, using audio from the separately installed BA Bard Songs mod.
    // sourceRoot defaults to Skyrim's virtual Data/Sound/fx/mus/bard path.
    BaPrepareStats PrepareInstalledBaLibrary(
        const std::filesystem::path& songsRoot,
        const std::filesystem::path& sourceRoot = {});

    // Resolve one managed BA song on demand. Non-BA folders and already
    // cached songs return true without work. A pending managed folder is
    // converted synchronously and atomically published as song.opus.
    bool EnsureInstalledBaSongAudio(
        const std::filesystem::path& songFolder,
        const std::filesystem::path& sourceRoot = {},
        std::string* error = nullptr);
}
