#include "chart/TempoMap.h"

#include <algorithm>
#include <cassert>

namespace bard {

    void TempoMap::AddBpm(std::uint32_t tick, double bpm) {
        assert(!_finalized);
        _markers.push_back({ tick, bpm, 0.0 });
    }

    void TempoMap::Finalize() {
        // stable_sort keeps insertion order among equal ticks, then the
        // dedupe pass keeps the LAST of each run (spec: last wins).
        std::stable_sort(_markers.begin(), _markers.end(),
                         [](const Marker& a, const Marker& b) {
                             return a.tick < b.tick;
                         });
        std::vector<Marker> unique;
        for (const auto& m : _markers) {
            if (!unique.empty() && unique.back().tick == m.tick) {
                unique.back() = m;
            } else {
                unique.push_back(m);
            }
        }
        _markers = std::move(unique);
        if (_markers.empty() || _markers.front().tick != 0) {
            _markers.insert(_markers.begin(), { 0, 120.0, 0.0 });
        }
        for (std::size_t i = 1; i < _markers.size(); ++i) {
            const auto& p     = _markers[i - 1];
            _markers[i].secAt = p.secAt + (60.0 / p.bpm) *
                                              (_markers[i].tick - p.tick) /
                                              _resolution;
        }
        _finalized = true;
    }

    double TempoMap::SecondsAt(double tick) const {
        assert(_finalized);
        // last marker with marker.tick <= tick. Below the first marker
        // (negative session-clock time during the lead-in) there is no
        // such marker - use the first marker itself so the return line
        // extrapolates linearly at its rate instead of walking off the
        // front of _markers (UB).
        auto it = std::upper_bound(
            _markers.begin(), _markers.end(), tick,
            [](double t, const Marker& m) { return t < m.tick; });
        const Marker& m = (it == _markers.begin()) ? *it : *(it - 1);
        return m.secAt + (60.0 / m.bpm) * (tick - m.tick) / _resolution;
    }

    double TempoMap::TickAt(double seconds) const {
        assert(_finalized);
        // Same below-first-marker extrapolation as SecondsAt, mirrored.
        auto it = std::upper_bound(
            _markers.begin(), _markers.end(), seconds,
            [](double s, const Marker& m) { return s < m.secAt; });
        const Marker& m = (it == _markers.begin()) ? *it : *(it - 1);
        return m.tick + (seconds - m.secAt) * m.bpm / 60.0 * _resolution;
    }

    double TempoMap::BpmAtTick(double tick) const {
        assert(_finalized);
        auto it = std::upper_bound(
            _markers.begin(), _markers.end(), tick,
            [](double t, const Marker& m) { return t < m.tick; });
        return (it == _markers.begin() ? *it : *(it - 1)).bpm;
    }
}
