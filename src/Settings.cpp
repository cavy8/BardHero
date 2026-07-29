#include "PCH.h"
#include "Settings.h"
#include "util/PathText.h"

#include <SimpleIni.h>
#include <ShlObj.h>

#include <algorithm>  // std::clamp on the payout INI values
#include <filesystem>
#include <string_view>

namespace SH {
    namespace {
        namespace fs = std::filesystem;
        constexpr const char* kIniPath = "Data/SKSE/Plugins/BardHero.ini";
        constexpr std::string_view kDocumentsToken = "{Documents}";
        // Under My Games rather than loose in Documents (changed 2026-07-27):
        // it sits beside Skyrim's own per-user data, the INIs and the saves,
        // which is where somebody actually looks for a Skyrim folder - and it
        // keeps Documents from collecting one more top-level directory.
        //
        // Still resolved from FOLDERID_Documents, so OneDrive redirection is
        // handled. An EXISTING install keeps whatever its INI already says,
        // so nobody loses a library to this; only fresh installs land here.
        constexpr std::string_view kDefaultUserSongs =
            "{Documents}/My Games/Skyrim Special Edition/Bard Hero Songs";

        fs::path ResolveSongsPath(std::string_view configured) {
            if (!configured.starts_with(kDocumentsToken)) {
                return path_text::FromUtf8(configured);
            }

            PWSTR documents = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents,
                                            KF_FLAG_DEFAULT, nullptr,
                                            &documents))) {
                return path_text::FromUtf8("Bard Hero Songs");
            }
            fs::path resolved(documents);
            CoTaskMemFree(documents);

            auto suffix = configured.substr(kDocumentsToken.size());
            while (!suffix.empty() &&
                   (suffix.front() == '/' || suffix.front() == '\\')) {
                suffix.remove_prefix(1);
            }
            if (!suffix.empty()) resolved /= path_text::FromUtf8(suffix);
            return resolved;
        }
    }

    Settings& Settings::GetSingleton() {
        static Settings instance;
        return instance;
    }

    void Settings::Load() {
        userSongsFolder = ResolveSongsPath(kDefaultUserSongs);
        CSimpleIniA ini;
        ini.SetUnicode();
        const auto rc = ini.LoadFile(kIniPath);
        if (rc < 0) {
            spdlog::warn("Settings: {} not found, using defaults.", kIniPath);
            return;
        }
        verboseLog        = ini.GetBoolValue("General", "bVerboseLog", verboseLog);
        spike2Audio       = ini.GetBoolValue("Spikes", "bSpike2Audio", spike2Audio);
        spike3Render      = ini.GetBoolValue("Spikes", "bSpike3Render", spike3Render);
        spike2DurationSec = static_cast<int>(
            ini.GetLongValue("Spikes", "iSpike2DurationSec", spike2DurationSec));
        debugStartKey = static_cast<int>(
            ini.GetLongValue("Session", "iDebugStartKey", debugStartKey));
        debugAbortKey = static_cast<int>(
            ini.GetLongValue("Session", "iDebugAbortKey", debugAbortKey));
        debugPracticeKey = static_cast<int>(ini.GetLongValue(
            "Session", "iDebugPracticeKey", debugPracticeKey));
        practiceSpeedDownKey = static_cast<int>(ini.GetLongValue(
            "Session", "iPracticeSpeedDownKey", practiceSpeedDownKey));
        practiceSpeedUpKey = static_cast<int>(ini.GetLongValue(
            "Session", "iPracticeSpeedUpKey", practiceSpeedUpKey));
        songsFolder = ini.GetValue("Session", "sSongsFolder", songsFolder.c_str());
        userSongsFolder = ResolveSongsPath(
            ini.GetValue("Session", "sUserSongsFolder",
                         kDefaultUserSongs.data()));
        autoRescanUserSongs = ini.GetBoolValue(
            "Session", "bAutoRescanUserSongs", autoRescanUserSongs);
        hideMenusDuringSession =
            ini.GetValue("Session", "sHideMenusDuringSession",
                         hideMenusDuringSession.c_str());
        difficulty  = static_cast<int>(
            ini.GetLongValue("Session", "iDifficulty", difficulty));
        performSpell =
            ini.GetValue("Session", "sPerformSpell", performSpell.c_str());
        // sPerformSpellLute wins over the legacy sPerformSpell alias
        performSpell = ini.GetValue("Session", "sPerformSpellLute",
                                    performSpell.c_str());
        performSpellFlute = ini.GetValue("Session", "sPerformSpellFlute",
                                         performSpellFlute.c_str());
        performSpellDrum = ini.GetValue("Session", "sPerformSpellDrum",
                                        performSpellDrum.c_str());
        performSpellGuitar = ini.GetValue("Session", "sPerformSpellGuitar",
                                          performSpellGuitar.c_str());
        handleLute   = ini.GetBoolValue("Session", "bHandleLute", handleLute);
        handleFlute  = ini.GetBoolValue("Session", "bHandleFlute", handleFlute);
        handleDrum   = ini.GetBoolValue("Session", "bHandleDrum", handleDrum);
        handleGuitar = ini.GetBoolValue("Session", "bHandleGuitar",
                                        handleGuitar);
        fret1Key  = static_cast<int>(ini.GetLongValue("Input", "iFret1Key", fret1Key));
        fret2Key  = static_cast<int>(ini.GetLongValue("Input", "iFret2Key", fret2Key));
        fret3Key  = static_cast<int>(ini.GetLongValue("Input", "iFret3Key", fret3Key));
        fret4Key  = static_cast<int>(ini.GetLongValue("Input", "iFret4Key", fret4Key));
        fret5Key  = static_cast<int>(ini.GetLongValue("Input", "iFret5Key", fret5Key));
        strumKey  = static_cast<int>(ini.GetLongValue("Input", "iStrumKey", strumKey));
        spKey     = static_cast<int>(ini.GetLongValue("Input", "iStarPowerKey", spKey));
        whammyKey = static_cast<int>(ini.GetLongValue("Input", "iWhammyKey", whammyKey));
        pauseKey  = static_cast<int>(ini.GetLongValue("Input", "iPauseKey", pauseKey));
        fret1Key2 = static_cast<int>(ini.GetLongValue("Input", "iFret1Key2", fret1Key2));
        fret2Key2 = static_cast<int>(ini.GetLongValue("Input", "iFret2Key2", fret2Key2));
        fret3Key2 = static_cast<int>(ini.GetLongValue("Input", "iFret3Key2", fret3Key2));
        fret4Key2 = static_cast<int>(ini.GetLongValue("Input", "iFret4Key2", fret4Key2));
        fret5Key2 = static_cast<int>(ini.GetLongValue("Input", "iFret5Key2", fret5Key2));
        strumKey2 = static_cast<int>(ini.GetLongValue("Input", "iStrumKey2", strumKey2));
        strumKey3 = static_cast<int>(ini.GetLongValue("Input", "iStrumKey3", strumKey3));
        spKey2    = static_cast<int>(ini.GetLongValue("Input", "iStarPowerKey2", spKey2));
        whammyKey2 = static_cast<int>(ini.GetLongValue("Input", "iWhammyKey2", whammyKey2));
        pauseKey2  = static_cast<int>(ini.GetLongValue("Input", "iPauseKey2", pauseKey2));
        controllerEnabled = ini.GetBoolValue(
            "Input", "bControllerEnabled", controllerEnabled);
        gamepadMode =
            ini.GetBoolValue("Input", "bGamepadMode", gamepadMode);
        fretsOnly = ini.GetBoolValue("Input", "bFretsOnly", fretsOnly);
        gamepadFret1 = static_cast<int>(ini.GetLongValue(
            "Input", "iGamepadFret1", gamepadFret1));
        gamepadFret2 = static_cast<int>(ini.GetLongValue(
            "Input", "iGamepadFret2", gamepadFret2));
        gamepadFret3 = static_cast<int>(ini.GetLongValue(
            "Input", "iGamepadFret3", gamepadFret3));
        gamepadFret4 = static_cast<int>(ini.GetLongValue(
            "Input", "iGamepadFret4", gamepadFret4));
        gamepadFret5 = static_cast<int>(ini.GetLongValue(
            "Input", "iGamepadFret5", gamepadFret5));
        gamepadStrum1 = static_cast<int>(ini.GetLongValue(
            "Input", "iGamepadStrum1", gamepadStrum1));
        gamepadStrum2 = static_cast<int>(ini.GetLongValue(
            "Input", "iGamepadStrum2", gamepadStrum2));
        gamepadSp = static_cast<int>(ini.GetLongValue(
            "Input", "iGamepadStarPower", gamepadSp));
        gamepadWhammy = static_cast<int>(ini.GetLongValue(
            "Input", "iGamepadWhammy", gamepadWhammy));
        gamepadPause = static_cast<int>(ini.GetLongValue(
            "Input", "iGamepadPause", gamepadPause));
        gamepadConfirm = static_cast<int>(ini.GetLongValue(
            "Input", "iGamepadConfirm", gamepadConfirm));
        gamepadCancel = static_cast<int>(ini.GetLongValue(
            "Input", "iGamepadCancel", gamepadCancel));
        hookNeverFilter =
            ini.GetBoolValue("Input", "bHookNeverFilter", hookNeverFilter);
        hookMode = static_cast<int>(
            ini.GetLongValue("Input", "iHookMode", hookMode));
        liveCrowdMood =
            ini.GetBoolValue("SGT", "bLiveCrowdMood", liveCrowdMood);
        moodWindowSec =
            ini.GetDoubleValue("SGT", "fMoodWindowSec", moodWindowSec);
        moodGreatAt = ini.GetDoubleValue("SGT", "fMoodGreatAt", moodGreatAt);
        moodTerribleBelow = ini.GetDoubleValue("SGT", "fMoodTerribleBelow",
                                               moodTerribleBelow);
        moodHoldSec  = ini.GetDoubleValue("SGT", "fMoodHoldSec", moodHoldSec);
        moodStartSec = ini.GetDoubleValue("SGT", "fMoodStartSec", moodStartSec);
        tuning.hitWindowScale = ini.GetDoubleValue(
            "Difficulty", "fHitWindowScale", tuning.hitWindowScale);
        tuning.strumLeniencySec = ini.GetDoubleValue(
            "Difficulty", "fStrumLeniencySec", tuning.strumLeniencySec);
        tuning.earlyStrumLeniencySec = ini.GetDoubleValue(
            "Difficulty", "fEarlyStrumLeniencySec",
            tuning.earlyStrumLeniencySec);
        tuning.hopoLeniencySec = ini.GetDoubleValue(
            "Difficulty", "fHopoLeniencySec", tuning.hopoLeniencySec);
        tuning.sustainDropLeniencySec = ini.GetDoubleValue(
            "Difficulty", "fSustainDropLeniencySec",
            tuning.sustainDropLeniencySec);
        tuning.infiniteFrontEnd = ini.GetBoolValue(
            "Difficulty", "bInfiniteFrontEnd", tuning.infiniteFrontEnd);
        tuning.antiGhosting = ini.GetBoolValue(
            "Difficulty", "bAntiGhosting", tuning.antiGhosting);
        tuning.maxMultiplier = static_cast<int>(ini.GetLongValue(
            "Difficulty", "iMaxMultiplier", tuning.maxMultiplier));
        tuning.gloryMeterSpanHits = ini.GetDoubleValue(
            "Difficulty", "fGloryMeterSpanHits",
            tuning.gloryMeterSpanHits);
        tuning.gloryBadWeight = ini.GetDoubleValue(
            "Difficulty", "fGloryBadWeight", tuning.gloryBadWeight);
        tuning.gloryStarPowerHitScale = ini.GetDoubleValue(
            "Difficulty", "fGloryStarPowerHitScale",
            tuning.gloryStarPowerHitScale);
        tuning.gloryStarPowerBadScale = ini.GetDoubleValue(
            "Difficulty", "fGloryStarPowerBadScale",
            tuning.gloryStarPowerBadScale);
        tuning.gloryOpeningSec = ini.GetDoubleValue(
            "Difficulty", "fGloryOpeningSec", tuning.gloryOpeningSec);
        tuning.gloryOpeningBadScale = ini.GetDoubleValue(
            "Difficulty", "fGloryOpeningBadScale",
            tuning.gloryOpeningBadScale);
        tuning.gloryRecoveryHits = static_cast<int>(ini.GetLongValue(
            "Difficulty", "iGloryRecoveryHits",
            tuning.gloryRecoveryHits));
        tuning.gloryRedBelow = ini.GetDoubleValue(
            "Difficulty", "fGloryRedBelow", tuning.gloryRedBelow);
        tuning.gloryGreenAt = ini.GetDoubleValue(
            "Difficulty", "fGloryGreenAt", tuning.gloryGreenAt);
        tuning.failureDangerBelow = ini.GetDoubleValue(
            "Difficulty", "fFailureDangerBelow",
            tuning.failureDangerBelow);
        tuning.failureRecoverAt = ini.GetDoubleValue(
            "Difficulty", "fFailureRecoverAt", tuning.failureRecoverAt);
        tuning.failureGraceSec = ini.GetDoubleValue(
            "Difficulty", "fFailureGraceSec", tuning.failureGraceSec);
        tuning.failureStartSec = ini.GetDoubleValue(
            "Difficulty", "fFailureStartSec", tuning.failureStartSec);
        tuning.failureFurtherBad = static_cast<int>(ini.GetLongValue(
            "Difficulty", "iFailureFurtherBad",
            tuning.failureFurtherBad));
        tuning.audienceCommentDelaySec = ini.GetDoubleValue(
            "Difficulty", "fAudienceCommentDelaySec",
            tuning.audienceCommentDelaySec);
        difficulty::Normalize(tuning);
        driveAudienceStage = ini.GetBoolValue("SGT", "bDriveAudienceStage",
                                              driveAudienceStage);
        crowdReactions =
            ini.GetBoolValue("SGT", "bCrowdReactions", crowdReactions);
        cheerEveryNotes = static_cast<int>(
            ini.GetLongValue("SGT", "iCheerEveryNotes", cheerEveryNotes));
        streakBreakNotes = static_cast<int>(
            ini.GetLongValue("SGT", "iStreakBreakNotes", streakBreakNotes));
        reactionCooldownSec = ini.GetDoubleValue(
            "SGT", "fReactionCooldownSec", reactionCooldownSec);
        crowdVolume = ini.GetDoubleValue("Audio", "fCrowdVolume", crowdVolume);
        performancePayout =
            ini.GetBoolValue("Gold", "bPerformancePayout", performancePayout);
        buskBase    = ini.GetDoubleValue("Gold", "fBuskBase", buskBase);
        buskOutside = ini.GetDoubleValue("Gold", "fBuskOutside", buskOutside);
        renownAtRank1 =
            ini.GetDoubleValue("Gold", "fRenownAtRank1", renownAtRank1);
        moodPayTerrible =
            ini.GetDoubleValue("Gold", "fMoodPayTerrible", moodPayTerrible);
        payoutCap = static_cast<int>(
            ini.GetLongValue("Gold", "iPayoutCap", payoutCap));
        payoutMinStars = static_cast<int>(
            ini.GetLongValue("Gold", "iPayoutMinStars", payoutMinStars));
        ownEnding = ini.GetBoolValue("SGT", "bOwnEnding", ownEnding);
        duetPassthrough = ini.GetBoolValue("SGT", "bDuetPassthrough",
                                           duetPassthrough);
        reactionNeutralStars  = static_cast<int>(ini.GetLongValue(
            "SGT", "iReactionNeutralStars", reactionNeutralStars));
        reactionPositiveStars = static_cast<int>(ini.GetLongValue(
            "SGT", "iReactionPositiveStars", reactionPositiveStars));
        xpPerStar =
            static_cast<int>(ini.GetLongValue("SGT", "iXpPerStar", xpPerStar));
        xpBonus5 =
            static_cast<int>(ini.GetLongValue("SGT", "iXpBonus5", xpBonus5));
        alwaysGatherCrowd =
            ini.GetBoolValue("SGT", "bAlwaysGatherCrowd", alwaysGatherCrowd);
        // Read the old key as a compatibility fallback, then let the clearer
        // effect-based name win when both exist.
        reactionEffects =
            ini.GetBoolValue("SGT", "bReactionPotions", reactionEffects);
        reactionEffects =
            ini.GetBoolValue("SGT", "bReactionEffects", reactionEffects);
        payoutLengthRefSec = ini.GetDoubleValue(
            "Gold", "fPayoutLengthRefSec", payoutLengthRefSec);
        payoutLengthMin =
            ini.GetDoubleValue("Gold", "fPayoutLengthMin", payoutLengthMin);
        payoutLengthMax =
            ini.GetDoubleValue("Gold", "fPayoutLengthMax", payoutLengthMax);
        // INI-only tuning, read here and deliberately never written by the
        // save path (iFret*Key2 precedent): these have no settings-tool UI,
        // so writing them back would only pin today's defaults against a
        // future rebalance.
        audienceRadius = ini.GetDoubleValue(
            "Gold", "fAudienceRadius", audienceRadius);
        audiencePayLone = ini.GetDoubleValue(
            "Gold", "fAudiencePayLone", audiencePayLone);
        audienceFullAt = static_cast<int>(ini.GetLongValue(
            "Gold", "iAudienceFullAt", audienceFullAt));
        // Bound what the INI can put into the payout formula - this is where
        // untrusted input enters. payout::Deserved rounds with std::lround
        // BEFORE clamping to [0, cap], so a buskBase above about 1.43e9
        // overflows long and the function returns 0 instead of the cap: it
        // fails CLOSED rather than minting gold, but a nonsense entry that
        // silently pays nothing is still a bug, and it is cheaper to reject
        // here than to reason about it there. The multipliers are bounded on
        // their own terms too: a rank-1 renown above 1.0 inverts the curve it
        // is supposed to interpolate up from, and a room you failed to win
        // paying more than one you won is not a setting, it is a typo.
        buskBase        = std::clamp(buskBase, 0.0, 10000.0);
        buskOutside     = std::clamp(buskOutside, 0.0, 10.0);
        renownAtRank1   = std::clamp(renownAtRank1, 0.0, 1.0);
        moodPayTerrible = std::clamp(moodPayTerrible, 0.0, 1.0);
        payoutCap       = std::max(0, payoutCap);
        // radius 0 disables counting; the ceiling is a loaded-cell scale,
        // past which "in earshot" stops meaning anything
        audienceRadius  = std::clamp(audienceRadius, 0.0, 16384.0);
        audiencePayLone = std::clamp(audiencePayLone, 0.0, 1.0);
        audienceFullAt  = std::clamp(audienceFullAt, 1, 100);
        // The star bars are array/curve inputs, so bound them here rather
        // than trusting every consumer to clamp. A positive bar below the
        // neutral one would make a good run read as a bad one.
        payoutMinStars        = std::clamp(payoutMinStars, 1, 5);
        reactionNeutralStars  = std::clamp(reactionNeutralStars, 0, 5);
        reactionPositiveStars = std::clamp(reactionPositiveStars,
                                           reactionNeutralStars, 5);
        xpPerStar             = std::clamp(xpPerStar, 0, 20);
        xpBonus5              = std::clamp(xpBonus5, 0, 20);
        // A non-positive reference disables the length factor by contract
        // (payout::LengthMult), so let 0 through but reject nonsense above.
        payoutLengthRefSec = std::clamp(payoutLengthRefSec, 0.0, 3600.0);
        payoutLengthMin    = std::clamp(payoutLengthMin, 0.0, 10.0);
        payoutLengthMax    = std::clamp(payoutLengthMax, 0.0, 10.0);
        // the old boolean still wins if someone has it set
        if (hookNeverFilter) { hookMode = 1; }
        debugProbeKey =
            static_cast<int>(ini.GetLongValue("Input", "iProbeKey", debugProbeKey));
        pauseWorld = ini.GetBoolValue("Session", "bPauseWorld", pauseWorld);
        performanceVanityCamera = ini.GetBoolValue(
            "Session", "bPerformanceVanityCamera",
            performanceVanityCamera);
        performanceCameraDirector = ini.GetBoolValue(
            "Session", "bPerformanceCameraDirector",
            performanceCameraDirector);
        streakFireHandsAt = static_cast<int>(ini.GetLongValue(
            "Highway", "iStreakFireHandsAt", streakFireHandsAt));
        streakFireBlazeAt = static_cast<int>(ini.GetLongValue(
            "Highway", "iStreakFireBlazeAt", streakFireBlazeAt));
        // A tier at or below zero is OFF, and a blaze below the hands tier
        // would mean the body lights before the hands - clamp rather than
        // trust, because a hand-edited INI is the normal case here.
        if (streakFireHandsAt < 0) { streakFireHandsAt = 0; }
        if (streakFireBlazeAt < 0) { streakFireBlazeAt = 0; }
        if (streakFireBlazeAt > 0 && streakFireHandsAt > 0 &&
            streakFireBlazeAt < streakFireHandsAt) {
            streakFireBlazeAt = streakFireHandsAt;
        }
        highwayLookaheadSec = ini.GetDoubleValue(
            "Highway", "fLookaheadSec", highwayLookaheadSec);
        richFx = ini.GetBoolValue("Highway", "bRichFx", richFx);
        duckCurrentMusic =
            ini.GetBoolValue("Audio", "bDuckCurrentMusic", duckCurrentMusic);
        duckAmbience = ini.GetBoolValue("Audio", "bDuckAmbience", duckAmbience);
        worldAudio   = ini.GetBoolValue("Audio", "bWorldAudio", worldAudio);
        songVolume   = static_cast<float>(
            ini.GetDoubleValue("Audio", "fSongVolume", songVolume));
        missMutesGuitar =
            ini.GetBoolValue("Audio", "bMissMutesGuitar", missMutesGuitar);
        missSfx = ini.GetBoolValue("Audio", "bMissSfx", missSfx);
        missSfxVolume =
            ini.GetDoubleValue("Audio", "fMissSfxVolume", missSfxVolume);
        uiSfx       = ini.GetBoolValue("Audio", "bUiSfx", uiSfx);
        uiSfxVolume =
            ini.GetDoubleValue("Audio", "fUiSfxVolume", uiSfxVolume);
        spFilter       = ini.GetBoolValue("Audio", "bSpFilter", spFilter);
        spFilterRateHz = ini.GetDoubleValue("Audio", "fSpFilterRateHz",
                                            spFilterRateHz);
        spFilterBaseMs = ini.GetDoubleValue("Audio", "fSpFilterBaseMs",
                                            spFilterBaseMs);
        spFilterDepthMs = ini.GetDoubleValue("Audio", "fSpFilterDepthMs",
                                             spFilterDepthMs);
        spFilterFeedback = ini.GetDoubleValue("Audio", "fSpFilterFeedback",
                                              spFilterFeedback);
        spFilterWet = ini.GetDoubleValue("Audio", "fSpFilterWet",
                                         spFilterWet);
        spFilterSongStem = ini.GetBoolValue("Audio", "bSpFilterSongStem",
                                            spFilterSongStem);
        blockMovement =
            ini.GetBoolValue("Session", "bBlockMovement", blockMovement);
        goldScale  = ini.GetBoolValue("Gold", "bGoldScale", goldScale);
        goldAccMin = ini.GetDoubleValue("Gold", "fGoldAccMin", goldAccMin);
        goldAccMax = ini.GetDoubleValue("Gold", "fGoldAccMax", goldAccMax);
        goldMultEasy =
            ini.GetDoubleValue("Gold", "fGoldMultEasy", goldMultEasy);
        goldMultMedium =
            ini.GetDoubleValue("Gold", "fGoldMultMedium", goldMultMedium);
        goldMultHard =
            ini.GetDoubleValue("Gold", "fGoldMultHard", goldMultHard);
        goldMultExpert =
            ini.GetDoubleValue("Gold", "fGoldMultExpert", goldMultExpert);
        wholeSongPerform =
            ini.GetBoolValue("SGT", "bWholeSongPerform", wholeSongPerform);
        blankReactionMessages = ini.GetBoolValue(
            "SGT", "bBlankReactionMessages", blankReactionMessages);
        standalonePerform =
            ini.GetBoolValue("SGT", "bStandalonePerform", standalonePerform);
        sgtIdleKeepAlive =
            ini.GetBoolValue("SGT", "bIdleKeepAlive", sgtIdleKeepAlive);
        sgtEndCaptureSec =
            ini.GetDoubleValue("SGT", "fEndCaptureSec", sgtEndCaptureSec);
        star1 = ini.GetDoubleValue("Stars", "fStar1", star1);
        star2 = ini.GetDoubleValue("Stars", "fStar2", star2);
        star3 = ini.GetDoubleValue("Stars", "fStar3", star3);
        star4 = ini.GetDoubleValue("Stars", "fStar4", star4);
        star5 = ini.GetDoubleValue("Stars", "fStar5", star5);
        rankGate = ini.GetBoolValue("Stars", "bRankGate", rankGate);
        gateSongs2 = static_cast<int>(
            ini.GetLongValue("Stars", "iGateSongs2", gateSongs2));
        gateSongs3 = static_cast<int>(
            ini.GetLongValue("Stars", "iGateSongs3", gateSongs3));
        gateSongs4 = static_cast<int>(
            ini.GetLongValue("Stars", "iGateSongs4", gateSongs4));
        gateSongs5 = static_cast<int>(
            ini.GetLongValue("Stars", "iGateSongs5", gateSongs5));
        gate5NeedsFiveStar = ini.GetBoolValue("Stars", "bGate5NeedsFiveStar",
                                              gate5NeedsFiveStar);
        xpFeed     = ini.GetBoolValue("SGT", "bXpFeed", xpFeed);
        xpFeedBase = ini.GetDoubleValue("SGT", "fXpFeedBase", xpFeedBase);
        xpFeedMinAccuracy = ini.GetDoubleValue("SGT", "fXpFeedMinAccuracy",
                                               xpFeedMinAccuracy);
        bardTeachingUnlocks = ini.GetBoolValue("SGT", "bBardTeachingUnlocks",
                                               bardTeachingUnlocks);
        tierPromotion =
            ini.GetBoolValue("SGT", "bTierPromotion", tierPromotion);
        promote4Floor = static_cast<int>(
            ini.GetLongValue("SGT", "iPromote4Floor", promote4Floor));
        promote5Floor = static_cast<int>(
            ini.GetLongValue("SGT", "iPromote5Floor", promote5Floor));
        followerKeepAlive =
            ini.GetBoolValue("SGT", "bFollowerKeepAlive", followerKeepAlive);
        enchantedBand =
            ini.GetBoolValue("Band", "bEnchantedBand", enchantedBand);
        autoPlay = ini.GetBoolValue("Cheats", "bAutoPlay", autoPlay);
        noFail   = ini.GetBoolValue("Cheats", "bNoFail", noFail);
        sgtNativeStart =
            ini.GetBoolValue("SGT", "bNativeStart", sgtNativeStart);
        closeInventoryOnTrigger = ini.GetBoolValue(
            "Session", "bCloseInventoryOnTrigger", closeInventoryOnTrigger);
        sgtBrowseSheathe =
            ini.GetBoolValue("SGT", "bBrowseSheathe", sgtBrowseSheathe);
        spdlog::info("Settings: loaded (spikes {}/{}).",
                     spike2Audio, spike3Render);
    }

    void Settings::Save() const {
        CSimpleIniA ini;
        ini.SetUnicode();
        // best effort: an absent file just gets the UI-owned keys
        ini.LoadFile(kIniPath);
        ini.SetLongValue("Session", "iDifficulty", difficulty);
        ini.SetBoolValue("Session", "bPauseWorld", pauseWorld);
        // UI-owned: a key the settings tool can change MUST be written here
        // or the change silently reverts on the next load.
        ini.SetBoolValue("Session", "bHandleLute", handleLute);
        ini.SetBoolValue("Session", "bHandleFlute", handleFlute);
        ini.SetBoolValue("Session", "bHandleDrum", handleDrum);
        ini.SetBoolValue("Session", "bHandleGuitar", handleGuitar);
        ini.SetBoolValue("SGT", "bDuetPassthrough", duetPassthrough);
        ini.SetBoolValue("Band", "bEnchantedBand", enchantedBand);
        ini.SetBoolValue("Input", "bFretsOnly", fretsOnly);
        // In Save the moment it gained a UI: a key the settings tool can
        // change but Save does not write silently reverts on the next load.
        ini.SetBoolValue("Input", "bGamepadMode", gamepadMode);
        // Presentation, all now reachable from the settings page. A key the
        // UI can change MUST be written here or the change silently reverts
        // on the next load, which reads as the toggle being broken.
        ini.SetBoolValue("Session", "bPerformanceCameraDirector",
                         performanceCameraDirector);
        ini.SetBoolValue("Session", "bPerformanceVanityCamera",
                         performanceVanityCamera);
        ini.SetBoolValue("Highway", "bRichFx", richFx);
        ini.SetDoubleValue("Highway", "fLookaheadSec", highwayLookaheadSec);
        ini.SetDoubleValue("Audio", "fMissSfxVolume", missSfxVolume);
        ini.SetDoubleValue("Audio", "fUiSfxVolume", uiSfxVolume);
        ini.SetDoubleValue("Audio", "fSongVolume", songVolume);
        ini.SetBoolValue("SGT", "bIdleKeepAlive", sgtIdleKeepAlive);
        ini.SetBoolValue("Cheats", "bAutoPlay", autoPlay);
        ini.SetBoolValue("Cheats", "bNoFail", noFail);
        ini.SetDoubleValue("Stars", "fStar1", star1);
        ini.SetDoubleValue("Stars", "fStar2", star2);
        ini.SetDoubleValue("Stars", "fStar3", star3);
        ini.SetDoubleValue("Stars", "fStar4", star4);
        ini.SetDoubleValue("Stars", "fStar5", star5);
        ini.SetBoolValue("Stars", "bRankGate", rankGate);
        ini.SetLongValue("Stars", "iGateSongs2", gateSongs2);
        ini.SetLongValue("Stars", "iGateSongs3", gateSongs3);
        ini.SetLongValue("Stars", "iGateSongs4", gateSongs4);
        ini.SetLongValue("Stars", "iGateSongs5", gateSongs5);
        ini.SetBoolValue("Stars", "bGate5NeedsFiveStar",
                         gate5NeedsFiveStar);
        ini.SetLongValue("SGT", "iReactionNeutralStars",
                         reactionNeutralStars);
        ini.SetLongValue("SGT", "iReactionPositiveStars",
                         reactionPositiveStars);
        ini.SetLongValue("SGT", "iXpPerStar", xpPerStar);
        ini.SetLongValue("SGT", "iXpBonus5", xpBonus5);
        ini.SetLongValue("Gold", "iPayoutMinStars", payoutMinStars);
        // [Difficulty] writes only what differs from the compiled defaults
        // and DELETES keys that match them. The unconditional writes this
        // replaces pinned the whole section at save-time values, so the
        // 2026-07-25 Glory rebalance (7.5/12 -> 4.0/3) never reached any
        // install whose settings tool had ever saved - the INI kept
        // re-asserting the old numbers as if the user had chosen them.
        // Same failure as the muffle-list pin (d11cdd0), and the same
        // principle as the secondary keyboard column below, which is
        // deliberately never written. Values a user actually changed
        // differ from the defaults, so those still persist.
        {
            const difficulty::Tuning defs;
            auto setD = [&](const char* a_key, double a_v, double a_def) {
                if (a_v == a_def) {
                    ini.Delete("Difficulty", a_key, true);
                } else {
                    ini.SetDoubleValue("Difficulty", a_key, a_v);
                }
            };
            auto setL = [&](const char* a_key, long a_v, long a_def) {
                if (a_v == a_def) {
                    ini.Delete("Difficulty", a_key, true);
                } else {
                    ini.SetLongValue("Difficulty", a_key, a_v);
                }
            };
            auto setB = [&](const char* a_key, bool a_v, bool a_def) {
                if (a_v == a_def) {
                    ini.Delete("Difficulty", a_key, true);
                } else {
                    ini.SetBoolValue("Difficulty", a_key, a_v);
                }
            };
            setD("fHitWindowScale", tuning.hitWindowScale,
                 defs.hitWindowScale);
            setD("fStrumLeniencySec", tuning.strumLeniencySec,
                 defs.strumLeniencySec);
            setD("fEarlyStrumLeniencySec", tuning.earlyStrumLeniencySec,
                 defs.earlyStrumLeniencySec);
            setD("fHopoLeniencySec", tuning.hopoLeniencySec,
                 defs.hopoLeniencySec);
            setD("fSustainDropLeniencySec", tuning.sustainDropLeniencySec,
                 defs.sustainDropLeniencySec);
            setB("bInfiniteFrontEnd", tuning.infiniteFrontEnd,
                 defs.infiniteFrontEnd);
            setB("bAntiGhosting", tuning.antiGhosting, defs.antiGhosting);
            setL("iMaxMultiplier", tuning.maxMultiplier,
                 defs.maxMultiplier);
            setD("fGloryMeterSpanHits", tuning.gloryMeterSpanHits,
                 defs.gloryMeterSpanHits);
            setD("fGloryBadWeight", tuning.gloryBadWeight,
                 defs.gloryBadWeight);
            setD("fGloryStarPowerHitScale", tuning.gloryStarPowerHitScale,
                 defs.gloryStarPowerHitScale);
            setD("fGloryStarPowerBadScale", tuning.gloryStarPowerBadScale,
                 defs.gloryStarPowerBadScale);
            setD("fGloryOpeningSec", tuning.gloryOpeningSec,
                 defs.gloryOpeningSec);
            setD("fGloryOpeningBadScale", tuning.gloryOpeningBadScale,
                 defs.gloryOpeningBadScale);
            setL("iGloryRecoveryHits", tuning.gloryRecoveryHits,
                 defs.gloryRecoveryHits);
            setD("fGloryRedBelow", tuning.gloryRedBelow,
                 defs.gloryRedBelow);
            setD("fGloryGreenAt", tuning.gloryGreenAt, defs.gloryGreenAt);
            setD("fFailureDangerBelow", tuning.failureDangerBelow,
                 defs.failureDangerBelow);
            setD("fFailureRecoverAt", tuning.failureRecoverAt,
                 defs.failureRecoverAt);
            setD("fFailureGraceSec", tuning.failureGraceSec,
                 defs.failureGraceSec);
            setD("fFailureStartSec", tuning.failureStartSec,
                 defs.failureStartSec);
            setL("iFailureFurtherBad", tuning.failureFurtherBad,
                 defs.failureFurtherBad);
            setD("fAudienceCommentDelaySec", tuning.audienceCommentDelaySec,
                 defs.audienceCommentDelaySec);
        }
        // Bindings tab (2026-07-27). The secondary keyboard column
        // (iFret1Key2 etc.) is deliberately NOT written: it is INI-only
        // bridge-layout territory, preserved by the load-merge above.
        ini.SetLongValue("Input", "iFret1Key", fret1Key);
        ini.SetLongValue("Input", "iFret2Key", fret2Key);
        ini.SetLongValue("Input", "iFret3Key", fret3Key);
        ini.SetLongValue("Input", "iFret4Key", fret4Key);
        ini.SetLongValue("Input", "iFret5Key", fret5Key);
        ini.SetLongValue("Input", "iStrumKey", strumKey);
        ini.SetLongValue("Input", "iStarPowerKey", spKey);
        ini.SetLongValue("Input", "iWhammyKey", whammyKey);
        ini.SetLongValue("Input", "iPauseKey", pauseKey);
        ini.SetLongValue("Input", "iGamepadFret1", gamepadFret1);
        ini.SetLongValue("Input", "iGamepadFret2", gamepadFret2);
        ini.SetLongValue("Input", "iGamepadFret3", gamepadFret3);
        ini.SetLongValue("Input", "iGamepadFret4", gamepadFret4);
        ini.SetLongValue("Input", "iGamepadFret5", gamepadFret5);
        ini.SetLongValue("Input", "iGamepadStrum1", gamepadStrum1);
        ini.SetLongValue("Input", "iGamepadStrum2", gamepadStrum2);
        ini.SetLongValue("Input", "iGamepadStarPower", gamepadSp);
        ini.SetLongValue("Input", "iGamepadWhammy", gamepadWhammy);
        ini.SetLongValue("Input", "iGamepadPause", gamepadPause);
        const auto rc = ini.SaveFile(kIniPath);
        if (rc < 0) {
            spdlog::warn("Settings: save to {} failed ({})", kIniPath,
                         static_cast<int>(rc));
        }
    }
}
