#include "chart/SongIni.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace bard {
    namespace {
        std::string_view Trim(std::string_view s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                                  s.front() == '\r'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                                  s.back() == '\r'))
                s.remove_suffix(1);
            return s;
        }
        std::string Lower(std::string_view s) {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            return out;
        }
        // Strip TextMeshPro rich-text tags (<b>, <color=...>, </i>, ...) from
        // display strings (spec 4.2).
        std::string StripTags(std::string_view s) {
            std::string out;
            bool        inTag = false;
            for (char c : s) {
                if (c == '<') {
                    inTag = true;
                } else if (c == '>') {
                    inTag = false;
                } else if (!inTag) {
                    out.push_back(c);
                }
            }
            return out;
        }
        bool ParseBool(std::string_view v, bool def) {
            const auto l = Lower(Trim(v));
            if (l == "true" || l == "1") return true;
            if (l == "false" || l == "0") return false;
            return def;
        }
    }

    bool ParseSongIniText(std::string_view text, SongIniValues& out) {
        bool inSong = false;
        // Precedence bookkeeping: the correctly-spelled cutoff key wins over
        // CH's shipped typo when both appear (spec 4.2).
        bool sawCorrectCutoff = false;

        std::size_t pos = 0;
        while (pos <= text.size()) {
            const auto nl   = text.find('\n', pos);
            const auto end  = (nl == std::string_view::npos) ? text.size() : nl;
            auto       line = Trim(text.substr(pos, end - pos));
            pos             = end + 1;
            const bool last = (nl == std::string_view::npos);

            if (!line.empty() && line.front() == '[' && line.back() == ']') {
                inSong = Lower(line.substr(1, line.size() - 2)) == "song";
                if (last) break;
                continue;
            }
            const auto eq = line.find('=');
            if (!inSong || eq == std::string_view::npos) {
                if (last) break;
                continue;
            }
            const auto key = Lower(Trim(line.substr(0, eq)));
            const auto val = Trim(line.substr(eq + 1));
            const auto sval = std::string(val);

            if (key == "name") {
                out.name = StripTags(val);
            } else if (key == "artist") {
                out.artist = StripTags(val);
            } else if (key == "album") {
                out.album = StripTags(val);
            } else if (key == "charter" || key == "frets") {
                out.charter = StripTags(val);
            } else if (key == "loading_phrase") {
                out.loadingPhrase = StripTags(val);
            } else if (key == "delay") {
                const double raw = std::atof(sval.c_str());
                out.hasDelay     = true;
                // Heuristic (spec 4.2): |delay| < 100 is almost certainly
                // seconds - CH shipped this bug-compat.
                out.delayMs = (std::abs(raw) < 100.0) ? raw * 1000.0 : raw;
            } else if (key == "hopo_frequency") {
                out.hopoFrequency = std::atoll(sval.c_str());
            } else if (key == "eighthnote_hopo") {
                out.eighthNoteHopo = ParseBool(val, out.eighthNoteHopo);
            } else if (key == "sustain_cutoff_threshold") {
                out.sustainCutoff = std::atoll(sval.c_str());
                sawCorrectCutoff  = true;
            } else if (key == "sustain_cuttoff_threshold") {  // CH's typo
                if (!sawCorrectCutoff) {
                    out.sustainCutoff = std::atoll(sval.c_str());
                }
            } else if (key == "multiplier_note" || key == "star_power_note") {
                const int n = std::atoi(sval.c_str());
                if (n == 103 || n == 116) {  // 103/116 ONLY (spec 4.2)
                    out.multiplierNote = n;
                }
            } else if (key == "end_events") {
                out.endEvents = ParseBool(val, out.endEvents);
            } else if (key == "diff_guitar") {
                out.diffGuitar = std::atoi(sval.c_str());
            } else if (key == "unlock_rank") {
                out.unlockRank = std::atoi(sval.c_str());
            } else if (key == "preview_start_time") {
                out.previewStartMs = std::atof(sval.c_str());
            } else if (key == "song_length") {
                out.songLengthMs = std::atof(sval.c_str());
            } else if (key == "single_instrument") {
                out.singleInstrument =
                    ParseBool(val, out.singleInstrument);
            } else if (key == "instrument") {
                const auto l = Lower(val);
                if (l == "lute" || l == "flute" || l == "drum" ||
                    l == "guitar") {
                    out.instrument = l;
                }
            }
            if (last) break;
        }
        return true;
    }

    std::int64_t ResolveHopoThreshold(const SongIniValues& ini,
                                      std::uint32_t res, bool isMid) {
        if (ini.hopoFrequency > 0) return ini.hopoFrequency;
        if (ini.eighthNoteHopo) return res / 2;
        return isMid ? (res / 3 + 1)
                     : static_cast<std::int64_t>(65ull * res / 192ull);
    }

    std::int64_t ResolveSustainCutoff(const SongIniValues& ini,
                                      std::uint32_t res, bool isMid) {
        if (ini.sustainCutoff >= 0) return ini.sustainCutoff;
        return isMid ? res / 3 : 0;
    }
}
