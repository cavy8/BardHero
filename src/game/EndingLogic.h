// src/game/EndingLogic.h
#pragma once

#include <algorithm>
#include <string>
#include <string_view>

// PURE end-of-performance decision logic (no RE/OS - headless-tested by
// EndingTests).
//
// WHY THIS EXISTS. Skyrim's Got Talent decides the whole ending from the
// player's RANK and a dice roll, and never looks at the performance
// (`_Talent_PlayInstrument.psc:308-471`, `MessageAndEXP`):
//
//   * `_Talent_ReceiveOvation.SetValue(1)` is the FIRST line, unconditional
//     - the crowd cheers for a 0% run exactly as hard as for a perfect one
//   * the reaction message is `RandomInt(1,14)` inside an expertise band, so
//     an Advanced bard draws a positive line 6 times in 14 regardless
//   * gold is `RandomInt(1,10)` for any expertise >= 66 standing in an inn,
//     with no quality term at all
//   * experience is `RandomInt(1,3)` plus speechcraft and rested bonuses
//
// Field-confirmed 2026-07-22: a deliberate 0%-accuracy run (2 of 286 notes)
// still drew cheering animations, a positive notification and gold.
//
// BardHero owns the trigger - the payout is HELD and dispatched by us after
// the results window closes - so the fix is not to fight SGT's function but
// to stop calling it and run these decisions instead. That also removes the
// gold clawback: SGT pays nothing if it is never asked to, so the purse is
// ours alone and `payout::TopUp` grants the whole thing.
namespace SH::ending {

    // SGT appends implementation-facing status tags to otherwise immersive
    // reaction copy. BardHero applies the real effect separately, so expose
    // the authored line without "(Active Debuff)" / "(Active Bonus)".
    [[nodiscard]] inline std::string SanitizeReactionText(
        std::string_view a_text) {
        while (!a_text.empty() &&
               (a_text.back() == ' ' || a_text.back() == '\t' ||
                a_text.back() == '\r' || a_text.back() == '\n')) {
            a_text.remove_suffix(1);
        }
        for (const std::string_view suffix :
             { std::string_view("(Active Debuff)"),
               std::string_view("(Active Bonus)") }) {
            if (a_text.size() >= suffix.size() &&
                a_text.substr(a_text.size() - suffix.size()) == suffix) {
                a_text.remove_suffix(suffix.size());
                while (!a_text.empty() &&
                       (a_text.back() == ' ' || a_text.back() == '\t')) {
                    a_text.remove_suffix(1);
                }
                break;
            }
        }
        return std::string(a_text);
    }

    // A crowd-failed performance must skip SGT stage 20: that stage is the
    // applause scene. Stage 200 is SGT's own terminal/failsafe stage.
    [[nodiscard]] constexpr int AudienceTerminalStageForFailure(
        int a_currentStage) {
        return a_currentStage >= 10 && a_currentStage < 200
                 ? 200
                 : a_currentStage;
    }

    // SGT's ordinary effect teardown enters its stage-20 applause scene
    // independently of BardHero's _Talent_ReceiveOvation value. Only a
    // completed performance that actually finishes in Glory's green zone
    // may use that path.
    [[nodiscard]] constexpr bool ShouldSuppressAudienceApplause(
        bool a_completed, bool a_finishedGreat) {
        return a_completed && !a_finishedGreat;
    }

    // What the room thinks of the run just played.
    enum class Valence { kNegative = 0, kNeutral = 1, kPositive = 2 };

    [[nodiscard]] constexpr const char* FallbackReactionText(
        Valence a_valence) {
        switch (a_valence) {
            case Valence::kNegative:
                return "The jeers leave you shaken.";
            case Valence::kNeutral:
                return "The room weighs your performance in silence.";
            case Valence::kPositive:
                return "The crowd's praise leaves you inspired.";
        }
        return "The room weighs your performance in silence.";
    }

    struct Thresholds {
        // At or above this many stars the room is at least not disappointed.
        // Deliberately the same bar as the purse (`payout::Params::minStars`)
        // so the game has ONE definition of "a good performance" - the
        // player should never see applause for a run that paid nothing, nor
        // the reverse.
        int neutralAt = 2;
        // At or above this, they are actively pleased.
        int positiveAt = 4;
    };

    [[nodiscard]] constexpr Valence ValenceFor(int a_stars,
                                               const Thresholds& a_t) {
        const int s = a_stars < 0 ? 0 : (a_stars > 5 ? 5 : a_stars);
        if (s >= a_t.positiveAt) { return Valence::kPositive; }
        if (s >= a_t.neutralAt) { return Valence::kNeutral; }
        return Valence::kNegative;
    }

