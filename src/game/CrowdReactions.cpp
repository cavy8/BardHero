// src/game/CrowdReactions.cpp
#include "PCH.h"
#include "game/CrowdReactions.h"

#include "Settings.h"
#include "audio/AudioEngine.h"

#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace SH::CrowdReactions {

    namespace {
        // THE mapping, and the only copy of it. Slot i holds
        // "<kNames[i]>.wav" and logs as kNames[i], so the loader and the log
        // read the same array and the index-to-sound contract that
        // PlayCrowdSfx relies on has nothing to drift against.
        constexpr const char* kNames[] = { "cheer", "groan", "swell",
                                           "applause", "awkward" };
        static_assert(std::size(kNames) ==
                          static_cast<std::size_t>(Kind::kAwkward) + 1,
                      "kNames must cover every Kind, in enumerator order");

        enum class Boot { kCold, kReady, kFailed };

        // Session thread only (Prepare, Fire and Reset are all called from
        // it), so plain statics - no atomics needed and none implied.
        Boot   g_boot   = Boot::kCold;
        double g_lastAt = -1e9;

        // Deliberately never deleted once ready. A static AudioEngine would
        // run ~AudioEngine -> Uninit() -> miniaudio device teardown during
        // process exit, on an unpredictable thread and after the game has
        // begun tearing itself down; a bank of one-shots has nothing worth
        // releasing there, and the OS reclaims it anyway. Outliving
        // everything is also the entire point: the applause fires
        // microseconds before the SESSION's engine dies, so it cannot be
        // playing on that engine.
        AudioEngine* g_engine = nullptr;

        const char* NameOf(Kind a_kind) {
            const auto i = static_cast<std::size_t>(a_kind);
            return i < std::size(kNames) ? kNames[i] : "?";
        }
    }

    void Prepare() {
        // kFailed is TERMINAL. Prepare runs at every session start, and a
        // machine with no usable playback device now will not have one three
        // songs later - retrying would re-attempt a failing WASAPI open once
        // per song, forever, on the session thread.
        if (g_boot != Boot::kCold) { return; }
        // Left cold rather than failed: the setting is read from the INI once
        // per process, but staying cold costs nothing and means a build that
        // ever learns to reload it still gets a bank at the next start.
        if (!Settings::GetSingleton().crowdReactions) { return; }

        g_engine = new AudioEngine();
        if (!g_engine->Init()) {
            // Init has already unwound its own partial state. Drop the object
            // here, where the thread and the moment are both known, rather
            // than keeping a device-less engine around for the process.
            delete g_engine;
            g_engine = nullptr;
            g_boot   = Boot::kFailed;
            spdlog::warn(
                "[crowd] no audio device - reactions are off for this "
                "process (the performance itself is unaffected)");
            return;
        }
        const std::filesystem::path dir =
            "Data/SKSE/Plugins/BardHero/sfx/crowd";
        std::vector<std::filesystem::path> files;
        for (const char* n : kNames) {
            files.push_back(dir / (std::string(n) + ".wav"));
        }
        // Loaded ONCE for the process. fCrowdVolume is read from the INI at
        // plugin init and never reloaded, so a per-session reload would apply
        // the identical value at the cost of decoding all five again.
        g_engine->LoadCrowdSfx(
            files, static_cast<float>(Settings::GetSingleton().crowdVolume));
        g_boot = Boot::kReady;
    }

    void Fire(Kind a_kind, double a_nowQpc) {
        const auto& st = Settings::GetSingleton();
        if (!st.crowdReactions || g_boot != Boot::kReady) { return; }
        // The end-of-song sounds are the payoff for the whole performance
        // and each fires exactly once, so a cooldown from the last cheer
        // must never eat one.
        //
        // kSwell joined this list on 2026-07-22, when it finally got a
        // caller: the rank-up moment, fired from EndSession IMMEDIATELY
        // after the room's applause. Without the bypass that applause sets
        // g_lastAt and then swallows the swell for the whole cooldown - the
        // rarest sound in the mod, eaten by the one that always precedes it.
        const bool endOfSong = a_kind == Kind::kApplause ||
                               a_kind == Kind::kAwkward ||
                               a_kind == Kind::kSwell;
        if (!endOfSong && a_nowQpc - g_lastAt < st.reactionCooldownSec) {
            return;
        }
        g_lastAt = a_nowQpc;
        g_engine->PlayCrowdSfx(static_cast<int>(a_kind));
        // Logged even when the slot is empty: "the reaction fired but you
        // heard nothing" and "the reaction never fired" are different bugs
        // that sound identical, and the load warning already names the file.
        spdlog::info("[crowd] reaction {}", NameOf(a_kind));
    }

    void Reset() { g_lastAt = -1e9; }
}
