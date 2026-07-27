// src/audio/AudioEngine.cpp
#include "PCH.h"
#include "audio/AudioEngine.h"

#include "QpcClock.h"
#include "audio/StemSeekLogic.h"
#include "game/BaLibraryBootstrap.h"

#include "miniaudio/miniaudio.h"

// Pitch-preserving time-stretch for practice playback speed (plan P6).
// Header-only and vendored under extern/signalsmith; tests/test_stretch.cpp
// pins the two API facts the callback below depends on - the ratio is
// implicit in process(in, inFrames, out, outFrames), and the buffers are
// PLANAR while miniaudio is interleaved.
#include "signalsmith-stretch.h"

// Hand-written libopus decoding backend, defined in MiniaudioImpl.cpp (the one
// MINIAUDIO_IMPLEMENTATION TU). The vendored 0.11.21 ma_libopus is data-source
// only, so the vtable is authored there rather than exported by the header;
// we reference it by extern here to register it with the resource manager.
extern ma_decoding_backend_vtable g_ma_decoding_backend_vtable_libopus;

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace SH {

    namespace {
        // For logging only: path.string() can throw/mojibake on exactly the
        // non-ANSI names we open with the _w variant; u8string never does.
        std::string PathUtf8(const std::filesystem::path& p) {
            const auto u8 = p.u8string();
            return std::string(reinterpret_cast<const char*>(u8.data()),
                               u8.size());
        }

        // Kept only as the spatializer's distance BASIS - they no longer
        // shape a gain curve, because rolloff is 0 (see LoadStems). They
        // used to carry a real falloff with a 0.35 gain floor, chosen so an
        // ordinary third-person orbit at 150-250u sat inside the flat zone.
        // That held only while the camera stayed put: the field log for
        // 2026-07-26 shows three sessions at 162-257u untouched and one at
        // 433u audibly ducked, and the camera director will spend most of a
        // song outside 300u by design. A threshold the feature is built to
        // cross is not a threshold.
        constexpr float kMinDistUnits = 300.0f;
        constexpr float kMaxDistUnits = 4000.0f;
    }

    // GH-feel P4: whammy pitch bend for the guitar stem. A modulated
    // FRACTIONAL DELAY, not a rate change: the read tap oscillates a few
    // milliseconds around a fixed centre, which bends pitch audibly while
    // the stem stays sample-locked to MasterClock by construction (a true
    // pitch/rate shift would drift, and per the header the master
    // resampler must remain the only rate authority). Fully wet only
    // while the whammy is engaged; the dry/wet ramp returns to a
    // bit-exact passthrough when the smoothed depth settles at zero, so
    // idle playback is untouched. The audio thread owns every field
    // except `target`.
    struct VibratoNode {
        ma_node_base base{};
        static constexpr ma_uint32 kFrames = 1024;  // 21ms at 48k, pow2
        float              ring[kFrames * 2]{};
        ma_uint32          cursor = 0;
        double             phase  = 0.0;
        float              depth  = 0.0f;
        std::atomic<float> target{ 0.0f };
    };

    namespace {
        void VibratoProcess(ma_node* node, const float** framesIn,
                            ma_uint32* frameCountIn, float** framesOut,
                            ma_uint32* frameCountOut) {
            auto*        v   = reinterpret_cast<VibratoNode*>(node);
            const float* in  = framesIn[0];
            float*       out = framesOut[0];
            const ma_uint32 n = std::min(*frameCountIn, *frameCountOut);
            const float tgt = v->target.load(std::memory_order_relaxed);
            // One-pole depth smoothing per frame: ~35ms swell in, ~105ms
            // settle out - the same feel as the trail envelope, and it
            // keeps engagement clickless.
            constexpr float  kUp    = 0.0006f;
            constexpr float  kDn    = 0.0002f;
            constexpr float  kBase  = 96.0f;   // 2ms centre tap
            constexpr float  kSwing = 110.0f;  // ~0.8 semitone peak bend
            constexpr double kLfo =
                2.0 * 3.14159265358979 * 5.5 / 48000.0;  // 5.5Hz
            constexpr ma_uint32 kMask = VibratoNode::kFrames - 1;
            for (ma_uint32 f = 0; f < n; ++f) {
                v->depth += (tgt - v->depth)
                    * (tgt > v->depth ? kUp : kDn);
                v->phase += kLfo;
                if (v->phase > 6.28318530718) {
                    v->phase -= 6.28318530718;
                }
                const float mod =
                    0.5f + 0.5f * static_cast<float>(std::sin(v->phase));
                const float delay = kBase + v->depth * kSwing * mod;
                const ma_uint32 w = v->cursor;
                v->ring[w * 2 + 0] = in[f * 2 + 0];
                v->ring[w * 2 + 1] = in[f * 2 + 1];
                v->cursor = (w + 1) & kMask;
                const float rp = static_cast<float>(w) - delay;
                const float fl = std::floor(rp);
                const ma_uint32 i0 =
                    static_cast<ma_uint32>(static_cast<int>(fl)) & kMask;
                const ma_uint32 i1 = (i0 + 1) & kMask;
                const float frac = rp - fl;
                const float wetL = v->ring[i0 * 2 + 0]
                    + (v->ring[i1 * 2 + 0] - v->ring[i0 * 2 + 0]) * frac;
                const float wetR = v->ring[i0 * 2 + 1]
                    + (v->ring[i1 * 2 + 1] - v->ring[i0 * 2 + 1]) * frac;
                // Bit-exact dry when depth has settled at zero.
                const float wet = std::min(1.0f, v->depth * 6.0f);
                out[f * 2 + 0] =
                    in[f * 2 + 0] + (wetL - in[f * 2 + 0]) * wet;
                out[f * 2 + 1] =
                    in[f * 2 + 1] + (wetR - in[f * 2 + 1]) * wet;
            }
            *frameCountIn  = n;
            *frameCountOut = n;
        }

        constexpr ma_uint32 kVibratoChannels[1] = { 2 };
        const ma_node_vtable kVibratoVtable = {
            VibratoProcess, nullptr, 1, 1, 0
        };
    }

    // Star Power flanger as a graph node so it can be scoped PER STEM.
    // It sat on the whole engine output first (device callback); field
    // 2026-07-26: "the flanger affects the voice too much, and it sounds
    // bad" - the backing/vocal "song" stem must stay clean, so the effect
    // now lives where the vibrato does and only instrument stems route
    // through it (LoadStems decides which).
    struct FlangerNode {
        ma_node_base base{};
        SpFlanger    dsp;
    };

    namespace {
        void FlangerProcess(ma_node* node, const float** framesIn,
                            ma_uint32* frameCountIn, float** framesOut,
                            ma_uint32* frameCountOut) {
            auto*           fn = reinterpret_cast<FlangerNode*>(node);
            const ma_uint32 n  = std::min(*frameCountIn, *frameCountOut);
            std::memcpy(framesOut[0], framesIn[0],
                        static_cast<std::size_t>(n) * 2 * sizeof(float));
            fn->dsp.Process(framesOut[0], n);
            *frameCountIn  = n;
            *frameCountOut = n;
        }

        const ma_node_vtable kFlangerVtable = {
            FlangerProcess, nullptr, 1, 1, 0
        };
    }

    struct AudioEngine::Impl {
        ma_resource_manager resourceManager{};
        ma_engine    engine{};
        ma_device    device{};
        ma_resampler resampler{};
        bool         rmUp = false;
        bool         engineUp = false, deviceUp = false, resamplerUp = false;

        // ma_sound must never move after init: sized once in LoadStems.
        std::vector<ma_sound>    sounds;
        std::vector<std::string> names;
        std::size_t              loaded = 0;
        // stems went through the 3D spatializer this session; the listener
        // and source setters are no-ops otherwise
        bool                     spatial = false;
        // base stem level; the miss-mute fade restores to THIS, not 1.0
        std::atomic<float>       songVolume{ 1.0f };

        // Miss SFX one-shots (same no-move rule: sized once in LoadMissSfx).
        std::vector<ma_sound> sfx;
        std::size_t           sfxLoaded = 0;
        std::atomic<unsigned> sfxNext{ 0 };

        // Crowd reaction one-shots (same no-move rule: sized once in
        // LoadCrowdSfx). Index-addressed, so the slots stay aligned with the
        // caller's file order and crowdOk[i] - not a count - says whether
        // slot i holds an initialised sound.
        std::vector<ma_sound> crowd;
        std::vector<char>     crowdOk;

        // UI SFX one-shots (same no-move and index-addressed rules as the
        // crowd bank above).
        std::vector<ma_sound> ui;
        std::vector<char>     uiOk;

        // Whammy vibrato node, inserted between the guitar stem and the
        // engine endpoint (see VibratoNode above).
        VibratoNode vibrato;
        bool        vibratoUp = false;

        // Star Power flanger node; instrument stems route through it, the
        // backing/vocal "song" stem does not (see FlangerNode above).
        FlangerNode flanger;
        bool        flangerUp = false;
        // routing policy for LoadStems: also send a multi-stem song's
        // "song" stem through the filter (default OFF - that stem carries
        // the vocals). A single-stem song always routes: there the whole
        // track IS the instrument (miss-mute precedent).
        bool flangeSongStem = false;
        // last published SP state, for transition logging only (SetSpFilter
        // is called every session tick; the log wants edges, not spam)
        bool spFilterLast = false;

        std::atomic<bool>   paused{ false };
        std::atomic<double> targetRate{ 1.0 };
        bard::AnchorSeqlock anchor;
        std::uint64_t       framesConsumed = 0;  // audio thread only
        double              lastRate = 1.0;      // audio thread only: ratio the
                                                 // resampler actually accepted

        // Device period is ~480 frames (10ms); worst-case required input at
        // ratio 1.05 is ~504. The inWant clamp chunks any oversized period,
        // so 4096 is pure headroom - correctness never depends on it.
        static constexpr ma_uint32 kScratchFrames = 4096;
        float scratch[kScratchFrames * 2]{};

        // ---- practice time-stretch (plan P6) --------------------------
        // Every field here is inert at speed 1.0, which is the only speed an
        // ordinary performance ever sees: the callback's unity branch does
        // not touch the stretcher at all.
        std::atomic<double> songSpeed{ 1.0 };
        double              lastSpeed = 1.0;  // audio thread only
        // Set by SetSongSpeed on the SESSION thread once presetDefault has
        // allocated. Until then the callback refuses to enter the stretch
        // path, so the audio thread never touches an uninitialised stretcher.
        std::atomic<bool> stretchReady{ false };
        signalsmith::stretch::SignalsmithStretch<float> stretch;
        float stretchIn[kScratchFrames * 2]{};   // interleaved engine read
        float planarIn[2][kScratchFrames]{};     // ...de-interleaved
        float planarOut[2][kScratchFrames]{};    // ...and the stretched result

        // static member so it may access the private nested Impl type
        static void DataCallback(ma_device* dev, void* out, const void*,
                                 ma_uint32 frameCount) {
            auto* im  = static_cast<Impl*>(dev->pUserData);
            auto* dst = static_cast<float*>(out);
            double rate = 0.0;  // published rate; 0 freezes interpolation
            if (im->paused.load(std::memory_order_acquire)) {
                std::memset(dst, 0, sizeof(float) * 2 * frameCount);
            } else {
                rate = im->targetRate.load(std::memory_order_acquire);
                // Publish only the rate the resampler actually applies: skip
                // the redundant set at an unchanged ratio, and on rejection
                // fall back to the last accepted ratio so the estimator never
                // interpolates against a rate that isn't in effect.
                if (rate != im->lastRate) {
                    if (ma_resampler_set_rate_ratio(&im->resampler,
                                                    static_cast<float>(rate)) ==
                        MA_SUCCESS) {
                        im->lastRate = rate;
                    } else {
                        rate = im->lastRate;
                    }
                }
                // Practice playback speed. Read ONCE per callback so the
                // whole block is coherent, and forced to unity unless the
                // session thread has actually armed the stretcher.
                double speed = im->songSpeed.load(std::memory_order_acquire);
                if (!(speed > 0.0) ||
                    !im->stretchReady.load(std::memory_order_acquire)) {
                    speed = 1.0;
                }
                if (speed != im->lastSpeed) {
                    // A ratio change must not inherit a pipeline still full
                    // of the previous one - that reads as a smear at the
                    // moment of the change.
                    im->stretch.reset();
                    im->lastSpeed = speed;
                }
                ma_uint64 outDone = 0;
                while (outDone < frameCount) {
                    ma_uint64 outWant = frameCount - outDone;
                    ma_uint64 inWant  = 0;
                    ma_resampler_get_required_input_frame_count(
                        &im->resampler, outWant, &inWant);
                    if (inWant > kScratchFrames) inWant = kScratchFrames;
                    ma_uint64 inRead = 0;
                    ma_uint64 songFrames = 0;  // engine frames really consumed
                    if (speed == 1.0) {
                        // THE ORIGINAL PATH, unchanged. Ordinary performance
                        // never enters the branch below.
                        ma_engine_read_pcm_frames(&im->engine, im->scratch,
                                                  inWant, &inRead);
                        // An engine endpoint never underfills (renders silence
                        // to full length); this branch is defensive so a
                        // hypothetical short read cannot stall the output loop
                        // - it does advance the anchor past real engine time,
                        // acceptable for a can't-happen path.
                        if (inRead < inWant) {
                            std::memset(
                                im->scratch + inRead * 2, 0,
                                static_cast<std::size_t>(inWant - inRead) * 2 *
                                    sizeof(float));
                            inRead = inWant;
                        }
                    } else {
                        // The stretcher turns `engWant` engine frames into
                        // `inWant` frames for the resampler. Cap inWant FIRST:
                        // at speed 2.0 an uncapped inWant would ask the engine
                        // for twice the scratch buffer and overrun it.
                        ma_uint64 cap = static_cast<ma_uint64>(
                            static_cast<double>(kScratchFrames) / speed);
                        if (cap < 1) { cap = 1; }
                        if (inWant > cap) { inWant = cap; }
                        if (inWant < 1) { inWant = 1; }
                        ma_uint64 engWant = static_cast<ma_uint64>(
                            static_cast<double>(inWant) * speed + 0.5);
                        if (engWant < 1) { engWant = 1; }
                        if (engWant > kScratchFrames) {
                            engWant = kScratchFrames;
                        }
                        ma_uint64 engRead = 0;
                        ma_engine_read_pcm_frames(&im->engine, im->stretchIn,
                                                  engWant, &engRead);
                        if (engRead < engWant) {
                            std::memset(
                                im->stretchIn + engRead * 2, 0,
                                static_cast<std::size_t>(engWant - engRead) *
                                    2 * sizeof(float));
                            engRead = engWant;
                        }
                        // miniaudio is INTERLEAVED, the library is PLANAR.
                        for (ma_uint64 i = 0; i < engRead; ++i) {
                            im->planarIn[0][i] = im->stretchIn[i * 2];
                            im->planarIn[1][i] = im->stretchIn[i * 2 + 1];
                        }
                        float* inPtrs[2]  = { im->planarIn[0],
                                              im->planarIn[1] };
                        float* outPtrs[2] = { im->planarOut[0],
                                              im->planarOut[1] };
                        // No time-ratio setter exists: the ratio IS
                        // outputSamples/inputSamples (tests/test_stretch.cpp).
                        im->stretch.process(inPtrs, static_cast<int>(engRead),
                                            outPtrs, static_cast<int>(inWant));
                        for (ma_uint64 i = 0; i < inWant; ++i) {
                            im->scratch[i * 2]     = im->planarOut[0][i];
                            im->scratch[i * 2 + 1] = im->planarOut[1][i];
                        }
                        inRead     = inWant;
                        songFrames = engRead;
                    }
                    ma_uint64 inFrames = inRead, outFrames = outWant;
                    ma_resampler_process_pcm_frames(&im->resampler, im->scratch,
                                                    &inFrames, dst + outDone * 2,
                                                    &outFrames);
                    // framesConsumed is the SONG clock the estimator reads.
                    // At unity it stays exactly what the resampler consumed.
                    // Stretched, it has to be the frames pulled from the
                    // ENGINE instead: counting the stretcher's OUTPUT would
                    // advance song position at wall-clock rate and put the
                    // highway ahead of the audio by the whole speed factor.
                    im->framesConsumed += speed == 1.0 ? inFrames : songFrames;
                    outDone += outFrames;
                    if (outFrames == 0 && inFrames == 0) break;  // no progress
                }
                // The published rate is SONG seconds per wall second - what
                // AnchorEstimator::Position extrapolates with between
                // anchors. Stretching changes exactly that, so fold it in or
                // every between-callback read is off by (1 - speed) of a
                // period. Applied AFTER the resampler bookkeeping above, so
                // lastRate still tracks the ratio the resampler accepted.
                rate *= speed;
            }
            im->anchor.Publish({ im->framesConsumed, QpcSec(), rate });
        }

        // Stale-anchor diagnostics (field 2026-07-19: one unexplained
        // "anchor stale" pause): miniaudio raises these off OS/audio threads
        // when the device stops, reroutes (default-output switch - the
        // AutoAudioSwitch suspect), or is interrupted. Log-only; spdlog
        // sinks are _mt.
        static void OnNotification(const ma_device_notification* n) {
            const char* what = "unknown";
            switch (n->type) {
                case ma_device_notification_type_started:
                    what = "started"; break;
                case ma_device_notification_type_stopped:
                    what = "stopped"; break;
                case ma_device_notification_type_rerouted:
                    what = "rerouted (output device changed)"; break;
                case ma_device_notification_type_interruption_began:
                    what = "interruption began"; break;
                case ma_device_notification_type_interruption_ended:
                    what = "interruption ended"; break;
                case ma_device_notification_type_unlocked:
                    what = "unlocked"; break;
            }
            spdlog::info("[audio] device notification: {}", what);
        }
    };

    AudioEngine::AudioEngine() : _impl(std::make_unique<Impl>()) {}
    AudioEngine::~AudioEngine() { Uninit(); }

    bool AudioEngine::Init() {
        auto& im = *_impl;

        // Own the resource manager explicitly so we can register the libopus
        // decoding backend (spec Q5) alongside miniaudio's built-in decoders.
        // decodedFormat/Channels/SampleRate mirror the engine's own defaults
        // (f32, stereo, 48 kHz), so existing .ogg/.wav loads are unchanged; the
        // custom backend only handles .opus.
        static ma_decoding_backend_vtable* kBackends[] = {
            &g_ma_decoding_backend_vtable_libopus,
        };
        ma_resource_manager_config rmcfg = ma_resource_manager_config_init();
        rmcfg.ppCustomDecodingBackendVTables = kBackends;
        rmcfg.customDecodingBackendCount     = 1;
        rmcfg.decodedFormat     = ma_format_f32;
        rmcfg.decodedChannels   = 2;
        rmcfg.decodedSampleRate = 48000;
        if (ma_resource_manager_init(&rmcfg, &im.resourceManager) !=
            MA_SUCCESS) {
            spdlog::error("[audio] ma_resource_manager_init failed");
            return false;
        }
        im.rmUp = true;

        ma_engine_config ecfg = ma_engine_config_init();
        ecfg.noDevice        = MA_TRUE;
        ecfg.channels        = 2;
        ecfg.sampleRate      = 48000;
        ecfg.pResourceManager = &im.resourceManager;
        if (ma_engine_init(&ecfg, &im.engine) != MA_SUCCESS) {
            spdlog::error("[audio] ma_engine_init failed");
            Uninit();
            return false;
        }
        im.engineUp = true;

        {
            // SP flanger first so the vibrato can chain into it:
            // guitar -> vibrato -> flanger -> endpoint, other instrument
            // stems -> flanger -> endpoint, "song" -> endpoint.
            ma_node_config fcfg  = ma_node_config_init();
            fcfg.vtable          = &kFlangerVtable;
            fcfg.pInputChannels  = kVibratoChannels;
            fcfg.pOutputChannels = kVibratoChannels;
            if (ma_node_init(ma_engine_get_node_graph(&im.engine), &fcfg,
                             nullptr, &im.flanger.base) == MA_SUCCESS) {
                ma_node_attach_output_bus(
                    &im.flanger.base, 0,
                    ma_engine_get_endpoint(&im.engine), 0);
                im.flangerUp = true;
            } else {
                // Presentation-only, like the vibrato: no audible SP
                // filter this session; nothing else may depend on it.
                spdlog::warn("[audio] sp flanger node init failed");
            }
        }

        {
            ma_node_config ncfg   = ma_node_config_init();
            ncfg.vtable           = &kVibratoVtable;
            ncfg.pInputChannels   = kVibratoChannels;
            ncfg.pOutputChannels  = kVibratoChannels;
            if (ma_node_init(ma_engine_get_node_graph(&im.engine), &ncfg,
                             nullptr, &im.vibrato.base) == MA_SUCCESS) {
                ma_node_attach_output_bus(
                    &im.vibrato.base, 0,
                    im.flangerUp
                        ? &im.flanger.base
                        : reinterpret_cast<ma_node*>(
                              ma_engine_get_endpoint(&im.engine)),
                    0);
                im.vibratoUp = true;
            } else {
                // Presentation-only: a failed node just means no audible
                // whammy this session; nothing else may depend on it.
                spdlog::warn("[audio] whammy vibrato node init failed");
            }
        }

        ma_resampler_config rcfg = ma_resampler_config_init(
            ma_format_f32, 2, 48000, 48000, ma_resample_algorithm_linear);
        rcfg.linear.lpfOrder = 0;  // exact pass-through at ratio 1.0
        if (ma_resampler_init(&rcfg, nullptr, &im.resampler) != MA_SUCCESS) {
            spdlog::error("[audio] ma_resampler_init failed");
            Uninit();
            return false;
        }
        im.resamplerUp = true;

        ma_device_config dcfg  = ma_device_config_init(ma_device_type_playback);
        dcfg.playback.format   = ma_format_f32;
        dcfg.playback.channels = 2;
        dcfg.sampleRate        = 48000;
        dcfg.dataCallback         = Impl::DataCallback;
        dcfg.notificationCallback = Impl::OnNotification;
        dcfg.pUserData            = &im;
        // WASAPI SHARED is miniaudio's default (M0-proven coexistence)
        if (ma_device_init(nullptr, &dcfg, &im.device) != MA_SUCCESS) {
            spdlog::error("[audio] ma_device_init failed");
            Uninit();
            return false;
        }
        im.deviceUp = true;
        QpcSec();  // prime the magic-static freq guard off the audio thread (Spike2 precedent)
        if (ma_device_start(&im.device) != MA_SUCCESS) {
            spdlog::error("[audio] ma_device_start failed");
            Uninit();
            return false;
        }
        spdlog::info("[audio] device up: {} Hz, period {} frames",
                     im.device.sampleRate,
                     im.device.playback.internalPeriodSizeInFrames);
        return true;
    }

    void AudioEngine::Uninit() {
        auto& im = *_impl;
        if (im.deviceUp) ma_device_uninit(&im.device);       // stops callback
        for (std::size_t i = 0; i < im.loaded; ++i)
            ma_sound_uninit(&im.sounds[i]);
        im.sounds.clear();
        im.names.clear();
        im.loaded = 0;
        for (std::size_t i = 0; i < im.sfxLoaded; ++i)
            ma_sound_uninit(&im.sfx[i]);
        im.sfx.clear();
        im.sfxLoaded = 0;
        im.sfxNext.store(0);
        for (std::size_t i = 0; i < im.crowdOk.size(); ++i) {
            if (im.crowdOk[i]) ma_sound_uninit(&im.crowd[i]);
        }
        im.crowd.clear();
        im.crowdOk.clear();
        for (std::size_t i = 0; i < im.uiOk.size(); ++i) {
            if (im.uiOk[i]) ma_sound_uninit(&im.ui[i]);
        }
        im.ui.clear();
        im.uiOk.clear();
        if (im.vibratoUp) {
            // after the sounds (they feed it), before the engine graph
            ma_node_uninit(&im.vibrato.base, nullptr);
            im.vibratoUp = false;
        }
        if (im.resamplerUp) ma_resampler_uninit(&im.resampler, nullptr);
        if (im.engineUp) ma_engine_uninit(&im.engine);
        // RM must outlive the engine that borrows it: uninit it AFTER.
        if (im.rmUp) ma_resource_manager_uninit(&im.resourceManager);
        im.deviceUp = im.resamplerUp = im.engineUp = im.rmUp = false;
        im.framesConsumed = 0;
        im.lastRate = 1.0;  // a re-Init starts a fresh resampler at ratio 1.0
        im.paused.store(false);
        im.targetRate.store(1.0);
    }

    bool AudioEngine::Ready() const { return _impl->deviceUp; }

    int AudioEngine::LoadStems(
        const std::map<std::string, std::filesystem::path>& stems,
        bool a_spatialize) {
        auto& im = *_impl;
        // Re-entry: unload any prior load first - a second call would leak
        // decoders and overwrite live ma_sound nodes still attached to the
        // engine graph.
        for (std::size_t i = 0; i < im.loaded; ++i)
            ma_sound_uninit(&im.sounds[i]);
        im.names.clear();
        im.loaded = 0;
        im.sounds.resize(stems.size());  // fixed BEFORE any init (no moves)
        im.spatial = a_spatialize;
        const ma_uint32 flags =
            a_spatialize ? 0u : MA_SOUND_FLAG_NO_SPATIALIZATION;
        for (const auto& [name, path] : stems) {
            if (name == "preview") continue;
            // BA compatibility charts are visible immediately with an empty
            // managed song.opus placeholder. Resolve only the selected song
            // here, immediately before miniaudio opens it; subsequent plays
            // take the fast non-empty-cache path.
            std::string baError;
            if (!EnsureInstalledBaSongAudio(path.parent_path(), {}, &baError)) {
                spdlog::warn("[audio] lazy BA audio FAILED: {} ({})",
                             baError, PathUtf8(path));
                continue;
            }
            // _w variant: CH song folders routinely have non-ANSI names
            if (ma_sound_init_from_file_w(&im.engine, path.wstring().c_str(),
                                          flags, nullptr, nullptr,
                                          &im.sounds[im.loaded]) != MA_SUCCESS) {
                spdlog::warn("[audio] stem load FAILED: {} ({})", name,
                             PathUtf8(path));
                continue;
            }
            ma_sound_set_volume(&im.sounds[im.loaded],
                                im.songVolume.load(std::memory_order_relaxed));
            if (a_spatialize) {
                auto& s = im.sounds[im.loaded];
                // PANNED, NEVER DUCKED (field 2026-07-26: "the farther the
                // camera is the less loud the sound is").
                //
                // The chart audio is not scenery - it is the CLOCK the
                // player is being judged against. Loudness that depends on
                // where the camera happens to be makes a song harder to play
                // for a reason that has nothing to do with playing it, and
                // the incoming camera director makes that structural rather
                // than occasional: wide and behind-the-band shots are the
                // whole point of it, and every one of them would pull the
                // listener past any distance threshold we picked.
                //
                // The lever is ROLLOFF ZERO, and it has to be. Read
                // ma_attenuation_inverse (miniaudio.h): gain is
                //     minDistance / (minDistance + rolloff * (d - minDistance))
                // so a rolloff of 0 is identically 1.0 at every distance,
                // while the spatializer itself keeps running and keeps
                // panning the song around the room.
                //
                // ⚠ NOT ma_attenuation_model_none, which is the obvious
                // reading of the name and is wrong: it takes an early-out
                // that skips the spatializer altogether and just channel-
                // converts (miniaudio.h, "If we're not spatializing we need
                // to run an optimized path"), so it is exactly equivalent to
                // MA_SOUND_FLAG_NO_SPATIALIZATION - the flat sound rejected
                // in the field on 2026-07-20 as "clearly raw straight to the
                // speakers". Its own enum comment says "no distance
                // attenuation AND no spatialization"; only the first half is
                // wanted here.
                ma_sound_set_attenuation_model(&s,
                                               ma_attenuation_model_inverse);
                ma_sound_set_min_distance(&s, kMinDistUnits);
                ma_sound_set_max_distance(&s, kMaxDistUnits);
                ma_sound_set_rolloff(&s, 0.0f);
            }
            im.names.push_back(name);
            ++im.loaded;
        }
        // Route the guitar stem through the whammy vibrato. Idle depth is
        // a bit-exact dry path, so non-whammy playback is unchanged.
        im.vibrato.target.store(0.0f, std::memory_order_relaxed);
        if (im.vibratoUp) {
            for (std::size_t i = 0; i < im.loaded; ++i) {
                if (im.names[i] == "guitar") {
                    ma_node_attach_output_bus(
                        &im.sounds[i], 0, &im.vibrato.base, 0);
                    spdlog::info(
                        "[audio] whammy vibrato attached to guitar stem");
                    break;
                }
            }
        }
        // Route INSTRUMENT stems through the SP flanger (guitar already
        // rides vibrato -> flanger). The backing/vocal "song" stem stays
        // on the endpoint in multi-stem songs - field 2026-07-26: a
        // flanged voice reads as broken audio, not as star power - but a
        // single-stem song routes fully: there the whole track IS the
        // instrument (the miss-mute rule).
        if (im.flangerUp) {
            int routed = 0;
            for (std::size_t i = 0; i < im.loaded; ++i) {
                if (im.names[i] == "guitar" && im.vibratoUp) continue;
                if (im.names[i] == "song" && im.loaded > 1 &&
                    !im.flangeSongStem) {
                    continue;
                }
                ma_node_attach_output_bus(
                    &im.sounds[i], 0, &im.flanger.base, 0);
                ++routed;
            }
            spdlog::info(
                "[audio] sp flanger scope: {} stem(s) routed directly, "
                "guitar via vibrato, song stem {}",
                routed,
                (im.loaded > 1 && !im.flangeSongStem) ? "clean" : "routed");
        }
        spdlog::info("[audio] {} stems loaded ({})", im.loaded,
                     a_spatialize ? "world-positioned" : "direct to master");
        return static_cast<int>(im.loaded);
    }

    void AudioEngine::SetWhammy(bool a_engaged) {
        _impl->vibrato.target.store(a_engaged ? 1.0f : 0.0f,
                                    std::memory_order_relaxed);
    }

    void AudioEngine::SetSpFilter(bool a_active) {
        if (!_impl->flangerUp) return;  // no node = no effect, no lying log
        _impl->flanger.dsp.SetActive(a_active);
        // Field evidence: one line per engage/release edge. The caller is
        // the session tick, so the edge check needs no synchronization.
        if (a_active != _impl->spFilterLast) {
            _impl->spFilterLast = a_active;
            spdlog::info("[audio] sp filter {}",
                         a_active ? "engaged" : "released");
        }
    }

    void AudioEngine::SetSpFilterParams(const SpFlangerParams& a_params,
                                        bool a_flangeSongStem) {
        if (!_impl->flangerUp) return;
        _impl->flanger.dsp.Configure(a_params);
        // Routing policy consumed by the NEXT LoadStems call - the caller
        // pushes params between Init and LoadStems.
        _impl->flangeSongStem = a_flangeSongStem;
        // One line per session so a field log shows which tuning actually
        // loaded (the LIVE INI wins over dist; this is how you catch a
        // stale or misspelled key).
        spdlog::info(
            "[audio] sp filter params: rate {:.2f}Hz delay {:.2f}+{:.2f}ms "
            "fb {:.2f} wet {:.2f}",
            a_params.rateHz, a_params.baseMs, a_params.depthMs,
            a_params.feedback, a_params.wet);
    }

    void AudioEngine::SetListener(float a_px, float a_py, float a_pz,
                                  float a_fx, float a_fy, float a_fz,
                                  float a_ux, float a_uy, float a_uz) {
        auto& im = *_impl;
        if (!im.engineUp || !im.spatial) { return; }
        ma_engine_listener_set_position(&im.engine, 0, a_px, a_py, a_pz);
        ma_engine_listener_set_direction(&im.engine, 0, a_fx, a_fy, a_fz);
        ma_engine_listener_set_world_up(&im.engine, 0, a_ux, a_uy, a_uz);
    }

    void AudioEngine::SetSongVolume(float a_volume) {
        auto& im     = *_impl;
        const float v = a_volume < 0.0f ? 0.0f : (a_volume > 1.0f ? 1.0f
                                                                  : a_volume);
        im.songVolume.store(v, std::memory_order_relaxed);
        for (std::size_t i = 0; i < im.loaded; ++i) {
            ma_sound_set_volume(&im.sounds[i], v);
        }
    }

    float AudioEngine::SongVolume() const {
        return _impl->songVolume.load(std::memory_order_relaxed);
    }

    void AudioEngine::SetSourcePosition(float a_x, float a_y, float a_z) {
        auto& im = *_impl;
        if (!im.engineUp || !im.spatial) { return; }
        for (std::size_t i = 0; i < im.loaded; ++i) {
            ma_sound_set_position(&im.sounds[i], a_x, a_y, a_z);
        }
    }

    std::uint64_t AudioEngine::ScheduleStart(double leadSeconds) {
        auto&      im = *_impl;
        const auto X  = ma_engine_get_time_in_pcm_frames(&im.engine) +
                       static_cast<std::uint64_t>(leadSeconds * kSampleRate);
        for (std::size_t i = 0; i < im.loaded; ++i) {
            ma_sound_set_start_time_in_pcm_frames(&im.sounds[i], X);
            ma_sound_start(&im.sounds[i]);
        }
        spdlog::info("[audio] {} stems scheduled @ engine frame {}", im.loaded,
                     X);
        return X;
    }

    std::uint64_t AudioEngine::SeekStems(double a_songSec) {
        auto&      im = *_impl;
        const auto songFrames =
            stem_seek::FramesForSongSec(a_songSec, kSampleRate);
        int failed = 0, restarted = 0;
        for (std::size_t i = 0; i < im.loaded; ++i) {
            auto& snd = im.sounds[i];
            // The result was previously DISCARDED, which made a failing seek
            // completely silent - both literally and in the log. A decoder
            // that cannot seek backwards leaves the stem wherever it was and
            // nothing anywhere said so.
            if (ma_sound_seek_to_pcm_frame(&snd, songFrames) != MA_SUCCESS) {
                ++failed;
                spdlog::warn(
                    "[audio] stem '{}' SEEK FAILED to frame {} - it will not "
                    "play for the rest of this run",
                    im.names[i], songFrames);
                continue;
            }
            // A stem that reached the END of its data was auto-stopped by
            // miniaudio, and seeking backwards does NOT restart it: the read
            // cursor moves but the sound stays stopped, so the next practice
            // loop plays in silence.
            //
            // WHOLE-SONG practice hits this on every single restart, because
            // that range runs to the end of the audio by definition - and
            // whole song is the common case, since most charts carry no
            // section markers. It is not specific to any one decoder.
            if (!ma_sound_is_playing(&snd)) {
                // ScheduleStart pinned a start time that is now long past.
                // Clear it, or the restart sits waiting on a deadline that
                // has already gone by.
                ma_sound_set_start_time_in_pcm_frames(&snd, 0);
                if (ma_sound_start(&snd) == MA_SUCCESS) { ++restarted; }
            }
        }
        // Read engine time AFTER the seek: the caller has paused, so the
        // counter is frozen either way, but this is the moment the stems
        // actually sit at songFrames. The arithmetic lives in
        // StemSeekLogic.h so the underflow rules are the tested ones
        // (test_stemseek) rather than a second copy here.
        const auto X = stem_seek::StartFrameForSeek(
            ma_engine_get_time_in_pcm_frames(&im.engine), songFrames);
        spdlog::info(
            "[audio] {} stems seeked to {:.3f}s (song frame {}), song 0 @ "
            "engine frame {} - {} restarted, {} failed",
            im.loaded, a_songSec, songFrames, X, restarted, failed);
        return X;
    }

    void AudioEngine::SetPaused(bool paused) {
        _impl->paused.store(paused, std::memory_order_release);
    }
    void AudioEngine::SetRate(double ratio) {
        _impl->targetRate.store(ratio, std::memory_order_release);
    }

    void AudioEngine::SetSongSpeed(double a_speed) {
        auto&        im    = *_impl;
        const double speed = a_speed > 0.0 ? a_speed : 1.0;
        if (speed != 1.0 &&
            !im.stretchReady.load(std::memory_order_acquire)) {
            // presetDefault ALLOCATES, which is why this function is session
            // thread only. Arming it here is what lets the audio callback
            // treat the stretcher as ready-made and never allocate.
            im.stretch.presetDefault(2, static_cast<float>(kSampleRate));
            im.stretch.reset();
            // Release AFTER the setup above: the callback's acquire load is
            // what keeps it out of a half-configured stretcher.
            im.stretchReady.store(true, std::memory_order_release);
            spdlog::info(
                "[audio] time-stretch armed: block={} interval={} latency "
                "in={} out={}",
                im.stretch.blockSamples(), im.stretch.intervalSamples(),
                im.stretch.inputLatency(), im.stretch.outputLatency());
        }
        im.songSpeed.store(speed, std::memory_order_release);
        spdlog::info("[audio] song speed -> {:.2f}x (pitch preserved)", speed);
    }

    double AudioEngine::CallbackPeriodSec() const {
        const auto& im = *_impl;
        if (!im.deviceUp || im.device.sampleRate == 0) return 0.010;
        return static_cast<double>(
                   im.device.playback.internalPeriodSizeInFrames) /
               im.device.sampleRate;
    }

    int AudioEngine::FindStem(std::string_view name) const {
        auto& im = *_impl;
        for (std::size_t i = 0; i < im.loaded; ++i) {
            if (im.names[i] == name) return static_cast<int>(i);
        }
        return -1;
    }

    int AudioEngine::LoadMissSfx(
        const std::vector<std::filesystem::path>& files, float volume) {
        auto& im = *_impl;
        for (std::size_t i = 0; i < im.sfxLoaded; ++i)
            ma_sound_uninit(&im.sfx[i]);
        im.sfxLoaded = 0;
        im.sfx.resize(files.size());  // fixed BEFORE any init (no moves)
        for (const auto& path : files) {
            // DECODE: tiny files, and the trigger must never touch disk
            if (ma_sound_init_from_file_w(
                    &im.engine, path.wstring().c_str(),
                    MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
                    nullptr, nullptr, &im.sfx[im.sfxLoaded]) != MA_SUCCESS) {
                spdlog::warn("[audio] miss sfx load FAILED: {}",
                             PathUtf8(path));
                continue;
            }
            ma_sound_set_volume(&im.sfx[im.sfxLoaded], volume);
            ++im.sfxLoaded;
        }
        spdlog::info("[audio] {} miss sfx loaded (vol {:.2f})", im.sfxLoaded,
                     volume);
        return static_cast<int>(im.sfxLoaded);
    }

    void AudioEngine::PlayMissSfx() {
        auto& im = *_impl;
        if (im.sfxLoaded == 0) return;
        const auto i = im.sfxNext.fetch_add(1) % im.sfxLoaded;
        ma_sound_seek_to_pcm_frame(&im.sfx[i], 0);
        ma_sound_start(&im.sfx[i]);
    }

    int AudioEngine::LoadCrowdSfx(
        const std::vector<std::filesystem::path>& files, float volume) {
        auto& im = *_impl;
        for (std::size_t i = 0; i < im.crowdOk.size(); ++i) {
            if (im.crowdOk[i]) ma_sound_uninit(&im.crowd[i]);
        }
        im.crowdOk.assign(files.size(), 0);
        im.crowd.resize(files.size());  // fixed BEFORE any init (no moves)
        int loaded = 0;
        for (std::size_t i = 0; i < files.size(); ++i) {
            // DECODE: tiny files, and the trigger must never touch disk
            if (ma_sound_init_from_file_w(
                    &im.engine, files[i].wstring().c_str(),
                    MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
                    nullptr, nullptr, &im.crowd[i]) != MA_SUCCESS) {
                // A missing reaction costs only ITSELF: the slot stays empty,
                // the rest of the bank still plays, and the session carries
                // on. Do NOT compact here (see the header) - index i must
                // keep belonging to files[i].
                spdlog::warn("[audio] crowd sfx load FAILED: {}",
                             PathUtf8(files[i]));
                continue;
            }
            ma_sound_set_volume(&im.crowd[i], volume);
            im.crowdOk[i] = 1;
            ++loaded;
        }
        spdlog::info("[audio] {}/{} crowd sfx loaded (vol {:.2f})", loaded,
                     files.size(), volume);
        return loaded;
    }

    void AudioEngine::PlayCrowdSfx(int a_index) {
        auto& im = *_impl;
        if (a_index < 0 ||
            static_cast<std::size_t>(a_index) >= im.crowdOk.size() ||
            !im.crowdOk[a_index]) {
            return;  // out of range, or that file never loaded
        }
        ma_sound_seek_to_pcm_frame(&im.crowd[a_index], 0);
        ma_sound_start(&im.crowd[a_index]);
    }

    int AudioEngine::LoadUiSfx(
        const std::vector<std::filesystem::path>& files, float volume) {
        auto& im = *_impl;
        for (std::size_t i = 0; i < im.uiOk.size(); ++i) {
            if (im.uiOk[i]) ma_sound_uninit(&im.ui[i]);
        }
        im.uiOk.assign(files.size(), 0);
        im.ui.resize(files.size());  // fixed BEFORE any init (no moves)
        int loaded = 0;
        for (std::size_t i = 0; i < files.size(); ++i) {
            // DECODE: tiny files, and the trigger must never touch disk
            if (ma_sound_init_from_file_w(
                    &im.engine, files[i].wstring().c_str(),
                    MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
                    nullptr, nullptr, &im.ui[i]) != MA_SUCCESS) {
                // Slot stays EMPTY, never compacted - index i must keep
                // belonging to files[i] (same rule as the crowd bank).
                spdlog::warn("[audio] ui sfx load FAILED: {}",
                             PathUtf8(files[i]));
                continue;
            }
            ma_sound_set_volume(&im.ui[i], volume);
            im.uiOk[i] = 1;
            ++loaded;
        }
        spdlog::info("[audio] {}/{} ui sfx loaded (vol {:.2f})", loaded,
                     files.size(), volume);
        return loaded;
    }

    void AudioEngine::PlayUiSfx(int a_index) {
        auto& im = *_impl;
        if (a_index < 0 ||
            static_cast<std::size_t>(a_index) >= im.uiOk.size() ||
            !im.uiOk[a_index]) {
            return;  // out of range, or that file never loaded
        }
        ma_sound_seek_to_pcm_frame(&im.ui[a_index], 0);
        ma_sound_start(&im.ui[a_index]);
    }

    void AudioEngine::StartUiLoop(int a_index) {
        auto& im = *_impl;
        if (a_index < 0 ||
            static_cast<std::size_t>(a_index) >= im.uiOk.size() ||
            !im.uiOk[a_index]) {
            return;
        }
        auto& s = im.ui[a_index];
        // Undo any previous fade-stop IN THIS ORDER: cancel the armed
        // stop time, then force the fader back to 1.0 (a settled fader
        // never disarms itself - explicit start volume, not -1, so the
        // fader is SET rather than continued from its parked 0), then
        // rewind and go. ma_sound_set_volume from load stays untouched:
        // volume is the level knob, the fader is the transient - they
        // multiply, and confusing them buries the sound (see FadeStem).
        ma_sound_set_stop_time_in_pcm_frames(&s, (ma_uint64)-1);
        ma_sound_set_fade_in_milliseconds(&s, 1.0f, 1.0f, 0);
        ma_sound_set_looping(&s, MA_TRUE);
        ma_sound_seek_to_pcm_frame(&s, 0);
        ma_sound_start(&s);
    }

    void AudioEngine::SetUiSfxVolume(float volume) {
        auto& im = *_impl;
        for (std::size_t i = 0; i < im.uiOk.size(); ++i) {
            if (im.uiOk[i]) ma_sound_set_volume(&im.ui[i], volume);
        }
    }

    void AudioEngine::SetUiSfxSlotVolume(int a_index, float volume) {
        auto& im = *_impl;
        if (a_index < 0 ||
            static_cast<std::size_t>(a_index) >= im.uiOk.size() ||
            !im.uiOk[a_index]) {
            return;
        }
        ma_sound_set_volume(&im.ui[a_index], volume);
    }

    void AudioEngine::StopUiLoop(int a_index) {
        auto& im = *_impl;
        if (a_index < 0 ||
            static_cast<std::size_t>(a_index) >= im.uiOk.size() ||
            !im.uiOk[a_index]) {
            return;
        }
        // Clickless: the tick grid is 60ms, so a hard stop can land
        // mid-tick. 40ms rides under one grid cell and overwrites any
        // scheduled stop (miniaudio doc on this exact call).
        ma_sound_stop_with_fade_in_milliseconds(&im.ui[a_index], 40);
    }

    void AudioEngine::FadeStem(int index, float targetVolume,
                               unsigned fadeMs) {
        auto& im = *_impl;
        if (index < 0 || static_cast<std::size_t>(index) >= im.loaded) return;
        // -1 start volume = "from current" (click-free either direction)
        ma_sound_set_fade_in_milliseconds(&im.sounds[index], -1.0f,
                                          targetVolume, fadeMs);
    }

    double AudioEngine::MaxStemLengthSec() const {
        auto&  im  = *_impl;
        double max = 0.0;
        for (std::size_t i = 0; i < im.loaded; ++i) {
            ma_uint64 frames = 0;
            if (ma_sound_get_length_in_pcm_frames(&im.sounds[i], &frames) ==
                MA_SUCCESS) {
                max = std::max(max, static_cast<double>(frames) / kSampleRate);
            }
        }
        return max;
    }

    bard::AudioAnchor AudioEngine::ReadAnchor() const {
        return _impl->anchor.Read();
    }
    bool AudioEngine::AnchorPublished() const {
        return _impl->anchor.Published();
    }
    std::uint32_t AudioEngine::DeviceState() const {
        return static_cast<std::uint32_t>(
            ma_device_get_state(&_impl->device));
    }
}