    // Whole-song stars remain the primary verdict, but a top reaction also
    // requires the performance to finish with the live Glory meter in
    // green. A severe late fumble therefore caps praise at neutral without
    // rewriting the stars, payout or progression the player earned.
    [[nodiscard]] constexpr Valence ValenceForPerformance(
        int a_stars, bool a_finishedGreat, const Thresholds& a_t) {
        const auto base = ValenceFor(a_stars, a_t);
        return base == Valence::kPositive && !a_finishedGreat
                 ? Valence::kNeutral
                 : base;
    }

    // The ovation global drives SGT's cheer/applause animations. Reserve it
    // for the positive band: a neutral line can be encouraging without the
    // room celebrating as though the performance was excellent.
    [[nodiscard]] constexpr bool ShouldOvate(Valence a_valence) {
        return a_valence == Valence::kPositive;
    }

    // SGT's five expertise bands (`_Talent_PlayInstrument.psc:313-421`).
    enum class Tier {
        kClueless  = 0,  // < 26
        kBeginner  = 1,  // < 46
        kDecent    = 2,  // < 66
        kBardLevel = 3,  // < 86
        kPro       = 4   // >= 86
    };

    [[nodiscard]] constexpr Tier TierFor(int a_expertise) {
        if (a_expertise < 26) { return Tier::kClueless; }
        if (a_expertise < 46) { return Tier::kBeginner; }
        if (a_expertise < 66) { return Tier::kDecent; }
        if (a_expertise < 86) { return Tier::kBardLevel; }
        return Tier::kPro;
    }

    // Experience for the run, replacing SGT's `RandomInt(1,3)`.
    //
    // Two stars or less is not yet a learned performance. Each star above
    // that threshold earns one tier of XP, with an extra flawless bonus.
    // This makes the common 3-star clear worth 1 rather than 3 and prevents
    // short BA songs from advancing one full rank band in a few minutes.
    struct XpParams {
        int perStar = 1;  // per star ABOVE two
        int bonus5  = 1;  // extra for a flawless run
    };

    [[nodiscard]] constexpr int PerformanceXp(int a_stars,
                                              const XpParams& a_p) {
        const int s   = a_stars < 0 ? 0 : (a_stars > 5 ? 5 : a_stars);
        const int per = a_p.perStar < 0 ? 0 : a_p.perStar;
        int       xp  = std::max(s - 2, 0) * per;
        if (s >= 5) { xp += a_p.bonus5 < 0 ? 0 : a_p.bonus5; }
        return xp;
    }

    // ---- reaction message selection --------------------------------------
    //
    // SGT's message properties are grouped by rank tier, and the tiers are
    // NOT symmetric: the three middle tiers ship no positive line, the top
    // two ship no negative one. So a plain tier x valence lookup has holes,
    // and we fall back along a chain that keeps the VALENCE (which is the
    // thing the player is being told) and gives up the tier flavour.
    //
    // ⚠ The `_Talent_Clueless_Broken*` messages are deliberately absent from
    // every pool. Their text says the instrument snapped, and SGT pairs them
    // with `PlayerRef.RemoveItem(Instrument)`. We only Show() a message and
    // never run SGT's side effects, so including them would tell the player
    // their lute broke while it demonstrably had not.
    struct Pool {
        const char* const* names = nullptr;
        int                count = 0;
    };

    namespace detail {
        inline constexpr const char* kCluelessNeg[] = {
            "_Talent_Clueless_Negative"
        };
        inline constexpr const char* kCluelessNeu[] = {
            "_Talent_Clueless_OK",  "_Talent_Clueless_OK1",
            "_Talent_Clueless_OK2", "_Talent_Clueless_OK3",
            "_Talent_Clueless_OK4", "_Talent_Clueless_OK5",
            "_Talent_Clueless_OK6", "_Talent_Clueless_OK7",
            "_Talent_Clueless_OK8"
        };
        inline constexpr const char* kBeginnerNeg[] = {
            "_Talent_Beginner_Negative"
        };
        inline constexpr const char* kBeginnerNeu[] = {
            "_Talent_Beginner_OK", "_Talent_Beginner_OK1"
        };
        inline constexpr const char* kDecentNeu[] = {
            "_Talent_Decent_OK", "_Talent_Decent_OK2", "_Talent_Decent_OK3"
        };
        inline constexpr const char* kBardLevelNeu[] = {
            "_Talent_BardLevel_OK", "_Talent_BardLevel_OK1",
            "_Talent_BardLevel_OK2"
        };
        inline constexpr const char* kBardLevelPos[] = {
            "_Talent_BardLevel_Positive"
        };
        inline constexpr const char* kProNeu[] = {
            "_Talent_Pro_OK", "_Talent_Pro_OK1", "_Talent_Pro_OK2",
            "_Talent_Pro_OK3"
        };
        inline constexpr const char* kProPos[] = { "_Talent_Pro_Positive" };
    }

