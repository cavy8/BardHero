// src/game/SgtProgression.cpp
#include "PCH.h"
#include "game/SgtProgression.h"

#include "Settings.h"
#include "game/StarLedger.h"
#include "game/UnlockLogic.h"  // IsLessonEdge - the pure all-three rule

#include "RE/T/TESDataHandler.h"
#include "RE/T/TESGlobal.h"

#include <atomic>
#include <cmath>

namespace SH::SgtProgression {
    namespace {
        constexpr const char* kSgtPlugin = "SkyrimsGotTalent-Bards.esp";
        // index = stars::Instrument (kLute, kFlute, kDrum)
        constexpr RE::FormID kGlobLocal[stars::kInstrumentCount] = {
            0x000D62, 0x000D61, 0x000D63
        };

        // written once at kDataLoaded, before the session thread exists -
        // later reads from any thread need no synchronization.
        RE::TESGlobal* g_glob[stars::kInstrumentCount] = {};
        bool           g_installed = false;

        // pending clamp checks (two per cast) + pending payout finish.
        // Stored by NoteCast (game thread) / NoteSessionPayout (session
        // thread), consumed by Tick (session thread) - atomics only.
        std::atomic<double> g_clampDue[2] = { -1.0, -1.0 };
        std::atomic<int>    g_clampInst{ 0 };
        std::atomic<double> g_finishDue{ -1.0 };
        std::atomic<int>    g_finishInst{ 0 };
        std::atomic<int>    g_finishBonus{ 0 };

        // promotion bracket state (game thread only)
        int g_actualBefore = -1;
        int g_promoted     = -1;

        // settings-page dev tooling (2026-07-20): last game-thread GLOB
        // sample per instrument (render thread reads), plus the clamp
        // suspension latch a hand-set expertise arms.
        std::atomic<int>  g_uiSample[stars::kInstrumentCount] = { -1, -1,
                                                                  -1 };
        std::atomic<bool> g_clampSuspended{ false };

        // Bard teaching (spec 6.3): last sampled GLOB per instrument, -1
        // for "not sampled". Game thread only - PollTeachingEdge and the
        // two load hooks (also game thread) are the sole touchers.
        int g_lastSeen[stars::kInstrumentCount] = { -1, -1, -1 };
        // 1, not 5: the Orc bard (Bard_FragmentOrc.psc) charges the same
        // 100 gold for +1 to all three, and 5 rejected it outright. The
        // all-three rule is the discriminator; this is only a noise floor,
        // and nothing of ours raises all three at all.
        constexpr int kTeachingStep = 1;

        int ReadGlob(stars::Instrument a_inst) {
            auto* g = g_glob[static_cast<int>(a_inst)];
            if (!g || !std::isfinite(g->value)) { return -1; }
            return static_cast<int>(std::lround(g->value));
        }
        void WriteGlob(stars::Instrument a_inst, int a_v) {
            if (auto* g = g_glob[static_cast<int>(a_inst)]) {
                g->value = static_cast<float>(a_v);
            }
        }
        void SampleTeachingBaseline() {  // game thread only
            for (int i = 0; i < stars::kInstrumentCount; ++i) {
                g_lastSeen[i] = ReadGlob(static_cast<stars::Instrument>(i));
            }
        }
        void SampleGlobs() {  // game thread only
            for (int i = 0; i < stars::kInstrumentCount; ++i) {
                g_uiSample[i].store(
                    ReadGlob(static_cast<stars::Instrument>(i)));
            }
        }
    }

