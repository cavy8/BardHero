// src/game/ResultsLogic.h
#pragma once

// PURE presentation logic for the HUD and the results window (no RE/OS -
// headless-tested by ResultsLogicTests). Spec:
// docs/specs/2026-07-22-performance-ui.md.
//
// Everything here answers a question the player is asking WHILE or JUST
// AFTER playing - "is this run going well", "what did that earn me", "am I
// close to the next rank" - and none of it may need the game to be running
// to be tested. The windows own pixels and colours; this owns the numbers
// and the wording.
#include <algorithm>
#include <string>

#include "game/StarsLogic.h"

namespace SH::results {

    // ---- live accuracy ---------------------------------------------------
    //
    // The denominator is notes whose judgment window has already CLOSED
    // (hit + missed), NOT the whole chart. A whole-chart denominator makes a
    // flawless run read 3% at the first note and climb all song, which tells
    // the player the opposite of the truth at the exact moment they are
    // deciding whether the run is worth finishing.
    //
    // Before the first note resolves there is nothing to average, and the
    // answer is 1.0 rather than 0.0 for the same reason: a run with no
    // mistakes in it has not made any. The number becomes real the instant
    // the first window closes.
    //
    // At song end every window has closed (the session runs to songLen +
    // 0.25s, well past the last note), so hit + missed == the chart's note
    // count and this equals the notesHit/notesTotal figure the results
    // window reports. That equality is acceptance criterion 1 and it holds
    // by construction, not by coincidence - do not "fix" one side alone.
    [[nodiscard]] inline double LiveAccuracy(int a_hit, int a_missed) {
        const int hit    = a_hit < 0 ? 0 : a_hit;
        const int missed = a_missed < 0 ? 0 : a_missed;
        const int seen   = hit + missed;
        if (seen <= 0) { return 1.0; }
        return static_cast<double>(hit) / seen;
    }

    // The star band an accuracy currently sits in. This IS
    // stars::StarsFromAccuracy - deliberately, and not a second set of
    // bands. The HUD colour-grades on it and the results window counts stars
    // with it, so the colour and the final rating cannot disagree
    // (acceptance criterion 3). A caller that wants a different feel must
    // move fStar1..fStar5, which moves both.
    [[nodiscard]] inline int BandFor(double a_acc, const stars::StarParams& p) {
        return stars::StarsFromAccuracy(a_acc, p);
    }

    // Filled/empty pips for a 0..5 star count. Out-of-range clamps rather
    // than indexing off the end - this is called from a Draw() at frame
    // rate and a bad count must not be a crash.
    [[nodiscard]] inline const char* PipString(int a_stars) {
        static constexpr const char* kPips[6] = { "-----", "*----", "**---",
                                                  "***--", "****-", "*****" };
        return kPips[std::clamp(a_stars, 0, 5)];
    }

    // ---- the rank ladder, as the player reads it -------------------------
    //
    // SGT's own five bands, in SGT's own vocabulary (the same table
    // SettingsTool::TierOf renders, so a rank-up line and the settings page
    // can never call the same standing two different things). Rank is
    // 1-based: 1 is 0-25, 2 is 26-45, 3 is 46-65, 4 is 66-85, 5 is 86+.
    [[nodiscard]] inline const char* StandingName(int a_rank) {
        static constexpr const char* kNames[5] = { "Clueless", "Beginner",
                                                   "Decent", "Advanced",
                                                   "Pro" };
        return kNames[std::clamp(a_rank, 1, 5) - 1];
    }

    // How far through the current band an expertise value sits, and what it
    // is climbing toward. The bar exists to make the next rank feel
    // reachable, so the TOP band reports full rather than an empty bar that
    // can never move again.
    struct RankProgress {
        int    rank  = 1;      // 1..5
        int    lo    = 0;      // first expertise value in this band
        int    next  = 26;     // first value of the NEXT band (== lo at max)
        double frac  = 0.0;    // 0..1 through the band
        bool   maxed = false;  // already Pro; there is nothing above
    };

    [[nodiscard]] inline RankProgress ProgressFor(int a_expertise) {
        // The four band edges, as SGT's own branches test them.
        static constexpr int kEdge[4] = { 26, 46, 66, 86 };
        const int            e        = a_expertise < 0 ? 0 : a_expertise;
        RankProgress         r;
        r.rank = 1;
        for (int i = 0; i < 4; ++i) {
            if (e >= kEdge[i]) { r.rank = i + 2; }
        }
        if (r.rank >= 5) {
            r.lo    = kEdge[3];
            r.next  = kEdge[3];
            r.frac  = 1.0;
            r.maxed = true;
            return r;
        }
        r.lo   = r.rank == 1 ? 0 : kEdge[r.rank - 2];
        r.next = kEdge[r.rank - 1];
        const double span = static_cast<double>(r.next - r.lo);
        r.frac = span > 0.0 ? std::clamp((e - r.lo) / span, 0.0, 1.0) : 0.0;
        return r;
    }

