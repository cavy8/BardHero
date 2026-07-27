// tests/test_sgtstartlead.cpp - suite 21
#include "game/SgtStartLead.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace SH::sgtlead;

namespace {
    bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }
}

int main() {
    // The wind-up is read off _Talent_PlayInstrument.psc: the menu-mode
    // wait (0.3), ForceThirdPerson (0.1) and PlayMusic's own wait (0.1)
    // run before PlayIdle(IdleToPlay) puts the character on screen.
    assert(Near(WindUpSeconds(false), 0.5));

    // A drawn weapon adds SheatheWeapon() + Utility.Wait(2) and nothing
    // else - the difference between the two must be exactly that branch.
    assert(Near(WindUpSeconds(true) - WindUpSeconds(false),
                kSheatheBranchSec));

    // ACCEPTANCE (handoff 2a): the sheathed case must not gain a needless
    // delay. Phase 3 sheathes during browse, so this is the normal path.
    assert(Near(LeadSeconds(2.0, false), 2.0));

    // A weapon still drawn at pick (browse sheathe failed - combat
    // auto-redraw) must push the first note past PlayIdle, with margin.
    assert(LeadSeconds(2.0, true) >= WindUpSeconds(true) + kReadyMarginSec);
    assert(LeadSeconds(2.0, true) > 2.0);

    // The lead never shrinks below the caller's base - a longer configured
    // lead-in wins over our estimate.
    assert(Near(LeadSeconds(5.0, true), 5.0));
    assert(Near(LeadSeconds(5.0, false), 5.0));

    // Monotonic in the branch: drawn is never cheaper than sheathed.
    assert(LeadSeconds(2.0, true) >= LeadSeconds(2.0, false));

    // A degenerate base still yields a lead that covers the wind-up
    // rather than scheduling the first note in the past.
    assert(LeadSeconds(0.0, false) >= WindUpSeconds(false));
    assert(LeadSeconds(-1.0, true) >= WindUpSeconds(true));

    std::puts("all SgtStartLead tests passed");
    return 0;
}
