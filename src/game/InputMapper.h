// src/game/InputMapper.h
#pragma once

// Host-side DI-event -> engine-input mapper (spec 7.1). PURE logic: no RE,
// no Windows, no logging - unit-tested headless (InputMapperTests). The
// hook owns the OS reads and passes (DI buffer, timeGetTime, QPC) in; this
// class owns seeding, plausibility, sequence tracking, bind mapping, edge
// semantics, whammy refire, and engage-diff synthesis. Events come out in
// raw QPC seconds; the hook converts to InputTime under the feed lock
// (clock access) and queues to the engine.

#include <cstdint>
#include <vector>

#include "engine/InputTypes.h"

namespace SH {

    // Local mirror of DirectInput's DIDEVICEOBJECTDATA (x64: 24 bytes). The
    // pinned CommonLibSSE-NG ships no DI header; this is the Win32 ABI
    // (same mirror the M0 spike proved on both runtimes).
    struct DiEvent {
        std::uint32_t ofs;        // DIK scan code for keyboards
        std::uint32_t data;       // 0x80 = pressed
        std::uint32_t timeStamp;  // timeGetTime()-domain ms, event-time (M0)
        std::uint32_t sequence;   // DirectInput-global monotonic counter
        std::uint64_t appData;
    };
    static_assert(sizeof(DiEvent) == 24);

    // DIK scan codes; 0 = unbound. Filled from Settings at hook install.
    // Every action has a secondary slot so a controller-as-keyboard bridge
    // works alongside the plain keyboard binds (field 2026-07-19: a Wii GH
    // guitar emitting A,S,J,K,L frets, Up/Down-arrow strum bar, ';' whammy
    // - the exact Clone Hero second-column layout). Strum gets a THIRD
    // slot because a strum bar is two switches (up + down), both = strum.
    // Known simplification: two keys on one slot share held state, so
    // releasing one while holding the other drops the fret - simultaneous
    // same-fret keyboard+guitar holds are not a real scenario.
    struct Binds {
        int fret[5] = { 0x02, 0x03, 0x04, 0x05, 0x06 };  // number row 1-5
        int strum   = 0x39;                              // Space
        int sp      = 0x2A;                              // Left Shift
        int whammy  = 0x00;                              // unbound (spec 14)
        int pause   = 0x01;                              // Esc
        int fret2[5] = { 0x1E, 0x1F, 0x24, 0x25, 0x26 };  // A S J K L
        int strum2   = 0xC8;                              // Up arrow
        int strum3   = 0xD0;                              // Down arrow
        int sp2      = 0x23;  // H (gh3.PIE: guitar Minus button + tilt)
        int whammy2  = 0x27;  // ';' (gh3.PIE whammy is Mouse.x - PIE edit)
        int pause2   = 0x1C;  // Enter (gh3.PIE: guitar Plus) - toggle

        bool IsBound(std::uint32_t dik) const;
    };

    struct MappedEvent {
        double            qpcSec;  // event time, QPC seconds
        bard::InputAction action;
        std::int32_t      value;   // 1 press / 0 release (strum always 1)
        std::uint32_t     dik;     // DIK or normalized pad code; log only
    };

    // Skyrim AE can omit modifier keys from the keyboard device's raw
    // DirectInput buffer while still publishing them as ButtonEvents. This
    // narrow fallback covers the primary keyboard Star Power key only; the
    // raw mapper remains authoritative for frets, strum, H, and every other
    // binding. Equivalent same-frame raw events are de-duplicated.
    class KeyboardSpFallback {
    public:
        void FeedButton(std::uint32_t code, bool down, double qpcNowSec,
                        const Binds& binds, bool engaged,
                        std::vector<MappedEvent>& out);
        void EmitEngageDiff(double qpcNowSec, const Binds& binds,
                            std::vector<MappedEvent>& out);
        void Reset();
        // The engine was REBUILT (practice-loop seek): whatever it was told
        // no longer exists over there. Call before EmitEngageDiff so the
        // diff re-emits everything physically held.
        void ForgetEngineState() { _heldEngine = false; }

    private:
        bool _heldRaw    = false;
        bool _heldEngine = false;
    };

    // Skyrim/SKSE normalized gamepad macro codes (266..281). Defaults follow
    // Clone Hero's official gamepad recommendation: LT/LB/RB/RT/A are the
    // five frets. D-pad up/down remain explicit strums for open notes.
    struct GamepadBinds {
        int fret[5] = { 280, 274, 275, 281, 276 };
        int strum[2] = { 266, 267 };
        int sp       = 278;  // X / Square
        int whammy   = 279;  // Y / Triangle
        int pause    = 270;  // Start / Options
        int confirm  = 276;  // A / Cross
        int cancel   = 277;  // B / Circle

        bool IsBound(std::uint32_t code) const;
    };

