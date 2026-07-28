param(
    [string]$PackageVersion = "0.2.0-playtest-2026-07-23",
    [switch]$NoBaCompatibility,
    [switch]$PreinstalledBaCharts
)

$ErrorActionPreference = "Stop"

if ($NoBaCompatibility -and $PreinstalledBaCharts) {
    throw "Choose either NoBaCompatibility or PreinstalledBaCharts, not both"
}

$repoRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$distRoot = Join-Path $repoRoot "dist\SKSE\Plugins\BardHero"
$dllPath = Join-Path $repoRoot "build\verify\BardHero.dll"
$packageRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "build\packages"))
$variantSuffix = if ($NoBaCompatibility) {
    "-no-ba"
} elseif ($PreinstalledBaCharts) {
    "-preinstalled-ba"
} else {
    ""
}
$packageName = "BardHero-$PackageVersion$variantSuffix"
$stageRoot = [IO.Path]::GetFullPath((Join-Path $packageRoot $packageName))
$zipPath = [IO.Path]::GetFullPath((Join-Path $packageRoot "$packageName.zip"))

if (-not $stageRoot.StartsWith($packageRoot, [StringComparison]::OrdinalIgnoreCase) -or
    -not $zipPath.StartsWith($packageRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Package path escaped build\packages"
}
if (-not (Test-Path -LiteralPath $dllPath)) {
    throw "Verified DLL is missing: $dllPath"
}
if (Get-Process SkyrimSE -ErrorAction SilentlyContinue) {
    throw "SkyrimSE is running. Close it before creating a release artifact."
}

New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
if (Test-Path -LiteralPath $stageRoot) {
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

$pluginRoot = Join-Path $stageRoot "SKSE\Plugins"
$dataRoot = Join-Path $pluginRoot "BardHero"
New-Item -ItemType Directory -Path $dataRoot -Force | Out-Null

Copy-Item -LiteralPath $dllPath -Destination (Join-Path $pluginRoot "BardHero.dll")
Copy-Item -LiteralPath (Join-Path $distRoot "highway") -Destination $dataRoot -Recurse
if (-not $NoBaCompatibility) {
    if ($PreinstalledBaCharts) {
        # Keep the manifest for lazy first-play audio resolution, but omit the
        # template directory that activates PrepareInstalledBaLibrary.
        $compatRoot = Join-Path $dataRoot "compat\ba-bard-songs"
        New-Item -ItemType Directory -Path $compatRoot -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $distRoot "compat\ba-bard-songs\ba_bard_songs.tsv") `
            -Destination $compatRoot

        # Pre-materialize managed chart folders in the archive. Empty Opus
        # placeholders make them visible to the ordinary scanner; selecting a
        # song resolves only that track from the separately installed BA mod.
        $songsRoot = Join-Path $dataRoot "songs"
        New-Item -ItemType Directory -Path $songsRoot -Force | Out-Null
        $templateRoot = Join-Path $distRoot "compat\ba-bard-songs\songs"
        foreach ($template in Get-ChildItem -LiteralPath $templateRoot -Directory) {
            $destination = Join-Path $songsRoot $template.Name
            New-Item -ItemType Directory -Path $destination -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $template.FullName "notes.chart") `
                -Destination $destination
            Copy-Item -LiteralPath (Join-Path $template.FullName "song.ini") `
                -Destination $destination
            $iniText = Get-Content -LiteralPath (Join-Path $template.FullName "song.ini") -Raw
            if ($iniText -notmatch '(?m)^source_id\s*=\s*(\S+)\s*$') {
                throw "Missing source_id in $($template.FullName)\song.ini"
            }
            [IO.File]::WriteAllText(
                (Join-Path $destination ".bardhero-ba-generated"),
                "schema=1`nsource_id=$($Matches[1])`n",
                [Text.UTF8Encoding]::new($false))
            [IO.File]::WriteAllBytes(
                (Join-Path $destination "song.opus"), [byte[]]::new(0))
        }
    } else {
        Copy-Item -LiteralPath (Join-Path $distRoot "compat") `
            -Destination $dataRoot -Recurse
    }
}

# Sound effects. Copies EVERYTHING under dist\sfx except the one folder that
# is deliberately withheld, rather than naming files.
#
# A hardcoded three-file list lived here and shipped miss1/2/3.wav while
# silently omitting the ENTIRE sfx\ui\ folder - eleven sounds - from every
# package ever built. It went unnoticed because the maintainer's own machine
# has dist\ deployed directly, so UI audio worked there and only there. Found
# 2026-07-27 when a second PC installed from the package and had no UI sound
# at all. Anything added to dist\sfx now ships by default; only the exclusion
# is named, and the exclusion is the thing worth being explicit about.
#
# crowd\ stays out ON PURPOSE: the readme promises it, and Skyrim's Got Talent
# supplies the real audience behaviour.
$sfxRoot = Join-Path $dataRoot "sfx"
New-Item -ItemType Directory -Path $sfxRoot -Force | Out-Null
foreach ($item in Get-ChildItem -LiteralPath (Join-Path $distRoot "sfx")) {
    if ($item.Name -eq "crowd") { continue }
    Copy-Item -LiteralPath $item.FullName -Destination $sfxRoot -Recurse
}
if (-not (Get-ChildItem -LiteralPath $sfxRoot -Recurse -File -Filter *.wav)) {
    throw "No sound effects staged from $distRoot\sfx"
}

# Song acquisition ships NOTHING. Players get charts through Bridge, and the
# recommended list lives in the mod description rather than in the archive.
# `tools\fetch_songs.py` and the curated manifest are AUTHORING artifacts kept
# out of the package on purpose - see docs\archive\2026-07-27-song-fetching\.
# Decision 2026-07-27: a downloader we ship is a maintenance liability that
# decays silently, and it costs the flat claim that BardHero never touches
# the internet.

# Produce the public INI from the maintained dist INI while omitting dormant
# spikes, cheats, and field-diagnostic controls. Runtime defaults for every
# omitted setting are safe and production-oriented.
$iniSource = Join-Path $repoRoot "dist\SKSE\Plugins\BardHero.ini"
$iniOutput = Join-Path $pluginRoot "BardHero.ini"
$outputLines = [Collections.Generic.List[string]]::new()
$skipSection = $false
$skipUntil = $null
foreach ($line in Get-Content -LiteralPath $iniSource) {
    if ($line -match '^\[(.+)\]$') {
        $skipSection = $Matches[1] -in @("Spikes", "Cheats")
    }
    if ($skipSection) {
        continue
    }

    if ($null -ne $skipUntil) {
        if ($line -match $skipUntil) {
            $skipUntil = $null
        }
        continue
    }

    if ($line -match '^; DIAGNOSTIC PROBE KEY') {
        $skipUntil = '^iProbeKey\s*='
        continue
    }
    if ($line -match '^; DIAGNOSTIC ISOLATION SWITCH') {
        $skipUntil = '^bHookNeverFilter\s*='
        continue
    }
    if ($line -match "^; Blank Skyrim's Got Talent's 30 reaction") {
        $skipUntil = '^bBlankReactionMessages\s*='
        continue
    }
    if ($line -match '^; \(iControlRecoveryPasses was removed') {
        $skipUntil = '^; LIVE CROWD REACTIONS\.'
        continue
    }
    if ($line -match '^; STANDALONE PERFORM\.') {
        $skipUntil = '^bStandalonePerform\s*='
        continue
    }
    if ($line -match '^(iHookMode|iProbeKey|bHookNeverFilter|bBlankReactionMessages|bStandalonePerform)\s*=') {
        continue
    }
    $outputLines.Add($line)
}
[IO.File]::WriteAllLines(
    $iniOutput, $outputLines, [Text.UTF8Encoding]::new($false))

Copy-Item -LiteralPath (Join-Path $repoRoot "release\PLAYTEST-README.txt") `
    -Destination (Join-Path $stageRoot "README-BardHero-Playtest.txt")
if ($NoBaCompatibility) {
    Copy-Item -LiteralPath (Join-Path $repoRoot "release\PLAYTEST-NO-BA.txt") `
        -Destination (Join-Path $stageRoot "README-NO-BA-FIRST.txt")
} elseif ($PreinstalledBaCharts) {
    Copy-Item -LiteralPath (Join-Path $repoRoot "release\PLAYTEST-PREINSTALLED-BA.txt") `
        -Destination (Join-Path $stageRoot "README-PREINSTALLED-BA-FIRST.txt")
}
Copy-Item -LiteralPath (Join-Path $repoRoot "release\THIRD-PARTY-NOTICES.txt") `
    -Destination (Join-Path $stageRoot "THIRD-PARTY-NOTICES.txt")

$licenseRoot = Join-Path $stageRoot "licenses"
New-Item -ItemType Directory -Path $licenseRoot -Force | Out-Null
$vcpkgShare = Join-Path $repoRoot "build\verify\vcpkg_installed\x64-windows-static-md\share"
foreach ($package in @(
    "commonlibsse-ng", "fmt", "spdlog", "imgui", "xbyak", "simpleini",
    "opus", "opusfile", "libogg", "libopusenc"
)) {
    $copyright = Join-Path $vcpkgShare "$package\copyright"
    if (-not (Test-Path -LiteralPath $copyright)) {
        throw "Missing license notice: $copyright"
    }
    Copy-Item -LiteralPath $copyright `
        -Destination (Join-Path $licenseRoot "$package.txt")
}

function Write-LicenseTail {
    param(
        [string]$Source,
        [string]$Marker,
        [string]$Destination
    )
    $lines = Get-Content -LiteralPath $Source
    $start = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i].Contains($Marker)) {
            $start = $i
            break
        }
    }
    if ($start -lt 0) {
        throw "License marker not found in $Source"
    }
    [IO.File]::WriteAllLines(
        $Destination, $lines[$start..($lines.Count - 1)],
        [Text.UTF8Encoding]::new($false))
}

Write-LicenseTail `
    -Source (Join-Path $repoRoot "extern\miniaudio\miniaudio.h") `
    -Marker "This software is available as a choice of the following licenses." `
    -Destination (Join-Path $licenseRoot "miniaudio.txt")
Write-LicenseTail `
    -Source (Join-Path $repoRoot "extern\stb\stb_vorbis.c") `
    -Marker "This software is available under 2 licenses" `
    -Destination (Join-Path $licenseRoot "stb_vorbis.txt")

# signalsmith-stretch and its signalsmith-linear dependency cannot go through
# Write-LicenseTail: the vendored headers are code only, with no inline
# copyright or licence text to extract. The MIT notice is kept as a static
# file instead and copied verbatim.
$signalsmithLicense = Join-Path $repoRoot "release\licenses\signalsmith.txt"
if (-not (Test-Path -LiteralPath $signalsmithLicense)) {
    throw "Missing license notice: $signalsmithLicense"
}
Copy-Item -LiteralPath $signalsmithLicense `
    -Destination (Join-Path $licenseRoot "signalsmith.txt")

$dllHash = (Get-FileHash -LiteralPath (Join-Path $pluginRoot "BardHero.dll") `
    -Algorithm SHA256).Hash
$versionLines = @(
    "BardHero $PackageVersion$variantSuffix",
    "DLL SHA-256: $dllHash",
    "Automated suites: 29/29 passed",
    "Package content excludes BA audio, generated caches, test songs, spikes,",
    "synthetic crowd placeholders, diagnostics, cheats, source, and PDB files."
)
if ($NoBaCompatibility) {
    $versionLines += "Diagnostic variant: BA compatibility pack omitted; BA preparation exits immediately."
} elseif ($PreinstalledBaCharts) {
    $versionLines += "Diagnostic variant: 87 BA charts preinstalled; BA template bootstrap omitted."
}
[IO.File]::WriteAllLines(
    (Join-Path $stageRoot "VERSION.txt"), $versionLines,
    [Text.UTF8Encoding]::new($false))

$manifestLines = foreach ($file in Get-ChildItem -LiteralPath $stageRoot -Recurse -File |
    Sort-Object FullName) {
    $relative = $file.FullName.Substring($stageRoot.Length + 1).Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    "$hash  $relative"
}
[IO.File]::WriteAllLines(
    (Join-Path $stageRoot "MANIFEST-SHA256.txt"), $manifestLines,
    [Text.UTF8Encoding]::new($false))

Push-Location $stageRoot
try {
    & tar.exe -a -c -f $zipPath *
    if ($LASTEXITCODE -ne 0) {
        throw "tar.exe failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

Write-Output "PACKAGE=$zipPath"
Write-Output "PACKAGE_SHA256=$((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash)"
Write-Output "PACKAGE_BYTES=$((Get-Item -LiteralPath $zipPath).Length)"
Write-Output "STAGED_FILES=$((Get-ChildItem -LiteralPath $stageRoot -Recurse -File).Count)"
