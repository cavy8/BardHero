// src/game/SongEligibility.h
#pragma once

// PURE instrument eligibility for the Songbook (no RE/OS includes).
//
// An SGT perform trigger binds the Songbook to exactly one instrument. The
// debug/start-key Songbook is deliberately context-free so legacy untagged
// charts remain discoverable there. Filtering and final Play authorization
// share this one decision layer so a stale selected row cannot bypass it.

#include <filesystem>
#include <optional>
#include <string_view>

namespace SH::songeligibility {
    inline constexpr int kInvalidContext = -2;
    inline constexpr int kContextFree    = -1;
    inline constexpr int kLute           = 0;
    inline constexpr int kFlute          = 1;
    inline constexpr int kDrum           = 2;
    inline constexpr int kGuitar         = 3;
    inline constexpr int kInstrumentContextCount = 4;

    [[nodiscard]] constexpr bool IsBoundContext(int a_context) {
        return a_context >= kLute && a_context <= kGuitar;
    }

    // Perform-trigger indices are lute/flute/drum plus the optional guitar.
    // Fail closed on an out-of-range source rather than widening it into the
    // context-free debug Songbook.
    [[nodiscard]] constexpr int ContextForTrigger(int a_triggerIndex) {
        return IsBoundContext(a_triggerIndex) ? a_triggerIndex
                                              : kInvalidContext;
    }

    // Guitar is a distinct repertoire and trigger context, but the Electric
    // addon deliberately reuses SGT's lute expertise and payout ladder.
    // Keep that alias explicit so a selection-context index is never cast
    // directly to the three-value progression enum.
    [[nodiscard]] constexpr int ProgressionContext(int a_context) {
        if (a_context == kGuitar) { return kLute; }
        return a_context >= kLute && a_context <= kDrum
                 ? a_context
                 : kInvalidContext;
    }

    // Guitar shares lute progression, but not the lute's visible animated
    // object. Keep presentation tied to the exact selection context rather
    // than the aliased progression context.
    [[nodiscard]] constexpr bool UsesGuitarPerformanceProp(int a_context) {
        return a_context == kGuitar;
    }

    [[nodiscard]] constexpr std::string_view InstrumentName(int a_context) {
        switch (a_context) {
        case kLute:
            return "lute";
        case kFlute:
            return "flute";
        case kDrum:
            return "drum";
        case kGuitar:
            return "guitar";
        default:
            return {};
        }
    }

    [[nodiscard]] constexpr bool IsAsciiSpace(char a_ch) {
        return a_ch == ' ' || a_ch == '\t' || a_ch == '\r' ||
               a_ch == '\n' || a_ch == '\f' || a_ch == '\v';
    }

    [[nodiscard]] constexpr char AsciiLower(char a_ch) {
        return a_ch >= 'A' && a_ch <= 'Z'
                 ? static_cast<char>(a_ch + ('a' - 'A'))
                 : a_ch;
    }

    [[nodiscard]] constexpr std::string_view Trim(std::string_view a_tag) {
        while (!a_tag.empty() && IsAsciiSpace(a_tag.front())) {
            a_tag.remove_prefix(1);
        }
        while (!a_tag.empty() && IsAsciiSpace(a_tag.back())) {
            a_tag.remove_suffix(1);
        }
        return a_tag;
    }

    [[nodiscard]] constexpr bool EqualsFolded(std::string_view a_left,
                                               std::string_view a_right) {
        if (a_left.size() != a_right.size()) { return false; }
        for (std::size_t i = 0; i < a_left.size(); ++i) {
            if (AsciiLower(a_left[i]) != AsciiLower(a_right[i])) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr std::optional<int> TaggedInstrument(
        std::string_view a_tag) {
        const auto tag = Trim(a_tag);
        if (EqualsFolded(tag, "lute")) { return kLute; }
        if (EqualsFolded(tag, "flute")) { return kFlute; }
        if (EqualsFolded(tag, "drum")) { return kDrum; }
        if (EqualsFolded(tag, "guitar")) { return kGuitar; }
        return std::nullopt;
    }

    [[nodiscard]] inline bool PathPartEqualsFolded(
        const std::filesystem::path& a_part, std::string_view a_expected) {
        const auto& native = a_part.native();
        if (native.size() != a_expected.size()) { return false; }
        for (std::size_t i = 0; i < native.size(); ++i) {
            const auto ch = native[i];
            if (ch < 0 || ch > 0x7f ||
                AsciiLower(static_cast<char>(ch)) !=
                    AsciiLower(a_expected[i])) {
                return false;
            }
        }
        return true;
    }

    // The caller supplies a folder already returned by ScanSongs(root), so
    // containment is established by the scanner rather than reconstructed
    // from filesystem path spelling. This matters under MO2, where the
    // virtual root and recursive entry can use different native forms.
    // Explicit supported metadata always wins; the song folder's own name is
    // deliberately ignored.
    [[nodiscard]] inline std::string_view ResolveScannedInstrumentTag(
        std::string_view a_explicitTag,
        const std::filesystem::path& a_songFolder) {
        if (const auto tagged = TaggedInstrument(a_explicitTag)) {
            return InstrumentName(*tagged);
        }

        for (const auto& part : a_songFolder.parent_path()) {
            if (PathPartEqualsFolded(part, "guitar")) { return "guitar"; }
        }
        return {};
    }

    [[nodiscard]] constexpr bool IsEligible(std::string_view a_songTag,
                                             int a_context) {
        if (a_context == kContextFree) {
            // The debug Songbook is the compatibility surface for both old
            // untagged charts and malformed metadata that needs diagnosis.
            return true;
        }
        if (!IsBoundContext(a_context)) { return false; }
        const auto tagged = TaggedInstrument(a_songTag);
        return tagged.has_value() && *tagged == a_context;
    }

    // Kept as a named final gate even though it currently shares the same
    // predicate as row visibility. Callers must re-run it at Play time.
    [[nodiscard]] constexpr bool CanStart(std::string_view a_songTag,
                                          int a_context) {
        return IsEligible(a_songTag, a_context);
    }
}
