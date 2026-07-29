# Bard Hero

A Clone Hero style rhythm minigame inside Skyrim, built as an SKSE plugin.
You walk up to an instrument, pick a song, and play it on a five fret note
highway while the tavern watches. Good playing earns gold and bard
expertise. Bad playing gets you heckled.

The lute, flute and drum contexts come from Skyrim's Got Talent. An
optional addon adds the Doom Lute, an electric guitar you forge at the
Atronach Forge, with a spectral skeleton band that plays behind you.

## Features

- Note highways for lute, flute, drum and electric guitar
- A songbook with per instrument repertoire, difficulty tabs and song
  unlocks
- Practice mode with section looping and speed control
- Keyboard, gamepad and Guitar Hero controller support, rebindable in the
  in game settings
- Crowd mood, NPC commentary, gold payouts and expertise progression
- Star Power with audio effects on the stems
- Reads standard Clone Hero charts (.chart, .mid, song.ini), so you can
  drop in your own songs

## Requirements

- Skyrim Special Edition 1.5.97, or Anniversary Edition 1.6.317 up to
  1.6.1170
- [SKSE64](https://skse.silverlock.org/)
- Skyrim's Got Talent
- FLICK (Fuzz's Legally Intelligible Core Kit)
- Optional: Inventory Interface Information Injector for the "Instrument"
  item category. TrueHUD and the STB widget mods work fine and get hidden
  automatically during songs.

Install the packaged FOMOD release with your mod manager. Release packages
are built with tools/package_fomod.ps1. Song packs install separately.

## Building from source

Visual Studio 2022 or newer with the C++ workload, CMake 3.21 or newer,
and vcpkg.

```bat
call "<VS>\VC\Auxiliary\Build\vcvars64.bat"
set VCPKG_ROOT=<your vcpkg>
cmake --preset release
cmake --build build/release --target BardHero
```

tools/test.bat builds and runs the 41 headless test suites. Adjust the
paths at the top for your machine.

The electric guitar addon's art (models, animations, the Doom Lute
plugin) ships separately and is not in this repository. A source build
produces the core plugin.

## License

The source code is GPL-3.0, see LICENSE. Bundled game assets under dist/
(audio, textures, charts) are not covered by the GPL and remain under
their own terms. THIRD-PARTY-NOTICES.md lists the libraries this project
builds on.
