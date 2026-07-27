#include "harness.h"
#include "game/SongChallengeLogic.h"
#include "game/UnlockLogic.h"

#include <string>
#include <vector>

using namespace SH::unlock;

static void RunTests() {
    // SGT's paid-bard fragments emit this exact legacy skill notification.
    // BardHero replaces the lesson payoff with a learned song, so only this
    // exact sentence is hidden; similar native/mod notifications survive.
    CHECK(ShouldSuppressLessonNotification(
        "Your musical talent increases"));
    CHECK(!ShouldSuppressLessonNotification(
        "Your musical talent increased"));
    CHECK(!ShouldSuppressLessonNotification(
        "Your musical talent increases."));
    CHECK(!ShouldSuppressLessonNotification("New song learned: Ragnar"));
    CHECK(!ShouldSuppressLessonNotification(""));
    CHECK(LearnedSongNotification("Ragnar the Red", "lute") ==
          "New lute song learned: Ragnar the Red");
    CHECK(LearnedSongNotification("The Dragonborn Comes", " FLUTE ") ==
          "New flute song learned: The Dragonborn Comes");
    CHECK(LearnedSongNotification("Age of Aggression", "DrUm") ==
          "New drum song learned: Age of Aggression");
    CHECK(LearnedSongNotification("Age of Voltage", "guitar") ==
          "New guitar song learned: Age of Voltage");
    CHECK(LearnedSongNotification("Legacy Song", "") ==
          "New song learned: Legacy Song");
    CHECK(LearnedSongNotification("Malformed Song", "lyre") ==
          "New song learned: Malformed Song");

    // --- chart-measured challenge ---------------------------------------
    using SH::song_challenge::Metrics;
    using SH::song_challenge::NaturalRequiredRank;
    using SH::song_challenge::RankInput;
    using SH::song_challenge::RankLibrary;
    using SH::song_challenge::Score;

    const Metrics easy{ 78.0, 0.8, 1.2, 34.0 };
    const Metrics medium{ 135.0, 2.4, 3.8, 120.0 };
    const Metrics hard{ 168.0, 3.4, 5.6, 220.0 };
    CHECK(Score(easy) < Score(medium));
    CHECK(Score(medium) < Score(hard));
    CHECK(NaturalRequiredRank(Score(easy)) == 2);
    CHECK(NaturalRequiredRank(Score(medium)) == 3);
    CHECK(NaturalRequiredRank(Score(hard)) == 5);

    // Two easiest measured songs are always known PER INSTRUMENT. Existing
    // blanket song.ini overrides do not defeat measurement. A missing
    // measurement keeps the legacy metadata fallback.
    std::vector<RankInput> measured = {
        { 0, Score(medium), 5, "lute-b" },
        { 0, Score(easy),   5, "lute-a" },
        { 0, Score(hard),   2, "lute-c" },
        { 1, Score(hard),   5, "flute-c" },
        { 1, Score(easy),   5, "flute-a" },
        { 1, Score(medium), 5, "flute-b" },
        { 2, 0.0,           4, "drum-unmeasured", false },
        { 3, Score(hard),   5, "guitar-c" },
        { 3, Score(easy),   5, "guitar-a" },
        { 3, Score(medium), 5, "guitar-b" }
    };
    const auto natural = RankLibrary(measured, 2);
    CHECK(natural.size() == measured.size());
    CHECK(natural[0] == 1);  // second-easiest lute starter
    CHECK(natural[1] == 1);  // easiest lute starter
    CHECK(natural[2] == 5);  // measured hard song ignores override rank 2
    CHECK(natural[3] == 5);  // hard flute is the non-starter
    CHECK(natural[4] == 1);
    CHECK(natural[5] == 1);
    CHECK(natural[6] == 4);  // no metrics: metadata fallback survives
    CHECK(natural[7] == 5);  // hard guitar is the non-starter
    CHECK(natural[8] == 1);
    CHECK(natural[9] == 1);

    // Starter ties are deterministic by stable chart key, not scan order.
    std::vector<RankInput> ties = {
        { 2, 20.0, 3, "drum-c" },
        { 2, 20.0, 3, "drum-a" },
        { 2, 20.0, 3, "drum-b" }
    };
    const auto tiedRanks = RankLibrary(ties, 2);
    CHECK(tiedRanks[0] == 2);
    CHECK(tiedRanks[1] == 1);
    CHECK(tiedRanks[2] == 1);

    // A real-sized all-hard repertoire still forms a complete learning
    // ladder by relative difficulty. Absolute score may lower a rank, but
    // never makes the instrument's easiest remaining song jump to rank 4.
    std::vector<RankInput> ladder;
    for (int i = 0; i < 9; ++i) {
        ladder.push_back(
            { 2, 75.0 + i, 5, "drum-" + std::to_string(i) });
    }
    const auto ladderRanks = RankLibrary(ladder, 2);
    CHECK(ladderRanks[0] == 1);
    CHECK(ladderRanks[1] == 1);
    CHECK(ladderRanks[2] == 2);
    CHECK(ladderRanks[3] == 3);
    CHECK(ladderRanks[5] == 4);
    CHECK(ladderRanks[7] == 5);
    CHECK(ladderRanks[8] == 5);

    // --- required rank derivation (spec 6.1) ---
    CHECK(RequiredRank(-1, -1) == 1);   // no difficulty, no override
    CHECK(RequiredRank(0, -1) == 1);
    CHECK(RequiredRank(1, -1) == 1);
    CHECK(RequiredRank(2, -1) == 2);
    CHECK(RequiredRank(3, -1) == 3);
    CHECK(RequiredRank(4, -1) == 4);
    CHECK(RequiredRank(5, -1) == 5);
    CHECK(RequiredRank(6, -1) == 5);    // above the scale clamps
    CHECK(RequiredRank(99, -1) == 5);
    // an explicit override always wins
    CHECK(RequiredRank(5, 2) == 2);
    CHECK(RequiredRank(-1, 4) == 4);
    // a nonsense override is ignored rather than making a chart unreachable
    CHECK(RequiredRank(2, 0) == 2);
    CHECK(RequiredRank(2, 99) == 2);

    // --- the gate ---
    CHECK(IsUnlocked(1, 1, false));
    CHECK(!IsUnlocked(3, 2, false));
    CHECK(IsUnlocked(3, 3, false));
    CHECK(IsUnlocked(5, 1, true));      // taught by a bard beats any rank
    // taught survives a rank drop
    CHECK(IsUnlocked(4, 1, true));

    // --- an unsampled playerRank (-1) must not lock the whole library ---
    CHECK(IsUnlocked(1, -1, false));    // rank-1 chart stays open, unsampled
    CHECK(!IsUnlocked(3, -1, false));   // higher tiers still lock
    CHECK(IsUnlocked(5, -1, true));     // taught still beats everything

    // --- teaching picks the LOWEST-tier locked chart ---
    std::vector<int>  req   = { 1, 4, 2, 5 };
    std::vector<bool> taught(4, false);
    CHECK(PickTeachingTarget(req, taught, 1) == 2);   // the rank-2 chart
    taught[2] = true;
    CHECK(PickTeachingTarget(req, taught, 1) == 1);   // then the rank-4 one
    taught[1] = true;
    CHECK(PickTeachingTarget(req, taught, 1) == 3);
    taught[3] = true;
    CHECK(PickTeachingTarget(req, taught, 1) == -1);  // nothing left
    // charts already open by rank are never chosen
    std::vector<int>  req2 = { 1, 2 };
    std::vector<bool> t2(2, false);
    CHECK(PickTeachingTarget(req2, t2, 5) == -1);
    CHECK(PickLessonTarget(req2, t2, 5) == 0);
    t2[0] = true;
    CHECK(PickLessonTarget(req2, t2, 5) == 1);
    t2[1] = true;
    CHECK(PickLessonTarget(req2, t2, 5) == -1);
    // empty library is a no-op, not a crash
    std::vector<int>  none;
    std::vector<bool> noneT;
    CHECK(PickTeachingTarget(none, noneT, 1) == -1);

    // --- teaching for an unsampled player (-1) must not treat it as
    // rank-omniscient - the rank-1 chart is still open, so a lesson must
    // buy the rank-3 one instead ---
    std::vector<int>  reqNeg = { 1, 3 };
    std::vector<bool> tNeg(2, false);
    CHECK(PickTeachingTarget(reqNeg, tNeg, -1) == 1);

    // --- a tie: two locked charts at the same required rank picks the
    // FIRST by index, and is deterministic across repeated calls ---
    std::vector<int>  reqTie = { 3, 3 };
    std::vector<bool> tTie(2, false);
    CHECK(PickTeachingTarget(reqTie, tTie, 1) == 0);
    CHECK(PickTeachingTarget(reqTie, tTie, 1) == 0);   // same call, same answer

    // --- a taught vector LONGER than requiredRanks is safe: the excess
    // tail belongs to no chart and must never be read ---
    std::vector<int>  reqLong = { 1, 3 };
    std::vector<bool> tLong   = { false, false, true, true };
    CHECK(PickTeachingTarget(reqLong, tLong, 1) == 1);

    // --- the bard-lesson edge (spec 6.3) ---
    // A lesson raises ALL THREE globals. This is the whole basis for
    // telling one apart from our own writes, so it is tested hard.
    {
        // the ordinary lesson: +10 to all three
        const int prev[3] = { 5, 5, 5 };
        const int now[3]  = { 15, 15, 15 };
        CHECK(IsLessonEdge(prev, now, 3, 5));
        CHECK(IsLessonEdge(prev, now, 3, 1));
    }
    {
        // the Orc bard (Bard_FragmentOrc.psc): same 100 gold, +1 to all
        // three. Registers at step 1 and is REJECTED by the old step of 5.
        const int prev[3] = { 40, 12, 7 };
        const int now[3]  = { 41, 13, 8 };
        CHECK(IsLessonEdge(prev, now, 3, 1));
        CHECK(!IsLessonEdge(prev, now, 3, 5));
    }
    {
        // ONE global rises, hugely: the promotion bracket writing
        // max(cur, promote5Floor). NOT a lesson, at any threshold.
        const int prev[3] = { 10, 10, 10 };
        const int now[3]  = { 66, 10, 10 };
        CHECK(!IsLessonEdge(prev, now, 3, 1));
    }
    {
        // TWO of three - still not a lesson. This is the case a naive
        // "most of them moved" rule would wave through.
        const int prev[3] = { 5, 5, 5 };
        const int now[3]  = { 15, 15, 5 };
        CHECK(!IsLessonEdge(prev, now, 3, 1));
        const int now2[3] = { 15, 5, 15 };
        CHECK(!IsLessonEdge(prev, now2, 3, 1));
        const int now3[3] = { 5, 15, 15 };
        CHECK(!IsLessonEdge(prev, now3, 3, 1));
    }
    {
        // all three FALL: the post-load clamp sweep. Monotone-down is why
        // ClampPass is allowed to touch all three at once.
        const int prev[3] = { 60, 60, 60 };
        const int now[3]  = { 25, 25, 25 };
        CHECK(!IsLessonEdge(prev, now, 3, 1));
    }
    {
        // mixed: one falls while two rise (a clamp landing in the same
        // poll window as a lesson). Rejected - a missed unlock beats a
        // phantom one, and the next lesson still registers.
        const int prev[3] = { 20, 20, 30 };
        const int now[3]  = { 30, 30, 25 };
        CHECK(!IsLessonEdge(prev, now, 3, 1));
    }
    {
        // no movement at all
        const int same[3] = { 25, 25, 25 };
        CHECK(!IsLessonEdge(same, same, 3, 1));
    }
    {
        // an unsampled baseline (-1, and the -1 ReadGlob returns for an
        // absent or non-finite global) can never make a lesson - this is
        // what stops the first poll after a load from firing.
        const int prev[3] = { -1, 5, 5 };
        const int now[3]  = { 15, 15, 15 };
        CHECK(!IsLessonEdge(prev, now, 3, 1));
        const int prevOk[3] = { 5, 5, 5 };
        const int nowBad[3] = { 15, -1, 15 };
        CHECK(!IsLessonEdge(prevOk, nowBad, 3, 1));
    }
    {
        // degenerate arguments are refusals, not crashes
        const int prev[3] = { 5, 5, 5 };
        const int now[3]  = { 15, 15, 15 };
        CHECK(!IsLessonEdge(nullptr, now, 3, 1));
        CHECK(!IsLessonEdge(prev, nullptr, 3, 1));
        CHECK(!IsLessonEdge(prev, now, 0, 1));
        CHECK(!IsLessonEdge(prev, now, 3, 0));
    }
}

TEST_MAIN("Unlock")
