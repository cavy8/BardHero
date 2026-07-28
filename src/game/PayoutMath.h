// src/game/PayoutMath.h
#pragma once

// PURE payout maths (no RE/OS includes - headless-tested by PayoutTests).
//
// SGT pays performance gold ONLY at expertise >= 66
// (_Talent_PlayInstrument.psc:389-421), and that branch itself pays only
// inside a LocTypeInn. Ranks 1-3 have no gold branch at all, so a good run
// by an unknown bard earns nothing from it. A street performance is not
// truly unpaid though - Fragment_GiveGoldOutside.psc:9-13 pays a flat
// RandomInt(3,9) busking tip outdoors with no inn check and no expertise
// gate at all. This computes what the performance was actually worth; the
// caller grants only the shortfall.
//
// Renown scales the purse; the performance decides whether there is one.
#include <algorithm>
#include <cmath>

namespace SH::payout {

    struct Params {
        double buskBase        = 20.0;
        double buskOutside     = 0.35;
        double renownAtRank1   = 0.40;
        double moodPayTerrible = 0.0;
        double moodPayMiddling = 0.5;
        double diffMult[4]     = { 0.5, 0.75, 1.0, 1.5 };
        int    cap             = 60;
        // Fewer stars than this and the performance earns nothing at all.
        //
        // A crowd will forgive a rough night; it will not pay for one. 2 is
        // the same bar `ending::Thresholds::neutralAt` uses to decide
        // whether the room applauds, deliberately - the player should never
        // be applauded for a run that paid nothing, nor paid for one that
        // was met with silence. It is NOT the same bar as the song-unlock
        // ledger (3 stars): earning a coin is a lower bar than proving you
        // have a song down well enough to unlock another.
        //
        // The paying range is rescaled onto whatever survives the bar, so
        // raising minStars makes the low end poorer rather than lopping the
        // bottom off the curve and leaving a cliff.
        int minStars = 2;
        // ---- song length -------------------------------------------------
        // A four-minute set is more work than a ninety-second one and should
        // pay for it (field ask 2026-07-22). Linear in length against a
        // reference, then CLAMPED at both ends: linear-forever would let one
        // absurdly long chart mint the cap every time, and unbounded-small
        // would make a 20-second test chart pay effectively nothing even
        // when played perfectly.
        //
        // Defaults: a 2-minute song is the reference and pays 1.0x, a
        // 1-minute song pays 0.5x, and anything from 4 minutes up pays 2.0x.
        double lengthRefSec = 120.0;
        double lengthMin    = 0.5;
        double lengthMax    = 2.0;
        // ---- audience ----------------------------------------------------
        // Nobody around, nobody pays (field 2026-07-28: gold for a flawless
        // set on an empty mountainside). The listener count is a
        // time-weighted average over the whole song, so someone who walks
        // off mid-set stops counting from that point, and someone who
        // wanders in late counts for the part they heard.
        //
        // One listener is not a fraction of a crowd, it is the busking
        // fantasy working as intended - a lone patron tipping a bard - so
        // it pays audiencePayLone of the purse rather than 1/fullAt of it.
        // Linear from there up to audienceFullAt listeners, where the purse
        // is whole. SGT's own inn payout is untouched by all of this; the
        // factor scales only what THIS feature adds.
        double audiencePayLone = 0.40;
        int    audienceFullAt  = 4;
    };

    // Length multiplier for a song of a_songSec. A non-positive reference
    // disables the feature (returns 1.0) rather than dividing by zero.
    [[nodiscard]] inline double LengthMult(double a_songSec,
                                           const Params& p) {
        if (!(p.lengthRefSec > 0.0)) { return 1.0; }
        // A non-finite or negative length is a caller bug, not a free
        // multiplier: fall back to the floor rather than the reference.
        if (!(a_songSec > 0.0)) { return p.lengthMin; }
        const double lo = std::min(p.lengthMin, p.lengthMax);
        const double hi = std::max(p.lengthMin, p.lengthMax);
        return std::clamp(a_songSec / p.lengthRefSec, lo, hi);
    }

    // Which mood the room was in for MOST of the performance, not the one
    // it happened to be in at the final note. The payout gates on this
    // instead of crowd::Mood::Committed(): the model is a 3s rolling window
    // behind a 5s hold, so ~6.5s of continuous end-of-song fumbling flips a
    // committed kGreat to kTerrible, and moodPayTerrible = 0.0 then pays
    // NOTHING for a run the player already earned over three minutes. On a
    // 3-minute song at rank 5, Expert, in an inn, that is an 11.5s-wide
    // window in which 4 stars are kept and 0 gold is paid - verbatim the
    // complaint spec 5.6 exists to remove. The flaw is one-directional:
    // stars scale the purse linearly, so a bad song with a clean last bar
    // can never manufacture an unearned purse, but an earned one can be
    // destroyed.
    //
    // Deliberately NOT a change to crowd::Mood. That model has a second live
    // consumer - MoodGlobals, driving SGT's reaction globals - which
    // genuinely wants the INSTANTANEOUS level; making it representative
    // would break the ambient crowd layer to fix the payout.
    struct MoodTally {
        double sec[3] = { 0.0, 0.0, 0.0 };

