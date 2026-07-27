// tests/test_stemseek.cpp
#include "audio/StemSeekLogic.h"

#include "harness.h"

#include <cstdint>
#include <limits>

using namespace SH::stem_seek;

// The engine's real rate (AudioEngine::kSampleRate). Spelled out here
// rather than included, because AudioEngine.h drags in miniaudio's world;
// if the engine's rate ever changes, this suite still proves the ARITHMETIC
// and AudioEngine.cpp is what supplies the live constant.
static constexpr double kRate = 48000.0;

static void FramesForSongSecTests() {
    // Zero is a real, reachable practice start: a range that begins in the
    // first two seconds clamps to 0 (PracticeRange.h kLeadInSec).
    CHECK(FramesForSongSec(0.0, kRate) == 0u);

    // Ordinary positive seek.
    CHECK(FramesForSongSec(1.0, kRate) == 48000u);
    CHECK(FramesForSongSec(30.0, kRate) == 1440000u);

    // Fractional seconds land on the truncated frame, matching
    // ScheduleStart's static_cast<std::uint64_t>(leadSeconds * kSampleRate).
    CHECK(FramesForSongSec(0.5, kRate) == 24000u);
    CHECK(FramesForSongSec(2.25, kRate) == 108000u);
    // 1/48000 s under a whole frame: truncates DOWN, never up.
    CHECK(FramesForSongSec(1.0 - 0.5 / kRate, kRate) == 47999u);

    // THE reason this is a separate function. A negative song time fed
    // straight into std::uint64_t frame math underflows to an astronomical
    // frame number and seeks the stems past the end of the universe.
    // -1.5s must be 0, not ~1.8e19.
    CHECK(FramesForSongSec(-1.5, kRate) == 0u);
    CHECK(FramesForSongSec(-0.001, kRate) == 0u);
    CHECK(FramesForSongSec(-1e9, kRate) == 0u);
    // Belt and braces: a wrapped value would be enormous, so also assert
    // the result sits in a sane band rather than only that it equals 0.
    CHECK(FramesForSongSec(-1.5, kRate) < 1000u);

    // Past the song end is NOT clamped here - this header does not know
    // the song length. miniaudio's seek handles an out-of-range frame; the
    // arithmetic must simply not lie about it.
    CHECK(FramesForSongSec(9999.0, kRate) == 479952000u);

    // NaN is not a frame count. Treat it as 0 rather than as UB.
    CHECK(FramesForSongSec(std::numeric_limits<double>::quiet_NaN(), kRate) ==
          0u);

    // Absurd input saturates instead of overflowing the double->uint64 cast.
    CHECK(FramesForSongSec(std::numeric_limits<double>::infinity(), kRate) ==
          kMaxFrames);
    CHECK(FramesForSongSec(1e30, kRate) == kMaxFrames);

    // The rate is a parameter, not a baked-in 44100/48000.
    CHECK(FramesForSongSec(1.0, 44100.0) == 44100u);
}

// AnchorEstimator::Position's frame term, verbatim (AnchorEstimator.cpp:8).
//
// StartFrameForSeek's raw return value is an intermediate that nothing reads
// on its own - it goes straight to AnchorEstimator::SetStartFrame, and the
// only thing that matters is what Position reads back THROUGH it. So the
// assertions below are round trips rather than equalities on the raw value.
// That distinction is not academic: a version of StartFrameForSeek that
// saturates at 0 satisfies a naive equality test and fails every round trip
// here by the full size of the seek.
static std::int64_t PositionFrames(std::uint64_t a_anchorFrames,
                                   std::uint64_t a_startFrame) {
    return static_cast<std::int64_t>(a_anchorFrames - a_startFrame);
}

static void StartFrameForSeekTests() {
    // Engine alive 10s, seek the song to 4s: song position 0 "played" 4s
    // worth of frames ago, and the estimator must read the song at 4s.
    {
        const auto start = StartFrameForSeek(480000u, 192000u);
        CHECK(start == 288000u);
        CHECK(PositionFrames(480000u, start) == 192000);
    }

    // Seek to 0: song zero is NOW.
    {
        const auto start = StartFrameForSeek(480000u, 0u);
        CHECK(start == 480000u);
        CHECK(PositionFrames(480000u, start) == 0);
    }

    // Engine time exactly equal to the seek target: song 0 played at engine
    // frame 0, so the start frame is 0 - and Position still reads the SEEK
    // TARGET, not 0. That is the invariant every case here checks:
    //   PositionFrames(now, StartFrameForSeek(now, songFrames)) == songFrames
    // for every `now`, because the two subtractions cancel.
    CHECK(StartFrameForSeek(192000u, 192000u) == 0u);
    CHECK(PositionFrames(192000u, StartFrameForSeek(192000u, 192000u)) ==
          192000);

    // THE case a saturating implementation gets wrong, and it is the common
    // case rather than an edge: seeking further into the song than the
    // engine has been alive. Engine alive 1s, seek to 30s. The start frame
    // genuinely wraps - that is the intended mechanism - and Position must
    // still read 30s (1440000 frames), not the 48000 frames (1s) that
    // saturating at 0 produces.
    {
        const auto start = StartFrameForSeek(48000u, 1440000u);
        CHECK(start > (1ull << 63));  // it really did wrap
        CHECK(PositionFrames(48000u, start) == 1440000);
        // ...and it keeps reading correctly as the engine advances: one
        // more second of engine frames is one more second of song.
        CHECK(PositionFrames(48000u + 48000u, start) == 1440000 + 48000);
    }

    // One frame past the boundary - the tightest possible wrap check.
    CHECK(PositionFrames(192000u, StartFrameForSeek(192000u, 192001u)) ==
          192001);
    CHECK(PositionFrames(0u, StartFrameForSeek(0u, 1u)) == 1);

    // Both zero (engine barely started, seek to song 0).
    CHECK(PositionFrames(0u, StartFrameForSeek(0u, 0u)) == 0);

    // Composed end to end: the pair is what AudioEngine::SeekStems runs.
    CHECK(PositionFrames(
              480000u, StartFrameForSeek(480000u, FramesForSongSec(4.0, kRate))) ==
          192000);
    // The practice loop's actual shape: a late seek early in the session,
    // expressed in seconds.
    CHECK(PositionFrames(48000u, StartFrameForSeek(
                                     48000u, FramesForSongSec(30.0, kRate))) ==
          1440000);
    // ...and a negative request cannot move the start frame backwards.
    CHECK(StartFrameForSeek(480000u, FramesForSongSec(-2.0, kRate)) == 480000u);
}

void RunTests() {
    FramesForSongSecTests();
    StartFrameForSeekTests();
}

TEST_MAIN("StemSeek")
