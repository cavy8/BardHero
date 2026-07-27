// src/game/SongLibrary.cpp
#include "PCH.h"
#include "game/SongLibrary.h"

#include "Settings.h"
#include "game/BaLibraryBootstrap.h"
#include "game/SongChallengeLogic.h"
#include "game/SongCache.h"
#include "game/SongEligibility.h"
#include "game/SongIdentity.h"
#include "game/StarLedger.h"
#include "game/UserSongWatchLogic.h"
#include "game/UnlockLogic.h"
#include "util/PathText.h"

#include "chart/LoadSong.h"
#include "chart/SongIni.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

namespace SH {
    namespace {
        namespace fs = std::filesystem;

        std::atomic<bool> g_scanning{ false };
        std::atomic<bool> g_watcherStarted{ false };
        std::atomic<int>  g_bad{ 0 };
        std::mutex        g_mx;
        std::shared_ptr<const std::vector<SongInfo>> g_snap =
            std::make_shared<const std::vector<SongInfo>>();  // guarded

        fs::path CachePath() {
            return fs::path(Settings::GetSingleton().songsFolder)
                       .parent_path() /
                   "songcache.tsv";
        }

        std::vector<fs::path> SongRoots() {
            const auto& settings = Settings::GetSingleton();
            std::vector<fs::path> roots{
                path_text::FromUtf8(settings.songsFolder)
            };
            if (!settings.userSongsFolder.empty()) {
                roots.push_back(settings.userSongsFolder);
            }
            return roots;
        }

        long long MtimeOf(const fs::path& p) {
            std::error_code ec;
            const auto      t = fs::last_write_time(p, ec);
            return ec ? 0 : static_cast<long long>(
                                t.time_since_epoch().count());
        }

        std::string ReadFileText(const fs::path& p) {
            std::ifstream f(p, std::ios::binary);
            if (!f) return {};
            std::ostringstream ss;
            ss << f.rdbuf();
            return std::move(ss).str();
        }

        std::optional<song_challenge::Metrics> MeasureChallenge(
            const bard::ParsedChart& chart) {
            if (chart.notes.empty()) { return std::nullopt; }

            double first = chart.notes.front().time;
            double last  = first;
            std::vector<double> bpms;
            bpms.reserve(chart.notes.size());
            for (const auto& note : chart.notes) {
                last = std::max(last, note.time);
                for (double end : note.sustainEnd) {
                    if (end > 0.0) { last = std::max(last, end); }
                }
                const double bpm = chart.tempo.BpmAtTick(note.tick);
                if (std::isfinite(bpm) && bpm > 0.0) {
                    bpms.push_back(bpm);
                }
            }
            const double noteSpan = std::max(last - first, 1.0);
            const double duration =
                chart.meta.songLengthMs > 0.0
                    ? std::max(chart.meta.songLengthMs / 1000.0, noteSpan)
                    : noteSpan;

            std::sort(bpms.begin(), bpms.end());
            if (bpms.empty()) { return std::nullopt; }
            const double medianBpm = bpms[bpms.size() / 2];

            const double window = std::min(5.0, noteSpan);
            std::size_t  left = 0;
            std::size_t  peak = 0;
            for (std::size_t right = 0; right < chart.notes.size(); ++right) {
                while (left < right &&
                       chart.notes[right].time - chart.notes[left].time >
                           window) {
                    ++left;
                }
                peak = std::max(peak, right - left + 1);
            }

            song_challenge::Metrics metrics;
            metrics.bpm         = medianBpm;
            metrics.averageNps  =
                static_cast<double>(chart.notes.size()) / noteSpan;
            metrics.peakNps     = static_cast<double>(peak) / window;
            metrics.durationSec = duration;
            return metrics;
        }

