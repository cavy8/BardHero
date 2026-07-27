// src/game/SongLibrary.h
#pragma once

// Async song scan + metadata for the browser (deferred-ledger item "scan
// metadata cache + async scan"). Owns a detached worker; the browser polls
// Scanning()/Snapshot(). Metadata = song.ini (folder name fallback) with
// the SongCache TSV keyed by chart-file mtime.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "chart/Scan.h"

namespace SH {
    struct SongInfo {
        bard::SongEntry entry;
        std::string     name, artist, charter;
        double          lengthMs = -1.0;
        int             diff = -1;
        std::string     instrument;  // lowercase lute/flute/drum/guitar, or empty
        int             unlockRank = -1;  // metadata fallback when unmeasured
        int             requiredRank = 1; // resolved natural/fallback rank
        double          challengeScore = 0.0;
        bool            challengeMeasured = false;
        // Which difficulties the chart actually carries (bit d = Easy..
        // Expert d non-empty; bard::LoadedSong::difficultyMask from the
        // measure pass). 0 = unmeasured/unreadable - the browser then
        // shows no availability and assumes the requested difficulty.
        std::uint8_t    diffMask = 0;
    };

    class SongLibrary {
    public:
        static SongLibrary& GetSingleton();
        void EnsureScan();  // first call launches the background scan
        void Rescan();      // re-walk (no-op while a scan runs)

        // NO DeleteSong, deliberately. A Recycle-Bin delete briefly lived
        // here (2026-07-26) and was removed the same day, by decision:
        // BardHero READS the song library and never writes to it.
        //
        // The reason it cannot be made safe cheaply: most of a real
        // library is not in the player's own songs folder at all, it sits
        // inside the installed mods, served through MO2's virtual
        // filesystem. The runtime sees REAL paths under MODS\mods\..., so
        // a shell delete there destroys shipped mod content rather than a
        // download, and a delete from a dist-deployed folder silently
        // comes back on the next deploy. Restricting it to the user
        // folder was the guard we shipped, and it refused most of the
        // library, which is the bug report that ended the feature.
        // Removing a bad chart is a file-manager job. Do not re-add a
        // destructive path here without a new decision from the user.
        bool Scanning() const;
        int  BadCount() const;  // rejected folders from the last scan
        std::shared_ptr<const std::vector<SongInfo>> Snapshot() const;
    };
}
