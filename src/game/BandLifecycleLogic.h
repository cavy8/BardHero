#pragma once

namespace bard::band {
    inline constexpr int kGuitarContext = 3;
    inline constexpr float kDismissVisualSeconds = 1.5f;
    inline constexpr float kDismissDeleteDelaySeconds = 1.6f;
    inline constexpr double kResultsRevealDelaySeconds = 1.75;

    enum class BeginDecision {
        kIgnore,
        kSpawn,
        kAlreadyActive,
    };

    enum class ResourceDecision {
        kAbort,
        kSpawnWithoutDrum,
        kSpawnWithDrum,
    };

    struct ArrivalDecision {
        bool hide{ false };
        bool reveal{ false };
        bool playArt{ false };
        bool playShader{ false };
    };

    // ONE beat, not two: attached hit art inherits the actor's alpha
    // (measured 2026-07-27 - the portal vanished with the hidden actor),
    // so every effect lands on the same pulse as the reveal, on a VISIBLE
    // performer. Hidden below the stage until the 3D exists and the
    // warm-up has run (band::kArrivalWarmupPulses).
    [[nodiscard]] constexpr ArrivalDecision DecideArrival(
        bool a_revealed,
        bool a_has3D,
        bool a_hasArt,
        bool a_arriveDue) noexcept {
        if (a_revealed) { return {}; }
        if (!a_has3D || !a_arriveDue) { return { .hide = true }; }
        return {
            .reveal = true,
            .playArt = a_hasArt,
            .playShader = true,
        };
    }

    [[nodiscard]] constexpr bool ResultsMayPublish(
        double a_now,
        double a_notBefore) noexcept {
        return a_now >= a_notBefore;
    }

    [[nodiscard]] constexpr ResourceDecision DecideResources(
        bool a_hasPlayer,
        bool a_hasData,
        bool a_hasBass,
        bool a_hasGuitar,
        bool a_hasDrum) noexcept {
        if (!a_hasPlayer || !a_hasData || !a_hasBass || !a_hasGuitar) {
            return ResourceDecision::kAbort;
        }
        // The drum is presentation only. A missing or overridden world model
        // must never roll back all four successfully resolved performers.
        return a_hasDrum
            ? ResourceDecision::kSpawnWithDrum
            : ResourceDecision::kSpawnWithoutDrum;
    }

    class Lifecycle {
    public:
        [[nodiscard]] BeginDecision Begin(int a_instrumentContext) noexcept {
            if (a_instrumentContext != kGuitarContext) {
                return BeginDecision::kIgnore;
            }
            if (active_) {
                return BeginDecision::kAlreadyActive;
            }
            active_ = true;
            return BeginDecision::kSpawn;
        }

        [[nodiscard]] bool End() noexcept {
            if (!active_) { return false; }
            active_ = false;
            return true;
        }

        [[nodiscard]] bool Active() const noexcept { return active_; }

    private:
        bool active_{ false };
    };
}
