#include "harness.h"
#include "chart/SongIni.h"

using namespace bard;

static void RunTests() {
    {   // case-insensitive section+keys, booleans, TMP tag strip
        SongIniValues v;
        CHECK(ParseSongIniText(
            "[song]\nNAME = <b>My</b> Song\nDelay = 200\n"
            "hopo_frequency = 100\neighthnote_hopo = True\n"
            "sustain_cuttoff_threshold = 99\nmultiplier_note = 116\n"
            "diff_guitar = 4\n",
            v));
        CHECK(v.name == "My Song");          // rich-text stripped
        CHECK_NEAR(v.delayMs, 200.0, 1e-9);  // |200| >= 100 -> ms
        CHECK(v.hopoFrequency == 100);
        CHECK(v.eighthNoteHopo == true);
        CHECK(v.sustainCutoff == 99);        // CH's shipped typo key
        CHECK(v.multiplierNote == 116);
        CHECK(v.diffGuitar == 4);
    }
    {   // delay seconds heuristic: |delay| < 100 is almost certainly seconds
        SongIniValues v;
        CHECK(ParseSongIniText("[Song]\ndelay = 1.5\n", v));
        CHECK(v.hasDelay);
        CHECK_NEAR(v.delayMs, 1500.0, 1e-9);
        SongIniValues w;
        CHECK(ParseSongIniText("[Song]\ndelay = -2\n", w));
        CHECK_NEAR(w.delayMs, -2000.0, 1e-9);
    }
    {   // correctly-spelled cutoff key wins over the typo, in either order
        SongIniValues v;
        CHECK(ParseSongIniText("[Song]\nsustain_cuttoff_threshold = 11\n"
                               "sustain_cutoff_threshold = 22\n",
                               v));
        CHECK(v.sustainCutoff == 22);
        SongIniValues w;
        CHECK(ParseSongIniText("[Song]\nsustain_cutoff_threshold = 22\n"
                               "sustain_cuttoff_threshold = 11\n",
                               w));
        CHECK(w.sustainCutoff == 22);
    }
    {   // multiplier_note: 103/116 ONLY; anything else ignored
        SongIniValues v;
        CHECK(ParseSongIniText("[Song]\nstar_power_note = 42\n", v));
        CHECK(v.multiplierNote == -1);
    }
    {   // threshold resolution precedence + format defaults
        SongIniValues none;
        CHECK(ResolveHopoThreshold(none, 192, false) == 65);
        CHECK(ResolveHopoThreshold(none, 480, false) == 162);
        CHECK(ResolveHopoThreshold(none, 480, true) == 161);
        SongIniValues eighth;
        eighth.eighthNoteHopo = true;
        CHECK(ResolveHopoThreshold(eighth, 480, true) == 240);   // PINNED res/2
        SongIniValues freq;
        freq.hopoFrequency  = 100;
        freq.eighthNoteHopo = true;
        CHECK(ResolveHopoThreshold(freq, 480, true) == 100);     // freq wins
    }
    {   // cutoff resolution: ini wins both formats; else mid res/3, chart 0
        SongIniValues none;
        CHECK(ResolveSustainCutoff(none, 480, true) == 160);
        CHECK(ResolveSustainCutoff(none, 480, false) == 0);
        SongIniValues set;
        set.sustainCutoff = 5;
        CHECK(ResolveSustainCutoff(set, 480, false) == 5);
    }
    {   // end_events default true, parseable off
        SongIniValues v;
        CHECK(ParseSongIniText("[Song]\nend_events = False\n", v));
        CHECK(v.endEvents == false);
    }
    {   // single_instrument: default false, 1/true on, 0 off
        SongIniValues a;
        CHECK(ParseSongIniText("[song]\nname = X\n", a));
        CHECK(a.singleInstrument == false);
        SongIniValues b;
        CHECK(ParseSongIniText("[song]\nsingle_instrument = 1\n", b));
        CHECK(b.singleInstrument == true);
        SongIniValues c;
        CHECK(ParseSongIniText("[song]\nsingle_instrument = True\n", c));
        CHECK(c.singleInstrument == true);
        SongIniValues d;
        CHECK(ParseSongIniText("[song]\nsingle_instrument = 0\n", d));
        CHECK(d.singleInstrument == false);
    }
    {   // instrument tag: lute|flute|drum|guitar, lowercase normalized;
        // anything else = empty (untagged)
        SongIniValues v;
        CHECK(ParseSongIniText("[song]\ninstrument = Lute\n", v));
        CHECK(v.instrument == "lute");
        SongIniValues v2;
        CHECK(ParseSongIniText("[song]\ninstrument = DRUM\n", v2));
        CHECK(v2.instrument == "drum");
        SongIniValues v2b;
        CHECK(ParseSongIniText("[song]\ninstrument = flute\n", v2b));
        CHECK(v2b.instrument == "flute");
        SongIniValues v2c;
        CHECK(ParseSongIniText("[song]\ninstrument =  GuItAr \n", v2c));
        CHECK(v2c.instrument == "guitar");
        SongIniValues v3;
        CHECK(ParseSongIniText("[song]\ninstrument = banjo\n", v3));
        CHECK(v3.instrument.empty());
        SongIniValues v4;
        CHECK(ParseSongIniText("[song]\nname = x\n", v4));
        CHECK(v4.instrument.empty());
    }
    {   // unlock_rank is parsed; absent is -1
        bard::SongIniValues v;
        CHECK(bard::ParseSongIniText("[song]\nunlock_rank = 3\n", v));
        CHECK(v.unlockRank == 3);
        bard::SongIniValues v2;
        CHECK(bard::ParseSongIniText("[song]\nname = x\n", v2));
        CHECK(v2.unlockRank == -1);
    }
    {   // a malformed unlock_rank is atoi's 0, which is OUT of the 1..5 band
        // RequiredRank() honors - so garbage falls through to diff_guitar
        // rather than gating the chart at some bogus rank.
        SongIniValues v;
        CHECK(ParseSongIniText("[song]\nunlock_rank = banana\n", v));
        CHECK(v.unlockRank == 0);
    }
}

TEST_MAIN("SongIni")
