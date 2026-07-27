// src/game/PerformTriggerHook.h
#pragma once

namespace SH {

    // The SGT perform trigger of record (spec
    // docs/specs/2026-07-20-native-perform-start.md, Phase 1).
    //
    // MagicTarget::AddTarget is vfunc 0x01 on the MagicTarget sub-object
    // PlayerCharacter inherits through Actor. It fires SYNCHRONOUSLY when a
    // magic effect is added, before Papyrus schedules the effect's
    // OnEffectStart - field-measured 165-355ms ahead of the 500ms HasSpell
    // poll, which is the latency Phase 2's native start suppression needs.
    // Being a vfunc, it is NOT exposed to the AE inlined-hook-sites trap.
    //
    // The poll remains installed as a belt; a shared arming latch
    // (PerformTriggerLogic.h) guarantees only one of them requests a start.
    // If this hook fails to install, the poll simply keeps triggering as it
    // always did, 165-355ms later - degraded, not broken.
    namespace PerformTriggerHook {
        // kDataLoaded, AFTER Session::Install (the filter reads the
        // perform-spell IDs it resolves). Finds the MagicTarget sub-vtable
        // at install time by matching the live vptr - no hardcoded index.
        void Install();
    }
}
