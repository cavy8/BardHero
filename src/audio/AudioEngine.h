// src/audio/AudioEngine.h
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "audio/SpFlanger.h"
#include "clock/AnchorSeqlock.h"

namespace SH {

    // miniaudio WASAPI-shared audio host (spec 8), production shape proven
    // by SPIKE-2: ma_engine with noDevice=TRUE + our own ma_device whose
    // data callback reads the engine THROUGH a master ma_resampler (the
    // slave controller's rate nudge - one resampler keeps every stem
    // sample-locked) and publishes the {frames, qpc, rate} anchor via
    // seqlock. All control methods are session-thread-only unless noted.
    class AudioEngine {
    public:
        static constexpr double kSampleRate = 48000.0;

        AudioEngine();
        ~AudioEngine();

        bool Init();    // caller thread must already be COM MTA (spec 8)
        void Uninit();
        bool Ready() const;

        // Loads every reserved stem except "preview" (v1 plays everything,
        // spec 4.1/8). Returns the number loaded (0 = fail the session).
        // A repeat call unloads the previous stems first.
        //
        // a_spatialize puts the stems through the engine's 3D spatializer
        // instead of straight to the master mix, so the song attenuates and
        // pans as a source standing where the player is (field 2026-07-20:
        // "it's clearly raw straight to the speakers"). ⚠ The spatializer
        // is a POINT source, so a stereo stem collapses toward mono - that
        // is correct for a diegetic instrument but it is a taste call,
        // which is why it is a setting.
        int LoadStems(const std::map<std::string, std::filesystem::path>& stems,
                      bool a_spatialize = false);

        // World-audio placement, all in SKYRIM units. Any thread: these are
        // plain float writes the mixer reads, no allocation and no locking.
        // No-ops when the stems were loaded unspatialized.
        //
        // The listener is the CAMERA, not the player - in third person the
        // camera orbits several hundred units away, and that offset is most
        // of what makes the instrument read as being out in the world.
        void SetListener(float a_px, float a_py, float a_pz, float a_fx,
                         float a_fy, float a_fz, float a_ux, float a_uy,
                         float a_uz);
        void SetSourcePosition(float a_x, float a_y, float a_z);

        // Base playback level for every song stem, 0..1. Any thread, and
        // safe mid-song - the settings slider applies live.
        //
        // This sets the SOUND's volume (ma_sound_set_volume), which is a
        // different multiplicative stage from the fader FadeStem drives.
        // The miss-mute fade must therefore restore the fader to 1.0 and
        // NOT to SongVolume(): restoring it to SongVolume() multiplies the
        // level in twice and buries the song. Setting this mid-song does
        // not disturb a settled fader, so the slider stays live.
        void  SetSongVolume(float a_volume);
        float SongVolume() const;

        // Schedules every loaded stem at engine frame X = now + leadSeconds
        // (sample-synchronized start, spec 8) and returns X.
        std::uint64_t ScheduleStart(double leadSeconds);

        // Seek every loaded song stem to a_songSec and return the engine
        // frame at which song position 0 now plays, for
        // AnchorEstimator::SetStartFrame. Practice mode's loop is a
        // BACKWARDS seek, which is why this exists at all.
        //
        // Session-thread-only, and thin by design: no state and no locking
        // of its own. The caller must already have paused audio and
        // disengaged the engine feed - while paused the data callback skips
        // ma_engine_read_pcm_frames, so engine time is frozen and the
        // returned frame cannot go stale between the seek and
        // SetStartFrame. The caller must ALSO call SetStartFrame with the
        // result and not merely note it: AnchorEstimator's monotonic clamp
        // otherwise pins Position at the pre-seek value forever, and
        // SetStartFrame is what clears it (AnchorEstimator.h ResetMonotonic).
        //
        // Negative a_songSec clamps to 0 (see StemSeekLogic.h) - a practice
        // range starting inside the first two seconds resolves negative.
        std::uint64_t SeekStems(double a_songSec);

        void   SetPaused(bool paused);  // any thread: callback emits silence
        // Any thread: resampler ratio. Ratio is trusted unclamped; the
        // caller (SlaveController) owns the +/-5% policy.
        void   SetRate(double ratio);

        // Practice playback speed, PITCH PRESERVED (plan P6). SESSION THREAD
        // ONLY: it allocates the stretcher on first non-unity use, which must
        // never happen on the audio thread.
        //
        // Deliberately NOT SetRate. SetRate is the resampler nudge the
        // SlaveController owns under a +/-5% drift policy, and it changes
        // PITCH; driving practice speed through it would both detune the song
        // and fight the drift corrector over one knob. Speed routes through
        // the vendored signalsmith time-stretch instead, and at exactly 1.0
        // the stretcher is bypassed entirely so ordinary performance keeps
        // the original, field-proven code path untouched.
        void   SetSongSpeed(double a_speed);

        double CallbackPeriodSec() const;
        double MaxStemLengthSec() const;

