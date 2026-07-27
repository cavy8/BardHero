#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace bard {

    // Low-level SMF (type 0/1, PPQN division only) reader. Tolerant per spec
    // 4.4: running status survives interleaved meta/SysEx events; 0xFF bytes
    // inside SysEx payloads are data, not terminators.
    struct SmfNote {
        std::uint32_t tick = 0;
        std::uint8_t  key  = 0;
        bool          on   = false;  // note-on vel 0 reported as off
    };
    struct SmfSysEx {
        std::uint32_t             tick = 0;
        std::vector<std::uint8_t> data;  // payload without F0/F7 framing
    };
    struct SmfText {
        std::uint32_t tick = 0;
        std::string   text;
    };
    struct SmfTrack {
        std::string           name;  // meta 0x03 (last wins)
        std::vector<SmfNote>  notes;
        std::vector<SmfSysEx> sysex;
        std::vector<SmfText>  texts;  // meta 0x01
    };
    struct SmfFile {
        std::uint16_t         division = 480;  // ticks per quarter
        std::vector<SmfTrack> tracks;
        // (tick, bpm) from meta 0x51 in ANY track (type-1 tempo track)
        std::vector<std::pair<std::uint32_t, double>> tempoTrackBpms;
        // (tick, num, denomPow2) from meta 0x58
        std::vector<std::tuple<std::uint32_t, std::uint8_t, std::uint8_t>>
            timeSigs;
    };

    bool ParseSmf(const std::uint8_t* data, std::size_t size, SmfFile& out);
}
