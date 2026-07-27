#include "harness.h"
#include "game/CrowdMoodLogic.h"
#include "game/DifficultyTuning.h"

using namespace SH::crowd;

// Feed a stretch of play: `sec` seconds ending at t, all hits or all misses.
static void Play(Mood& m, const Params& p, Sample& acc, double fromSec,
                 double toSec, bool clean) {
    for (double t = fromSec; t <= toSec + 1e-9; t += 0.25) {
        if (clean) { acc.notesHit += 2; } else { acc.notesMissed += 2; }
        acc.songSec = t;
        m.Feed(acc, p);
    }
}

static void RunTests() {
    // The settings surface maps onto the exact shipping gameplay defaults.
    // This keeps merely opening/saving FLICK from changing how a song feels.
    {
        SH::difficulty::Tuning tuning;
        const auto engine = SH::difficulty::EngineParamsFor(tuning);
        CHECK_NEAR(engine.maxWindow, 0.140, 1e-12);
        CHECK_NEAR(engine.minWindow, 0.140, 1e-12);
        CHECK_NEAR(engine.strumLeniency, 0.050, 1e-12);
        CHECK_NEAR(engine.strumLeniencySmall, 0.025, 1e-12);
        CHECK_NEAR(engine.hopoLeniency, 0.080, 1e-12);
        CHECK_NEAR(engine.sustainDropLeniency, 0.025, 1e-12);
        CHECK(engine.antiGhosting);
        CHECK(!engine.infiniteFrontEnd);
        CHECK(engine.maxMultiplier == 4);

        // GH-parity rebalance 2026-07-25.
        const auto rock = SH::difficulty::RockParamsFor(tuning);
        CHECK_NEAR(rock.hitGain, 1.0 / 300.0, 1e-12);
        CHECK_NEAR(rock.badLoss, 4.0 / 300.0, 1e-12);
        CHECK_NEAR(rock.spHitScale, 1.25, 1e-12);
        CHECK_NEAR(rock.spBadScale, 0.80, 1e-12);
        CHECK_NEAR(rock.openingSec, 8.0, 1e-12);
        CHECK_NEAR(rock.openingBadScale, 0.25, 1e-12);
        CHECK(rock.recoveryHitsRequired == 3);
        CHECK_NEAR(rock.terribleBelow, 1.0 / 3.0, 1e-12);
        CHECK_NEAR(rock.greatAt, 2.0 / 3.0, 1e-12);
        // A mistake must cost a few notes of progress, never a phrase:
        // GH sits near 1:3-1:4, and anything past 1:6 is what made the
        // old build "very easy to fail".
        CHECK(rock.badLoss <= rock.hitGain * 4.5);
        CHECK(rock.badLoss >= rock.hitGain * 2.0);

        // THE SHORT-SONG CEILING (field 2026-07-26). Asserted as BEHAVIOUR
        // and against its own counter-example, not as arithmetic on the
        // constants: the expected value here was derived from the report,
        // which is that a FLAWLESS run of a short BA chart could not please
        // the room however well it was played.
        {
            constexpr int kShortNotes = 28;
            auto playFlawless = [](const RockParams& p, int notes) {
                RockMeter  meter;
                RockSample s;
                s.songSec = 30.0;  // past the opening damp
                for (int i = 1; i <= notes; ++i) {
                    s.notesHit = i;
                    (void)meter.Feed(s, p);
                }
                return meter.Committed();
            };
            // Sized to the song: a perfect short set reaches the green zone.
            CHECK(playFlawless(
                      SH::difficulty::RockParamsFor(tuning, kShortNotes),
                      kShortNotes) == Level::kGreat);
            // THE BUG, kept as the contrast. With the absolute 300-note
            // span, 28 perfect notes move the meter 28/300 = 0.093 - from
            // 0.5 to 0.59, short of greatAt (2/3) and unreachable by any
            // amount of skill. If this ever starts passing as kGreat, the
            // span stopped being the thing that was wrong.
            CHECK(playFlawless(SH::difficulty::RockParamsFor(tuning),
                               kShortNotes) != Level::kGreat);
        }

        // Full-length charts are UNTOUCHED - the entire point of clamping
        // at the tuned span rather than scaling everything.
        CHECK_NEAR(SH::difficulty::RockParamsFor(tuning, 500).hitGain,
                   1.0 / 300.0, 1e-12);
        CHECK_NEAR(SH::difficulty::RockParamsFor(tuning, 2000).hitGain,
                   1.0 / 300.0, 1e-12);
        // ...and a caller with no chart keeps the configured span exactly.
        CHECK_NEAR(SH::difficulty::RockParamsFor(tuning, 0).hitGain,
                   1.0 / 300.0, 1e-12);
        // The bad:good RATIO is the tuned quantity and survives the
        // rescale: a short song is not a more forgiving song.
        {
            const auto shortRock =
                SH::difficulty::RockParamsFor(tuning, 28);
            CHECK_NEAR(shortRock.badLoss / shortRock.hitGain,
                       rock.badLoss / rock.hitGain, 1e-9);
        }
        // The floor binds, so a fragment of a chart cannot produce a meter
        // that one miss empties.
        CHECK_NEAR(MeterSpanForSong(300.0, 5), 40.0, 1e-12);
        CHECK_NEAR(MeterSpanForSong(300.0, 100), 60.0, 1e-12);

        const auto failure = SH::difficulty::FailureParamsFor(tuning);
        CHECK_NEAR(failure.dangerBelow, 0.15, 1e-12);
        CHECK_NEAR(failure.recoverAt, 0.30, 1e-12);
        CHECK_NEAR(failure.graceSec, 5.0, 1e-12);
        CHECK_NEAR(failure.startSec, 8.0, 1e-12);
        CHECK(failure.furtherBadRequired == 4);

        // The headline balance claim, asserted end to end rather than as
        // arithmetic on the constants: from the neutral start, an
        // unanswered run of mistakes past the opening damp must take
        // more than twenty of them to even reach the danger line.
        {
            RockMeter meter;
            RockSample sample;
            sample.songSec = 30.0;   // past openingSec
            int bad = 0;
            while (meter.Sentiment() >= failure.dangerBelow && bad < 200) {
                ++bad;
                sample.notesMissed = bad;
                meter.Feed(sample, rock);
            }
            CHECK(bad > 20);
            // ...and one clean hit still has to be worth something the
            // moment the recovery gate is paid off.
            RockMeter recovering;
            RockSample r;
            r.songSec = 30.0;
            r.notesMissed = 1;
            recovering.Feed(r, rock);
            const double dipped = recovering.Sentiment();
            for (int i = 1; i <= rock.recoveryHitsRequired + 1; ++i) {
                r.notesHit = i;
                recovering.Feed(r, rock);
            }
            CHECK(recovering.Sentiment() > dipped);
        }
    }

    // Normal comments stay suppressed through the lead-in and opening
    // phrase. The boundary is inclusive, so a six-second delay releases at
    // exactly six seconds rather than waiting for another arbitrary tick.
    {
        SH::difficulty::Tuning tuning;
        CHECK_NEAR(tuning.audienceCommentDelaySec, 6.0, 1e-12);
        CHECK(!SH::difficulty::AudienceCommentsAllowed(-1.0, tuning));
        CHECK(!SH::difficulty::AudienceCommentsAllowed(0.0, tuning));
        CHECK(!SH::difficulty::AudienceCommentsAllowed(5.999, tuning));
        CHECK(SH::difficulty::AudienceCommentsAllowed(6.0, tuning));
        CHECK(SH::difficulty::AudienceCommentsAllowed(30.0, tuning));
        tuning.audienceCommentDelaySec = 0.0;
        CHECK(!SH::difficulty::AudienceCommentsAllowed(-0.001, tuning));
        CHECK(SH::difficulty::AudienceCommentsAllowed(0.0, tuning));
    }

    // Malformed INI values fail into bounded, internally ordered settings.
    {
        SH::difficulty::Tuning tuning;
        tuning.hitWindowScale = -10.0;
        tuning.gloryMeterSpanHits = 0.0;
        tuning.gloryRedBelow = 0.90;
        tuning.gloryGreenAt = 0.10;
        tuning.failureDangerBelow = 0.80;
        tuning.failureRecoverAt = -2.0;
        tuning.audienceCommentDelaySec = -5.0;
        tuning.maxMultiplier = 99;
        SH::difficulty::Normalize(tuning);
        CHECK_NEAR(tuning.hitWindowScale, 0.50, 1e-12);
        CHECK_NEAR(tuning.gloryMeterSpanHits, 50.0, 1e-12);
        CHECK(tuning.gloryGreenAt >= tuning.gloryRedBelow);
        CHECK(tuning.failureRecoverAt >= tuning.failureDangerBelow);
        CHECK_NEAR(tuning.audienceCommentDelaySec, 0.0, 1e-12);
        CHECK(tuning.maxMultiplier == 8);
    }

    // --- Guitar Hero-style Glory meter ---------------------------------
    // Field regression (16:54 run): the opening five seconds presented as
    // one early fumble but delivered seven misses plus two overstrums. Full
    // steady-state loss made that first phrase dump most of neutral Glory.
    // The four-second settle window must damp opening damage without making
    // later mistakes cheaper.
    {
        RockParams rp;
        CHECK_NEAR(rp.openingSec, 4.0, 1e-12);
        CHECK_NEAR(rp.openingBadScale, 0.25, 1e-12);

        RockMeter meter;
        RockSample s;
        s.songSec = 3.0;
        s.notesMissed = 7;
        s.overstrums = 2;
        meter.Feed(s, rp);
        CHECK_NEAR(meter.Sentiment(), 0.44375, 1e-12);
        CHECK(meter.Committed() == Level::kMiddling);

        // The same burst after the settle window remains genuinely bad.
        meter.Reset();
        s.songSec = 5.0;
        meter.Feed(s, rp);
        CHECK_NEAR(meter.Sentiment(), 0.275, 1e-12);
        CHECK(meter.Committed() == Level::kTerrible);
    }
    // Recovery is earned as a streak, not bought one hit at a time. After a
    // mistake the first twelve consecutive hits stabilize the performance
    // but do not move the needle. The thirteenth begins recovery. Star Power
    // improves the gain after that gate but cannot bypass it.
    {
        RockParams rp;
        CHECK(rp.recoveryHitsRequired == 12);
        CHECK_NEAR(rp.hitGain, 1.0 / 300.0, 1e-12);
        CHECK_NEAR(rp.spHitScale, 1.25, 1e-12);

        RockMeter meter;
        RockSample s;
        s.songSec = 10.0;
        s.notesMissed = 1;
        meter.Feed(s, rp);
        const double afterMiss = meter.Sentiment();
        s.notesHit = 12;
        meter.Feed(s, rp);
        CHECK_NEAR(meter.Sentiment(), afterMiss, 1e-12);
        s.notesHit = 13;
        meter.Feed(s, rp);
        CHECK_NEAR(meter.Sentiment(), afterMiss + 1.0 / 300.0, 1e-12);

        RockMeter powered;
        RockSample p;
        p.songSec = 10.0;
        p.notesMissed = 1;
        powered.Feed(p, rp);
        const double poweredAfterMiss = powered.Sentiment();
        p.spActive = true;
        p.notesHit = 12;
        powered.Feed(p, rp);
        CHECK_NEAR(powered.Sentiment(), poweredAfterMiss, 1e-12);
        p.notesHit = 13;
        powered.Feed(p, rp);
        CHECK_NEAR(powered.Sentiment(),
                   poweredAfterMiss + (1.0 / 300.0) * 1.25, 1e-12);
    }
    // Eleven good notes followed by another miss never clear the recovery
    // gate. Dense but inconsistent play must continue losing Glory instead
    // of oscillating upward on raw note volume.
    {
        RockParams rp;
        RockMeter meter;
        RockSample s;
        s.songSec = 10.0;
        s.notesMissed = 1;
        meter.Feed(s, rp);
        const double once = meter.Sentiment();
        s.notesHit = 11;
        meter.Feed(s, rp);
        CHECK_NEAR(meter.Sentiment(), once, 1e-12);
        ++s.notesMissed;
        meter.Feed(s, rp);
        CHECK(meter.Sentiment() < once);
        s.notesHit += 11;
        meter.Feed(s, rp);
        CHECK(meter.Sentiment() < once);
    }

    // Field balance contract (2026-07-24): a clean opening should take about
    // fifty notes to climb from neutral into green, Star Power must not halve
    // that journey, and seven unanswered steady-state mistakes from neutral
    // should already put the performance in red.
    {
        RockParams rp;
        CHECK_NEAR(rp.hitGain, 1.0 / 300.0, 1e-12);
        CHECK_NEAR(rp.badLoss, 7.5 / 300.0, 1e-12);
        CHECK_NEAR(rp.spHitScale, 1.25, 1e-12);
        CHECK_NEAR(rp.spBadScale, 0.80, 1e-12);

        RockMeter normal;
        RockSample n;
        n.notesHit = 49;
        CHECK(!normal.Feed(n, rp));
        CHECK(normal.Committed() == Level::kMiddling);
        n.notesHit = 51;
        CHECK(normal.Feed(n, rp));
        CHECK(normal.Committed() == Level::kGreat);

        RockMeter powered;
        RockSample p;
        p.spActive = true;
        p.notesHit = 39;
        CHECK(!powered.Feed(p, rp));
        CHECK(powered.Committed() == Level::kMiddling);
        p.notesHit = 41;
        CHECK(powered.Feed(p, rp));
        CHECK(powered.Committed() == Level::kGreat);

        RockMeter rough;
        RockSample r;
        r.notesMissed = 6;
        CHECK(!rough.Feed(r, rp));
        CHECK(rough.Committed() == Level::kMiddling);
        r.notesMissed = 7;
        CHECK(rough.Feed(r, rp));
        CHECK(rough.Committed() == Level::kTerrible);
    }
    // Failure danger begins in the bottom quarter, only clears after
    // visibly recovering into yellow, and commits after three continuous
    // danger seconds plus two further bad judgments.
    {
        FailureParams fp;
        CHECK_NEAR(fp.dangerBelow, 0.25, 1e-12);
        CHECK_NEAR(fp.recoverAt, 0.35, 1e-12);
        CHECK_NEAR(fp.graceSec, 3.0, 1e-12);
        CHECK(fp.furtherBadRequired == 2);

        FailureGate gate;
        CHECK(!gate.Feed(4.0, 0.24, 1, 0, fp));
        CHECK(gate.Dangerous());
        CHECK(!gate.Feed(6.9, 0.24, 2, 0, fp));
        CHECK(gate.Feed(7.0, 0.34, 3, 0, fp));
        CHECK(gate.State() == FailureState::kFailed);
    }

    // Starts in yellow, gains one step per hit, loses 7.5 hit-steps per miss
    // or overstrum, and Star Power makes recovery stronger while softening a
    // bad judgment. One full meter spans 300 hit-steps: ordinary note
    // density must not fling the needle between zones. The live level is the
    // exact state NPC comments consume.
    {
        RockMeter meter;
        RockParams rp;
        RockSample s;
        CHECK_NEAR(meter.Sentiment(), 0.5, 1e-12);
        CHECK(meter.Committed() == Level::kMiddling);

        s.notesHit = 6;
        CHECK(!meter.Feed(s, rp));
        CHECK_NEAR(meter.Sentiment(), 0.5 + 6.0 / 300.0, 1e-12);
        ++s.notesMissed;
        CHECK(!meter.Feed(s, rp));
        CHECK_NEAR(meter.Sentiment(), 0.495, 1e-12);

        // From dead centre, six further misses remain yellow; the seventh
        // enters red. The later streak gate makes recovery cost much more
        // than this raw 7.5-hit loss ratio.
        s.notesMissed += 6;
        CHECK(!meter.Feed(s, rp));
        CHECK(meter.Committed() == Level::kMiddling);
        s.notesMissed += 1;
        CHECK(meter.Feed(s, rp));
        CHECK(meter.Committed() == Level::kTerrible);
        CHECK(meter.Sentiment() < rp.terribleBelow);

        s.spActive = true;
        s.notesHit += 12;
        CHECK(!meter.Feed(s, rp));
        CHECK(meter.Committed() == Level::kTerrible);
        const double afterGate = meter.Sentiment();
        s.notesHit += 1;
        CHECK(!meter.Feed(s, rp));
        CHECK_NEAR(meter.Sentiment(),
                   afterGate + (1.0 / 300.0) * 1.25, 1e-12);
    }
    {
        RockMeter normal, powered;
        RockParams rp;
        RockSample a, b;
        a.notesHit = b.notesHit = 1;
        b.spActive = true;
        normal.Feed(a, rp);
        powered.Feed(b, rp);
        CHECK(powered.Sentiment() - 0.5 >
              normal.Sentiment() - 0.5);

        ++a.notesMissed;
        ++b.notesMissed;
        normal.Feed(a, rp);
        powered.Feed(b, rp);
        CHECK(0.5 - normal.Sentiment() >
              0.5 - powered.Sentiment());
    }
    {
        RockMeter meter;
        RockParams rp;
        RockSample s;
        s.notesHit = 51;
        meter.Feed(s, rp);
        const double beforeSilence = meter.Sentiment();
        CHECK(!meter.Feed(s, rp));
        CHECK_NEAR(meter.Sentiment(), beforeSilence, 1e-12);

        // Counter rollback is a new/reloaded run, not a giant negative
        // delta. It returns to a clean midpoint and baselines the counters.
        s.notesHit = 0;
        CHECK(meter.Feed(s, rp));
        CHECK_NEAR(meter.Sentiment(), 0.5, 1e-12);
        CHECK(meter.Committed() == Level::kMiddling);
    }

    Params p;  // window 3, great 0.85, terrible 0.55, hold 5, start 4
    // SGT dialogue changing valence is not enough: a non-great commit must
    // also release any celebration idle already running on an audience NPC.
    {
        const auto bad = ReactionFor(Level::kTerrible);
        const auto mid = ReactionFor(Level::kMiddling);
        const auto good = ReactionFor(Level::kGreat);
        CHECK(bad.terrible == 1.0f && bad.good == 0.0f);
        CHECK(mid.terrible == 0.0f && mid.good == 2.0f);
        CHECK(good.terrible == 0.0f && good.good == 1.0f);
        CHECK(bad.releaseCelebration);
        CHECK(mid.releaseCelebration);
        CHECK(!good.releaseCelebration);
    }
    // --- cold start: middling, and nothing commits before startSec ---
    {
        Mood m; Sample a;
        CHECK(m.Committed() == Level::kMiddling);
        Play(m, p, a, 0.0, 3.5, true);
        CHECK(m.Committed() == Level::kMiddling);  // clean, but too early
    }
    // --- a clean run commits kGreat, exactly once ---
    {
        Mood m; Sample a; int changes = 0;
        for (double t = 0.0; t <= 20.0; t += 0.25) {
            a.notesHit += 2; a.songSec = t;
            if (m.Feed(a, p)) { ++changes; }
        }
        CHECK(m.Committed() == Level::kGreat);
        CHECK(changes == 1);
    }
    // --- collapse commits kTerrible only after the hold elapses ---
    {
        Mood m; Sample a;
        Play(m, p, a, 0.0, 20.0, true);
        CHECK(m.Committed() == Level::kGreat);
        Play(m, p, a, 20.25, 22.0, false);          // 1.75s of failure
        CHECK(m.Committed() == Level::kGreat);      // hold not yet met
        Play(m, p, a, 22.25, 32.0, false);
        CHECK(m.Committed() == Level::kTerrible);
    }
    // --- silence holds the level, it is not failure ---
    {
        Mood m; Sample a;
        Play(m, p, a, 0.0, 20.0, true);
        CHECK(m.Committed() == Level::kGreat);
        for (double t = 20.25; t <= 40.0; t += 0.25) {  // no notes at all
            a.songSec = t; m.Feed(a, p);
        }
        CHECK(m.Committed() == Level::kGreat);
    }
    // --- comeback: the whole-song-average regression test ---
    {
        Mood m; Sample a;
        Play(m, p, a, 0.0, 30.0, false);
        CHECK(m.Committed() == Level::kTerrible);
        Play(m, p, a, 30.25, 60.0, true);
        CHECK(m.Committed() == Level::kGreat);
    }
    // --- overstrums count against the window ---
    {
        Mood m; Sample a;
        for (double t = 0.0; t <= 20.0; t += 0.25) {
            a.notesHit += 1; a.overstrums += 3; a.songSec = t;
            m.Feed(a, p);
        }
        CHECK(m.Committed() == Level::kTerrible);
    }
    // --- Reset returns to cold start, and it is behavioural, not just the
    // level: kMiddling is also a brand new Mood's value, so a Reset that
    // forgot _lastRaw would still pass the check above. Prove _lastRaw was
    // actually cleared by confirming a zero-note stretch right after
    // Reset() cannot commit - it only could if a stale value survived.
    {
        Mood m; Sample a;
        Play(m, p, a, 0.0, 20.0, true);
        m.Reset();
        CHECK(m.Committed() == Level::kMiddling);
        Sample b; int changes = 0;
        for (double t = 0.0; t <= 20.0; t += 0.25) {
            b.songSec = t;
            if (m.Feed(b, p)) { ++changes; }
        }
        CHECK(m.Committed() == Level::kMiddling);
        CHECK(changes == 0);
    }
    // --- CRITICAL regression: songSec starts negative (the lead-in before
    // the song proper begins). A clean run must still commit kGreat once
    // startSec/holdSec elapse - the sign of songSec must never be read as
    // a "no pending yet" sentinel.
    {
        Mood m; Sample a;
        Play(m, p, a, -2.0, 20.0, true);
        CHECK(m.Committed() == Level::kGreat);
    }
    // --- CRITICAL regression: zero notes ever struck must never commit.
    // Before the fix, the first feed's empty window fell back to the
    // invented _lastRaw = 1.0 and committed kGreat for total silence.
    {
        Mood m; Sample a; int changes = 0;
        for (double t = 0.0; t <= 20.0; t += 0.25) {
            a.songSec = t;
            if (m.Feed(a, p)) { ++changes; }
            CHECK(m.Committed() == Level::kMiddling);
        }
        CHECK(changes == 0);
    }
    // --- kMiddling is a reachable COMMITTED outcome, not just the default
    // every other test starts from - hold ~70% accuracy (inside the
    // 0.55-0.85 band) long enough to commit after climbing to kGreat.
    {
        Mood m; Sample a;
        Play(m, p, a, 0.0, 20.0, true);
        CHECK(m.Committed() == Level::kGreat);
        int step = 0;
        for (double t = 20.25; t <= 40.0 + 1e-9; t += 0.25) {
            // 7 hits per 10 steps ~ 70%, comfortably inside the band even
            // as the rolling window straddles two phases of the pattern.
            if (step % 10 < 7) { a.notesHit += 1; } else { a.notesMissed += 1; }
            a.songSec = t;
            m.Feed(a, p);
            ++step;
        }
        CHECK(m.Committed() == Level::kMiddling);
    }
    // --- mechanism check: a pending change that flips back before it
    // commits must cancel cleanly, with no commit and no side effect.
    // This runs a deliberately NON-shipping holdSec (20.0, well past
    // windowSec) purely to isolate that cancel path from the rolling
    // window's own memory of the dip, which is a separate concern with
    // its own timing - covered honestly, at the real shipping defaults,
    // by the "SHIPPING-DEFAULT regression" block below.
    {
        Params q = p;
        q.holdSec = 20.0;
        Mood m; Sample a;
        Play(m, p, a, 0.0, 20.0, true);
        CHECK(m.Committed() == Level::kGreat);
        int changes = 0;
        for (double t = 20.25; t <= 21.75 + 1e-9; t += 0.25) {  // 1.5s dip
            a.notesMissed += 2; a.songSec = t;
            if (m.Feed(a, q)) { ++changes; }
        }
        CHECK(m.Committed() == Level::kGreat);
        for (double t = 22.0; t <= 34.0 + 1e-9; t += 0.25) {  // recovers
            a.notesHit += 2; a.songSec = t;
            if (m.Feed(a, q)) { ++changes; }
        }
        CHECK(m.Committed() == Level::kGreat);
        CHECK(changes == 0);
    }
    // --- SHIPPING-DEFAULT regression: this is the test that would have
    // caught "holdSec is a delay, not a suppressor" at the real Params
    // defaults, not a tuned copy - so it stays honest if the defaults are
    // ever retuned again. A "one bad bar" fumble (2.0s, matching a bar at
    // 120 BPM 4/4) in the middle of an otherwise clean run must commit
    // NOTHING: holdSec > windowSec keeps the fumble's raw excursion
    // (at most dipSec + windowSec) inside a single hold window.
    {
        Mood m; Sample a;
        Play(m, p, a, 0.0, 20.0, true);
        CHECK(m.Committed() == Level::kGreat);
        int changes = 0;
        for (double t = 20.25; t <= 22.25 + 1e-9; t += 0.25) {  // one bad bar
            a.notesMissed += 2; a.songSec = t;
            if (m.Feed(a, p)) { ++changes; }
        }
        for (double t = 22.5; t <= 35.0 + 1e-9; t += 0.25) {  // resumes clean
            a.notesHit += 2; a.songSec = t;
            if (m.Feed(a, p)) { ++changes; }
        }
        CHECK(m.Committed() == Level::kGreat);
        CHECK(changes == 0);
    }
    // --- a degenerate windowSec (an INI misconfiguration, or just 0) must
    // not drain the rolling window to a single sample and pin the mood at
    // whatever _lastRaw last held - pure misses must not read as kGreat.
    {
        Params q = p;
        q.windowSec = 0.0;
        Mood m; Sample a;
        for (double t = 0.0; t <= 20.0; t += 0.25) {
            a.notesMissed += 2; a.songSec = t;
            m.Feed(a, q);
        }
        CHECK(m.Committed() != Level::kGreat);
    }

    // Glory is the live raw read of this SAME model. It is neutral before
    // observations and follows the rolling performance rather than the old
    // disconnected EngineStats meter.
    {
        Mood m; Sample a;
        CHECK_NEAR(m.Sentiment(), 0.5, 1e-9);
        Play(m, p, a, 0.0, 10.0, true);
        CHECK(m.Sentiment() > 0.99);
        Play(m, p, a, 10.25, 20.0, false);
        CHECK(m.Sentiment() < 0.01);
    }

    // Field regression: once the chart entered a judgment-free outro, old
    // misses aged out of the rolling deque and Glory rose from TERRIBLE to
    // GREAT despite the counters remaining byte-for-byte unchanged. Time
    // passing is not player performance; without a new judgment the live
    // score and committed crowd state must stay put.
    {
        Mood m; Sample a;
        a.songSec = 5.0;
        m.Feed(a, p);
        // A dense bad phrase occupies the old side of the final window.
        for (double t = 5.25; t <= 7.75 + 1e-9; t += 0.25) {
            a.notesMissed += 2;
            a.songSec = t;
            m.Feed(a, p);
        }
        // The final judgment is a hit. As the old misses age out, the
        // current implementation incorrectly lets this lone hit become the
        // whole rolling window even though nothing else happened.
        ++a.notesHit;
        a.songSec = 8.0;
        m.Feed(a, p);
        const double atOutro = m.Sentiment();
        CHECK(atOutro < p.terribleBelow);
        for (double t = 8.25; t <= 16.0 + 1e-9; t += 0.25) {
            a.songSec = t;  // no hit, miss or overstrum edge
            m.Feed(a, p);
            CHECK_NEAR(m.Sentiment(), atOutro, 1e-12);
            // A pending TERRIBLE observation may legitimately finish its
            // hold during the rest; what cannot happen is a fabricated
            // GREAT observation/commit.
            CHECK(m.Committed() != Level::kGreat);
        }
    }

    // Failure requires low sentiment, three seconds of continuous danger,
    // AND two further bad judgments. Time alone and one miss can never
    // eject the player.
    {
        FailureGate gate;
        FailureParams fp;
        CHECK(!gate.Feed(4.0, 0.10, 1, 0, fp));
        CHECK(gate.Dangerous());
        CHECK(!gate.Feed(20.0, 0.10, 1, 0, fp));  // silence is not failure
        CHECK(!gate.Feed(20.1, 0.10, 2, 0, fp));
        CHECK(gate.Feed(20.2, 0.10, 3, 0, fp));
        CHECK(gate.State() == FailureState::kFailed);
        CHECK(!gate.Feed(21.0, 0.0, 4, 0, fp));   // edge fires once
    }

    // A recovery to the separate 35% clear line fully disarms danger;
    // subsequent misses must earn a fresh grace window and bad-count budget.
    {
        FailureGate gate;
        FailureParams fp;
        CHECK(!gate.Feed(4.0, 0.10, 1, 0, fp));
        CHECK(!gate.Feed(5.0, 0.35, 1, 0, fp));
        CHECK(gate.State() == FailureState::kSafe);
        CHECK(!gate.Feed(10.0, 0.10, 2, 0, fp));
        CHECK(!gate.Feed(13.0, 0.10, 3, 0, fp));
        CHECK(gate.Feed(13.1, 0.10, 4, 0, fp));
    }
}

TEST_MAIN("CrowdMood")
