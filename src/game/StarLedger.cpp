// src/game/StarLedger.cpp
#include "PCH.h"
#include "game/StarLedger.h"

#include "Settings.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace SH {
    namespace {
        constexpr std::uint32_t kUniqueID   = 'SKHR';
        constexpr std::uint32_t kRecordStar = 'STLG';
        constexpr std::uint32_t kRecVersion = 1;

        std::mutex        g_mx;
        stars::LedgerData g_data;             // guarded by g_mx
        std::atomic<int>  g_activeInst{ 0 };  // stars::Instrument

        // GateCeiling and GateCeilingPeek MUST answer the same number, so
        // they read the INI through one function rather than two copies of
        // the same five assignments that can drift apart.
        stars::GateParams MakeGateParams() {
            const auto&       s = Settings::GetSingleton();
            stars::GateParams p;
            p.need3[0]  = s.gateSongs2;
            p.need3[1]  = s.gateSongs3;
            p.need3[2]  = s.gateSongs4;
            p.need3[3]  = s.gateSongs5;
            p.need5At85 = s.gate5NeedsFiveStar;
            return p;
        }

        void SaveCallback(SKSE::SerializationInterface* a_intfc) {
            std::vector<std::uint8_t> buf;
            {
                std::scoped_lock lk(g_mx);
                buf = stars::Serialize(g_data);
            }
            if (a_intfc->OpenRecord(kRecordStar, kRecVersion)) {
                a_intfc->WriteRecordData(buf.data(),
                                         static_cast<std::uint32_t>(buf.size()));
            }
        }

        void LoadCallback(SKSE::SerializationInterface* a_intfc) {
            stars::LedgerData fresh;  // absent record = clean save
            std::uint32_t     type, version, length;
            while (a_intfc->GetNextRecordInfo(type, version, length)) {
                if (type != kRecordStar || version != kRecVersion) {
                    // v2: branch on version here (a bare kRecVersion bump would silently drop v1 ledgers)
                    continue;  // unknown/newer record: skip, keep fresh
                }
                std::vector<std::uint8_t> buf(length);
                if (length > 0 &&
                    a_intfc->ReadRecordData(buf.data(), length) == length) {
                    // Legacy (pre-difficulty, buffer v1-v3) best entries
                    // are stamped with the user's default difficulty - the
                    // setting those runs were almost certainly played at.
                    const int legacyDiff = std::clamp(
                        Settings::GetSingleton().difficulty, 0, 3);
                    if (!stars::Deserialize(buf.data(), buf.size(),
                                            legacyDiff, fresh)) {
                        spdlog::warn("[stars] co-save record corrupt - "
                                     "starting a fresh ledger");
                        fresh = {};
                    } else if (!buf.empty() && buf[0] < 4 &&
                               !fresh.best.empty()) {
                        spdlog::info(
                            "[stars] {} legacy pre-difficulty record(s) "
                            "migrated to difficulty {}",
                            fresh.best.size(), legacyDiff);
                    }
                }
            }
            std::size_t entries = 0;
            {
                std::scoped_lock lk(g_mx);
                g_data  = std::move(fresh);
                entries = g_data.best.size();
            }
            spdlog::info("[stars] ledger loaded: {} entries", entries);
        }

        void RevertCallback(SKSE::SerializationInterface*) {
            std::scoped_lock lk(g_mx);
            g_data = {};
        }
    }

    StarLedger& StarLedger::GetSingleton() {
        static StarLedger s;
        return s;
    }

    void StarLedger::RegisterSerialization() {
        auto* ser = SKSE::GetSerializationInterface();
        ser->SetUniqueID(kUniqueID);
        ser->SetSaveCallback(SaveCallback);
        ser->SetLoadCallback(LoadCallback);
        ser->SetRevertCallback(RevertCallback);
    }

    int StarLedger::Best(const std::string& a_chartKey,
                         stars::Instrument a_inst, int a_difficulty) const {
        std::scoped_lock lk(g_mx);
        return stars::BestFor(g_data, a_chartKey, a_inst, a_difficulty);
    }

    std::string StarLedger::LastPlayed() const {
        std::scoped_lock lk(g_mx);
        return g_data.lastPlayed;
    }

    void StarLedger::Record(const std::string& a_chartKey,
                            stars::Instrument a_inst, int a_difficulty,
                            int a_stars) {
        std::scoped_lock lk(g_mx);
        const bool improved = stars::RecordResult(g_data, a_chartKey, a_inst,
                                                  a_difficulty, a_stars);
        spdlog::info("[stars] \"{}\" inst={} diff={} stars={}{}", a_chartKey,
                     static_cast<int>(a_inst), a_difficulty, a_stars,
                     improved ? " (new best)" : "");
    }

    bool StarLedger::Taught(const std::string& a_chartKey) const {
        std::scoped_lock lk(g_mx);
        return g_data.taught.count(a_chartKey) != 0;
    }

    void StarLedger::MarkTaught(const std::string& a_chartKey) {
        bool added = false;
        {
            std::scoped_lock lk(g_mx);
            added = g_data.taught.insert(a_chartKey).second;
            g_data.newSongs.insert(a_chartKey);
        }
        // Logged even when it changed nothing: "already known" is the
        // answer to "the notification fired but the chart was open anyway".
        spdlog::info("[stars] taught \"{}\"{}", a_chartKey,
                     added ? "" : " (already known)");
    }

    bool StarLedger::IsNew(const std::string& a_chartKey) const {
        std::scoped_lock lk(g_mx);
        return g_data.newSongs.count(a_chartKey) != 0;
    }

    void StarLedger::MarkNew(const std::string& a_chartKey) {
        std::scoped_lock lk(g_mx);
        if (g_data.newSongs.insert(a_chartKey).second) {
            spdlog::info("[stars] NEW tag added for \"{}\"", a_chartKey);
        }
    }

    void StarLedger::MarkSeen(const std::string& a_chartKey) {
        std::scoped_lock lk(g_mx);
        if (g_data.newSongs.erase(a_chartKey)) {
            spdlog::info("[stars] NEW tag cleared for \"{}\"", a_chartKey);
        }
    }

    int StarLedger::GateCeiling(stars::Instrument a_inst) {
        if (!Settings::GetSingleton().rankGate) { return 100; }
        const auto       p = MakeGateParams();
        std::scoped_lock lk(g_mx);
        const auto i = static_cast<int>(a_inst);
        const auto c = stars::CountFor(g_data, a_inst);
        g_data.lifted[i] =
            stars::UpdateLifted(g_data.lifted[i], c, p);
        return stars::ClampCeiling(g_data.lifted[i]);
    }

    int StarLedger::GateCeilingPeek(stars::Instrument a_inst) const {
        if (!Settings::GetSingleton().rankGate) { return 100; }
        const auto       p = MakeGateParams();
        std::scoped_lock lk(g_mx);
        const auto i = static_cast<int>(a_inst);
        const auto c = stars::CountFor(g_data, a_inst);
        // Derives the merged mask and throws it away rather than reading
        // the stored one back: the stored mask only advances at the
        // game-thread enforcement points, so a reader that skipped
        // UpdateLifted would answer a lower ceiling than GateCeiling does
        // until the next ClampPass - a chart the player has just earned
        // would read as locked for one more trip through the browser.
        return stars::ClampCeiling(
            stars::UpdateLifted(g_data.lifted[i], c, p));
    }

    void StarLedger::SetActiveInstrument(stars::Instrument a_inst) {
        g_activeInst.store(static_cast<int>(a_inst));
    }
    stars::Instrument StarLedger::ActiveInstrument() const {
        return static_cast<stars::Instrument>(g_activeInst.load());
    }
}
