// src/game/MoodGlobals.h
#pragma once

// Game-thread writer for SGT's two audience-reaction GLOBs
// (_Talent_IsPerformingTerrible / _Talent_IsPerformingGood in
// SkyrimsGotTalent-Bards.esp). SGT sets that pair ONCE from the expertise
// grind stat about three seconds in, before the player has played a note,
// and the pair then gates which audience scene runs and which of its
// dialogue reactions qualify. This unit hands the pair to how the player is
// actually playing instead.
//
// OWNERSHIP, non-negotiable: this unit writes GlobalVariables and nothing
// else. No ControlMap, no PlayerControls, no quest stages, no scenes. The
// component next door reached past its own job into refcounted engine state
// to "restore" controls and killed all player input while every flag still
// read like free roam; that cost a full day on 2026-07-21.
#include "game/CrowdMoodLogic.h"

namespace SH::MoodGlobals {
    // kDataLoaded, game thread. Resolves SGT's two reaction globals by
    // EDITOR ID, not by a hardcoded FormID: it survives SGT being merged,
    // repacked or renumbered, and it cannot be defeated by a wrong id
    // guessed from an adjacent record. Soft: absent SGT logs once and every
    // later call no-ops.
    void Install();
    bool Available();

    // Game thread. Reconciles the pair with the live Glory zone. Returns
    // true only when the actual globals differed and were corrected. The
    // caller uses that edge to release any incompatible celebration idle.
    bool Write(crowd::Level a_level);

    // Game thread. Restores 0/0, the same reset SGT's own OnEffectFinish
    // does between performances (_Talent_PlayInstrument.psc:258-260 and
    // :299-300). Safe on every exit path, idempotent.
    void Clear();
}
