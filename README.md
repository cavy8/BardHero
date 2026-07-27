# Bard Hero

A Clone Hero style rhythm minigame inside Skyrim, as an SKSE plugin. Walk up
to an instrument, pick a song from the Songbook, and play it on a five-fret
highway with scoring, Glory, Star Power, crowd reactions, gold payouts and
progression. Integrates with Skyrim's Got Talent for bard expertise and the
performance fantasy; brings its own judgment engine, audio playback, UI and
a spectral skeleton backing band for the electric guitar.

## Features

- Five-fret Clone Hero style highway with deterministic judgment and scoring
- Lute, flute and drum contexts through Skyrim's Got Talent, plus an
  electric Doom Lute addon with a conjured skeleton band
- Songbook with instrument-aware repertoire, difficulty tabs, sortable
  columns and song unlocks
- Practice mode with section looping and speed control
- Keyboard, gamepad and Guitar Hero controller support, with an in-game
  Bindings tab (press-to-detect rebinding)
- Crowd mood, NPC commentary, gold payout and expertise progression
- Stem-aware audio playback with Star Power effects
- Reads standard Clone Hero charts (.chart/.mid + song.ini); drop songs in
  and play

## Requirements

- Skyrim Special Edition / Anniversary Edition 1.6.1170
- [SKSE64](https://skse.silverlock.org/)
- Skyrim's Got Talent (bard expertise and performance framework)
- FLICK (Fuzz's Legally Intelligible Core Kit)
- Optional: Inventory Interface Information Injector (cosmetic
  "Instrument" item category), TrueHUD and STB widget mods (hidden
  automatically during songs)

Install the packaged FOMOD release with your mod manager. Release packages
are built with `tools/package_fomod.ps1`; song packs install separately.

## Building from source

Visual Studio 2022 or newer with the C++ workload, CMake 3.21+ and vcpkg.

```bat
call "<VS>\VC\Auxiliary\Build\vcvars64.bat"
set VCPKG_ROOT=<your vcpkg>
cmake --preset release
cmake --build build/release --target BardHero
```

`tools/test.bat` builds and runs the 41 headless test suites (adjust the
paths at the top for your machine). The electric guitar addon's art assets
(models, animations, the Doom Lute plugin) ship separately and are not in
this repository; a source build produces the core plugin.

## License

The source code is licensed under GPL-3.0 (see LICENSE). Bundled game
assets under `dist/` (audio, textures, charts) are not covered by the GPL
and remain under their own terms; see THIRD-PARTY-NOTICES.md for the
libraries this project builds on.
