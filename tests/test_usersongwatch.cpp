#include "harness.h"
#include "game/UserSongWatchLogic.h"

using SH::user_song_watch::QuietPeriod;

// This suite briefly also covered song DELETION (SongDeleteLogic.h). That
// feature was removed on 2026-07-26 by decision: BardHero reads the song
// library and never writes to it. Removing a bad chart is a file-manager
// job. See the note in src/game/SongLibrary.h before re-adding anything
// destructive - the containment rule that guarded it is exactly what made
// it useless, because most of a real library lives inside installed mods.

static void RunTests() {
    QuietPeriod quiet(2.0);
    CHECK(!quiet.Pending());
    CHECK(!quiet.TakeDue(100.0));

    quiet.ObserveChange(10.0);
    CHECK(quiet.Pending());
    CHECK(quiet.Deadline() == 12.0);
    CHECK(!quiet.TakeDue(11.999));

    // A later stem write extends the same batch.
    quiet.ObserveChange(11.5);
    CHECK(quiet.Deadline() == 13.5);
    CHECK(!quiet.TakeDue(13.499));
    CHECK(quiet.TakeDue(13.5));
    CHECK(!quiet.Pending());
    CHECK(!quiet.TakeDue(99.0));  // one settled batch triggers once

    // Invalid configuration fails safe to an immediate quiet period.
    QuietPeriod immediate(-1.0);
    immediate.ObserveChange(20.0);
    CHECK(immediate.TakeDue(20.0));
}

TEST_MAIN("UserSongWatch")
