#include "game/BaLibraryBootstrap.h"

#include "chart/Scan.h"
#include "harness.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
    struct TempTree {
        fs::path root =
            fs::temp_directory_path() /
            ("bardhero-ba-lazy-test-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
        TempTree() { fs::create_directories(root); }
        ~TempTree() {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    };

    void Write(const fs::path& path, std::string_view text) {
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
    }

    struct Fixture {
        TempTree tree;
        fs::path plugin = tree.root / "plugin";
        fs::path songs = plugin / "songs";
        fs::path source = tree.root / "ba-source";
        fs::path templates =
            plugin / "compat" / "ba-bard-songs" / "songs";
        std::string folderName = "BA LUTE_TEST - Test Dance";

        Fixture() {
            Write(plugin / "compat" / "ba-bard-songs" /
                      "ba_bard_songs.tsv",
                  "id\tinstrument\tartist\ttitle\tsource\n"
                  "lute_test\tlute\tTester\tDance\tlute/test.xwm\n");
            Write(templates / folderName / "notes.chart",
                  "[Song]\n{\n  Resolution = 192\n}\n"
                  "[SyncTrack]\n{\n  0 = B 120000\n}\n"
                  "[ExpertSingle]\n{\n  0 = N 0 0\n}\n");
            Write(templates / folderName / "song.ini",
                  "[song]\nname = Test Dance\ninstrument = lute\n"
                  "source_id = lute_test\n");
            Write(source / "lute" / "test.xwm", "not-xwm");
        }
    };
}

void RunTests() {
    {
        Fixture f;
        const auto first = SH::PrepareInstalledBaLibrary(f.songs, f.source);
        CHECK(first.templates == 1);
        CHECK(first.sourcesFound == 1);
        CHECK(first.generated == 1);
        CHECK(first.pending == 1);
        CHECK(first.ready == 0);
        CHECK(first.customSkipped == 0);
        CHECK(first.failed == 0);

        const auto folder = f.songs / f.folderName;
        CHECK(fs::is_regular_file(folder / "notes.chart"));
        CHECK(fs::is_regular_file(folder / "song.ini"));
        CHECK(fs::is_regular_file(folder / ".bardhero-ba-generated"));
        CHECK(fs::is_regular_file(folder / "song.opus"));
        CHECK(fs::file_size(folder / "song.opus") == 0);

        // The ordinary scanner sees the chart immediately despite the lazy
        // cache not existing yet.
        const auto scan = bard::ScanSongs(f.songs);
        CHECK(scan.bad.empty());
        CHECK(scan.songs.size() == 1);
        CHECK(scan.songs[0].stems.at("song") == folder / "song.opus");

        const auto second = SH::PrepareInstalledBaLibrary(f.songs, f.source);
        CHECK(second.generated == 0);
        CHECK(second.pending == 1);
        CHECK(second.ready == 0);

        // A malformed installed source fails at first play and does not turn
        // the placeholder into apparently valid audio.
        std::string error;
        CHECK(!SH::EnsureInstalledBaSongAudio(folder, f.source, &error));
        CHECK(!error.empty());
        CHECK(fs::file_size(folder / "song.opus") == 0);

        Write(folder / "song.opus", "cached");
        const auto cached = SH::PrepareInstalledBaLibrary(f.songs, f.source);
        CHECK(cached.pending == 0);
        CHECK(cached.ready == 1);
        error = "stale";
        CHECK(SH::EnsureInstalledBaSongAudio(folder, f.source, &error));
        CHECK(error.empty());
    }
    {
        Fixture f;
        // No separately installed BA source: no folders or placeholders.
        const auto missing = SH::PrepareInstalledBaLibrary(
            f.songs, f.tree.root / "missing-source");
        CHECK(missing.templates == 1);
        CHECK(missing.sourcesFound == 0);
        CHECK(missing.generated == 0);
        CHECK(!fs::exists(f.songs / f.folderName));
    }
    {
        Fixture f;
        // An unmarked destination is local/user-owned and never modified.
        const auto folder = f.songs / f.folderName;
        fs::create_directories(folder);
        Write(folder / "notes.chart", "local chart");
        const auto custom =
            SH::PrepareInstalledBaLibrary(f.songs, f.source);
        CHECK(custom.customSkipped == 1);
        CHECK(custom.generated == 0);
        CHECK(!fs::exists(folder / ".bardhero-ba-generated"));
        CHECK(!fs::exists(folder / "song.opus"));
    }
}

TEST_MAIN("BaLibraryBootstrap")
