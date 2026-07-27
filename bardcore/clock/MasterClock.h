#pragma once

namespace bard {

    // Master session timeline (spec 6). Pure logic: the host samples QPC and
    // passes rawSeconds; bardcore never touches an OS clock.
    //   InputTime  = (raw - offset) * speed          judgment, replays
    //   SongTime   = InputTime + audioCal * speed    audio interactions
    //   VisualTime = InputTime + videoCal * speed    highway render
    // Pause freezes outputs; Resume/SetSpeed rebase offset so outputs are
    // continuous (spec: observable movement < 1ms; here exact up to
    // rounding). ResumeSynced
    // rebases onto an externally observed InputTime (the audio position) so
    // pause cycles cannot accumulate audio-vs-clock skew.
    class MasterClock {
    public:
        static constexpr double kSongStartDelay = 2.0;  // spec 6 lead-in

        // Song position 0 (InputTime 0) lands at rawStart + kSongStartDelay.
        // speed > 0
        void Start(double rawStart, double songSpeed = 1.0);
        void SetCalibration(double audioSec, double videoSec);

        bool Paused() const { return _paused; }
        void Pause(double rawNow);
        void Resume(double rawNow);
        void ResumeSynced(double rawNow, double inputTime);
        void SetSpeed(double rawNow, double speed);

        double InputTime(double rawNow) const;
        double SongTime(double rawNow) const;
        double VisualTime(double rawNow) const;
        double Speed() const { return _speed; }

    private:
        double _offset = 0.0, _speed = 1.0;
        double _audioCal = 0.0, _videoCal = 0.0;
        bool   _paused = false;
        double _frozen = 0.0;  // InputTime while paused
    };
}
