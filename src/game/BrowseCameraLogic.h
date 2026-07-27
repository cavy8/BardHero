#pragma once

// PURE cross-thread gate for Songbook camera preparation (suite 20).
//
// The session thread requests an open, but only a later game-thread task may
// inspect/modify PlayerCamera. The render-visible open request must therefore
// stay unpublished until that task has sampled the camera, forced third person
// when needed, and called Complete(). A monotonically increasing token also
// makes queued work harmless after a load, teardown, or newer request.

#include <atomic>
#include <cstdint>

namespace SH::browsecamera {

    // The performance orbit uses Skyrim's own auto-vanity camera rather than
    // writing yaw/position every frame. The runtime maps PlayerCamera's state
    // IDs onto these pure categories before asking for the next action.
    enum class PerformanceCameraKind {
        kUnavailable,
        kFirstPerson,
        kAutoVanity,
        kThirdPerson,
        kTransition
    };

    enum class PerformanceVanityAction {
        kCancel,
        kDone,
        kForceThirdPerson,
        kEnterAutoVanity,
        kRetry,
        kGiveUp
    };

    inline constexpr int kPerformanceVanityMaxAttempts = 8;

    [[nodiscard]] constexpr PerformanceVanityAction PlanPerformanceVanity(
        bool a_requestCurrent, bool a_sessionActive, bool a_enabled,
        PerformanceCameraKind a_camera, int a_attempt) {
        if (!a_requestCurrent || !a_sessionActive || !a_enabled) {
            return PerformanceVanityAction::kCancel;
        }
        if (a_camera == PerformanceCameraKind::kAutoVanity) {
            return PerformanceVanityAction::kDone;
        }
        if (a_camera == PerformanceCameraKind::kFirstPerson) {
            return PerformanceVanityAction::kForceThirdPerson;
        }
        if (a_camera == PerformanceCameraKind::kThirdPerson) {
            return PerformanceVanityAction::kEnterAutoVanity;
        }
        return a_attempt < kPerformanceVanityMaxAttempts
                 ? PerformanceVanityAction::kRetry
                 : PerformanceVanityAction::kGiveUp;
    }

    struct SamplePlan {
        bool current          = false;
        bool forceThirdPerson = false;
    };

    class OpenGate {
    public:
        using Token = std::uint64_t;

        [[nodiscard]] Token Request() {
            const auto token =
                generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
            prepared_.store(0, std::memory_order_release);
            return token;
        }

        void Cancel() {
            generation_.fetch_add(1, std::memory_order_acq_rel);
            prepared_.store(0, std::memory_order_release);
        }

        [[nodiscard]] SamplePlan Sample(Token a_token, bool a_hasCamera,
                                        bool a_isFirstPerson) const {
            if (generation_.load(std::memory_order_acquire) != a_token) {
                return {};
            }
            return { true, a_hasCamera && a_isFirstPerson };
        }

        // Called only after the requested camera force (if any) returned.
        // False means a newer request/cancel overtook this task.
        [[nodiscard]] bool Complete(Token a_token) {
            if (generation_.load(std::memory_order_acquire) != a_token) {
                return false;
            }
            prepared_.store(a_token, std::memory_order_release);
            return generation_.load(std::memory_order_acquire) == a_token;
        }

        [[nodiscard]] bool PublishAllowed(Token a_token) const {
            return generation_.load(std::memory_order_acquire) == a_token &&
                   prepared_.load(std::memory_order_acquire) == a_token;
        }

    private:
        std::atomic<Token> generation_{ 0 };
        std::atomic<Token> prepared_{ 0 };
    };
}
