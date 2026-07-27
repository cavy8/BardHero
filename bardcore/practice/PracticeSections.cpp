#include "practice/PracticeSections.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace bard::practice {
    namespace {
        // The offsets below are derived from these, never hand-counted:
        // editing a keyword without its length would silently truncate
        // the section name rather than fail.
        inline constexpr std::string_view kSectionPrefix = "section ";
        inline constexpr std::string_view kPrcPrefix     = "prc_";
        inline constexpr std::string_view kEventsTrack   = "EVENTS";

        // '\0' is in the set because SMF strings are length-prefixed, not
        // NUL-terminated: a writer whose length counts the terminator
        // hands us a name/value with a trailing NUL byte.
        bool IsPad(char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\0';
        }

        std::string_view Trim(std::string_view s) {
            while (!s.empty() && IsPad(s.front())) { s.remove_prefix(1); }
            while (!s.empty() && IsPad(s.back())) { s.remove_suffix(1); }
            return s;
        }

        std::string_view Unquote(std::string_view s) {
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
                return s.substr(1, s.size() - 2);
            }
            return s;
        }

        // Case-insensitive prefix test; the KEYWORD is case-insensitive
        // but the section NAME's casing is the charter's and is kept.
        bool StartsWithFolded(std::string_view s, std::string_view prefix) {
            if (s.size() < prefix.size()) { return false; }
            for (std::size_t i = 0; i < prefix.size(); ++i) {
                const auto a = static_cast<unsigned char>(s[i]);
                const auto b = static_cast<unsigned char>(prefix[i]);
                if (std::tolower(a) != std::tolower(b)) { return false; }
            }
            return true;
        }

        bool EqualsFolded(std::string_view s, std::string_view other) {
            return s.size() == other.size() && StartsWithFolded(s, other);
        }

        // Strips a leading `E` token if present, then the outer quotes.
        std::string_view EventBody(std::string_view a_value) {
            auto body = Trim(a_value);
            if (StartsWithFolded(body, "e ")) {
                body = Trim(body.substr(2));
            } else if (body.size() == 1 &&
                       (body[0] == 'E' || body[0] == 'e')) {
                return {};
            }
            return Trim(Unquote(body));
        }
    }

    bool SectionNameFromChartEvent(std::string_view a_value,
                                   std::string& a_name) {
        const auto body = EventBody(a_value);
        if (body.empty()) { return false; }
        std::string_view name;
        if (StartsWithFolded(body, kSectionPrefix)) {
            name = Trim(body.substr(kSectionPrefix.size()));
        } else if (StartsWithFolded(body, kPrcPrefix)) {
            name = Trim(body.substr(kPrcPrefix.size()));
        } else {
            return false;
        }
        if (name.empty()) { return false; }
        a_name.assign(name);
        return true;
    }

    bool SectionNameFromMidText(std::string_view a_text,
                                std::string& a_name) {
        auto body = Trim(a_text);
        // Brackets are mandatory here: unbracketed text events in a .mid
        // are lyrics and charter notes, and treating those as sections
        // would fill the practice menu with song words.
        if (body.size() < 2 || body.front() != '[' || body.back() != ']') {
            return false;
        }
        body = Trim(body.substr(1, body.size() - 2));
        if (body.empty()) { return false; }
        std::string_view name;
        if (StartsWithFolded(body, kSectionPrefix)) {
            name = Trim(body.substr(kSectionPrefix.size()));
        } else if (StartsWithFolded(body, kPrcPrefix)) {
            name = Trim(body.substr(kPrcPrefix.size()));
        } else {
            return false;
        }
        if (name.empty()) { return false; }
        a_name.assign(name);
        return true;
    }

    namespace {
        // Shared tail: sort by tick and collapse ties to the LAST entry,
        // which is how both formats behave when a charter leaves two
        // markers on one tick.
        void SortAndUnique(std::vector<ChartSection>& a_out) {
            std::stable_sort(a_out.begin(), a_out.end(),
                             [](const ChartSection& l,
                                const ChartSection& r) {
                                 return l.tick < r.tick;
                             });
            if (a_out.empty()) { return; }
            std::vector<ChartSection> unique;
            unique.reserve(a_out.size());
            for (auto& s : a_out) {
                if (!unique.empty() && unique.back().tick == s.tick) {
                    unique.back() = s;  // last wins
                } else {
                    unique.push_back(s);
                }
            }
            a_out.swap(unique);
        }

        // Smf.h copies the meta-0x03 payload byte for byte - no trim, no
        // NUL strip - so a track a charter meant as EVENTS can arrive as
        // "EVENTS ", "Events", or with a trailing NUL. Tolerated because
        // the failure mode is SILENT: an unmatched name yields zero
        // sections and no error, so the practice menu would quietly
        // collapse to whole-song only and look exactly like a chart that
        // has no markers. (A mis-named PART track, by contrast, fails the
        // load loudly.) The tolerance stops at equality, though: EVENTS2
        // is a different track, not a sloppy spelling of this one.
        bool IsEventsTrack(std::string_view a_name) {
            return EqualsFolded(Trim(a_name), kEventsTrack);
        }
    }

    std::vector<ChartSection> SectionsFromChart(const ChartFile& a_chart,
                                                double a_offsetSeconds) {
        std::vector<ChartSection> out;
        const auto it = a_chart.sections.find("Events");
        if (it == a_chart.sections.end()) { return out; }
        for (const auto& [tick, value] : it->second) {
            ChartSection s;
            if (!SectionNameFromChartEvent(value, s.name)) { continue; }
            s.tick = tick;
            s.time = a_chart.tempo.SecondsAt(static_cast<double>(tick)) +
                     a_offsetSeconds;
            out.push_back(std::move(s));
        }
        SortAndUnique(out);
        return out;
    }

    std::vector<ChartSection> SectionsFromMid(const SmfFile& a_file,
                                              const TempoMap& a_tempo,
                                              double a_offsetSeconds) {
        std::vector<ChartSection> out;
        for (const auto& track : a_file.tracks) {
            // Only the EVENTS track. Section-shaped text on PART tracks
            // is a charter's private note, not a practice section.
            if (!IsEventsTrack(track.name)) { continue; }
            for (const auto& text : track.texts) {
                ChartSection s;
                if (!SectionNameFromMidText(text.text, s.name)) {
                    continue;
                }
                s.tick = text.tick;
                s.time =
                    a_tempo.SecondsAt(static_cast<double>(text.tick)) +
                    a_offsetSeconds;
                out.push_back(std::move(s));
            }
        }
        SortAndUnique(out);
        return out;
    }
}
