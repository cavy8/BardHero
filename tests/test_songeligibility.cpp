// tests/test_songeligibility.cpp
#include "game/SongEligibility.h"
#include "game/GuitarPropLogic.h"
#include "game/SongIdentity.h"
#include "render/SongSortLogic.h"

#include "harness.h"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace SH::songeligibility;

void RunTests() {
    // TESModel strips the Data\Meshes root from loaded model paths. Passing
    // "Meshes\..." to SetModel therefore resolves as Meshes\Meshes\... and
    // makes the animation object invisible.
    CHECK(SH::guitarprop::kAnimationObjectModel ==
          "BardHeroElectric\\GuitarAnimObject.nif");
    CHECK(SH::guitarprop::IsMeshesRelativePath(
        SH::guitarprop::kAnimationObjectModel));
    CHECK(!SH::guitarprop::IsMeshesRelativePath(
        "Meshes\\BardHeroElectric\\GuitarAnimObject.nif"));
    CHECK(!SH::guitarprop::IsMeshesRelativePath(
        "MESHES\\BardHeroElectric\\GuitarAnimObject.nif"));
    CHECK(!SH::guitarprop::IsMeshesRelativePath(
        "C:\\Data\\Meshes\\Guitar.nif"));
    CHECK(!SH::guitarprop::IsMeshesRelativePath("\\Guitar.nif"));

    // Every SGT trigger code preserves its exact instrument context.
    CHECK(ContextForTrigger(0) == kLute);
    CHECK(ContextForTrigger(1) == kFlute);
    CHECK(ContextForTrigger(2) == kDrum);
    CHECK(ContextForTrigger(3) == kGuitar);
    CHECK(ContextForTrigger(-1) == kInvalidContext);
    CHECK(ContextForTrigger(4) == kInvalidContext);
    CHECK(ProgressionContext(kLute) == kLute);
    CHECK(ProgressionContext(kFlute) == kFlute);
    CHECK(ProgressionContext(kDrum) == kDrum);
    CHECK(ProgressionContext(kGuitar) == kLute);
    CHECK(ProgressionContext(kContextFree) == kInvalidContext);
    CHECK(UsesGuitarPerformanceProp(kGuitar));
    CHECK(!UsesGuitarPerformanceProp(kLute));
    CHECK(!UsesGuitarPerformanceProp(kFlute));
    CHECK(!UsesGuitarPerformanceProp(kDrum));
    CHECK(!UsesGuitarPerformanceProp(kContextFree));
    CHECK(!UsesGuitarPerformanceProp(kInvalidContext));

    // Exact normalized matches are eligible; another supported instrument is
    // not. Check all three so a swapped trigger/index mapping cannot pass.
    CHECK(IsEligible("lute", kLute));
    CHECK(IsEligible("flute", kFlute));
    CHECK(IsEligible("drum", kDrum));
    CHECK(IsEligible("guitar", kGuitar));
    CHECK(!IsEligible("flute", kLute));
    CHECK(!IsEligible("drum", kFlute));
    CHECK(!IsEligible("lute", kDrum));
    CHECK(!IsEligible("guitar", kLute));
    CHECK(!IsEligible("lute", kGuitar));
    CHECK(!IsEligible("flute", kGuitar));
    CHECK(!IsEligible("drum", kGuitar));

    // song.ini tags are compared after trimming and ASCII case folding.
    CHECK(IsEligible("  LuTe\t", kLute));
    CHECK(IsEligible("\rFLUTE ", kFlute));
    CHECK(IsEligible("\tdRuM\r", kDrum));
    CHECK(IsEligible("  GuItAr\t", kGuitar));

    // Legacy untagged charts remain discoverable only in the context-free
    // debug Songbook. They never match an instrument-bound SGT flow.
    CHECK(IsEligible("", kContextFree));
    CHECK(IsEligible(" \t\r", kContextFree));
    CHECK(!IsEligible("", kLute));
    CHECK(!IsEligible(" \t\r", kDrum));
    CHECK(!IsEligible("", kGuitar));
    CHECK(!TaggedInstrument("").has_value());

    // An ancestor directory named exactly "guitar" is the zero-touch import
    // convention for Bridge/Clone Hero folders. Authored metadata wins, and
    // a song title containing the word guitar does not become a tag.
    namespace fs = std::filesystem;
    const fs::path songsRoot = fs::path("library") / "songs";
    CHECK(ResolveScannedInstrumentTag(
              "",
              songsRoot / "guitar" / "Metal" / "Example Song") ==
          "guitar");
    CHECK(ResolveScannedInstrumentTag(
              "",
              songsRoot / "GUITAR" / "Example Song") ==
          "guitar");
    CHECK(ResolveScannedInstrumentTag(
              "flute",
              songsRoot / "guitar" / "Example Song") ==
          "flute");
    CHECK(ResolveScannedInstrumentTag(
              "",
              songsRoot / "The Guitar Song") ==
          "");
    CHECK(ResolveScannedInstrumentTag(
              "",
              fs::path(
                  "Data/SKSE/Plugins/BardHero/songs\\guitar\\"
                  "AC\xEF\xBC\x8F" "DC - Back In Black (oRizho)")) ==
          "guitar");
    CHECK(SH::song_identity::ChartKey(
              fs::path("library") / "guitar" /
              L"AC\uFF0FDC - You Shook Me All Night Long") ==
          "AC\xEF\xBC\x8F" "DC - You Shook Me All Night Long");

    // Malformed tags are useful to expose in the debug Songbook, but they do
    // not silently become a match for any real instrument.
    constexpr std::array<std::string_view, 4> malformed{
        "banjo", "lute extra", "flute|drum", "drums"
    };
    for (const auto tag : malformed) {
        CHECK(IsEligible(tag, kContextFree));
        CHECK(!IsEligible(tag, kLute));
        CHECK(!IsEligible(tag, kFlute));
        CHECK(!IsEligible(tag, kDrum));
        CHECK(!IsEligible(tag, kGuitar));
        CHECK(!TaggedInstrument(tag).has_value());
    }

    // A bound Songbook can deterministically reach the empty-state branch.
    constexpr std::array<std::string_view, 3> noLuteSongs{
        "flute", "drum", ""
    };
    bool anyLute = false;
    for (const auto tag : noLuteSongs) {
        anyLute = anyLute || IsEligible(tag, kLute);
    }
    CHECK(!anyLute);

    // Final authorization must use the CURRENT frozen context. This models a
    // row selected in the context-free Songbook before a stale UI selection
    // is presented to a lute-bound Play action.
    CHECK(IsEligible("flute", kContextFree));
    CHECK(!CanStart("flute", kLute));
    CHECK(CanStart("  LUTE ", kLute));
    CHECK(IsEligible("lute", kContextFree));
    CHECK(!CanStart("lute", kGuitar));
    CHECK(CanStart(" GUITAR ", kGuitar));

    // An invalid/corrupted context fails closed rather than broadening to the
    // debug Songbook.
    CHECK(!IsEligible("lute", kInvalidContext));
    CHECK(!CanStart("lute", kInvalidContext));

    // Songbook headers use the same two-state sortable-table interaction as
    // Fitting Room. Keep the comparator pure so header clicks cannot make a
    // selected display row name the wrong raw song.
    using SH::song_sort::Column;
    using SH::song_sort::Key;
    using SH::song_sort::RatingState;
    CHECK(SH::song_sort::kDefaultColumn == Column::kRating);
    CHECK(!SH::song_sort::kDefaultAscending);
    std::vector<Key> sortKeys{
        { "zebra", "Beta", 0, 36000.0, RatingState::kLocked, 2 },
        { "Alpha", "gamma", 1, 42000.0, RatingState::kRated, 4 },
        { "bravo", "alpha", 2, 30000.0, RatingState::kUnplayed, 0 },
        { "alpha", "Delta", 3, 48000.0, RatingState::kRated, 5 }
    };
    std::vector<int> order{ 0, 1, 2, 3 };

    SH::song_sort::Sort(order, sortKeys, Column::kSong, true);
    CHECK(order == std::vector<int>({ 1, 3, 2, 0 }));
    SH::song_sort::Sort(order, sortKeys, Column::kSong, false);
    CHECK(order == std::vector<int>({ 0, 2, 3, 1 }));

    SH::song_sort::Sort(order, sortKeys, Column::kArtist, true);
    CHECK(order == std::vector<int>({ 2, 0, 3, 1 }));
    SH::song_sort::Sort(order, sortKeys, Column::kLength, true);
    CHECK(order == std::vector<int>({ 2, 0, 1, 3 }));

    // Ascending exposes easier locks first; descending puts the best earned
    // rating first, then unplayed, then locked material.
    SH::song_sort::Sort(order, sortKeys, Column::kRating, true);
    CHECK(order == std::vector<int>({ 0, 2, 1, 3 }));
    SH::song_sort::Sort(order, sortKeys, Column::kRating, false);
    CHECK(order == std::vector<int>({ 3, 1, 2, 0 }));

    // Screenshot regression: Rating-descending must not reverse the locked
    // rank sub-order. Best performances come first, but the learning path
    // below them remains lowest required rank to highest.
    std::vector<Key> mixedRating{
        { "three stars", "", 0, 0.0, RatingState::kRated, 3 },
        { "two stars", "", 1, 0.0, RatingState::kRated, 2 },
        { "rank five", "", 2, 0.0, RatingState::kLocked, 5 },
        { "rank four", "", 3, 0.0, RatingState::kLocked, 4 },
        { "rank two", "", 4, 0.0, RatingState::kLocked, 2 }
    };
    std::vector<int> mixedOrder{ 0, 1, 2, 3, 4 };
    SH::song_sort::Sort(mixedOrder, mixedRating, Column::kRating, false);
    CHECK(mixedOrder == std::vector<int>({ 0, 1, 4, 3, 2 }));
}

TEST_MAIN("SongEligibility")
