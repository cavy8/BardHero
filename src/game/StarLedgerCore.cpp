// src/game/StarLedgerCore.cpp
#include "game/StarLedgerCore.h"

#include <algorithm>

namespace SH::stars {
    namespace {
        // Written version. Every older version is still ACCEPTED on load
        // (v1-v3 best entries are stamped with the caller's legacy
        // difficulty), so the co-save record version must NOT be bumped
        // alongside this - see the note in StarLedger.cpp's LoadCallback.
        constexpr std::uint8_t kVersion = 4;
        static_assert(kInstrumentCount == 3,
                      "wire format v1 assumes 3 lifted bytes");

        void PutU16(std::vector<std::uint8_t>& b, std::uint16_t v) {
            b.push_back(static_cast<std::uint8_t>(v & 0xFF));
            b.push_back(static_cast<std::uint8_t>(v >> 8));
        }
        void PutU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
            for (int i = 0; i < 4; ++i) {
                b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
            }
        }
        void PutStr(std::vector<std::uint8_t>& b, const std::string& s) {
            const auto n = static_cast<std::uint16_t>(
                s.size() > 0xFFFF ? 0xFFFF : s.size());
            PutU16(b, n);
            b.insert(b.end(), s.begin(), s.begin() + n);
        }

        struct Reader {
            const std::uint8_t* p;
            std::size_t         n;
            std::size_t         at = 0;
            bool                ok = true;
            std::uint8_t U8() {
                if (at + 1 > n) { ok = false; return 0; }
                return p[at++];
            }
            std::uint16_t U16() {
                if (at + 2 > n) { ok = false; return 0; }
                const auto v = static_cast<std::uint16_t>(
                    p[at] | (p[at + 1] << 8));
                at += 2;
                return v;
            }
            std::uint32_t U32() {
                if (at + 4 > n) { ok = false; return 0; }
                std::uint32_t v = 0;
                for (int i = 0; i < 4; ++i) {
                    v |= static_cast<std::uint32_t>(p[at + i]) << (8 * i);
                }
                at += 4;
                return v;
            }
            std::string Str() {
                const auto len = U16();
                if (!ok || at + len > n) { ok = false; return {}; }
                std::string s(reinterpret_cast<const char*>(p + at), len);
                at += len;
                return s;
            }
        };
    }

    bool RecordResult(LedgerData& a_d, const std::string& a_key,
                      Instrument a_inst, int a_difficulty, int a_stars) {
        a_d.lastPlayed = a_key;
        const auto diff = static_cast<std::uint8_t>(
            a_difficulty < 0 ? 0 : (a_difficulty > 3 ? 3 : a_difficulty));
        const std::tuple<std::string, std::uint8_t, std::uint8_t> k{
            a_key, static_cast<std::uint8_t>(a_inst), diff
        };
        const auto it  = a_d.best.find(k);
        const int  cur = it == a_d.best.end() ? 0 : it->second;
        if (a_stars <= cur) { return false; }  // incl. 0-star: never a row
        if (it == a_d.best.end()) {
            a_d.best.emplace(k, static_cast<std::uint8_t>(a_stars));
        } else {
            it->second = static_cast<std::uint8_t>(a_stars);
        }
        return true;
    }

    int BestFor(const LedgerData& a_d, const std::string& a_key,
                Instrument a_inst, int a_difficulty) {
        const auto diff = static_cast<std::uint8_t>(
            a_difficulty < 0 ? 0 : (a_difficulty > 3 ? 3 : a_difficulty));
        const auto it = a_d.best.find(
            { a_key, static_cast<std::uint8_t>(a_inst), diff });
        return it == a_d.best.end() ? 0 : it->second;
    }

    GateCounts CountFor(const LedgerData& a_d, Instrument a_inst) {
        // Per DISTINCT chart: the best across every difficulty counts once.
        // The map sorts by (key, inst, diff), so one chart's rows for one
        // instrument are contiguous and a single group-max pass suffices.
        GateCounts  c;
        const auto  code = static_cast<std::uint8_t>(a_inst);
        std::string group;
        bool        haveGroup = false;
        int         groupMax  = 0;
        const auto  commit    = [&] {
            if (!haveGroup) { return; }
            if (groupMax >= 3) { ++c.chartsAt3; }
            if (groupMax >= 5) { ++c.chartsAt5; }
        };
        for (const auto& [k, v] : a_d.best) {
            if (std::get<1>(k) != code) { continue; }
            const auto& key = std::get<0>(k);
            if (!haveGroup || key != group) {
                commit();
                group     = key;
                haveGroup = true;
                groupMax  = 0;
            }
            groupMax = std::max<int>(groupMax, v);
        }
        commit();
        return c;
    }

    std::vector<std::uint8_t> Serialize(const LedgerData& a_d) {
        std::vector<std::uint8_t> b;
        b.push_back(kVersion);
        PutStr(b, a_d.lastPlayed);
        for (int i = 0; i < kInstrumentCount; ++i) {
            b.push_back(a_d.lifted[i]);
        }
        PutU32(b, static_cast<std::uint32_t>(a_d.best.size()));
        for (const auto& [k, v] : a_d.best) {
            PutStr(b, std::get<0>(k));
            b.push_back(std::get<1>(k));
            b.push_back(std::get<2>(k));  // v4: difficulty
            b.push_back(v);
        }
        // v2 tail. std::set walks sorted, so the bytes are order-independent
        // and two ledgers with the same taught charts serialize identically.
        PutU32(b, static_cast<std::uint32_t>(a_d.taught.size()));
        for (const auto& k : a_d.taught) { PutStr(b, k); }
        PutU32(b, static_cast<std::uint32_t>(a_d.newSongs.size()));
        for (const auto& k : a_d.newSongs) { PutStr(b, k); }
        return b;
    }

    bool Deserialize(const std::uint8_t* a_p, std::size_t a_n,
                     int a_legacyDifficulty, LedgerData& a_out) {
        if (!a_p || a_n < 1) { return false; }
        const auto legacyDiff = static_cast<std::uint8_t>(
            a_legacyDifficulty < 0
                ? 3
                : (a_legacyDifficulty > 3 ? 3 : a_legacyDifficulty));
        Reader r{ a_p, a_n };
        const auto ver = r.U8();
        if (ver < 1 || ver > kVersion) { return false; }
        LedgerData d;
        d.lastPlayed = r.Str();
        for (int i = 0; i < kInstrumentCount; ++i) { d.lifted[i] = r.U8(); }
        const auto count = r.U32();
        if (!r.ok) { return false; }
        for (std::uint32_t i = 0; i < count; ++i) {
            auto       key  = r.Str();
            const auto inst = r.U8();
            // v1-v3 entries carry no difficulty: stamp them with the
            // host-supplied legacy difficulty (the default setting those
            // runs were almost certainly played at; pre-release records).
            const auto diff  = ver >= 4 ? r.U8() : legacyDiff;
            const auto stars = r.U8();
            if (!r.ok) { return false; }
            // Domain check: out-of-range inst/diff/stars = corrupt buffer.
            // Whole-record reject - lifted bits are monotonic and persisted,
            // so one bad load must never leak into the ledger.
            if (inst >= kInstrumentCount || diff > 3 || stars > 5) {
                return false;
            }
            d.best[{ std::move(key), inst, diff }] = stars;
        }
        // v2 tail. A v1 buffer ends above, and so does a v2 one whose taught
        // set was empty when it was written; both load with `taught` empty
        // rather than failing. Anything PAST that boundary is a real
        // truncation and the bounded reads below reject it.
        if (ver >= 2 && r.at < a_n) {
            const auto taughtCount = r.U32();
            if (!r.ok) { return false; }
            for (std::uint32_t i = 0; i < taughtCount; ++i) {
                auto key = r.Str();
                if (!r.ok) { return false; }
                d.taught.insert(std::move(key));
            }
        } else if (ver >= 3) {
            return false;
        }
        if (ver >= 3) {
            const auto newCount = r.U32();
            if (!r.ok) { return false; }
            for (std::uint32_t i = 0; i < newCount; ++i) {
                auto key = r.Str();
                if (!r.ok) { return false; }
                d.newSongs.insert(std::move(key));
            }
        }
        a_out = std::move(d);
        return true;
    }
}
