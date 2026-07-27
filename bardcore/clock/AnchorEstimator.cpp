#include "clock/AnchorEstimator.h"

namespace bard {

    double AnchorEstimator::Position(const AudioAnchor& a, double qpcNow) {
        // signed distance handles the pre-start countdown (frames < start)
        const double base =
            static_cast<double>(static_cast<std::int64_t>(a.frames - _start)) /
            _rate;
        double pos = base + (qpcNow - a.qpc) * a.rate;
        if (pos < _last) pos = _last;  // monotonic clamp (spec 6)
        _last = pos;
        return pos;
    }
}
