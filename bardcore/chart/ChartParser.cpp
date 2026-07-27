#include "chart/ChartParser.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
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
        // Strip only the OUTER quotes; quotes-inside-quotes survive (spec 4.3).
        std::string Unquote(std::string_view s) {
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
                s = s.substr(1, s.size() - 2);
            }
            return std::string(s);
        }
        bool ToU32(std::string_view s, std::uint32_t& v) {
            auto r = std::from_chars(s.data(), s.data() + s.size(), v);
            return r.ec == std::errc{};
        }
    }

    bool ParseChartText(std::string_view text, ChartFile& out) {
        std::string currentSection;
        std::vector<std::pair<std::uint32_t, double>> bpms;

        std::size_t pos = 0;
        while (pos <= text.size()) {
            const auto nl   = text.find('\n', pos);
            const auto end  = (nl == std::string_view::npos) ? text.size() : nl;
            auto       line = Trim(text.substr(pos, end - pos));
            pos             = end + 1;
            const bool last = (nl == std::string_view::npos);

            if (line.empty() || line == "{" || line == "}") {
                if (last) break;
                continue;
            }
            if (line.front() == '[' && line.back() == ']') {
                currentSection = std::string(line.substr(1, line.size() - 2));
                if (last) break;
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string_view::npos) {
                if (last) break;
                continue;
            }
            const auto key = Trim(line.substr(0, eq));
            const auto val = Trim(line.substr(eq + 1));

            if (currentSection == "Song") {
                if (key == "Resolution") {
                    std::uint32_t r;
                    if (ToU32(val, r) && r > 0) out.resolution = r;
                } else if (key == "Offset") {
                    out.offsetSeconds = std::atof(std::string(val).c_str());
                } else if (key == "Name") {
                    out.meta.name = Unquote(val);
                } else if (key == "Artist") {
                    out.meta.artist = Unquote(val);
                } else if (key == "Album") {
                    out.meta.album = Unquote(val);
                } else if (key == "Charter") {
                    out.meta.charter = Unquote(val);
                }
            } else if (currentSection == "SyncTrack") {
                std::uint32_t tick;
                if (ToU32(key, tick)) {
                    // val: "B 120000" | "TS 4" | "TS 7 3" | "A ..."
                    const auto sp   = val.find(' ');
                    const auto kind = val.substr(0, sp);
                    const auto rest = sp == std::string_view::npos
                                          ? std::string_view{}
                                          : Trim(val.substr(sp + 1));
                    if (kind == "B") {
                        std::uint32_t millibpm;
                        if (ToU32(rest, millibpm) && millibpm > 0) {
                            bpms.emplace_back(tick, millibpm / 1000.0);
                        }
                    } else if (kind == "TS") {
                        const auto    sp2 = rest.find(' ');
                        std::uint32_t num, exp = 2;  // exponent default 2 -> /4
                        if (ToU32(rest.substr(0, sp2), num)) {
                            TimeSig ts;
                            ts.tick = tick;
                            ts.num  = num;
                            if (sp2 != std::string_view::npos) {
                                ToU32(Trim(rest.substr(sp2 + 1)), exp);
                            }
                            ts.denom = 1u << exp;  // power-of-2 EXPONENT
                            out.timeSigs.push_back(ts);
                        }
                    }  // "A" anchors: editor-only, ignored (spec 4.3)
                }
            } else if (!currentSection.empty()) {
                std::uint32_t tick;
                if (ToU32(key, tick)) {
                    out.sections[currentSection].emplace_back(
                        tick, std::string(val));
                }
            }
            if (last) break;
        }

        out.tempo.SetResolution(out.resolution);
        for (auto& [tick, bpm] : bpms) out.tempo.AddBpm(tick, bpm);
        out.tempo.Finalize();
        return true;
    }

    bool BuildRawTrack(const ChartFile& cf, const std::string& section,
                       RawTrack& out) {
        auto it = cf.sections.find(section);
        if (it == cf.sections.end()) return false;

        // .chart natural threshold = floor(65/192 * resolution) (spec 4.3).
        out.hopoThresholdTicks =
            static_cast<std::uint32_t>(65ull * cf.resolution / 192ull);

        std::uint32_t soloStart = 0;
        bool          inSolo    = false;

        for (const auto& [tick, entry] : it->second) {
            const auto sp1  = entry.find(' ');
            const auto kind = entry.substr(0, sp1);
            const auto rest = sp1 == std::string::npos
                                  ? std::string{}
                                  : std::string(Trim(
                                        std::string_view(entry).substr(sp1 + 1)));
            auto chordAt = [&]() -> RawChord& {
                if (out.chords.empty() || out.chords.back().tick != tick) {
                    out.chords.push_back({});
                    out.chords.back().tick = tick;
                }
                return out.chords.back();
            };
            if (kind == "N") {
                std::uint32_t type = 0, len = 0;
                if (std::sscanf(rest.c_str(), "%u %u", &type, &len) != 2) {
                    continue;
                }
                auto& c = chordAt();
                if (type <= 4) {
                    c.mask |= LaneBit(static_cast<int>(type));
                    c.sustainTicks[type] = len;
                } else if (type == 5) {  // strum/HOPO FLIP, not "force HOPO"
                    c.forcing = Forcing::kFlip;
                } else if (type == 6) {  // tap (overrides flip)
                    c.tap = true;
                } else if (type == 7) {  // open
                    c.mask |= kOpenBit;
                    c.sustainTicks[kOpenLane] = len;
                }
            } else if (kind == "S") {
                std::uint32_t type = 0, len = 0;
                if (std::sscanf(rest.c_str(), "%u %u", &type, &len) != 2) {
                    continue;
                }
                if (type == 2) {  // star power phrase
                    SpPhrase p;
                    p.startTick = tick;
                    p.endTick   = tick + len;
                    p.zeroLen   = (len == 0);
                    out.spPhrases.push_back(p);
                }  // S 64 = drums-only activation: ignore on guitar (spec 4.3)
            } else if (kind == "E") {
                if (rest == "solo") {
                    inSolo    = true;
                    soloStart = tick;
                } else if (rest == "soloend" && inSolo) {
                    out.solos.push_back({ soloStart, tick, 0 });  // inclusive
                    inSolo = false;
                } else if (rest == "end") {
                    out.hasEnd  = true;
                    out.endTick = tick;
                }
            }
        }
        // Chords arrive grouped only if the file lists same-tick lines
        // adjacently (CH exports do). Sort + merge defensively anyway.
        std::stable_sort(out.chords.begin(), out.chords.end(),
                         [](const RawChord& a, const RawChord& b) {
                             return a.tick < b.tick;
                         });
        std::vector<RawChord> merged;
        for (const auto& c : out.chords) {
            if (!merged.empty() && merged.back().tick == c.tick) {
                auto& m = merged.back();
                m.mask |= c.mask;
                if (c.forcing != Forcing::kNone) m.forcing = c.forcing;
                m.tap |= c.tap;
                for (int l = 0; l < kLaneCount; ++l) {
                    m.sustainTicks[l] =
                        std::max(m.sustainTicks[l], c.sustainTicks[l]);
                }
            } else {
                merged.push_back(c);
            }
        }
        out.chords = std::move(merged);
        return true;
    }
}
