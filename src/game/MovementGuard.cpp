// src/game/MovementGuard.cpp
#include "PCH.h"
#include "game/MovementGuard.h"

#include "Settings.h"
#include "game/InputHook.h"

#include "RE/C/ControlMap.h"  // umbrella covers it; explicit is safe

#include <cstdint>
#include <excpt.h>  // EXCEPTION_EXECUTE_HANDLER for the SEH-guarded bind read
#include <string_view>

namespace SH {
    namespace {
        // game thread only (every touch is inside an SKSE task)
        bool g_blocked = false;

        // Reading the binding table walks ControlMap::controlMap[] at +0x60,
        // which is the ONE part of this object that is demonstrably right on
        // AE 1.6.1170 - every field run reads back fwd=0x11 back=0x1f
        // left=0x1e right=0x20 jump=0x39, i.e. WASD and Space. The members
        // PAST that array are the broken ones (see MovementGuard.h). The SEH
        // wrapper stays anyway: a diagnostic must never be the thing that
        // takes the game down.
        struct Binds {
            std::uint32_t fwd{}, back{}, left{}, right{}, jump{}, sneak{},
                pov{}, activate{};
            bool ok{ false };
        };

        Binds ReadBinds(RE::ControlMap* a_cm) noexcept {
            Binds b{};
            if (!a_cm) { return b; }
            // constructed OUTSIDE __try: trivially destructible, so the block
            // needs no object unwinding
            const std::string_view kFwd{ "Forward" };
            const std::string_view kBack{ "Back" };
            const std::string_view kLeft{ "Strafe Left" };
            const std::string_view kRight{ "Strafe Right" };
            const std::string_view kJump{ "Jump" };
            const std::string_view kSneak{ "Sneak" };
            const std::string_view kPov{ "Toggle POV" };
            const std::string_view kAct{ "Activate" };
            constexpr auto kKb = RE::INPUT_DEVICE::kKeyboard;
            __try {
                b.fwd      = a_cm->GetMappedKey(kFwd, kKb);
                b.back     = a_cm->GetMappedKey(kBack, kKb);
                b.left     = a_cm->GetMappedKey(kLeft, kKb);
                b.right    = a_cm->GetMappedKey(kRight, kKb);
                b.jump     = a_cm->GetMappedKey(kJump, kKb);
                b.sneak    = a_cm->GetMappedKey(kSneak, kKb);
                b.pov      = a_cm->GetMappedKey(kPov, kKb);
                b.activate = a_cm->GetMappedKey(kAct, kKb);
                b.ok       = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                b.ok = false;
            }
            return b;
        }

        void Apply(bool blocked) {
            if (blocked == g_blocked) { return; }  // idempotent
            MovementGuard::LogBinds(blocked ? "pre-block" : "pre-restore");
            if (blocked) {
                auto* cm = RE::ControlMap::GetSingleton();
                const auto b = ReadBinds(cm);
                if (!b.ok) {
                    // No binds, no block. Failing open costs the player a
                    // stray step; failing closed used to cost them the
                    // whole input stack.
                    spdlog::warn(
                        "[moveguard] binding table unreadable - movement "
                        "NOT blocked this session");
                    return;
                }
                const std::uint32_t diks[] = { b.fwd,   b.back,  b.left,
                                               b.right, b.jump,  b.sneak,
                                               b.pov };
                InputHook::SetMovementSwallow(diks, 7);
                spdlog::info(
                    "[moveguard] blocked (swallowing fwd={:#04x} back={:#04x} "
                    "left={:#04x} right={:#04x} jump={:#04x} sneak={:#04x} "
                    "pov={:#04x})",
                    b.fwd, b.back, b.left, b.right, b.jump, b.sneak, b.pov);
            } else {
                InputHook::SetMovementSwallow(nullptr, 0);
                spdlog::info("[moveguard] restored");
            }
            g_blocked = blocked;
            MovementGuard::LogBinds(blocked ? "post-block" : "post-restore");
        }
    }

    void MovementGuard::Post(bool blocked) {
        if (!Settings::GetSingleton().blockMovement) { return; }
        SKSE::GetTaskInterface()->AddTask([blocked] { Apply(blocked); });
    }

    void MovementGuard::LogBinds(const char* a_when) {
        auto* cm = RE::ControlMap::GetSingleton();
        auto* pc = RE::PlayerCharacter::GetSingleton();
        // in-world only; never touch this table during startup or a load
        if (!cm || !pc || !pc->Get3D()) { return; }
        const auto b = ReadBinds(cm);
        if (!b.ok) {
            spdlog::info("[bind] {}: FAULTED (binding table unreadable)",
                         a_when);
            return;
        }
        // 0xFF is UNBOUND. Walking dies the instant fwd/back/left/right read
        // 0xFF while jump stays bound - that is the whole bug, if it is this.
        spdlog::info(
            "[bind] {}: fwd={:#04x} back={:#04x} left={:#04x} right={:#04x} "
            "| jump={:#04x} activate={:#04x}{}",
            a_when, b.fwd, b.back, b.left, b.right, b.jump, b.activate,
            (b.fwd == 0xFF || b.back == 0xFF || b.left == 0xFF ||
             b.right == 0xFF)
                ? "  <<< MOVEMENT UNBOUND"
                : "");
    }
}
