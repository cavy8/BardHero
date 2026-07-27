// src/game/PerformanceCamera.h
#pragma once

// Performance camera: the game-thread applier for CameraDirectorLogic.h,
// plus the (now answered) measurement spikes that gated it.
//
// Spec: docs/superpowers/specs/2026-07-26-performance-camera-director-design.md
//
// Tier 2 by design: it drives vanilla ThirdPersonState fields rather than
// owning the camera node, so the engine's collision, smoothing and state
// handling all stay intact. The director's values are written BEFORE the
// original Update, which is load-bearing - the engine's collision pass runs
// inside Update, so writing first means it solves against the shot we
// actually want this frame, and the corrected position it leaves behind is
// what the occlusion blacklist reads back.
//
// What the 2026-07-26 spikes settled, so it is not re-litigated here:
//   - `posOffsetExpected` is NOT a control input (the engine recomputes it
//     inside Update, and `posOffsetActual` glides rather than snapping), so
//     there are no per-member pivots and every shot is an orbit around the
//     player. CameraDirectorLogic.h carries the full account.
//   - Tier-2 yaw/zoom writes SURVIVE with SmoothCam installed, so no
//     SmoothCam API handshake and no RequestCameraControl.
namespace SH::PerformanceCamera {
    // SmoothCam handshake, in this order and at these exact moments.
    //
    // Needed because writing the vanilla fields is NOT enough when SmoothCam
    // is active: it owns the final camera position and treats vanilla state
    // as an input to its own solver, so our writes land in the fields
    // (field-verified) while the camera keeps SmoothCam's interpolation,
    // distance and FOV. Without this, cuts drag, the FOV never changes and
    // the camera sits far enough out to clip the walls.
    void RegisterSmoothCam();  // kPostLoad, once
    void RequestSmoothCam();   // kPostPostLoad, once

    // kDataLoaded or later. Installs the ThirdPersonState::Update hook if
    // either the director or the spike is enabled; otherwise does nothing at
    // all, so a default build carries no camera hook.
    void Install();

    // Session lifecycle. Safe from any thread and cheap when the director is
    // off.
    //
    // Release() is called from every ending path - song end before results,
    // abort, crowd failure, and save load - because the camera must be the
    // player's again before anything else happens, and a director left
    // engaged across a load would drive a session that no longer exists. It
    // only RAISES a request; the restore itself happens on the next game
    // thread pass, which is the only place the camera is safe to touch.
    void Release();

    // Live toggle from the FLICK settings page. Any thread.
    void SetEnabled(bool a_on);

    // Was the camera hook installed at load? False means a director enabled
    // now cannot take effect until the game restarts - which the settings
    // page says out loud, because a toggle that appears to do nothing is
    // indistinguishable from a broken one.
    [[nodiscard]] bool Hooked();
}
