// src/game/CrowdMoodLogic.h
#pragma once

// PURE crowd-mood model (no RE/OS includes - headless-tested by
// CrowdMoodTests). Fed CUMULATIVE engine counters plus a song time; it
// differences them itself, so the caller needs no event stream.
//
// Why a rolling window and not a whole-song average: an average cannot
// express a comeback, and a comeback is the entire point of the feature.
#include <algorithm>
#include <cstddef>
#include <deque>

namespace SH::crowd {

    enum class Level { kTerrible, kMiddling, kGreat };

    // The exact SGT global encoding for a committed crowd level, plus the
    // animation transition BardHero must request. SGT re-evaluates the
    // globals for later dialogue lines, but an NPC already inside its
    // dance/drink idle does not leave that animation merely because the
    // next line became negative. Any non-great state therefore releases
    // celebration idles on SGT's audience aliases before later dialogue
    // supplies its own gesture.
    struct ReactionState {
        float terrible;
        float good;
        bool  releaseCelebration;
    };

    [[nodiscard]] constexpr ReactionState ReactionFor(Level a_level) {
        switch (a_level) {
            case Level::kTerrible: return { 1.0f, 0.0f, true };
            case Level::kMiddling: return { 0.0f, 2.0f, true };
            case Level::kGreat: return { 0.0f, 1.0f, false };
        }
        return { 0.0f, 2.0f, true };
    }

    // Guitar Hero-style live performance health. Unlike Mood's historical
    // rolling-accuracy window below, this is an event accumulator: every hit
    // nudges the needle right, every miss/overstrum moves it 7.5 times as far
    // left, and Star Power strengthens recovery while softening damage. It
    // starts in the middle and silence cannot move it. A full meter spans 300
    // ordinary hit-steps. Field feedback on both the 160-step, 1:3 model and
    // its first 240-step replacement was that raw note volume still bought
    // recovery too easily. Recovery is therefore streak-gated: after any bad
    // judgment the first twelve clean hits stabilize the performance but do
    // not raise Glory. The first four seconds separately damp damage so an
    // opening input/animation settle cannot empty the meter before the player
    // has established the rhythm.
    struct RockParams {
        double hitGain      = 1.0 / 300.0;
        double badLoss      = 7.5 / 300.0;
        double spHitScale   = 1.25;
        double spBadScale   = 0.80;
        double openingSec      = 4.0;
        double openingBadScale = 0.25;
        int    recoveryHitsRequired = 12;
        double terribleBelow = 1.0 / 3.0;
        double greatAt       = 2.0 / 3.0;
    };

    // THE SHORT-SONG CEILING (field 2026-07-26: "even when flawless
    // clearing of shorter songs from the BA Songs pack we can't reach
    // higher glory").
    //
    // The meter is denominated in NOTES, so a song's LENGTH decided the
    // ceiling a player could reach rather than how well they played. From
    // the 0.5 start, greatAt (2/3) is 1/6 of the span away - 50 clean hits
    // at the default 300 span. A 28-note BA song cannot produce 50 hits at
    // all, so a FLAWLESS run of it topped out near 0.59 and the crowd never
    // warmed up. No amount of skill could move it, which is the opposite of
    // what a performance meter is for.
    //
    // So the span is read as "the fraction of THIS song you must play clean
    // to move the needle end to end". Long songs are untouched: the clamp
    // binds at a_spanHits for anything from ~500 notes up, which is every
    // full-length chart, so their behaviour is bit-identical. Only the songs
    // that were unwinnable change.
    //
    // The floor matters as much as the fraction. Below it one miss costs a
    // punishing share of the meter (7.5/40 at the default bad weight) -
    // which IS right for a short song, where every note is a larger part of
    // the performance, but it should not keep shrinking without limit.
    inline constexpr double kSpanNoteFraction = 0.6;
    inline constexpr double kMinMeterSpan     = 40.0;

    [[nodiscard]] constexpr double MeterSpanForSong(double a_spanHits,
                                                    int a_songNotes) {
        if (a_songNotes <= 0) { return a_spanHits; }  // unknown: unchanged
        const double scaled =
            static_cast<double>(a_songNotes) * kSpanNoteFraction;
        const double floored =
            scaled < kMinMeterSpan ? kMinMeterSpan : scaled;
        return floored < a_spanHits ? floored : a_spanHits;
    }

