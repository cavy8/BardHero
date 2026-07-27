#include "chart/Smf.h"

namespace bard {
    namespace {
        struct Reader {
            const std::uint8_t* p;
            const std::uint8_t* end;
            bool                ok = true;

            std::uint8_t U8() {
                if (p >= end) {
                    ok = false;
                    return 0;
                }
                return *p++;
            }
            std::uint32_t U32() {
                std::uint32_t v = 0;
                for (int i = 0; i < 4; ++i) v = (v << 8) | U8();
                return v;
            }
            std::uint16_t U16() {
                std::uint16_t v = 0;
                for (int i = 0; i < 2; ++i)
                    v = static_cast<std::uint16_t>((v << 8) | U8());
                return v;
            }
            std::uint32_t Vlq() {
                std::uint32_t v = 0;
                for (int i = 0; i < 4; ++i) {
                    const auto b = U8();
                    v            = (v << 7) | (b & 0x7F);
                    if (!(b & 0x80)) break;
                }
                return v;
            }
            void Skip(std::uint32_t n) {
                if (end - p < static_cast<std::ptrdiff_t>(n)) {
                    ok = false;
                    p  = end;
                } else {
                    p += n;
                }
            }
        };
    }

    bool ParseSmf(const std::uint8_t* data, std::size_t size, SmfFile& out) {
        Reader r{ data, data + size };
        if (r.U8() != 'M' || r.U8() != 'T' || r.U8() != 'h' || r.U8() != 'd')
            return false;
        if (r.U32() != 6) return false;
        const auto format = r.U16();
        const auto ntrks  = r.U16();
        const auto div    = r.U16();
        if (format > 1) return false;
        if (div & 0x8000) return false;  // SMPTE unsupported (spec 4.4)
        out.division = div;

        for (std::uint16_t ti = 0; ti < ntrks && r.ok; ++ti) {
            if (r.U8() != 'M' || r.U8() != 'T' || r.U8() != 'r' || r.U8() != 'k')
                return false;
            const auto  len      = r.U32();
            const auto* trackEnd = r.p + len;
            if (trackEnd > r.end) return false;

            SmfTrack      track;
            std::uint32_t tick    = 0;
            std::uint8_t  running = 0;

            while (r.p < trackEnd && r.ok) {
                tick += r.Vlq();
                if (r.p >= trackEnd) break;
                std::uint8_t status = *r.p;
                if (status < 0x80) {
                    // Running status - do NOT consume the byte. Tolerated even
                    // right after meta/SysEx (breaks strict parsers, spec 4.4).
                    status = running;
                    if (status == 0) return false;
                } else {
                    r.U8();
                    if (status < 0xF0) running = status;
                }
                const auto type = status & 0xF0;
                if (status == 0xFF) {  // meta
                    const auto  metaType = r.U8();
                    const auto  metaLen  = r.Vlq();
                    const auto* body     = r.p;
                    r.Skip(metaLen);
                    if (!r.ok) break;
                    if (metaType == 0x03) {
                        track.name.assign(reinterpret_cast<const char*>(body),
                                          metaLen);
                    } else if (metaType == 0x01) {
                        track.texts.push_back(
                            { tick,
                              std::string(reinterpret_cast<const char*>(body),
                                          metaLen) });
                    } else if (metaType == 0x51 && metaLen == 3) {
                        const std::uint32_t us =
                            (body[0] << 16) | (body[1] << 8) | body[2];
                        if (us > 0) {
                            out.tempoTrackBpms.emplace_back(tick,
                                                            60000000.0 / us);
                        }
                    } else if (metaType == 0x58 && metaLen >= 2) {
                        out.timeSigs.emplace_back(tick, body[0], body[1]);
                    } else if (metaType == 0x2F) {
                        r.p = trackEnd;  // end of track
                    }
                } else if (status == 0xF0 || status == 0xF7) {  // sysex
                    const auto sxLen = r.Vlq();
                    SmfSysEx   sx;
                    sx.tick = tick;
                    // Length-delimited: 0xFF inside the payload is DATA (spec
                    // 4.4). A trailing F7 terminator is framing - drop it.
                    for (std::uint32_t i = 0; i < sxLen && r.ok; ++i) {
                        sx.data.push_back(r.U8());
                    }
                    if (!sx.data.empty() && sx.data.back() == 0xF7) {
                        sx.data.pop_back();
                    }
                    track.sysex.push_back(std::move(sx));
                } else if (type == 0x90 || type == 0x80) {
                    const auto key = r.U8();
                    const auto vel = r.U8();
                    track.notes.push_back(
                        { tick, key, type == 0x90 && vel > 0 });
                } else if (type == 0xA0 || type == 0xB0 || type == 0xE0) {
                    r.Skip(2);
                } else if (type == 0xC0 || type == 0xD0) {
                    r.Skip(1);
                } else {
                    return false;
                }
            }
            if (!r.ok) return false;
            r.p = trackEnd;
            out.tracks.push_back(std::move(track));
        }
        return r.ok;
    }
}
