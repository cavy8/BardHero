#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "chart/ChartTypes.h"

// Practice range resolution (spec 2026-07-26-practice-mode 6.1). Header
// only and constexpr-friendly: this is arithmetic, not policy.
namespace bard::practice {

    // Approach room before the first note of the range. Dropping the
    // player onto a note with zero warning is unplayable.
    inline constexpr double kLeadInSec = 2.0;
    // Room past the range end so a note ON the boundary still gets its
    // full judgment window before the loop snaps back.
    inline constexpr double kTailPadSec = 0.5;

    struct PracticeRange {
        // Where playback SEEKS to: kLeadInSec of approach room before the
        // section, clamped at 0.
        double startSec = 0.0;
        double endSec   = 0.0;
        // Where the chart's NOTES begin - the section's own time, with no
        // lead-in subtracted. Distinct from startSec on purpose, and the
        // distinction is the whole ease-in (Clone Hero's practice-mode
        // behaviour): the audio restarts a couple of seconds early so the
        // player can hear their way in, but the notes belonging to the
        // PREVIOUS section are omitted rather than thrown at them during
        // that window. Slicing from startSec instead makes every loop
        // restart open with notes the player is not practising.
        //
        // Equal to startSec for a whole-song range, where there is nothing
        // before the start to omit.
        double notesFromSec = 0.0;
        int    startSection = -1;  // -1 = whole song
        int    endSection   = -1;
    };

    // a_songEndSec is the end of playable content (last note or stem
    // length - the host picks, the range only reads it).
    [[nodiscard]] inline PracticeRange ResolveRange(
        const std::vector<ChartSection>& a_sections, int a_start,
        int a_end, double a_songEndSec) {
        PracticeRange range;
        const int count = static_cast<int>(a_sections.size());
        const bool usable = count > 0 && a_start >= 0 && a_end >= 0 &&
                            a_start < count && a_end < count;
        if (!usable) {
            // Whole song. This is the common case: most charts carry no
            // markers at all. The std::max guard is the same one the
            // section branch uses, and for the same reason: a_songEndSec
            // is built from user-editable ini fields, so a pathological
            // delay can drive it below zero. Clamping here means the
            // range is NEVER inverted - a caller that restarts when the
            // clock passes endSec would otherwise restart forever.
            range.startSec     = 0.0;
            range.notesFromSec = 0.0;  // nothing before the start to omit
            range.endSec       = std::max(range.startSec, a_songEndSec) +
                             kTailPadSec;
            return range;
        }
        int first = a_start;
        int last  = a_end;
        if (first > last) { std::swap(first, last); }
        range.startSection = first;
        range.endSection   = last;
        // The section's own time is where the NOTES start; startSec backs off
        // by the lead-in so the audio eases in. Clamped the same way, so a
        // section inside the first kLeadInSec seconds cannot put notesFromSec
        // behind startSec and resurrect the notes the ease-in exists to drop.
        range.notesFromSec =
            std::max(0.0, a_sections[static_cast<std::size_t>(first)].time);
        range.startSec = std::max(0.0, range.notesFromSec - kLeadInSec);
        const int afterLast = last + 1;
        const double rawEnd =
            afterLast < count
                ? a_sections[static_cast<std::size_t>(afterLast)].time
                : a_songEndSec;
        range.endSec = std::max(range.startSec, rawEnd) + kTailPadSec;
        return range;
    }
}
