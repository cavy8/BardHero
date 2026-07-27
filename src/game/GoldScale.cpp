// src/game/GoldScale.cpp
#include "PCH.h"
#include "game/GoldScale.h"

#include "QpcClock.h"
#include "Settings.h"
#include "game/EngineFeed.h"
#include "game/GoldScaleMath.h"
#include "game/PayoutMath.h"

#include "RE/S/ScriptEventSourceHolder.h"
#include "RE/T/TESContainerChangedEvent.h"

#include <algorithm>
#include <atomic>

namespace SH {
    namespace {
        constexpr RE::FormID kGoldForm   = 0x0000000F;
        constexpr RE::FormID kPlayerForm = 0x00000014;
        // SGT's payout = RegisterForSingleUpdate(32.8) after the cast;
        // Papyrus latency smears it, so accept this window.
        constexpr double kWindowMin = 25.0, kWindowMax = 75.0;
        constexpr int    kMaxGrant  = 25;  // Pro tier ceiling RandomInt(4,25)

        std::atomic<double> g_lastCast{ -1.0e9 };
        std::atomic<int>    g_captured{ 0 };

        // whole-song deferred payout (plan 2026-07-19)
        std::atomic<bool>   g_wholeLive{ false };
        std::atomic<bool>   g_defArmed{ false };
        std::atomic<double> g_endUntil{ -1.0 };  // QPC close of end window
        std::atomic<int> g_defHit{ 0 }, g_defTotal{ 0 },
            g_defDiff{ 3 };  // inert; ArmDeferred always sets
        // performance payout inputs (spec 5.6), same lifetime as the above
        std::atomic<int>  g_defStars{ 0 };
        std::atomic<int>  g_defMood{ 0 };
        std::atomic<int>  g_defRank{ 1 };
        std::atomic<bool>   g_defAtInn{ false };
        std::atomic<double> g_defSongSec{ 0.0 };

        goldscale::Params MakeParams() {
            const auto&      st = Settings::GetSingleton();
            goldscale::Params p;
            p.accMin      = st.goldAccMin;
            p.accMax      = st.goldAccMax;
            p.diffMult[0] = st.goldMultEasy;
            p.diffMult[1] = st.goldMultMedium;
            p.diffMult[2] = st.goldMultHard;
            p.diffMult[3] = st.goldMultExpert;
            return p;
        }

        class Sink : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
        public:
            static Sink* GetSingleton() {
                static Sink s;
                return &s;
            }
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESContainerChangedEvent* ev,
                RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
                if (ev && ev->baseObj == kGoldForm &&
                    ev->newContainer == kPlayerForm && ev->itemCount >= 1 &&
                    ev->itemCount <= kMaxGrant) {
                    const double now = QpcSec();
                    if (now <= g_endUntil.load()) {
                        // whole-song end window: SGT's deferred payout (and
                        // any audience tip in the applause) - capture
                        g_captured.fetch_add(ev->itemCount);
                        spdlog::info(
                            "[goldscale] end-capture: {} gold", ev->itemCount);
                    } else if (EngineFeed::GetSingleton().active.load() &&
                               !g_wholeLive.load()) {
                        const double dt = now - g_lastCast.load();
                        if (dt >= kWindowMin && dt <= kWindowMax) {
                            g_captured.fetch_add(ev->itemCount);
                            spdlog::info(
                                "[goldscale] captured {} gold ({:.1f}s after "
                                "perform cast)",
                                ev->itemCount, dt);
                        }
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        int PlayerGoldCount() {  // game thread; form 0xF directly, never
                                 // GetGoldAmount (pinned-NG CTD)
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!pc) { return 0; }
            auto counts = pc->GetInventoryCounts([](RE::TESBoundObject& o) {
                return o.GetFormID() == kGoldForm;
            });
            int n = 0;
            for (const auto& entry : counts) {
                n += std::max(0, entry.second);
            }
            return n;
        }

        void ApplyDelta(int delta) {  // game thread (SKSE task)
            auto* pc = RE::PlayerCharacter::GetSingleton();
            auto* gold =
                RE::TESForm::LookupByID<RE::TESBoundObject>(kGoldForm);
            if (!pc || !gold || delta == 0) { return; }
            if (delta > 0) {
                pc->AddObjectToContainer(gold, nullptr, delta, nullptr);
                RE::DebugNotification("The crowd loved it! Bonus gold!");
            } else {
                const int take = std::min(-delta, PlayerGoldCount());
                if (take > 0) {
                    pc->RemoveItem(gold, take,
                                   RE::ITEM_REMOVE_REASON::kRemove, nullptr,
                                   nullptr);
                    RE::DebugNotification(
                        "The crowd wants some coin back...");
                }
            }
        }
    }

    void GoldScale::Install() {
        RE::ScriptEventSourceHolder::GetSingleton()
            ->AddEventSink<RE::TESContainerChangedEvent>(Sink::GetSingleton());
        spdlog::info("[goldscale] installed");
    }

    void GoldScale::NotePerformCast() { g_lastCast.store(QpcSec()); }

