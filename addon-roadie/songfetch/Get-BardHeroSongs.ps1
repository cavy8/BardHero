# Installs Bard Hero's curated song list onto THIS computer.
#
# WHY THIS IS POWERSHELL AND NOT THE PYTHON TOOL. tools\fetch_songs.py does
# the same job and is the AUTHORING tool - it is how the list is researched,
# screened and pinned. But it needs Python on PATH, which most Skyrim players
# do not have, and "install Python first" is where an optional addon loses
# nearly everyone. PowerShell ships with Windows. This is a port, verified
# byte-for-byte against the Python unpacker's output.
#
# WHAT IT DOES, IN ONE LINE: downloads each pinned chart from Encore
# (files.enchor.us) - the public service the Bridge app uses - checks it
# against a hash baked into the list, and unpacks it into your songs folder.
#
# Bard Hero itself never connects to the internet. This is a separate program
# you chose to run.

[CmdletBinding()]
param(
    # Override the songs folder. Default matches sUserSongsFolder.
    [string]$SongsFolder,
    # List what would happen and change nothing.
    [switch]$WhatIfOnly
)

$ErrorActionPreference = "Stop"

# PowerShell 5.1 still negotiates TLS 1.0 by default on some builds, which
# enchor.us refuses. Without this the first download dies with an unhelpful
# "underlying connection was closed".
[Net.ServicePointManager]::SecurityProtocol =
    [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls11

$UA = "BardHero-GetSongs/1.0 (+Skyrim mod helper)"
$FILES = "https://files.enchor.us"

# The .sng payload is masked byte-by-byte with a position-dependent key. A
# PowerShell loop over ~300 MB of audio would take minutes; this is the same
# arithmetic at native speed, compiled by the .NET already on the machine.
Add-Type -TypeDefinition @'
public static class BardHeroSng {
    public static void Unmask(byte[] data, byte[] mask) {
        for (int i = 0; i < data.Length; i++) {
            data[i] = (byte)(data[i] ^ mask[i % 16] ^ (i & 0xFF));
        }
    }
}
'@

function Get-SafeFolderName([string]$Name) {
    # Mirrors the Python: strip only what Windows forbids, keep everything
    # else so the folder still matches the chart's own metadata.
    ($Name.ToCharArray() | Where-Object { '<>:"/\|?*' -notcontains $_ }) -join ''
}

function Expand-Sng {
    param([string]$SngPath, [string]$OutRoot)

    $fs = [IO.File]::OpenRead($SngPath)
    try {
        $br = New-Object IO.BinaryReader($fs)
        if (-not [Text.Encoding]::ASCII.GetString($br.ReadBytes(6)).Equals("SNGPKG")) {
            throw "$SngPath is not an SNG package"
        }
        $null = $br.ReadUInt32()            # format version
        $mask = $br.ReadBytes(16)

        # --- metadata: these pairs ARE the song.ini keys -------------------
        $null = $br.ReadUInt64()            # section length
        $count = $br.ReadUInt64()
        $meta = [ordered]@{}
        for ($i = 0; $i -lt [int]$count; $i++) {
            $k = [Text.Encoding]::UTF8.GetString($br.ReadBytes($br.ReadInt32()))
            $v = [Text.Encoding]::UTF8.GetString($br.ReadBytes($br.ReadInt32()))
            $meta[$k] = $v
        }

        # --- file index ---------------------------------------------------
        $null = $br.ReadUInt64()
        $fileCount = $br.ReadUInt64()
        $entries = @()
        for ($i = 0; $i -lt [int]$fileCount; $i++) {
            $nameLen = $br.ReadByte()
            $entries += [pscustomobject]@{
                Name   = [Text.Encoding]::UTF8.GetString($br.ReadBytes($nameLen))
                Length = $br.ReadUInt64()
                Offset = $br.ReadUInt64()      # absolute, from file start
            }
        }

        $artist = if ($meta["artist"]) { $meta["artist"].Trim() } else { "Unknown" }
        if (-not $artist) { $artist = "Unknown" }
        $name = if ($meta["name"]) { $meta["name"] } else { [IO.Path]::GetFileNameWithoutExtension($SngPath) }
        $dest = Join-Path $OutRoot (Get-SafeFolderName "$artist - $name".Trim())
        New-Item -ItemType Directory -Path $dest -Force | Out-Null

        foreach ($e in $entries) {
            $fs.Position = [int64]$e.Offset
            $buf = $br.ReadBytes([int]$e.Length)
            [BardHeroSng]::Unmask($buf, $mask)
            $leaf = Split-Path ($e.Name -replace '\\', '/') -Leaf
            [IO.File]::WriteAllBytes((Join-Path $dest $leaf), $buf)
        }

        # song.ini is RECONSTRUCTED from the metadata pairs, not shipped in
        # the package. LF newlines and no BOM, matching the Python exactly -
        # the byte-for-byte comparison depends on it.
        $sb = New-Object Text.StringBuilder
        [void]$sb.Append("[song]`n")
        foreach ($k in $meta.Keys) { [void]$sb.Append("$k = $($meta[$k])`n") }
        [IO.File]::WriteAllText((Join-Path $dest "song.ini"), $sb.ToString(),
            (New-Object Text.UTF8Encoding($false)))

        return [pscustomobject]@{ Dest = $dest; FileCount = $entries.Count }
    } finally { $fs.Dispose() }
}

function Set-UnlockRank {
    param([string]$SongDir, [int]$Rank)
    # Bard Hero's own ini key. UnlockLogic reads an ABSENT diff_guitar as
    # rank 1 - the easiest tier - so without this the hardest charts in the
    # list arrive unlocked from the very first rank. Clone Hero and every
    # other reader ignore an unknown key.
    if ($Rank -lt 1 -or $Rank -gt 5) { return }
    $ini = Join-Path $SongDir "song.ini"
    if (-not (Test-Path -LiteralPath $ini)) { return }
    $lines = [IO.File]::ReadAllLines($ini) |
        Where-Object { $_ -notmatch '^\s*unlock_rank\s*=' }
    $lines += "unlock_rank = $Rank"
    [IO.File]::WriteAllLines($ini, $lines, (New-Object Text.UTF8Encoding($false)))
}

# ---- resolve where songs go ---------------------------------------------
if (-not $SongsFolder) {
    # Ask Windows rather than assuming %USERPROFILE%\Documents: OneDrive
    # redirection is common and would silently install to the wrong place.
    $docs = [Environment]::GetFolderPath('MyDocuments')
    if (-not $docs) { $docs = Join-Path $env:USERPROFILE 'Documents' }
    # MUST match sUserSongsFolder's default in BardHero.ini. It lives under My
    # Games beside Skyrim's own INIs and saves. If these two ever disagree,
    # the songs land somewhere the game never looks and nothing says why.
    $SongsFolder = Join-Path $docs `
        'My Games\Skyrim Special Edition\Bard Hero Songs\guitar'
}

$manifest = Join-Path $PSScriptRoot 'recommended-songs.json'
if (-not (Test-Path -LiteralPath $manifest)) {
    throw "Cannot find recommended-songs.json next to this script."
}
$songs = @((Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json).songs)

Write-Host ""
Write-Host "  Bard Hero - curated song list" -ForegroundColor Cyan
Write-Host "  ============================="
Write-Host ""
Write-Host "  $($songs.Count) charts, downloaded from files.enchor.us - the public"
Write-Host "  service the Bridge app uses. It is a plain request for a file:"
Write-Host "  no account, no login, nothing about you or your game is sent."
Write-Host ""
Write-Host "  Bard Hero ships no songs and never connects to the internet."
Write-Host "  Each chart is checked against a hash stored in the list, so you"
Write-Host "  get exactly the chart the mod was tested against or nothing."
Write-Host ""
Write-Host "  Installing to:" -ForegroundColor Yellow
Write-Host "      $SongsFolder"
Write-Host ""
Write-Host "  Charts you already have are skipped. Nothing else is modified."
Write-Host ""

if ($WhatIfOnly) {
    foreach ($s in $songs) { Write-Host "  would install: $($s.artist) - $($s.name)" }
    return
}

Write-Host "  Press Enter to start, or close this window to cancel." -ForegroundColor Green
[void](Read-Host)

New-Item -ItemType Directory -Path $SongsFolder -Force | Out-Null
$installed = 0; $skipped = 0; $failed = 0
$wc = New-Object Net.WebClient
$wc.Headers.Add("User-Agent", $UA)

foreach ($s in $songs) {
    $label = "$($s.artist) - $($s.name)"
    $dest = Join-Path $SongsFolder (Get-SafeFolderName "$($s.artist) - $($s.name)".Trim())
    if (Test-Path -LiteralPath $dest) {
        Write-Host ("  HAVE  {0}" -f $label) -ForegroundColor DarkGray
        if ($s.unlock_rank) { Set-UnlockRank -SongDir $dest -Rank $s.unlock_rank }
        $skipped++
        continue
    }

    $suffix = if ($s.novideo) { "_novideo" } else { "" }
    $url = "$FILES/$($s.md5)$suffix.sng"
    Write-Host ("  GET   {0}" -f $label) -NoNewline
    try {
        $blob = $wc.DownloadData($url)
    } catch {
        Write-Host "  FAILED: $($_.Exception.Message)" -ForegroundColor Red
        $failed++
        continue
    }

    # The pin is the whole point: it is what guarantees you get the chart
    # that was play-tested rather than whatever is on the server today.
    $sha = ([BitConverter]::ToString(
        [Security.Cryptography.SHA256]::Create().ComputeHash($blob))
        ) -replace '-',''
    if ($s.sha256 -and $sha -ne $s.sha256.ToUpper()) {
        Write-Host "  REFUSED: hash mismatch" -ForegroundColor Red
        Write-Host "          this is not the chart Bard Hero was tested with;"
        Write-Host "          the charter has most likely re-uploaded it."
        $failed++
        continue
    }

    $tmp = Join-Path $SongsFolder ".$($s.md5).sng.part"
    try {
        [IO.File]::WriteAllBytes($tmp, $blob)
        $r = Expand-Sng -SngPath $tmp -OutRoot $SongsFolder
        if ($s.unlock_rank) { Set-UnlockRank -SongDir $r.Dest -Rank $s.unlock_rank }
        Write-Host ("  ok ({0} files)" -f $r.FileCount) -ForegroundColor Green
        $installed++
    } catch {
        Write-Host "  FAILED to unpack: $($_.Exception.Message)" -ForegroundColor Red
        $failed++
    } finally {
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force }
    }
    Start-Sleep -Milliseconds 1000   # Encore is a free community service
}

Write-Host ""
Write-Host ("  installed {0}, already had {1}, failed {2}" -f $installed, $skipped, $failed)
if ($installed -gt 0) {
    Write-Host "  Start Skyrim and open the Songbook - Bard Hero picks up new" -ForegroundColor Green
    Write-Host "  songs by itself, with no restart and nothing to press."
}
Write-Host ""