        // Out-of-range levels and non-positive intervals are dropped, not
        // clamped: a caller feeding either is buggy, and clamping would
        // silently bias the tally rather than simply not counting.
        // (!(a_sec > 0.0) also catches NaN, which a clamp would not.)
        void Add(int a_level, double a_sec) {
            if (a_level < 0 || a_level > 2 || !(a_sec > 0.0)) { return; }
            sec[a_level] += a_sec;
        }

        int Dominant() const {
            // An empty tally answers kMiddling - the same cold-start level
            // crowd::Mood::Reset() holds - so a song too short to feed, or
            // one played with the mood feed off, never reads as a room that
            // booed and never pays zero for that reason alone.
            int    best    = 1;
            double bestSec = 0.0;
            for (int i = 0; i < 3; ++i) {
                // >= makes ties resolve UPWARD, toward the more generous
                // level. Intentional: the player has already been penalised
                // for the run by `stars`, and this feature exists to pay
                // people, not to find reasons not to.
                if (sec[i] > 0.0 && sec[i] >= bestSec) {
                    best    = i;
                    bestSec = sec[i];
                }
            }
            return best;
        }
    };

    // Pay share for a time-averaged listener count. Piecewise linear:
    // f(0) = 0, f(1) = audiencePayLone, f(fullAt) = 1, clamped above.
    // Fractions below one listener scale the lone share - a patron present
    // for 40% of the song is 40% of a lone patron. Non-finite or negative
    // counts pay nothing: an empty room must never round up to a purse.
    [[nodiscard]] inline double AudienceMult(double a_listeners,
                                             const Params& p) {
        if (!(a_listeners > 0.0)) { return 0.0; }
        const double lone = std::clamp(p.audiencePayLone, 0.0, 1.0);
        const int    full = std::max(p.audienceFullAt, 1);
        if (a_listeners >= static_cast<double>(full)) { return 1.0; }
        if (full == 1) { return a_listeners >= 1.0 ? 1.0 : a_listeners; }
        if (a_listeners <= 1.0) { return lone * a_listeners; }
        return lone + (1.0 - lone) * (a_listeners - 1.0) / (full - 1);
    }

    // moodLevel matches crowd::Level: 0 terrible, 1 middling, 2 great.
    // songSec scales the purse by how long the set actually was.
    inline int Deserved(int stars, int moodLevel, int difficulty, int rank,
                        bool atInn, double songSec, const Params& p) {
        const double mood = moodLevel >= 2   ? 1.0
                          : moodLevel == 1   ? p.moodPayMiddling
                                             : p.moodPayTerrible;
        if (mood <= 0.0) { return 0; }
        const int s = std::clamp(stars, 0, 5);
        const int m = std::clamp(p.minStars, 1, 5);
        if (s < m) { return 0; }
        // s == m pays the bottom of the range, s == 5 pays all of it.
        const double star = static_cast<double>(s - m + 1) / (6 - m);
        const double diff  = p.diffMult[std::clamp(difficulty, 0, 3)];
        const double venue = atInn ? 1.0 : p.buskOutside;
        const int    r     = std::clamp(rank, 1, 5);
        const double renown =
            p.renownAtRank1 + (1.0 - p.renownAtRank1) * ((r - 1) / 4.0);
        const double gold = p.buskBase * mood * star * diff * venue * renown *
                            LengthMult(songSec, p);
        return std::clamp(static_cast<int>(std::lround(gold)), 0, p.cap);
    }

    // Audience-scaled purse. audience is the time-averaged listener count;
    // a caller with no count should pass p.audienceFullAt, which pays the
    // pre-audience figure rather than inventing an empty room.
    inline int Deserved(int stars, int moodLevel, int difficulty, int rank,
                        bool atInn, double songSec, double audience,
                        const Params& p) {
        const double aud = AudienceMult(audience, p);
        if (aud <= 0.0) { return 0; }
        const int base = Deserved(stars, moodLevel, difficulty, rank, atInn,
                                  songSec, p);
        return static_cast<int>(std::lround(base * aud));
    }

    // Back-compat overload: a song exactly at the reference length, so the
    // length multiplier is 1.0 and the purse is the pre-2026-07-22 figure.
    // Every call that actually knows the song's duration should pass it.
    inline int Deserved(int stars, int moodLevel, int difficulty, int rank,
                        bool atInn, const Params& p) {
        return Deserved(stars, moodLevel, difficulty, rank, atInn,
                        p.lengthRefSec, p);
    }

    // Grant only the gap. Never negative: clawback is GoldScale's business
    // under its own settings, and this feature may only ever add.
    inline int TopUp(int deserved, int observed) {
        return std::max(0, deserved - observed);
    }
}
