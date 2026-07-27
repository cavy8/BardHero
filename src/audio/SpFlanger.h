// src/audio/SpFlanger.h
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace SH {

    // GH-feel: the Star Power flanger. While SP is active the WHOLE song
    // mix runs through a swept comb - the Guitar Hero sound. Full-mix on
    // purpose: per-stem was measured near inaudible on real material (the
    // test song's guitar stem is ~25% of the mix energy with 99% of it
    // below 2 kHz), and the user picked the "classic" variant from
    // full-mix previews. Reference implementation and the audition
    // renders: BardHero Electric tools/proto_sp_flanger.py -
    // test_spflanger cross-validates this against checkpoints generated
    // from that reference, so the two must be changed together.
    //
    // Defaults are the "gentle" tuning (field 2026-07-26; the spec's
    // "classic" row was voiced for full-mix scope and played too strong
    // on the instrument stems). Settings overrides these per session -
    // keep the three default sites in step: here, Settings.h, dist INI.
    struct SpFlangerParams {
        float rateHz      = 0.40f;   // LFO sweep rate
        float baseMs      = 0.5f;    // minimum delay
        float depthMs     = 2.0f;    // sweep width above the minimum
        float feedback    = 0.35f;   // clamped to < 0.75 (loop stability)
        float wet         = 0.40f;   // wet mix at full engagement
        float stereoPhase = 0.25f;   // right-channel LFO offset, in cycles
        float trimDb      = -2.5f;   // ramped output trim (comb peaks +6 dB)
        float rampSec     = 0.100f;  // engage/disengage wet ramp
    };

    // Stereo-interleaved 48 kHz flanger with the engage/disengage ramp
    // built in. SetActive from any thread (one atomic); Configure only
    // while idle (the audio thread reads the plain param fields on the
    // non-idle path only, and the session configures before ScheduleStart
    // arms SP). Process on the audio thread; when fully idle it is a
    // true no-op that never touches the buffer, so inactive playback is
    // bit-exact by construction and costs nothing.
    class SpFlanger {
    public:
        static constexpr double kSampleRate = 48000.0;
        // pow2 ring; must exceed the worst delay (base+depth, spec caps at
        // 6 ms = 288 samples) plus the interpolation neighbour.
        static constexpr std::uint32_t kRing = 512;
        static constexpr std::uint32_t kMask = kRing - 1;

        void Configure(const SpFlangerParams& a_params) {
            _p = a_params;
            if (_p.feedback > 0.74f) _p.feedback = 0.74f;
            if (_p.feedback < 0.0f) _p.feedback = 0.0f;
            if (_p.rampSec < 0.005f) _p.rampSec = 0.005f;
            const float maxDelayMs =
                (kRing - 4) * 1000.0f / static_cast<float>(kSampleRate);
            if (_p.baseMs < 0.05f) _p.baseMs = 0.05f;
            if (_p.baseMs + _p.depthMs > maxDelayMs)
                _p.depthMs = maxDelayMs - _p.baseMs;
            if (_p.depthMs < 0.0f) _p.depthMs = 0.0f;
            Reset();
        }

        void SetActive(bool a_active) {
            _target.store(a_active, std::memory_order_relaxed);
        }

        bool Idle() const { return _idle; }

        void Process(float* a_frames, std::uint32_t a_frameCount) {
            const bool want = _target.load(std::memory_order_relaxed);
            if (_idle) {
                if (!want) return;  // the bit-exact bypass: never touched
                _idle = false;
            }
            const double step   = 1.0 / (_p.rampSec * kSampleRate);
            const double lfoInc = 2.0 * kPi * _p.rateHz / kSampleRate;
            const double offR   = 2.0 * kPi * _p.stereoPhase;
            // trim = 10^(trimDb/20 * wetNorm), evaluated as exp
            const double trimLn = _p.trimDb / 20.0 * kLn10;
            for (std::uint32_t f = 0; f < a_frameCount; ++f) {
                if (want) {
                    _wetNorm += step;
                    if (_wetNorm > 1.0) _wetNorm = 1.0;
                } else {
                    _wetNorm -= step;
                    if (_wetNorm <= 0.0) {
                        // Disengage settled: park idle so the NEXT
                        // activation starts from the same silence the
                        // reference does (empty ring, phase zero).
                        Reset();
                        return;  // remaining frames stay untouched = dry
                    }
                }
                const double wetGain = _p.wet * _wetNorm;
                const float  trim =
                    static_cast<float>(std::exp(trimLn * _wetNorm));
                for (int c = 0; c < 2; ++c) {
                    const double d =
                        (_p.baseMs +
                         _p.depthMs *
                             (0.5 + 0.5 * std::sin(_lfoPhase +
                                                   (c ? offR : 0.0)))) *
                        kSampleRate / 1000.0;
                    const float x = a_frames[f * 2 + c];
                    const float w = x + _p.feedback * _z[c];
                    _ring[c][_cursor & kMask] = w;
                    // Index math in double: a float cursor loses sample
                    // precision within minutes at 48 kHz.
                    const double rp = static_cast<double>(_cursor) - d;
                    float        z  = 0.0f;
                    if (rp > 0.0) {
                        const double fl   = std::floor(rp);
                        const float  frac = static_cast<float>(rp - fl);
                        const auto   j =
                            static_cast<std::uint64_t>(fl);
                        const float  z0 =
                            _ring[c][static_cast<std::uint32_t>(j) & kMask];
                        const float z1 =
                            _ring[c][static_cast<std::uint32_t>(j + 1) &
                                     kMask];
                        z = z0 + (z1 - z0) * frac;
                    }
                    a_frames[f * 2 + c] =
                        (x + static_cast<float>(wetGain) * z) * trim;
                    _z[c] = z;
                }
                _lfoPhase += lfoInc;
                ++_cursor;
            }
        }

    private:
        static constexpr double kPi   = 3.14159265358979323846;
        static constexpr double kLn10 = 2.30258509299404568402;

        void Reset() {
            for (auto& r : _ring)
                for (auto& s : r) s = 0.0f;
            _z[0] = _z[1] = 0.0f;
            _lfoPhase     = 0.0;
            _wetNorm      = 0.0;
            _cursor       = 0;
            _idle         = true;
        }

        SpFlangerParams   _p{};
        std::atomic<bool> _target{ false };
        // audio-thread state
        float         _ring[2][kRing]{};
        float         _z[2]{};
        double        _lfoPhase = 0.0;
        double        _wetNorm  = 0.0;
        std::uint64_t _cursor   = 0;
        bool          _idle     = true;
    };
}
