// tests/test_clock.cpp
#include "harness.h"
#include "clock/MasterClock.h"
#include "clock/ResumeCountdown.h"
#include "clock/ResumeWorldPauseLogic.h"
#include "clock/AnchorEstimator.h"
#include "clock/AnchorSeqlock.h"
#include "clock/SlaveController.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

using namespace bard;

static void MasterClockTests() {
    {   // start + lead-in: InputTime crosses 0 exactly at rawStart + 2
        MasterClock c;
        c.Start(100.0);
        CHECK_NEAR(c.InputTime(100.0), -2.0, 1e-12);
        CHECK_NEAR(c.InputTime(102.0), 0.0, 1e-12);
        CHECK_NEAR(c.InputTime(150.0), 48.0, 1e-12);
        CHECK_NEAR(c.SongTime(150.0), 48.0, 1e-12);    // cal 0
        CHECK_NEAR(c.VisualTime(150.0), 48.0, 1e-12);
    }
    {   // calibration terms (spec 6 formulas)
        MasterClock c;
        c.Start(0.0);
        c.SetCalibration(0.030, -0.010);
        CHECK_NEAR(c.SongTime(10.0), 8.0 + 0.030, 1e-12);
        CHECK_NEAR(c.VisualTime(10.0), 8.0 - 0.010, 1e-12);
    }
    {   // pause freezes, resume continuous
        MasterClock c;
        c.Start(0.0);
        const double before = c.InputTime(50.0);
        c.Pause(50.0);
        CHECK(c.Paused());
        CHECK_NEAR(c.InputTime(60.0), before, 1e-12);      // frozen
        c.Resume(64.0);
        CHECK_NEAR(c.InputTime(64.0), before, 1e-12);      // continuous
        CHECK_NEAR(c.InputTime(65.0), before + 1.0, 1e-12);
    }
    {   // ResumeSynced rebases onto the observed audio position
        MasterClock c;
        c.Start(0.0);
        c.Pause(50.0);
        c.ResumeSynced(64.0, 47.5);
        CHECK_NEAR(c.InputTime(64.0), 47.5, 1e-12);
        CHECK_NEAR(c.InputTime(65.0), 48.5, 1e-12);
    }
    {   // speed change rebases so outputs are continuous
        MasterClock c;
        c.Start(0.0);
        const double t = c.InputTime(30.0);  // 28
        c.SetSpeed(30.0, 0.5);
        CHECK_NEAR(c.InputTime(30.0), t, 1e-12);
        CHECK_NEAR(c.InputTime(32.0), t + 1.0, 1e-12);     // half rate
        c.SetCalibration(0.020, 0.0);
        CHECK_NEAR(c.SongTime(32.0), t + 1.0 + 0.020 * 0.5, 1e-12);
    }
    {   // non-1 speed from the start: lead-in scales in InputTime units
        MasterClock c;
        c.Start(0.0, 2.0);
        CHECK_NEAR(c.InputTime(0.0), -4.0, 1e-12);
        CHECK_NEAR(c.InputTime(2.0), 0.0, 1e-12);
        CHECK_NEAR(c.InputTime(3.0), 2.0, 1e-12);
    }
    {   // double Pause / Resume are no-ops
        MasterClock c;
        c.Start(0.0);
        c.Pause(10.0);
        c.Pause(20.0);
        CHECK_NEAR(c.InputTime(30.0), 8.0, 1e-12);  // frozen at first pause
        c.Resume(30.0);
        c.Resume(40.0);
        CHECK_NEAR(c.InputTime(31.0), 9.0, 1e-12);
    }
}

