# Builds the FOMOD archive for "Bard Hero - The Roadie".
#
# The Roadie is a TOOL, not a mod. It installs no game files, and this
# packager exists so the installer can explain what the tool does BEFORE
# anybody runs it - a downloader that explains itself afterwards is a
# downloader nobody should trust.
#
# It ships SEPARATELY from Bard Hero on purpose. Folding it into the main
# FOMOD, even as an unticked option, would mean the mod archive contains a
# downloader - and "Bard Hero never touches the internet" stops being a flat,
# checkable claim and becomes a caveat to re-argue in every comment thread.
#
# Archive layout:
#     fomod\        ModuleConfig.xml + info.xml
#     Images\       the installer banner
#     tool\         everything that lands on disk, under one named folder
#     README.txt    at the archive root, never installed

param(
    [string]$PackageVersion = "1.0"
)

$ErrorActionPreference = "Stop"

$repoRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$srcRoot = Join-Path $repoRoot "addon-roadie"
$packageRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "build\packages"))
$stageRoot = Join-Path $packageRoot "BardHero-The-Roadie-$PackageVersion"
$zipPath = Join-Path $packageRoot "BardHero-The-Roadie-$PackageVersion.zip"

foreach ($p in @($stageRoot, $zipPath)) {
    if (-not ([IO.Path]::GetFullPath($p)).StartsWith($packageRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package path escaped build\packages: $p"
    }
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
}
New-Item -ItemType Directory -Path $stageRoot -Force | Out-Null

# ---- tool\ : the only thing that reaches disk --------------------------
$toolRoot = Join-Path $stageRoot "tool"
New-Item -ItemType Directory -Path $toolRoot -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $srcRoot "Run The Roadie.bat") -Destination $toolRoot
Copy-Item -LiteralPath (Join-Path $srcRoot "songfetch") -Destination $toolRoot -Recurse
Copy-Item -LiteralPath (Join-Path $srcRoot "README.txt") -Destination $toolRoot

# The manifest is the whole product. An entry with no sha256 is one nobody
# verified, and the tool would have nothing to check the download against.
$manifest = Join-Path $toolRoot "songfetch\recommended-songs.json"
$songs = @((Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json).songs)
if ($songs.Count -lt 1) { throw "The manifest lists no songs" }
foreach ($s in $songs) {
    if (-not $s.sha256) {
        throw "Song '$($s.artist) - $($s.name)' has no sha256 pin"
    }
}

# ---- docs at the archive root, never installed -------------------------
Copy-Item -LiteralPath (Join-Path $srcRoot "README.txt") -Destination $stageRoot

# ---- the installer -----------------------------------------------------
Copy-Item -LiteralPath (Join-Path $srcRoot "fomod") -Destination $stageRoot -Recurse
$imagesRoot = Join-Path $stageRoot "Images"
New-Item -ItemType Directory -Path $imagesRoot -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot "fomod\banner.jpg") `
    -Destination (Join-Path $imagesRoot "roadie.jpg")

# Stamp the staged installer metadata with the real package version (same
# fix as package_fomod.ps1 - the source info.xml value is a fallback).
$infoPath = Join-Path $stageRoot "fomod\info.xml"
$infoXml = Get-Content -LiteralPath $infoPath -Raw
$infoXml = $infoXml -replace '<Version>[^<]*</Version>', "<Version>$PackageVersion</Version>"
Set-Content -LiteralPath $infoPath -Value $infoXml -Encoding UTF8 -NoNewline

# Every path ModuleConfig.xml names must exist, or the installer fails on the
# user's machine with a message they cannot act on.
$config = [xml](Get-Content -LiteralPath (Join-Path $stageRoot "fomod\ModuleConfig.xml") -Raw)
$referenced = @($config.config.moduleImage.path)
$referenced += $config.SelectNodes("//folder") | ForEach-Object { $_.source }
$referenced += $config.SelectNodes("//file")   | ForEach-Object { $_.source }
$referenced += $config.SelectNodes("//image")  | ForEach-Object { $_.path }
foreach ($rel in ($referenced | Where-Object { $_ } | Sort-Object -Unique)) {
    if (-not (Test-Path -LiteralPath (Join-Path $stageRoot $rel))) {
        throw "ModuleConfig.xml references a missing path: $rel"
    }
}

Push-Location $stageRoot
try {
    & tar.exe -a -c -f $zipPath *
    if ($LASTEXITCODE -ne 0) { throw "tar.exe failed with exit code $LASTEXITCODE" }
} finally { Pop-Location }

Write-Output "PACKAGE=$zipPath"
Write-Output "PACKAGE_SHA256=$((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash)"
Write-Output "PACKAGE_BYTES=$((Get-Item -LiteralPath $zipPath).Length)"
Write-Output "SONGS=$($songs.Count)"