    // Pure normalized gamepad-event mapper. Unlike DirectInput, Skyrim's
    // gamepad ButtonEvents have no event timestamp, so the hook supplies the
    // current QPC time. In Gamepad Mode all fret downs received in one input
    // frame are applied first, then coalesced into one auto-strum so a chord
    // cannot overstrum once per constituent button.
    class GamepadMapper {
    public:
        void FeedButton(std::uint32_t code, bool down, double qpcNowSec,
                        const GamepadBinds& binds, bool engaged,
                        bool gamepadMode, std::vector<MappedEvent>& out);
        // graceSec: same chord-join window as the keyboard's fret-only mode
        // (see InputMapper::Feed) - a controller chord pressed across two
        // input frames armed a second auto-strum straight into the engine's
        // still-pending first one, which is an immediate overstrum. Same
        // mechanism, found via the keyboard field report of 2026-07-29. A
        // d-pad strum arms the window too, so d-pad + fret in one frame no
        // longer double-strums. Defaulted so existing callers keep the old
        // 50ms engine default; the hook passes the live tuning value.
        void EndFrame(double qpcNowSec, const GamepadBinds& binds,
                      bool engaged, bool gamepadMode,
                      std::vector<MappedEvent>& out,
                      double graceSec = 0.050);
        void EmitEngageDiff(double qpcNowSec, const GamepadBinds& binds,
                            std::vector<MappedEvent>& out);
        void Reset();
        // See InputMapper::ForgetEngineState.
        void ForgetEngineState() { _heldEngine = 0; }

        std::uint8_t HeldFretMask() const { return _heldRaw & 0x1F; }

    private:
        std::uint8_t _heldRaw       = 0;
        std::uint8_t _heldEngine    = 0;
        bool         _whammyHeldRaw = false;
        bool         _whammyEdge    = false;
        bool         _autoStrum     = false;
        // Last strum emitted, manual or auto (see InputMapper::_lastStrumAt).
        double       _lastStrumAt   = -1.0e18;
    };

    class InputMapper {
    public:
        struct FeedStats {
            int  fresh      = 0;      // plausible events past the high-water mark
            int  mapped     = 0;      // events appended to out
            int  stale      = 0;      // |age| > kStaleMs (alt-tab backlog)
            bool bufferFull = false;  // every slot fresh in one frame
        };
        static constexpr std::int32_t kStaleMs = 1000;

        // Once per hook frame while a session exists (any state). The first
        // call after Reset() seeds the sequence high-water mark and emits
        // nothing (M0: pre-existing/garbage entries must never replay).
        // Held state is tracked on every call; `out` gets events only while
        // `engaged`.
        //
        // fretsOnly: every fret PRESS also strums, so the strum key is not
        // needed at all. The keyboard twin of Gamepad Mode, asked for by two
        // players on the same day who cannot press Space and a fret at once.
        // ONE strum per Feed batch: a chord arrives as several fret events
        // in a single DI buffer and must strum once. The strum carries the
        // triggering fret's OWN event timestamp rather than frame time -
        // the whole reason this path reads the DI buffer is that its stamps
        // are sub-frame, and the strum is the input that decides the score.
        //
        // fretsOnlyGraceSec: how long after a strum a new fret press JOINS
        // the chord instead of strumming again. Field 2026-07-29, first
        // fret-only session: a human chord spreads over 20-100ms, which
        // crosses Feed batches, so per-batch coalescing alone re-strummed
        // into the engine's still-pending first strum - and a second strum
        // while one is pending is an IMMEDIATE overstrum
        // (GuitarEngine.cpp "double strum"). Pass the engine's OWN live
        // strum leniency (difficulty::EngineParamsFor(tuning).strumLeniency)
        // and the two windows agree by construction: while the engine would
        // still accept the chord completing, the mapper stays quiet; once
        // the pending strum is gone, a fresh press strums again. A manual
        // strum-key strum arms the same window, so strumming a chord and
        // fretting it a few ms later cannot double-strum either.
        FeedStats Feed(const DiEvent* buf, int len, std::uint32_t tgtNow,
                       double qpcNowSec, const Binds& binds, bool engaged,
                       bool fretsOnly, double fretsOnlyGraceSec,
                       std::vector<MappedEvent>& out);

        // Call on the disengaged -> engaged transition, AFTER that frame's
        // Feed (keeps the queue monotonic): emits fret/SP diffs between the
        // raw physical state and what the engine last saw, stamped
        // qpcNowSec. A release swallowed while paused (M0: 23.5s-late
        // alt-tab release) must not leave the engine's held mask stuck.
        void EmitEngageDiff(double qpcNowSec, const Binds& binds,
                            std::vector<MappedEvent>& out);

        void Reset();  // new session: fresh seed + cleared held state

        // The engine was REBUILT (practice-loop seek, SeekTo): its fret
        // mask is zero again, so what the engine "last saw" is nothing,
        // whatever this mapper remembers telling the old one. Without this,
        // a fret held across the loop wrap diffs as already-known and the
        // engage diff emits nothing - the note refuses to register until
        // the player lifts and re-presses, with the strike-line pad sitting
        // lit the whole time (field 2026-07-28).
        void ForgetEngineState() { _heldEngine = 0; }

        // Physical fret bits 0-4 as last observed (render: fret press glow).
        std::uint8_t HeldFretMask() const { return _heldRaw & 0x1F; }

    private:
        std::uint32_t _lastSeq       = 0;
        bool          _seeded        = false;
        std::uint8_t  _heldRaw       = 0;  // bits 0-4 frets, bit 5 SP (physical)
        std::uint8_t  _heldEngine    = 0;  // what the engine has been told
        // QPC time of the last strum we emitted, manual or auto. The
        // fret-only chord-join window measures from here; hugely negative
        // so the first strum after Reset always emits.
        double        _lastStrumAt   = -1.0e18;
        bool          _whammyHeldRaw = false;
    };
}
