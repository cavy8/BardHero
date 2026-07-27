#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace bard::band {
    enum class Role {
        kBassist,
        kRhythmGuitarist,
        kDrummer,
        kSinger,
    };

    struct PerformerSpec {
        Role role;
        std::uint32_t actorLocalId;
        float right;
        float forward;
        std::string_view label;
    };

    inline constexpr std::array<PerformerSpec, 4> kFormation{ {
        { Role::kBassist, 0x806, -105.0f, 0.0f, "bassist" },
        { Role::kSinger, 0x809, -55.0f, 0.0f, "singer" },
        { Role::kRhythmGuitarist, 0x807, 55.0f, 0.0f,
          "rhythm guitarist" },
        { Role::kDrummer, 0x808, 105.0f, 0.0f, "drummer" },
    } };

    // ---- WHO CARRIES THE CONJURATION ART (AND SO THE SOUND) -------------
    //
    // The sample is baked into SummonTargetFX's NIF - no form field, no
    // volume lever - so the count of art objects IS the count of sounds.
    //
    // true  - every performer gets the burst; all four look identical and
    //         the sound stacks four-deep (~+12 dB over one, measured).
    //         Owner's call, 2026-07-27: "they should all have it".
    // false - only kArrivalArtBearer (the singer) gets it: one sound, one
    //         visibly richer arrival. The whole retreat if four proves too
    //         loud.
    //
    // BandLifecycleTests asserts the bearer count against kFormation:
    // a bearer role nobody holds is a silent arrival that ships looking
    // fine.
    inline constexpr Role kArrivalArtBearer = Role::kSinger;
    inline constexpr bool kArrivalArtOnEveryPerformer = true;

    [[nodiscard]] inline constexpr bool BearsArrivalArt(
        Role a_role) noexcept {
        return kArrivalArtOnEveryPerformer || a_role == kArrivalArtBearer;
    }
}