    // Recovery is gated on a clean streak, and 12 clean hits out of a
    // 28-note song is most of the song spent unable to gain at all - the
    // same defect one layer down. Scales with the span, so a long song keeps
    // whatever was tuned.
    [[nodiscard]] constexpr int RecoveryHitsForSpan(int a_recoveryHits,
                                                    double a_span,
                                                    double a_spanHits) {
        if (a_spanHits <= 0.0 || a_span >= a_spanHits) {
            return a_recoveryHits;
        }
        const double scaled =
            static_cast<double>(a_recoveryHits) * a_span / a_spanHits;
        const int rounded = static_cast<int>(scaled + 0.5);
        const int floored = rounded < 3 ? 3 : rounded;
        return floored > a_recoveryHits ? a_recoveryHits : floored;
    }

    struct RockSample {
        // Default means steady-state for pure callers that have no timeline.
        // Session supplies the real song time, including its negative lead.
        double songSec     = 1.0e9;
        int  notesHit    = 0;
        int  notesMissed = 0;
        int  overstrums  = 0;
        bool spActive    = false;
    };

    class RockMeter {
    public:
        RockMeter() { Reset(); }

        void Reset() {
            _value       = 0.5;
            _level       = Level::kMiddling;
            _notesHit    = 0;
            _notesMissed = 0;
            _overstrums  = 0;
            _recoveryHits = 0;
            _recovering   = false;
        }

        // Returns true when the live red/yellow/green zone changes.
        bool Feed(const RockSample& a_sample, const RockParams& a_params) {
            if (a_sample.notesHit < _notesHit ||
                a_sample.notesMissed < _notesMissed ||
                a_sample.overstrums < _overstrums) {
                const bool changed = _level != Level::kMiddling;
                Reset();
                _notesHit    = std::max(0, a_sample.notesHit);
                _notesMissed = std::max(0, a_sample.notesMissed);
                _overstrums  = std::max(0, a_sample.overstrums);
                return changed;
            }

            const int hit = a_sample.notesHit - _notesHit;
            const int bad =
                (a_sample.notesMissed - _notesMissed) +
                (a_sample.overstrums - _overstrums);
            _notesHit    = a_sample.notesHit;
            _notesMissed = a_sample.notesMissed;
            _overstrums  = a_sample.overstrums;

            int gainHits = std::max(0, hit);
            const int badJudgments = std::max(0, bad);
            if (badJudgments > 0) {
                // A mixed 10Hz batch has no reliable event order. Let the
                // mistake win: none of its same-batch hits can silently buy
                // recovery, and a fresh clean streak begins afterward.
                _recovering   = true;
                _recoveryHits = 0;
                gainHits      = 0;
            } else if (_recovering && gainHits > 0) {
                const int required =
                    std::max(0, a_params.recoveryHitsRequired);
                const int needed = std::max(0, required - _recoveryHits);
                const int banked = std::min(gainHits, needed);
                _recoveryHits += banked;
                gainHits -= banked;
                if (_recoveryHits >= required) { _recovering = false; }
            }

            const double hitScale =
                a_sample.spActive ? std::max(0.0, a_params.spHitScale) : 1.0;
            const double badScale =
                a_sample.spActive ? std::max(0.0, a_params.spBadScale) : 1.0;
            const double openingScale =
                a_sample.songSec < std::max(0.0, a_params.openingSec)
                  ? std::clamp(a_params.openingBadScale, 0.0, 1.0)
                  : 1.0;
            _value = std::clamp(
                _value +
                    static_cast<double>(gainHits) *
                        std::max(0.0, a_params.hitGain) * hitScale -
                    static_cast<double>(badJudgments) *
                        std::max(0.0, a_params.badLoss) * badScale *
                        openingScale,
                0.0, 1.0);

            const auto prior = _level;
            const double red = std::clamp(a_params.terribleBelow, 0.0, 1.0);
            const double green =
                std::clamp(a_params.greatAt, red, 1.0);
            _level = _value >= green ? Level::kGreat
                   : _value < red    ? Level::kTerrible
                                     : Level::kMiddling;
            return _level != prior;
        }

        [[nodiscard]] Level Committed() const { return _level; }
        [[nodiscard]] double Sentiment() const { return _value; }

    private:
        double _value = 0.5;
        Level  _level = Level::kMiddling;
        int    _notesHit = 0;
        int    _notesMissed = 0;
        int    _overstrums = 0;
        int    _recoveryHits = 0;
        bool   _recovering = false;
    };

