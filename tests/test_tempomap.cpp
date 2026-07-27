#include "harness.h"
#include "chart/TempoMap.h"

using bard::TempoMap;

static void RunTests() {
    {   // no marker at 0 -> 120 BPM default (spec 4.3)
        TempoMap m;
        m.SetResolution(192);
        m.Finalize();
        CHECK_NEAR(m.SecondsAt(192), 0.5, 1e-9);  // one beat at 120
        CHECK_NEAR(m.TickAt(0.5), 192.0, 1e-6);
    }
    {   // marker mid-song + cumulative seconds
        TempoMap m;
        m.SetResolution(192);
        m.AddBpm(0, 120.0);
        m.AddBpm(384, 240.0);  // after 2 beats, double time
        m.Finalize();
        CHECK_NEAR(m.SecondsAt(384), 1.0, 1e-9);
        CHECK_NEAR(m.SecondsAt(384 + 192), 1.25, 1e-9);
        CHECK_NEAR(m.TickAt(1.25), 576.0, 1e-6);
        CHECK_NEAR(m.BpmAtTick(383.0), 120.0, 1e-9);
        CHECK_NEAR(m.BpmAtTick(384.0), 240.0, 1e-9);
    }
    {   // fractional ticks (sustain scoring + SP math need them)
        TempoMap m;
        m.SetResolution(480);  // MIDI-converted charts carry 480
        m.AddBpm(0, 100.0);
        m.Finalize();
        CHECK_NEAR(m.SecondsAt(240.0), 0.3, 1e-9);  // half beat at 100 BPM
    }
    {   // duplicate marker on one tick: last wins; unsorted input tolerated
        TempoMap m;
        m.SetResolution(192);
        m.AddBpm(192, 60.0);
        m.AddBpm(0, 120.0);
        m.AddBpm(192, 240.0);
        m.Finalize();
        CHECK_NEAR(m.SecondsAt(384), 0.5 + 0.25, 1e-9);
    }
    {   // inverse round-trip across markers
        TempoMap m;
        m.SetResolution(192);
        m.AddBpm(0, 133.7);
        m.AddBpm(1000, 87.3);
        m.AddBpm(5000, 201.0);
        m.Finalize();
        for (double tick : { 0.0, 500.0, 1000.0, 3000.0, 5000.0, 9000.0 }) {
            CHECK_NEAR(m.TickAt(m.SecondsAt(tick)), tick, 1e-6);
        }
    }
    {   // below-first-marker lookups (session clock runs negative during
        // the kSongStartDelay lead-in): must resolve as a DEFINED linear
        // extrapolation using marker 0's rate, not walk off the front of
        // the marker vector. Default 120 BPM, res 192.
        TempoMap m;
        m.SetResolution(192);
        m.Finalize();
        CHECK_NEAR(m.TickAt(-1.0), -384.0, 1e-9);
        CHECK_NEAR(m.SecondsAt(-384.0), -1.0, 1e-9);
        CHECK_NEAR(m.TickAt(m.SecondsAt(-384.0)), -384.0, 1e-6);
    }
}

TEST_MAIN("TempoMap")
