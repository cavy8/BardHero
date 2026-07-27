#include "clock/MasterClock.h"

#include <cassert>

namespace bard {

    void MasterClock::Start(double rawStart, double songSpeed) {
        assert(songSpeed > 0.0);
        _speed  = songSpeed;
        _offset = rawStart + kSongStartDelay;
        _paused = false;
    }

    void MasterClock::SetCalibration(double audioSec, double videoSec) {
        _audioCal = audioSec;
        _videoCal = videoSec;
    }

    void MasterClock::Pause(double rawNow) {
        if (_paused) return;
        _frozen = InputTime(rawNow);
        _paused = true;
    }

    void MasterClock::Resume(double rawNow) {
        if (!_paused) return;
        ResumeSynced(rawNow, _frozen);
    }

    void MasterClock::ResumeSynced(double rawNow, double inputTime) {
        assert(_speed > 0.0);
        _offset = rawNow - inputTime / _speed;
        _paused = false;
    }

    void MasterClock::SetSpeed(double rawNow, double speed) {
        assert(speed > 0.0);
        const double t = InputTime(rawNow);
        _speed         = speed;
        _offset        = rawNow - t / _speed;
        // frozen InputTime is speed-invariant; no update needed while paused
    }

    double MasterClock::InputTime(double rawNow) const {
        return _paused ? _frozen : (rawNow - _offset) * _speed;
    }
    double MasterClock::SongTime(double rawNow) const {
        return InputTime(rawNow) + _audioCal * _speed;
    }
    double MasterClock::VisualTime(double rawNow) const {
        return InputTime(rawNow) + _videoCal * _speed;
    }
}
