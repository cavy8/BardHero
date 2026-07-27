#include "PCH.h"
#include "Spike2Audio.h"

#include "Settings.h"

#include <Windows.h>
#include <objbase.h>  // CoInitializeEx (excluded by WIN32_LEAN_AND_MEAN)

#include "miniaudio/miniaudio.h"

#include <atomic>
#include <cmath>
#include <format>
#include <thread>

namespace SH::Spike2 {
    namespace {
        std::atomic_bool g_ran{ false };

        double g_qpcFreq = 0.0;
        double QpcSec() {
            LARGE_INTEGER c;
            QueryPerformanceCounter(&c);
            return static_cast<double>(c.QuadPart) / g_qpcFreq;
        }

        // Anchor sample published by the device callback (spec section 6).
        // The spike only logs; the real seqlock arrives in M2.
        struct Anchor {
            std::uint64_t frames = 0;
            double        qpc    = 0.0;
        };
        std::atomic<std::uint64_t> g_cbCount{ 0 };
        Anchor    g_anchor;   // written by audio thread only
        ma_engine g_engine;
        ma_device g_device;
        double    g_cbPrevQpc   = 0.0;
        double    g_cbPeriodMin = 1e9, g_cbPeriodMax = 0.0,
                  g_cbPeriodSum = 0.0, g_cbPeriodSum2 = 0.0;

        void DataCallback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
            ma_engine_read_pcm_frames(&g_engine, out, frames, nullptr);
            const double now = QpcSec();
            g_anchor.frames += frames;
            g_anchor.qpc = now;
            if (g_cbPrevQpc > 0.0) {
                const double p = (now - g_cbPrevQpc) * 1000.0;  // ms
                g_cbPeriodMin = std::min(g_cbPeriodMin, p);
                g_cbPeriodMax = std::max(g_cbPeriodMax, p);
                g_cbPeriodSum += p;
                g_cbPeriodSum2 += p * p;
            }
            g_cbPrevQpc = now;
            g_cbCount.fetch_add(1, std::memory_order_release);
            (void)dev;
        }

        void Run() {
            // Own COM init, own thread - never Skyrim's (spec section 8).
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            g_qpcFreq = static_cast<double>(f.QuadPart);

            // Engine without device; we own the device so the data callback
            // can publish the anchor (this is the M2-production shape too).
            ma_engine_config ecfg = ma_engine_config_init();
            ecfg.noDevice   = MA_TRUE;
            ecfg.channels   = 2;
            ecfg.sampleRate = 48000;
            if (ma_engine_init(&ecfg, &g_engine) != MA_SUCCESS) {
                spdlog::error("[SPIKE-2] ma_engine_init FAILED");
                return;
            }
            ma_device_config dcfg = ma_device_config_init(ma_device_type_playback);
            dcfg.playback.format   = ma_format_f32;
            dcfg.playback.channels = 2;
            dcfg.sampleRate        = 48000;
            dcfg.dataCallback      = DataCallback;
            // WASAPI SHARED mode is miniaudio's default (no exclusive flag set)
            if (ma_device_init(nullptr, &dcfg, &g_device) != MA_SUCCESS) {
                spdlog::error("[SPIKE-2] ma_device_init FAILED");
                ma_engine_uninit(&g_engine);
                return;
            }
            spdlog::info("[SPIKE-2] device up: {} Hz, period {} frames, com=0x{:X}",
                         g_device.sampleRate,
                         g_device.playback.internalPeriodSizeInFrames,
                         static_cast<std::uint32_t>(hr));

            // Load 4 stems and start them sample-synchronized 0.5s out.
            const char* names[4] = { "song", "guitar", "bass", "drums" };
            ma_sound    sounds[4]{};
            bool        ok[4]{};
            int         loaded = 0;
            for (int i = 0; i < 4; ++i) {
                const auto path = std::format(
                    "Data/SKSE/Plugins/BardHero/spike/stem_{}.ogg", names[i]);
                if (ma_sound_init_from_file(&g_engine, path.c_str(),
                                            MA_SOUND_FLAG_NO_SPATIALIZATION,
                                            nullptr, nullptr,
                                            &sounds[i]) == MA_SUCCESS) {
                    ok[i] = true;
                    ++loaded;
                } else {
                    spdlog::error("[SPIKE-2] stem load FAILED: {}", path);
                }
            }
            const ma_uint64 startAt =
                ma_engine_get_time_in_pcm_frames(&g_engine) + 24000;  // +0.5s
            for (int i = 0; i < 4; ++i) {
                if (!ok[i]) continue;
                ma_sound_set_start_time_in_pcm_frames(&sounds[i], startAt);
                ma_sound_start(&sounds[i]);
            }
            ma_device_start(&g_device);
            spdlog::info("[SPIKE-2] {} stems started @frame {}", loaded, startAt);

            // Observe: once per second compare wall-clock elapsed against
            // frames-played elapsed (anchor read is racy-by-design here; the
            // seqlock is M2 - jitter numbers a few ms wide are still valid).
            const double t0     = QpcSec();
            double       maxRes = 0.0;
            const int    durSec = Settings::GetSingleton().spike2DurationSec;
            for (int s = 0; s < durSec; ++s) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const double elapsed  = QpcSec() - t0;
                const double audioPos =
                    static_cast<double>(g_anchor.frames) / g_device.sampleRate;
                const double interp = audioPos + (QpcSec() - g_anchor.qpc);
                const double res    = std::abs(interp - elapsed);
                maxRes              = std::max(maxRes, res);
                if (s % 10 == 0) {
                    spdlog::info(
                        "[SPIKE-2] t={:.1f}s cb={} pos={:.3f}s resid={:.1f}ms",
                        elapsed, g_cbCount.load(), audioPos, res * 1000.0);
                }
                if (s == 5) {
                    // Stem alignment check: cursors of all loaded stems match.
                    for (int i = 0; i < 4; ++i) {
                        if (!ok[i]) continue;
                        ma_uint64 cur = 0;
                        ma_sound_get_cursor_in_pcm_frames(&sounds[i], &cur);
                        spdlog::info("[SPIKE-2] stem {} cursor={} frames",
                                     names[i], cur);
                    }
                }
            }
            const auto   n    = static_cast<double>(g_cbCount.load());
            const double mean = g_cbPeriodSum / std::max(1.0, n - 1);
            const double var  = g_cbPeriodSum2 / std::max(1.0, n - 1) - mean * mean;
            spdlog::info(
                "[SPIKE-2] DONE cb={} period mean={:.2f}ms min={:.2f} max={:.2f} "
                "sd={:.2f} | anchor maxResid={:.1f}ms",
                g_cbCount.load(), mean, g_cbPeriodMin, g_cbPeriodMax,
                std::sqrt(std::max(0.0, var)), maxRes * 1000.0);

            for (int i = 0; i < 4; ++i) {
                if (ok[i]) ma_sound_uninit(&sounds[i]);
            }
            ma_device_uninit(&g_device);
            ma_engine_uninit(&g_engine);
            spdlog::info("[SPIKE-2] teardown clean");
        }
    }

    namespace {
        void StartOnce(int delaySec) {
            if (!Settings::GetSingleton().spike2Audio ||
                g_ran.exchange(true)) {
                return;
            }
            std::thread([delaySec] {
                std::this_thread::sleep_for(std::chrono::seconds(delaySec));
                Run();
            }).detach();
        }
    }

    void OnSaveLoaded() { StartOnce(5); }
    void ArmAtMenu() { StartOnce(20); }
}
