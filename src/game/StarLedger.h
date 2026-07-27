// src/game/StarLedger.h
#pragma once

// Thread-safe wrapper around the pure StarLedgerCore data + the SKSE
// co-save callbacks. Readers: render thread (browser), session thread
// (record/gating). Serialization callbacks arrive on the game thread.
#include <string>

#include "game/StarLedgerCore.h"
#include "game/StarsLogic.h"

namespace SH {
    class StarLedger {
    public:
        static StarLedger& GetSingleton();
        // SKSEPluginLoad-time (after SKSE::Init): unique ID + callbacks.
        static void RegisterSerialization();

        // Per-difficulty records (Guitar-Hero convention, 2026-07-25).
        // a_difficulty is the RESOLVED difficulty a run actually plays -
        // callers get it from LoadedSong::resolvedDifficulty or
        // bard::ResolveDifficulty(mask, requested), never from the raw
        // setting.
        int         Best(const std::string& a_chartKey,
                         stars::Instrument a_inst, int a_difficulty) const;
        std::string LastPlayed() const;
        void        Record(const std::string& a_chartKey,
                           stars::Instrument a_inst, int a_difficulty,
                           int a_stars);
        // Bard teaching (spec 6.3): charts a bard unlocked outright. Key is
        // the same song-folder LEAF name Best/Record use. Monotonic - a
        // chart never un-teaches - and it rides the star record's own
        // buffer (format v3), so there is nothing extra to register here.
        bool Taught(const std::string& a_chartKey) const;
        void MarkTaught(const std::string& a_chartKey);
        bool IsNew(const std::string& a_chartKey) const;
        void MarkNew(const std::string& a_chartKey);
        void MarkSeen(const std::string& a_chartKey);

        // Re-derives the gate from current counts, merges monotonically
        // into the persisted lifted mask, returns the clamp ceiling
        // (25/45/65/85, or 100 all-lifted; 100 when bRankGate=0). The
        // merge is a WRITE to persisted state, so this belongs to the
        // game-thread enforcement points (SgtProgression::ClampPass and
        // FinishPayout) and to nothing else.
        int         GateCeiling(stars::Instrument a_inst);
        // The same number, derived the same way, WITHOUT storing the merged
        // mask - for readers, and only readers. SgtProgression::
        // EffectiveRank answers the browser, the browser draws on the
        // render thread, so routing it through GateCeiling made a persisted
        // field a side effect of drawing, at frame rate. That was safe, but
        // only because GateCeiling holds the mutex and UpdateLifted is an
        // idempotent monotonic merge, and nothing enforces either. A reader
        // that cannot write cannot lose that argument later.
        int         GateCeilingPeek(stars::Instrument a_inst) const;

        // Instrument of the most recent perform cast (browser display +
        // session attribution). Start-key sessions default to kLute.
        void              SetActiveInstrument(stars::Instrument a_inst);
        stars::Instrument ActiveInstrument() const;
    };
}
