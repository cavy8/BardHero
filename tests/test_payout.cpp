#include "harness.h"
#include "game/PayoutMath.h"

#include <limits>

using namespace SH::payout;

static void RunTests() {
    Params p;  // base 20, outside 0.35, renown@1 0.40, cap 60
    const int kGreat = 2, kMid = 1, kTerrible = 0;

    // a cheering room at Master, Expert, in an inn: base * 1 * 1 * 1.5 * 1 * 1
    CHECK(Deserved(5, kGreat, 3, 5, true, p) == 30);
    // the SAME run at rank 1 pays less, but NOT nothing - the headline
    const int r1 = Deserved(5, kGreat, 3, 1, true, p);
    const int r5 = Deserved(5, kGreat, 3, 5, true, p);
    CHECK(r1 > 0);
    CHECK(r1 < r5);
    CHECK(r1 == 12);              // 30 * 0.40
    // the renown curve's midpoints, not just its anchors - a bad divisor
    // or an off-by-one on (r - 1) would still pass at rank 1 and rank 5
    CHECK(Deserved(5, kGreat, 3, 2, true, p) == 17);
    CHECK(Deserved(5, kGreat, 3, 3, true, p) == 21);
    CHECK(Deserved(5, kGreat, 3, 4, true, p) == 26);
    // a cold room pays nothing whatever the stars or rank
    CHECK(Deserved(5, kTerrible, 3, 5, true, p) == 0);
    // moodPayTerrible is a configurable escape hatch, gated on the VALUE
    // not the level - a "simplify to moodLevel == 0" refactor would
    // silently break this without any other test noticing
    Params q = p; q.moodPayTerrible = 0.25;
    CHECK(Deserved(5, kTerrible, 3, 5, true, q) == 8);
    // below one star pays nothing even to a cheering room
    CHECK(Deserved(0, kGreat, 3, 5, true, p) == 0);
    // middling room pays half
    CHECK(Deserved(5, kMid, 3, 5, true, p) == 15);
    // busking outside an inn still pays a great run
    const int out = Deserved(5, kGreat, 3, 1, false, p);
    CHECK(out > 0);
    CHECK(out < r1);
    // the cap bounds a maximal stack
    Params big = p; big.buskBase = 1000.0;
    CHECK(Deserved(5, kGreat, 3, 5, true, big) == big.cap);
    // difficulty scales it
    CHECK(Deserved(5, kGreat, 0, 5, true, p) <
          Deserved(5, kGreat, 3, 5, true, p));
    // rank and difficulty are clamped, never out of range
    CHECK(Deserved(5, kGreat, 99, 99, true, p) ==
          Deserved(5, kGreat, 3, 5, true, p));
    CHECK(Deserved(5, kGreat, -5, -5, true, p) ==
          Deserved(5, kGreat, 0, 1, true, p));

    // --- the room's DOMINANT mood, not the one at the final note ---------
    // an empty tally is the mood model's own cold start, not a booing room:
    // a song too short to feed must never pay zero for that reason
    MoodTally empty;
    CHECK(empty.Dominant() == kMid);
    // a clear majority reports itself
    MoodTally clear;
    clear.Add(kGreat, 12.0);
    clear.Add(kMid, 3.0);
    CHECK(clear.Dominant() == kGreat);
    // THE REGRESSION this type exists for: 170s of a cheering room, then 10s
    // of fumbling over the last notes. The COMMITTED level at song end reads
    // terrible and pays nothing; the tally still says the room was great.
    MoodTally endFumble;
    endFumble.Add(kGreat, 170.0);
    endFumble.Add(kTerrible, 10.0);
    CHECK(endFumble.Dominant() == kGreat);
    CHECK(Deserved(4, endFumble.Dominant(), 3, 5, true, p) > 0);
    CHECK(Deserved(4, kTerrible, 3, 5, true, p) == 0);  // what it replaces
    // ties resolve UPWARD - `stars` has already priced the sloppy half
    MoodTally tieLow;
    tieLow.Add(kTerrible, 30.0);
    tieLow.Add(kMid, 30.0);
    CHECK(tieLow.Dominant() == kMid);
    MoodTally tieHigh;
    tieHigh.Add(kMid, 8.0);
    tieHigh.Add(kGreat, 8.0);
    CHECK(tieHigh.Dominant() == kGreat);
    // an out-of-range level is dropped, never written out of bounds
    MoodTally oob;
    oob.Add(-1, 100.0);
    oob.Add(3, 100.0);
    oob.Add(99, 100.0);
    CHECK(oob.Dominant() == kMid);
    CHECK(oob.sec[0] == 0.0);
    CHECK(oob.sec[2] == 0.0);
    // a non-positive interval is dropped too (a clock that went backwards,
    // or the very first feed of a session)
    MoodTally nonPos;
    nonPos.Add(kGreat, -5.0);
    nonPos.Add(kTerrible, 0.0);
    CHECK(nonPos.Dominant() == kMid);
    nonPos.Add(kTerrible, 4.0);
    CHECK(nonPos.Dominant() == kTerrible);

    // --- the two-star earning bar (field ask 2026-07-22) -----------------
    // A 0%-accuracy run was still being paid, because SGT pays on rank. The
    // purse now starts at 2 stars.
    CHECK(p.minStars == 2);
    CHECK(Deserved(0, kGreat, 3, 5, true, p) == 0);
    CHECK(Deserved(1, kGreat, 3, 5, true, p) == 0);
    CHECK(Deserved(2, kGreat, 3, 5, true, p) > 0);
    // monotonic across the whole star range at a fixed room - more stars is
    // never worth less, and the rescale never dips below the bar
    for (int s = 1; s <= 5; ++s) {
        CHECK(Deserved(s, kGreat, 3, 5, true, p) >=
              Deserved(s - 1, kGreat, 3, 5, true, p));
    }
    // the range is RESCALED onto 2..5, not just truncated: two stars pays a
    // quarter of a five-star night, and five still pays the full 30
    CHECK(Deserved(5, kGreat, 3, 5, true, p) == 30);
    CHECK(Deserved(2, kGreat, 3, 5, true, p) == 8);   // 30 * 0.25
    CHECK(Deserved(3, kGreat, 3, 5, true, p) == 15);  // 30 * 0.50
    CHECK(Deserved(4, kGreat, 3, 5, true, p) == 23);  // 30 * 0.75
    // the bar is configurable, and moving it moves the floor AND the curve
    Params lenient = p; lenient.minStars = 1;
    CHECK(Deserved(1, kGreat, 3, 5, true, lenient) == 6);   // 30 * 0.2
    CHECK(Deserved(5, kGreat, 3, 5, true, lenient) == 30);
    Params harsh = p; harsh.minStars = 4;
    CHECK(Deserved(3, kGreat, 3, 5, true, harsh) == 0);
    CHECK(Deserved(4, kGreat, 3, 5, true, harsh) == 15);    // 30 * 0.5
    CHECK(Deserved(5, kGreat, 3, 5, true, harsh) == 30);
    // an out-of-range bar clamps instead of dividing by zero or paying all
    Params silly = p; silly.minStars = 99;
    CHECK(Deserved(4, kGreat, 3, 5, true, silly) == 0);
    CHECK(Deserved(5, kGreat, 3, 5, true, silly) == 30);
    Params zero = p; zero.minStars = 0;
    CHECK(Deserved(0, kGreat, 3, 5, true, zero) == 0);
    CHECK(Deserved(5, kGreat, 3, 5, true, zero) == 30);

    // --- song length scales the purse (field ask 2026-07-22) -------------
    // A longer set is more work and pays for it. The old 5-arg call means
    // "a song at the reference length", so every figure above still holds.
    CHECK(Deserved(5, kGreat, 3, 5, true, p) ==
          Deserved(5, kGreat, 3, 5, true, p.lengthRefSec, p));
    CHECK(LengthMult(p.lengthRefSec, p) == 1.0);
    CHECK(LengthMult(60.0, p) == 0.5);    // a minute: half
    CHECK(LengthMult(240.0, p) == 2.0);   // four minutes: double
    // monotonic in length, and CLAMPED at both ends so one absurd chart
    // cannot mint the cap and a stub cannot pay nothing
    CHECK(LengthMult(10.0, p) == p.lengthMin);
    CHECK(LengthMult(99999.0, p) == p.lengthMax);
    double prev = 0.0;
    for (double sec = 10.0; sec <= 400.0; sec += 10.0) {
        const double mult = LengthMult(sec, p);
        CHECK(mult >= prev);
        CHECK(mult >= p.lengthMin);
        CHECK(mult <= p.lengthMax);
        prev = mult;
    }
    // it actually moves the gold, in the right direction
    CHECK(Deserved(5, kGreat, 3, 5, true, 60.0, p) <
          Deserved(5, kGreat, 3, 5, true, 240.0, p));
    // ...and it cannot resurrect a run that earned nothing on its own -
    // length multiplies the purse, it does not create one
    CHECK(Deserved(1, kGreat, 3, 5, true, 600.0, p) == 0);
    CHECK(Deserved(5, kTerrible, 3, 5, true, 600.0, p) == 0);
    // a garbage or missing length falls to the FLOOR, never to a free 1.0 -
    // a caller that forgets to pass one must not be rewarded for it
    CHECK(LengthMult(0.0, p) == p.lengthMin);
    CHECK(LengthMult(-30.0, p) == p.lengthMin);
    // a disabled reference switches the whole factor off rather than
    // dividing by zero
    Params noLen = p; noLen.lengthRefSec = 0.0;
    CHECK(LengthMult(9.0, noLen) == 1.0);
    CHECK(LengthMult(9000.0, noLen) == 1.0);
    // inverted bounds are tolerated rather than producing an empty range
    Params flipped = p; flipped.lengthMin = 2.0; flipped.lengthMax = 0.5;
    CHECK(LengthMult(60.0, flipped) >= 0.5);
    CHECK(LengthMult(60.0, flipped) <= 2.0);
    // the cap still bounds a long song at max settings
    Params bigLen = p; bigLen.buskBase = 1000.0;
    CHECK(Deserved(5, kGreat, 3, 5, true, 600.0, bigLen) == bigLen.cap);

    // --- nobody around, nobody pays (field 2026-07-28) ------------------
    // an empty mountainside pays zero for a flawless set - THE bug
    CHECK(Deserved(5, kGreat, 3, 5, true, 120.0, 0.0, p) == 0);
    // a full room pays exactly the pre-audience figure, which is also the
    // no-count fallback callers use: fullAt as audience == the old purse
    CHECK(Deserved(5, kGreat, 3, 5, true, 120.0,
                   static_cast<double>(p.audienceFullAt), p) ==
          Deserved(5, kGreat, 3, 5, true, 120.0, p));
    // more than a full room never pays more than one
    CHECK(Deserved(5, kGreat, 3, 5, true, 120.0, 25.0, p) ==
          Deserved(5, kGreat, 3, 5, true, 120.0, p));
    // one listener is the busking fantasy, not 1/fullAt of a crowd:
    // it pays the lone share (30 * 0.40 = 12)
    CHECK(Deserved(5, kGreat, 3, 5, true, 120.0, 1.0, p) == 12);
    // the factor is linear between lone and full, and monotonic
    CHECK(AudienceMult(1.0, p) == 0.40);
    CHECK(AudienceMult(2.5, p) == 0.70);   // 0.4 + 0.6 * 1.5/3
    CHECK(AudienceMult(4.0, p) == 1.0);
    CHECK(AudienceMult(2.0, p) > AudienceMult(1.0, p));
    CHECK(AudienceMult(3.0, p) > AudienceMult(2.0, p));
    // below one listener scales the lone share - present for 40% of the
    // song is 40% of a lone patron, and never rounds up to a purse
    CHECK(AudienceMult(0.5, p) == 0.20);
    CHECK(Deserved(5, kGreat, 3, 5, true, 120.0, 0.01, p) == 0);
    // garbage counts pay nothing rather than everything
    CHECK(AudienceMult(-3.0, p) == 0.0);
    CHECK(AudienceMult(std::numeric_limits<double>::quiet_NaN(), p) == 0.0);
    // a degenerate fullAt of 1 (or below, clamped to 1) still ramps 0..1
    // and pays whole from the first listener on
    Params solo = p; solo.audienceFullAt = 1;
    CHECK(AudienceMult(1.0, solo) == 1.0);
    CHECK(AudienceMult(0.5, solo) == 0.5);
    Params degenerate = p; degenerate.audienceFullAt = -2;
    CHECK(AudienceMult(1.0, degenerate) == 1.0);
    // audience scales the purse, it cannot create one
    CHECK(Deserved(5, kTerrible, 3, 5, true, 120.0, 25.0, p) == 0);
    CHECK(Deserved(0, kGreat, 3, 5, true, 120.0, 25.0, p) == 0);

    // --- top-up is a shortfall, never a second payment, never negative ---
    CHECK(TopUp(30, 0) == 30);    // SGT paid nothing (rank 1-3)
    CHECK(TopUp(30, 12) == 18);   // partial -> exactly the gap
    CHECK(TopUp(30, 30) == 0);    // already paid enough
    CHECK(TopUp(30, 45) == 0);    // paid MORE - never claw back
    CHECK(TopUp(0, 0) == 0);
}

TEST_MAIN("Payout")
