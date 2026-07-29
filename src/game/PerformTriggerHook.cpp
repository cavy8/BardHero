// src/game/PerformTriggerHook.cpp
#include "PCH.h"

#include "PerformTriggerHook.h"

#include "Settings.h"
#include "game/PerformTriggerLogic.h"
#include "game/Session.h"

#include "RE/M/MagicItem.h"
#include "RE/M/MagicTarget.h"
#include "RE/P/PlayerCharacter.h"

namespace SH {
    namespace {
        struct AddTargetHook {
            static bool thunk(RE::MagicTarget*                a_this,
                              RE::MagicTarget::AddTargetData& a_data) {
                const bool ret = func(a_this, a_data);
                auto*      pc  = RE::PlayerCharacter::GetSingleton();
                if (pc && a_this == pc->AsMagicTarget() && a_data.magicItem) {
                    const auto id = a_data.magicItem->GetFormID();
                    if (Session::IsPerformSpell(id)) {
                        // The player asked a follower to play together. SGT
                        // builds that duet inside the OnEffectStart we are
                        // about to prevent, so BardHero takes none of this
                        // equip: no arming, no strip, no songbook, no
                        // sheathe. SGT runs the whole performance.
                        if (performtrigger::DecideTrigger(
                                true, Settings::GetSingleton().duetPassthrough,
                                Session::DuetPending()) ==
                            performtrigger::TriggerAction::kStandDownForDuet) {
                            Session::NoteDuetPassthrough(id);
                            spdlog::info(
                                "[sgt] follower duet pending - standing down "
                                "so SGT plays it (bDuetPassthrough)");
                            return ret;
                        }
                        // TRIGGER OF RECORD (Phase 1). The 500ms poll stays
                        // as a belt; the shared Arming latch means only one
                        // of the two fires per arming.
                        Session::NotePerformAbilityAdded(id);
                        // NATIVE START (Phase 2). Stripping HERE - before
                        // Papyrus ever schedules OnEffectStart - stops SGT's
                        // start flow running at all: no DisablePlayerControls
                        // (psc 114/117), no SheatheWeapon + Wait(2) (133-136),
                        // no ForceThirdPerson (151-154), no PlayIdle (163).
                        // Field-proven 2026-07-20: four equips, zero SGT
                        // side effects. Givespell has no re-add loop (spec
                        // 5.3) so the strip holds.
                        //
                        // Posted as a task, not called inline: RemoveSpell
                        // re-entrantly inside AddTarget would mutate the
                        // effect list we are currently inside. Still the
                        // same frame, still far ahead of the poll.
                        //
                        // NOTE the settle-then-strip dance is deliberately
                        // NOT used here. It exists only because we used to
                        // strip an effect whose OnEffectStart thread was
                        // already in flight; nothing that never started has
                        // an in-flight thread to leave ghost idle or music
                        // behind (memory papyrus-dispel-inflight-threads).
                        if (Settings::GetSingleton().sgtNativeStart &&
                            !Session::IsSelfAddInFlight()) {
                            SKSE::GetTaskInterface()->AddTask([id] {
                                auto* p = RE::PlayerCharacter::GetSingleton();
                                auto* sp =
                                    RE::TESForm::LookupByID<RE::SpellItem>(id);
                                if (p && sp && p->HasSpell(sp)) {
                                    p->RemoveSpell(sp);
                                    spdlog::info(
                                        "[sgt] native start: ability 0x{:X} "
                                        "stripped before OnEffectStart - "
                                        "SGT's start flow never runs",
                                        id);
                                }
                            });
                        }
                    }
                }
                return ret;
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        // Which VTABLE_PlayerCharacter entry is the MagicTarget sub-object?
        // A hardcoded index was rejected: the array has 17 entries and three
        // carry zero SE/AE IDs, so a static index drifts SILENTLY into an
        // unrelated vtable. Match the LIVE vptr instead - AsMagicTarget()
        // already resolves the 0x98 (SE) / 0xA0 (AE 1.6.629+) split, and the
        // first 8 bytes of the sub-object are its vtable pointer. Returns -1
        // when nothing matches, so the failure is loud.
        int FindMagicTargetVtable(RE::PlayerCharacter* a_pc) {
            const auto live =
                *reinterpret_cast<std::uintptr_t*>(a_pc->AsMagicTarget());
            for (std::size_t i = 0; i < RE::PlayerCharacter::VTABLE.size();
                 ++i) {
                const REL::Relocation<std::uintptr_t> v{
                    RE::PlayerCharacter::VTABLE[i]
                };
                if (v.address() == live) { return static_cast<int>(i); }
            }
            return -1;
        }
    }

    void PerformTriggerHook::Install() {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) {
            spdlog::error(
                "[sgt] trigger hook: no PlayerCharacter at install time - "
                "move Install() later (kPostLoadGame or first frame). The "
                "500ms poll still triggers, just 165-355ms later.");
            return;
        }
        const int idx = FindMagicTargetVtable(pc);
        if (idx < 0) {
            spdlog::error(
                "[sgt] trigger hook: MagicTarget sub-vtable not found among "
                "{} VTABLE_PlayerCharacter entries - NOT hooking, falling "
                "back to the poll",
                RE::PlayerCharacter::VTABLE.size());
            return;
        }
        // not const: write_vfunc is a non-const member
        REL::Relocation<std::uintptr_t> vtbl{
            RE::PlayerCharacter::VTABLE[idx]
        };
        AddTargetHook::func = vtbl.write_vfunc(0x01, AddTargetHook::thunk);
        spdlog::info(
            "[sgt] trigger hook installed (AddTarget, VTABLE idx {}, "
            "slot 0x01) - synchronous perform trigger",
            idx);
    }
}
