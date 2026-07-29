# Changelog

Every user-visible change lands here in the release it ships with, written
when the change lands rather than reconstructed at release time. The Nexus
changelog tab is pasted from the current release's section (through the
house copy pass first); this file is the source of truth, the page is the
copy.

## 1.2 - 2026-07-30

### Fixed

- Quitting a song within the first few seconds no longer leaves you stuck
  playing the lute. Skyrim's Got Talent starts its performance on a script
  thread that takes a few seconds, and quitting mid-startup let the tail of
  that thread put you back into the playing pose after the cleanup had
  already run. The exit now catches the late-landing pose and ends it, and
  the cleanup learned the exit animation earlier so it works on a session of
  any age.

- Vanilla lutes no longer turn into a bass guitar. The summoned band borrows
  the game's shared lute prop record for its own instruments, and the code
  that puts it back was reading its "original" value after the band had
  already changed it, so it restored a bass. Once that happened every lute in
  the game was a bass until you restarted, including the ones tavern bards
  play.
- A crash while restoring third-party HUD widgets at the end of a song. The
  1.1 fix checked that a widget's script layer was alive before calling into
  it, but only on one of the two paths that talk to Flash. The other path now
  checks too.
- The whammy key reads as ";" on the Bindings page. It was only ever bound in
  the secondary controller-bridge column, which that page does not show, so
  it looked unbound while the key worked perfectly well in game.
- Playing a song with a follower works again. Asking someone with an
  instrument to play together would show the dialogue option and then do
  nothing at all. Skyrim's Got Talent builds that duet inside the script
  Bard Hero replaces when you equip an instrument, so the duet was being
  prevented before it ever started. Bard Hero now stands down for the whole
  performance whenever you have asked a follower to play, handing the equip
  back untouched. This needs no setting and applies whatever else you have
  configured.
- Controller chords no longer overstrum themselves. In Gamepad Mode a chord
  pressed with a small spread between the buttons landed across two input
  frames, and each frame strummed - the engine counts a second strum that
  fast as an overstrum, so playing the chord cleanly still broke your
  combo. Fret presses within the strum leniency window now join the chord
  instead of strumming again.

### Added

- A No Fail cheat, under Settings then Cheats. However badly a song goes,
  the crowd never ends it early: it plays to the end, and the stars, XP and
  gold still judge the run honestly. Takes effect when the next song starts.
- The Songbook now tells you when the scan found songs it is not showing.
  An ordinary Clone Hero chart carries no instrument tag, and an untagged
  chart is invisible from every instrument's songbook, which used to look
  exactly like the scan failing - one charter went hunting through MO2
  folders over it. The empty Songbook now says how many songs were found,
  why they are hidden, and that a folder named "guitar" is what tags them.
  The INI and readme spell out the same rule.
- Gamepad Mode is now a setting in the game, under Settings then Difficulty.
  It was always in the INI, but with no way to see it: if you play a real
  guitar controller with a strum bar, Gamepad Mode makes fretting strum for
  you and the bar does nothing. Turn it off and strum for yourself.
- Fret-only play, under Settings then Difficulty. Pressing a fret strums by
  itself, so you never need the strum key. For anyone who finds holding a
  fret and hitting Space at the same time awkward, which two of you asked
  for on the same day. A chord counts as a single strum even when your
  fingers land slightly spread, and the Strum leniency slider on the same
  page widens how much spread is forgiven, if you need more. The strum key
  keeps working if you want it. One thing to know before you switch it on:
  a fret pressed when no note is due now costs you an overstrum.
- The conjured band can be switched off, under Settings then General.
  Guitar songs then play with no ensemble and nothing else changes. Worth
  knowing beyond taste: one crash shortly after a guitar song started went
  away with the band off, so if that happens to you, this switch is the
  first thing to try - and do tell us if it helps, because that is exactly
  the report that pins it down.
- An Instruments tab in Settings, deciding which instruments open the Bard
  Hero songbook. Untick one and it goes back to Skyrim's Got Talent
  completely, with its own performances and its own songs, including any
  custom ones you added there, while Bard Hero keeps the ones you left
  ticked. All four stay ticked by default, so nothing changes unless you
  want it to. A toggle made during a performance applies once the song ends.
- The Doom Lute has an inventory description, through Description Framework.
  Optional, and the same mechanism Skyrim's Got Talent uses to describe the
  vanilla lute, flute and drum. Without the framework the card simply has no
  description line.

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
