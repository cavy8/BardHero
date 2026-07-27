// src/game/UiSfx.h
#pragma once

#include "game/UiSfxLogic.h"

namespace SH::UiSfx {

    // Session thread, which must already be COM MTA (AudioEngine::Init).
    // Opens the UI bank's OWN audio device and loads the 11-file set the
    // first time it is called; every later call returns immediately. Call
    // at session START, never mid-song - the first call opens a WASAPI
    // device.
    //
    // The bank keeps a process-lifetime engine for the same reason the
    // crowd does: the song-end stings and the results score tick fire
    // around and after the session engine's teardown, so they cannot play
    // on it. (miss_electric1..5 are the exception - they load into the
    // SESSION engine's miss bank in Session.cpp, replacing miss1..3 in
    // the electric context, so the existing InputHook trigger and
    // fMissSfxVolume keep working unchanged.)
    void Prepare();

    // ANY thread - banners and the results tick fire from the render
    // thread, the stings from the session thread, so readiness is
    // published release/acquire (unlike CrowdReactions' session-only
    // statics). Silent no-op when bUiSfx is off, Prepare found no device,
    // or that one file failed to load. Logs every fire: the field pass
    // confirms call sites by log line.
    void Fire(ui_sfx::Cue a_cue);

    // Any thread. Live re-apply of the ONE common gain (settings
    // slider); a no-op until Prepare succeeded. Needed because this bank
    // loads once per process - unlike the miss bank there is no
    // next-session reload to pick a new value up.
    void SetVolume(float a_gain);

    // Any thread. The looped score_tick under the results count-up.
    // Start rewinds and re-opens the fader; Stop is clickless (40 ms
    // fade) and logs only on a genuine running -> stopped transition, so
    // the belt-and-braces Stop in ResultsWindow::Close() stays silent
    // when the loop already ended.
    void StartScoreTick();
    void StopScoreTick();
}
