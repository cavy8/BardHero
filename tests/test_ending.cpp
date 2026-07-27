#include "harness.h"
#include "game/EndingLogic.h"

#include <cstring>
#include <set>
#include <string>

using namespace SH::ending;

static bool Contains(const char* a_hay, const char* a_needle) {
    return a_hay && std::strstr(a_hay, a_needle) != nullptr;
}

static void RunTests() {
    Thresholds t;  // neutral at 2, positive at 4

    // ---- the bug this file exists for ---------------------------------
    // A 0%-accuracy run by an Advanced or Pro bard drew a POSITIVE line and
    // an ovation, because SGT picks on rank. Both must now be negative and
    // silent, and that must hold at every tier - the top two are the ones
    // that shipped no negative message of their own.
    for (const Tier tier : { Tier::kClueless, Tier::kBeginner, Tier::kDecent,
                             Tier::kBardLevel, Tier::kPro }) {
        const char* m = MessageFor(tier, ValenceFor(0, t), 0);
        CHECK(m != nullptr);
        CHECK(!Contains(m, "Positive"));
        CHECK(Contains(m, "Negative"));
    }
    CHECK(!ShouldOvate(Valence::kNegative));
    CHECK(!ShouldOvate(Valence::kNeutral));
    CHECK(ShouldOvate(Valence::kPositive));

    // ---- valence bands -------------------------------------------------
    CHECK(ValenceFor(0, t) == Valence::kNegative);
    CHECK(ValenceFor(1, t) == Valence::kNegative);
    CHECK(ValenceFor(2, t) == Valence::kNeutral);
    CHECK(ValenceFor(3, t) == Valence::kNeutral);
    CHECK(ValenceFor(4, t) == Valence::kPositive);
    CHECK(ValenceFor(5, t) == Valence::kPositive);
    // out-of-range stars clamp rather than fall through a band
    CHECK(ValenceFor(-3, t) == Valence::kNegative);
    CHECK(ValenceFor(99, t) == Valence::kPositive);
    // the thresholds are honoured, not hardcoded
    Thresholds strict{ 3, 5 };
    CHECK(ValenceFor(2, strict) == Valence::kNegative);
    CHECK(ValenceFor(4, strict) == Valence::kNeutral);
    CHECK(ValenceFor(5, strict) == Valence::kPositive);

    // Stars describe the whole run, but the last Glory state describes how
    // the room actually experienced its ending. A hard late fumble caps an
    // otherwise excellent result at a restrained neutral response. It does
    // not rewrite the earned stars or turn the run into a hostile reaction.
    CHECK(ValenceForPerformance(5, true, t) == Valence::kPositive);
    CHECK(ValenceForPerformance(4, true, t) == Valence::kPositive);
    CHECK(ValenceForPerformance(5, false, t) == Valence::kNeutral);
    CHECK(ValenceForPerformance(4, false, t) == Valence::kNeutral);
    CHECK(ValenceForPerformance(3, false, t) == Valence::kNeutral);
    CHECK(ValenceForPerformance(1, true, t) == Valence::kNegative);

    // ---- tier boundaries (SGT's own bands) -----------------------------
    CHECK(TierFor(0) == Tier::kClueless);
    CHECK(TierFor(25) == Tier::kClueless);
    CHECK(TierFor(26) == Tier::kBeginner);
    CHECK(TierFor(45) == Tier::kBeginner);
    CHECK(TierFor(46) == Tier::kDecent);
    CHECK(TierFor(65) == Tier::kDecent);
    CHECK(TierFor(66) == Tier::kBardLevel);
    CHECK(TierFor(85) == Tier::kBardLevel);
    CHECK(TierFor(86) == Tier::kPro);
    CHECK(TierFor(100) == Tier::kPro);

    // ---- message selection invariants ----------------------------------
    // Walk every tier x valence x a spread of rolls and assert the whole
    // contract at once. A hand-written table is exactly the kind of thing
    // that grows a hole later.
    std::set<std::string> seen;
    for (const Tier tier : { Tier::kClueless, Tier::kBeginner, Tier::kDecent,
                             Tier::kBardLevel, Tier::kPro }) {
        for (const Valence v : { Valence::kNegative, Valence::kNeutral,
                                 Valence::kPositive }) {
            for (int roll = -2; roll < 20; ++roll) {
                const char* m = MessageFor(tier, v, roll);
                // every combination resolves to something showable
                CHECK(m != nullptr);
                if (!m) { continue; }
                seen.insert(m);
                // the instrument-snapped lines are never reachable: we do
                // not run SGT's RemoveItem, so the text would be a lie
                CHECK(!Contains(m, "Broken"));
                // valence is never inverted by a fallback
                if (v == Valence::kNegative) {
                    CHECK(!Contains(m, "Positive"));
                    CHECK(!Contains(m, "_OK"));
                }
                if (v == Valence::kPositive) {
                    CHECK(!Contains(m, "Negative"));
                    CHECK(!Contains(m, "_OK"));
                }
                if (v == Valence::kNeutral) {
                    CHECK(!Contains(m, "Positive"));
                    CHECK(!Contains(m, "Negative"));
                }
            }
        }
    }
    // the pools are actually being drawn from, not collapsed to one line
    CHECK(seen.size() > 10);

    // a tier that ships the valence itself keeps its own flavour
    CHECK(std::string(MessageFor(Tier::kPro, Valence::kPositive, 0)) ==
          "_Talent_Pro_Positive");
    CHECK(Contains(MessageFor(Tier::kDecent, Valence::kNeutral, 0),
                   "Decent"));
    // ...and one that does not falls back without inverting the message
    CHECK(std::string(MessageFor(Tier::kDecent, Valence::kPositive, 0)) ==
          "_Talent_BardLevel_Positive");
    CHECK(std::string(MessageFor(Tier::kPro, Valence::kNegative, 0)) ==
          "_Talent_Beginner_Negative");

    // the roll spreads across a multi-entry pool instead of always picking
    // slot 0 (a `% count` dropped to a plain [0] would pass everything above)
    CHECK(std::string(MessageFor(Tier::kPro, Valence::kNeutral, 0)) !=
          std::string(MessageFor(Tier::kPro, Valence::kNeutral, 1)));
    CHECK(std::string(MessageFor(Tier::kClueless, Valence::kNeutral, 0)) !=
          std::string(MessageFor(Tier::kClueless, Valence::kNeutral, 4)));

    // ---- reaction effects ----------------------------------------------
    // The first cut showed SGT's message but never applied the effect it is
    // paired with, so the player was told they were blessed or rattled and
    // got nothing (field-reported 2026-07-22). Every message that promises
    // an effect must now come with one.
    for (const Tier tier : { Tier::kClueless, Tier::kBeginner, Tier::kDecent,
                             Tier::kBardLevel, Tier::kPro }) {
        const char* neg = EffectPropertyFor(tier, Valence::kNegative);
        const char* pos = EffectPropertyFor(tier, Valence::kPositive);
        const char* neu = EffectPropertyFor(tier, Valence::kNeutral);
        CHECK(neg != nullptr);
        CHECK(pos != nullptr);
        // an unremarkable night moves nothing - SGT carries no potion here
        // either, and handing one out would make "neutral" a reward
        CHECK(neu == nullptr);
        // valence is never inverted
        CHECK(Contains(neg, "Negative"));
        CHECK(Contains(pos, "Positive"));
    }
    // the harsher debuff goes to the bottom tier, the better blessing to a
    // master - matching which effect SGT itself pairs with each band
    CHECK(std::string(EffectPropertyFor(Tier::kClueless, Valence::kNegative)) ==
          "Negative2");
    CHECK(std::string(EffectPropertyFor(Tier::kPro, Valence::kNegative)) ==
          "Negative1");
    CHECK(std::string(EffectPropertyFor(Tier::kPro, Valence::kPositive)) ==
          "Positive2");
    CHECK(std::string(EffectPropertyFor(Tier::kClueless, Valence::kPositive)) ==
          "Positive1");
    // a run that draws a positive MESSAGE must also draw a positive potion:
    // the pairing is the whole point, so assert them together
    for (int s = 0; s <= 5; ++s) {
        const auto  v = ValenceFor(s, t);
        const char* m = MessageFor(Tier::kBardLevel, v, 0);
        const char* q = EffectPropertyFor(Tier::kBardLevel, v);
        CHECK(m != nullptr);
        if (v == Valence::kNeutral) {
            CHECK(q == nullptr);
        } else {
            CHECK(q != nullptr);
            const bool msgPos = Contains(m, "Positive");
            const bool potPos = Contains(q, "Positive");
            CHECK(msgPos == potPos);
        }
    }

    // ---- experience ----------------------------------------------------
    XpParams xp;  // 1 per earned tier above 2 stars, +1 at five
    CHECK(PerformanceXp(0, xp) == 0);   // showing up earns nothing
    CHECK(PerformanceXp(1, xp) == 0);
    CHECK(PerformanceXp(2, xp) == 0);
    CHECK(PerformanceXp(3, xp) == 1);
    CHECK(PerformanceXp(4, xp) == 2);
    CHECK(PerformanceXp(5, xp) == 4);   // 3 earned tiers + flawless bonus
    // monotonic in stars - a bad run can never out-earn a better one
    for (int s = 1; s <= 5; ++s) {
        CHECK(PerformanceXp(s, xp) >= PerformanceXp(s - 1, xp));
    }
    // clamped, and never negative however the params are abused
    CHECK(PerformanceXp(-5, xp) == 0);
    CHECK(PerformanceXp(99, xp) == PerformanceXp(5, xp));
    XpParams bad{ -3, -3 };
    CHECK(PerformanceXp(5, bad) == 0);
    // configurable rate actually scales
    XpParams fast{ 3, 0 };
    CHECK(PerformanceXp(4, fast) == 6);

    // SGT's source messages expose implementation language to the player.
    // BardHero keeps their authored flavor but must remove only the trailing
    // mechanical status tag before presenting the line.
    CHECK(SanitizeReactionText(
              "You now have a terrible headache...(Active Debuff)") ==
          "You now have a terrible headache...");
    CHECK(SanitizeReactionText(
              "That performance was bad. (Active Debuff)") ==
          "That performance was bad.");
    CHECK(SanitizeReactionText(
              "That last performance has left you inspired. (Active Bonus)") ==
          "That last performance has left you inspired.");
    CHECK(SanitizeReactionText("A performance to remember.") ==
          "A performance to remember.");

    // A failed run must bypass SGT's stage-20 applause phase and use SGT's
    // own terminal/failsafe stage. A quest already terminal is left alone.
    CHECK(AudienceTerminalStageForFailure(10) == 200);
    CHECK(AudienceTerminalStageForFailure(20) == 200);
    CHECK(AudienceTerminalStageForFailure(200) == 200);

    // SGT's effect finish enters stage 20 even when BardHero's own ending
    // correctly sets ovation=false. Any completed run that does not finish
    // in green must stop the live audience before that generic applause
    // teardown; aborts have their own lifecycle and strong finishes may use
    // SGT's normal applause path.
    CHECK(!ShouldSuppressAudienceApplause(false, false));
    CHECK(!ShouldSuppressAudienceApplause(true, true));
    CHECK(ShouldSuppressAudienceApplause(true, false));
}

TEST_MAIN("Ending")
