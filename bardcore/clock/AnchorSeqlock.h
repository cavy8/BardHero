#pragma once
#include <atomic>
#include <cstdint>

namespace bard {

    struct AudioAnchor {
        std::uint64_t frames = 0;  // engine frames consumed since device start
        double        qpc    = 0.0;  // rawSeconds when published
        double        rate   = 1.0;  // engine-frames-per-device-frame ratio;
                                     // 0.0 while paused (freezes interpolation)
    };

    // Single-writer seqlock (spec 6): the audio callback publishes, host
    // threads read. The unsynchronized payload copy is the standard seqlock
    // idiom - formally a data race (TSan will flag it), safe on x86-64/MSVC
    // for trivially-copyable PODs because seq validation discards torn
    // copies. Read() spins only while a write is in flight; a writer dying
    // mid-publish (device teardown) would hang it - callers stop reading
    // once the session pauses on the staleness guard.
    class AnchorSeqlock {
    public:
        void Publish(const AudioAnchor& a) {
            const auto s = _seq.load(std::memory_order_relaxed) + 1;
            _seq.store(s, std::memory_order_relaxed);             // odd: write begins
            std::atomic_thread_fence(std::memory_order_release);  // odd visible before payload
            _anchor = a;
            std::atomic_thread_fence(std::memory_order_release);  // payload before even
            _seq.store(s + 1, std::memory_order_release);         // even: write ends
        }
        AudioAnchor Read() const {
            for (;;) {
                const auto s1 = _seq.load(std::memory_order_acquire);
                if (s1 & 1) continue;
                const AudioAnchor a = _anchor;
                std::atomic_thread_fence(std::memory_order_acquire);
                if (_seq.load(std::memory_order_acquire) == s1) return a;
            }
        }
        bool Published() const {
            return _seq.load(std::memory_order_acquire) != 0;
        }

    private:
        std::atomic<std::uint64_t> _seq{ 0 };
        AudioAnchor                _anchor;
    };
}
