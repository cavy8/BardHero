#include "harness.h"
#include "game/AudienceLifecycleLogic.h"
#include "game/SgtPerformLogic.h"

using namespace SH::sgtperform;

static void RunTests() {
    using SH::audience::Action;
    // A fresh/unfinished quest only needs stage 10. Stage 10 is already
    // healthy. Stage 20/200 belongs to an older performance's applause/
    // teardown and must be reset before stage 10 can start a speaking scene.
    CHECK(SH::audience::Plan(0) == Action::kSetStage10);
    CHECK(SH::audience::Plan(9) == Action::kSetStage10);
    CHECK(SH::audience::Plan(10) == Action::kNone);
    CHECK(SH::audience::Plan(19) == Action::kNone);
    CHECK(SH::audience::Plan(20) == Action::kResetAndSetStage10);
    CHECK(SH::audience::Plan(200) == Action::kResetAndSetStage10);

    // A crowd failure needs a short humiliation beat. The negative line is
    // dispatched immediately while SGT's audience and terrible globals are
    // still live; teardown waits long enough for the bark to begin. A new
    // performance stays blocked throughout that hold so the delayed cleanup
    // cannot kill its spell or audience.
    SH::audience::FailureFeedbackSequence failure;
    const auto beginFailure = failure.Begin(100.0);
    CHECK(beginFailure.dispatchFeedback);
    CHECK(beginFailure.stopAudience);
    CHECK(!beginFailure.teardown);
    CHECK(beginFailure.blockStarts);
    CHECK(failure.Pending());

    const auto heldFailure = failure.Poll(101.74);
    CHECK(!heldFailure.dispatchFeedback);
    CHECK(!heldFailure.stopAudience);
    CHECK(!heldFailure.teardown);
    CHECK(heldFailure.blockStarts);
    CHECK(failure.Pending());

    const auto dueFailure = failure.Poll(101.75);
    CHECK(!dueFailure.dispatchFeedback);
    CHECK(!dueFailure.stopAudience);
    CHECK(dueFailure.teardown);
    CHECK(!dueFailure.blockStarts);
    CHECK(!failure.Pending());

    const auto idleFailure = failure.Poll(200.0);
    CHECK(!idleFailure.dispatchFeedback);
    CHECK(!idleFailure.stopAudience);
    CHECK(!idleFailure.teardown);
    CHECK(!idleFailure.blockStarts);

    Config c;  // defaults: 5s cadence, 3 probes

    // kOff never ticks
    Logic off{ c };
    CHECK(!off.NextTick(0.0, true, true).run);

    // first tick fires immediately at session start, then rate-limits
    Logic l{ c };
    l.OnSessionStart(100.0);
    CHECK(l.NextTick(100.0, true, true).run);
    CHECK(!l.NextTick(102.0, true, true).run);
    CHECK(l.NextTick(105.1, true, true).run);

    // exact tick boundary: rate limit is a_now < nextTick_, so equality RUNS
    // (next boundary after the 105.1 run is 110.1)
    CHECK(!l.NextTick(110.0, true, true).run);
    CHECK(l.NextTick(110.1, true, true).run);

    // Once the SGT effect is found, its SongToPlay variable is still filled
    // asynchronously about a second later. Startup passes must poll quickly
    // until that SFX instance is stopped; the steady 5s keeper cadence lets
    // SGT's competing bard clip play over BardHero for several seconds.
    Logic startup{ c };
    startup.OnSessionStart(0.0);
    CHECK(startup.NextTick(0.0, true, true).run);
    startup.Observe(Seen::kPresent, false);
    CHECK(startup.NextTick(0.01, true, true).run);
    CHECK(!startup.NextTick(0.05, true, true).run);
    CHECK(startup.NextTick(0.11, true, true).run);
    startup.Observe(Seen::kPresent, true);
    CHECK(startup.NextTick(0.22, true, true).run);
    CHECK(!startup.NextTick(5.21, true, true).run);
    CHECK(startup.NextTick(5.23, true, true).run);

    // probing -> live on a present observation; keepIdle only live+playing+enabled
    Logic p{ c };
    p.OnSessionStart(0.0);
    CHECK(p.NextTick(0.0, true, true).keepIdle == false);  // probing
    p.Observe(Seen::kPresent);
    CHECK(p.state() == Logic::State::kLive);
    CHECK(p.Confirmed());
    CHECK(p.NextTick(5.1, true, true).keepIdle);
    CHECK(!p.NextTick(10.2, false, true).keepIdle);  // paused
    CHECK(!p.NextTick(15.3, true, false).keepIdle);  // INI off

    // probing -> dormant after 3 absent observations; dormant never ticks
    Logic d{ c };
    d.OnSessionStart(0.0);
    d.Observe(Seen::kAbsent);
    d.Observe(Seen::kAbsent);
    CHECK(d.state() == Logic::State::kProbing);
    d.Observe(Seen::kAbsent);
    CHECK(d.state() == Logic::State::kDormant);
    CHECK(!d.NextTick(60.0, true, true).run);
    CHECK(!d.Confirmed());

    // cross-session reset: production reuses ONE long-lived instance - a new
    // session must re-probe from scratch (a leaked probes_ counter would tip
    // to kDormant on the first absent)
    d.OnSessionStart(70.0);
    CHECK(d.state() == Logic::State::kProbing);
    d.Observe(Seen::kAbsent);
    d.Observe(Seen::kAbsent);
    CHECK(d.state() == Logic::State::kProbing);
    d.Observe(Seen::kAbsent);
    CHECK(d.state() == Logic::State::kDormant);

    // live -> lost latch on absent; unknown observations never change state
    Logic x{ c };
    x.OnSessionStart(0.0);
    x.Observe(Seen::kUnknown);
    CHECK(x.state() == Logic::State::kProbing);
    x.Observe(Seen::kPresent);
    x.Observe(Seen::kAbsent);
    CHECK(x.Lost());
    CHECK(x.Confirmed());                       // lost still = SGT session
    CHECK(!x.NextTick(20.0, true, true).run);   // keeper stops when lost

    // session end resets to kOff
    x.OnSessionEnd();
    CHECK(x.state() == Logic::State::kOff);

    // pause recovery: kLost -> Recover -> probing again, immediately ticking
    Logic r{ c };
    r.OnSessionStart(0.0);
    r.Observe(Seen::kPresent);
    r.Observe(Seen::kAbsent);
    CHECK(r.Lost());
    r.Recover(50.0);
    CHECK(r.state() == Logic::State::kProbing);
    CHECK(r.NextTick(50.0, true, true).run);  // first re-probe fires now
    r.Observe(Seen::kPresent);
    CHECK(r.state() == Logic::State::kLive);

    // Recover from kLive is legal (death between keeper passes is invisible
    // to the session - resume always re-probes); probes_ restarts so three
    // fresh absents are tolerated before dormant
    r.Recover(60.0);
    CHECK(r.state() == Logic::State::kProbing);
    r.Observe(Seen::kAbsent);
    r.Observe(Seen::kAbsent);
    CHECK(r.state() == Logic::State::kProbing);

    // Recover is a no-op from kOff / kProbing / kDormant
    Logic n{ c };
    n.Recover(0.0);
    CHECK(n.state() == Logic::State::kOff);
    n.OnSessionStart(0.0);
    n.Recover(1.0);
    CHECK(n.state() == Logic::State::kProbing);
    n.Observe(Seen::kAbsent);
    n.Observe(Seen::kAbsent);
    n.Observe(Seen::kAbsent);
    CHECK(n.state() == Logic::State::kDormant);
    n.Recover(2.0);
    CHECK(n.state() == Logic::State::kDormant);

    // ---- browse standstill ----------------------------------------------

    // kOff inert; Begin -> first pass immediate, then rate-limited
    Standstill s;
    CHECK(!s.NextTick(0.0).run);
    s.Begin(10.0);
    auto sp = s.NextTick(10.0);
    CHECK(sp.run);
    CHECK(!sp.strip);
    CHECK(!s.NextTick(10.2).run);

    // settle -> stop seen -> linger pass -> strip pass -> done
    s.Observe(Standstill::Pass::kStarting);
    CHECK(s.state() == Standstill::State::kSettling);
    sp = s.NextTick(10.6);
    CHECK(sp.run && !sp.strip);
    s.Observe(Standstill::Pass::kStopped);
    CHECK(s.state() == Standstill::State::kLinger);
    sp = s.NextTick(11.1);  // linger pass: one more UnregisterForUpdate
    CHECK(sp.run && !sp.strip);
    sp = s.NextTick(11.6);  // strip pass
    CHECK(sp.run && sp.strip);
    CHECK(s.state() == Standstill::State::kDone);
    CHECK(!s.Active());
    CHECK(!s.NextTick(20.0).run);

    // effect vanishing on its own ends it at any stage (no strip)
    Standstill a;
    a.Begin(0.0);
    CHECK(a.NextTick(0.0).run);
    a.Observe(Standstill::Pass::kAbsent);
    CHECK(a.state() == Standstill::State::kDone);
    CHECK(!a.NextTick(5.0).run);

    // ...including between stop and strip
    Standstill a2;
    a2.Begin(0.0);
    a2.NextTick(0.0);
    a2.Observe(Standstill::Pass::kStopped);
    a2.NextTick(0.6);  // -> kStripReady
    a2.Observe(Standstill::Pass::kAbsent);
    CHECK(a2.state() == Standstill::State::kDone);
    CHECK(!a2.NextTick(1.2).run);

    // fast pick cancels before the strip; kUnknown never changes state
    Standstill f;
    f.Begin(0.0);
    f.NextTick(0.0);
    f.Observe(Standstill::Pass::kUnknown);
    CHECK(f.state() == Standstill::State::kSettling);
    f.Cancel();
    CHECK(!f.Active());
    CHECK(!f.NextTick(1.0).run);

    // waitLog fires exactly once, at the 20th settling pass
    Standstill w;
    w.Begin(0.0);
    int logs = 0;
    for (int i = 0; i < 30; ++i) {
        const auto wp = w.NextTick(i * 0.6);
        CHECK(wp.run);
        if (wp.waitLog) { ++logs; }
    }
    CHECK(logs == 1);

    // frozen passes (browse world-pause) still run but never count toward
    // the waitLog - a long frozen browse must not fire it; unfrozen passes
    // afterwards resume the count and it still fires exactly once
    Standstill z;
    z.Begin(0.0);
    int zlogs = 0;
    for (int i = 0; i < 40; ++i) {  // ~24s frozen: passes run, no waitLog
        const auto zp = z.NextTick(i * 0.6, true);
        CHECK(zp.run);
        if (zp.waitLog) { ++zlogs; }
    }
    CHECK(zlogs == 0);
    for (int i = 40; i < 80; ++i) {  // unfrozen (cancel path): count resumes
        const auto zp = z.NextTick(i * 0.6, false);
        if (zp.waitLog) { ++zlogs; }
    }
    CHECK(zlogs == 1);

    // a reused instance restarts clean (production keeps one long-lived
    // Standstill on the session thread)
    w.Begin(100.0);
    CHECK(w.state() == Standstill::State::kSettling);
    CHECK(w.NextTick(100.0).run);
}

TEST_MAIN("SgtPerform")
