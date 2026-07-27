#include "engine/GuitarEngine.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace bard {

    GuitarEngine::GuitarEngine(const ParsedChart& chart,
                               const EngineParams& params)
        : _chart(chart), _p(params) {
        _notes.resize(chart.notes.size());
        _phrases.resize(chart.spPhrases.size());
        _solos.resize(chart.solos.size());
        _noteSolo.assign(chart.notes.size(), -1);
        _soloLast.assign(chart.solos.size(), -1);
        _noteSustainFirst.assign(chart.notes.size(), -1);
        _noteSustainCount.assign(chart.notes.size(), 0);
        for (std::size_t si = 0; si < chart.solos.size(); ++si) {
            const auto& so = chart.solos[si];
            for (std::size_t ni = 0; ni < chart.notes.size(); ++ni) {
                if (chart.notes[ni].tick >= so.startTick &&
                    chart.notes[ni].tick <= so.endTick) {
                    _noteSolo[ni] = static_cast<std::int32_t>(si);
                    _soloLast[si] = static_cast<std::int32_t>(ni);
                }
            }
        }
    }

    void GuitarEngine::Queue(const NoteInput& input) {
        NoteInput q = input;
        // Monotonic clamp (spec section 3 / YARG semantics): out-of-order
        // input is moved FORWARD to the previous input's time, never
        // reordered.
        if (!_queue.empty() && q.time < _queue.back().time) {
            q.time = _queue.back().time;
        }
        _queue.push_back(q);
    }

    void GuitarEngine::Update(double time) {
        while (_queueHead < _queue.size() &&
               _queue[_queueHead].time <= time) {
            NoteInput in = _queue[_queueHead++];
            if (in.time < _now) in.time = _now;  // late delivery: clamp fwd
            AdvanceTo(in.time);                  // leaves _now == in.time
            RunStep(_now, &in);
        }
        AdvanceTo(time);
    }

    void GuitarEngine::AdvanceTo(double target) {
        while (_now < target) {
            const double b = NextBoundary(_now, target);
            AccrueSustains(_now, b);
            _now = b;
            RunStep(_now, nullptr);
        }
    }

    void GuitarEngine::RunStep(double now, const NoteInput* input) {
        // 0. integrate SP over the closed interval and move its anchor here.
        // Every SP rate change (whammy input, window close, sustain finish,
        // activation, phrase award, zero) happens AT a landing, so rates are
        // constant between anchors and this stays a pure function of time.
        ReanchorSp(now);
        // 1. expire timers - the ONLY source of deferred overstrum (spec 5.2)
        if (_hopoExpire <= now) _hopoExpire = kInf;
        if (_strumExpire <= now) {
            _strumExpire = kInf;
            Overstrum(now);
        }
        // 2+3. input handling
        if (input) ApplyInput(now, *input);
        // 4. hit logic to fixpoint (spec: re-runs after each landing)
        while (CheckForNoteHit(now)) {}
        // 5. sustains + solo + per-step flag clear
        UpdateSustains(now);
        UpdateSolo(now);
        _strumThisStep = false;
    }

    double GuitarEngine::NextBoundary(double after, double limit) const {
        double b        = limit;
        auto   consider = [&](double t) {
            if (t > after && t < b) b = t;
        };
        // head-note miss point (back end + 1 ulp) and pending front-end
        // entries (a buffered strum or standing fret credit can hit exactly
        // when a note's window opens)
        for (std::size_t i = _head; i < _chart.notes.size(); ++i) {
            if (_notes[i].judged != Judgment::kPending) continue;
            const double t = _chart.notes[i].time;
            if (t - _p.maxWindow > limit) break;
            consider(t + FrontEnd(i));
            if (i == _head) {
                consider(std::nextafter(t + BackEnd(i), kInf));
            }
        }
        // timers
        if (_strumExpire < kInf) consider(_strumExpire);
        if (_hopoExpire < kInf) consider(_hopoExpire);
        // sustains: burst, true end, drop deadlines (Task 16)
        for (const auto& s : _sustains) {
            if (s.finished) continue;
            if (!s.burstDone) consider(s.burstTime);
            consider(s.endTime);
            if (s.dropDeadline < kInf) consider(s.dropDeadline);
        }
        // star power boundaries (Task 17)
        if (_stats.spActive) consider(SpZeroTime());
        if (_lastWhammy > -kInf) consider(_lastWhammy + kWhammyWindow);
        return b;
    }

    // ---- windows ----------------------------------------------------------

    double GuitarEngine::WindowFor(std::size_t i) const {
        if (!_p.isDynamic) return _p.maxWindow;
        // W(d) = clamp((d/(S*Wmax))^g * (Wmax - k*Wmin) + k*Wmin, Wmin, Wmax)
        // d = mean of gap-before and gap-after; edge notes use their one gap.
        const auto& notes = _chart.notes;
        double      gapB = -1.0, gapA = -1.0;
        if (i > 0) gapB = notes[i].time - notes[i - 1].time;
        if (i + 1 < notes.size()) gapA = notes[i + 1].time - notes[i].time;
        const double d = (gapB >= 0 && gapA >= 0)
                             ? 0.5 * (gapB + gapA)
                             : (gapB >= 0 ? gapB
                                          : (gapA >= 0 ? gapA : _p.maxWindow));
        const double raw =
            std::pow(d / (_p.dynamicScale * _p.maxWindow), _p.dynamicGamma) *
                (_p.maxWindow - _p.dynamicSlope * _p.minWindow) +
            _p.dynamicSlope * _p.minWindow;
        return std::clamp(raw, _p.minWindow, _p.maxWindow);
    }
    double GuitarEngine::FrontEnd(std::size_t i) const {
        return -(WindowFor(i) / 2.0) * _p.frontToBackRatio;
    }
    double GuitarEngine::BackEnd(std::size_t i) const {
        return (WindowFor(i) / 2.0) * (2.0 - _p.frontToBackRatio);
    }
    bool GuitarEngine::InWindow(std::size_t i, double now) const {
        const double t = _chart.notes[i].time;
        return now >= t + FrontEnd(i) && now <= t + BackEnd(i);
    }

    // ---- input ------------------------------------------------------------

    void GuitarEngine::ApplyInput(double now, const NoteInput& in) {
        switch (in.action) {
            case InputAction::kStrum: {
                if (!in.value) break;  // replays may carry 0s - ignore
                if (_hopoExpire < kInf) {  // EATEN: kills both timers (5.2)
                    _hopoExpire  = kInf;
                    _strumExpire = kInf;
                    break;  // the eaten strum cannot hit a later note
                }
                if (_strumExpire < kInf) Overstrum(now);  // double strum
                _strumThisStep = true;
                const bool inWindow =
                    _head < _chart.notes.size() && InWindow(_head, now);
                _strumExpire = now + (inWindow ? _p.strumLeniency
                                               : _p.strumLeniencySmall);
                break;
            }
            case InputAction::kFret1:
            case InputAction::kFret2:
            case InputAction::kFret3:
            case InputAction::kFret4:
            case InputAction::kFret5: {
                const auto bit = static_cast<std::uint8_t>(
                    1u << static_cast<int>(in.action));
                const std::uint8_t before = _held;
                if (in.value) {
                    _held |= bit;
                } else {
                    _held &= ~bit;
                }
                if (_held == before) break;  // not a fret CHANGE
                _hasTapped = true;
                if (_head < _chart.notes.size()) {
                    _frontEndExpire = _p.infiniteFrontEnd
                                          ? kInf
                                          : now + std::abs(FrontEnd(_head));
                }
                // ghost check (spec 5.5): press only; predecessor exists;
                // note in window; press raised the TOP fret; no held fret in
                // the note's mask. Detection always runs; the antiGhosting
                // setting only gates the penalty (checked at hit time).
                if (in.value && _head > 0 && _head < _chart.notes.size() &&
                    InWindow(_head, now) && bit > before &&
                    (_chart.notes[_head].mask & _held) == 0) {
                    _notes[_head].ghosted = true;
                    _stats.ghostInputs++;
                }
                break;
            }
            case InputAction::kStarPower:
                if (in.value) TryActivateSp();
                break;
            case InputAction::kWhammy:
                ReanchorSp(now);
                _lastWhammy = now;
                break;
            case InputAction::kPause:
                break;  // host-side; engine time simply stops arriving
        }
    }

    // ---- hit logic ----------------------------------------------------------

    bool GuitarEngine::CanNoteBeHit(const Note& n) const {
        // Frets owned by active extended sustains are subtracted before
        // testing (spec 5.2) - but never the candidate note's OWN frets: a
        // same-lane repeat inside its extended sustain must stay hittable
        // (spec 5.4: hitting a note sharing a fret with a sustain drops it,
        // which presumes such hits happen).
        std::uint8_t held = _held;
        for (const auto& s : _sustains) {
            if (!s.finished && s.extended) {
                held &= static_cast<std::uint8_t>(~(s.maskBits & ~n.mask));
            }
        }
        const std::uint8_t mask = held ? held : kOpenBit;
        if (n.mask == kOpenBit) return mask == kOpenBit;  // open: exactly open
        if (mask == kOpenBit) return false;               // nothing held
        if (std::popcount(n.mask) == 1) {
            // single: anchoring allowed - the HIGHEST held bit is the note
            const std::uint8_t top = static_cast<std::uint8_t>(
                1u << (std::bit_width(static_cast<unsigned>(mask)) - 1));
            return top == n.mask;
        }
        if (n.isHopo || n.isTap) {
            // chord anchoring only BELOW the chord's lowest fret
            if ((mask & n.mask) != n.mask) return false;
            const std::uint8_t extras = mask & static_cast<std::uint8_t>(~n.mask);
            const std::uint8_t lowest = n.mask & static_cast<std::uint8_t>(-n.mask);
            return extras < lowest;
        }
        return mask == n.mask;  // strum chords: exact
    }

    bool GuitarEngine::CheckForNoteHit(double now) {
        for (std::size_t i = _head; i < _chart.notes.size(); ++i) {
            if (_notes[i].judged != Judgment::kPending) continue;
            const Note&  n     = _chart.notes[i];
            const double front = n.time + FrontEnd(i);
            const double back  = n.time + BackEnd(i);
            if (now > back) {
                if (i == _head) {
                    MissNote(now, i);
                    return true;  // fixpoint re-runs
                }
                return false;
            }
            if (now < front) return false;
            if (!CanNoteBeHit(n)) continue;  // skip - never a penalty

            const bool isHead = (i == _head);
            const bool hopoOk =
                n.isHopo && isHead && (_stats.combo > 0 || i == 0);
            const bool tapOk = n.isTap && (isHead || _stats.combo == 0);
            const bool frontEndOk =
                _p.infiniteFrontEnd || now <= _frontEndExpire;
            const bool ghostBlock = _notes[i].ghosted && _p.antiGhosting;
            if (_hasTapped && (hopoOk || tapOk) && frontEndOk && !ghostBlock) {
                HitNote(now, i, false);
                return true;
            }
            if ((_strumThisStep || _strumExpire < kInf) &&
                (isHead || _stats.combo == 0)) {
                HitNote(now, i, true);
                return true;
            }
        }
        return false;
    }

    void GuitarEngine::HitNote(double now, std::size_t idx, bool viaStrum) {
        // hitting a non-head note force-misses everything skipped (spec 5.2)
        for (std::size_t j = _head; j < idx; ++j) {
            if (_notes[j].judged == Judgment::kPending) MissNote(now, j);
        }
        const Note& n       = _chart.notes[idx];
        _notes[idx].judged  = Judgment::kHit;
        _notes[idx].ghosted = false;
        _anyNoteResolved    = true;
        _stats.notesHit++;
        _stats.glory = std::min(1.0, _stats.glory + 0.0125);
        _stats.combo++;
        _stats.maxCombo   = std::max(_stats.maxCombo, _stats.combo);
        _stats.multiplier = std::min(_stats.combo / 10 + 1, _p.maxMultiplier);
        const int lanes   = std::popcount(n.mask);
        _stats.score += 50ll * lanes * EffectiveMultiplier();

        _strumExpire = kInf;  // spec: strum timer killed early on hit
        if (viaStrum) {
            _strumThisStep  = false;
            _hasTapped      = true;  // stays true - next hopo is frettable
            _frontEndExpire = kInf;  // front end reset to infinity
        } else {
            _hasTapped  = false;  // each hopo needs its own fret event
            _hopoExpire = now + _p.hopoLeniency;
        }
        if (n.spPhrase >= 0) PhraseNoteHit(idx);
        if (_noteSolo[idx] >= 0) _solos[_noteSolo[idx]].hits++;
        StartSustains(now, idx);
        AdvanceHead();
    }

    void GuitarEngine::MissNote(double now, std::size_t idx) {
        _notes[idx].judged  = Judgment::kMissed;
        _notes[idx].ghosted = false;
        _anyNoteResolved    = true;
        _stats.notesMissed++;
        _stats.glory = std::max(0.0, _stats.glory - 0.075);
        _stats.combo      = 0;
        _stats.multiplier = 1;
        _hasTapped        = false;
        if (_chart.notes[idx].spPhrase >= 0) StripPhraseOf(idx);
        AdvanceHead();
        (void)now;
    }

    void GuitarEngine::Overstrum(double now) {
        // Both suppressions below stay EXACTLY as they were for scoring;
        // they only gained a presentation counter, so the host can sound
        // a dud strum in the lead-in and the outro without any of it
        // reaching combo, Glory or the fail gate.
        if (!_anyNoteResolved) {
            _stats.unscoredStrums++;
            return;  // suppressed before first resolve
        }
        const bool tailQuiet =
            _head >= _chart.notes.size() &&
            std::all_of(_sustains.begin(), _sustains.end(),
                        [](const Sustain& s) { return s.finished; });
        if (tailQuiet) {
            _stats.unscoredStrums++;
            return;  // suppressed after last note, no sustains
        }
        _stats.overstrums++;
        _stats.glory = std::max(0.0, _stats.glory - 0.04);
        _stats.combo      = 0;
        _stats.multiplier = 1;
        BreakAllSustains(now);  // score committed, not refunded (spec 5.2)
        if (_head < _chart.notes.size()) {
            const auto& n = _chart.notes[_head];
            if (n.spPhrase >= 0 &&
                _chart.spPhrases[n.spPhrase].startTick != n.tick) {
                StripPhraseOf(_head);  // stripped unless the note STARTS it
            }
        }
    }

    void GuitarEngine::AdvanceHead() {
        while (_head < _notes.size() &&
               _notes[_head].judged != Judgment::kPending) {
            ++_head;
        }
    }

    // ---- sustains (spec 5.3/5.4) -------------------------------------------

    void GuitarEngine::StartSustains(double now, std::size_t idx) {
        const Note& n = _chart.notes[idx];
        // A new note sharing a fret with an active sustain drops that
        // sustain (spec 5.4). Score committed, not refunded.
        for (auto& s : _sustains) {
            if (!s.finished && (s.maskBits & n.mask & ~kOpenBit)) {
                FinishSustain(s);
            }
        }
        // Uniform chord (all sustained lanes equal length) -> ONE object,
        // all-or-nothing; otherwise one object per sustained lane (disjoint,
        // spec 5.4). Scoring is per lane either way (25/beat/lane, PINNED).
        std::uint32_t uniformLen = 0;
        bool          uniform    = true;
        int           lanes      = 0;
        int           firstLane  = -1;
        for (int l = 0; l < kLaneCount; ++l) {
            if (!(n.mask & LaneBit(l))) continue;
            const auto len = n.sustainTicks[l];
            if (lanes == 0) {
                uniformLen = len;
                firstLane  = l;
            } else if (len != uniformLen) {
                uniform = false;
            }
            ++lanes;
        }
        auto add = [&](std::uint8_t bits, std::uint32_t len, int width,
                       int lane) {
            if (len == 0) return;
            Sustain s;
            s.note      = idx;
            s.maskBits  = bits & static_cast<std::uint8_t>(~kOpenBit);
            s.open      = (bits & kOpenBit) != 0;
            s.startTick = n.tick;
            s.endTick   = n.tick + len;
            s.endTime   = n.sustainEnd[lane];
            // burst: all remaining points award one sixteenth (res/4) before
            // the notated end; the object lives to its true end (spec 5.3)
            const double burstTick = std::max(
                static_cast<double>(n.tick),
                static_cast<double>(s.endTick) - _chart.resolution / 4.0);
            s.burstTime =
                _chart.tempo.SecondsAt(burstTick) + _chart.offsetSeconds;
            s.lanes    = width;
            s.extended = (n.extendedMask & bits) != 0;
            _sustains.push_back(s);
            if (_noteSustainFirst[idx] < 0)
                _noteSustainFirst[idx] =
                    static_cast<std::int32_t>(_sustains.size()) - 1;
            ++_noteSustainCount[idx];
        };
        if (uniform && lanes >= 1) {
            add(n.mask, uniformLen, lanes, firstLane);
        } else {
            for (int l = 0; l < kLaneCount; ++l) {
                if (n.mask & LaneBit(l)) {
                    add(LaneBit(l), n.sustainTicks[l], 1, l);
                }
            }
        }
        (void)now;
    }

    void GuitarEngine::SustainMasks(std::size_t idx, std::uint8_t& active,
                                    std::uint8_t& dropped) const {
        active  = 0;
        dropped = 0;
        const auto first = _noteSustainFirst[idx];
        for (std::int32_t k = 0; k < _noteSustainCount[idx]; ++k) {
            const Sustain&     s    = _sustains[first + k];
            const std::uint8_t bits = s.open ? kOpenBit : s.maskBits;
            if (s.finished) dropped |= bits;
            else            active |= bits;
        }
    }

    void GuitarEngine::AccrueSustains(double from, double to) {
        // Integer tick-points at res/25 spacing - a cumulative counter over
        // a pure function of time, so chunking cannot change totals
        // (determinism rule). 1 base point per point per lane, multiplied at
        // accrual time: points earned before a multiplier change keep the
        // old multiplier (the spec 5.3 rebase semantics, float-free).
        const double spacing = _chart.resolution / 25.0;
        for (auto& s : _sustains) {
            if (s.finished || s.burstDone) continue;
            const double capT = std::min(to, s.burstTime);
            if (capT <= from) continue;
            const double tick =
                _chart.tempo.TickAt(capT - _chart.offsetSeconds);
            const auto target = static_cast<std::uint32_t>(std::max(
                0.0, (tick - static_cast<double>(s.startTick)) / spacing));
            const auto total  = static_cast<std::uint32_t>(
                (s.endTick - s.startTick) / spacing);
            const auto capped = std::min(target, total);
            if (capped > s.scoredPoints) {
                const auto pts =
                    static_cast<std::int64_t>(capped - s.scoredPoints) *
                    s.lanes * EffectiveMultiplier();
                s.scoredPoints = capped;
                _stats.sustainScore += pts;
                _stats.score += pts;
            }
        }
    }

    void GuitarEngine::UpdateSustains(double now) {
        const double spacing = _chart.resolution / 25.0;
        for (auto& s : _sustains) {
            if (s.finished) continue;
            if (now >= s.burstTime && !s.burstDone) {
                const auto total = static_cast<std::uint32_t>(
                    (s.endTick - s.startTick) / spacing);
                if (total > s.scoredPoints) {
                    const auto pts =
                        static_cast<std::int64_t>(total - s.scoredPoints) *
                        s.lanes * EffectiveMultiplier();
                    s.scoredPoints = total;
                    _stats.sustainScore += pts;
                    _stats.score += pts;
                }
                s.burstDone = true;
            }
            if (now >= s.endTime) {
                FinishSustain(s);
                continue;
            }
            // hold test: required frets ⊆ held (extras are anchoring, spec
            // 5.4); open sustains cannot be fret-dropped (PINNED)
            const bool ok = s.open || (s.maskBits & _held) == s.maskBits;
            if (ok) {
                s.held         = true;
                s.dropDeadline = kInf;  // grace fully resets on re-grip
            } else if (s.dropDeadline == kInf) {
                s.dropDeadline = now + _p.sustainDropLeniency;
            } else if (now >= s.dropDeadline) {
                FinishSustain(s);  // accrued (incl. grace) stays committed
            }
        }
    }

    void GuitarEngine::FinishSustain(Sustain& s) { s.finished = true; }
    void GuitarEngine::BreakAllSustains(double) {
        for (auto& s : _sustains) {
            if (!s.finished) FinishSustain(s);
        }
    }

    // ---- star power (spec 5.6) ---------------------------------------------
    // Measure ticks == chart ticks (1 measure = 4*res regardless of TS).
    // Full bar 32*res, activation minimum 16*res, phrase award 8*res,
    // whammy fills a full bar per 30 beats -> +32/30 per tick, drain 1:1.

    void GuitarEngine::PhraseNoteHit(std::size_t idx) {
        const auto pi = _chart.notes[idx].spPhrase;
        auto&      ps = _phrases[pi];
        ps.hits++;
        const auto& phrase = _chart.spPhrases[pi];
        if (!ps.stripped &&
            static_cast<std::int32_t>(idx) == phrase.lastNoteIndex &&
            ps.hits == phrase.noteCount) {
            // award on the last note, all notes comboed (anchor is already
            // at the current step, see RunStep step 0)
            const double cap = 32.0 * _chart.resolution;
            _spAnchorGauge =
                std::min(_spAnchorGauge + 8.0 * _chart.resolution, cap);
            _stats.spGaugeMeasureTicks = _spAnchorGauge;
            _stats.spPhrasesCompleted++;
        }
    }

    void GuitarEngine::StripPhraseOf(std::size_t idx) {
        _phrases[_chart.notes[idx].spPhrase].stripped = true;
    }

    void GuitarEngine::TryActivateSp() {
        // anchor is at the current step (RunStep step 0)
        if (!_stats.spActive &&
            _spAnchorGauge >= 16.0 * _chart.resolution) {
            _stats.spActive = true;  // drain runs from this anchor
        }
    }

    bool GuitarEngine::WhammyEngagedIn(double t) const {
        if (t - _lastWhammy > kWhammyWindow) return false;
        // whammy gains only on an SP-phrase note's live sustain (spec 5.6)
        for (const auto& s : _sustains) {
            if (!s.finished && _chart.notes[s.note].spPhrase >= 0) {
                return true;
            }
        }
        return false;
    }

    double GuitarEngine::SpGaugeAt(double t) const {
        const double tick = _chart.tempo.TickAt(t - _chart.offsetSeconds);
        const double dt   = tick - _spAnchorTick;
        double       g    = _spAnchorGauge;
        if (dt > 0.0) {
            if (_stats.spActive) g -= dt;
            if (WhammyEngagedIn(t)) g += dt * (32.0 / 30.0);
        }
        return std::clamp(g, 0.0, 32.0 * static_cast<double>(_chart.resolution));
    }

    double GuitarEngine::SpZeroTime() const {
        if (!_stats.spActive) return kInf;
        // Drain-only estimate; if whammy gain is running, landing early is a
        // harmless extra boundary (re-anchor finds gauge > 0 and continues).
        return _chart.tempo.SecondsAt(_spAnchorTick + _spAnchorGauge) +
               _chart.offsetSeconds;
    }

    void GuitarEngine::ReanchorSp(double now) {
        _spAnchorGauge = SpGaugeAt(now);
        _spAnchorTick  = _chart.tempo.TickAt(now - _chart.offsetSeconds);
        // Only reaching zero ends SP (spec 5.6). Epsilon so a float-dust
        // gauge cannot schedule a zero boundary at now+ulp forever.
        if (_stats.spActive && _spAnchorGauge <= 1e-6) {
            _spAnchorGauge  = 0.0;
            _stats.spActive = false;
        }
        _stats.spGaugeMeasureTicks = _spAnchorGauge;
    }

    // ---- solo bonus (spec 5.3, formula PINNED) -----------------------------

    void GuitarEngine::UpdateSolo(double now) {
        for (std::size_t si = 0; si < _chart.solos.size(); ++si) {
            auto& st = _solos[si];
            if (st.awarded) continue;
            const auto last = _soloLast[si];
            if (last < 0 ||
                _notes[last].judged == Judgment::kPending) {
                continue;  // solo still running
            }
            const int total = _chart.solos[si].noteCount;
            if (total > 0) {
                const double p = static_cast<double>(st.hits) / total;
                const double scale = p < 0.6 ? 0.0 : (p - 0.6) / 0.4;
                const auto   bonus =
                    static_cast<std::int64_t>(total * 100.0 * scale / 50.0) *
                    50;  // floor to nearest 50
                _stats.soloBonus += bonus;
                _stats.score += bonus;
            }
            st.awarded = true;
        }
        (void)now;
    }
}
