#pragma once

namespace SH::audience {
    inline constexpr double kFailureFeedbackHoldSec = 1.75;

    enum class Action {
        kNone,
        kSetStage10,
        kResetAndSetStage10,
    };

    // SGT stage 10 owns the live audience scene. Stage 20 is applause and
    // stage 200 is terminal teardown. Seeing either terminal stage during a
    // live BardHero performance means an earlier effect's delayed cleanup
    // overtook the new performance and the quest must be restarted.
    [[nodiscard]] constexpr Action Plan(int a_stage) {
        if (a_stage < 10) { return Action::kSetStage10; }
        if (a_stage < 20) { return Action::kNone; }
        return Action::kResetAndSetStage10;
    }

    struct FailureFeedbackPlan {
        bool dispatchFeedback = false;
        bool stopAudience     = false;
        bool teardown         = false;
        bool blockStarts      = false;
    };

    // Session-thread policy for a crowd-loss humiliation beat. Beginning the
    // sequence dispatches the negative bark and stops the applause-owning
    // audience quest immediately while the Terrible global is still live.
    // Poll holds only spell/mood teardown for a short deterministic window,
    // then releases the start gate as the caller removes the effect.
    class FailureFeedbackSequence {
    public:
        [[nodiscard]] FailureFeedbackPlan Begin(double a_now) {
            pending_   = true;
            teardownAt_ = a_now + kFailureFeedbackHoldSec;
            return { true, true, false, true };
        }

        [[nodiscard]] FailureFeedbackPlan Poll(double a_now) {
            if (!pending_) { return {}; }
            if (a_now < teardownAt_) {
                return { false, false, false, true };
            }
            pending_ = false;
            return { false, false, true, false };
        }

        void Cancel() {
            pending_   = false;
            teardownAt_ = 0.0;
        }

        [[nodiscard]] bool Pending() const { return pending_; }

    private:
        bool   pending_    = false;
        double teardownAt_ = 0.0;
    };
}
