#pragma once

namespace SH::user_song_watch {
    // Pure debounce gate for a noisy filesystem watcher. Every file change
    // restarts the quiet period. TakeDue consumes exactly one settled batch.
    class QuietPeriod {
    public:
        explicit QuietPeriod(double seconds) :
            seconds_(seconds > 0.0 ? seconds : 0.0) {}

        void ObserveChange(double nowSeconds) {
            pending_  = true;
            deadline_ = nowSeconds + seconds_;
        }

        [[nodiscard]] bool Pending() const { return pending_; }
        [[nodiscard]] double Deadline() const { return deadline_; }

        bool TakeDue(double nowSeconds) {
            if (!pending_ || nowSeconds < deadline_) return false;
            pending_ = false;
            return true;
        }

    private:
        double seconds_  = 0.0;
        double deadline_ = 0.0;
        bool   pending_  = false;
    };
}
