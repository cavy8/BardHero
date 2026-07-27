#pragma once

namespace bard {
    // Timing parameters, spec 5.1 verbatim. All seconds.
    struct EngineParams {
        double maxWindow           = 0.140;
        double minWindow           = 0.140;
        bool   isDynamic           = false;
        double frontToBackRatio    = 1.0;  // front=-(W/2)r, back=+(W/2)(2-r)
        double strumLeniency       = 0.050;
        double strumLeniencySmall  = 0.025;
        double hopoLeniency        = 0.080;
        double sustainDropLeniency = 0.025;
        bool   infiniteFrontEnd    = false;
        bool   antiGhosting        = true;
        int    maxMultiplier       = 4;
        // dynamic-window shape (Precision preset values; spec 5.1)
        double dynamicScale = 1.0, dynamicSlope = 0.93, dynamicGamma = 1.5;

        static EngineParams Default() { return {}; }
        static EngineParams Casual() {
            EngineParams p;
            p.infiniteFrontEnd = true;
            p.antiGhosting     = false;
            p.strumLeniency += 0.010;
            p.strumLeniencySmall += 0.010;
            return p;
        }
        static EngineParams Precision() {
            EngineParams p;
            p.maxWindow = 0.120;
            p.minWindow = 0.040;
            p.isDynamic = true;
            p.strumLeniency -= 0.010;
            p.strumLeniencySmall -= 0.010;
            return p;
        }
    };
}