    void Install() {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) { return; }
        int found = 0;
        for (int i = 0; i < stars::kInstrumentCount; ++i) {
            g_glob[i] =
                dh->LookupForm<RE::TESGlobal>(kGlobLocal[i], kSgtPlugin);
            if (g_glob[i]) { ++found; }
        }
        g_installed = found == stars::kInstrumentCount;
        spdlog::info("[progress] SGT expertise globals: {}/{} resolved{}",
                     found, stars::kInstrumentCount,
                     g_installed ? "" : " - progression inert");
    }

    bool Available() { return g_installed; }

    void ClampPass(stars::Instrument a_inst, const char* a_reason) {
        if (!g_installed) { return; }
        // dev latch: a hand-set expertise must survive every enforcement
        // point until the user re-arms the clamp (settings page)
        if (g_clampSuspended.load()) {
            spdlog::info(
                "[progress] clamp ({}) skipped - suspended by the dev "
                "tier setter", a_reason);
            return;
        }
        // clamping the promoted value mid-window would both defeat the tier
        // promotion and corrupt the restore baseline; the skipped clamp
        // self-heals at the next enforcement point.
        if (g_actualBefore >= 0) {
            spdlog::info(
                "[progress] clamp ({}) skipped - promotion bracket armed",
                a_reason);
            return;
        }
        const int cur = ReadGlob(a_inst);
        if (cur < 0) { return; }
        const int ceiling = StarLedger::GetSingleton().GateCeiling(a_inst);
        if (cur > ceiling) {
            WriteGlob(a_inst, ceiling);
            spdlog::info(
                "[progress] clamp ({}): inst={} {} -> {} (gate ceiling)",
                a_reason, static_cast<int>(a_inst), cur, ceiling);
        }
    }

    void BeginPromotion(stars::Instrument a_inst, int a_floor) {
        g_actualBefore = -1;
        g_promoted     = -1;
        if (!g_installed) { return; }
        const int cur = ReadGlob(a_inst);
        if (cur < 0) { return; }
        g_actualBefore = cur;
        g_promoted     = cur > a_floor ? cur : a_floor;
        if (g_promoted != cur) {
            WriteGlob(a_inst, g_promoted);
            spdlog::info(
                "[progress] tier promotion: inst={} {} -> {} for the payout",
                static_cast<int>(a_inst), cur, g_promoted);
        }
    }

    void FinishPayout(stars::Instrument a_inst, int a_feedBonus) {
        if (!g_installed || g_actualBefore < 0) { return; }
        const int cur  = ReadGlob(a_inst);
        int       next = stars::RestoredValue(g_actualBefore, g_promoted,
                                              cur < 0 ? g_promoted : cur);
        next += a_feedBonus;
        // the payout cap honors the dev suspension too - else one session
        // would pull a hand-set tier straight back to the gate ceiling
        const int ceiling =
            g_clampSuspended.load()
                ? 100
                : StarLedger::GetSingleton().GateCeiling(a_inst);
        const int cap     = ceiling < 100 ? ceiling : 100;
        if (next > cap) { next = cap; }
        if (next < 0) { next = 0; }
        WriteGlob(a_inst, next);
        spdlog::info(
            "[progress] payout finish: inst={} actual={} promoted={} cur={} "
            "feed=+{} -> {} (cap {})",
            static_cast<int>(a_inst), g_actualBefore, g_promoted, cur,
            a_feedBonus, next, cap);
        g_actualBefore = -1;
        g_promoted     = -1;
    }

    void NoteCast(stars::Instrument a_inst, double a_nowQpc) {
        g_clampInst.store(static_cast<int>(a_inst));
        g_clampDue[0].store(a_nowQpc + 45.0);
        g_clampDue[1].store(a_nowQpc + 80.0);
    }

    void NoteSessionPayout(stars::Instrument a_inst, int a_feedBonus,
                           double a_deadlineQpc) {
        g_finishInst.store(static_cast<int>(a_inst));
        g_finishBonus.store(a_feedBonus);
        g_finishDue.store(a_deadlineQpc);
    }

    void Tick(double a_nowQpc) {
        if (!g_installed) { return; }
        for (auto& due : g_clampDue) {
            const double d = due.load();
            if (d > 0.0 && a_nowQpc >= d) {
                due.store(-1.0);
                const auto inst =
                    static_cast<stars::Instrument>(g_clampInst.load());
                SKSE::GetTaskInterface()->AddTask(
                    [inst] { ClampPass(inst, "post-cast"); });
            }
        }
        const double f = g_finishDue.load();
        if (f > 0.0 && a_nowQpc >= f) {
            g_finishDue.store(-1.0);
            const auto inst =
                static_cast<stars::Instrument>(g_finishInst.load());
            const int bonus = g_finishBonus.load();
            SKSE::GetTaskInterface()->AddTask(
                [inst, bonus] { FinishPayout(inst, bonus); });
        }
    }

    void OnPostLoadGame() {
        if (!g_installed) { return; }
        // Synchronous disarm FIRST (messaging callbacks run on the game
        // thread, so touching the bracket here is legal): a quickload
        // inside the finish window must not let a stale FinishPayout
        // restore another save's GLOB - the reset wins against any queued
        // task, whose g_actualBefore<0 guard then no-ops it. Accepted
        // residual: a save written inside the promotion window bakes the
        // promoted value; post-load ClampPass pulls it back only to the
        // gate ceiling.
        g_actualBefore = -1;
        g_promoted     = -1;
        g_finishDue.store(-1.0);
        g_clampDue[0].store(-1.0);
        g_clampDue[1].store(-1.0);
        // Re-baseline teaching against THIS save's values rather than
        // clearing to the sentinel: a load that takes 5/5/5 to 60/60/60 has
        // to read as no change, never as a +55 lesson. kPostLoadGame is the
        // game thread, so reading the globals here is legal. OnPreLoadGame
        // covers the gap before this point.
        SampleTeachingBaseline();
        SKSE::GetTaskInterface()->AddTask([] {
            for (int i = 0; i < stars::kInstrumentCount; ++i) {
                ClampPass(static_cast<stars::Instrument>(i), "post-load");
            }
        });
    }

    void OnPreLoadGame() {
        // Invalidate BEFORE the save's globals are swapped in. A poll that
        // lands between the swap and kPostLoadGame would otherwise diff the
        // abandoned timeline's values against the loaded save's; at
        // kTeachingStep 1 that is ANY save with all three higher than the
        // one being left. kPreLoadGame is the game thread, which owns this.
        for (auto& v : g_lastSeen) { v = -1; }
    }

    int LiveExpertise(stars::Instrument a_inst) {
        return g_installed ? ReadGlob(a_inst) : -1;
    }

    int RankFromExpertise(int a_expertise) {
        if (a_expertise < 26) { return 1; }
        if (a_expertise < 46) { return 2; }
        if (a_expertise < 66) { return 3; }
        if (a_expertise < 86) { return 4; }
        return 5;
    }

    int EffectiveRank(stars::Instrument a_inst, int a_expertise) {
        // Peek, NOT GateCeiling: the browser asks this every frame from the
        // render thread, and the merging call writes the persisted lifted
        // mask. Same ceiling either way - see StarLedger.h.
        const int ceiling =
            g_clampSuspended.load()
                ? 100
                : StarLedger::GetSingleton().GateCeilingPeek(a_inst);
        return RankFromExpertise(a_expertise < ceiling ? a_expertise
                                                       : ceiling);
    }

    int PollTeachingEdge(bool a_sessionActive) {
        if (!g_installed) { return -1; }
        int prev[stars::kInstrumentCount], now[stars::kInstrumentCount];
        for (int i = 0; i < stars::kInstrumentCount; ++i) {
            prev[i] = g_lastSeen[i];
            now[i]  = ReadGlob(static_cast<stars::Instrument>(i));
        }
        // Re-baseline unconditionally and BEFORE the bail-out: a window
        // that kept its old baseline would hand the next poll an
        // accumulated delta.
        for (int i = 0; i < stars::kInstrumentCount; ++i) {
            g_lastSeen[i] = now[i];
        }
        const bool lesson = unlock::IsLessonEdge(
            prev, now, stars::kInstrumentCount, kTeachingStep);
        if (!lesson) { return -1; }
        if (a_sessionActive) {
            // Not silent. A lesson takes a dialogue and the player cannot
            // reach one mid-performance, so this should never print; if it
            // ever does, a paid lesson was dropped and that has to be
            // diagnosable from the log alone.
            spdlog::warn(
                "[unlock] lesson-shaped edge while a session was live - "
                "dropped ({}->{}, {}->{}, {}->{})",
                prev[0], now[0], prev[1], now[1], prev[2], now[2]);
            return -1;
        }
        // The instrument index is structurally 0 for every real lesson (all
        // three rose), so the transitions go in the message instead.
        spdlog::info("[unlock] bard lesson detected: lute {}->{}, "
                     "flute {}->{}, drum {}->{}",
                     prev[0], now[0], prev[1], now[1], prev[2], now[2]);
        return 0;
    }

    // ---- settings-page dev tooling (2026-07-20) ----------------------------

    void PostUiSample() {
        if (!g_installed) { return; }
        SKSE::GetTaskInterface()->AddTask([] { SampleGlobs(); });
    }

    int UiSampled(stars::Instrument a_inst) {
        return g_uiSample[static_cast<int>(a_inst)].load();
    }

    void CheatSetExpertise(stars::Instrument a_inst, int a_value) {
        if (!g_installed) { return; }
        g_clampSuspended.store(true);
        SKSE::GetTaskInterface()->AddTask([a_inst, a_value] {
            WriteGlob(a_inst, a_value);
            SampleGlobs();
            spdlog::info(
                "[progress] CHEAT: expertise inst={} set to {} (clamp "
                "suspended until re-armed)",
                static_cast<int>(a_inst), a_value);
        });
    }

    bool ClampSuspended() { return g_clampSuspended.load(); }

    void SetClampSuspended(bool a_suspend) {
        g_clampSuspended.store(a_suspend);
        if (!a_suspend) {
            SKSE::GetTaskInterface()->AddTask([] {
                for (int i = 0; i < stars::kInstrumentCount; ++i) {
                    ClampPass(static_cast<stars::Instrument>(i),
                              "clamp re-armed");
                }
                SampleGlobs();
            });
        }
    }
}