    // The Results snapshot carries the frozen session instrument as its
    // serialization code (StarsLogic.h). Keep this helper integer-based so
    // UiBus stays a tiny render mailbox and malformed snapshots fail open to
    // the original/default lute presentation.
    [[nodiscard]] inline const char* ProficiencyHeading(int a_instrument) {
        switch (a_instrument) {
            case 1: return "FLUTE PROFICIENCY";
            case 2: return "DRUM PROFICIENCY";
            default: return "LUTE PROFICIENCY";
        }
    }

    // At the top of the ladder there is no next band to buy. Results uses a
    // single celebratory MAX label and suppresses the otherwise misleading
    // +XP count-up.
    [[nodiscard]] inline const char* MaxStandingLabel() { return "MAX"; }
    [[nodiscard]] inline bool ShowXpGain(int a_expertise) {
        return !ProgressFor(a_expertise).maxed;
    }

    // ---- why the purse was what it was -----------------------------------
    //
    // The purse is driven by stars, mood, difficulty, venue, renown AND song
    // length (payout::Deserved). A player shown only a number cannot tell
    // which of those six levers to pull, so the results window names the
    // ones that actually moved it. Deliberately at most three clauses: this
    // is one line under a score, not a receipt.
    //
    // Difficulty and renown are left out on purpose. Both are standing
    // facts about the player rather than things about the run just played,
    // and a line that says "on Expert, as a well-known bard" every single
    // time stops being read after the second song.
    struct GoldFacts {
        int    gold       = 0;
        int    stars      = 0;
        int    minStars   = 2;    // payout::Params::minStars
        int    moodLevel  = 1;    // crowd::Level: 0 terrible/1 middling/2 great
        double lengthMult = 1.0;  // payout::LengthMult
        bool   atInn      = true;
        double audienceMult = 1.0;  // payout::AudienceMult
    };

    [[nodiscard]] inline std::string GoldReason(const GoldFacts& f) {
        const int s = std::clamp(f.stars, 0, 5);
        const int m = std::clamp(f.minStars, 1, 5);

        // A zero purse has exactly three causes and the player deserves to
        // know WHICH - "nobody heard it", "you were not good enough" and
        // "the room hated it" call for completely different next attempts.
        // The empty room is tested FIRST: with no listeners the mood level
        // is the model talking to itself, and "you lost the room" would
        // blame the player for a room that never existed.
        if (f.gold <= 0) {
            if (f.audienceMult <= 0.0) {
                return "No gold - nobody was around to hear it.";
            }
            if (f.moodLevel <= 0) {
                return "No gold - you lost the room before the end.";
            }
            if (s < m) {
                return "No gold - a crowd will forgive a rough night, but "
                       "it will not pay for one.";
            }
            // Everything else that can round to nothing: a busked half-star
            // set by an unknown bard on Easy. Say so plainly rather than
            // inventing a cause.
            return "No gold - not enough of a set to pay for.";
        }

        std::string out = std::to_string(f.gold) + " gold - ";
        bool        first = true;
        const auto  add   = [&out, &first](const char* clause) {
            if (!first) { out += ", "; }
            out += clause;
            first = false;
        };

        // length: only when it actually swung the purse
        if (f.lengthMult >= 1.5) {
            add("a long set");
        } else if (f.lengthMult <= 0.7) {
            add("a short set");
        }
        // quality
        if (s >= 5) {
            add("played flawlessly");
        } else if (s == 4) {
            add("well played");
        } else if (s == 3) {
            add("solidly played");
        } else {
            add("scraped through");
        }
        // room and venue
        if (!f.atInn) {
            add("out on the street");
        } else if (f.moodLevel >= 2) {
            add("to a warm room");
        } else {
            add("to a polite room");
        }
        // a thin crowd that shrank the purse is worth naming; a full one is
        // the expected case and stays silent like a 1.0 length multiplier
        if (f.audienceMult > 0.0 && f.audienceMult <= 0.5) {
            add("for a handful of listeners");
        }
        out += ".";
        return out;
    }

    // Results has only half a panel for the payout explanation. Preserve the
    // actual reasons, but phrase them as short complete thoughts so the UI
    // never needs an ellipsis that looks like clipped text.
    [[nodiscard]] inline std::string CompactGoldReason(
        const std::string& reason) {
        const auto has = [&reason](const char* text) {
            return reason.find(text) != std::string::npos;
        };
        if (has("nobody was around")) {
            return "Nobody was around to hear it.";
        }
        if (has("lost the room")) {
            return "The room was lost before the end.";
        }
        if (has("will not pay")) {
            return "Too rough to earn a purse.";
        }
        if (has("not enough of a set")) {
            return "Too brief to earn a purse.";
        }

        std::string out;
        const auto add = [&out](const char* text) {
            if (!out.empty()) { out += ". "; }
            out += text;
        };
        if (has("a long set")) add("Long set");
        if (has("a short set")) add("Short set");
        if (has("played flawlessly")) add("Flawless");
        else if (has("well played")) add("Well played");
        else if (has("solidly played")) add("Solid play");
        else if (has("scraped through")) add("Rough play");
        if (has("out on the street")) add("Street crowd");
        else if (has("to a warm room")) add("Warm room");
        else if (has("to a polite room")) add("Polite room");
        if (out.empty()) return "Performance complete.";
        out += ".";
        return out;
    }
}
