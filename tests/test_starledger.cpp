#include "harness.h"
#include "game/StarLedgerCore.h"

using namespace SH::stars;

// Hand-built legacy buffer pieces (v1-v3 wire layout; best entries have NO
// difficulty byte). The relabel-a-modern-buffer trick that older revisions
// of this suite used is dead: a v4 buffer's entries carry the extra byte,
// so a legacy version label on one misparses by construction.
static void PutU16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
static void PutU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}
static void PutStr(std::vector<std::uint8_t>& b, const std::string& s) {
    PutU16(b, static_cast<std::uint16_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}
// header + one legacy best entry (key, inst, stars - no diff byte)
static std::vector<std::uint8_t> LegacyHeader(std::uint8_t version) {
    std::vector<std::uint8_t> b;
    b.push_back(version);
    PutU16(b, 0);                                    // lastPlayed empty
    b.push_back(0); b.push_back(0); b.push_back(0);  // lifted[3]
    return b;
}

static void RunTests() {
    LedgerData d;

    // record: best-only updates, lastPlayed always updates
    CHECK(RecordResult(d, "SongA", Instrument::kLute, 3, 3));
    CHECK(d.lastPlayed == "SongA");
    CHECK(!RecordResult(d, "SongA", Instrument::kLute, 3, 2));  // not better
    CHECK(BestFor(d, "SongA", Instrument::kLute, 3) == 3);
    CHECK(RecordResult(d, "SongA", Instrument::kLute, 3, 5));
    CHECK(BestFor(d, "SongA", Instrument::kLute, 3) == 5);

    // per-DIFFICULTY isolation (GH convention): the Expert record above
    // says nothing about Hard, and a Hard record neither reads nor
    // overwrites Expert.
    CHECK(BestFor(d, "SongA", Instrument::kLute, 2) == 0);
    CHECK(RecordResult(d, "SongA", Instrument::kLute, 2, 4));
    CHECK(BestFor(d, "SongA", Instrument::kLute, 2) == 4);
    CHECK(BestFor(d, "SongA", Instrument::kLute, 3) == 5);
    CHECK(BestFor(d, "SongA", Instrument::kLute, 0) == 0);

    // per-instrument isolation
    CHECK(BestFor(d, "SongA", Instrument::kDrum, 3) == 0);
    RecordResult(d, "SongA", Instrument::kDrum, 3, 4);
    CHECK(BestFor(d, "SongA", Instrument::kDrum, 3) == 4);
    CHECK(BestFor(d, "SongA", Instrument::kLute, 3) == 5);

    // gate counts stay per DISTINCT CHART: SongA holds lute records on TWO
    // difficulties (5* Expert, 4* Hard) yet counts once - one song
    // 3-starred twice must never open a gate that demands two songs.
    RecordResult(d, "SongB", Instrument::kLute, 3, 3);
    RecordResult(d, "SongC", Instrument::kLute, 3, 2);  // below 3*: no count
    const auto c = CountFor(d, Instrument::kLute);
    CHECK(c.chartsAt3 == 2);   // SongA(max 5), SongB(3)
    CHECK(c.chartsAt5 == 1);   // SongA
    CHECK(CountFor(d, Instrument::kDrum).chartsAt3 == 1);
    CHECK(CountFor(d, Instrument::kDrum).chartsAt5 == 0);
    // ...and the cross-difficulty MAX is what counts: 2* + 2* on two
    // difficulties is still not a 3* chart.
    {
        LedgerData g;
        RecordResult(g, "Low", Instrument::kLute, 0, 2);
        RecordResult(g, "Low", Instrument::kLute, 3, 2);
        CHECK(CountFor(g, Instrument::kLute).chartsAt3 == 0);
    }

    // serialization round-trip (incl. lifted masks + lastPlayed + diffs)
    d.lifted[0] = 0b0011;
    d.lifted[2] = 0b0001;
    const auto  buf = Serialize(d);
    LedgerData  r;
    CHECK(Deserialize(buf.data(), buf.size(), 3, r));
    CHECK(r.lastPlayed == d.lastPlayed);
    CHECK(r.lifted[0] == 0b0011);
    CHECK(r.lifted[1] == 0);
    CHECK(r.lifted[2] == 0b0001);
    CHECK(BestFor(r, "SongA", Instrument::kLute, 3) == 5);
    CHECK(BestFor(r, "SongA", Instrument::kLute, 2) == 4);
    CHECK(BestFor(r, "SongA", Instrument::kDrum, 3) == 4);
    CHECK(BestFor(r, "SongB", Instrument::kLute, 3) == 3);
    CHECK(BestFor(r, "SongC", Instrument::kLute, 3) == 2);

    // corrupt/truncated input never crashes, returns false
    LedgerData bad;
    CHECK(!Deserialize(buf.data(), 3, 3, bad));
    CHECK(!Deserialize(nullptr, 0, 3, bad));
    // wrong version rejected
    auto v = buf;
    v[0] = 0x63;
    CHECK(!Deserialize(v.data(), v.size(), 3, bad));

    // empty ledger round-trips
    LedgerData e0, e1;
    const auto eb = Serialize(e0);
    CHECK(Deserialize(eb.data(), eb.size(), 3, e1));
    CHECK(e1.lastPlayed.empty());

    // malicious count: valid header + count=0xFFFFFFFF, no body -> false
    // fast (bounded reads fail on the first entry, no huge alloc/hang)
    {
        auto m = LegacyHeader(1);
        for (int i = 0; i < 4; ++i) { m.push_back(0xFF); }  // count
        LedgerData out;
        CHECK(!Deserialize(m.data(), m.size(), 3, out));
    }

    // mid-entry truncation + out-of-domain entry bytes. v4 entry tail is
    // (..., inst, diff, stars); the 8-byte suffix is v4's empty taught and
    // NEW counts.
    {
        LedgerData one;
        RecordResult(one, "K", Instrument::kLute, 1, 4);
        const auto        ob   = Serialize(one);
        const std::size_t tail = 8;
        LedgerData        out;
        // count claims 1 entry but the body cuts off inside it
        CHECK(!Deserialize(ob.data(), ob.size() - tail - 1, 3, out));
        // stars out of range (>5) -> corrupt, whole-record reject
        auto s = ob;
        s[s.size() - tail - 1] = 200;
        CHECK(!Deserialize(s.data(), s.size(), 3, out));
        // difficulty out of range (>3) -> reject
        auto q = ob;
        q[q.size() - tail - 2] = 9;
        CHECK(!Deserialize(q.data(), q.size(), 3, out));
        // instrument out of range (>= kInstrumentCount) -> reject
        auto w = ob;
        w[w.size() - tail - 3] = 7;
        CHECK(!Deserialize(w.data(), w.size(), 3, out));

        // a_out untouched on ANY failed Deserialize
        LedgerData keep;
        RecordResult(keep, "Keep", Instrument::kFlute, 2, 4);
        keep.lifted[1] = 0b0101;
        CHECK(!Deserialize(s.data(), s.size(), 3, keep));
        CHECK(!Deserialize(ob.data(), ob.size() - tail - 1, 3, keep));
        CHECK(keep.lastPlayed == "Keep");
        CHECK(BestFor(keep, "Keep", Instrument::kFlute, 2) == 4);
        CHECK(keep.lifted[1] == 0b0101);
    }

    // zero-star play: lastPlayed updates but NO best row materializes
    {
        LedgerData z;
        CHECK(!RecordResult(z, "Zero", Instrument::kDrum, 3, 0));
        CHECK(z.lastPlayed == "Zero");
        CHECK(BestFor(z, "Zero", Instrument::kDrum, 3) == 0);
        LedgerData ref;
        ref.lastPlayed = "Zero";  // same header, zero entries
        CHECK(Serialize(z).size() == Serialize(ref).size());
        // non-improving replay adds no row either
        LedgerData w;
        RecordResult(w, "W", Instrument::kLute, 3, 3);
        const auto sz = Serialize(w).size();
        CHECK(!RecordResult(w, "W", Instrument::kLute, 3, 2));
        CHECK(Serialize(w).size() == sz);
        CHECK(!RecordResult(w, "Z", Instrument::kLute, 3, 0));
        CHECK(w.lastPlayed == "Z");  // same-length key: size delta = rows
        CHECK(Serialize(w).size() == sz);  // 0-star on a NEW key: no row
    }

    {   // v4 round-trips taught charts and NEW tags
        LedgerData d;
        d.taught.insert("TheFatRat - Fire");
        d.taught.insert("Become a Bard - Secunda");
        d.newSongs.insert("TheFatRat - Fire");
        const auto bytes = Serialize(d);
        LedgerData back;
        CHECK(Deserialize(bytes.data(), bytes.size(), 3, back));
        CHECK(back.taught.size() == 2);
        CHECK(back.taught.count("TheFatRat - Fire") == 1);
        CHECK(back.newSongs.size() == 1);
        CHECK(back.newSongs.count("TheFatRat - Fire") == 1);
    }

    {   // LEGACY v1 (hand-built wire bytes, no diff in entries): loads,
        // and every best entry is stamped with the caller's legacy
        // difficulty - visible at that difficulty and no other.
        auto b = LegacyHeader(1);
        PutU32(b, 1);
        PutStr(b, "K");
        b.push_back(1);  // kFlute
        b.push_back(4);  // stars
        LedgerData back;
        CHECK(Deserialize(b.data(), b.size(), 2, back));
        CHECK(back.taught.empty());
        CHECK(back.newSongs.empty());
        CHECK(BestFor(back, "K", Instrument::kFlute, 2) == 4);
        CHECK(BestFor(back, "K", Instrument::kFlute, 3) == 0);
        CHECK(BestFor(back, "K", Instrument::kFlute, 1) == 0);
        // out-of-range legacy difficulty clamps (bad host input, not a
        // corrupt buffer)
        LedgerData clamped;
        CHECK(Deserialize(b.data(), b.size(), -1, clamped));
        CHECK(BestFor(clamped, "K", Instrument::kFlute, 3) == 4);
        // v1 ignores trailing bytes (an old writer never wrote more); a
        // taught-block-shaped tail must not be read as one.
        auto t = b;
        PutU32(t, 1);
        PutStr(t, "Some Chart");
        LedgerData ig;
        CHECK(Deserialize(t.data(), t.size(), 2, ig));
        CHECK(ig.taught.empty());
    }

    {   // LEGACY v3 with a best entry AND taught/NEW blocks: everything
        // lands, entries take the legacy difficulty.
        auto b = LegacyHeader(3);
        PutU32(b, 1);
        PutStr(b, "K");
        b.push_back(0);  // kLute
        b.push_back(5);  // stars
        PutU32(b, 1);
        PutStr(b, "Some Chart");    // taught
        PutU32(b, 1);
        PutStr(b, "Fresh Chart");   // NEW
        LedgerData back;
        CHECK(Deserialize(b.data(), b.size(), 0, back));
        CHECK(BestFor(back, "K", Instrument::kLute, 0) == 5);
        CHECK(BestFor(back, "K", Instrument::kLute, 3) == 0);
        CHECK(back.taught.count("Some Chart") == 1);
        CHECK(back.newSongs.count("Fresh Chart") == 1);
    }

    {   // empty taught/NEW sets write zero counts and replace stale state
        LedgerData d;
        const auto bytes = Serialize(d);
        LedgerData back;
        back.taught.insert("stale");   // must be replaced, not merged into
        back.newSongs.insert("stale-new");
        CHECK(Deserialize(bytes.data(), bytes.size(), 3, back));
        CHECK(back.taught.empty());
        CHECK(back.newSongs.empty());
    }

    {   // truncation inside the taught block: false, a_out untouched.
        // Hand-built v2 (its taught block is the LAST thing in a v2
        // buffer), walking the cut across mid-key-bytes, mid-keyLen and
        // mid-count in turn.
        auto b = LegacyHeader(2);
        PutU32(b, 0);                    // no best entries
        PutU32(b, 1);
        PutStr(b, "LongEnoughKey");      // 13 bytes
        const std::size_t block = 4 + 2 + 13;  // u32 count, u16 len, key
        LedgerData keep;
        keep.lastPlayed = "Keep";
        keep.taught.insert("Kept");
        for (std::size_t cut = 1; cut < block; ++cut) {
            CHECK(!Deserialize(b.data(), b.size() - cut, 3, keep));
        }
        CHECK(keep.lastPlayed == "Keep");
        CHECK(keep.taught.count("Kept") == 1);
        // cutting the block off ENTIRELY is not truncation - it is
        // indistinguishable from a v2 buffer written with nothing taught,
        // and loads as such.
        LedgerData edge;
        CHECK(Deserialize(b.data(), b.size() - block, 3, edge));
        CHECK(edge.taught.empty());
    }

    {   // truncation inside the NEW block is rejected atomically
        LedgerData d;
        d.newSongs.insert("New Song");
        const auto bytes = Serialize(d);
        LedgerData keep;
        keep.newSongs.insert("Keep");
        for (std::size_t cut = 1; cut < 2 + 8 + 4; ++cut) {
            CHECK(!Deserialize(bytes.data(), bytes.size() - cut, 3, keep));
        }
        CHECK(keep.newSongs.count("Keep") == 1);
    }
}

TEST_MAIN("StarLedger")
