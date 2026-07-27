#include "harness.h"
#include "game/GoldScaleMath.h"

using namespace SH::goldscale;

static void RunTests() {
    Params p;  // defaults: accMin 0, accMax 3, diff {0.5,0.75,1.0,1.5}
    // no capture / no notes -> no delta
    CHECK(Delta(0, 10, 10, 3, p) == 0);
    CHECK(Delta(20, 0, 0, 3, p) == 0);
    // perfect Expert: mult = 3.0*1.5 = 4.5 -> +70 on a 20g payout
    CHECK(Delta(20, 10, 10, 3, p) == 70);
    // zero accuracy: mult 0 -> take the whole payout back
    CHECK(Delta(20, 0, 10, 3, p) == -20);
    // neutral point: Hard, accuracy 1/3 -> mult exactly 1 -> 0
    CHECK(Delta(9, 1, 3, 2, p) == 0);
    // 50% on Hard: mult 1.5 -> +5 on 10g
    CHECK(Delta(10, 5, 10, 2, p) == 5);
    // difficulty clamped; hit>total clamped to 1.0
    CHECK(Delta(20, 10, 10, 99, p) == Delta(20, 10, 10, 3, p));
    CHECK(Delta(20, 15, 10, 3, p) == Delta(20, 10, 10, 3, p));
    // hostile settings: negative accMin cannot take more than captured
    Params q = p;
    q.accMin = -5.0;
    CHECK(Delta(20, 0, 10, 3, q) == -20);
    // rounding: Easy=0.5, 90% acc -> mult 1.35; 10*(0.35) = 3.5 -> lround 4
    CHECK(Delta(10, 9, 10, 0, p) == 4);
}

TEST_MAIN("GoldScale")
