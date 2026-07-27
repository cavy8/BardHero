# tools/stage-bisect-build.ps1
#
# Stage whatever the current bisect checkout just built into the ONE mod
# folder the game loads, and leave the config playable. Run after
# tools\build.bat, with the game closed.
#
# Why this exists: commits before 95f09cc (20/07 10:36) are pre-rename.
# They build SkyHero.dll, deploy to MODS\mods\SkyHero - which is where the
# known-good 19/07 binary lives - read SkyHero.ini, and ship a 1-song
# SkyHero\songs tree. Staging by hand went wrong twice: the known-good
# binary was overwritten, and a stale second plugin DLL was left in the
# folder where SKSE loads EVERY *.dll it finds.

param(
    [string]$Repo      = 'C:\Studios\Mod Studio\BardHero',
    [string]$ModDir    = 'C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods\BardHero',
    [string]$KnownGood = 'C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods\SkyHero\SKSE\Plugins\SkyHero.dll',
    # blob of the user-confirmed working 19/07 21:48 build, committed in 69486e9
    [string]$KnownGoodBlob = '4e41f2f6df09b2375c6f16765d9ab1b1d5519e5d',
    [string]$KnownGoodSha  = 'FB78B9B9A6ABC1AA3C878F64DD3341EA29B5BA822C3B28D5A1F12722856AD882'
)

$ErrorActionPreference = 'Stop'
if (Get-Process SkyrimSE -ErrorAction SilentlyContinue) {
    throw 'Skyrim is running - close it before staging.'
}

$plugins = Join-Path $ModDir 'SKSE\Plugins'
$built   = Get-ChildItem (Join-Path $Repo 'build\release\*.dll') |
           Where-Object { $_.Name -match '^(BardHero|SkyHero)\.dll$' } |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $built) { throw 'no built plugin DLL found in build\release' }

# Exactly one plugin DLL may sit in the load folder.
Get-ChildItem "$plugins\*.dll" | Where-Object {
    $_.Name -match '^(BardHero|SkyHero)\.dll$'
} | Remove-Item -Force
Copy-Item $built.FullName (Join-Path $plugins $built.Name) -Force

# A pre-rename build deploys into mods\SkyHero and overwrites the
# known-good binary. Put it back and prove it by hash.
Push-Location $Repo
& cmd /c "git cat-file blob $KnownGoodBlob > `"$env:TEMP\kg.dll`""
Pop-Location
Copy-Item "$env:TEMP\kg.dll" $KnownGood -Force
$sha = (Get-FileHash $KnownGood -Algorithm SHA256).Hash
if ($sha -ne $KnownGoodSha) { throw "known-good restore FAILED (sha $sha)" }

# Point whichever INI this build reads at the full song library; its own
# dist ships a 1-song tree under the old name. No BOM - a BOM in front of
# a section header stops it being recognised.
foreach ($ini in Get-ChildItem "$plugins\*.ini") {
    $lines = (Get-Content $ini.FullName) -replace `
        '^sSongsFolder\s*=.*', 'sSongsFolder = Data/SKSE/Plugins/BardHero/songs'
    [System.IO.File]::WriteAllLines($ini.FullName, $lines,
        (New-Object System.Text.UTF8Encoding($false)))
}

Write-Output "staged: $($built.Name) built $($built.LastWriteTime)"
Write-Output "known-good restored and hash-verified"
Get-ChildItem $plugins -File | Select-Object Name, LastWriteTime | Format-Table -AutoSize
