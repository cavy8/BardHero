#include "chart/MidParser.h"

#include <algorithm>
#include <optional>

namespace bard {
    namespace {
        struct Range {
            std::uint32_t start = 0, end = 0;  // [start, end)
            bool Covers(std::uint32_t t) const { return t >= start && t < end; }
        };
        // Collect [on,off) ranges for one key.
        std::vector<Range> RangesFor(const SmfTrack& t, std::uint8_t key) {
            std::vector<Range>           out;
            std::optional<std::uint32_t> open;
            for (const auto& n : t.notes) {
                if (n.key != key) continue;
                if (n.on && !open) {
                    open = n.tick;
                } else if (!n.on && open) {
                    out.push_back({ *open, n.tick });
                    open.reset();
                }
            }
            return out;
        }
        bool AnyCovers(const std::vector<Range>& rs, std::uint32_t t) {
            return std::any_of(rs.begin(), rs.end(),
                               [&](const Range& r) { return r.Covers(t); });
        }
    }

    bool BuildRawTrackFromMid(const SmfFile& f, int difficulty,
                              const MidOptions& opt, RawTrack& out) {
        // Track by name: PART GUITAR, T1 GEMS legacy alias (spec 4.4).
        const SmfTrack* g = nullptr;
        for (const auto& t : f.tracks) {
            if (t.name == "PART GUITAR" || t.name == "T1 GEMS") {
                g = &t;
                break;
            }
        }
        if (!g) return false;

        const std::uint32_t div  = f.division;
        const std::uint8_t  base =
            static_cast<std::uint8_t>(60 + 12 * difficulty);

        // --- gates + phrase markers ---------------------------------------
        bool enhancedOpens = false;
        for (const auto& tx : g->texts) {
            if (tx.text == "[ENHANCED_OPENS]" || tx.text == "ENHANCED_OPENS") {
                enhancedOpens = true;
            }
        }
        // PS SysEx: payload 'P' 'S' 0x00 <type> <difficulty> <value> per the
        // repo format spec (4.4). type 0x01 open, 0x04 tap; difficulty 0xFF =
        // all; value 1 = phrase start, 0 = end. Asymmetry (spec 4.4): tap
        // phrases affect their END tick (+1), open phrases do not.
        // PINNED: field order follows spec 4.4 verbatim; synthetic tests use
        // the same writer, the M5 corpus pass validates against real files.
        std::vector<Range> psOpen, psTap;
        {
            std::optional<std::uint32_t> openStart, tapStart;
            for (const auto& sx : g->sysex) {
                if (sx.data.size() < 6 || sx.data[0] != 'P' ||
                    sx.data[1] != 'S' || sx.data[2] != 0x00) {
                    continue;
                }
                const auto type = sx.data[3];
                const auto diff = sx.data[4];
                const auto val  = sx.data[5];
                if (diff != 0xFF && diff != difficulty) continue;
                if (type != 0x01 && type != 0x04) continue;
                auto& start = (type == 0x01) ? openStart : tapStart;
                auto& list  = (type == 0x01) ? psOpen : psTap;
                if (val) {
                    start = sx.tick;
                } else if (start) {
                    list.push_back(
                        { *start, (type == 0x04) ? sx.tick + 1 : sx.tick });
                    start.reset();
                }
            }
            // dangling phrases (no end marker) are ignored
        }

        // 116 = star power; if NO 116 exists anywhere, 103 IS star power
        // (GH1/2 compat, spec 4.4). ini multiplier_note (103/116) overrides.
        const bool anySp116 = !RangesFor(*g, 116).empty();
        const int  spKey    = (opt.spNote == 103 || opt.spNote == 116)
                                  ? opt.spNote
                                  : (anySp116 ? 116 : 103);
        const bool solosAreSp = (spKey == 103);
        auto spRanges = RangesFor(*g, static_cast<std::uint8_t>(spKey));
        auto soloRanges =
            solosAreSp ? std::vector<Range>{} : RangesFor(*g, 103);
        auto tapRanges = RangesFor(*g, 104);  // 104 or PS SysEx 0x04 = tap
        for (const auto& r : psTap) tapRanges.push_back(r);
        auto forceH = RangesFor(*g, base + 5);  // true overrides (spec 4.4)
        auto forceS = RangesFor(*g, base + 6);

        // --- gems ----------------------------------------------------------
        struct Gem {
            std::uint32_t tick, len;
            int           lane;
        };
        std::vector<Gem> gems;
        auto addLane = [&](std::uint8_t key, int lane) {
            for (const auto& r : RangesFor(*g, key)) {
                gems.push_back({ r.start, r.end - r.start, lane });
            }
        };
        for (int l = 0; l < 5; ++l) addLane(base + l, l);
        // open = base-1, gated behind ENHANCED_OPENS text or PS SysEx 0x01
        for (const auto& r : RangesFor(*g, base - 1)) {
            if (enhancedOpens || AnyCovers(psOpen, r.start)) {
                gems.push_back({ r.start, r.end - r.start, kOpenLane });
            }
        }
        std::stable_sort(gems.begin(), gems.end(),
                         [](const Gem& a, const Gem& b) {
                             return a.tick < b.tick;
                         });

        // --- chord snap (<=10 ticks merge to EARLIEST) + sustain cutoff ----
        const std::uint32_t cutoff =
            opt.sustainCutoffTicks >= 0
                ? static_cast<std::uint32_t>(opt.sustainCutoffTicks)
                : div / 3;
        std::uint32_t anchor = 0;
        for (std::size_t i = 0; i < gems.size(); ++i) {
            if (i == 0 || gems[i].tick - anchor > 10) {
                anchor = gems[i].tick;
            }
            auto& c = [&]() -> RawChord& {
                if (out.chords.empty() || out.chords.back().tick != anchor) {
                    out.chords.push_back({});
                    out.chords.back().tick = anchor;
                }
                return out.chords.back();
            }();
            c.mask |= LaneBit(gems[i].lane);
            const auto len = gems[i].len < cutoff ? 0u : gems[i].len;
            c.sustainTicks[gems[i].lane] =
                std::max(c.sustainTicks[gems[i].lane], len);
        }

        // --- per-chord modifiers ------------------------------------------
        for (auto& c : out.chords) {
            const bool fs = AnyCovers(forceS, c.tick);
            const bool fh = AnyCovers(forceH, c.tick);
            // PINNED: when both markers overlap one chord, strum wins.
            if (fs) {
                c.forcing = Forcing::kForceStrum;
            } else if (fh) {
                c.forcing = Forcing::kForceHopo;
            }
            c.tap = AnyCovers(tapRanges, c.tick);
        }

        // --- phrases -------------------------------------------------------
        for (const auto& r : spRanges) {
            out.spPhrases.push_back({ r.start, r.end, r.start == r.end });
        }
        for (const auto& r : soloRanges) {
            if (r.end > r.start) {
                out.solos.push_back({ r.start, r.end - 1, 0 });  // inclusive
            }
        }

        // .mid natural threshold = floor(division/3) + 1 (deliberate +1)
        out.hopoThresholdTicks =
            opt.hopoThresholdTicks >= 0
                ? static_cast<std::uint32_t>(opt.hopoThresholdTicks)
                : div / 3 + 1;
        return true;
    }
}
