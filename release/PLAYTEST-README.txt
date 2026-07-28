BardHero 0.2.0
Private playtest build, 23 July 2026

WHAT THIS IS

BardHero turns Skyrim bard performances into a five-fret rhythm minigame.
Equip and activate a lute, flute, or drum, choose a song, and play while
Skyrim's Got Talent handles the instrument animation and audience.

This is an early private playtest. Do not upload or publicly redistribute it.

REQUIRED MODS

Install versions matching your Skyrim runtime:

1. SKSE64
2. Address Library for SKSE Plugins
3. SKSE Menu Framework, also called FLICK
4. BA Bard Songs 1.3
5. Skyrim's Got Talent 1.76
6. BardHero

If you install the optional Doom Lute and want its custom animations:

7. Open Animation Replacer

That is all it is for. Everything else works without it - the guitar, the
crafting, the band, the songs. What you lose is the animation set: you get
the ordinary lute idles instead, with no error and nothing in the log, so it
simply looks like the animations were never made. You do NOT need Nemesis or
FNIS: this only REPLACES existing animations and adds nothing to the
behavior graph.

And if you want the Doom Lute to read as an Instrument in your inventory
rather than as Clutter:

8. Inventory Interface Information Injector, usually called I4

Also cosmetic, and also only for the optional plugin. The guitar behaves
identically either way; the difference is the type line and the icon on its
inventory card. This one cannot be fixed from the plugin side: Skyrim has no
instrument keyword anywhere in it - the vanilla lute, flute and drum carry no
keyword at all - and an item's type is drawn by the interface rather than
stored in the record. A rule for I4 is the only lever there is. That rule
ships with the Doom Lute component and does nothing whatsoever if I4 is not
installed.

The tested setup uses:

- Skyrim AE 1.6.1170
- Address Library 11
- FLICK 3.13 Hotfix 2
- BA Bard Songs 1.3
- Skyrim's Got Talent 1.76

The DLL accepts Skyrim 1.5.97 and any AE from 1.6.317 onward, but only AE
1.6.1170 has received real playtesting. If it refuses to load, the log says
which runtime it saw.

INSTALLATION

Install the ZIP with Mod Organizer 2 or another mod manager. Keep BardHero
after BA Bard Songs and Skyrim's Got Talent. Do not enable an older SkyHero
or BardHero build at the same time.

BardHero has no ESP and should not add a plugin to your load order.

PLAYING

Activate a lute, flute, or drum through Skyrim's Got Talent. The Songbook
opens with songs for that instrument.

Default keyboard controls:

- Number row 1 to 5: hold frets
- Space: strum
- Left Shift: Star Power
- Up and Down: move through the Songbook
- Enter: confirm
- Escape: pause, resume, or cancel the current BardHero screen

The in-game Bard Hero page in FLICK contains difficulty, audio, and playtest
settings.

SONGS AND FIRST-PLAY CACHE

This archive contains BardHero-authored charts and metadata for 87 BA Bard
Songs tracks. It contains no BA audio, plugin, scripts, or records.

The Songbook lists the tracks immediately. The first time you start one,
BardHero converts only that track from your installed BA Bard Songs copy into
a local Ogg Opus cache. This normally adds about half a second. Replays use
the cache without converting again.

Do not share the generated song.opus cache files.

PLAYTEST NOTES

- The 87 BA charts are generated playable baselines, not final hand-authored
  transcriptions. Please report awkward patterns, bad alignment, or unsuitable
  rank assignments with the exact song title and difficulty.
- Each instrument starts with two rank-1 songs.
- Rating is the default Songbook sort: best stars, unplayed songs, then locked
  songs from the lowest required rank upward.
- Crowd reaction one-shot samples are intentionally not included. Skyrim's Got
  Talent still supplies the actual NPC audience behavior.
- No bundled test song, development spike, debug probe, cheat, or PDB is
  included.

REPORTING A PROBLEM

Please include:

1. What you did and what happened.
2. The exact song and instrument, if relevant.
3. BardHero.log and BardHero.prev.log.
4. A crash log if Skyrim closed unexpectedly.

On the tested Nolvus setup, the logs are under:

Documents\My Games\Skyrim.INI\SKSE\

The normal Skyrim SKSE log folder may contain an older unrelated copy.

KNOWN PLAYTEST LIMITS

- AE 1.6.1170 is the only field-tested runtime.
- The chart library and lazy audio path need broader machine coverage.
- Some long-standing acceptance checks around pause, camera, and unusual
  gameplay states still need more field time.
- This is not a public release candidate.