        void ScanWorkerBody(std::string_view& stage) {
            // start line pairs with the completion line below: a scan that
            // never STARTS (or hangs) is diagnosable from the log alone
            const auto roots = SongRoots();
            spdlog::info("[library] scan started: packaged={} user={}",
                         Settings::GetSingleton().songsFolder,
                         Settings::GetSingleton().userSongsFolder.empty()
                             ? std::string("(disabled)")
                             : path_text::Utf8(
                                   Settings::GetSingleton().userSongsFolder));
            if (!Settings::GetSingleton().userSongsFolder.empty()) {
                std::error_code createEc;
                fs::create_directories(
                    Settings::GetSingleton().userSongsFolder / "guitar",
                    createEc);
                if (createEc) {
                    spdlog::warn(
                        "[library] could not create user-song folder {}: {}",
                        path_text::Utf8(
                            Settings::GetSingleton().userSongsFolder),
                        createEc.message());
                }
            }
            stage = "BA preparation";
            const auto ba = PrepareInstalledBaLibrary(
                Settings::GetSingleton().songsFolder);
            if (ba.templates > 0) {
                spdlog::info(
                    "[ba-compat] {} templates, {} BA sources, {} generated, "
                    "{} pending, {} ready, {} local/custom kept, {} failed",
                    ba.templates, ba.sourcesFound, ba.generated, ba.pending,
                    ba.ready, ba.customSkipped, ba.failed);
            }
            stage = "filesystem discovery";
            const auto scan = bard::ScanSongs(roots);
            g_bad.store(static_cast<int>(scan.bad.size()));
            for (const auto& b : scan.bad) {
                spdlog::info("[library] rejected {}: {}",
                             path_text::Utf8(b.folder), b.reason);
            }
            if (!scan.complete) {
                const auto retained = [&]() {
                    std::scoped_lock lk(g_mx);
                    return g_snap->size();
                }();
                spdlog::warn(
                    "[library] incomplete filesystem scan; retained previous "
                    "snapshot ({} songs, {} rejected)",
                    retained, g_bad.load());
                return;
            }
            stage = "cache read";
            std::map<std::string, SongCacheEntry> cache;
            for (auto& e : ParseSongCache(ReadFileText(CachePath()))) {
                cache[e.folder] = std::move(e);
            }
            auto out = std::make_shared<std::vector<SongInfo>>();
            std::vector<SongCacheEntry> newCache;
            std::vector<song_challenge::RankInput> rankInputs;
            int inferredGuitarCount = 0;
            stage = "metadata and chart measurement";
            for (const auto& entry : scan.songs) {
                SongInfo    si;
                si.entry = entry;
                const auto key   = path_text::Utf8(entry.folder);
                const auto mtime = MtimeOf(entry.chartFile);
                const auto it    = cache.find(key);
                if (it != cache.end() && it->second.chartMtime == mtime) {
                    si.name     = it->second.name;
                    si.artist   = it->second.artist;
                    si.charter  = it->second.charter;
                    si.lengthMs   = it->second.lengthMs;
                    si.diff       = it->second.diff;
                    si.instrument = it->second.instrument;
                    si.unlockRank = it->second.unlockRank;
                    // Eligibility metadata must not be trapped behind a
                    // chart-mtime-only cache hit. In particular, caches
                    // written before guitar was a recognized tag stored it
                    // as empty. Re-read just this small song.ini field so an
                    // installed Electric repertoire appears immediately.
                    if (!entry.iniFile.empty()) {
                        bard::SongIniValues currentIni;
                        bard::ParseSongIniText(
                            ReadFileText(entry.iniFile), currentIni);
                        si.instrument = currentIni.instrument;
                    }
                } else {
                    if (!entry.iniFile.empty()) {
                        bard::SongIniValues ini;
                        bard::ParseSongIniText(ReadFileText(entry.iniFile),
                                               ini);
                        si.name       = ini.name;
                        si.artist     = ini.artist;
                        si.charter    = ini.charter;
                        si.lengthMs   = ini.songLengthMs;
                        si.diff       = ini.diffGuitar;
                        si.instrument = ini.instrument;
                        si.unlockRank = ini.unlockRank;
                    }
                    if (si.name.empty()) {
                        si.name = path_text::Utf8(entry.folder.filename());
                    }
                }
                const bool hadExplicitInstrument =
                    songeligibility::TaggedInstrument(si.instrument)
                        .has_value();
                si.instrument =
                    songeligibility::ResolveScannedInstrumentTag(
                        si.instrument, entry.folder);
                if (!hadExplicitInstrument && si.instrument == "guitar") {
                    ++inferredGuitarCount;
                }
                newCache.push_back({ key, mtime, si.name, si.artist,
                                     si.charter, si.lengthMs, si.diff,
                                     si.instrument, si.unlockRank });

                const int fallback =
                    unlock::RequiredRank(si.diff, si.unlockRank);
                song_challenge::RankInput rankInput;
                rankInput.fallbackRank = fallback;
                rankInput.key          = key;
                rankInput.measured     = false;
                if (const auto instrument =
                        songeligibility::TaggedInstrument(si.instrument)) {
                    rankInput.instrument = *instrument;
                }
                bard::LoadedSong measuredSong;
                std::string      measureError;
                if (bard::LoadSong(entry, 3, measuredSong, &measureError)) {
                    // The measure load probes every difficulty anyway -
                    // carry the availability mask to the browser (Diff
                    // column + per-difficulty stars).
                    si.diffMask = measuredSong.difficultyMask;
                    if (const auto metrics =
                            MeasureChallenge(measuredSong.chart)) {
                        si.challengeScore =
                            song_challenge::Score(*metrics);
                        si.challengeMeasured = true;
                        rankInput.score       = si.challengeScore;
                        rankInput.measured =
                            rankInput.instrument >= 0;
                    }
                } else {
                    spdlog::warn(
                        "[unlock] challenge fallback for {}: {}", key,
                        measureError);
                }
                rankInputs.push_back(std::move(rankInput));
                out->push_back(std::move(si));
            }
            spdlog::info(
                "[library] inferred guitar tag for {} scanned folder(s)",
                inferredGuitarCount);
            stage = "natural rank assignment";
            const auto ranks = song_challenge::RankLibrary(rankInputs, 2);
            int rankCounts[4][5]{};
            int measuredCount = 0;
            for (std::size_t i = 0; i < out->size(); ++i) {
                auto& song = (*out)[i];
                song.requiredRank =
                    i < ranks.size()
                        ? ranks[i]
                        : unlock::RequiredRank(song.diff, song.unlockRank);
                if (song.challengeMeasured) { ++measuredCount; }
                const auto instrument =
                    songeligibility::TaggedInstrument(song.instrument);
                if (instrument && song.requiredRank >= 1 &&
                    song.requiredRank <= 5) {
                    ++rankCounts[*instrument][song.requiredRank - 1];
                }
            }
            spdlog::info(
                "[unlock] measured {}/{} charts; natural ranks "
                "lute={}/{}/{}/{}/{} flute={}/{}/{}/{}/{} "
                "drum={}/{}/{}/{}/{} guitar={}/{}/{}/{}/{}",
                measuredCount, out->size(),
                rankCounts[0][0], rankCounts[0][1], rankCounts[0][2],
                rankCounts[0][3], rankCounts[0][4],
                rankCounts[1][0], rankCounts[1][1], rankCounts[1][2],
                rankCounts[1][3], rankCounts[1][4],
                rankCounts[2][0], rankCounts[2][1], rankCounts[2][2],
                rankCounts[2][3], rankCounts[2][4],
                rankCounts[3][0], rankCounts[3][1], rankCounts[3][2],
                rankCounts[3][3], rankCounts[3][4]);
            std::sort(out->begin(), out->end(),
                      [](const SongInfo& a, const SongInfo& b) {
                          const auto lc = [](const std::string& s) {
                              std::string r = s;
                              for (auto& c : r) {
                                  c = static_cast<char>(std::tolower(
                                      static_cast<unsigned char>(c)));
                              }
                              return r;
                          };
                          return lc(a.name) < lc(b.name);
                      });
            stage = "cache write";
            {
                std::ofstream f(CachePath(), std::ios::binary);
                if (f) f << SerializeSongCache(newCache);
            }
            stage = "snapshot publish";
            const auto publishedCount = out->size();
            // Charts that appeared since the LAST published snapshot get the
            // NEW tag - this is how a Bridge download shows up as new
            // (user request 2026-07-26). Deliberately gated on there being a
            // previous snapshot: the first scan of a process would otherwise
            // tag the entire library, which is noise rather than news. That
            // also means a chart installed while the game was closed is not
            // tagged, which is the honest limit of a snapshot diff - only a
            // persisted "already seen" set could catch that, and the ledger
            // has no such field.
            std::vector<std::string> freshKeys;
            {
                std::scoped_lock lk(g_mx);
                const bool hadPrevious = !g_snap->empty();
                if (hadPrevious) {
                    std::set<std::string> previous;
                    for (const auto& song : *g_snap) {
                        previous.insert(
                            song_identity::ChartKey(song.entry.folder));
                    }
                    for (const auto& song : *out) {
                        auto key =
                            song_identity::ChartKey(song.entry.folder);
                        if (!previous.count(key)) {
                            freshKeys.push_back(std::move(key));
                        }
                    }
                }
                g_snap = std::move(out);
            }
            // Outside the library mutex: the ledger takes its own, and
            // holding two locks across a call is the deadlock this project
            // already has a rule about.
            for (const auto& key : freshKeys) {
                StarLedger::GetSingleton().MarkNew(key);
            }
            if (!freshKeys.empty()) {
                spdlog::info("[library] {} newly added chart(s) tagged NEW",
                             freshKeys.size());
            }
            spdlog::info("[library] {} songs ({} rejected)",
                         publishedCount, g_bad.load());
        }

