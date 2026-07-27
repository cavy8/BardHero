# tools/make-short-test-song.ps1
#
# Builds a very short playable song so a bisect/field run costs seconds
# instead of a full 81s track. The post-session control lock reproduces on
# the COMPLETED-song exit path only, so the song just has to END - length
# is pure overhead.
#
# Source material is an existing charted song; audio is trimmed with ffmpeg
# and the chart is truncated to the same window on every difficulty.
#
# Writes straight into the DEPLOYED mod folder. copy_directory in the CMake
# POST_BUILD only adds/overwrites, it never deletes, so the song survives
# every rebuild - including the pre-rename bisect builds, as long as
# sSongsFolder keeps pointing at the BardHero songs tree.

param(
    [string]$SourceSong = 'Become a Bard - Secunda',
    [string]$Name       = 'AA Short Test',
    [double]$Seconds    = 10.0,
    [string]$SongsRoot  = 'C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods\BardHero\SKSE\Plugins\BardHero\songs',
    [string]$Ffmpeg     = 'C:\Program Files\ImageMagick-7.1.1-Q16-HDRI\ffmpeg.exe'
)

$ErrorActionPreference = 'Stop'

# Set-Content -Encoding utf8 emits a BOM on PowerShell 5.1, and a BOM in
# front of "[song]" / "[Song]" stops the section header being recognised -
# the song still scans (0 rejected) but comes out nameless in the browser.
function Write-PlainText([string]$Path, [string[]]$Lines) {
    [System.IO.File]::WriteAllLines($Path, $Lines,
        (New-Object System.Text.UTF8Encoding($false)))
}

$src = Join-Path $SongsRoot $SourceSong
$dst = Join-Path $SongsRoot $Name
if (-not (Test-Path $src)) { throw "source song not found: $src" }
if (-not (Test-Path $Ffmpeg)) { throw "ffmpeg not found: $Ffmpeg" }
New-Item -ItemType Directory -Force $dst | Out-Null

# --- audio ------------------------------------------------------------
# Re-encode rather than stream-copy: a copy cuts on the nearest page
# boundary, and the exact end time is the thing under test here.
& $Ffmpeg -y -hide_banner -loglevel error -i (Join-Path $src 'song.ogg') `
    -t $Seconds -c:a libvorbis -q:a 4 (Join-Path $dst 'song.ogg')
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed ($LASTEXITCODE)" }

# --- chart ------------------------------------------------------------
# Keep every note whose tick lands inside the trimmed window, minus a
# small tail so the last note is not clipped by the audio cut.
$chart      = Get-Content (Join-Path $src 'notes.chart')
$resolution = [int](($chart | Select-String -Pattern 'Resolution\s*=\s*(\d+)').Matches[0].Groups[1].Value)
$bpm        = [double](($chart | Select-String -Pattern '^\s*0\s*=\s*B\s+(\d+)').Matches[0].Groups[1].Value) / 1000.0
$ticksPerSec = $resolution * $bpm / 60.0
$maxTick     = [int](($Seconds - 1.5) * $ticksPerSec)

$out       = New-Object System.Collections.Generic.List[string]
$inNotes   = $false
foreach ($line in $chart) {
    if ($line -match '^\[(\w+)\]') {
        $inNotes = $Matches[1] -match 'Single$'
        $out.Add($line); continue
    }
    if ($inNotes -and $line -match '^\s*(\d+)\s*=\s*[NSE]\s') {
        if ([int]$Matches[1] -le $maxTick) { $out.Add($line) }
        continue
    }
    if ($line -match '^\s*MusicStream') { $out.Add('  MusicStream = "song.ogg"'); continue }
    if ($line -match '^\s*Name\s*=')    { $out.Add("  Name = `"$Name`"");        continue }
    $out.Add($line)
}
Write-PlainText (Join-Path $dst 'notes.chart') $out

# --- song.ini ---------------------------------------------------------
$lengthMs = [int]($Seconds * 1000)
Write-PlainText (Join-Path $dst 'song.ini') @(
    '[song]'
    "name = $Name"
    'artist = BardHero'
    'charter = bisect harness'
    "song_length = $lengthMs"
    'single_instrument = 1'
)

$notes = ($out | Select-String -Pattern '^\s*\d+\s*=\s*N\s').Count
Write-Output "created '$Name': ${Seconds}s, $notes notes (all difficulties), maxTick=$maxTick"
Get-ChildItem $dst | Select-Object Name, Length | Format-Table -AutoSize