static void ResumeCountdownTests() {
    using bard::ResumeCountdown;
    using Cue = ResumeCountdown::Cue;

    ResumeCountdown c;
    CHECK(!c.Active());
    CHECK(c.CueAt(100.0) == Cue::kDone);

    CHECK(c.Start(10.0));
    CHECK(c.Active());
    CHECK(c.CueAt(10.0) == Cue::kThree);
    CHECK(c.CueAt(10.749999) == Cue::kThree);
    CHECK(c.CueAt(10.75) == Cue::kTwo);
    CHECK(c.CueAt(11.499999) == Cue::kTwo);
    CHECK(c.CueAt(11.50) == Cue::kOne);
    CHECK(c.CueAt(12.249999) == Cue::kOne);
    CHECK(c.CueAt(12.25) == Cue::kGo);
    CHECK(c.CueAt(12.999999) == Cue::kGo);
    CHECK(c.CueAt(13.0) == Cue::kDone);
    CHECK(c.Complete(13.0));

    // Repeated resume cannot extend an active countdown. Re-pause/teardown
    // cancellation is idempotent, and a later resume starts a fresh epoch.
    CHECK(!c.Start(12.0));
    c.Cancel();
    c.Cancel();
    CHECK(!c.Active());
    CHECK(c.Start(20.0));
    CHECK(c.CueAt(20.0) == Cue::kThree);
}

static void ResumeWorldPauseTests() {
    using SH::resume_world::Phase;
    using SH::resume_world::ShouldPause;

    // The custom pause owns Skyrim's pause counter all the way through
    // 3,2,1,GO and the live-audio-anchor wait. Only the transition back to
    // playable, clock-synchronised gameplay may release it.
    CHECK(ShouldPause(Phase::kPauseMenu));
    CHECK(ShouldPause(Phase::kCountdown));
    CHECK(ShouldPause(Phase::kAwaitingAudioAnchor));
    CHECK(!ShouldPause(Phase::kPlaying));
    CHECK(!ShouldPause(Phase::kTeardown));
}

static void AnchorEstimatorTests() {
    {   // exact interpolation, start-frame relative
        AnchorEstimator e(48000.0, 0.010);
        e.SetStartFrame(96000);
        AudioAnchor a{ 96000 + 4800, 100.0, 1.0 };
        CHECK_NEAR(e.Position(a, 100.0), 0.1, 1e-12);
        CHECK_NEAR(e.Position(a, 100.004), 0.104, 1e-12);
    }
    {   // pre-start countdown: negative position allowed
        AnchorEstimator e(48000.0, 0.010);
        e.SetStartFrame(96000);
        AudioAnchor a{ 48000, 10.0, 1.0 };
        CHECK_NEAR(e.Position(a, 10.0), -1.0, 1e-12);
    }
    {   // rate scales the interpolation term (nudged resampler)
        AnchorEstimator e(48000.0, 0.010);
        e.SetStartFrame(0);
        AudioAnchor a{ 48000, 5.0, 1.05 };
        CHECK_NEAR(e.Position(a, 5.010), 1.0 + 0.010 * 1.05, 1e-12);
    }
    {   // rate 0 (paused publisher) freezes the position
        AnchorEstimator e(48000.0, 0.010);
        e.SetStartFrame(0);
        AudioAnchor a{ 48000, 5.0, 0.0 };
        CHECK_NEAR(e.Position(a, 7.0), 1.0, 1e-12);
    }
    {   // monotonic clamp: a late-stamped anchor cannot step time backwards
        AnchorEstimator e(48000.0, 0.010);
        e.SetStartFrame(0);
        CHECK_NEAR(e.Position({ 4800, 1.000, 1.0 }, 1.009), 0.109, 1e-12);
        // same frames, later qpc stamp: raw math would say 0.107
        CHECK_NEAR(e.Position({ 4800, 1.002, 1.0 }, 1.009), 0.109, 1e-12);
    }
    {   // jittered-callback simulation (spec 11): error bounded by jitter
        AnchorEstimator e(48000.0, 0.010);
        e.SetStartFrame(0);
        const double jit[] = { 0.003, -0.002, 0.001, -0.003, 0.002, 0.0, -0.001 };
        double       worst = 0.0;
        for (int k = 1; k <= 500; ++k) {
            const double ideal = 0.010 * k;
            AudioAnchor  a{ static_cast<std::uint64_t>(480u) * k,
                            ideal + jit[k % 7], 1.0 };
            const double probe = ideal + 0.005;
            const double pos   = e.Position(a, probe);
            worst = std::max(worst, std::abs(pos - probe));
        }
        CHECK(worst <= 0.0031);
    }
    {   // staleness guard at max(250ms, 2x callback period): transient
        // 1-2 period gaps are ordinary process hitches (field 2026-07-19)
        // and must NOT pause; a dead device crosses the floor fast
        AnchorEstimator e(48000.0, 0.010);
        AudioAnchor     a{ 0, 1.0, 1.0 };
        CHECK(e.StaleLimitSec() == 0.25);  // floor wins at a 10ms period
        CHECK(!e.Stale(a, 1.021));         // 21ms: old 2x-period trip point
        CHECK(!e.Stale(a, 1.249));
        CHECK(e.Stale(a, 1.251));
        // a slow-callback config where 2x period exceeds the floor
        AnchorEstimator s(48000.0, 0.200);
        CHECK(s.StaleLimitSec() == 0.4);
        CHECK(!s.Stale(a, 1.399));
        CHECK(s.Stale(a, 1.401));
    }
}

