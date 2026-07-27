#include "chart/Normalize.h"

#include <bit>

namespace bard {

    void ApplySustainCutoff(RawTrack& track, std::int64_t cutoffTicks) {
        if (cutoffTicks <= 0) {
            return;
        }
        for (auto& c : track.chords) {
            for (auto& len : c.sustainTicks) {
                if (len > 0 &&
                    len < static_cast<std::uint32_t>(cutoffTicks)) {
                    len = 0;
                }
            }
        }
    }

    ParsedChart Normalize(const RawTrack& track, const TempoMap& tempo,
                          double offsetSeconds) {
        ParsedChart out;
        out.resolution    = tempo.Resolution();
        out.tempo         = tempo;
        out.offsetSeconds = offsetSeconds;
        out.spPhrases.assign(track.spPhrases.begin(), track.spPhrases.end());
        out.solos.assign(track.solos.begin(), track.solos.end());

        const std::uint32_t threshold = track.hopoThresholdTicks;
        out.notes.reserve(track.chords.size());

        for (std::size_t i = 0; i < track.chords.size(); ++i) {
            const RawChord& rc = track.chords[i];
            if (track.hasEnd && rc.tick >= track.endTick) {
                break;  // end_events truncation (spec 4.2)
            }
            Note n;
            n.tick = rc.tick;
            n.time = tempo.SecondsAt(rc.tick) + offsetSeconds;
            n.mask = rc.mask;

            // Natural HOPO (spec 4.3): gap < threshold, not a chord, not the
            // same mask as the previous chord, never the first note.
            bool natural = false;
            if (i > 0) {
                const RawChord& prev    = track.chords[i - 1];
                const bool      isChord = std::popcount(rc.mask) > 1;
                natural = !isChord && rc.mask != prev.mask &&
                          (rc.tick - prev.tick) < threshold;
            }
            switch (rc.forcing) {
                case Forcing::kNone:       n.isHopo = natural; break;
                case Forcing::kFlip:       n.isHopo = !natural; break;
                case Forcing::kForceHopo:  n.isHopo = true; break;
                case Forcing::kForceStrum: n.isHopo = false; break;
            }
            if (rc.tap) {  // tap overrides (spec 4.3)
                n.isTap  = true;
                n.isHopo = false;
            }

            for (int lane = 0; lane < kLaneCount; ++lane) {
                const std::uint32_t len = rc.sustainTicks[lane];
                n.sustainTicks[lane]    = len;
                if (len > 0) {
                    n.sustainEnd[lane] =
                        tempo.SecondsAt(static_cast<double>(rc.tick) + len) +
                        offsetSeconds;
                    // extended = any LATER chord starts before this sustain ends
                    for (std::size_t j = i + 1; j < track.chords.size(); ++j) {
                        if (track.chords[j].tick >= rc.tick + len) break;
                        n.extendedMask |= LaneBit(lane);
                        break;
                    }
                }
            }
            out.notes.push_back(n);
        }

        // SP phrase membership + counts (end exclusive, zero-length = its tick)
        for (std::size_t p = 0; p < out.spPhrases.size(); ++p) {
            auto& ph = out.spPhrases[p];
            for (std::size_t ni = 0; ni < out.notes.size(); ++ni) {
                const auto t  = out.notes[ni].tick;
                const bool in = ph.zeroLen
                                    ? (t == ph.startTick)
                                    : (t >= ph.startTick && t < ph.endTick);
                if (in) {
                    out.notes[ni].spPhrase = static_cast<std::int32_t>(p);
                    ph.noteCount++;
                    ph.lastNoteIndex = static_cast<std::int32_t>(ni);
                }
            }
        }
        // Solo note counts (end INCLUSIVE)
        for (auto& so : out.solos) {
            for (const auto& n : out.notes) {
                if (n.tick >= so.startTick && n.tick <= so.endTick) {
                    so.noteCount++;
                }
            }
        }
        return out;
    }
}
