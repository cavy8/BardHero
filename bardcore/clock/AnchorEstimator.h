#pragma once
#include <cstdint>
#include <limits>

#include "clock/AnchorSeqlock.h"

namespace bard {

    // Song-domain audio position from callback anchors (spec 6):
    //   pos = (frames - startFrame)/sampleRate + (qpcNow - anchor.qpc)*rate
    // The raw frames counter LEADS wall clock by the constant buffering
    // depth (M0: 33-38ms); that offset is common to expected and actual and
    // cancels in the controller delta. Monotonic clamp. Staleness flags a
    // stalled audio thread (-> pause session) at max(250ms, 2x callback
    // period): field 2026-07-19 proved 1-2 period gaps are ordinary
    // process hitches (SGT's performance start: 27-38ms with the device
    // running) that the extrapolation rides through losslessly, while a
    // genuinely dead device crosses 250ms immediately.
    class AnchorEstimator {
    public:
        AnchorEstimator(double sampleRate, double callbackPeriodSec)
            : _rate(sampleRate), _period(callbackPeriodSec) {}

        void SetStartFrame(std::uint64_t f) {
            _start = f;
            ResetMonotonic();
        }
        void ResetMonotonic() { _last = kNoFloor; }

        double Position(const AudioAnchor& a, double qpcNow);
        bool   Stale(const AudioAnchor& a, double qpcNow) const {
            return (qpcNow - a.qpc) > StaleLimitSec();
        }
        double StaleLimitSec() const {
            return 2.0 * _period > kStaleFloorSec ? 2.0 * _period
                                                  : kStaleFloorSec;
        }
        double Period() const { return _period; }  // staleness diagnostics

    private:
        static constexpr double kStaleFloorSec = 0.25;
        static constexpr double kNoFloor =
            -std::numeric_limits<double>::infinity();

        double        _rate, _period;
        std::uint64_t _start = 0;
        double        _last  = kNoFloor;
    };
}
