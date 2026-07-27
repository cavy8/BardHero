// src/game/EngineFeed.h
#pragma once

// The session-thread <-> game-thread seam for M3 (spec 7 / handoff item 4).
// The input hook (game thread) converts stamps and feeds the engine; the
// session thread owns lifecycle and every clock REBASE. One mutex guards
// {engine, clock, counters}; `active`/`engaged` are the lock-free gates.
// Rules (Session.cpp's review-hardened threading rules extend here):
//   - session thread: publish at StartSession / unpublish at EndSession,
//     and wrap EVERY clock mutation (Pause, ResumeSynced - both the resume
//     site and the countdown re-projection) in mx. Its own clock READS need
//     no lock: it is the only writer.
//   - game thread (hook): all engine/clock access under mx; NEVER touches
//     Session's g_s.
//   - render thread (FLICK overlay): all reads under mx, null-checks
//     {song, clock, engine} first, never writes, never touches g_s.
//   - teardown order: engaged=false -> active=false -> under mx null the
//     pointers -> only then stats-read/delete (no use-after-free window).

#include <atomic>
#include <cstdint>
#include <mutex>

namespace bard {
    class  GuitarEngine;
    class  MasterClock;
    struct LoadedSong;
}

namespace SH {
    class AudioEngine;

    struct EngineFeed {
        static EngineFeed& GetSingleton() {
            static EngineFeed s;
            return s;
        }

        std::mutex          mx;
        bard::GuitarEngine* engine = nullptr;  // guarded by mx
        bard::MasterClock*  clock  = nullptr;  // guarded by mx
        // M4 render-path publications (guarded by mx; song's CONTENT is
        // immutable while published, audio points to an object whose
        // cross-thread-safe methods are documented in AudioEngine.h; all
        // nulled in EndSession's nulling block so the teardown-order proof
        // covers the render thread too)
        const bard::LoadedSong* song  = nullptr;
        AudioEngine*            audio = nullptr;
        double songLen       = 0.0;
        double maxSustainSec = 0.0;  // visible-window back-extension
        int    guitarStem    = -1;   // miss-mute target; -1 = none

        std::atomic<bool> active{ false };   // a session exists (any state)
        std::atomic<bool> engaged{ false };  // kPlaying/kResuming: route+feed
        // hook-written physical fret bits 0-4 (render: fret press glow)
        std::atomic<std::uint8_t> heldFrets{ 0 };
        // Bumped by the session thread whenever the song CLOCK is rebased
        // under a live session - today that is a practice loop restart.
        //
        // AutoPlayBot holds a monotonic cursor into the chart, so a restart
        // that moves the clock BACKWARDS leaves the cursor past every note
        // and the bot silently emits nothing for the second and later loops
        // (field 2026-07-26, `[Cheats] bAutoPlay`). The hook cannot see the
        // restart any other way: `active`/`engaged` never drop, so its
        // new-session seed does not fire. It compares this against its own
        // copy once per frame and re-seeds on a change - which also
        // re-primes the fast-forward, so the bot resumes cleanly at the
        // range start instead of replaying the range as a burst of late
        // hits.
        std::atomic<std::uint64_t> clockGeneration{ 0 };

        struct Counters {  // guarded by mx; hook writes, EndSession drains
            long   fed = 0, stale = 0, bufferFull = 0;
            double maxAgeMs = 0.0, sumAgeMs = 0.0;
        } counters;
    };
}
