#include "harness.h"
#include "game/SongCache.h"

using namespace SH;

static void RunTests() {
    {   // round-trip (last field = instrument)
        std::vector<SongCacheEntry> in(2);
        in[0] = { "C:/songs/A", 111, "Alpha", "Artist A", "ch", 90000.0, 3,
                  "drum" };
        in[1] = { "C:/songs/B", 222, "Beta", "", "", -1.0, -1, "" };
        const auto text = SerializeSongCache(in);
        const auto out  = ParseSongCache(text);
        CHECK(out.size() == 2);
        CHECK(out[0].folder == "C:/songs/A" && out[0].chartMtime == 111);
        CHECK(out[0].name == "Alpha" && out[0].artist == "Artist A");
        CHECK_NEAR(out[0].lengthMs, 90000.0, 1e-9);
        CHECK(out[0].instrument == "drum");
        CHECK(out[1].diff == -1 && out[1].artist.empty());
        CHECK(out[1].instrument.empty());
    }
    {   // back-compat: the parser is strict on column count (7 fields was
        // the pre-instrument shape, now 8 are required), so an OLD-format
        // line is rejected outright rather than parsed with instrument
        // defaulted to empty. That is an accepted cache miss (forces a
        // rescan of that folder), not a bug - see task 4 notes.
        const auto out = ParseSongCache(
            "skyherocache\t1\nF\t1\ta\tb\tc\t0\t0\n");
        CHECK(out.empty());
    }
    {   // tabs/newlines in names are sanitized, not corrupting
        std::vector<SongCacheEntry> in(1);
        in[0] = { "F", 1, "bad\tname\nx", "a", "c", 0.0, 0 };
        const auto out = ParseSongCache(SerializeSongCache(in));
        CHECK(out.size() == 1);
        CHECK(out[0].name == "bad name x");
    }
    {   // wrong version header -> empty; malformed lines skipped
        CHECK(ParseSongCache("nonsense\t9\nF\t1\ta\tb\tc\t0\t0\tlute")
                  .empty());
        const auto out = ParseSongCache(
            "skyherocache\t1\nF\tNOTANUMBER\ta\tb\tc\t0\t0\tlute\n"
            "G\t5\tName\tArt\tCh\t100\t2\tflute\n"
            "short\tline\n");
        CHECK(out.size() == 1);
        CHECK(out[0].folder == "G" && out[0].chartMtime == 5);
        CHECK(out[0].instrument == "flute");
    }
    {   // unlockRank survives a cache round-trip
        std::vector<SH::SongCacheEntry> in(1);
        in[0].folder     = "songs/Fire";
        in[0].diff       = 4;
        in[0].unlockRank = 2;
        const auto out = SH::ParseSongCache(SH::SerializeSongCache(in));
        CHECK(out.size() == 1);
        CHECK(out[0].unlockRank == 2);
    }
    {   // back-compat: unlockRank is column 9, appended AFTER the format
        // shipped, so a cache file already on disk ends at the instrument.
        // A short row must keep the -1 "derive from diff" sentinel, not be
        // dropped (that would rescan every folder on the first launch after
        // the upgrade) and not be left as garbage.
        const auto out = ParseSongCache(
            "skyherocache\t1\nF\t1\ta\tb\tc\t0\t2\tlute\n"
            "G\t2\ta\tb\tc\t0\t2\t\n");   // old row, untagged instrument
        CHECK(out.size() == 2);
        CHECK(out[0].instrument == "lute" && out[0].unlockRank == -1);
        CHECK(out[1].instrument.empty() && out[1].unlockRank == -1);
        // and the new shape still splits instrument off correctly when the
        // instrument itself is empty
        const auto n = ParseSongCache(
            "skyherocache\t1\nH\t3\ta\tb\tc\t0\t2\t\t5\n");
        CHECK(n.size() == 1);
        CHECK(n[0].instrument.empty() && n[0].unlockRank == 5);
    }
    CHECK(ParseSongCache("").empty());
}

TEST_MAIN("SongCache")
