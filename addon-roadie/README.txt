Bard Hero - The Roadie
======================

Loads in your starter setlist: the 22 charts Bard Hero has actually been
tested against. You do not need it to play, and Bard Hero works perfectly
well without it.

WHAT IT DOES

Downloads each chart from Encore (files.enchor.us) - the same public service
the Bridge app uses - and unpacks it into:

    Documents\My Games\Skyrim Special Edition\Bard Hero Songs\guitar

Bard Hero picks new songs up by itself. No restart, nothing to press.

HOW TO USE IT

The Roadie is a TOOL, not a mod. It installs no game files, adds no plugin,
and there is nothing to enable. It ships as an installer only so it can tell
you what it does before you run it.

1. Install it with your mod manager, or just extract it anywhere - both work.
2. Open the "Bard Hero - The Roadie" folder and double-click
   "Run The Roadie.bat".
   In Mod Organizer 2, right-click the mod and choose Open in Explorer.
   Browsing to Data will NOT find it: MO2 virtualises that folder.
3. Read the screen it shows you, then press Enter.

It needs nothing installed. No Python, no extra tools - it uses PowerShell,
which is already part of Windows.

WHY YOU MIGHT WANT IT

A chart is somebody's audio plus somebody's charting work, so Bard Hero does
not include any, and it never connects to the internet itself. You can browse
for charts yourself with Bridge and many people should - but several of these
songs have more than one upload, and the difference is not cosmetic. Some
uploads have no separate guitar track, so missing a note makes no sound at
all. Some carry only an Expert chart, so there is nothing playable below
Expert. This list pins the exact upload that was tested, by hash.

If the chart on the server ever stops matching that hash, the download is
REFUSED rather than quietly swapped for something nobody has played.

WHAT IT SENDS

Nothing about you. It is a plain request for a file, the same as clicking a
download link: no account, no login, no telemetry. Every file it writes goes
under the songs folder shown on screen, and it changes nothing else.

Both this file and the script are plain text. Open them in Notepad if you
would rather read what happens than take a mod's word for it.

OPTIONS

Somewhere other than the default folder:

    powershell -ExecutionPolicy Bypass -File "songfetch\Get-BardHeroSongs.ps1" -SongsFolder "D:\My Songs\guitar"

See what it would do without downloading anything:

    powershell -ExecutionPolicy Bypass -File "songfetch\Get-BardHeroSongs.ps1" -WhatIfOnly

IF SOMETHING GOES WRONG

"hash mismatch" - the charter re-uploaded that song, so what is on the server
is no longer what Bard Hero was tested with. It is skipped on purpose. Get
that one through Bridge if you want it.

"FAILED" with a 404 - the chart was taken down or replaced. Same answer.

Songs do not appear - check they landed in Documents\My Games\Skyrim Special Edition\Bard Hero Songs\guitar
and not somewhere else, then reopen the Songbook.
