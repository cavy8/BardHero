#include "harness.h"
#include "chart/Smf.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace bard;

// Minimal SMF byte builder for synthetic tests.
struct SmfBuilder {
    std::vector<std::uint8_t> bytes;
    void U16(std::uint16_t v) {
        bytes.push_back(v >> 8);
        bytes.push_back(v & 0xFF);
    }
    void U32(std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) bytes.push_back((v >> s) & 0xFF);
    }
    void Raw(std::initializer_list<std::uint8_t> b) {
        bytes.insert(bytes.end(), b);
    }
    void Str(const std::string& s) {
        for (char c : s) bytes.push_back(static_cast<std::uint8_t>(c));
    }
    void Vlq(std::uint32_t v) {
        std::uint8_t buf[4];
        int          n = 0;
        do {
            buf[n++] = v & 0x7F;
            v >>= 7;
        } while (v);
        while (n--) bytes.push_back(buf[n] | (n ? 0x80 : 0));
    }
    void Header(std::uint16_t ntrks, std::uint16_t division) {
        Raw({ 'M', 'T', 'h', 'd' });
        U32(6);
        U16(1);
        U16(ntrks);
        U16(division);
    }
    // returns index where track length must be patched
    std::size_t BeginTrack() {
        Raw({ 'M', 'T', 'r', 'k' });
        U32(0);
        return bytes.size();
    }
    void EndTrack(std::size_t start) {
        Vlq(0);
        Raw({ 0xFF, 0x2F, 0x00 });
        const auto len = static_cast<std::uint32_t>(bytes.size() - start);
        for (int s = 0; s < 4; ++s) {
            bytes[start - 4 + s] = (len >> (24 - 8 * s)) & 0xFF;
        }
    }
};

static void RunSmfTests() {
    {   // two tracks, name meta, note on/off, running status, tempo meta
        SmfBuilder b;
        b.Header(2, 480);
        auto t1 = b.BeginTrack();
        b.Vlq(0);
        b.Raw({ 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20 });  // 500000us = 120bpm
        b.EndTrack(t1);
        auto t2 = b.BeginTrack();
        b.Vlq(0);
        b.Raw({ 0xFF, 0x03, 0x0B });
        b.Str("PART GUITAR");
        b.Vlq(0);
        b.Raw({ 0x91, 96, 100 });  // note on 96
        b.Vlq(120);
        b.Raw({ 96, 0 });          // RUNNING STATUS note-on vel 0 = off
        b.EndTrack(t2);

        SmfFile f;
        CHECK(ParseSmf(b.bytes.data(), b.bytes.size(), f));
        CHECK(f.division == 480);
        CHECK(f.tracks.size() == 2);
        CHECK(f.tracks[1].name == "PART GUITAR");
        CHECK(f.tracks[1].notes.size() == 2);
        CHECK(f.tracks[1].notes[0].tick == 0 && f.tracks[1].notes[0].on);
        CHECK(f.tracks[1].notes[1].tick == 120 && !f.tracks[1].notes[1].on);
        CHECK(f.tempoTrackBpms.size() == 1);
        CHECK_NEAR(f.tempoTrackBpms[0].second, 120.0, 1e-6);
    }
    {   // robustness (spec 4.4): unreset running status after SysEx/meta must
        // parse; 0xFF INSIDE SysEx payload must not terminate it
        SmfBuilder b;
        b.Header(1, 480);
        auto t = b.BeginTrack();
        b.Vlq(0);
        b.Raw({ 0x91, 96, 100 });
        b.Vlq(0);
        b.Raw({ 0xF0, 0x04, 0x50, 0xFF, 0x53, 0xF7 });  // sysex w/ 0xFF inside
        b.Vlq(10);
        b.Raw({ 97, 100 });  // running status SURVIVES the sysex
        b.EndTrack(t);
        SmfFile f;
        CHECK(ParseSmf(b.bytes.data(), b.bytes.size(), f));
        CHECK(f.tracks[0].notes.size() == 2);
        CHECK(f.tracks[0].notes[1].key == 97);
        CHECK(f.tracks[0].notes[1].tick == 10);
        CHECK(f.tracks[0].sysex.size() == 1);
        CHECK(f.tracks[0].sysex[0].data.size() == 3);  // trailing F7 dropped
        CHECK(f.tracks[0].sysex[0].data[1] == 0xFF);
    }
    {   // SMPTE division rejected
        SmfBuilder b;
        b.Header(0, 0);
        b.bytes[12] = 0xE7;  // negative SMPTE fps byte -> high bit set
        SmfFile f;
        CHECK(!ParseSmf(b.bytes.data(), b.bytes.size(), f));
    }
}

