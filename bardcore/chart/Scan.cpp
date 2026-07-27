#include "chart/Scan.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_set>

#include "util/PathText.h"

namespace fs = std::filesystem;

namespace bard {
    namespace {
        // Extensions tried in order (spec 4.1).
        constexpr std::array<const char*, 4> kAudioExt = { ".ogg", ".opus",
                                                           ".mp3", ".wav" };
        // Reserved stem filenames (spec 4.1 table).
        constexpr std::array<const char*, 14> kStems = {
            "song",    "guitar",   "rhythm",   "bass",    "keys",
            "crowd",   "drums",    "drums_1",  "drums_2", "drums_3",
            "drums_4", "vocals",   "vocals_1", "vocals_2"
        };

        std::string Lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return s;
        }

        int ExtRank(const std::string& lowerExt) {
            for (std::size_t i = 0; i < kAudioExt.size(); ++i) {
                if (lowerExt == kAudioExt[i]) return static_cast<int>(i);
            }
            return -1;
        }

        std::string PathKey(const fs::path& path) {
            std::error_code ec;
            auto normalized = fs::weakly_canonical(path, ec);
            if (ec) {
                ec.clear();
                normalized = fs::absolute(path, ec);
                if (ec) normalized = path;
            }
            auto key = SH::path_text::Utf8(normalized.lexically_normal());
#ifdef _WIN32
            key = Lower(std::move(key));
#endif
            return key;
        }

        void AddIoError(const fs::path& path, const std::error_code& ec,
                        ScanResult& out) {
            out.complete = false;
            out.bad.push_back(
                { path, "io error: " + (ec ? ec.message()
                                           : std::string("not a directory")) });
        }

        void ScanFolder(const fs::path& dir, ScanResult& out,
                        std::vector<fs::path>& childDirectories) {
            fs::path chart, mid, ini;
            // stem name -> (rank, path); lower rank = preferred extension
            std::map<std::string, std::pair<int, fs::path>> stems;
            fs::path preview;
            int      previewRank = 99;
            bool     anyAudio    = false;

            std::error_code ec;
            fs::directory_iterator it(
                dir, fs::directory_options::skip_permission_denied, ec);
            if (ec) {
                AddIoError(dir, ec, out);
                return;
            }
            const fs::directory_iterator end;
            while (it != end) {
                const auto e = *it;
                std::error_code statusEc;
                const auto status = e.status(statusEc);
                if (statusEc) {
                    AddIoError(e.path(), statusEc, out);
                } else if (fs::is_directory(status)) {
                    childDirectories.push_back(e.path());
                } else if (fs::is_regular_file(status)) {
                    const auto name =
                        Lower(SH::path_text::Utf8(e.path().filename()));
                    const auto stem =
                        Lower(SH::path_text::Utf8(e.path().stem()));
                    const auto ext =
                        Lower(SH::path_text::Utf8(e.path().extension()));
                    if (name == "notes.chart") chart = e.path();
                    else if (name == "notes.mid") mid = e.path();
                    else if (name == "song.ini") ini = e.path();
                    const int rank = ExtRank(ext);
                    if (rank >= 0) {
                        anyAudio = true;
                        if (stem == "preview") {
                            if (rank < previewRank) {
                                previewRank = rank;
                                preview     = e.path();
                            }
                        } else {
                            for (const auto* s : kStems) {
                                if (stem == s) {
                                    auto found = stems.find(stem);
                                    if (found == stems.end() ||
                                        rank < found->second.first) {
                                        stems[stem] = { rank, e.path() };
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
                ec.clear();
                it.increment(ec);
                if (ec) {
                    AddIoError(dir, ec, out);
                    break;
                }
            }
            if (chart.empty() && mid.empty()) {
                return;  // not a song folder at all - silent
            }
            if (!anyAudio) {
                out.bad.push_back({ dir, "chart present but no audio stem" });
                return;
            }

            // single-vs-split suppression (spec 4.1): drums ignored if any
            // drums_N exists; vocals same vs vocals_1/2.
            const bool splitDrums =
                stems.count("drums_1") || stems.count("drums_2") ||
                stems.count("drums_3") || stems.count("drums_4");
            if (splitDrums) stems.erase("drums");
            const bool splitVocals =
                stems.count("vocals_1") || stems.count("vocals_2");
            if (splitVocals) stems.erase("vocals");

            SongEntry s;
            s.folder    = dir;
            s.chartFile = !chart.empty() ? chart : mid;  // .chart preferred
            s.isMid     = chart.empty();
            s.iniFile   = ini;
            for (auto& [name, rp] : stems) {
                s.stems[name] = rp.second;
            }
            if (!preview.empty()) {
                s.stems["preview"] = preview;
            }
            if (s.stems.empty() ||
                (s.stems.size() == 1 && s.stems.count("preview"))) {
                out.bad.push_back({ dir, "no reserved audio stem" });
                return;
            }
            out.songs.push_back(std::move(s));
        }

        void ScanTree(const fs::path& root, ScanResult& out,
                      std::unordered_set<std::string>& visited) {
            std::error_code ec;
            const bool exists = fs::exists(root, ec);
            if (ec) {
                AddIoError(root, ec, out);
                return;
            }
            // Optional roots do not have to exist yet.
            if (!exists) return;
            if (!fs::is_directory(root, ec)) {
                AddIoError(root, ec, out);
                return;
            }

            std::vector<fs::path> pending{ root };
            while (!pending.empty()) {
                auto dir = std::move(pending.back());
                pending.pop_back();
                if (!visited.insert(PathKey(dir)).second) continue;

                std::vector<fs::path> children;
                ScanFolder(dir, out, children);
                std::sort(children.begin(), children.end(),
                          [](const fs::path& a, const fs::path& b) {
                              return PathKey(a) > PathKey(b);
                          });
                for (auto& child : children) {
                    pending.push_back(std::move(child));
                }
            }
        }
    }

    ScanResult ScanSongs(const fs::path& root) {
        return ScanSongs(std::vector<fs::path>{ root });
    }

    ScanResult ScanSongs(const std::vector<fs::path>& roots) {
        ScanResult out;
        std::unordered_set<std::string> visited;
        for (const auto& root : roots) {
            if (root.empty()) continue;
            ScanTree(root, out, visited);
        }
        return out;
    }
}
