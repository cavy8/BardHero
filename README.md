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
- Single-theme UI customization through `Data/SKSE/Plugins/BardHero/theme.ini`
- Live theme editing and preview through FLICK

## Themes

BardHero currently uses one theme from
`Data/SKSE/Plugins/BardHero/theme.ini`. The same theme is editable in-game
through FLICK's **Bard Hero Theme** tool. Changes to menu colors, geometry,
fret/gameplay colors and highway tint apply immediately and are saved back
to `theme.ini` automatically.

The preview is the real menu. Each tab of the editor puts the actual
BardHero window it themes on screen behind the FLICK menu, at full size, so
you are looking at the finished result rather than a sample of it:

- **Menus** shows the real Songbook or the real results panel — pick which
  from the tab's *Preview* control. Song rows do nothing while the Songbook
  is a preview, and the results panel opens settled on a sample run, with
  its celebration animation and sounds held. Neither is shown during a song;
  both close themselves when a run starts.
- **Gameplay** and **Highway** show the real note highway on a short demo
  phrase, drawn by the same code and at the same size as gameplay, over the
  real background image. If a song is already playing — pause it and open
  the editor from the pause menu's **Settings** row — you tune against your
  own chart instead.

Every preview is tied to the page being open: leave the tab, switch tools or
close the FLICK menu and it takes itself down.

The theme intentionally exposes a small set of high-impact choices rather
than every ImGui/style value: BardHero panel colors and rounding, the core
gameplay palette (all five frets, open notes, misses and Star Power), and an
optional highway image. FLICK remains responsible for its own host/sidebar
chrome.

Highway artwork follows the Clone Hero convention: use a 1:2 width:height
image such as 512x1024 or 1024x2048. The image is mapped onto BardHero's
perspective highway as one continuous image quad and scrolls with the notes;
it is never tiled or split into strips. Changing the image path is
committed when the path field is finished (or when **Save now** is pressed),
then the texture is released/reloaded without restarting Skyrim. Manual
`theme.ini` edits can be picked up with **Reload from file**.

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
