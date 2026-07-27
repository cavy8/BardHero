#include "game/BaCompatibility.h"

#include "harness.h"

using namespace SH::ba;

void RunTests() {
    {
        const std::string manifest =
            "id\tinstrument\tartist\ttitle\tsource\n"
            "lute_01_01\tlute\tPlayer\tDance\t"
            "lute/mus_bardlute_01_01.xwm\r\n"
            "flute_02_03\tflute\tPlayer\tAir\t"
            "flute\\mus_bardflute_02_03.xwm\n";
        std::vector<ManifestEntry> entries;
        std::string                error;
        CHECK(ParseManifest(manifest, entries, error));
        CHECK(error.empty());
        CHECK(entries.size() == 2);
        CHECK(entries[0].sourceId == "lute_01_01");
        CHECK(entries[0].instrument == "lute");
        CHECK(entries[1].sourceRelative.generic_string() ==
              "flute/mus_bardflute_02_03.xwm");
    }
    {
        std::vector<ManifestEntry> entries;
        std::string                error;
        CHECK(!ParseManifest(
            "x\tbanjo\ta\tb\tlute/x.xwm\n", entries, error));
        CHECK(error.find("instrument") != std::string::npos);
        CHECK(!ParseManifest(
            "x\tlute\ta\tb\t../stolen.xwm\n", entries, error));
        CHECK(error.find("unsafe") != std::string::npos);
        CHECK(!ParseManifest(
            "x\tlute\ta\tb\tlute/a.xwm\n"
            "x\tlute\ta\tb\tlute/b.xwm\n",
            entries, error));
        CHECK(error.find("duplicate") != std::string::npos);
    }
    {
        CHECK(SourceIdFromIni(
                  "[song]\r\nname = Test\r\nsource_id = flute_06_01\r\n") ==
              "flute_06_01");
        CHECK(SourceIdFromIni(
                  "[other]\nsource_id=wrong\n[SONG]\nSOURCE_ID = drum_03_02\n") ==
              "drum_03_02");
        CHECK(SourceIdFromIni("[song]\nname = no source\n").empty());
    }
}

TEST_MAIN("BaCompatibility")
