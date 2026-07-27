#include "game/BaLibraryBootstrap.h"

#include "audio/XwmTranscoder.h"
#include "game/BaCompatibility.h"
#include "util/PathText.h"

#include <windows.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace SH {
    namespace {
        namespace fs = std::filesystem;

        std::string ReadText(const fs::path& path) {
            std::ifstream input(path, std::ios::binary);
            if (!input) return {};
            return { std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>() };
        }

        bool HasReadyAudio(const fs::path& folder) {
            static constexpr std::array<std::string_view, 4> extensions{
                ".ogg", ".opus", ".wav", ".mp3"
            };
            std::error_code ec;
            for (fs::directory_iterator it(folder, ec), end; !ec && it != end;
                 it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                auto ext = path_text::Utf8(it->path().extension());
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });
                if (std::ranges::find(extensions, ext) != extensions.end()) {
                    const auto size = it->file_size(ec);
                    if (!ec && size > 0) return true;
                }
            }
            return false;
        }

        bool CopyTemplate(const fs::path& source, const fs::path& destination,
                          std::string& error) {
            std::error_code ec;
            fs::create_directories(destination, ec);
            if (ec) {
                error = "could not create song folder: " + ec.message();
                return false;
            }
            for (const auto* name : { "notes.chart", "song.ini" }) {
                const auto from = source / name;
                const auto to = destination / name;
                if (!fs::is_regular_file(from, ec) ||
                    !fs::copy_file(from, to,
                                   fs::copy_options::overwrite_existing, ec)) {
                    error = "could not install " + std::string(name) + ": " +
                            (ec ? ec.message() : "template missing");
                    return false;
                }
            }
            return true;
        }

        bool WriteMarker(const fs::path& destination,
                         std::string_view sourceId) {
            std::ofstream marker(destination / ".bardhero-ba-generated",
                                 std::ios::binary | std::ios::trunc);
            marker << "schema=1\nsource_id=" << sourceId << '\n';
            return static_cast<bool>(marker);
        }

        bool WritePendingAudio(const fs::path& destination,
                               std::string& error) {
            const auto pending = destination / "song.opus";
            std::error_code ec;
            if (fs::is_regular_file(pending, ec)) return true;
            std::ofstream output(pending,
                                 std::ios::binary | std::ios::trunc);
            if (!output) {
                error = "could not create the pending audio placeholder";
                return false;
            }
            return true;
        }

        bool LoadManifest(const fs::path& pluginRoot,
                          std::vector<ba::ManifestEntry>& manifest,
                          std::string& error) {
            const auto path =
                pluginRoot / "compat" / "ba-bard-songs" /
                "ba_bard_songs.tsv";
            return ba::ParseManifest(ReadText(path), manifest, error);
        }

        const ba::ManifestEntry* FindSource(
            const std::vector<ba::ManifestEntry>& manifest,
            std::string_view sourceId) {
            const auto it = std::ranges::find(
                manifest, sourceId, &ba::ManifestEntry::sourceId);
            return it == manifest.end() ? nullptr : &*it;
        }

        fs::path DefaultSourceRoot(const fs::path& requested) {
            return requested.empty() ?
                       fs::path("Data") / "Sound" / "fx" / "mus" / "bard" :
                       requested;
        }
    }

    BaPrepareStats PrepareInstalledBaLibrary(const fs::path& songsRoot,
                                             const fs::path& requestedSource) {
        BaPrepareStats stats;
        const auto     pluginRoot = songsRoot.parent_path();
        const auto     compatRoot =
            pluginRoot / "compat" / "ba-bard-songs";
        const auto manifestPath = compatRoot / "ba_bard_songs.tsv";
        const auto templateRoot = compatRoot / "songs";
        if (!fs::is_regular_file(manifestPath) ||
            !fs::is_directory(templateRoot)) {
            return stats;  // compatibility pack is not part of this install
        }

        std::vector<ba::ManifestEntry> manifest;
        std::string                    error;
        if (!LoadManifest(pluginRoot, manifest, error)) {
            spdlog::error("[ba-compat] invalid manifest: {}", error);
            ++stats.failed;
            return stats;
        }
        std::unordered_map<std::string, const ba::ManifestEntry*> byId;
        for (const auto& entry : manifest) byId.emplace(entry.sourceId, &entry);

        const fs::path sourceRoot = DefaultSourceRoot(requestedSource);
        std::error_code ec;
        std::vector<fs::path> templates;
        for (fs::directory_iterator it(templateRoot, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec)) templates.push_back(it->path());
        }
        std::ranges::sort(templates);

        for (const auto& folder : templates) {
            ++stats.templates;
            const auto sourceId =
                ba::SourceIdFromIni(ReadText(folder / "song.ini"));
            const auto manifestIt = byId.find(sourceId);
            if (sourceId.empty() || manifestIt == byId.end()) {
                spdlog::error("[ba-compat] template {} has unknown source_id",
                              path_text::Utf8(folder.filename()));
                ++stats.failed;
                continue;
            }
            const auto source =
                sourceRoot / manifestIt->second->sourceRelative;
            if (!fs::is_regular_file(source, ec)) continue;
            ++stats.sourcesFound;

            const auto destination = songsRoot / folder.filename();
            const auto marker = destination / ".bardhero-ba-generated";
            const bool exists = fs::is_directory(destination, ec);
            const bool managed = fs::is_regular_file(marker, ec);
            if (exists && !managed) {
                // Never overwrite a hand-authored or older locally imported
                // chart. Its audio and metadata remain entirely user-owned.
                ++stats.customSkipped;
                continue;
            }

            error.clear();
            if (!CopyTemplate(folder, destination, error)) {
                spdlog::error("[ba-compat] {}: {}",
                              path_text::Utf8(folder.filename()), error);
                ++stats.failed;
                continue;
            }
            if (!WriteMarker(destination, sourceId)) {
                spdlog::error("[ba-compat] {} could not write ownership marker",
                              path_text::Utf8(folder.filename()));
                ++stats.failed;
                continue;
            }
            if (HasReadyAudio(destination)) {
                ++stats.ready;
                continue;
            }
            if (!WritePendingAudio(destination, error)) {
                spdlog::error("[ba-compat] {}: {}",
                              path_text::Utf8(folder.filename()), error);
                ++stats.failed;
                continue;
            }
            if (!exists) ++stats.generated;
            ++stats.pending;
        }
        return stats;
    }

    bool EnsureInstalledBaSongAudio(const fs::path& songFolder,
                                    const fs::path& requestedSource,
                                    std::string* callerError) {
        std::string localError;
        auto&       error = callerError ? *callerError : localError;
        error.clear();
        if (!fs::is_regular_file(songFolder / ".bardhero-ba-generated")) {
            return true;
        }
        const auto audio = songFolder / "song.opus";
        std::error_code ec;
        if (fs::is_regular_file(audio, ec) && fs::file_size(audio, ec) > 0 &&
            !ec) {
            return true;
        }

        const auto sourceId =
            ba::SourceIdFromIni(ReadText(songFolder / "song.ini"));
        if (sourceId.empty()) {
            error = "managed BA song has no source_id";
            return false;
        }
        std::vector<ba::ManifestEntry> manifest;
        if (!LoadManifest(songFolder.parent_path().parent_path(), manifest,
                          error)) {
            error = "invalid BA compatibility manifest: " + error;
            return false;
        }
        const auto* entry = FindSource(manifest, sourceId);
        if (!entry) {
            error = "source_id is not present in the BA manifest";
            return false;
        }
        const auto source =
            DefaultSourceRoot(requestedSource) / entry->sourceRelative;
        if (!fs::is_regular_file(source, ec)) {
            error = "required BA source is missing";
            return false;
        }

        const auto partial = songFolder / "song.opus.partial";
        fs::remove(partial, ec);
        if (!TranscodeXwmToOpus(source, partial, error)) return false;
        if (!MoveFileExW(partial.c_str(), audio.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = "could not publish the local BA audio cache (Windows " +
                    std::to_string(GetLastError()) + ')';
            fs::remove(partial, ec);
            return false;
        }
        if (!fs::is_regular_file(audio, ec) || fs::file_size(audio, ec) == 0 ||
            ec) {
            error = "published BA audio cache is empty";
            return false;
        }
        return true;
    }
}
