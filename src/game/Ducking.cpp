// src/game/Ducking.cpp
#include "PCH.h"
#include "game/Ducking.h"

namespace SH {
    namespace {
        // Pinned by Task 6 from Skyrim.esm (SNCT records, RE-verified list
        // in the M2 plan).
        constexpr RE::FormID kMusFormId = 0x00071E64;  // AudioCategoryMUS
        constexpr RE::FormID kAmbFormId = 0x0007F80B;  // AudioCategoryAMB
        // BSIMusicType ducking value is "ck value * 100" (dB x100);
        // 100 dB = inaudible. Slot 4 forwards it to the active track
        // (Task 6: dispatcher, safe no-op when no track is active).
        constexpr std::uint16_t kDuckDbX100 = 10000;

        RE::BGSSoundCategory* Category(RE::FormID id) {
            auto* dh = RE::TESDataHandler::GetSingleton();
            return dh ? dh->LookupForm<RE::BGSSoundCategory>(id, "Skyrim.esm")
                      : nullptr;
        }
    }

    Ducking& Ducking::GetSingleton() {
        static Ducking instance;
        return instance;
    }

    void Ducking::Apply(bool duckCurrentMusic, bool duckAmbience) {
        if (_applied) return;
        _musDucked = false;
        if (auto* mus = Category(kMusFormId)) {
            // cache the LOCAL multiplier - GetCategoryVolume() is the
            // recursive effective product (Task 6 RE: slot 2 walks parents);
            // caching effective and restoring local decays the user's slider
            // every cycle (first field run: 0.06 -> 0.01 over five sessions).
            // SetCategoryVolume writes the local mult, so the read/restore
            // pair is symmetric on volumeMult (+0x50).
            _musVol = mus->volumeMult;
            mus->SetCategoryVolume(0.0f);
            _musDucked = true;
        } else {
            spdlog::warn("[duck] music category not found");
        }
        _ambDucked = false;
        if (duckAmbience) {
            if (auto* amb = Category(kAmbFormId)) {
                _ambVol = amb->volumeMult;  // local mult, same asymmetry as MUS
                amb->SetCategoryVolume(0.0f);
                _ambDucked = true;
            } else {
                spdlog::warn("[duck] ambience category not found");
            }
        }
        _duckedMusic = nullptr;
        if (duckCurrentMusic) {
            if (auto* mm = RE::BSMusicManager::GetSingleton(); mm && mm->current) {
                mm->current->DoApplyDuckingAttenuation(kDuckDbX100);
                // current is always a BGSMusicType form; forms never unload,
                // so this cannot dangle
                _duckedMusic = mm->current;
            }
        }
        _applied = true;
        spdlog::info("[duck] applied (musVol {:.2f}, amb {}, current {})",
                     _musVol, _ambDucked, _duckedMusic != nullptr);
    }

    void Ducking::Restore() {
        if (!_applied) return;
        if (_musDucked) {
            if (auto* mus = Category(kMusFormId)) mus->SetCategoryVolume(_musVol);
        }
        if (_ambDucked) {
            if (auto* amb = Category(kAmbFormId)) amb->SetCategoryVolume(_ambVol);
        }
        if (_duckedMusic) {
            _duckedMusic->DoClearDucking();
            _duckedMusic = nullptr;
        }
        _applied = false;
        spdlog::info("[duck] restored");
    }
}
