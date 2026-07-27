// bardcore/chart/LoadSong.cpp
#include "chart/LoadSong.h"

#include <array>
#include <fstream>
#include <sstream>
#include <utility>

#include "chart/ChartParser.h"
#include "chart/MidParser.h"
#include "chart/Normalize.h"
#include "chart/Smf.h"
#include "practice/PracticeSections.h"
#include "util/PathText.h"

namespace bard {
    namespace {
        bool ReadFileBytes(const std::filesystem::path& p, std::string& out,
                           std::string* error) {
            std::ifstream f(p, std::ios::binary);
            if (!f) {
                if (error) {
                    *error = "cannot open " + SH::path_text::Utf8(p);
                }
                return false;
            }
            std::ostringstream ss;
            ss << f.rdbuf();
            out = std::move(ss).str();
            return true;
        }

        constexpr std::array<const char*, 4> kChartSections = {
            "EasySingle", "MediumSingle", "HardSingle", "ExpertSingle"
        };

        void OverlayMeta(const SongIniValues& ini, SongMeta& meta) {
            if (!ini.name.empty()) meta.name = ini.name;
            if (!ini.artist.empty()) meta.artist = ini.artist;
            if (!ini.album.empty()) meta.album = ini.album;
            if (!ini.charter.empty()) meta.charter = ini.charter;
            if (!ini.loadingPhrase.empty()) meta.loadingPhrase = ini.loadingPhrase;
            if (ini.diffGuitar >= 0) meta.diffGuitar = ini.diffGuitar;
            if (ini.previewStartMs >= 0.0) meta.previewStartMs = ini.previewStartMs;
            if (ini.songLengthMs >= 0.0) meta.songLengthMs = ini.songLengthMs;
        }
    }

    bool LoadSong(const SongEntry& entry, int difficulty, LoadedSong& out,
                  std::string* error) {
        out = {};
        if (difficulty < 0 || difficulty > 3) difficulty = 3;

        if (!entry.iniFile.empty()) {
            std::string iniText;
            if (ReadFileBytes(entry.iniFile, iniText, nullptr)) {
                ParseSongIniText(iniText, out.ini);  // tolerant; defaults on failure
            }
        }

        std::string bytes;
        if (!ReadFileBytes(entry.chartFile, bytes, error)) {
            return false;
        }

        if (!entry.isMid) {
            ChartFile cf;
            if (!ParseChartText(bytes, cf)) {
                if (error) *error = "notes.chart parse failed";
                return false;
            }
            // Probe every difficulty, not just the fallback path: the mask
            // feeds the Songbook's availability display and the
            // per-difficulty record keying, so "which difficulties exist"
            // must be answered even for the ones this load skips.
            std::array<RawTrack, 4> cand{};
            for (int d = 0; d < 4; ++d) {
                RawTrack c;
                if (BuildRawTrack(cf, kChartSections[d], c) &&
                    !c.chords.empty()) {
                    out.difficultyMask |= static_cast<std::uint8_t>(1u << d);
                    cand[d] = std::move(c);
                }
            }
            out.resolvedDifficulty =
                ResolveDifficulty(out.difficultyMask, difficulty);
            if (out.resolvedDifficulty < 0) {
                if (error) *error = "no guitar difficulty section";
                return false;
            }
            RawTrack t = std::move(cand[out.resolvedDifficulty]);
            const auto hopo = ResolveHopoThreshold(out.ini, cf.resolution, false);
            if (hopo > 0) t.hopoThresholdTicks = static_cast<std::uint32_t>(hopo);
            ApplySustainCutoff(t, ResolveSustainCutoff(out.ini, cf.resolution, false));
            if (!out.ini.endEvents) t.hasEnd = false;  // spec 4.2 override
            const double offset = cf.offsetSeconds + out.ini.delayMs / 1000.0;
            out.chart           = Normalize(t, cf.tempo, offset);
            out.chart.timeSigs  = cf.timeSigs;
            out.chart.meta      = cf.meta;
            // Practice-mode section markers (spec
            // 2026-07-26-practice-mode). Same offset the notes carry, so
            // section times and note times share one domain.
            out.chart.sections  = practice::SectionsFromChart(cf, offset);
        } else {
            SmfFile f;
            if (!ParseSmf(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                          bytes.size(), f)) {
                if (error) *error = "notes.mid parse failed";
                return false;
            }
            TempoMap tempo;
            tempo.SetResolution(f.division);
            for (const auto& [tick, bpm] : f.tempoTrackBpms) tempo.AddBpm(tick, bpm);
            tempo.Finalize();

            MidOptions opt;
            opt.sustainCutoffTicks = ResolveSustainCutoff(out.ini, f.division, true);
            opt.hopoThresholdTicks = ResolveHopoThreshold(out.ini, f.division, true);
            opt.spNote             = out.ini.multiplierNote;

            // Same all-four probe as the .chart path (mask + resolved).
            std::array<RawTrack, 4> cand{};
            for (int d = 0; d < 4; ++d) {
                RawTrack c;
                if (BuildRawTrackFromMid(f, d, opt, c) && !c.chords.empty()) {
                    out.difficultyMask |= static_cast<std::uint8_t>(1u << d);
                    cand[d] = std::move(c);
                }
            }
            out.resolvedDifficulty =
                ResolveDifficulty(out.difficultyMask, difficulty);
            if (out.resolvedDifficulty < 0) {
                if (error) *error = "no guitar notes in any difficulty";
                return false;
            }
            RawTrack t = std::move(cand[out.resolvedDifficulty]);
            if (!out.ini.endEvents) t.hasEnd = false;
            const double offset = out.ini.delayMs / 1000.0;
            out.chart           = Normalize(t, tempo, offset);
            for (const auto& [tick, num, denomPow] : f.timeSigs) {
                out.chart.timeSigs.push_back(
                    { tick, num, static_cast<std::uint32_t>(1u << denomPow) });
            }
            // Practice-mode section markers (spec
            // 2026-07-26-practice-mode). `tempo` is finalized above, and
            // the offset is the same one Normalize gave the notes.
            out.chart.sections = practice::SectionsFromMid(f, tempo, offset);
        }
        OverlayMeta(out.ini, out.chart.meta);
        if (out.chart.notes.empty()) {
            if (error) *error = "chart has zero notes";
            return false;
        }
        return true;
    }
}
