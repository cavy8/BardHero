// src/game/UiSfx.cpp
#include "PCH.h"
#include "game/UiSfx.h"

#include "Settings.h"
#include "audio/AudioEngine.h"

#include <atomic>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace SH::UiSfx {

    namespace {
        enum class Boot { kCold, kReady, kFailed };

        // Prepare runs on the session thread, but Fire and the tick loop
        // run from the render thread too, so readiness is a
        // release-store the consumers acquire-load; g_engine is written
        // before the kReady store and never changes afterwards.
        std::atomic<Boot> g_boot{ Boot::kCold };

        // Deliberately never deleted once ready - same rationale as the
        // crowd's engine (CrowdReactions.cpp): teardown at process exit
        // runs on an unpredictable thread after the game is half-dead,
        // a bank of one-shots has nothing worth releasing there, and
        // outliving the session engine is the entire point.
        AudioEngine* g_engine = nullptr;

        // Render-thread only in practice, but atomic so the session
        // thread could ever join in without a rewrite. Exists so the
        // belt-and-braces StopScoreTick in ResultsWindow::Close() does
        // not double-log after the count-up already stopped the loop.
        std::atomic<bool> g_tickOn{ false };
    }

    void Prepare() {
        // kFailed is TERMINAL, exactly like the crowd bank: a machine
        // with no usable playback device now will not grow one three
        // songs later, and retrying would re-open a failing WASAPI
        // device once per song forever.
        if (g_boot.load(std::memory_order_acquire) != Boot::kCold) {
            return;
        }
        // Left cold rather than failed: the setting is read once per
        // process today, but staying cold costs nothing and a build that
        // ever learns to reload it still gets a bank at the next start.
        if (!Settings::GetSingleton().uiSfx) { return; }

        auto* engine = new AudioEngine();
        if (!engine->Init()) {
            delete engine;
            g_boot.store(Boot::kFailed, std::memory_order_release);
            spdlog::warn(
                "[ui_sfx] no audio device - UI sfx are off for this "
                "process (the performance itself is unaffected)");
            return;
        }
        const std::filesystem::path dir =
            "Data/SKSE/Plugins/BardHero/sfx/ui";
        std::vector<std::filesystem::path> files;
        for (const char* n : ui_sfx::kCueNames) {
            files.push_back(dir / (std::string(n) + ".wav"));
        }
        // ONE common gain for the whole bank: relative loudness is baked
        // into the files (request-to-main-2026-07-25-ui-sfx.md); a
        // per-file normalization here would collapse the mix. The only
        // departure is ui_sfx::CueGainScale's SP trim, applied by
        // SetVolume immediately below (documented at the table).
        const auto gain =
            static_cast<float>(Settings::GetSingleton().uiSfxVolume);
        engine->LoadUiSfx(files, gain);
        g_engine = engine;
        g_boot.store(Boot::kReady, std::memory_order_release);
        SetVolume(gain);
    }

    void Fire(ui_sfx::Cue a_cue) {
        if (g_boot.load(std::memory_order_acquire) != Boot::kReady) {
            return;
        }
        g_engine->PlayUiSfx(static_cast<int>(a_cue));
        // Logged even when the slot is empty: "the cue fired but you
        // heard nothing" and "the cue never fired" are different bugs
        // that sound identical, and the load warning already names the
        // missing file.
        spdlog::info("[ui_sfx] {}", ui_sfx::FileName(a_cue));
    }

    void SetVolume(float a_gain) {
        if (g_boot.load(std::memory_order_acquire) != Boot::kReady) {
            return;
        }
        // Per SLOT, so the SP trim survives a live slider move. Walking
        // kCueNames rather than the engine's slot count keeps the cue
        // enum the single source of the mapping.
        for (std::size_t i = 0; i < std::size(ui_sfx::kCueNames); ++i) {
            const auto cue = static_cast<ui_sfx::Cue>(i);
            g_engine->SetUiSfxSlotVolume(
                static_cast<int>(i), a_gain * ui_sfx::CueGainScale(cue));
        }
    }

    void StartScoreTick() {
        if (g_boot.load(std::memory_order_acquire) != Boot::kReady) {
            return;
        }
        g_tickOn.store(true, std::memory_order_relaxed);
        g_engine->StartUiLoop(static_cast<int>(ui_sfx::Cue::kScoreTick));
        spdlog::info("[ui_sfx] score_tick loop start");
    }

    void StopScoreTick() {
        if (g_boot.load(std::memory_order_acquire) != Boot::kReady) {
            return;
        }
        if (!g_tickOn.exchange(false, std::memory_order_relaxed)) {
            return;
        }
        g_engine->StopUiLoop(static_cast<int>(ui_sfx::Cue::kScoreTick));
        spdlog::info("[ui_sfx] score_tick loop stop");
    }
}
