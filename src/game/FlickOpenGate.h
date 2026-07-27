// src/game/FlickOpenGate.h
#pragma once

// PURE cross-thread gate for first-open FLICK host state.
//
// FUCK's input hook reads its WindowState hash map while the render thread
// mutates that map for a newly visible IWindow. The upstream race has crashed
// Results repeatedly inside IsInputBlocked/do_find. This gate blocks NEW
// downstream input calls, lets any call already inside FUCK drain, then allows
// Results publication. Input resumes after the first Results draw completes.

#include <atomic>

namespace SH::flick_open {
    class Gate {
    public:
        [[nodiscard]] bool TryEnterInput() {
            if (_blocking.load(std::memory_order_seq_cst)) {
                return false;
            }
            _activeInputs.fetch_add(1, std::memory_order_seq_cst);
            // BeginOpen may have landed between the first read and increment.
            // Back out rather than entering the unsafe host lookup.
            if (_blocking.load(std::memory_order_seq_cst)) {
                _activeInputs.fetch_sub(1, std::memory_order_seq_cst);
                return false;
            }
            return true;
        }

        void LeaveInput() {
            _activeInputs.fetch_sub(1, std::memory_order_seq_cst);
        }

        void BeginOpen() {
            _blocking.store(true, std::memory_order_seq_cst);
        }

        [[nodiscard]] bool CanPublish() const {
            return _blocking.load(std::memory_order_seq_cst) &&
                   _activeInputs.load(std::memory_order_seq_cst) == 0;
        }

        void CompleteFirstDraw() {
            _blocking.store(false, std::memory_order_seq_cst);
        }

        void Cancel() {
            _blocking.store(false, std::memory_order_seq_cst);
        }

        [[nodiscard]] bool BlockingInput() const {
            return _blocking.load(std::memory_order_seq_cst);
        }

    private:
        std::atomic<unsigned> _activeInputs{ 0 };
        std::atomic<bool>     _blocking{ false };
    };
}
