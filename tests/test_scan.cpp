#include "harness.h"
#include "chart/Scan.h"
#include "util/PathText.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace bard;

static void Touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p).put('x');
}

static const SongEntry* Find(const ScanResult& r, const std::string& folder) {
    for (const auto& s : r.songs) {
        if (SH::path_text::Utf8(s.folder.filename()) == folder) return &s;
    }
    return nullptr;
}

static void RunTests() {
    const auto root =
        fs::temp_directory_path() / "skyhero_scan_test";
    fs::remove_all(root);

    Touch(root / "s1" / "notes.chart");
    Touch(root / "s1" / "song.ogg");
    Touch(root / "s1" / "song.ini");

    Touch(root / "s2" / "notes.mid");
    Touch(root / "s2" / "guitar.mp3");

    Touch(root / "s3" / "notes.chart");
    Touch(root / "s3" / "notes.mid");
    Touch(root / "s3" / "song.wav");

    Touch(root / "s4_not_a_song" / "song.ogg");   // no chart: silent skip

    Touch(root / "s5_bad" / "notes.chart");       // no audio: badsongs

    Touch(root / "s6" / "notes.chart");
    Touch(root / "s6" / "song.ogg");
    Touch(root / "s6" / "song.mp3");              // ogg outranks mp3
    Touch(root / "s6" / "guitar.wav");
    Touch(root / "s6" / "guitar.opus");           // opus outranks wav

    Touch(root / "s7" / "notes.chart");
    Touch(root / "s7" / "drums.ogg");             // suppressed by drums_N
    Touch(root / "s7" / "drums_1.ogg");
    Touch(root / "s7" / "drums_2.ogg");
    Touch(root / "s7" / "vocals.ogg");            // suppressed by vocals_1
    Touch(root / "s7" / "vocals_1.ogg");
    Touch(root / "s7" / "preview.ogg");

    Touch(root / "nested" / "deeper" / "s8" / "notes.chart");
    Touch(root / "nested" / "deeper" / "s8" / "bass.ogg");

    const std::string unicodeName = "Sch\xC3\xA4" "fer";
    const fs::path unicodeFolder = fs::path(L"Sch\u00E4fer");
    Touch(root / unicodeFolder / "notes.chart");
    Touch(root / unicodeFolder / "song.ogg");

    const auto r = ScanSongs(root);

    CHECK(r.songs.size() == 7);
    CHECK(r.bad.size() == 1);
    CHECK(r.bad[0].folder.filename() == "s5_bad");

    const auto* s1 = Find(r, "s1");
    CHECK(s1 && !s1->isMid && !s1->iniFile.empty());
    CHECK(s1 && s1->stems.count("song") == 1);

    const auto* s2 = Find(r, "s2");
    CHECK(s2 && s2->isMid);

    const auto* s3 = Find(r, "s3");
    CHECK(s3 && !s3->isMid);   // .chart preferred over .mid
    CHECK(s3 && s3->chartFile.filename() == "notes.chart");

    CHECK(Find(r, "s4_not_a_song") == nullptr);

    const auto* s6 = Find(r, "s6");
    CHECK(s6 && s6->stems.at("song").extension() == ".ogg");
    CHECK(s6 && s6->stems.at("guitar").extension() == ".opus");

    const auto* s7 = Find(r, "s7");
    CHECK(s7 && s7->stems.count("drums") == 0);
    CHECK(s7 && s7->stems.count("drums_1") == 1);
    CHECK(s7 && s7->stems.count("drums_2") == 1);
    CHECK(s7 && s7->stems.count("vocals") == 0);
    CHECK(s7 && s7->stems.count("vocals_1") == 1);
    CHECK(s7 && s7->stems.count("preview") == 1);

    CHECK(Find(r, "s8") != nullptr);   // recursive walk
    CHECK(SH::path_text::Utf8(unicodeFolder) == unicodeName);
    CHECK(SH::path_text::FromUtf8(unicodeName) == unicodeFolder);
    CHECK(Find(r, unicodeName) != nullptr);

    // A broken root must not prevent a separate healthy user-song root from
    // being discovered. A regular file is a deterministic stand-in for the
    // transient "path exists but cannot be enumerated as a directory" state
    // observed while Bridge completed a download under MO2.
    const auto userRoot = root.parent_path() / "skyhero_scan_user_test";
    fs::remove_all(userRoot);
    Touch(userRoot / "guitar" / "user_song" / "notes.chart");
    Touch(userRoot / "guitar" / "user_song" / "song.opus");
    Touch(root / "not_a_directory");
    const auto multi = ScanSongs(
        std::vector<fs::path>{ root / "not_a_directory", userRoot, userRoot });
    CHECK(!multi.complete);
    CHECK(Find(multi, "user_song") != nullptr);
    CHECK(multi.songs.size() == 1);  // duplicate root is scanned once
    CHECK(!multi.bad.empty());

    fs::remove_all(root);
    fs::remove_all(userRoot);
}

TEST_MAIN("Scan")
