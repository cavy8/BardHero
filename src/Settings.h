#pragma once

#include "game/DifficultyTuning.h"
#include "game/WidgetMuffleLogic.h"

#include <filesystem>
#include <string>

namespace SH {
    // INI-backed settings. M0 scope: verbose logging + spike switches. Read
    // from Data/SKSE/Plugins/BardHero.ini (the deployed dist copy).
    class Settings {
    public:
        static Settings& GetSingleton();
        void Load();
        // Persist the settings-page fields back to the INI (SimpleIni
        // load-modify-save keeps everything else intact). Called from the
        // render thread on every UI change - the mutated fields are plain
        // word-sized PODs read lock-free elsewhere; aligned word tearing
        // does not exist on x64 and every consumer tolerates a one-frame
        // stale value.
        void Save() const;

        bool verboseLog = true;

        bool spike2Audio = false;
        bool spike3Render = false;
        int  spike2DurationSec = 60;

        // M2 session debug entry points, now OFF BY DEFAULT (2026-07-26).
        //
        // These were the original pre-UI flow: Numpad * started/paused a
        // session and Numpad - aborted it. The real entry point has been
        // instrument item interaction for a long time, and the pause menu
        // owns quitting, so a live key that can start or kill a performance
        // out from under the player is a hazard rather than a convenience -
        // user ask, 2026-07-26: "remove the hotkeys to close the BH game or
        // open it".
        //
        // Kept as SETTINGS rather than deleted, because they are genuinely
        // useful for development when there is no instrument to hand. 0 = off
        // is the same "gated on the key being set at all" shape iProbeKey and
        // iDebugPracticeKey already use.
        //
        // ⚠ A live INI carrying the old 106/109 values OVERRIDES this
        // default. Changing the code default is not enough on an existing
        // install - the deployed INI has to be edited too.
        int         debugStartKey = 0;  // was VK_MULTIPLY (Numpad *)
        int         debugAbortKey = 0;  // was VK_SUBTRACT (Numpad -)
        // VK_DIVIDE (Numpad /): start a PRACTICE session over the whole song
        // on the last song played (or the first scanned one). Temporary P2
        // field-test entry point - the section picker in P4 replaces it.
        // Deliberately not 0x6B, which iProbeKey already claims. 0 = off.
        int         debugPracticeKey = 0x6F;
        // Practice playback speed, 5% a press (plan P6). VK_OEM_MINUS /
        // VK_OEM_PLUS - the unshifted -/= pair above the letters, which no
        // rhythm bind claims. Live during a PRACTICE run only; ignored
        // otherwise, so they can never disturb an ordinary performance.
        // 0 = off.
        int         practiceSpeedDownKey = 0xBD;
        int         practiceSpeedUpKey   = 0xBB;
        // VK_ADD (Numpad +): dump one control probe wherever you are. Purely
        // a logger - it filters nothing and changes no state. Exists because
        // the session-start probe fires with the hook already engaged, so it
        // can never answer "what does this read when the player CAN move?".
        // Off unless a diagnostic INI explicitly opts in. Release packages
        // omit the probe setting entirely.
        int         debugProbeKey = 0;
        std::string songsFolder   = "Data/SKSE/Plugins/BardHero/songs";
        // Physical user library scanned alongside the MO2/VFS library.
        // Resolved from {Documents} while loading the INI so Bridge can add
        // completed downloads without requiring Skyrim to remount a mod.
        std::filesystem::path userSongsFolder;
        bool autoRescanUserSongs = true;
        // Third-party HUD widget MENUS hidden while a session runs (comma
        // list; blank disables). Defaults cover STB Widgets and STB Active
        // Effects - see game/WidgetMuffleLogic.h for where the names come
        // from and how unverified ones are marked.
        std::string hideMenusDuringSession{
            std::string(widget_muffle::kDefaultMenus) };
        int         difficulty    = 3;     // 0..3 Easy..Expert
        // Optional spell-cast start trigger (user request): casting this
        // spell/power starts the session like the start key. Format
        // "Plugin.esp|0xLOCALID"; empty = disabled. Default = Skyrim's Got
        // Talent - Improve As a Bard's Lute performance power.
        std::string performSpell  = "SkyrimsGotTalent-Bards.esp|0x0022F0";
        // SGT integration: the three native abilities and the optional
        // Electric addon ability trigger instrument-bound Songbooks.
        // performSpell above stays the legacy lute alias;
        // sPerformSpellLute wins when present. Empty string = that
        // instrument disabled.
        std::string performSpellFlute = "SkyrimsGotTalent-Bards.esp|0x0022EE";
        std::string performSpellDrum  = "SkyrimsGotTalent-Bards.esp|0x0022ED";
        std::string performSpellGuitar =
            "Bard Hero - Doom Lute.esp|000803";
        // M3 input binds (spec 7 / 14.2) - DIK scan codes (DirectInput
        // domain: matches both the DI buffer's ofs and the InputEvent
        // idCode), NOT the VK codes the debug keys use. 0 = unbound.
        int fret1Key  = 0x02;  // DIK_1 (number row)
        int fret2Key  = 0x03;  // DIK_2
        int fret3Key  = 0x04;  // DIK_3
        int fret4Key  = 0x05;  // DIK_4
        int fret5Key  = 0x06;  // DIK_5
        int strumKey  = 0x39;  // DIK_SPACE (press = strum, CH/YARG keyboard)
        int spKey     = 0x2A;  // DIK_LSHIFT (star power)
        int whammyKey = 0x00;  // unbound (spec 14 keyboard-whammy OQ)
        int pauseKey  = 0x01;  // DIK_ESCAPE (session pause)
        // Secondary binds (field 2026-07-19): a controller-as-keyboard
        // bridge (Wii GH guitar) plays alongside the keyboard - defaults
        // mirror its Clone Hero layout. Strum has a third slot because a
        // strum bar is two switches (up + down). 0 = unbound.
        int fret1Key2 = 0x1E;  // DIK_A
        int fret2Key2 = 0x1F;  // DIK_S
        int fret3Key2 = 0x24;  // DIK_J
        int fret4Key2 = 0x25;  // DIK_K
        int fret5Key2 = 0x26;  // DIK_L
        int strumKey2 = 0xC8;  // DIK_UP (strum bar up)
        int strumKey3 = 0xD0;  // DIK_DOWN (strum bar down)
        int spKey2    = 0x23;  // DIK_H (gh3.PIE: guitar Minus + tilt)
        int whammyKey2 = 0x27;  // DIK_SEMICOLON
        int pauseKey2  = 0x1C;  // DIK_RETURN (gh3.PIE: guitar Plus) -
                                // pauses while playing, RESUMES while
                                // paused (hook-captured toggle)
        // Native gamepad support. Values are SKSE's normalized gamepad
        // macro codes, so Xbox and PlayStation controllers share one
        // configuration. Defaults follow Clone Hero's recommended gamepad
        // layout, with Gamepad Mode auto-strumming fret presses.
        bool controllerEnabled = true;
        bool gamepadMode       = true;
        int  gamepadFret1      = 280;  // LT / L2
        int  gamepadFret2      = 274;  // LB / L1
        int  gamepadFret3      = 275;  // RB / R1
        int  gamepadFret4      = 281;  // RT / R2
        int  gamepadFret5      = 276;  // A / Cross
        int  gamepadStrum1     = 266;  // D-pad up
        int  gamepadStrum2     = 267;  // D-pad down
        int  gamepadSp         = 278;  // X / Square
        int  gamepadWhammy     = 279;  // Y / Triangle
        int  gamepadPause      = 270;  // Start / Options
        int  gamepadConfirm    = 276;  // A / Cross
        int  gamepadCancel     = 277;  // B / Circle
        // ISOLATION KILL-SWITCH (diagnostic, 2026-07-20 walk-lock). When
        // true, the input hook (InputHook.cpp DispatchHook::thunk) becomes a
        // pure no-op pass-through: it NEVER walks or unlinks the engine's
        // intrusive InputEvent list and never feeds the guitar engine. It
        // exists to answer ONE question in one field run - does the
        // post-session walking lock survive with our list splice removed? -
        // and is read ONCE at Install into a static. Leave 0 in shipping
        // builds; frets do not register while it is on. See
        // docs/handoffs/2026-07-20-2240-walking-lock-narrowed-to-input-hook.md.
        bool hookNeverFilter = false;
        // Freeze the game world while the session is paused (manual
        // numPausesGame bump - menu-less, so Skyrim Souls cannot strip it).
        bool pauseWorld = true;
        // Presentation: enter Skyrim's native auto-vanity camera during a
        // performance so the rooted bard and audience remain visually alive.
        // The session restores third person before results, then the original
        // first-person state through its existing camera ledger.
        bool performanceVanityCamera = false;
        // Cinematic performance camera (spec
        // docs/superpowers/specs/2026-07-26-performance-camera-director-design.md).
        // Directs the camera during a song: musically motivated cuts on bar
        // and section boundaries, beat-locked shake, an FOV push on Star
        // Power. Takes precedence over performanceVanityCamera above, which
        // remains the simpler fallback and whose meaning is unchanged.
        //
        // ⚠ Read at LOAD time to decide whether to install the camera hook
        // at all. The settings page can toggle the director live, but
        // turning it on for the first time only takes effect on the next
        // game start - the page says so under the checkbox.
        //
        // ON BY DEFAULT since 2026-07-27, at the owner's call. It is the
        // feature the mod is being shown off with, and shipping it off
        // meant every fresh install saw the plain camera, toggled it, saw
        // nothing change, and concluded it was broken - which happened
        // twice on a second machine before anyone read the log.
        //
        // The cost, since the previous default existed to avoid it: EVERY
        // install now carries the ThirdPersonState::Update hook, including
        // players who never wanted the director. The per-frame path is
        // still gated on the live setting, so turning it off costs an
        // atomic load per frame and nothing else.
        bool performanceCameraDirector = true;
        // Streak fire (GH parity): the note streak at which the performer
        // catches light. A sustained whole-body flame cloak on Skyrim's own
        // FireCloakFXShader. It is NOT silent by construction - that claim
        // was wrong: EffectShaderData carries an `ambientSound`, and this
        // shader sets one. Session.cpp clears it on first use. 0 disables
        // the cloak entirely, which is what
        // the settings-page toggle writes; the threshold itself stays
        // INI-only, so toggling off and back on restores the number the
        // player chose rather than snapping to the default.
        int streakFireHandsAt = 30;
        // Reserved for a fiercer second tier. NOT WIRED - only one fire
        // shader has ever been confirmed to render in the field. See
        // streakfire::Params::bigAt.
        int streakFireBlazeAt = 90;
        // M4 highway (spec 9): noteSpeed as seconds-of-lookahead (the CH
        // number mapping table is M5 settings UI).
        double highwayLookaheadSec = 1.10;
        // Juice pack (2026-07-19): hit bursts, sustain flames, fret kicks,
        // beat pulse, combo rails, SP glints. 0 = the plain M4 look.
        bool richFx = true;
        // What a session silences. The goal is "cut the MUSIC, keep the
        // world": the song has to be audible, but Skyrim should still
        // sound alive around the performance.
        //
        // MUS (AudioCategoryMUS) plus the currently-playing music type are
        // always ducked - that is the actual competing audio.
        bool duckCurrentMusic = true;
        // ⚠ AMB is NOT music. AudioCategoryAMB is Skyrim's whole ambience
        // bus, and sound categories are hierarchical - zeroing its local
        // multiplier takes everything beneath it with it. Defaulted ON
        // originally to clear the soundscape; field 2026-07-20: "the rest
        // of the world went silent... ideally we just want to cut out the
        // music and keep all the sound effects." Now OFF. SFX, voices and
        // footsteps were never touched and still are not.
        bool duckAmbience = false;
        // Play the song through the 3D spatializer, positioned where the
        // player is standing, with the CAMERA as the listener - so the
        // instrument reads as sounding in the world rather than piped
        // straight to the speakers (field 2026-07-20). ⚠ The spatializer is
        // a point source, so a stereo stem collapses toward mono. That is
        // right for a diegetic instrument and wrong if you would rather
        // hear the mix in full stereo, which is why it is a switch.
        bool worldAudio = true;
        // Song level against the rest of Skyrim, 0..1. Was effectively 1.0
        // and only sounded balanced because we were also silencing the
        // world; with ambience restored (field 2026-07-20) it "overpowers
        // the game world a lot". Sits under the world rather than on top
        // of it - a bard is part of the scene, not the soundtrack.
        // MUST equal ui_sfx::kSongDefaultGain. That constant is the slider's
        // 100% reference (UiSfxLogic.h SongGainFromPercent), so this value is
        // what "Song volume 100%" means on the settings page - it is NOT a
        // 0..1 fraction of full volume. Shipping 1.0 here put the slider at
        // ~667% (field 2026-07-26). Change both together or the shipped
        // default stops reading as 100%.
        float songVolume = 0.15f;
        bool missMutesGuitar = true;  // M4: mute guitar stem on miss
        // Miss-feel v1: fret-buzz one-shot on every new miss/overstrum
        // (works for stemless songs too).
        bool   missSfx       = true;
        // 0.04 = 200% of the 0.02 reference (field recalibration
        // 2026-07-25: the miss bank was inaudible under the song).
        // ...now 0.10 = 500% of that 0.02 reference (user instruction
        // 2026-07-26).
        double missSfxVolume = 0.10;
        // P6 UI SFX set (final 11-file cut 2026-07-25): banner pop, results
        // score-tick loop, the SP gain/use pair, and the electric-only
        // song-end stings. ONE common gain for the whole bank - relative
        // loudness is baked into the files, so a per-file trim here would
        // collapse the mix (contract:
        // BardHero Electric\docs\request-to-main-2026-07-25-ui-sfx.md).
        // The electric miss recordings ride bMissSfx/fMissSfxVolume
        // instead: they replace miss1..3 inside the session's miss bank.
        // 0.07 = 350% of the 0.02 reference (field recalibration
        // 2026-07-25; the first pass shipped 0.6, which played "very
        // loud", and the 0.02 correction then sat too quiet). The two SP
        // impacts carry an additional 25% trim - ui_sfx::CueGainScale.
        bool   uiSfx       = true;
        // ...now 0.10 = 500% of the 0.02 reference (user instruction
        // 2026-07-26).
        double uiSfxVolume = 0.10;
        // GH Star Power filter: flanger on the INSTRUMENT stems while SP
        // is active. Defaults are the "gentle" tuning (field 2026-07-26:
        // "classic" was voiced to punch through the full mix and played
        // way too strong once scoped; spec + reference DSP in
        // BardHero Electric docs/request-to-main-2026-07-25-sp-filter.md
        // and tools/proto_sp_flanger.py there). stereoPhase/trim/ramp
        // stay code-only - the five knobs below are the ones worth
        // turning. ⚠ The live INI pins these same values; change BOTH.
        bool   spFilter         = true;
        double spFilterRateHz   = 0.40;
        double spFilterBaseMs   = 0.5;
        double spFilterDepthMs  = 2.0;
        double spFilterFeedback = 0.35;
        double spFilterWet      = 0.40;
        // Also flange a multi-stem song's backing/vocal "song" stem.
        // OFF: field 2026-07-26, a flanged voice "sounds bad". Single-
        // stem songs always flange - the whole track is the instrument.
        bool spFilterSongStem = false;
        // Block player movement-class controls (move/jump/sneak/POV) while a
        // session is actively playing; everything returns on pause/end/load.
        bool blockMovement = true;
        // SGT accuracy-gold scaling (survey: docs/research/2026-07-18-*).
        bool   goldScale      = true;
        double goldAccMin     = 0.0;
        double goldAccMax     = 3.0;
        double goldMultEasy   = 0.5;
        double goldMultMedium = 0.75;
        double goldMultHard   = 1.0;
        double goldMultExpert = 1.5;
        // Whole-song SGT performance (plan 2026-07-19): keep the perform effect
        // alive for the entire session (suppress its 32.8s exit timer), stop
        // its ~33s clip under our song, defer its payout to session end.
        bool   wholeSongPerform = true;
        // Blank SGT's 30 reaction Message properties before dispatching
        // MessageAndEXP. Added 2026-07-19 (961dffe) so its modal boxes could
        // not fight our results window - on the premise that Show() on None
        // is a no-op. It is not: it is a Papyrus error, and it convicted
        // itself as the post-session control lock in the 2026-07-21 bisect
        // (961dffe was the first bad commit, its parent good). The collision
        // it was invented for is gone anyway - the payout is now HELD until
        // the results window closes. Default OFF; 1 restores the old
        // behaviour for A/B only.
        bool   blankReactionMessages = false;
        // Hook isolation MODE - splits what bHookNeverFilter used to switch
        // as one lump. That switch skipped the engine feed AND the capture
        // decision AND the intrusive-list unlink together, so proving "the
        // lock goes away with it on" never said WHICH of the three does it
        // (2026-07-21: restoring the unlink alone did not fix the lock).
        //   0 = normal: feed + capture + unlink (ship path)
        //   1 = pure pass-through, none of the three (old bHookNeverFilter)
        //   2 = feed the engine, then pass through untouched - no capture,
        //       no unlink. Notes register; nothing is ever swallowed.
        //   3 = feed + capture + unlink, but hand the game ITS OWN event
        //       array with the head swapped and put back, instead of a
        //       pointer to our stack array ([[chained-hook-pitfalls]]:
        //       never pass non-engine memory down a shared chain - FLICK
        //       hooks this same site).
        int    hookMode = 0;
        // ---- live crowd reactions (spec 2026-07-21 section 5) ----------
        // Drive SGT's two reaction globals from the live Glory/Rock Meter.
        // The five numeric fields remain parsed for old INIs but are legacy:
        // the current meter has fixed Guitar-Hero-style per-judgment steps
        // and red/yellow/green thirds instead of a rolling time window.
        bool   liveCrowdMood     = true;
        double moodWindowSec     = 3.0;
        double moodGreatAt       = 0.85;
        double moodTerribleBelow = 0.55;
        double moodHoldSec       = 5.0;
        double moodStartSec      = 4.0;
        // Safe player-facing gameplay tuning. Timing values are snapshotted
        // when a song starts; Glory, failure, and audience timing are read
        // by the live 10 Hz performance policy.
        difficulty::Tuning tuning;
        // OPT-IN and default OFF: pushes SGT's audience quest to its
        // applause stage. This touches SGT's QUEST, not just a value, so it
        // ships off until a field run says the ambient layer needs it.
        bool   driveAudienceStage = false;
        // Our own punctuation one-shots: a cheer on a combo milestone, a
        // groan when a long streak dies, applause or an awkward silence at
        // the final note. (A swell one-shot is loaded and reserved, but
        // nothing fires it yet - star power is not wired to it.)
        // OFF by default since 2026-07-22. Skyrim's Got Talent already
        // gives the audience its own reaction sounds through the NPCs
        // themselves, and field feedback is that they work well - our
        // synthetic one-shots layered on top were redundant AND the shipped
        // files are placeholders (shaped noise from tools/gen_crowd_sfx.py).
        // The code path is kept and tested: a user who drops real samples
        // into sfx/crowd can switch it on. The end-of-song performance
        // sting this comment long promised exists now, but as part of the
        // UI SFX bank (song_pass/_fail_electric under bUiSfx), not here.
        bool   crowdReactions      = false;
        int    cheerEveryNotes     = 25;
        int    streakBreakNotes    = 20;
        double reactionCooldownSec = 4.0;
        double crowdVolume         = 0.6;
        // Performance payout (spec section 5.6). SGT pays only at rank 4+
        // and only in an inn; this pays what the performance was worth.
        bool   performancePayout = true;
        double buskBase          = 20.0;
        double buskOutside       = 0.35;
        double renownAtRank1     = 0.40;
        double moodPayTerrible   = 0.0;
        int    payoutCap         = 60;
        // Fewest stars that earns anything at all. User-set 2 on 2026-07-22
        // after a 0%-accuracy run was still paid; see PayoutMath.h.
        int    payoutMinStars    = 2;
        // ---- our own end-of-performance reaction (2026-07-22) -----------
        // SGT decides the ending from rank and a dice roll and never looks
        // at the playing. 1 = BardHero decides it from the run instead
        // (ovation only if earned, message picked by stars, no SGT gold).
        // 0 = call SGT's MessageAndEXP as before, for A/B.
        bool   ownEnding             = true;
        int    reactionNeutralStars  = 2;  // at/above: the room is content
        int    reactionPositiveStars = 4;  // at/above: actively pleased
        int    xpPerStar             = 1;
        int    xpBonus5              = 1;  // extra for a flawless run
        // SGT only starts its audience quest in four of its five expertise
        // bands - the 46-65 "Medium Player" band performs to nobody. 1 sets
        // the same stage SGT's own branches set, so a crowd always gathers.
        bool   alwaysGatherCrowd     = true;
        // Apply the buff/debuff SGT pairs with each reaction message. The
        // message text describes the effect, so showing one without the
        // other tells the player they were blessed and gives them nothing.
        bool   reactionEffects       = true;
        // Song length scales the purse: a set twice as long is twice the
        // work. Linear against the reference, clamped at both ends.
        double payoutLengthRefSec    = 120.0;
        double payoutLengthMin       = 0.5;
        double payoutLengthMax       = 2.0;
        // STANDALONE PERFORM (architecture change 2026-07-20). SGT's
        // _Talent_PlayInstrument OnEffectStart is one long serial coroutine
        // of Papyrus latents that seizes controls, equipment, camera and
        // animation, and whose release half lives in OnEffectFinish behind a
        // Utility.Wait. We start it, freeze the VM under it with our own
        // world pause, then strip the effect mid-flight - so the release
        // never runs. Eight fixes tried to restore the state it leaked and
        // every one moved the symptom instead of removing it.
        //
        // On: the perform ability is NEVER re-added, so that script never
        // runs at all and there is nothing to leak. BardHero owns the
        // session outright.
        //
        // ⚠ STAGE 1 IS DELIBERATELY INCOMPLETE. With the effect gone we also
        // lose what it drew: the play idle, the instrument prop, the crowd
        // reactions and SGT's own end payout. Stage 1 exists to answer ONE
        // question in one run - does the exit lock survive without SGT?
        //
        // DEFAULT IS OFF (2026-07-21). It used to default on here while the
        // shipped INI set it to 0, so the two disagreed and an install with
        // a missing or unreadable INI silently got the lock experiment: no
        // crowd, no prop, no idle, and nothing in the log saying why. The
        // question it was built to answer has since been answered - the exit
        // lock was two faults of our OWN (blanking SGT's reaction Message
        // properties, and hand-clearing the refcounted ControlMap::unk11C),
        // both fixed and both off by default. SGT's coroutine was never the
        // culprit. Set to 1 only to isolate that script as a suspect again.
        bool   standalonePerform = false;
        bool   sgtIdleKeepAlive = true;   // re-up the play idle if it drops
        double sgtEndCaptureSec = 10.0;   // payout capture window at song end
        // Star thresholds (accuracy, inclusive) - spec 3.
        double star1 = 0.50;
        double star2 = 0.65;
        double star3 = 0.80;
        double star4 = 0.90;
        double star5 = 0.96;
        // Long-performance endurance bonus - spec 4. Short songs earn only
        // their quality-scaled end grant; sets beyond two minutes can add a
        // small accuracy-scaled amount.
        bool   xpFeed            = true;
        double xpFeedBase        = 3.0;
        double xpFeedMinAccuracy = 0.5;
        // Star-clamped rank gate - spec 5.
        bool rankGate = true;
        int  gateSongs2 = 2;  // distinct charts at 3 stars or better to pass 25
        int  gateSongs3 = 3;  // ... to pass 45
        int  gateSongs4 = 4;  // ... to pass 65
        int  gateSongs5 = 5;  // ... to pass 85
        bool gate5NeedsFiveStar = true;  // boundary 85 also needs one 5-star chart
        // Bard teaching unlocks a chart instead of feeding a number the
        // rank gate would claw back - spec 6.3.
        bool bardTeachingUnlocks = true;
        // Effective-tier promotion - spec 6.
        bool tierPromotion = true;
        int  promote4Floor = 46;
        int  promote5Floor = 66;
        // Duet follower keep-alive - spec 8, default OFF until the log-first
        // field run confirms the re-trigger target.
        bool followerKeepAlive = false;
        // Cheats (settings page 2026-07-20). AutoPlay: the input feeder
        // synthesizes perfect chart inputs (progression testing); real
        // fret/strum keys are ignored while on, pause keys stay live.
        bool autoPlay = false;
        // Phase 2 native start: strip the perform ability at the AddTarget
        // hook, before Papyrus schedules OnEffectStart. Field-proven
        // 2026-07-20 - SGT's start flow then does not run at all, so no
        // root, no sheathe stall, no idle, no camera swap. OFF falls back
        // to the old settle-then-strip standstill.
        bool sgtNativeStart = true;
        // The instrument is equipped from the inventory, so that menu is
        // still up when the trigger fires - and it HIDES our browser
        // behind it. Close it for the player.
        bool closeInventoryOnTrigger = true;
        // Phase 3: sheathe natively at the perform trigger, so the weapon
        // is already away by the time a song is picked and SGT skips its
        // SheatheWeapon() + Utility.Wait(2) branch. That 2s branch is what
        // made notes scroll while the character was still drawing the lute
        // (field 2026-07-20). Costs nothing - the player is standing in a
        // browser at that point anyway.
        bool sgtBrowseSheathe = true;
    };
}
