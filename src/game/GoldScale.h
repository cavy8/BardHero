// src/game/GoldScale.h
#pragma once

namespace SH {
    // Scales SGT's performance gold by session accuracy x difficulty.
    // SGT pays once, ~32.8s after the perform cast (hardcoded RandomInt,
    // no ModEvents - docs/research/2026-07-18-bard-mods-survey.md): a
    // container sink captures the grant in a time window after the cast;
    // OnSessionEnd applies captured*(mult-1) on the game thread.
    class GoldScale {
    public:
        static void Install();          // container sink (kDataLoaded, gated)
        static void NotePerformCast();  // game thread (spell sink)
        static void OnSessionStart();   // session thread: reset capture
        static void OnSessionEnd(bool completed, int notesHit,
                                 int notesTotal, int difficulty);
        // Whole-song mode (plan 2026-07-19): while the SGT performance is
        // LIVE the 32.8s payout cannot fire, so the mid-song legacy window
        // must not capture stray grants; the payout is dispatched at session
        // end instead and captured in a short end window.
        static void SetWholeSongLive(bool live);      // session thread
        // stars/mood/inn/rank feed the performance payout (spec 5.6).
        // audience is the time-averaged listener count near the player
        // over the song; pass a negative value when counting was disabled
        // and the purse should not be audience-scaled at all.
        static void ArmDeferred(int notesHit, int notesTotal, int difficulty,
                                int stars, int moodLevel, int rank,
                                bool atInn, double songSec,
                                double audience);     // session thread
        static void CancelDeferred();                 // game thread (dispatch failed)
        static void OpenEndCapture();                 // game thread (payout sent)
        static void TickDeferred(double nowQpc);      // session thread, every loop
    };
}