    // INVARIANT: holdSec must exceed windowSec. A dip keeps diluting the
    // trailing window for a full windowSec after the dip itself ends
    // (the window cannot "forget" it any sooner), so a transient's raw
    // excursion lasts up to dipSec + windowSec regardless of how brief
    // the dip was. If holdSec <= windowSec, that excursion always
    // outlasts the hold, so holdSec cannot suppress a single bad bar - it
    // only delays the flip-flop by holdSec and then commits it anyway,
    // often committing AGAIN on the way back. Only holdSec > windowSec
    // guarantees the hold outlives the window's own memory of any
    // transient, while a genuinely sustained change keeps raw past the
    // threshold indefinitely, so its hold always still elapses. As a
    // rule of thumb, raw first crosses greatAt once a dip's bad fraction
    // exceeds windowSec * (1 - greatAt) - e.g. 8.0 * 0.15 = 1.2s at the
    // old (broken) defaults, which a single bad bar clears easily.
    struct Params {
        double windowSec     = 3.0;
        double greatAt       = 0.85;
        double terribleBelow = 0.55;
        double holdSec       = 5.0;
        double startSec      = 4.0;
    };

    // Cumulative counters as the engine already reports them.
    struct Sample {
        double songSec     = 0.0;
        int    notesHit    = 0;
        int    notesMissed = 0;
        int    overstrums  = 0;
    };

    class Mood {
    public:
        // The constructor and Reset() must set the same cold-start values -
        // give them exactly one list to agree on, instead of two that can
        // drift apart, by having construction simply call Reset().
        Mood() { Reset(); }

        void Reset() {
            _hist.clear();
            _committed  = Level::kMiddling;
            _pending    = Level::kMiddling;
            _pendingAt  = 0.0;
            _hasPending = false;
            _lastRaw    = kNoData;
        }

        // Returns true when the COMMITTED level changed on this feed.
        bool Feed(const Sample& s, const Params& p) {
            const bool countersChanged =
                _hist.empty() ||
                s.notesHit != _hist.back().notesHit ||
                s.notesMissed != _hist.back().notesMissed ||
                s.overstrums != _hist.back().overstrums;
            _hist.push_back(s);

            // windowSec is INI-tunable and can arrive at or below the feed
            // interval; read literally, the pop loop below would then
            // drain to the single newest sample every time, so `old` would
            // always equal `s`, seen would stay 0 forever, and the mood
            // would pin at whatever _lastRaw last held. Floor it instead
            // of trusting the caller, without mutating their Params.
            const double win    = std::max(p.windowSec, kMinWindowSec);
            const double cutoff = s.songSec - win;
            while (_hist.size() > 1 && _hist.front().songSec < cutoff) {
                _hist.pop_front();
            }
            // If songSec ever stops advancing (a stuck caller, a paused
            // clock that still feeds), the time-based pop above never
            // fires - bound the deque by count too.
            while (_hist.size() > kMaxSamples) {
                _hist.pop_front();
            }

            if (countersChanged) {
                const Sample& old = _hist.front();
                const int hit  = s.notesHit - old.notesHit;
                const int bad  = (s.notesMissed - old.notesMissed) +
                                 (s.overstrums - old.overstrums);
                const int seen = hit + bad;
                if (seen > 0) {
                    // Clamp: counters that go backwards (a session reset, a
                    // reload racing the window) must not poison _lastRaw and
                    // pin the mood through every later rest.
                    _lastRaw = std::clamp(
                        static_cast<double>(hit) /
                            static_cast<double>(seen),
                        0.0, 1.0);
                }
            }
            if (_lastRaw < 0.0) {
                // No real observation has ever landed - the crowd has no
                // opinion yet. Cold silence must never fabricate a commit,
                // so bail out before touching _pending/_committed at all.
                return false;
            }
            // A feed with unchanged counters is a rest, not a fresh
            // observation. Pruning old deque entries must never turn one
            // final hit into a rising Glory score during an empty outro.
            // Keep the last real ratio while still advancing the pending
            // hold clock below.
            const double raw = _lastRaw;

            const Level rawLevel = raw >= p.greatAt      ? Level::kGreat
                                 : raw < p.terribleBelow ? Level::kTerrible
                                                         : Level::kMiddling;
            if (rawLevel != _pending) {
                _pending    = rawLevel;
                _pendingAt  = s.songSec;
                _hasPending = true;
            }
            if (_pending == _committed) { return false; }
            if (s.songSec < p.startSec) { return false; }
            // _hasPending, never the sign of _pendingAt: songSec runs
            // negative through the pre-song lead-in, so a negative stamp
            // is a real timestamp, not a "nothing pending" marker.
            if (!_hasPending || s.songSec - _pendingAt < p.holdSec) {
                return false;
            }
            _committed = _pending;
            return true;
        }

