# Changelog

Every user-visible change lands here in the release it ships with, written
when the change lands rather than reconstructed at release time. The Nexus
changelog tab is pasted from the current release's section (through the
house copy pass first); this file is the source of truth, the page is the
copy.

## 1.1 - 2026-07-28

### Fixed

- Skyrim AE 1.6.317 through 1.6.1129 (1.6.640 above all) now loads. The
  runtime gate accepted only SE 1.5.97 and AE 1.6.1130+, so everything in
  between got the SKSE "reported as incompatible during load" dialog from
  our own refusal, on a DLL with no actual reason to refuse. Every engine
  address is runtime-resolved and every hook site is verified before
  install, so an unknown runtime degrades to a logged no-hook rather than
  a bad write. Field-confirmed on 1.6.640.
- Crash at session start while hiding third-party HUD widgets. The gold
  readout tears its Flash state down on its own schedule, and invoking
  into a movie whose ActionScript layer was already dead crashed inside
  Scaleform. The muffle now verifies the layer is alive before calling
  into it, on hide, on resume and on restore.
- The performance camera opens on the same shot no matter how you were
  looking when the song started. The engine folds the player's look
  pitch into camera state in several places at once, so entering while
  looking up used to put the camera under the performer staring up, and
  entering from a high look-down angle parked it directly overhead. The
  director now measures your look pitch once at the moment the song
  starts and cancels it out of every shot for the whole performance,
  without ever touching the player's own view state - your camera is
  exactly as you left it when the song ends.
- The 2026-07-25 difficulty rebalance now actually reaches installs. The
  packaged INI shipped explicit copies of the old tuning values and the
  settings tool re-wrote them on every save, so the old miss weight (7.5,
  with a 12-hit recovery gate) kept playing under the new version's name.
  Difficulty tuning now ships as commented documentation, and the settings
  tool writes only values you actually changed.
- Gold is no longer paid to an empty room. The performance payout scaled
  on stars, mood, difficulty, rank, venue and song length - but never on
  whether anyone was listening, so a flawless set on an empty mountainside
  paid a full busking purse. Listeners near the player are now counted
  during the song and the purse scales on the time-weighted average:
  nobody pays nothing, one lone patron pays a reduced tip, a small crowd
  pays in full. Your own summoned band does not count as its own audience,
  and neither does wildlife. The results screen says so when nobody was
  around, and `fAudienceRadius = 0` in the INI restores the old behaviour.

- Practice loop no longer eats the fret you hold across the wrap. The
  loop restart rebuilds the judgment engine, and the input mapper kept
  believing the old engine still knew which frets were down - so a note
  held through the reset refused to register until you physically lifted
  and re-pressed, with the strike-line pad sitting lit the whole time.
  The mapper now re-tells a rebuilt engine everything that is physically
  held, the moment the rebuild happens.
- The right instrument stays in your hands. The player, the summoned
  bassist and the rhythm guitarist all share one vanilla animation-object
  record, and whoever's model swap landed last decided what the next
  capture wore - which is how a performance could hand you the band's
  guitar, or a bass, especially on songs with no rhythm stem, where the
  record was left pointing at the bass for every performance after. The
  band now restores the session's own instrument one capture-interval
  after each of its swaps, and again when the band leaves.

### Changed

- New `[Gold]` INI keys, INI-only and never rewritten by the settings
  tool: `fAudienceRadius` (default 1200 units, about 17 m),
  `fAudiencePayLone` (default 0.40), `iAudienceFullAt` (default 4).
- The results screen's payout line now names a thin crowd when it shrank
  the purse.

### Internal

- `tools/build.bat` had run zero test suites since its 2026-07-26 rewrite:
  findstr's `$` anchor never matches CRLF lines, so the suite list parsed
  empty and the guard failed every build at the test step. All 41 suites
  run again.

## 1.0 - 2026-07-28

First public release. Rhythm engine (chart/mid parsing, CH/YARG-faithful
judgment, frame-rate-independent clock), note highway and HUD, songbook
with rank progression and unlocks, live crowd mood with Guitar Hero-style
performance meter and failure, SGT integration (lute, flute, drum),
practice mode with pitch-preserving speed, native controller support,
performance camera director, the optional Doom Lute electric addon, and
the BA Bard Songs compatibility pack. Ships complete third-party licence
texts and asset attribution.
