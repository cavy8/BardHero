#pragma once
#include <cstdint>
#include <limits>
#include <vector>

#include "chart/ChartTypes.h"
#include "engine/EngineParams.h"
#include "engine/InputTypes.h"

namespace bard {

    enum class Judgment : std::uint8_t { kPending, kHit, kMissed };

    struct EngineStats {
        std::int64_t score        = 0;  // grand total (notes+sustains+solo)
        std::int64_t sustainScore = 0;
        std::int64_t soloBonus    = 0;
        int  combo = 0, maxCombo = 0;
        int  notesHit = 0, notesMissed = 0, overstrums = 0, ghostInputs = 0;
        // PRESENTATION ONLY. Strums that resolved no note and were NOT
        // charged as an overstrum: before the first note resolves and
        // after the last one, where scoring deliberately stays silent
        // (a lead-in strum must not break a combo that has not begun).
        // Nothing in scoring, Glory, or the fail gate may read this - it
        // exists so the host can still play the miss cue, because a
        // strum that makes no sound at all reads as dropped input
        // (field 2026-07-25).
        int  unscoredStrums = 0;
        int  multiplier = 1;  // combo multiplier, before SP doubling
        bool spActive   = false;
        // Presentation-only performance pressure. Starts neutral, rises on
        // hits and falls harder on misses/overstrums. No fail state reads it:
        // this is the Skyrim "Glory" meter, not a new gameplay gate.
        double glory = 0.5;
        double spGaugeMeasureTicks = 0.0;
        int    spPhrasesCompleted  = 0;
    };

    // Frame-rate-independent 5-fret judgment engine (spec sections 5-6).
    // Driven by (time, input) tuples exclusively; state changes ONLY at input
    // times and scheduled boundaries. Between them every quantity is a pure
    // function of time via TempoMap - never accumulate floats across steps.
    class GuitarEngine {
    public:
        GuitarEngine(const ParsedChart& chart, const EngineParams& params);
        GuitarEngine(const GuitarEngine&)            = delete;
        GuitarEngine& operator=(const GuitarEngine&) = delete;

        void Queue(const NoteInput& input);  // monotonic clamp, never reorder
        void Update(double time);            // drain queue + sub-step to time

        const EngineStats& Stats() const { return _stats; }
        Judgment JudgmentOf(std::size_t noteIndex) const {
            return _notes[noteIndex].judged;
        }
        // Presentation read: a chart-authored SP note remains visually
        // chargeable only while its phrase has not been stripped by a
        // miss/qualifying overstrum. Regular notes and invalid indices are
        // false. Scoring still owns the authoritative phrase state.
        bool SpPhraseAvailableFor(std::size_t noteIndex) const {
            if (noteIndex >= _chart.notes.size()) { return false; }
            const auto phrase = _chart.notes[noteIndex].spPhrase;
            return phrase >= 0 &&
                   static_cast<std::size_t>(phrase) < _phrases.size() &&
                   !_phrases[phrase].stripped;
        }

        // ---- M4 render read API (const views; determinism untouched) ----
        // Live SP gauge as a fraction of the full bar (32 measures-worth of
        // ticks). t is InputTime-domain seconds. Stats().spGaugeMeasureTicks
        // only refreshes at re-anchors and would freeze a HUD bar mid-drain.
        double SpGaugeFraction(double t) const {
            return SpGaugeAt(t) /
                   (32.0 * static_cast<double>(_chart.resolution));
        }
        // Started-sustain lanes of note idx: still accruing vs finished
        // (dropped early or completed - the renderer tells those apart by
        // comparing endTime with now). {0,0} when nothing has started.
        void SustainMasks(std::size_t idx, std::uint8_t& active,
                          std::uint8_t& dropped) const;
        // Whammy pressed within the spec 5.6 refire window at time t.
        // Presentation-only read (trail wiggle, vibrato): the SP-gain
        // rule stays in WhammyEngagedIn and is untouched by this.
        bool WhammyRecentIn(double t) const {
            return t - _lastWhammy <= kWhammyWindow;
        }

    private:
        static constexpr double kInf = std::numeric_limits<double>::infinity();
        static constexpr double kWhammyWindow = 0.250;  // spec 5.6

