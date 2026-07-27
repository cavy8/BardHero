#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace bard {

    // One playable song folder (spec 4.1): notes.chart|notes.mid + >=1 audio
    // stem (+ optional song.ini). Stems recorded by reserved stem NAME with
    // the single-vs-split suppression rules applied (drums vs drums_N,
    // vocals vs vocals_1/2). Role interpretation beyond that (e.g. rhythm
    // standing in for bass when no bass stem exists) is the mixer's concern -
    // v1 plays every stem anyway.
    struct SongEntry {
        std::filesystem::path folder;
        std::filesystem::path chartFile;  // .chart preferred over .mid
        std::filesystem::path iniFile;    // empty when absent
        bool                  isMid = false;
        std::map<std::string, std::filesystem::path> stems;
    };

    struct BadSong {
        std::filesystem::path folder;
        std::string           reason;
    };

    struct ScanResult {
        std::vector<SongEntry> songs;
        std::vector<BadSong>   bad;  // the badsongs.txt escape hatch feed
        // False only when filesystem discovery itself was interrupted.
        // Ordinary content rejection (for example, a chart with no audio)
        // remains a complete scan. Hosts can therefore keep their last
        // healthy snapshot during a transient downloader/MO2 race.
        bool                   complete = true;
    };

    // Pure synchronous walk. Async + metadata cache are M4 host work; the
    // cache holds metadata only, never notes (spec 4.1).
    ScanResult ScanSongs(const std::filesystem::path& root);

    // Merge any number of independent physical or virtual roots. Duplicate
    // roots and duplicate song folders are returned once.
    ScanResult ScanSongs(const std::vector<std::filesystem::path>& roots);
}
