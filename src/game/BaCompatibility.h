#pragma once

// Pure parsing rules for the separately installed BA Bard Songs compatibility
// pack. Kept free of Skyrim/CommonLib and Windows APIs so malformed manifests
// and song.ini metadata can be covered by the headless suite.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace SH::ba {
    struct ManifestEntry {
        std::string sourceId;
        std::string instrument;
        std::string artist;
        std::string title;
        std::filesystem::path sourceRelative;
    };

    inline std::string Trim(std::string_view value) {
        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.front()))) {
            value.remove_prefix(1);
        }
        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.back()))) {
            value.remove_suffix(1);
        }
        return std::string(value);
    }

    inline std::vector<std::string> SplitTabs(std::string_view line) {
        std::vector<std::string> fields;
        std::size_t              begin = 0;
        while (true) {
            const auto end = line.find('\t', begin);
            fields.push_back(
                Trim(line.substr(begin, end == std::string_view::npos ?
                                            line.size() - begin :
                                            end - begin)));
            if (end == std::string_view::npos) break;
            begin = end + 1;
        }
        return fields;
    }

    inline bool SafeRelativeXwm(const std::filesystem::path& path) {
        if (path.empty() || path.is_absolute() || path.has_root_path() ||
            path.extension() != ".xwm") {
            return false;
        }
        for (const auto& part : path) {
            if (part == "..") return false;
        }
        return true;
    }

    inline bool ParseManifest(std::string_view text,
                              std::vector<ManifestEntry>& out,
                              std::string& error) {
        out.clear();
        error.clear();
        std::unordered_set<std::string> ids;
        std::istringstream              input{ std::string(text) };
        std::string                     line;
        std::size_t                     lineNo = 0;
        while (std::getline(input, line)) {
            ++lineNo;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto clean = Trim(line);
            if (clean.empty() || clean.front() == '#') continue;
            auto fields = SplitTabs(clean);
            if (fields.size() == 5 &&
                (fields[0] == "source_id" || fields[0] == "id") &&
                fields[1] == "instrument") {
                continue;
            }
            if (fields.size() != 5) {
                error = "line " + std::to_string(lineNo) +
                        ": expected 5 tab-separated fields";
                return false;
            }
            if (fields[0].empty() || !ids.insert(fields[0]).second) {
                error = "line " + std::to_string(lineNo) +
                        ": empty or duplicate source_id";
                return false;
            }
            if (fields[1] != "lute" && fields[1] != "flute" &&
                fields[1] != "drum") {
                error = "line " + std::to_string(lineNo) +
                        ": instrument must be lute, flute, or drum";
                return false;
            }
            std::replace(fields[4].begin(), fields[4].end(), '\\', '/');
            const std::filesystem::path relative(fields[4]);
            if (!SafeRelativeXwm(relative)) {
                error = "line " + std::to_string(lineNo) +
                        ": unsafe or non-XWM source path";
                return false;
            }
            out.push_back({ fields[0], fields[1], fields[2], fields[3],
                            relative });
        }
        if (out.empty()) {
            error = "manifest contains no songs";
            return false;
        }
        return true;
    }

    inline std::string SourceIdFromIni(std::string_view text) {
        std::istringstream input{ std::string(text) };
        std::string        line;
        bool               inSong = false;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto clean = Trim(line);
            if (clean.empty() || clean.front() == ';' ||
                clean.front() == '#') {
                continue;
            }
            if (clean.front() == '[' && clean.back() == ']') {
                std::string section = clean.substr(1, clean.size() - 2);
                std::transform(section.begin(), section.end(),
                               section.begin(), [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });
                inSong = section == "song";
                continue;
            }
            if (!inSong) continue;
            const auto eq = clean.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Trim(clean.substr(0, eq));
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            if (key == "source_id") return Trim(clean.substr(eq + 1));
        }
        return {};
    }
}
