// src/audio/StemSeekLogic.h
#pragma once

#include <cstdint>

namespace SH::stem_seek {

    // The frame arithmetic behind AudioEngine::SeekStems, split out from the
    // device so it can be tested headlessly (tests/test_stemseek.cpp) - the
    // engine owns a live miniaudio device and cannot be. Pure: no miniaudio,
    // no RE, no OS. Practice mode's loop is a BACKWARDS seek, and the two
    // functions here sit on opposite sides of the same unsigned hazard:
    // FramesForSongSec must NOT let a negative song time underflow (it would
    // seek the stems somewhere unrecoverable), while StartFrameForSeek must
    // be allowed to wrap, because the estimator that consumes it undoes the
    // wrap exactly. Read each function's comment before changing either.

    // Largest frame count a double represents exactly (2^53 - about 5.9
    // million years at 48 kHz). The saturation ceiling: past this a
    // double -> std::uint64_t cast stops being meaningful, and past
    // 2^64 it is undefined behaviour outright.
    inline constexpr std::uint64_t kMaxFrames = 1ull << 53;

    // Song time in seconds -> a PCM frame at the engine's sample rate.
    //
    // NEGATIVE INPUT CLAMPS TO 0, which is the whole reason this is a
    // function and not an inline cast. A practice range starts at
    // `sectionTime - kLeadInSec` (PracticeRange.h), so an early section
    // yields a negative time; `static_cast<std::uint64_t>(-1.5 * 48000.0)`
    // is undefined behaviour and in practice underflows to roughly 1.8e19
    // frames. Truncation (not rounding) matches ScheduleStart's
    // `static_cast<std::uint64_t>(leadSeconds * kSampleRate)`; the worst
    // error is one frame, ~21 microseconds.
    //
    // A time PAST the end of the song is deliberately not clamped - this
    // header does not know the song length. miniaudio's seek is what
    // handles an out-of-range frame.
    constexpr std::uint64_t FramesForSongSec(double a_songSec,
                                             double a_sampleRate) {
        // Written as !(x > 0) rather than (x <= 0) so a NaN song time -
        // which compares false against everything - also lands on 0
        // instead of on an undefined cast.
        if (!(a_songSec > 0.0) || !(a_sampleRate > 0.0)) { return 0u; }
        const double frames = a_songSec * a_sampleRate;
        if (!(frames < static_cast<double>(kMaxFrames))) { return kMaxFrames; }
        return static_cast<std::uint64_t>(frames);
    }

    // The value AnchorEstimator::SetStartFrame wants: given the engine's
    // current position (ma_engine_get_time_in_pcm_frames) and the song frame
    // we have just seeked to, the engine frame at which song position 0
    // plays. That is `engineNow - songFrames`.
    //
    // THE SUBTRACTION IS ALLOWED TO WRAP, and must be. Whenever the seek
    // target sits further into the song than the engine has been alive the
    // true start frame is NEGATIVE and std::uint64_t cannot hold it. That is
    // the COMMON case, not an edge case: the engine clock starts at Init, so
    // every practice range beginning past ~2.3 s hits it, and so does any
    // loop restart of a short section late in a long song.
    //
    // Unsigned subtraction wraps modulo 2^64, and AnchorEstimator::Position
    // recovers the value EXACTLY with the matching signed subtraction
    // `static_cast<std::int64_t>(a.frames - _start)` (AnchorEstimator.cpp:8),
    // which is well defined since C++20. This is not a trick played on the
    // estimator - the same wrap is what already carries the pre-start
    // countdown, where `frames < _start` on every tick and the comment there
    // reads "signed distance handles the pre-start countdown".
    //
    // Do NOT "fix" this by saturating at 0. Saturation reads as the safe
    // choice and is silently wrong: with the engine 1 s old and a seek to
    // 30 s, a start frame of 0 makes Position report 1.00 s instead of
    // 30.00 s - 29 s of permanent desync, with the highway and the audio
    // disagreeing for the entire run.
    constexpr std::uint64_t StartFrameForSeek(std::uint64_t a_engineNowFrames,
                                              std::uint64_t a_songFrames) {
        return a_engineNowFrames - a_songFrames;
    }
}