    void GoldScale::OnSessionStart() {
        if (g_defArmed.load()) {
            if (g_endUntil.load() > 0.0) {
                // a new session starts while the previous end window is
                // still open - close it now so its capture cannot bleed
                spdlog::info(
                    "[goldscale] flushing previous end-window at new "
                    "session start");
                TickDeferred(g_endUntil.load() + 1.0);
            } else {
                // armed but OpenEndCapture never ran (task still queued) -
                // drop it so old stats cannot fire into this session
                CancelDeferred();
                spdlog::info(
                    "[goldscale] deferred payout dropped - window never "
                    "opened before next session");
            }
        }
        g_wholeLive.store(false);  // insurance: keeper re-asserts while live
        g_captured.store(0);
    }

    void GoldScale::OnSessionEnd(bool completed, int notesHit,
                                 int notesTotal, int difficulty) {
        const int captured = g_captured.exchange(0);
        if (!completed || captured <= 0) { return; }
        const int delta = goldscale::Delta(captured, notesHit, notesTotal,
                                           difficulty, MakeParams());
        spdlog::info(
            "[goldscale] acc={}/{} diff={} captured={} -> delta={:+}",
            notesHit, notesTotal, difficulty, captured, delta);
        if (delta != 0) {
            SKSE::GetTaskInterface()->AddTask([delta] { ApplyDelta(delta); });
        }
    }

    void GoldScale::SetWholeSongLive(bool live) { g_wholeLive.store(live); }

    void GoldScale::ArmDeferred(int notesHit, int notesTotal, int difficulty,
                                int stars, int moodLevel, int rank,
                                bool atInn, double songSec) {
        g_defHit.store(notesHit);
        g_defTotal.store(notesTotal);
        g_defDiff.store(difficulty);
        g_defStars.store(stars);
        g_defMood.store(moodLevel);
        g_defRank.store(rank);
        g_defAtInn.store(atInn);
        g_defSongSec.store(songSec);
        g_defArmed.store(true);
    }

    void GoldScale::CancelDeferred() {
        g_defArmed.store(false);
        g_endUntil.store(-1.0);
    }

    void GoldScale::OpenEndCapture() {
        // arm was flushed/cancelled - never open a stale window
        if (!g_defArmed.load()) { return; }
        const double sec = Settings::GetSingleton().sgtEndCaptureSec;
        g_endUntil.store(QpcSec() + sec);
        spdlog::info("[goldscale] end-capture window open ({:.0f}s)", sec);
    }

    void GoldScale::TickDeferred(double nowQpc) {
        if (!g_defArmed.load()) { return; }
        const double until = g_endUntil.load();
        if (until < 0.0 || nowQpc <= until) { return; }  // not open / still open
        g_defArmed.store(false);
        g_endUntil.store(-1.0);
        const int captured = g_captured.exchange(0);
        int       delta    = 0;
        if (captured > 0) {
            delta = goldscale::Delta(captured, g_defHit.load(),
                                     g_defTotal.load(), g_defDiff.load(),
                                     MakeParams());
            spdlog::info(
                "[goldscale] end-capture acc={}/{} diff={} captured={} -> "
                "delta={:+}",
                g_defHit.load(), g_defTotal.load(), g_defDiff.load(),
                captured, delta);
        } else {
            spdlog::info(
                "[goldscale] end-capture: nothing captured (not an inn, or "
                "expertise below Advanced - SGT pays 0 there)");
        }

        // Performance payout (spec 5.6): pay the SHORTFALL against what the
        // run was worth. Deliberately NOT inside the captured > 0 branch -
        // the nothing-captured case above is exactly the one this feature
        // exists for, and the old early return meant a brilliant rank-1 run
        // fell straight off the end of this function with nothing.
        int topUp = 0;
        if (Settings::GetSingleton().performancePayout) {
            const auto&    st = Settings::GetSingleton();
            payout::Params pp;
            pp.buskBase        = st.buskBase;
            pp.buskOutside     = st.buskOutside;
            pp.renownAtRank1   = st.renownAtRank1;
            pp.moodPayTerrible = st.moodPayTerrible;
            pp.cap             = st.payoutCap;
            pp.minStars        = st.payoutMinStars;
            pp.lengthRefSec    = st.payoutLengthRefSec;
            pp.lengthMin       = st.payoutLengthMin;
            pp.lengthMax       = st.payoutLengthMax;
            const int deserved =
                payout::Deserved(g_defStars.load(), g_defMood.load(),
                                 g_defDiff.load(), g_defRank.load(),
                                 g_defAtInn.load(), g_defSongSec.load(), pp);
            topUp = payout::TopUp(deserved, captured + delta);
            // diff belongs HERE and not only in the [goldscale] line above:
            // that line prints only when captured > 0, which is never on
            // this feature's own main path, and diffMult spans 0.5 to 1.5 -
            // a 3x swing in the purse. Without it a run's gold cannot be
            // reconstructed from the log.
            spdlog::info(
                "[payout] stars={} mood={} diff={} rank={} inn={} "
                "deserved={} observed={} -> topUp={}",
                g_defStars.load(), g_defMood.load(), g_defDiff.load(),
                g_defRank.load(), g_defAtInn.load(), deserved,
                captured + delta, topUp);
        } else {
            // Silence here would be indistinguishable from TickDeferred
            // never having fired at all, and a field gate should not cost a
            // run to that ambiguity.
            spdlog::info(
                "[payout] performance payout disabled "
                "([Gold] bPerformancePayout = 0)");
        }

        const int total = delta + topUp;
        if (total != 0) {
            SKSE::GetTaskInterface()->AddTask([total] { ApplyDelta(total); });
        }
    }
}
