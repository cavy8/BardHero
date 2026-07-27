#include "harness.h"
#include "game/StarsLogic.h"

using namespace SH::stars;

static void RunTests() {
    // ---- stars from accuracy (defaults 0.50/0.65/0.80/0.90/0.96) ----
    StarParams sp;
    CHECK(StarsFromAccuracy(0.00, sp) == 0);
    CHECK(StarsFromAccuracy(0.49, sp) == 0);
    CHECK(StarsFromAccuracy(0.50, sp) == 1);   // boundary inclusive
    CHECK(StarsFromAccuracy(0.64, sp) == 1);
    CHECK(StarsFromAccuracy(0.65, sp) == 2);
    CHECK(StarsFromAccuracy(0.80, sp) == 3);
    CHECK(StarsFromAccuracy(0.90, sp) == 4);
    CHECK(StarsFromAccuracy(0.9599, sp) == 4);
    CHECK(StarsFromAccuracy(0.96, sp) == 5);
    CHECK(StarsFromAccuracy(1.00, sp) == 5);

    // ---- long-performance XP feed (starts after a two-minute set) ----
    FeedParams fp;  // base 3.0, minAcc 0.5
    CHECK(FeedBonus(1.00, 210.0, fp) == 2);   // 3.5min: (1.75-1)*3
    CHECK(FeedBonus(0.80, 210.0, fp) == 2);
    CHECK(FeedBonus(0.49, 210.0, fp) == 0);   // below accuracy floor
    CHECK(FeedBonus(1.00, 120.0, fp) == 0);   // reference-length set
    CHECK(FeedBonus(1.00, 35.0, fp) == 0);    // short BA performance
    CHECK(FeedBonus(1.00, 300.0, fp) == 5);   // long set still matters

    // ---- promotion floor (5*->66 Pretty Good, 4*->46 OK) ----
    PromotionParams pp;
    CHECK(PromotionFloor(5, pp) == 66);
    CHECK(PromotionFloor(4, pp) == 46);
    CHECK(PromotionFloor(3, pp) == 0);
    CHECK(PromotionFloor(0, pp) == 0);

    // ---- rank gate: lifted mask + clamp ceiling ----
    GateParams gp;  // need3 = {2,3,4,5}, boundary85 also needs one 5*
    GateCounts c;
    CHECK(UpdateLifted(0, c, gp) == 0);
    CHECK(ClampCeiling(0) == 25);
    c.chartsAt3 = 2;
    CHECK(UpdateLifted(0, c, gp) == 0b0001);
    CHECK(ClampCeiling(0b0001) == 45);
    c.chartsAt3 = 4;
    CHECK(UpdateLifted(0, c, gp) == 0b0111);
    CHECK(ClampCeiling(0b0111) == 85);
    c.chartsAt3 = 5;                      // 5 charts but no 5* yet
    CHECK(UpdateLifted(0, c, gp) == 0b0111);
    c.chartsAt5 = 1;
    CHECK(UpdateLifted(0, c, gp) == 0b1111);
    CHECK(ClampCeiling(0b1111) == 100);
    // lifted is monotonic: counts dropping (charts uninstalled) never
    // re-clamps a lifted boundary
    c = GateCounts{};
    CHECK(UpdateLifted(0b1111, c, gp) == 0b1111);

    // ---- promotion restore arithmetic ----
    // actual 10, promoted to 66, SGT granted +3 during payout (69):
    // final base = actual + grant = 13
    CHECK(RestoredValue(10, 66, 69) == 13);
    // no promotion (promoted == actual): plain passthrough
    CHECK(RestoredValue(50, 50, 53) == 53);
    // defensive: VM weirdness making cur < promoted never subtracts
    CHECK(RestoredValue(10, 66, 60) == 10);
}

TEST_MAIN("Stars")
