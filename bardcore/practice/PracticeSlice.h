#pragma once

#include "chart/ChartTypes.h"

// Chart slicing for practice mode (spec 2026-07-26-practice-mode 6.2).
//
// WHY a slice and not an engine mode: GuitarEngine is constructed from a
// chart and steps forward only. Slicing is pure data, so a practice
// restart is "build a new engine from a smaller chart" and the engine's
// state machine - the most safety-critical code in the project - is not
// touched at all. Determinism is preserved by construction.
namespace bard::practice {

    // Notes whose time falls in [a_startSec, a_endSec]. Sustains that
    // cross the end are kept WHOLE. SP phrases and solos are kept only
    // when fully contained. Timing identity (tempo, timeSigs,
    // resolution, offset, meta, sections) is copied verbatim.
    //
    // Both cross-vector index fields - SpPhrase::lastNoteIndex and
    // Note::spPhrase - are RE-BASED onto the sliced vectors, because
    // GuitarEngine indexes both unchecked. A phrase whose award note is
    // missing from the slice is dropped rather than re-pointed, and a
    // surviving note whose phrase was dropped reads -1 ("no phrase").
    //
    // CALL THIS WITH PracticeRange::notesFromSec, NOT startSec.
    //
    // This function has no opinion about the lead-in: it slices exactly the
    // window it is given. The ease-in lives in the CALLER's choice of start.
    // Passing startSec (which is kLeadInSec earlier) sweeps in every note of
    // the PREVIOUS section that falls inside the approach window, so each
    // loop restart opens with notes the player is not practising - which is
    // exactly what made the loop feel jarring in the field. Passing
    // notesFromSec omits them, matching Clone Hero, where the audio rolls in
    // early but the chart does not.
    [[nodiscard]] ParsedChart SliceChart(const ParsedChart& a_chart,
                                         double a_startSec,
                                         double a_endSec);
}
