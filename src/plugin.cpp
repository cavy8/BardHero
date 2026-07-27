#include "PCH.h"

#include "Branding.h"
#include "Settings.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h's INI callback typedefs

#include "FUCK_API.h"

#include "game/AtronachRecipe.h"
#include "game/InputHook.h"
#include "game/LessonNotificationHook.h"
#include "game/MoodGlobals.h"
#include "game/MovementGuard.h"
#include "game/PerformTriggerHook.h"
#include "game/PerformanceCamera.h"
#include "game/Session.h"
#include "game/ShadowPause.h"
#include "game/SgtProgression.h"
#include "game/StarLedger.h"

#include "render/RenderUi.h"

#include "spikes/Spike2Audio.h"
#include "spikes/Spike3Render.h"

#include <filesystem>    // log rotation
#include <system_error>  // non-throwing rename/remove

namespace {
    constexpr auto kLogName     = "BardHero.log";
    constexpr auto kPrevLogName = "BardHero.prev.log";

    void SetupLog() {
        auto path = SKSE::log::log_directory();
        if (!path) {
            return;
        }
        auto prev = *path / kPrevLogName;
        *path /= kLogName;

        // Rotate, do not just truncate. A field run's evidence lives ONLY
        // in this file and the next launch wipes it - which has now cost
        // two diagnoses outright (2026-07-20: a lock repro and a camera
        // repro, both relaunched before the log was collected). Keeping one
        // generation means a forgotten collection is recoverable instead of
        // needing the whole test re-run. Best effort: a failed rename must
        // never stop the plugin loading.
        std::error_code ec;
        std::filesystem::remove(prev, ec);
        std::filesystem::rename(*path, prev, ec);

        auto sink   = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
        logger->set_level(spdlog::level::debug);
        logger->flush_on(spdlog::level::debug);

        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        if (!a_msg) {
            return;
        }
        switch (a_msg->type) {
            // SmoothCam's handshake is a two-step at fixed moments: register
            // the interface callback at kPostLoad, ask for the interface at
            // kPostPostLoad. Both are no-ops when SmoothCam is absent.
            case SKSE::MessagingInterface::kPostLoad:
                SH::PerformanceCamera::RegisterSmoothCam();
                break;
            case SKSE::MessagingInterface::kPostPostLoad:
                SH::PerformanceCamera::RequestSmoothCam();
                break;
            case SKSE::MessagingInterface::kDataLoaded:
                SH::Settings::GetSingleton().Load();
                // Soft dependency: without FUCK.dll BardHero has no UI yet -
                // one log line and the plugin idles (house pattern).
                if (!FUCK::Connect(SH::kDisplayName)) {
                    spdlog::info("FLICK (FUCK.dll) not present; UI disabled.");
                } else {
                    // Connect-before-Register, house ordering.
                    SH::Spike3::Register();
                    SH::RenderUi::Register();
                }
                SH::LessonNotificationHook::Install();
                // Real (movie-less) pausing menu for the session's world
                // pause - the engine's own bookkeeping pauses voice lines,
                // which the manual numPausesGame bump never did.
                bard::ShadowPause::Register();
                SH::InputHook::Install();
                SH::Spike2::ArmAtMenu();
                SH::Session::GetSingleton().Install();
                // AFTER Session::Install - the hook's IsPerformSpell filter
                // reads the perform-spell IDs that Install resolves.
                SH::PerformTriggerHook::Install();
                // Cinematic performance camera. Self-gating: returns without
                // installing anything unless [Session]
                // bPerformanceCameraDirector is set at load, so a player who
                // has never enabled it carries no ThirdPersonState hook.
                SH::PerformanceCamera::Install();
                SH::MoodGlobals::Install();
                // Appends the Doom Lute's ingredients to the vanilla
                // Atronach Forge lists. Deliberately a runtime append rather
                // than an ESP override - see AtronachRecipe.cpp.
                SH::AtronachRecipe::Install();
                break;
            case SKSE::MessagingInterface::kPostLoadGame:
            case SKSE::MessagingInterface::kNewGame:
                SH::Spike2::OnSaveLoaded();
                SH::MovementGuard::Post(false);  // un-strand after any load
                SH::SgtProgression::OnPostLoadGame();
                // kNewGame lands here without ever seeing kPreLoadGame, so
                // per-timeline state has to be dropped on this edge too.
                SH::Session::GetSingleton().OnPostLoadGame();
                break;
            case SKSE::MessagingInterface::kPreLoadGame:
                SH::Session::GetSingleton().OnPreLoadGame();
                // BEFORE the save's expertise globals are swapped in - the
                // teaching baseline must not straddle the swap.
                SH::SgtProgression::OnPreLoadGame();
                break;
            default:
                break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SetupLog();
    // __DATE__/__TIME__ are the COMPILE stamp of this translation unit, so
    // they change with every rebuild while kVersion does not.
    //
    // Why it is worth a line: kVersion is a hand-maintained string that stays
    // identical across a whole evening of builds, so a log could not answer
    // "is the game even running the DLL I just deployed?". On 2026-07-27 that
    // cost a round trip - a fix was reported broken from a session running a
    // binary two hours older than the one on disk, and only the ABSENCE of a
    // new log message gave it away. This makes the first line of the log
    // answer it outright.
    spdlog::info("{} v{} loading... (build {} {})", SH::kDisplayName,
                 SH::kVersion, __DATE__, __TIME__);

    // Universal SE + AE build, same gate as Fitting Room: SE 1.5.97 or
    // next-gen AE (1.6.1130+). Every engine address is runtime-resolved;
    // AE hook offsets get byte-verified before install (InputHook).
    const auto ver       = a_skse->RuntimeVersion();
    const bool supported = (ver == SKSE::RUNTIME_SSE_1_5_97) ||
                           (ver >= REL::Version(1, 6, 1130, 0));
    if (!supported) {
        spdlog::error("Unsupported Skyrim runtime {}; not loading.", ver.string());
        return false;
    }

    SKSE::Init(a_skse);
    SH::StarLedger::RegisterSerialization();
    SKSE::AllocTrampoline(64);
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
