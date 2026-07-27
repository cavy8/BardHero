#pragma once
#include <cassert>

namespace bard {

    // Audio-slave drift controller (spec 6). delta = expected - actual song
    // seconds. Engage at |delta| > 15ms, correct at the full +/-5% rate
    // nudge, disengage below 5ms (3:1 hysteresis) or on overshoot (delta
    // sign flip vs engagement). Output is the master resampler ratio;
    // corrections should be ~never in steady state (M0: zero drift growth).
    class SlaveController {
    public:
        struct Params {
            double engage    = 0.015;
            double disengage = 0.005;
            double nudge     = 0.05;
        };
        explicit SlaveController(Params p = {}) : _p(p) {
            // disengage < engage or the hysteresis chatters at tick rate;
            // 0 < nudge < 1 or the correction points the wrong way / kills
            // the slow-down ratio.
            assert(_p.disengage < _p.engage);
            assert(_p.nudge > 0.0 && _p.nudge < 1.0);
            assert(_p.disengage >= 0.0);
        }

        double Update(double delta);
        void   Reset() { _engaged = false; }  // keeps the lifetime corrections count; _sign re-latches on next engage
        bool   Engaged() const { return _engaged; }
        int    Corrections() const { return _count; }

    private:
        Params _p;
        bool   _engaged = false;
        double _sign    = 0.0;  // sign of delta at engagement
        int    _count   = 0;
    };
}
