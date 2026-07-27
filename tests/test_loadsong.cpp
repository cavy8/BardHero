// tests/test_loadsong.cpp
#include "harness.h"
#include "chart/LoadSong.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

using namespace bard;
namespace fs = std::filesystem;

static fs::path gRoot;

static void WriteText(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

static SongEntry MakeEntry(const fs::path& folder, bool isMid) {
    SongEntry e;
    e.folder    = folder;
    e.chartFile = folder / (isMid ? "notes.mid" : "notes.chart");
    e.isMid     = isMid;
    if (fs::exists(folder / "song.ini")) e.iniFile = folder / "song.ini";
    e.stems["song"] = folder / "song.ogg";  // LoadSong never opens stems
    return e;
}

static const char* kChart =
    "[Song]\n{\n  Resolution = 192\n  Offset = 0.5\n  Name = \"ChartName\"\n}\n"
    "[SyncTrack]\n{\n  0 = B 120000\n}\n"
    "[ExpertSingle]\n{\n"
    "  0 = N 0 0\n"
    "  90 = N 1 0\n"      // gap 90 ticks: HOPO iff threshold >= 90
    "  192 = N 2 100\n"   // 100-tick sustain
    "}\n";

// Minimal SMF builder (same shape as tests/test_midparser.cpp - repeated
// here so this suite stands alone).
struct SmfBuilder {
    std::vector<std::uint8_t> bytes;
    void U16(std::uint16_t v) { bytes.push_back(v >> 8); bytes.push_back(v & 0xFF); }
    void U32(std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) bytes.push_back((v >> s) & 0xFF);
    }
    void Raw(std::initializer_list<std::uint8_t> b) { bytes.insert(bytes.end(), b); }
    void Str(const std::string& s) {
        for (char c : s) bytes.push_back(static_cast<std::uint8_t>(c));
    }
    void Vlq(std::uint32_t v) {
        std::uint8_t buf[4];
        int          n = 0;
        do { buf[n++] = v & 0x7F; v >>= 7; } while (v);
        while (n--) bytes.push_back(buf[n] | (n ? 0x80 : 0));
    }
    // Meta length is a VLQ and is DERIVED here: hand-counted lengths
    // silently truncate the payload instead of failing the parse.
    void Meta(std::uint8_t type, const std::string& s) {
        Raw({ 0xFF, type });
        Vlq(static_cast<std::uint32_t>(s.size()));
        Str(s);
    }
    void Header(std::uint16_t ntrks, std::uint16_t division) {
        Raw({ 'M', 'T', 'h', 'd' }); U32(6); U16(1); U16(ntrks); U16(division);
    }
    std::size_t BeginTrack() { Raw({ 'M', 'T', 'r', 'k' }); U32(0); return bytes.size(); }
    void EndTrack(std::size_t start) {
        Vlq(0); Raw({ 0xFF, 0x2F, 0x00 });
        const auto len = static_cast<std::uint32_t>(bytes.size() - start);
        for (int s = 0; s < 4; ++s) bytes[start - 4 + s] = (len >> (24 - 8 * s)) & 0xFF;
    }
};

