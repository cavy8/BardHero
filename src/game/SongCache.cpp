// src/game/SongCache.cpp
#include "game/SongCache.h"

#include <charconv>

namespace SH {
    namespace {
        constexpr std::string_view kHeader = "skyherocache\t1";

        std::string Sanitize(std::string s) {
            for (auto& c : s) {
                if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            }
            return s;
        }

        template <class T>
        bool Num(std::string_view f, T& out) {
            const auto* end = f.data() + f.size();
            return std::from_chars(f.data(), end, out).ec == std::errc{};
        }
    }

    std::vector<SongCacheEntry> ParseSongCache(std::string_view text) {
        std::vector<SongCacheEntry> out;
        std::size_t                 pos = 0;
        bool                        first = true;
        while (pos <= text.size()) {
            auto nl = text.find('\n', pos);
            if (nl == std::string_view::npos) nl = text.size();
            std::string_view line = text.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty()) continue;
            if (first) {
                if (line != kHeader) return {};
                first = false;
                continue;
            }
            // Split the first 7 columns on their tabs; the 8th (instrument)
            // is whatever remains, INCLUDING empty - it must not go
            // through the same "find tab else take-all" logic as the
            // others, or an empty trailing field (the common untagged
            // case) is indistinguishable from "no more fields" and the
            // whole line is dropped.
            std::string_view f[8];
            std::string_view rest = line;
            bool             ok   = true;
            for (int i = 0; i < 7; ++i) {
                const auto tab = rest.find('\t');
                if (tab == std::string_view::npos) { ok = false; break; }
                f[i] = rest.substr(0, tab);
                rest = rest.substr(tab + 1);
            }
            if (!ok) continue;
            // Column 9 (unlockRank) was appended after the cache format
            // shipped, so a file already on disk ends at the instrument.
            // Peel it off only when it is actually there and let an old
            // row keep the -1 sentinel: dropping short rows instead would
            // force a full rescan on the first launch after the upgrade.
            std::string_view unlockField;
            bool             hasUnlock = false;
            if (const auto tab = rest.find('\t');
                tab != std::string_view::npos) {
                unlockField = rest.substr(tab + 1);
                rest        = rest.substr(0, tab);
                hasUnlock   = true;
            }
            f[7] = rest;
            SongCacheEntry e;
            e.folder = std::string(f[0]);
            double lengthMs = -1.0;
            if (!Num(f[1], e.chartMtime)) continue;
            e.name    = std::string(f[2]);
            e.artist  = std::string(f[3]);
            e.charter = std::string(f[4]);
            if (!Num(f[5], lengthMs)) continue;
            e.lengthMs = lengthMs;
            if (!Num(f[6], e.diff)) continue;
            e.instrument = std::string(f[7]);
            if (hasUnlock && !Num(unlockField, e.unlockRank)) continue;
            out.push_back(std::move(e));
        }
        return out;
    }

    std::string SerializeSongCache(
        const std::vector<SongCacheEntry>& entries) {
        std::string out{ kHeader };
        out += '\n';
        for (const auto& e : entries) {
            out += Sanitize(e.folder);
            out += '\t';
            out += std::to_string(e.chartMtime);
            out += '\t';
            out += Sanitize(e.name);
            out += '\t';
            out += Sanitize(e.artist);
            out += '\t';
            out += Sanitize(e.charter);
            out += '\t';
            out += std::to_string(e.lengthMs);
            out += '\t';
            out += std::to_string(e.diff);
            out += '\t';
            out += Sanitize(e.instrument);
            out += '\t';
            out += std::to_string(e.unlockRank);
            out += '\n';
        }
        return out;
    }
}
