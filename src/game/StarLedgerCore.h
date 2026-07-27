// src/game/StarLedgerCore.h
#pragma once

// PURE per-save star ledger + byte-buffer serialization (no RE/OS - suite
// StarLedgerTests). The RE-side StarLedger singleton wraps this with a
// mutex and the SKSE co-save callbacks.
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "game/StarsLogic.h"

namespace SH::stars {

    struct LedgerData {
        // key = (song folder LEAF name, instrument code, difficulty 0..3).
        // Leaf name is stable across path moves; collisions across
        // libraries accepted - spec 3. Difficulty joined the key
        // 2026-07-25 (Guitar-Hero convention: records are per difficulty);
        // it is the RESOLVED difficulty the run actually played, never the
        // requested one.
        std::map<std::tuple<std::string, std::uint8_t, std::uint8_t>,
                 std::uint8_t>
            best;
        std::string  lastPlayed;                       // "" = none yet
        std::uint8_t lifted[kInstrumentCount] = {};    // gate bits, monotonic
        // Charts unlocked by paying a bard (spec 6.3). Rank-derived
        // unlocks are NOT stored - they are recomputed from live rank, so a
        // save can never disagree with the player's actual rank.
        std::set<std::string> taught;
        // Newly unlocked/learned charts remain tagged until their first
        // successful start. This is presentation state, persisted per save.
        std::set<std::string> newSongs;
    };

    // Updates lastPlayed always; best only if improved. Returns "improved".
    // 0 stars never improves; a non-improving play never materializes a row.
    // a_difficulty is the RESOLVED difficulty (0..3), clamped defensively.
    bool RecordResult(LedgerData& a_d, const std::string& a_key,
                      Instrument a_inst, int a_difficulty, int a_stars);
    int  BestFor(const LedgerData& a_d, const std::string& a_key,
                 Instrument a_inst, int a_difficulty);
    // Gate counts stay per DISTINCT CHART: a song's best across every
    // difficulty counts once (the gate thresholds say "distinct charts",
    // and the per-difficulty split must not let one song 3-starred on two
    // difficulties open a gate that needs two songs).
    GateCounts CountFor(const LedgerData& a_d, Instrument a_inst);

    // Format v1: u8 version | u16 lastPlayedLen + bytes | u8 lifted[3] |
    // u32 count | count x (u16 keyLen + bytes, u8 inst, u8 stars).
    // Format v2: v1 fields, then u32 taughtCount | taught entries.
    // Format v3: v2 fields, then u32 newCount | new entries.
    // Format v4: as v3 but best entries are (u16 keyLen + bytes, u8 inst,
    // u8 difficulty, u8 stars).
    // Older buffers load with their newer sets empty; v1-v3 best entries
    // carry no difficulty and are stamped with a_legacyDifficulty (the
    // host passes the user's default setting - the difficulty those runs
    // were almost certainly played at; pre-release records only).
    // Little-endian, no padding. Bounded reads + domain checks (inst <
    // kInstrumentCount, difficulty <= 3, stars <= 5): corrupt input ->
    // false, a_out untouched.
    std::vector<std::uint8_t> Serialize(const LedgerData& a_d);
    bool Deserialize(const std::uint8_t* a_p, std::size_t a_n,
                     int a_legacyDifficulty, LedgerData& a_out);
}
