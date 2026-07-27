# Builds the FOMOD-installer archive for Bard Hero.
#
# WHY THIS DELEGATES INSTEAD OF DUPLICATING. package_playtest.ps1 already
# knows how to produce a correct flat payload: it refuses to run while Skyrim
# is open, insists on a verified DLL, filters the public INI down from the
# maintained one (dropping spikes, cheats and diagnostics), and collects every
# vendored license. Copying ~150 lines of that here would guarantee the two
# drift. So this RUNS it, then rearranges its staged output into the layout
# ModuleConfig.xml expects.
#
# Layout produced (mirrors Fitting Room's):
#
#     fomod\        ModuleConfig.xml + info.xml
#     Images\       the installer banner
#     core\         required files - the whole mod except the Doom Lute
#     optional\     the Doom Lute: its plugin, meshes, textures, icon rule
#     *.txt         readme + notices at the ROOT, never installed to Data
#
# Docs stay at the archive root ON PURPOSE. Flat docs installed to the Data
# root collide across mods and read as overwrite conflicts in MO2.

param(
    [string]$PackageVersion = "0.2.0",
    [string]$ElectricRoot = "C:\Studios\Mod Studio\BardHero Electric\dist"
)

$ErrorActionPreference = "Stop"

$repoRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$packageRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "build\packages"))
$flatName = "BardHero-fomod-src-$PackageVersion"
$flatRoot = Join-Path $packageRoot $flatName
$stageRoot = Join-Path $packageRoot "BardHero-$PackageVersion-FOMOD"
$zipPath = Join-Path $packageRoot "BardHero-$PackageVersion-FOMOD.zip"

foreach ($p in @($stageRoot, $zipPath)) {
    if (-not ([IO.Path]::GetFullPath($p)).StartsWith($packageRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package path escaped build\packages: $p"
    }
}

# ---- 1. let the playtest packager build and validate the flat payload -----
& (Join-Path $PSScriptRoot "package_playtest.ps1") -PackageVersion "fomod-src-$PackageVersion" | Out-Null
if (-not (Test-Path -LiteralPath $flatRoot)) {
    throw "package_playtest.ps1 did not produce $flatRoot"
}

foreach ($p in @($stageRoot, $zipPath)) {
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
}
New-Item -ItemType Directory -Path $stageRoot -Force | Out-Null

# ---- 2. core: everything the flat payload installs to Data ---------------
$coreRoot = Join-Path $stageRoot "core"
New-Item -ItemType Directory -Path $coreRoot -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $flatRoot "SKSE") -Destination $coreRoot -Recurse

# Docs and licences ride at the archive root, not inside core\.
foreach ($doc in Get-ChildItem -LiteralPath $flatRoot -File) {
    Copy-Item -LiteralPath $doc.FullName -Destination $stageRoot
}
$flatLicenses = Join-Path $flatRoot "licenses"
if (Test-Path -LiteralPath $flatLicenses) {
    Copy-Item -LiteralPath $flatLicenses -Destination $stageRoot -Recurse
}

# ---- 3. optional: the Doom Lute -----------------------------------------
# The ONE component that adds a plugin. Sourced from the Electric repo, which
# stays the home of the guitar's art and animations.
$luteRoot = Join-Path $stageRoot "optional\doomlute"
New-Item -ItemType Directory -Path $luteRoot -Force | Out-Null

$esp = Join-Path $ElectricRoot "Data\Bard Hero - Doom Lute.esp"
if (-not (Test-Path -LiteralPath $esp)) {
    throw ("Doom Lute plugin missing: $esp`n" +
        "If it is still called 'Bard Hero - Guitar Addon.esp', the 2026-07-27 " +
        "rename was not applied to the Electric repo.")
}
Copy-Item -LiteralPath $esp -Destination $luteRoot

