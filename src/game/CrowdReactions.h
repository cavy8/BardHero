// src/game/CrowdReactions.h
#pragma once

namespace SH::CrowdReactions {

    // Fixed slot order: Kind IS the index into the one-shot bank, and the
    // enumerator's name IS the file stem it loads (cheer.wav, groan.wav,
    // ...). ONE array in the .cpp spells both out, so the mapping the bank
    // depends on cannot drift between two lists in two files.
    //
    // kSwell is the RANK-UP sound (2026-07-22, spec
    // docs/specs/2026-07-22-performance-ui.md 3.4). It was reserved and
    // callerless for months; star power never claimed it and the deferred
    // end-of-song sting is a separate, still-unwritten file.
    enum class Kind { kCheer, kGroan, kSwell, kApplause, kAwkward };

    // Session thread, which must already be COM MTA (AudioEngine::Init).
    // Opens the crowd's OWN audio device and loads the bank the first time
    // it is called; every later call returns immediately. Call at session
    // START and never mid-song - the first call opens a WASAPI device.
    //
    // The crowd owns its audio rather than borrowing the session's because
    // the reaction that matters most is the one at the final note, and the
    // session's engine is torn down microseconds after that one fires. This
    // engine outlives every session and is never uninitialised.
    void Prepare();

    // Session thread. Rate-limited by fReactionCooldownSec; kApplause and
    // kAwkward are end-of-song and bypass the limiter. A silent no-op when
    // the feature is off, when Prepare never got an audio device, or when
    // that one file failed to load - a reaction is punctuation, never a
    // precondition.
    void Fire(Kind a_kind, double a_nowQpc);

    // Session thread. Clears the rate limiter (a cooldown left over from the
    // last performance would swallow this one's first cheer).
    void Reset();
}
