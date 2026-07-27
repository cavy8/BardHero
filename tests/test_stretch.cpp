// Smoke test for the vendored signalsmith-stretch (extern/signalsmith).
// This is the pitch-preserving time-stretcher behind practice-mode playback
// speed. It proves the vendored headers compile under MSVC C++23 AND that the
// library actually stretches - it is deliberately NOT a DSP conformance suite,
// so every tolerance here is loose and no exact sample value is asserted.
//
// Two API facts this test pins down, because both are easy to get wrong when
// wiring the audio path later:
//   1. There is no time-ratio setter. The ratio is implicit in process():
//      outputSamples/inputSamples. Half speed = feed N in, ask for 2N out.
//   2. Buffers are PLANAR (inputs[channel][frame]), not interleaved.
#include "harness.h"
#include "signalsmith-stretch.h"

#include <cmath>
#include <vector>

namespace {

constexpr float kSampleRate = 44100.0f;
constexpr int   kChannels   = 2;
constexpr double kTau       = 6.28318530717958647692;

using Stretch = signalsmith::stretch::SignalsmithStretch<float>;

// Planar multi-channel buffer. operator[] hands back a float* so it satisfies
// the inputs[c][i] / outputs[c][i] access the library's templates require.
struct Planar {
    std::vector<std::vector<float>> ch;

    Planar(int channels, int frames) : ch(channels, std::vector<float>(frames, 0.0f)) {}

    float*       operator[](int c)       { return ch[c].data(); }
    const float* operator[](int c) const { return ch[c].data(); }
    int frames() const { return static_cast<int>(ch[0].size()); }
};

// A 440 Hz sine, comfortably above the library's internal noise floor so the
// silence-detection path is never what we end up measuring.
Planar MakeSine(int frames, double freqHz = 440.0) {
    Planar p(kChannels, frames);
    for (int c = 0; c < kChannels; ++c)
        for (int i = 0; i < frames; ++i)
            p[c][i] = static_cast<float>(
                0.5 * std::sin(kTau * freqHz * i / kSampleRate));
    return p;
}

bool AllFinite(const Planar& p) {
    for (const auto& c : p.ch)
        for (float s : c)
            if (!std::isfinite(s)) return false;
    return true;
}

// RMS over a frame range of channel 0. Used only to tell "audio" from
// "silence", never to assert a specific level.
double Rms(const Planar& p, int from, int to) {
    double acc = 0.0;
    for (int i = from; i < to; ++i) acc += double(p[0][i]) * double(p[0][i]);
    return std::sqrt(acc / double(to - from));
}

// Rising zero-crossings -> approximate fundamental. On a near-pure tone this
// is enough to tell a pitch-preserved stretch (still ~440 Hz) from naive
// varispeed (which would halve the pitch to ~220 Hz at half speed). That
// distinction is the entire reason this library is being vendored.
double ApproxHz(const Planar& p, int from, int to) {
    int crossings = 0;
    for (int i = from + 1; i < to; ++i)
        if (p[0][i - 1] <= 0.0f && p[0][i] > 0.0f) ++crossings;
    const double seconds = double(to - from) / kSampleRate;
    return crossings / seconds;
}

}  // namespace

static void RunTests() {
    // 1) Unity ratio: N frames in, N frames out. Output must be real audio,
    //    finite, and at roughly the input's level.
    {
        Stretch st;
        st.presetDefault(kChannels, kSampleRate);
        CHECK(st.blockSamples() > 0);
        CHECK(st.intervalSamples() > 0);

        constexpr int kN = 44100;  // 1 second
        const Planar in = MakeSine(kN);
        Planar out(kChannels, kN);
        st.process(in, kN, out, kN);

        CHECK(AllFinite(out));
        // Skip the algorithmic latency at the head - that region is legitimately
        // still filling and says nothing about whether the stretcher works.
        const int settled = st.inputLatency() + st.outputLatency();
        CHECK(settled < kN);
        const double rms = Rms(out, settled, kN);
        CHECK(rms > 0.05);   // non-silent
        CHECK(rms < 2.0);    // not blowing up
        // Unity ratio must not transpose.
        CHECK_NEAR(ApproxHz(out, settled, kN), 440.0, 60.0);
    }

    // 2) Half speed: N frames in produces 2N frames out. This is the actual
    //    time-stretch, and the output count really is double the input count.
    {
        Stretch st;
        st.presetDefault(kChannels, kSampleRate);

        constexpr int kIn  = 44100;
        constexpr int kOut = kIn * 2;
        const Planar in = MakeSine(kIn);
        Planar out(kChannels, kOut);
        st.process(in, kIn, out, kOut);

        CHECK(out.frames() == 2 * in.frames());
        CHECK(AllFinite(out));

        const int settled = st.inputLatency() + st.outputLatency();
        CHECK(settled < kOut);
        CHECK(Rms(out, settled, kOut) > 0.05);

        // The point of the whole exercise: slowed down, the pitch is UNCHANGED.
        // Varispeed would land near 220 Hz here and fail this outright.
        CHECK_NEAR(ApproxHz(out, settled, kOut), 440.0, 60.0);
    }

    // 3) Double speed: 2N frames in, N frames out - the other direction still
    //    holds together and stays finite.
    {
        Stretch st;
        st.presetDefault(kChannels, kSampleRate);

        constexpr int kIn  = 88200;
        constexpr int kOut = kIn / 2;
        const Planar in = MakeSine(kIn);
        Planar out(kChannels, kOut);
        st.process(in, kIn, out, kOut);

        CHECK(AllFinite(out));
        const int settled = st.inputLatency() + st.outputLatency();
        CHECK(settled < kOut);
        CHECK(Rms(out, settled, kOut) > 0.05);
        CHECK_NEAR(ApproxHz(out, settled, kOut), 440.0, 60.0);
    }

    // 4) reset() leaves the stretcher reusable: a second run after reset must
    //    behave like the first, not like a half-full pipeline. (Practice mode
    //    will reset on every seek/restart, so this path matters.)
    {
        Stretch st;
        st.presetDefault(kChannels, kSampleRate);

        constexpr int kN = 22050;
        const Planar in = MakeSine(kN);
        Planar first(kChannels, kN), second(kChannels, kN);

        st.process(in, kN, first, kN);
        st.reset();
        st.process(in, kN, second, kN);

        CHECK(AllFinite(first));
        CHECK(AllFinite(second));
        const int settled = st.inputLatency() + st.outputLatency();
        CHECK(settled < kN);
        const double a = Rms(first, settled, kN);
        const double b = Rms(second, settled, kN);
        CHECK(a > 0.05);
        CHECK(b > 0.05);
        // Same input, same state -> comparable energy. Loose: this only has to
        // catch "reset left the thing broken or silent".
        CHECK(b > a * 0.5 && b < a * 2.0);
    }

    // 5) Silence in stays finite (and quiet) - the noise-floor path must not
    //    emit NaN/Inf into the mix buffer.
    {
        Stretch st;
        st.presetDefault(kChannels, kSampleRate);

        constexpr int kN = 22050;
        const Planar in(kChannels, kN);  // all zeros
        Planar out(kChannels, kN * 2);
        st.process(in, kN, out, kN * 2);

        CHECK(AllFinite(out));
        CHECK(Rms(out, 0, kN * 2) < 0.01);
    }
}

TEST_MAIN("Stretch")