    // The pool a tier ships for a valence, or an empty pool if it ships none.
    [[nodiscard]] inline Pool PoolFor(Tier a_tier, Valence a_v) {
        using namespace detail;
        switch (a_tier) {
            case Tier::kClueless:
                if (a_v == Valence::kNegative) {
                    return { kCluelessNeg, 1 };
                }
                if (a_v == Valence::kNeutral) { return { kCluelessNeu, 9 }; }
                return {};
            case Tier::kBeginner:
                if (a_v == Valence::kNegative) {
                    return { kBeginnerNeg, 1 };
                }
                if (a_v == Valence::kNeutral) { return { kBeginnerNeu, 2 }; }
                return {};
            case Tier::kDecent:
                if (a_v == Valence::kNeutral) { return { kDecentNeu, 3 }; }
                return {};
            case Tier::kBardLevel:
                if (a_v == Valence::kNeutral) {
                    return { kBardLevelNeu, 3 };
                }
                if (a_v == Valence::kPositive) {
                    return { kBardLevelPos, 1 };
                }
                return {};
            case Tier::kPro:
                if (a_v == Valence::kNeutral) { return { kProNeu, 4 }; }
                if (a_v == Valence::kPositive) { return { kProPos, 1 }; }
                return {};
        }
        return {};
    }

    // The SGT property carrying the reaction EFFECT that goes with the
    // message, or nullptr when the ending carries no effect.
    //
    // ⚠ This exists because leaving it out was a bug. SGT pairs most of its
    // reaction messages with effect-carrier records, and the message TEXT
    // describes the effect. Showing the line without applying it tells the
    // player they were blessed or rattled and then gives them nothing;
    // field-reported 2026-07-22 as "notifications said I got a buff, but
    // nothing in active effects". BardHero applies the record's effects
    // directly; it never gives or consumes an inventory item.
    //
    // Neutral endings carry no effect in SGT either, and that is correct: an
    // unremarkable performance should not move the player's stats.
    [[nodiscard]] inline const char* EffectPropertyFor(Tier a_tier,
                                                       Valence a_v) {
        if (a_v == Valence::kNegative) {
            // SGT hands the harsher of its two debuffs to the bottom tier.
            return a_tier == Tier::kClueless ? "Negative2" : "Negative1";
        }
        if (a_v == Valence::kPositive) {
            // ...and the better blessing to a master.
            return a_tier == Tier::kPro ? "Positive2" : "Positive1";
        }
        return nullptr;
    }

    // Pick a message name for this tier and valence, falling back along the
    // valence's chain when the tier ships nothing.
    //
    // a_roll is any non-negative integer (a random draw); it selects within
    // the chosen pool. Never returns nullptr for a valid valence, so the
    // caller always has a line to show.
    [[nodiscard]] inline const char* MessageFor(Tier a_tier, Valence a_v,
                                                int a_roll) {
        // Own tier first, then toward the tiers that actually ship this
        // valence: negatives live at the bottom, positives at the top.
        static constexpr Tier kNegChain[] = { Tier::kBeginner,
                                              Tier::kClueless };
        static constexpr Tier kPosChain[] = { Tier::kBardLevel, Tier::kPro };

        Pool pool = PoolFor(a_tier, a_v);
        if (pool.count == 0) {
            if (a_v == Valence::kNegative) {
                for (const Tier t : kNegChain) {
                    pool = PoolFor(t, a_v);
                    if (pool.count > 0) { break; }
                }
            } else if (a_v == Valence::kPositive) {
                for (const Tier t : kPosChain) {
                    pool = PoolFor(t, a_v);
                    if (pool.count > 0) { break; }
                }
            }
        }
        if (pool.count == 0 || !pool.names) { return nullptr; }
        const int r = a_roll < 0 ? 0 : a_roll;
        return pool.names[r % pool.count];
    }
}
