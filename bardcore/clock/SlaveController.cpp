#include "clock/SlaveController.h"

#include <cmath>

namespace bard {

    double SlaveController::Update(double delta) {
        const double mag = std::abs(delta);
        if (!_engaged) {
            if (mag > _p.engage) {
                _engaged = true;
                _sign    = delta > 0.0 ? 1.0 : -1.0;
                ++_count;
            }
        } else if (mag < _p.disengage || delta * _sign < 0.0) {
            _engaged = false;  // converged, or overshot past zero (spec 6)
        }
        if (!_engaged) return 1.0;
        // delta > 0: audio behind expected -> consume engine frames faster
        // (resampler ratio = input/output > 1).
        return 1.0 + _p.nudge * _sign;
    }
}