        void ScanWorker() noexcept {
            std::string_view stage = "startup";
            try {
                ScanWorkerBody(stage);
            } catch (const std::exception& e) {
                spdlog::critical("[library] scan failed during {}: {}",
                                 stage, e.what());
            } catch (...) {
                spdlog::critical(
                    "[library] scan failed during {}: unknown exception",
                    stage);
            }
            g_scanning.store(false);
        }

        bool Launch() {
            bool expected = false;
            if (!g_scanning.compare_exchange_strong(expected, true)) {
                return false;
            }
            std::thread(ScanWorker).detach();
            return true;
        }

        double WatchNowSeconds() {
            return std::chrono::duration<double>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        void UserSongWatchWorker(fs::path root) noexcept {
            constexpr double kQuietSeconds = 2.0;
            const DWORD flags =
                FILE_NOTIFY_CHANGE_FILE_NAME |
                FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE;
            const HANDLE change =
                FindFirstChangeNotificationW(root.c_str(), TRUE, flags);
            if (change == INVALID_HANDLE_VALUE) {
                spdlog::warn(
                    "[library] automatic user-song watcher failed for {} "
                    "(Windows error {})",
                    path_text::Utf8(root), GetLastError());
                g_watcherStarted.store(false);
                return;
            }

            spdlog::info(
                "[library] automatic user-song watcher active: {} "
                "(quiet {:.1f}s)",
                path_text::Utf8(root), kQuietSeconds);
            user_song_watch::QuietPeriod quiet(kQuietSeconds);
            for (;;) {
                DWORD timeout = INFINITE;
                if (quiet.Pending()) {
                    const double remaining =
                        std::max(0.0, quiet.Deadline() - WatchNowSeconds());
                    timeout = static_cast<DWORD>(
                        std::min(remaining * 1000.0 + 0.999, 60000.0));
                }

                const DWORD wait = WaitForSingleObject(change, timeout);
                if (wait == WAIT_OBJECT_0) {
                    if (!FindNextChangeNotification(change)) {
                        spdlog::warn(
                            "[library] automatic user-song watcher could not "
                            "continue (Windows error {})",
                            GetLastError());
                        break;
                    }
                    quiet.ObserveChange(WatchNowSeconds());
                    continue;
                }
                if (wait == WAIT_TIMEOUT) {
                    if (quiet.TakeDue(WatchNowSeconds())) {
                        if (Launch()) {
                            spdlog::info(
                                "[library] user-song changes settled; "
                                "automatic rescan started");
                        } else {
                            // A manual or startup scan still owns the worker.
                            // Retry this settled batch instead of dropping it.
                            quiet.ObserveChange(WatchNowSeconds());
                        }
                    }
                    continue;
                }

                spdlog::warn(
                    "[library] automatic user-song watcher stopped "
                    "(Windows error {})",
                    GetLastError());
                break;
            }
            FindCloseChangeNotification(change);
            g_watcherStarted.store(false);
        }

        void StartUserSongWatcher() {
            const auto& settings = Settings::GetSingleton();
            if (!settings.autoRescanUserSongs ||
                settings.userSongsFolder.empty()) {
                return;
            }
            bool expected = false;
            if (!g_watcherStarted.compare_exchange_strong(expected, true)) {
                return;
            }

            std::error_code ec;
            fs::create_directories(settings.userSongsFolder / "guitar", ec);
            if (ec) {
                spdlog::warn(
                    "[library] automatic user-song watcher could not create "
                    "{}: {}",
                    path_text::Utf8(settings.userSongsFolder), ec.message());
                g_watcherStarted.store(false);
                return;
            }
            std::thread(UserSongWatchWorker, settings.userSongsFolder)
                .detach();
        }
    }

    SongLibrary& SongLibrary::GetSingleton() {
        static SongLibrary s;
        return s;
    }

    void SongLibrary::EnsureScan() {
        static std::atomic<bool> once{ false };
        bool                     expected = false;
        if (once.compare_exchange_strong(expected, true)) {
            StartUserSongWatcher();
            Launch();
        }
    }

    void SongLibrary::Rescan() {
        StartUserSongWatcher();
        Launch();
    }
    // The library is READ ONLY: nothing in this file writes to, moves, or
    // removes a chart folder. See the note in SongLibrary.h.

    bool SongLibrary::Scanning() const { return g_scanning.load(); }
    int  SongLibrary::BadCount() const { return g_bad.load(); }

    std::shared_ptr<const std::vector<SongInfo>>
    SongLibrary::Snapshot() const {
        std::scoped_lock lk(g_mx);
        return g_snap;
    }
}