        struct NoteState {
            Judgment judged  = Judgment::kPending;
            bool     ghosted = false;  // latch until the note resolves
        };
        struct Sustain {
            std::size_t   note      = 0;
            std::uint8_t  maskBits  = 0;  // fret bits (open bit excluded)
            std::uint32_t startTick = 0, endTick = 0;
            double        endTime = 0.0, burstTime = 0.0;
            int           lanes = 1;  // scoring width (uniform chord = n)
            std::uint32_t scoredPoints = 0;  // cumulative tick-points banked
            bool burstDone = false, finished = false, held = true;
            bool extended = false, open = false;
            double dropDeadline = kInf;  // active when finite
        };
        struct PhraseState {
            bool stripped = false;
            int  hits     = 0;
        };
        struct SoloState {
            int  hits    = 0;
            bool awarded = false;
        };

        // --- stepping core ---
        void   AdvanceTo(double target);
        void   RunStep(double now, const NoteInput* input);
        double NextBoundary(double after, double limit) const;

        // --- 5.2 machine ---
        void ApplyInput(double now, const NoteInput& in);
        bool CheckForNoteHit(double now);  // true = state changed
        bool CanNoteBeHit(const Note& n) const;
        void HitNote(double now, std::size_t idx, bool viaStrum);
        void MissNote(double now, std::size_t idx);
        void Overstrum(double now);
        void AdvanceHead();

        // --- windows ---
        double WindowFor(std::size_t idx) const;  // static or dynamic W(d)
        double FrontEnd(std::size_t idx) const;   // negative
        double BackEnd(std::size_t idx) const;    // positive
        bool   InWindow(std::size_t idx, double now) const;

        // --- sustains (Task 16) ---
        void StartSustains(double now, std::size_t idx);
        void UpdateSustains(double now);  // hold + drop + burst
        void AccrueSustains(double from, double to);
        void FinishSustain(Sustain& s);
        void BreakAllSustains(double now);
        std::uint8_t HeldMask() const {  // open bit iff nothing held (5.2)
            return _held ? _held : kOpenBit;
        }

        // --- star power + solo (Task 17) ---
        void   PhraseNoteHit(std::size_t idx);
        void   StripPhraseOf(std::size_t idx);
        void   TryActivateSp();
        void   ReanchorSp(double now);      // rate-change bookkeeping
        double SpGaugeAt(double t) const;   // pure in-interval function
        double SpZeroTime() const;
        bool   WhammyEngagedIn(double t) const;
        int    EffectiveMultiplier() const {
            return _stats.multiplier * (_stats.spActive ? 2 : 1);
        }
        void UpdateSolo(double now);

        // --- members ---
        // Owned by value: binding a caller temporary must be safe (a
        // dangling-ref bug here cost a test cycle at skeleton time).
        const ParsedChart _chart;
        EngineParams      _p;
        EngineStats        _stats;
        std::vector<NoteState>    _notes;
        std::vector<Sustain>      _sustains;
        std::vector<PhraseState>  _phrases;
        std::vector<SoloState>    _solos;
        std::vector<std::int32_t> _noteSolo;  // note index -> solo index / -1
        std::vector<std::int32_t> _soloLast;  // solo index -> last note / -1
        std::vector<std::int32_t> _noteSustainFirst;  // note -> _sustains / -1
        std::vector<std::int32_t> _noteSustainCount;
        std::vector<NoteInput>    _queue;
        std::size_t               _queueHead = 0;

        double       _now  = -1.0e9;
        std::size_t  _head = 0;  // first pending note
        std::uint8_t _held = 0;  // fret bits 0-4 only
        bool   _hasTapped       = false;
        double _frontEndExpire  = -kInf;  // -inf none; +inf infinite credit
        double _strumExpire     = kInf;   // active when finite
        double _hopoExpire      = kInf;
        bool   _strumThisStep   = false;
        bool   _anyNoteResolved = false;  // leading-overstrum suppression

        // SP: gauge is a pure function between re-anchors (determinism rule)
        double _spAnchorGauge = 0.0;  // measure ticks == chart ticks
        double _spAnchorTick  = 0.0;
        double _lastWhammy    = -kInf;
    };
}