// ---- mapper tests ---------------------------------------------------------
#include "chart/MidParser.h"

#include <algorithm>

// Overlapping note ranges need interleaved on/off events in time order; this
// collector sorts before emitting (off-before-on at equal ticks so zero-gap
// adjacent ranges don't fuse).
struct TrackEvents {
    struct Ev {
        std::uint32_t tick;
        std::uint8_t  key;
        bool          on;
    };
    std::vector<Ev> evs;
    void Note(std::uint32_t at, std::uint32_t len, std::uint8_t key) {
        evs.push_back({ at, key, true });
        evs.push_back({ at + len, key, false });
    }
    void Emit(SmfBuilder& b) {
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b2) {
            if (a.tick != b2.tick) return a.tick < b2.tick;
            return a.on < b2.on;  // off first
        });
        std::uint32_t cur = 0;
        for (const auto& e : evs) {
            b.Vlq(e.tick - cur);
            cur = e.tick;
            b.Raw({ static_cast<std::uint8_t>(e.on ? 0x90 : 0x80), e.key,
                    static_cast<std::uint8_t>(e.on ? 100 : 0) });
        }
    }
};

static void RunMidMapperTests() {
    {   // gems, chord snap (<=10 ticks merge to EARLIEST), sustain cutoff
        // (len < division/3 = 160 cut to 0), force strum, threshold
        SmfBuilder b;
        b.Header(1, 480);
        auto t = b.BeginTrack();
        b.Vlq(0);
        b.Raw({ 0xFF, 0x03, 0x0B });
        b.Str("PART GUITAR");
        TrackEvents ev;
        ev.Note(0, 100, 96);     // G, 100 < 160 -> cutoff to 0
        ev.Note(480, 200, 96);   // G, 200 >= 160 -> keeps 200
        ev.Note(488, 200, 97);   // R at 488: within 10 of 480 -> snaps to 480
        ev.Note(960, 100, 98);   // Y, cutoff to 0
        ev.Note(960, 10, 102);   // force-strum phrase over tick 960
        ev.Emit(b);
        b.EndTrack(t);

        SmfFile f;
        CHECK(ParseSmf(b.bytes.data(), b.bytes.size(), f));
        RawTrack   rt;
        MidOptions opt;  // defaults: no ini overrides
        CHECK(BuildRawTrackFromMid(f, 3 /*expert*/, opt, rt));
        CHECK(rt.chords.size() == 3);
        CHECK(rt.chords[0].tick == 0);
        CHECK(rt.chords[0].sustainTicks[0] == 0);  // cutoff applied
        CHECK(rt.chords[1].tick == 480);           // snap target (earliest)
        CHECK(rt.chords[1].mask == (LaneBit(0) | LaneBit(1)));
        CHECK(rt.chords[1].sustainTicks[0] == 200);
        CHECK(rt.chords[1].sustainTicks[1] == 200);  // R keeps its own length
        CHECK(rt.chords[2].tick == 960);
        CHECK(rt.chords[2].forcing == Forcing::kForceStrum);
        // .mid natural threshold = floor(division/3) + 1 = 161 (spec 4.4)
        CHECK(rt.hopoThresholdTicks == 161);
    }
    {   // SP fallback: without any 116, 103 IS star power (GH1/2 compat);
        // open 95 gated ON by [ENHANCED_OPENS] text event
        SmfBuilder b;
        b.Header(1, 480);
        auto t = b.BeginTrack();
        b.Vlq(0);
        b.Raw({ 0xFF, 0x03, 0x0B });
        b.Str("PART GUITAR");
        b.Vlq(0);
        b.Raw({ 0xFF, 0x01, 0x10 });
        b.Str("[ENHANCED_OPENS]");
        TrackEvents ev;
        ev.Note(0, 480, 95);      // open gem (gated ON)
        ev.Note(960, 480, 96);    // G inside the 103 phrase
        ev.Note(960, 480, 103);   // solo marker... becomes SP (no 116)
        ev.Emit(b);
        b.EndTrack(t);
        SmfFile f;
        CHECK(ParseSmf(b.bytes.data(), b.bytes.size(), f));
        RawTrack   rt;
        MidOptions opt;
        CHECK(BuildRawTrackFromMid(f, 3, opt, rt));
        CHECK(rt.chords.size() == 2);
        CHECK(rt.chords[0].mask == kOpenBit);
        CHECK(rt.spPhrases.size() == 1);  // 103 promoted to SP
        CHECK(rt.spPhrases[0].startTick == 960);
        CHECK(rt.solos.empty());
    }
    {   // with a 116 present, 103 stays a solo (inclusive end = off-tick - 1)
        SmfBuilder b;
        b.Header(1, 480);
        auto t = b.BeginTrack();
        b.Vlq(0);
        b.Raw({ 0xFF, 0x03, 0x0B });
        b.Str("PART GUITAR");
        TrackEvents ev;
        ev.Note(0, 480, 96);
        ev.Note(0, 480, 116);     // SP phrase
        ev.Note(0, 480, 103);     // solo
        ev.Emit(b);
        b.EndTrack(t);
        SmfFile f;
        CHECK(ParseSmf(b.bytes.data(), b.bytes.size(), f));
        RawTrack   rt;
        MidOptions opt;
        CHECK(BuildRawTrackFromMid(f, 3, opt, rt));
        CHECK(rt.spPhrases.size() == 1);
        CHECK(rt.solos.size() == 1);
        CHECK(rt.solos[0].endTick == 479);
    }
    {   // ungated open 95 is IGNORED (no text event, no PS SysEx)
        SmfBuilder b;
        b.Header(1, 480);
        auto t = b.BeginTrack();
        b.Vlq(0);
        b.Raw({ 0xFF, 0x03, 0x0B });
        b.Str("PART GUITAR");
        TrackEvents ev;
        ev.Note(0, 480, 95);
        ev.Note(960, 480, 96);
        ev.Emit(b);
        b.EndTrack(t);
        SmfFile f;
        CHECK(ParseSmf(b.bytes.data(), b.bytes.size(), f));
        RawTrack   rt;
        MidOptions opt;
        CHECK(BuildRawTrackFromMid(f, 3, opt, rt));
        CHECK(rt.chords.size() == 1);
        CHECK(rt.chords[0].mask == LaneBit(0));
    }
    {   // PS SysEx tap phrase: end tick AFFECTED (+1) - a note exactly on the
        // end marker is still tapped (spec 4.4 asymmetry). Hand-emitted so the
        // phrase-end SysEx interleaves at exactly tick 200.
        SmfBuilder b2;
        b2.Header(1, 480);
        auto t2 = b2.BeginTrack();
        b2.Vlq(0);
        b2.Raw({ 0xFF, 0x03, 0x0B });
        b2.Str("PART GUITAR");
        b2.Vlq(0);
        b2.Raw({ 0xF0, 0x07, 'P', 'S', 0x00, 0x04, 0xFF, 0x01, 0xF7 });
        b2.Vlq(0);
        b2.Raw({ 0x90, 96, 100 });   // G on @0
        b2.Vlq(10);
        b2.Raw({ 0x80, 96, 0 });     // G off @10
        b2.Vlq(190);
        b2.Raw({ 0xF0, 0x07, 'P', 'S', 0x00, 0x04, 0xFF, 0x00, 0xF7 });  // end @200
        b2.Vlq(0);
        b2.Raw({ 0x90, 97, 100 });   // R on @200
        b2.Vlq(10);
        b2.Raw({ 0x80, 97, 0 });     // R off @210
        b2.EndTrack(t2);
        SmfFile f;
        CHECK(ParseSmf(b2.bytes.data(), b2.bytes.size(), f));
        RawTrack   rt;
        MidOptions opt;
        CHECK(BuildRawTrackFromMid(f, 3, opt, rt));
        CHECK(rt.chords.size() == 2);
        CHECK(rt.chords[0].tap);
        CHECK(rt.chords[1].tap);   // end tick INCLUDED for tap phrases
    }
}

static void RunTests() {
    RunSmfTests();
    RunMidMapperTests();
}

TEST_MAIN("MidParser")