foreach ($dir in @("meshes", "textures")) {
    Copy-Item -LiteralPath (Join-Path $ElectricRoot "Data\$dir") `
        -Destination $luteRoot -Recurse
}

# InventoryInjector gives the lute its inventory icon and "Instrument" type.
# It is scoped to the optional component because its rules reference the
# plugin by name - shipping it in core would log a failed lookup for anyone
# who declined the guitar.
$injDst = Join-Path $luteRoot "SKSE\Plugins\InventoryInjector"
New-Item -ItemType Directory -Path $injDst -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $ElectricRoot "Data\SKSE\Plugins\InventoryInjector\Bard Hero - Doom Lute.json") `
    -Destination $injDst

# Guard the rename at package time: a stale plugin name in any of these is a
# silent failure in game (OAR conditions simply never match, and the icon rule
# never fires), so it must never reach an archive.
$stale = Get-ChildItem -LiteralPath $luteRoot -Recurse -File -Include *.json |
    Select-String -Pattern 'Bard Hero - Guitar Addon\.esp' -SimpleMatch
if ($stale) {
    throw ("Stale plugin name in: " +
        (($stale | ForEach-Object { $_.Path }) -join ", "))
}

# ---- 4. the installer itself --------------------------------------------
Copy-Item -LiteralPath (Join-Path $repoRoot "fomod") -Destination $stageRoot -Recurse
$imagesRoot = Join-Path $stageRoot "Images"
New-Item -ItemType Directory -Path $imagesRoot -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot "fomod\banner.jpg") `
    -Destination (Join-Path $imagesRoot "bardhero.jpg")
# The banner lives in fomod\ as the maintained source; only Images\ is what
# ModuleConfig.xml points at.
Remove-Item -LiteralPath (Join-Path $stageRoot "fomod\banner.jpg") -Force

# Stamp the staged installer metadata with the real package version - a
# hardcoded info.xml shipped 0.2.2 zips that told installers "0.2.0"
# (field screenshot, 2026-07-27). The source file's value is a fallback.
$infoPath = Join-Path $stageRoot "fomod\info.xml"
$infoXml = Get-Content -LiteralPath $infoPath -Raw
$infoXml = $infoXml -replace '<Version>[^<]*</Version>', "<Version>$PackageVersion</Version>"
Set-Content -LiteralPath $infoPath -Value $infoXml -Encoding UTF8 -NoNewline

# Every path ModuleConfig.xml references must actually exist, or the installer
# fails on the user's machine with a message they cannot act on.
$config = [xml](Get-Content -LiteralPath (Join-Path $stageRoot "fomod\ModuleConfig.xml") -Raw)
$referenced = @()
$referenced += $config.config.moduleImage.path
$referenced += $config.SelectNodes("//folder") | ForEach-Object { $_.source }
$referenced += $config.SelectNodes("//file") | ForEach-Object { $_.source }
$referenced += $config.SelectNodes("//image") | ForEach-Object { $_.path }
foreach ($rel in ($referenced | Where-Object { $_ } | Sort-Object -Unique)) {
    if (-not (Test-Path -LiteralPath (Join-Path $stageRoot $rel))) {
        throw "ModuleConfig.xml references a missing path: $rel"
    }
}

# ---- 4a. content the installer is useless without ------------------------
# Every one of these has shipped missing at least once, and each failed
# SILENTLY in game: no error, no log line, just a feature that quietly is not
# there. Cheap to assert, expensive to discover on somebody else's PC.
$mustExist = @{
    "core\SKSE\Plugins\BardHero.dll"            = "the plugin"
    "core\SKSE\Plugins\BardHero.ini"            = "the settings file"
    "core\SKSE\Plugins\BardHero\highway"        = "the highway atlas"
    "core\SKSE\Plugins\BardHero\sfx\ui"         = "UI sounds (omitted from every package until 2026-07-27)"
    "optional\doomlute\Bard Hero - Doom Lute.esp" = "the Doom Lute plugin"
}
foreach ($rel in $mustExist.Keys) {
    if (-not (Test-Path -LiteralPath (Join-Path $stageRoot $rel))) {
        throw "Package is missing $($mustExist[$rel]): $rel"
    }
}
$uiSounds = (Get-ChildItem -LiteralPath (Join-Path $stageRoot "core\SKSE\Plugins\BardHero\sfx\ui") -File -Filter *.wav).Count
if ($uiSounds -lt 1) { throw "sfx\ui exists but contains no sounds" }
Write-Output "  staged $uiSounds UI sounds"

# ---- 4b. rewrite VERSION.txt --------------------------------------------
# The inner packager was handed a synthetic version ("fomod-src-...") so its
# staging directory would not collide with a real release, and that string
# leaks into the VERSION.txt it writes. Rewrite it here with the real one.
#
# The suite count is DERIVED, never typed. package_playtest.ps1 hardcodes
# "29/29" and it had silently fallen nine suites behind by 2026-07-27 - the
# same hand-maintained-roll-call failure that build.bat was rewritten to kill.
# Reading test.bat and test.log means it cannot go stale without the numbers
# visibly disagreeing.
$testBat = Get-Content -LiteralPath (Join-Path $PSScriptRoot "test.bat") -Raw
$registered = ([regex]::Matches($testBat, 'build\\release\\(\w+)\.exe')).Count
$suiteLine = "Automated suites: unknown (no test.log - run tools\test.bat)"
$testLogPath = Join-Path $PSScriptRoot "test.log"
if (Test-Path -LiteralPath $testLogPath) {
    $testLog = Get-Content -LiteralPath $testLogPath -Raw
    $passed = ([regex]::Matches($testLog, 'all \w+ tests passed|Tests PASS')).Count
    $done = ([regex]::Matches($testLog, '=== ALL_DONE ===')).Count
    $suiteLine = if ($passed -eq $registered -and $done -eq 1) {
        "Automated suites: $passed/$registered passed"
    } else {
        "Automated suites: $passed/$registered passed - INCOMPLETE RUN, do not ship"
    }
}

$dllHash = (Get-FileHash -LiteralPath (Join-Path $coreRoot "SKSE\Plugins\BardHero.dll") `
    -Algorithm SHA256).Hash