static void RunTests() {
    gRoot = fs::temp_directory_path() / "skyhero_loadsong_tests";
    fs::remove_all(gRoot);

    // ResolveDifficulty: requested, then downward, then upward; -1 on an
    // empty mask; out-of-range wants clamp to Expert. The Songbook, the
    // session record and LoadSong itself all ride this one function.
    CHECK(ResolveDifficulty(0b1111, 3) == 3);
    CHECK(ResolveDifficulty(0b1111, 0) == 0);
    CHECK(ResolveDifficulty(0b0100, 3) == 2);   // Hard-only chart, Expert ask
    CHECK(ResolveDifficulty(0b1000, 0) == 3);   // Expert-only, Easy ask: up
    CHECK(ResolveDifficulty(0b1001, 2) == 0);   // down beats up: Easy, not X
    CHECK(ResolveDifficulty(0b1001, 3) == 3);
    CHECK(ResolveDifficulty(0b0000, 3) == -1);
    CHECK(ResolveDifficulty(0b0100, -7) == 2);  // bad want = Expert ask
    CHECK(ResolveDifficulty(0b0100, 9) == 2);

    {   // .chart, no ini: chart offset applied, format-default hopo (65 @192)
        const auto dir = gRoot / "plain";
        WriteText(dir / "notes.chart", kChart);
        LoadedSong  s;
        std::string err;
        CHECK(LoadSong(MakeEntry(dir, false), 3, s, &err));
        CHECK(s.chart.notes.size() == 3);
        CHECK(s.resolvedDifficulty == 3);       // Expert section, as asked
        CHECK(s.difficultyMask == 0b1000);      // ...and it is all there is
        CHECK_NEAR(s.chart.notes[0].time, 0.5, 1e-9);          // Offset only
        CHECK(!s.chart.notes[1].isHopo);                       // 90 > 65
        CHECK(s.chart.notes[2].sustainTicks[2] == 100);        // kept
        CHECK(s.chart.meta.name == "ChartName");
        // 120 BPM, res 192: one beat = 0.5s
        CHECK_NEAR(s.chart.tempo.SecondsAt(192.0), 0.5, 1e-9);
        CHECK_NEAR(s.chart.notes[2].time, 0.5 + 0.5, 1e-9);
    }
    {   // ini overlays: delay, hopo_frequency, sustain cutoff, name authority
        const auto dir = gRoot / "overlaid";
        WriteText(dir / "notes.chart", kChart);
        WriteText(dir / "song.ini",
                  "[song]\nname = IniName\ndelay = 200\nhopo_frequency = 100\n"
                  "sustain_cutoff_threshold = 160\n");
        LoadedSong s;
        CHECK(LoadSong(MakeEntry(dir, false), 3, s));
        CHECK_NEAR(s.chart.notes[0].time, 0.7, 1e-9);   // 0.5 chart + 0.2 ini
        CHECK(s.chart.notes[1].isHopo);                 // 90 < 100
        CHECK(s.chart.notes[2].sustainTicks[2] == 0);   // 100 < 160 cutoff
        CHECK(s.chart.meta.name == "IniName");          // ini wins
        CHECK(s.ini.hasDelay);
    }
    {   // difficulty fallback: only HardSingle present, Expert requested
        const auto dir = gRoot / "hardonly";
        WriteText(dir / "notes.chart",
                  "[Song]\n{\n  Resolution = 192\n}\n"
                  "[SyncTrack]\n{\n  0 = B 120000\n}\n"
                  "[HardSingle]\n{\n  0 = N 0 0\n}\n");
        LoadedSong s;
        CHECK(LoadSong(MakeEntry(dir, false), 3, s));
        CHECK(s.chart.notes.size() == 1);
        CHECK(s.resolvedDifficulty == 2);   // the record keys on Hard
        CHECK(s.difficultyMask == 0b0100);
    }
    {   // multi-difficulty chart: the mask reports EVERY populated section
        // and the pick honors the down-then-up order per request.
        const auto dir = gRoot / "multidiff";
        WriteText(dir / "notes.chart",
                  "[Song]\n{\n  Resolution = 192\n}\n"
                  "[SyncTrack]\n{\n  0 = B 120000\n}\n"
                  "[EasySingle]\n{\n  0 = N 0 0\n}\n"
                  "[ExpertSingle]\n{\n  0 = N 0 0\n  96 = N 1 0\n}\n");
        LoadedSong s;
        CHECK(LoadSong(MakeEntry(dir, false), 1, s));   // Medium ask
        CHECK(s.difficultyMask == 0b1001);
        CHECK(s.resolvedDifficulty == 0);               // down to Easy
        CHECK(s.chart.notes.size() == 1);               // Easy's one note
        LoadedSong x;
        CHECK(LoadSong(MakeEntry(dir, false), 3, x));
        CHECK(x.resolvedDifficulty == 3);
        CHECK(x.chart.notes.size() == 2);               // Expert's two
    }
    {   // section markers reach the loaded chart (practice mode)
        const auto dir = gRoot / "sections";
        WriteText(dir / "notes.chart",
                  "[Song]\n{\n  Resolution = 192\n}\n"
                  "[SyncTrack]\n{\n  0 = B 120000\n}\n"
                  "[Events]\n{\n"
                  "  0 = E \"section Intro\"\n"
                  "  384 = E \"section Verse 1\"\n"
                  "}\n"
                  "[ExpertSingle]\n{\n  0 = N 0 0\n  384 = N 1 0\n}\n");
        LoadedSong s;
        CHECK(LoadSong(MakeEntry(dir, false), 3, s));
        CHECK(s.chart.sections.size() == 2);
        CHECK(s.chart.sections[0].name == "Intro");
        CHECK(s.chart.sections[1].name == "Verse 1");
        CHECK_NEAR(s.chart.sections[1].time, 1.0, 1e-9);
    }
    {   // no guitar section at all -> false + reason
        const auto dir = gRoot / "nosection";
        WriteText(dir / "notes.chart",
                  "[Song]\n{\n  Resolution = 192\n}\n"
                  "[SyncTrack]\n{\n  0 = B 120000\n}\n");
        LoadedSong  s;
        std::string err;
        CHECK(!LoadSong(MakeEntry(dir, false), 3, s, &err));
        CHECK(!err.empty());
    }
    {   // missing chart file -> false
        LoadedSong  s;
        std::string err;
        CHECK(!LoadSong(MakeEntry(gRoot / "missing", false), 3, s, &err));
        CHECK(!err.empty());
    }
    {   // end_events honored by default, disabled by ini. NOTE: M1's proven
        // contract reads "E end" from the TRACK section (test_chartparser
        // does exactly this), so the fixture places it there.
        const auto body =
            "[Song]\n{\n  Resolution = 192\n}\n"
            "[SyncTrack]\n{\n  0 = B 120000\n}\n"
            "[ExpertSingle]\n{\n  0 = N 0 0\n  100 = E end\n  192 = N 1 0\n}\n";
        const auto d1 = gRoot / "endev_on";
        WriteText(d1 / "notes.chart", body);
        LoadedSong s1;
        CHECK(LoadSong(MakeEntry(d1, false), 3, s1));
        CHECK(s1.chart.notes.size() == 1);          // truncated at tick 100
        const auto d2 = gRoot / "endev_off";
        WriteText(d2 / "notes.chart", body);
        WriteText(d2 / "song.ini", "[song]\nend_events = 0\n");
        LoadedSong s2;
        CHECK(LoadSong(MakeEntry(d2, false), 3, s2));
        CHECK(s2.chart.notes.size() == 2);          // truncation disabled
    }
    {   // .mid: PART GUITAR, expert base 96; gap 120 < default 161 -> HOPO;
        // ini delay applied
        SmfBuilder b;
        b.Header(2, 480);
        auto t1 = b.BeginTrack();
        b.Vlq(0); b.Raw({ 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20 });  // 120 BPM
        b.EndTrack(t1);
        auto t2 = b.BeginTrack();
        b.Vlq(0); b.Raw({ 0xFF, 0x03, 0x0B }); b.Str("PART GUITAR");
        b.Vlq(0);   b.Raw({ 0x91, 96, 100 });   // green on (expert base)
        b.Vlq(60);  b.Raw({ 96, 0 });           // green off
        b.Vlq(60);  b.Raw({ 97, 100 });         // red on, tick 120
        b.Vlq(60);  b.Raw({ 97, 0 });
        b.EndTrack(t2);
        const auto dir = gRoot / "midsong";
        fs::create_directories(dir);
        std::ofstream f(dir / "notes.mid", std::ios::binary);
        f.write(reinterpret_cast<const char*>(b.bytes.data()),
                static_cast<std::streamsize>(b.bytes.size()));
        f.close();
        WriteText(dir / "song.ini", "[song]\nname = MidSong\ndelay = 100\n");
        LoadedSong s;
        CHECK(LoadSong(MakeEntry(dir, true), 3, s));
        CHECK(s.chart.notes.size() == 2);
        CHECK_NEAR(s.chart.notes[0].time, 0.1, 1e-9);   // delay only
        CHECK(s.chart.notes[1].isHopo);                 // 120 < 161
        CHECK(s.chart.meta.name == "MidSong");
        CHECK(s.resolvedDifficulty == 3);   // expert base notes only
        CHECK(s.difficultyMask == 0b1000);
    }
    {   // .mid: EVENTS-track markers reach the loaded chart too, in the
        // SAME time domain as the notes - the .mid branch is the one
        // where a wrong offset would go unnoticed.
        SmfBuilder b;
        b.Header(3, 480);
        auto t1 = b.BeginTrack();
        b.Vlq(0); b.Raw({ 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20 });  // 120 BPM
        b.EndTrack(t1);
        auto t2 = b.BeginTrack();
        b.Vlq(0);   b.Meta(0x03, "EVENTS");
        b.Vlq(0);   b.Meta(0x01, "[section Intro]");
        b.Vlq(960); b.Meta(0x01, "[section Solo]");   // 2 beats in
        b.Vlq(0);   b.Meta(0x01, "[lyric na]");       // not a section
        b.EndTrack(t2);
        auto t3 = b.BeginTrack();
        b.Vlq(0);   b.Meta(0x03, "PART GUITAR");
        b.Vlq(0);   b.Raw({ 0x91, 96, 100 });   // green on (expert base)
        b.Vlq(60);  b.Raw({ 96, 0 });
        b.Vlq(900); b.Raw({ 97, 100 });         // red on, tick 960
        b.Vlq(60);  b.Raw({ 97, 0 });
        b.EndTrack(t3);
        const auto dir = gRoot / "midsections";
        fs::create_directories(dir);
        std::ofstream f(dir / "notes.mid", std::ios::binary);
        f.write(reinterpret_cast<const char*>(b.bytes.data()),
                static_cast<std::streamsize>(b.bytes.size()));
        f.close();
        WriteText(dir / "song.ini", "[song]\ndelay = 100\n");
        LoadedSong s;
        CHECK(LoadSong(MakeEntry(dir, true), 3, s));
        CHECK(s.chart.sections.size() == 2);        // the lyric dropped
        CHECK(s.chart.sections[0].name == "Intro");
        CHECK(s.chart.sections[1].name == "Solo");
        CHECK(s.chart.sections[1].tick == 960);
        // 2 beats @120bpm = 1.0s, plus the 100 ms ini delay - the exact
        // shift the note sitting on that same tick carries.
        CHECK(s.chart.notes.size() == 2);
        CHECK_NEAR(s.chart.sections[1].time, 1.1, 1e-9);
        CHECK_NEAR(s.chart.notes[1].time, 1.1, 1e-9);
    }
    fs::remove_all(gRoot);
}

TEST_MAIN("LoadSong")
