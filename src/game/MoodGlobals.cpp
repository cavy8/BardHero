// src/game/MoodGlobals.cpp
#include "PCH.h"
#include "game/MoodGlobals.h"

#include "Settings.h"

#include "RE/T/TESDataHandler.h"
#include "RE/T/TESGlobal.h"

#include <cstring>

namespace SH::MoodGlobals {
    namespace {
        constexpr const char* kSgtPlugin    = "SkyrimsGotTalent-Bards.esp";
        constexpr const char* kTerribleEdid = "_Talent_IsPerformingTerrible";
        constexpr const char* kGoodEdid     = "_Talent_IsPerformingGood";

        // CROSS-CHECK ONLY - never the resolution path. The esp-local ids
        // the survey recorded (docs/research/2026-07-18-bard-mods-survey.md
        // :61). Resolving by editor ID instead means a merged, repacked or
        // renumbered SGT still works, and a renumber surfaces as a log line
        // rather than as a feature that silently does nothing forever.
        constexpr RE::FormID kTerribleLocalExpected = 0x0089BD;
        constexpr RE::FormID kGoodLocalExpected     = 0x0089BE;

        // written once at kDataLoaded, before the session thread exists -
        // later reads from any thread need no synchronization.
        RE::TESGlobal* g_terrible = nullptr;
        RE::TESGlobal* g_good     = nullptr;

        RE::TESGlobal* FindByEditorID(const char* a_edid) {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) { return nullptr; }
            for (auto* g : dh->GetFormArray<RE::TESGlobal>()) {
                if (!g) { continue; }
                const char* edid = g->GetFormEditorID();
                if (edid && std::strcmp(edid, a_edid) == 0) { return g; }
            }
            return nullptr;
        }

        // Log what each global actually resolved to, against what the survey
        // recorded, so an SGT renumber is visible instead of invisible. The
        // high byte of a runtime FormID is the load-order index, so the
        // esp-local id is the low 24 bits (an ESL-ified SGT would not match
        // that mask and would simply log the mismatch - which is the point:
        // resolution has already succeeded by then either way).
        void ReportId(const char* a_edid, RE::TESGlobal* a_g,
                      RE::FormID a_expectedLocal) {
            if (!a_g) { return; }
            const RE::FormID local = a_g->formID & 0x00FFFFFF;
            if (local == a_expectedLocal) {
                spdlog::info("[mood] {} = 0x{:08X}", a_edid, a_g->formID);
            } else {
                spdlog::warn("[mood] {} = 0x{:08X} (local 0x{:06X}, survey "
                             "recorded 0x{:06X}) - SGT renumbered; editor-ID "
                             "resolution still correct",
                             a_edid, a_g->formID, local, a_expectedLocal);
            }
        }

        void Set(RE::TESGlobal* a_g, float a_v) {
            if (a_g) { a_g->value = a_v; }
        }
    }

    void Install() {
        g_terrible = FindByEditorID(kTerribleEdid);
        g_good     = FindByEditorID(kGoodEdid);

        ReportId(kTerribleEdid, g_terrible, kTerribleLocalExpected);
        ReportId(kGoodEdid, g_good, kGoodLocalExpected);

        const int found = (g_terrible ? 1 : 0) + (g_good ? 1 : 0);
        if (found != 2) {
            spdlog::warn("[mood] SGT reaction globals: {}/2 resolved by "
                         "editor ID - is {} loaded? live crowd mood inert",
                         found, kSgtPlugin);
            return;
        }
        spdlog::info("[mood] SGT reaction globals: 2/2 resolved");
    }

    bool Available() { return g_terrible && g_good; }

    bool Write(crowd::Level a_level) {
        if (!Available() || !Settings::GetSingleton().liveCrowdMood) {
            return false;
        }
        // No "has the level changed" latch here, on purpose. The caller is
        // already edge-triggered (Mood::Feed returns true only on a
        // committed change), and SGT writes this same pair underneath us
        // during every performance (_Talent_PlayInstrument.psc:104-105,
        // :173-209, :259-260, :299-300) - so a cached "we already wrote
        // that" can desynchronise from the actual global inside a single
        // session and never self-heal. A Clear() missed on any exit path
        // would do the same across sessions: the next song commits the level
        // the stale cache already holds and writes nothing all song.
        // Re-asserting the pair is the correct behaviour, not a wasted write.
        //
        // SGT's own encoding, _Talent_PlayInstrument.psc:165-211. Middling
        // writes Good = 2 - SGT reusing the global rather than adding a
        // second one (its own comment says so at :189). Do NOT "tidy" that
        // to 1: of the 32 CTDA conditions in the esp that read this pair, 27
        // test Good == 2, so a 1 here would silently disqualify most of the
        // crowd's reaction lines.
        const auto reaction = crowd::ReactionFor(a_level);
        if (g_terrible->value == reaction.terrible &&
            g_good->value == reaction.good) {
            return false;
        }
        const float priorTerrible = g_terrible->value;
        const float priorGood = g_good->value;
        Set(g_terrible, reaction.terrible);
        Set(g_good, reaction.good);
        spdlog::info("[mood] crowd level -> {} (SGT globals {:.0f}/{:.0f} "
                     "-> {:.0f}/{:.0f})",
                     a_level == crowd::Level::kTerrible ? "TERRIBLE"
                     : a_level == crowd::Level::kMiddling ? "middling"
                                                          : "GREAT",
                     priorTerrible, priorGood, reaction.terrible,
                     reaction.good);
        return true;
    }

    void Clear() {
        // Same gate as Write(), and it matters more here than there: the
        // spec's promise is "off = SGT's stock behaviour", and SGT sets this
        // pair exactly ONCE per performance (~3s into OnEffectStart) and
        // never re-establishes it. A 0/0 from a disabled feature is
        // therefore not a harmless no-op - it would disqualify SGT's crowd
        // lines for the rest of a performance we are meant to be out of.
        if (!Available() || !Settings::GetSingleton().liveCrowdMood) {
            return;
        }
        const bool changed =
            g_terrible->value != 0.0f || g_good->value != 0.0f;
        Set(g_terrible, 0.0f);
        Set(g_good, 0.0f);
        if (changed) {
            spdlog::info("[mood] reaction globals cleared (0/0)");
        }
    }
}