        Level  Committed() const { return _committed; }
        double RawScore() const { return _lastRaw; }
        // Glory is the crowd model's live read, not GuitarEngine's old,
        // disconnected hit/miss meter. Before the room has observed a note,
        // neutral is the only honest value.
        double Sentiment() const {
            return _lastRaw < 0.0 ? 0.5 : std::clamp(_lastRaw, 0.0, 1.0);
        }

    private:
        // Floors for two INI-tunable values that would otherwise let a bad
        // config silently break the model (see the comments at their use).
        static constexpr double      kMinWindowSec = 0.5;
        // The session loop feeds this at ~10 Hz, rate-limited by the
        // nextMoodFeed gate and re-arm in Session.cpp's TickPlaying - NOT
        // at the loop's own ~200 Hz tick (the 5ms sleep at the top of
        // SessionThread). Cited by name and not by line number: this
        // comment has now gone stale twice, because later work kept adding
        // code above the sites it pointed at. 4096 samples is therefore
        // about 410s of window, so this cap no longer constrains windowSec
        // in any shipping configuration: the real bound is the invariant
        // above Params (windowSec must stay well under holdSec). The cap
        // survives purely as the stuck-clock backstop the second pop loop
        // above documents.
        static constexpr std::size_t kMaxSamples   = 4096;
        // _lastRaw's explicit "nothing observed yet" marker. Distinct from
        // any real ratio, which is always clamped into [0, 1].
        static constexpr double kNoData = -1.0;

        std::deque<Sample> _hist;
        Level  _committed;
        Level  _pending;
        double _pendingAt;
        bool   _hasPending;
        double _lastRaw;
    };

    // Deterministic fail gate layered on the same sentiment that drives
    // Glory. Entering danger never fails immediately: the room must stay
    // below the threshold for the full grace AND the player must add the
    // required number of further bad judgments. Failure is checked only on
    // a new miss/overstrum edge, so a silent rest cannot eject the player.
    struct FailureParams {
        double dangerBelow = 0.25;
        double recoverAt   = 0.35;
        double graceSec    = 3.0;
        double startSec    = 4.0;
        int    furtherBadRequired = 2;
    };

    enum class FailureState { kSafe, kDanger, kFailed };

    class FailureGate {
    public:
        void Reset() {
            _state       = FailureState::kSafe;
            _dangerAt    = 0.0;
            _badAtDanger = 0;
            _lastBad     = 0;
        }

        // Returns true exactly once, on the edge that commits failure.
        bool Feed(double a_songSec, double a_sentiment, int a_misses,
                  int a_overstrums, const FailureParams& a_p = {}) {
            const int bad = std::max(0, a_misses) + std::max(0, a_overstrums);
            if (bad < _lastBad) {
                // Counter rollback means a new/reloaded performance reached
                // a stale gate. Fail safe and start the history over.
                Reset();
            }
            const bool newBad = bad > _lastBad;
            _lastBad = bad;

            if (_state == FailureState::kFailed) { return false; }
            if (a_songSec < a_p.startSec) {
                _state = FailureState::kSafe;
                return false;
            }

            const double sentiment = std::clamp(a_sentiment, 0.0, 1.0);
            const double recoverAt = std::max(a_p.recoverAt, a_p.dangerBelow);
            if (_state == FailureState::kDanger && sentiment >= recoverAt) {
                _state = FailureState::kSafe;
            }
            if (_state == FailureState::kSafe &&
                sentiment < a_p.dangerBelow) {
                _state       = FailureState::kDanger;
                _dangerAt    = a_songSec;
                _badAtDanger = bad;
                return false;
            }
            if (_state != FailureState::kDanger) { return false; }

            const int required = std::max(1, a_p.furtherBadRequired);
            if (newBad && bad - _badAtDanger >= required &&
                a_songSec - _dangerAt >= std::max(0.0, a_p.graceSec)) {
                _state = FailureState::kFailed;
                return true;
            }
            return false;
        }

        [[nodiscard]] FailureState State() const { return _state; }
        [[nodiscard]] bool Dangerous() const {
            return _state == FailureState::kDanger;
        }

    private:
        FailureState _state       = FailureState::kSafe;
        double       _dangerAt    = 0.0;
        int          _badAtDanger = 0;
        int          _lastBad     = 0;
    };
}
