#pragma once

namespace SH::Spike3 {
    // Registers the throughput window with FLICK (bSpike3Render=1). Must be
    // called AFTER FUCK::Connect succeeded (house ordering rule).
    void Register();
}
