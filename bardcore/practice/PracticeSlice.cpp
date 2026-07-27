#include "practice/PracticeSlice.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bard::practice {
    namespace {
        // Does a phrase covering ticks [a_start, a_end) - or
        // [a_start, a_end] when a_inclusive - include this tick?
        bool Covers(std::uint32_t a_start, std::uint32_t a_end,
                    bool a_inclusive, std::uint32_t a_tick) {
            if (a_tick < a_start) { return false; }
            return a_inclusive ? a_tick <= a_end : a_tick < a_end;
        }
    }

    ParsedChart SliceChart(const ParsedChart& a_chart, double a_startSec,
                           double a_endSec) {
        ParsedChart out;
        // Timing identity is COPIED, never recomputed: a slice must play
        // at exactly the tempo the full chart plays at.
        out.resolution    = a_chart.resolution;
        out.tempo         = a_chart.tempo;
        out.timeSigs      = a_chart.timeSigs;
        out.offsetSeconds = a_chart.offsetSeconds;
        out.meta          = a_chart.meta;
        out.sections      = a_chart.sections;

        if (a_endSec < a_startSec) { return out; }

        bool anyNote = false;
        out.notes.reserve(a_chart.notes.size());
        for (const auto& n : a_chart.notes) {
            if (n.time < a_startSec || n.time > a_endSec) { continue; }
            anyNote = true;
            out.notes.push_back(n);
        }
        if (!anyNote) { return out; }

        // "Fully contained" (spec 6.2) means every note the phrase covers
        // survived the slice, and it covers at least one. Ask the NOTES
        // that directly; testing the phrase's tick window against the
        // surviving notes' tick SPAN is a proxy that is wrong in both
        // directions. Too strict: endTick routinely sits well past a
        // phrase's own last note, because charters park it on the next
        // beat, so a phrase the player plays in full gets dropped. Too
        // loose: a phrase lying entirely outside the slice covers no
        // surviving note at all, yet can still satisfy a span test and be
        // carried in with nothing to award.
        //
        // This is O(phrases * notes). It runs once per practice restart,
        // never per frame, so the clarity is worth more than the loop.
        const auto contained = [&](std::uint32_t a_start,
                                   std::uint32_t a_end, bool a_inclusive) {
            bool any = false;
            for (const auto& n : a_chart.notes) {
                if (!Covers(a_start, a_end, a_inclusive, n.tick)) {
                    continue;
                }
                // A note of this phrase fell outside the window. Awarding
                // Star Power for a phrase the player only played half of
                // is a scoring lie, so the whole phrase goes.
                if (n.time < a_startSec || n.time > a_endSec) {
                    return false;
                }
                any = true;
            }
            return any;
        };

        // ---- cross-vector indices ------------------------------------
        // Two fields point ACROSS the two vectors: Note::spPhrase indexes
        // spPhrases, SpPhrase::lastNoteIndex indexes notes. A slice
        // rewrites BOTH vectors, so BOTH fields go stale and MUST be
        // re-based here - whenever either vector is filtered, the other's
        // indices are wrong.
        //
        // This is not cosmetic. GuitarEngine indexes both UNCHECKED: it
        // sizes its phrase state to spPhrases.size() and then does
        // _phrases[n.spPhrase] and _chart.spPhrases[n.spPhrase] straight
        // from the note. A stale index is an out-of-bounds READ and WRITE
        // on the first Star Power note of the range, not a wrong number.
        //
        // phraseRemap[old index] = new index, or -1 for "dropped".
        std::vector<std::int32_t> phraseRemap(a_chart.spPhrases.size(), -1);

        for (std::size_t pi = 0; pi < a_chart.spPhrases.size(); ++pi) {
            const auto& p = a_chart.spPhrases[pi];
            // SpPhrase::endTick is EXCLUSIVE, except on a zero-length
            // phrase where it names the single tick (ChartTypes.h).
            if (!contained(p.startTick, p.endTick, p.zeroLen)) { continue; }

            // Re-base the award index. ParsedChart::notes is sorted by
            // tick with UNIQUE ticks (ChartTypes.h) and the slice keeps
            // that order, so a tick names one note in either vector - it
            // survives the filter as an identity where an index does not.
            // Linear, like contained() above.
            std::int32_t newLast = -1;
            if (p.lastNoteIndex >= 0 &&
                static_cast<std::size_t>(p.lastNoteIndex) <
                    a_chart.notes.size()) {
                const auto lastTick = a_chart.notes[p.lastNoteIndex].tick;
                for (std::size_t ni = 0; ni < out.notes.size(); ++ni) {
                    if (out.notes[ni].tick == lastTick) {
                        newLast = static_cast<std::int32_t>(ni);
                        break;
                    }
                }
            }
            // contained() said every note this phrase covers survived, so
            // the award note must be in the slice. If it is not, the two
            // disagree and the chart is corrupt - drop the phrase rather
            // than award Star Power on whatever note the stale index
            // happens to hit. phraseRemap keeps its -1, so the notes that
            // pointed here fall back to "no phrase" below.
            if (newLast < 0) { continue; }

            auto kept          = p;
            kept.lastNoteIndex = newLast;
            // noteCount is deliberately NOT adjusted: contained() keeps a
            // phrase only when EVERY note it covers survived, so the count
            // still matches what is present. The engine awards on
            // hits == noteCount, so trimming it would award early.
            phraseRemap[pi] = static_cast<std::int32_t>(out.spPhrases.size());
            out.spPhrases.push_back(kept);
        }

        // Only now that every keep/drop decision is final can the notes be
        // re-pointed - remapping earlier would map through a table that is
        // still being filled. Note that a SURVIVING note can belong to a
        // DROPPED phrase: contained() drops on one stray note, and that
        // phrase's other notes may sit well inside the window. Such an
        // orphan must read as "no phrase", never as a stale or shifted
        // index. Note::extendedMask needs NO re-basing: it is a lane
        // bitmask, not a cross-vector index, and a bit can only go stale
        // for a note cut for sitting past a_endSec - every note in that
        // sustain's shadow is then outside the slice too.
        for (auto& n : out.notes) {
            if (n.spPhrase < 0 ||
                static_cast<std::size_t>(n.spPhrase) >= phraseRemap.size()) {
                n.spPhrase = -1;  // sentinel, or defensively out of range
                continue;
            }
            n.spPhrase = phraseRemap[n.spPhrase];  // may itself be -1
        }

        for (const auto& s : a_chart.solos) {
            // SoloPhrase::endTick is INCLUSIVE (ChartTypes.h). Solos are
            // { startTick, endTick, noteCount } - they carry NO note
            // index, and no note points back at one, so there is nothing
            // to re-base here. Do not go looking for it.
            if (contained(s.startTick, s.endTick, true)) {
                out.solos.push_back(s);
            }
        }
        return out;
    }
}