[IO.File]::WriteAllLines(
    (Join-Path $stageRoot "VERSION.txt"),
    @(
        "Bard Hero $PackageVersion (FOMOD installer)",
        "DLL SHA-256: $dllHash",
        $suiteLine,
        "",
        "Installs with any FOMOD-aware mod manager. The base mod adds NO plugin;",
        "the optional Doom Lute adds exactly one, Bard Hero - Doom Lute.esp.",
        "",
        "Package content excludes BA audio, generated caches, test songs, spikes,",
        "synthetic crowd placeholders, diagnostics, cheats, source, and PDB files."
    ),
    [Text.UTF8Encoding]::new($false))

# MANIFEST-SHA256.txt came from the inner packager and describes the FLAT
# payload, so every path in it is wrong for this layout. Regenerate it.
$manifestLines = foreach ($file in Get-ChildItem -LiteralPath $stageRoot -Recurse -File |
    Where-Object { $_.Name -ne "MANIFEST-SHA256.txt" } | Sort-Object FullName) {
    $relative = $file.FullName.Substring($stageRoot.Length + 1).Replace('\', '/')
    "$((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)  $relative"
}
[IO.File]::WriteAllLines(
    (Join-Path $stageRoot "MANIFEST-SHA256.txt"), $manifestLines,
    [Text.UTF8Encoding]::new($false))

# ---- 5. zip -------------------------------------------------------------
Push-Location $stageRoot
try {
    & tar.exe -a -c -f $zipPath *
    if ($LASTEXITCODE -ne 0) { throw "tar.exe failed with exit code $LASTEXITCODE" }
} finally { Pop-Location }

Remove-Item -LiteralPath $flatRoot -Recurse -Force
Remove-Item -LiteralPath (Join-Path $packageRoot "$flatName.zip") -Force -ErrorAction SilentlyContinue

Write-Output "PACKAGE=$zipPath"
Write-Output "PACKAGE_SHA256=$((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash)"
Write-Output "PACKAGE_BYTES=$((Get-Item -LiteralPath $zipPath).Length)"
Write-Output "STAGED_FILES=$((Get-ChildItem -LiteralPath $stageRoot -Recurse -File).Count)"
