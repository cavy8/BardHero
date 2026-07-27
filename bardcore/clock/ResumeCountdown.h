#pragma once

namespace bard {
    // Deterministic 3, 2, 1, GO resume gate. It is sampled from an absolute
    // host timestamp, never accumulated per frame, so cadence cannot alter a
    // boundary. Start is idempotent while active; Cancel is always safe.
    class ResumeCountdown {
    public:
        enum class Cue : int {
            kDone  = 0,
            kThree = 3,
            kTwo   = 2,
            kOne   = 1,
            kGo    = 4,
        };

        static constexpr double kStageSeconds = 0.75;
        static constexpr double kDuration = 4.0 * kStageSeconds;

        bool Start(double now) {
            if (_active) return false;
            _startedAt = now;
            _active    = true;
            return true;
        }

        void Cancel() { _active = false; }
        [[nodiscard]] bool Active() const { return _active; }

        [[nodiscard]] Cue CueAt(double now) const {
            if (!_active) return Cue::kDone;
            const double elapsed = now - _startedAt;
            if (elapsed < kStageSeconds) return Cue::kThree;
            if (elapsed < 2.0 * kStageSeconds) return Cue::kTwo;
            if (elapsed < 3.0 * kStageSeconds) return Cue::kOne;
            if (elapsed < kDuration) return Cue::kGo;
            return Cue::kDone;
        }

        [[nodiscard]] bool Complete(double now) const {
            return _active && now - _startedAt >= kDuration;
        }

    private:
        double _startedAt = 0.0;
        bool   _active    = false;
    };
}
