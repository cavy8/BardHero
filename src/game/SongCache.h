// src/game/SongCache.h
#pragma once

// PURE TSV cache for browser metadata (plan decision 13): parse/serialize
// only, no filesystem - SongLibrary owns the IO. Headless-tested
// (SongCacheTests).

#include <string>
#include <string_view>
#include <vector>

namespace SH {
    struct SongCacheEntry {
        std::string folder;         // key: song folder path, utf8
        long long   chartMtime = 0; // chart file last_write_time ticks
        std::string name, artist, charter;
        double      lengthMs = -1.0;
        int         diff = -1;
        std::string instrument;  // lowercase lute/flute/drum, or empty
        int unlockRank = -1;  // song.ini override, -1 = derive from diff
    };

    std::vector<SongCacheEntry> ParseSongCache(std::string_view text);
    std::string SerializeSongCache(const std::vector<SongCacheEntry>& entries);
}