static void SeqlockTests() {
    {   // publish/read round trip + Published flag
        AnchorSeqlock s;
        CHECK(!s.Published());
        s.Publish({ 480, 1.0, 1.0 });
        CHECK(s.Published());
        const AudioAnchor a = s.Read();
        CHECK(a.frames == 480);
        CHECK(a.qpc == 1.0);
    }
    {   // torn-read check: writer publishes coupled values; every read must
        // satisfy qpc == frames * 1e-3 exactly (a torn read breaks it)
        AnchorSeqlock    s;
        std::atomic_bool stop{ false };
        std::thread      w([&] {
            for (std::uint64_t i = 1; i <= 200000; ++i) {
                s.Publish({ i, static_cast<double>(i) * 1e-3,
                            (i % 2) ? 1.0 : 0.0 });
            }
            stop.store(true);
        });
        int reads = 0;
        while (!stop.load()) {
            const AudioAnchor a = s.Read();
            if (a.frames == 0) continue;
            CHECK(a.qpc == static_cast<double>(a.frames) * 1e-3);
            ++reads;
        }
        w.join();
        CHECK(reads > 0);
    }
}

static void SlaveControllerTests() {
    {   // below engage: no correction
        SlaveController c;
        CHECK(c.Update(0.010) == 1.0);
        CHECK(!c.Engaged());
        CHECK(c.Corrections() == 0);
    }
    {   // engage > 15ms, hold through the band, disengage < 5ms, no re-engage
        SlaveController c;
        CHECK_NEAR(c.Update(0.016), 1.05, 1e-12);  // audio behind -> faster
        CHECK(c.Engaged());
        CHECK_NEAR(c.Update(0.010), 1.05, 1e-12);  // 10ms still engaged
        CHECK(c.Update(0.004) == 1.0);             // converged -> off
        CHECK(!c.Engaged());
        CHECK(c.Corrections() == 1);
        CHECK(c.Update(0.010) == 1.0);             // hysteresis: stays off
    }
    {   // negative delta: audio ahead -> slow down
        SlaveController c;
        CHECK_NEAR(c.Update(-0.020), 0.95, 1e-12);
    }
    {   // overshoot (sign flip vs engagement) disengages even above 5ms
        SlaveController c;
        CHECK_NEAR(c.Update(0.020), 1.05, 1e-12);
        CHECK(c.Update(-0.006) == 1.0);
        CHECK(!c.Engaged());
    }
    {   // Reset drops engagement (session pause path)
        SlaveController c;
        c.Update(0.020);
        c.Reset();
        CHECK(!c.Engaged());
        CHECK(c.Update(0.010) == 1.0);
    }
    {   // exact thresholds: strict > engages, strict < disengages
        SlaveController c;
        CHECK(c.Update(0.015) == 1.0);              // boundary: not engaged
        CHECK_NEAR(c.Update(0.0151), 1.05, 1e-12);
        CHECK_NEAR(c.Update(0.005), 1.05, 1e-12);   // boundary: stays engaged
        CHECK(c.Update(0.0049) == 1.0);
    }
    {   // second engage episode bumps the counter to 2
        SlaveController c;
        c.Update(0.020);
        c.Update(0.001);            // converge -> off
        c.Update(0.020);            // re-engage
        CHECK(c.Corrections() == 2);
    }
}

static void RunTests() {
    MasterClockTests();
    ResumeCountdownTests();
    ResumeWorldPauseTests();
    AnchorEstimatorTests();
    SeqlockTests();
    SlaveControllerTests();
}

TEST_MAIN("Clock")
