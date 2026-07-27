#include "replay/Replay.h"

#include <cstdio>
#include <cstring>

namespace bard {

    namespace {
        constexpr char          kMagic[4] = { 'S', 'H', 'R', 'P' };
        constexpr std::uint32_t kVersion  = 1;
    }

    bool SaveReplay(const std::string& path,
                    const std::vector<NoteInput>& in) {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        const auto count = static_cast<std::uint32_t>(in.size());
        bool ok = std::fwrite(kMagic, 4, 1, f) == 1 &&
                  std::fwrite(&kVersion, 4, 1, f) == 1 &&
                  std::fwrite(&count, 4, 1, f) == 1;
        if (ok && count > 0) {
            ok = std::fwrite(in.data(), sizeof(NoteInput), count, f) == count;
        }
        std::fclose(f);
        return ok;
    }

    bool LoadReplay(const std::string& path, std::vector<NoteInput>& out) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        char          magic[4];
        std::uint32_t version = 0, count = 0;
        bool ok = std::fread(magic, 4, 1, f) == 1 &&
                  std::memcmp(magic, kMagic, 4) == 0 &&
                  std::fread(&version, 4, 1, f) == 1 && version == kVersion &&
                  std::fread(&count, 4, 1, f) == 1;
        if (ok) {
            out.resize(count);
            if (count > 0) {
                ok = std::fread(out.data(), sizeof(NoteInput), count, f) ==
                     count;
            }
        }
        std::fclose(f);
        return ok;
    }

    ReplayResult RunReplay(const ParsedChart& chart, const EngineParams& p,
                           const std::vector<NoteInput>& inputs,
                           double cadenceHz, double endTime) {
        GuitarEngine e(chart, p);
        std::size_t  qi = 0;
        const double dt = 1.0 / cadenceHz;
        for (double t = 0.0; t < endTime; t += dt) {
            while (qi < inputs.size() && inputs[qi].time <= t) {
                e.Queue(inputs[qi++]);
            }
            e.Update(t);
        }
        while (qi < inputs.size()) e.Queue(inputs[qi++]);
        e.Update(endTime);

        ReplayResult r{ e.Stats(), {} };
        r.judgments.reserve(chart.notes.size());
        for (std::size_t i = 0; i < chart.notes.size(); ++i) {
            r.judgments.push_back(e.JudgmentOf(i));
        }
        return r;
    }
}
