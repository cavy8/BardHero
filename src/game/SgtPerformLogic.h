// src/game/SgtPerformLogic.h
#pragma once

// PURE state logic for the whole-song SGT performance keeper (no RE/OS
// includes - suite 16). The session thread owns one instance; game-thread
// keeper passes report back through a sampled tri-state, never callbacks.
// Plan: docs/plans/2026-07-19-sgt-whole-song-perform.md
namespace SH::sgtperform {

    // "is the perform effect on the player" - written by the game-thread
    // keeper pass, sampled (and consumed) by the session thread.
    enum class Seen { kUnknown = -1, kAbsent = 0, kPresent = 1 };

    struct Config {
        double tickPeriod = 5.0;    // steady keeper cadence (s)
        double startupTickPeriod = 0.1;  // poll until SGT's clip is stopped
        int    probeLimit = 3;      // absent probes before going dormant
    };

    // orders for one game-thread keeper pass
    struct TickPlan {
        bool run      = false;  // false: nothing this loop iteration
        bool keepIdle = false;  // re-up the play idle if the graph dropped it
    };

    // kOff -(session start)-> kProbing -> kLive (effect seen) or kDormant
    // (not an SGT session); kLive + absent -> kLost (latched: the user took
    // SGT's manual exit mid-song - the session should abort).
    class Logic {
    public:
        enum class State { kOff, kProbing, kLive, kDormant, kLost };

        explicit Logic(const Config& a_cfg) : cfg_(a_cfg) {}

        void OnSessionStart(double a_now) {
            state_           = State::kProbing;
            probes_          = 0;
            startupComplete_ = false;
            nextTick_        = a_now;  // first keeper pass fires immediately
        }
        void OnSessionEnd() { state_ = State::kOff; }

        // Caller MUST feed each keeper-pass result exactly once (typically
        // via atomic.exchange(-1) on the shared observation, NOT .load()).
        // Repeated delivery of the same non-kUnknown value while kProbing
        // double-counts absent probes and can trip probeLimit early.
        // (Do NOT "fix" this with edge-triggered dedup: consecutive real
        // passes legitimately deliver repeated kAbsent values.)
        void Observe(Seen a_seen, bool a_clipStopped = false) {
            if (a_clipStopped) { startupComplete_ = true; }
            if (a_seen == Seen::kUnknown) { return; }
            switch (state_) {
            case State::kProbing:
                if (a_seen == Seen::kPresent) {
                    state_ = State::kLive;
                    // The first pass commonly finds the effect before
                    // Papyrus fills SongToPlay. Do not retain the probing
                    // cadence's five-second deadline: poll the instance
                    // immediately, then at the startup cadence until stopped.
                    if (!startupComplete_) { nextTick_ = 0.0; }
                } else if (++probes_ >= cfg_.probeLimit) {
                    state_ = State::kDormant;
                }
                break;
            case State::kLive:
                if (a_seen == Seen::kAbsent) { state_ = State::kLost; }
                break;
            default:
                break;
            }
        }

        TickPlan NextTick(double a_now, bool a_playing, bool a_keepIdleIni) {
            TickPlan p;
            if (state_ != State::kProbing && state_ != State::kLive) {
                return p;
            }
            if (a_now < nextTick_) { return p; }
            const double period =
                state_ == State::kLive && !startupComplete_
                    ? cfg_.startupTickPeriod
                    : cfg_.tickPeriod;
            nextTick_  = a_now + period;
            p.run      = true;
            p.keepIdle = a_playing && a_keepIdleIni && state_ == State::kLive;
            return p;
        }

        // Pause recovery (design 2026-07-19): a paused session's performance
        // can die (SGT's manual-exit listener stays hot while the input hook
        // is disengaged). On resume the session re-adds the ability and this
        // re-arms probing. Valid from kLive too: a death between keeper
        // passes is invisible, so resume always re-probes.
        void Recover(double a_now) {
            if (state_ != State::kLive && state_ != State::kLost) { return; }
            state_           = State::kProbing;
            probes_          = 0;
            startupComplete_ = false;
            nextTick_        = a_now;
        }

