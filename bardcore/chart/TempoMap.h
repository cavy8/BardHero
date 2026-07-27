#pragma once
#include <cstdint>
#include <vector>

namespace bard {

    // Tick <-> seconds resolver built from the chart's BPM markers. double
    // throughout - float drifts audibly over a 5-minute song (spec 4.5).
    // All parse-time and engine time math funnels through this one class.
    class TempoMap {
    public:
        void          SetResolution(std::uint32_t res) { _resolution = res; }
        std::uint32_t Resolution() const { return _resolution; }

        // Any order; duplicates on one tick keep the last-added.
        void AddBpm(std::uint32_t tick, double bpm);
        // Sort + cumulative seconds. Inserts the 120 BPM default at tick 0
        // when absent. Must be called before any lookup.
        void Finalize();

        double SecondsAt(double tick) const;  // fractional ticks allowed
        double TickAt(double seconds) const;  // piecewise-linear inverse
        double BpmAtTick(double tick) const;  // active marker at this tick

    private:
        struct Marker {
            std::uint32_t tick  = 0;
            double        bpm   = 120.0;
            double        secAt = 0.0;
        };
        std::uint32_t       _resolution = 192;
        std::vector<Marker> _markers;
        bool                _finalized = false;
    };
}