        // M4 miss feedback. FindStem: index of a loaded stem by reserved
        // name, -1 if absent (call after LoadStems, any thread - the table
        // is immutable between LoadStems calls). FadeStem: any thread;
        // miniaudio fade parameters are mixer-read atomics.
        //
        // targetVolume drives the engine node's FADER, which multiplies on
        // top of SetSongVolume, so 1.0 here means "fader open" and not
        // "full loudness". A settled fader never disarms itself either: it
        // keeps applying its end volume forever, so any target below 1.0
        // is a permanent attenuation, not a transient one.
        int  FindStem(std::string_view name) const;
        void FadeStem(int index, float targetVolume, unsigned fadeMs);

        // Whammy pitch bend on the guitar stem (GH-feel spec P4). Any
        // thread: one atomic write; the audio thread smooths engagement
        // (~35ms in / ~105ms out) so toggling is clickless. Presentation
        // only - the engine's SP whammy rule is independent of this.
        void SetWhammy(bool engaged);

        // Star Power flanger (GH SP sound; spec BardHero Electric
        // docs/request-to-main-2026-07-25-sp-filter.md). A node in the
        // engine graph, scoped to INSTRUMENT stems: guitar rides
        // vibrato -> flanger, bass/drums route directly, and the
        // backing/vocal "song" stem stays clean in multi-stem songs
        // (field 2026-07-26: a flanged voice sounds broken, not
        // powered). A single-stem song routes fully - there the whole
        // track IS the instrument, the same rule miss-mute uses. Node
        // processing happens at the engine's 48 kHz, before the master
        // resampler, so the ms-domain sweep is exact under rate nudges;
        // UI SFX and crowd live in their own engines and are never
        // touched. SetSpFilter from any thread (one atomic; the audio
        // thread ramps the wet over ~100ms both ways, and a settled-off
        // filter is a true no-op with a bit-exact dry path).
        // SetSpFilterParams between Init and LoadStems - LoadStems
        // consumes the routing flag.
        void SetSpFilter(bool a_active);
        void SetSpFilterParams(const SpFlangerParams& a_params,
                               bool a_flangeSongStem = false);

        // Miss SFX one-shots (preloaded, decoded). Load on the session
        // thread after Init (before the feed publishes the AudioEngine
        // pointer); Play from any thread - round-robin variant, restart
        // from 0 (same immutable-after-load contract as FadeStem).
        int  LoadMissSfx(const std::vector<std::filesystem::path>& files,
                         float volume);
        void PlayMissSfx();

        // Crowd reaction one-shots. Same load-once contract as the miss bank
        // above, but its owner is not the session: CrowdReactions keeps a
        // process-lifetime AudioEngine of its own and loads this bank into
        // it once, because the end-of-song reaction fires microseconds
        // before the SESSION's engine is uninitialised. Load on the thread
        // that Init'd that engine; Play from any thread.
        //
        // Played BY INDEX, not round-robin: each CrowdReactions::Kind owns a
        // fixed slot, so the caller's file order IS the index mapping. A file
        // that fails to load therefore leaves its slot EMPTY rather than
        // compacting the bank - compacting would silently shift every later
        // reaction onto the wrong sound. PlayCrowdSfx no-ops on an
        // out-of-range index or an empty slot, which is what lets the feature
        // ship before the real samples exist.
        int  LoadCrowdSfx(const std::vector<std::filesystem::path>& files,
                          float volume);
        void PlayCrowdSfx(int a_index);

        // UI SFX one-shots (P6 set). Same contract as the crowd bank in
        // every way that matters: load once on the owning thread, play
        // from any thread, BY INDEX with empty slots no-oping so a
        // missing file costs only itself. Owner is UiSfx.cpp, which keeps
        // a process-lifetime engine for the same teardown reason the
        // crowd does.
        //
        // The loop pair drives the one looped slot (score_tick).
        // StartUiLoop must undo a previous StopUiLoop before starting:
        // the fade-stop parks the sound's FADER at 0 and arms a stop
        // time, and a settled fader never disarms itself (see FadeStem),
        // so a bare restart would play silence forever.
        int  LoadUiSfx(const std::vector<std::filesystem::path>& files,
                       float volume);
        void PlayUiSfx(int a_index);
        void StartUiLoop(int a_index);
        void StopUiLoop(int a_index);
        // Re-applies the ONE common gain to every loaded UI slot. Any
        // thread; ma_sound_set_volume is a mixer-read atomic, so the
        // settings slider applies live. Required because this bank loads
        // ONCE per process (unlike the miss bank's per-session reload) -
        // without it a volume change would wait for a game restart.
        void SetUiSfxVolume(float volume);
        // Same, for ONE slot - the per-cue trim (ui_sfx::CueGainScale)
        // rides on top of the common gain. Out-of-range or empty slots
        // no-op, exactly like PlayUiSfx.
        void SetUiSfxSlotVolume(int a_index, float volume);

        bard::AudioAnchor ReadAnchor() const;  // any thread
        bool              AnchorPublished() const;

        // ma_device_state as int (0 uninit / 1 stopped / 2 started /
        // 3 starting / 4 stopping) - stale-anchor diagnostics, any thread
        std::uint32_t DeviceState() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