        // current machine state (tests/diagnostics)
        [[nodiscard]] State state() const { return state_; }
        // this session was DEFINITELY SGT-triggered (live OR lost)
        [[nodiscard]] bool Confirmed() const {
            return state_ == State::kLive || state_ == State::kLost;
        }
        // effect vanished mid-session (latched) - abort the session
        [[nodiscard]] bool Lost() const { return state_ == State::kLost; }

    private:
        Config cfg_;
        State  state_    = State::kOff;
        int    probes_   = 0;
        double nextTick_ = 0.0;
        bool   startupComplete_ = false;
    };

    // Browse standstill (design 2026-07-19): while the song browser is open
    // the player just stands - the SGT performance the trigger started is
    // stripped, and the real one starts at song pick (AddSpell). "Settle
    // then strip": SGT's OnEffectStart thread keeps running after a dispel
    // (Papyrus never kills in-flight threads), so an immediate RemoveSpell
    // leaves its PlayIdle + Sound.Play to land on a dead effect with nothing
    // left to stop them (ghost animation + music). Instead the watcher waits
    // until the clip instance exists (SongToPlay filled -> stopped by the
    // pass), lingers one pass (one more UnregisterForUpdate swallows a late
    // 32.8s timer registration), THEN strips - removal only ever hits a
    // fully-started effect, the exact state SGT's own manual exit handles.
    class Standstill {
    public:
        enum class State { kOff, kSettling, kLinger, kStripReady, kDone };
        // observation from one game-thread pass (SgtVm::StandstillPass):
        // kStarting = effect present, clip not started yet;
        // kStopped  = clip instance existed and is stopped (this pass or a
        //             prior one)
        enum class Pass { kUnknown = -1, kAbsent = 0, kStarting = 1,
                          kStopped = 2 };
        struct Plan {
            bool run     = false;  // dispatch a game-thread pass
            bool strip   = false;  // this pass removes the ability
            bool waitLog = false;  // one-shot "still waiting" diagnostic
        };

        explicit Standstill(double a_tickPeriod = 0.5)
            : tick_(a_tickPeriod) {}

        void Begin(double a_now) {
            state_  = State::kSettling;
            next_   = a_now;  // first pass fires immediately
            passes_ = 0;
        }
        // song picked (or session started any other way) - stop stripping;
        // a fast pick can land before the strip, the performance then simply
        // continues and the session AddSpell is a no-op
        void Cancel() { state_ = State::kOff; }

        // Same consume-exactly-once contract as Logic::Observe.
        void Observe(Pass a_seen) {
            if (a_seen == Pass::kUnknown || state_ == State::kOff ||
                state_ == State::kDone) {
                return;
            }
            if (a_seen == Pass::kAbsent) {
                // effect vanished on its own (SGT self-exit: player sitting,
                // manual exit, load) - nothing left to strip
                state_ = State::kDone;
            } else if (a_seen == Pass::kStopped &&
                       state_ == State::kSettling) {
                state_ = State::kLinger;
            }
        }

        // a_frozen: the browse world-freeze is holding SGT's start thread -
        // settling CANNOT progress, so frozen passes do not count toward
        // the ~10s "still waiting" diagnostic (it would otherwise fire on
        // every browse longer than 10s and mean nothing).
        Plan NextTick(double a_now, bool a_frozen = false) {
            Plan p;
            if (state_ == State::kOff || state_ == State::kDone) { return p; }
            if (a_now < next_) { return p; }
            next_ = a_now + tick_;
            p.run = true;
            if (!a_frozen) { ++passes_; }
            if (state_ == State::kLinger) {
                state_ = State::kStripReady;
            } else if (state_ == State::kStripReady) {
                p.strip = true;
                state_  = State::kDone;
            } else if (passes_ == 20) {
                // ~10s and SGT's clip still never filled (a native menu can
                // park its start thread in WaitMenuMode) - say so once
                p.waitLog = true;
            }
            return p;
        }

        [[nodiscard]] State state() const { return state_; }
        [[nodiscard]] bool  Active() const {
            return state_ != State::kOff && state_ != State::kDone;
        }

    private:
        double tick_;
        State  state_  = State::kOff;
        double next_   = 0.0;
        int    passes_ = 0;
    };
}
