# Monolith Release Zip Builder
# Creates a release zip with "Installed": true for Blueprint-only compatibility.
# Automatically builds with optional dependencies disabled (MONOLITH_RELEASE_BUILD=1).
#
# Usage: powershell -ExecutionPolicy Bypass -File Scripts\make_release.ps1 -Version "0.10.0"
#
# What it does:
#   1. Sets MONOLITH_RELEASE_BUILD=1 (forces BA/GBA optional deps OFF in Build.cs)
#   2. Runs UBT to produce clean release binaries
#   3. Packages tracked files + binaries into a zip with Installed=true
#   4. Strips non-redistributable modules (MonolithSteamBridge) from
#      source, binaries, and the uplugin module list
#   5. Unsets env var (your next dev build auto-detects deps normally)
#
# Source users (GitHub clones) are unaffected — Build.cs auto-detects at compile time.
#
# Non-redistributable modules:
#   - MonolithSteamBridge: solo-dev only, Steam Integration Kit bridge (not public)
#
# Note: MonolithISX was extracted to a sibling plugin at Plugins/MonolithISX/ on
# 2026-04-21 — it no longer lives in this repo, so release packaging does not need
# to strip it here.

param(
    [Parameter(Mandatory=$true)]
    [string]$Version,
    [switch]$SkipBuild,
    # Allow releasing with a dirty working tree. DANGEROUS: WIP modifications to tracked
    # files end up in the zip because this script copies the working-tree content, not the
    # committed HEAD. Only use if you know exactly what dirty files you're shipping.
    [switch]$AllowDirtyTree
)

$ErrorActionPreference = "Stop"

# --- Step 0: Refuse to release a dirty working tree ---
# This script copies tracked files from the working tree (not from HEAD), so any
# uncommitted modification to a tracked file silently ends up in the release zip.
# Bitten by this shipping v0.13.1 with WIP CommonUI refs in MonolithUI. Never again.
$PluginDir = Split-Path -Parent $PSScriptRoot
Push-Location $PluginDir
try {
    # Porcelain is empty iff working tree and index both match HEAD (no modified, staged,
    # or untracked files). Ignores files under .gitignore. Perfect clean gate.
    $dirty = git status --porcelain
    if ($dirty -and -not $AllowDirtyTree) {
        Pop-Location
        Write-Host "`n  [FAIL] Working tree is dirty. Refusing to release." -ForegroundColor Red
        Write-Host "`n  Offending files:" -ForegroundColor Red
        $dirty | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
        Write-Host "`n  Options:" -ForegroundColor Cyan
        Write-Host "    - Commit or stash the changes, then re-run" -ForegroundColor Cyan
        Write-Host "    - Re-run with -AllowDirtyTree if you REALLY know what you're shipping" -ForegroundColor Cyan
        exit 1
    }
    if ($dirty -and $AllowDirtyTree) {
        Write-Host "`n  [WARN] -AllowDirtyTree set. Working tree has uncommitted changes:" -ForegroundColor Yellow
        $dirty | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
        Write-Host "  These WILL be in the release zip." -ForegroundColor Yellow
    }
}
finally {
    Pop-Location
}

$ProjectDir = Split-Path -Parent (Split-Path -Parent $PluginDir)

# --- $StrippedModules: defense-in-depth against accidental sibling-plugin re-merge ---
# Sibling plugins (MonolithSteamBridge, MonolithISX, future siblings) live OUTSIDE
# Plugins/Monolith/ at the project's Plugins/ level. They are naturally excluded from
# the release zip by `git ls-files` scope (which only sees files inside Plugins/Monolith/).
# This array exists purely as defense-in-depth: if someone ever accidentally re-merges
# sibling source back into Plugins/Monolith/Source/ (refactor mistake, copy-paste, etc.),
# the strip filter catches it before it ships.
#
# Auto-discover all "Monolith*" sibling folders alongside Plugins/Monolith/ — every new
# sibling gets protected automatically without script maintenance. Excludes Monolith
# itself.
$ProjectPluginsDir = Join-Path $ProjectDir "Plugins"
$StrippedModules = @(Get-ChildItem -Path $ProjectPluginsDir -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like "Monolith*" -and $_.Name -ne "Monolith" } |
    Select-Object -ExpandProperty Name)
if ($StrippedModules.Count -gt 0) {
    Write-Host "  [strip-list] Auto-discovered $($StrippedModules.Count) sibling plugin(s) to defend against: $($StrippedModules -join ', ')" -ForegroundColor DarkGray
}
$OutputZip = Join-Path $ProjectDir "Monolith-v$Version.zip"
$TempDir = Join-Path $env:TEMP "Monolith_Release_$Version"
$UBT = 'C:\Program Files (x86)\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe'
$UProject = Join-Path $ProjectDir "Leviathan.uproject"

Write-Host "Building Monolith v$Version release zip..." -ForegroundColor Cyan

# --- Step 1: Build with optional deps disabled ---
if (-not $SkipBuild) {
    Write-Host "`n  [1/4] Building release binaries (optional deps OFF)..." -ForegroundColor Yellow

    # Set env var so Build.cs files skip optional dependency detection
    $env:MONOLITH_RELEASE_BUILD = "1"
    Write-Host "    MONOLITH_RELEASE_BUILD=1 (BA/GBA/ComboGraph forced off)" -ForegroundColor DarkGray

    try {
        # Non-unity build catches missing includes and unity-only symbol collisions
        # before they reach public releases (feedback_non_unity_build_releases.md).
        & $UBT LeviathanEditor Win64 Development "-Project=$UProject" -waitmutex -DisableUnity
        if ($LASTEXITCODE -ne 0) {
            throw "UBT failed with exit code $LASTEXITCODE. Is the editor closed?"
        }
        Write-Host "    Build succeeded" -ForegroundColor Green
    }
    finally {
        # Always unset — even if build fails, don't poison future dev builds
        Remove-Item Env:\MONOLITH_RELEASE_BUILD -ErrorAction SilentlyContinue
        Write-Host "    MONOLITH_RELEASE_BUILD unset" -ForegroundColor DarkGray
    }
} else {
    Write-Host "`n  [1/4] Skipping build (-SkipBuild flag)" -ForegroundColor DarkGray
    Write-Host "    WARNING: Ensure you built with MONOLITH_RELEASE_BUILD=1" -ForegroundColor Red
}

# --- Step 2: Copy tracked files ---
Write-Host "`n  [2/4] Copying tracked files..." -ForegroundColor Yellow

if (Test-Path $TempDir) { Remove-Item $TempDir -Recurse -Force }
New-Item -ItemType Directory -Path $TempDir | Out-Null

Push-Location $PluginDir
$allTrackedFiles = git ls-files
# Strip non-redistributable module sources (Source/<Module>/ and Intermediate/<Module>/)
$trackedFiles = $allTrackedFiles | Where-Object {
    $path = $_
    $keep = $true
    foreach ($mod in $StrippedModules) {
        if ($path -like "Source/$mod/*" -or $path -like "Intermediate/*$mod*") {
            $keep = $false
            break
        }
    }
    # Strip internal testing-execution records (Docs/testing/) — per-feature test-pass
    # diaries with no downstream consumer value. Public-facing test artefacts live
    # elsewhere (SPEC sections, automation tests under Source/).
    if ($keep -and $path -like "Docs/testing/*") {
        $keep = $false
    }
    $keep
}
$strippedSourceCount = $allTrackedFiles.Count - $trackedFiles.Count
foreach ($file in $trackedFiles) {
    $destPath = Join-Path $TempDir $file
    $destDir = Split-Path -Parent $destPath
    if (-not (Test-Path $destDir)) {
        New-Item -ItemType Directory -Path $destDir -Force | Out-Null
    }
    Copy-Item $file -Destination $destPath -Force
}
Pop-Location
Write-Host "    $($trackedFiles.Count) files copied ($strippedSourceCount stripped: $($StrippedModules -join ', '))" -ForegroundColor Green

# --- Step 3: Copy binaries (gitignored but needed for Blueprint-only users) ---
Write-Host "`n  [3/4] Copying binaries..." -ForegroundColor Yellow

$binDir = Join-Path $PluginDir "Binaries"
if (Test-Path $binDir) {
    $destBin = Join-Path $TempDir "Binaries"
    New-Item -ItemType Directory -Path $destBin -Force | Out-Null
    $binCount = 0
    $binStripCount = 0
    # Build a regex that matches any stripped module's binary (e.g. "UnrealEditor-MonolithSteamBridge.")
    $stripModuleRegex = "(" + (($StrippedModules | ForEach-Object { [regex]::Escape($_) }) -join "|") + ")"
    Get-ChildItem $binDir -Recurse -File |
        Where-Object { $_.Extension -ne '.pdb' -and $_.Name -notmatch '\.patch_' } |
        ForEach-Object {
            if ($_.Name -match "UnrealEditor-$stripModuleRegex\.") {
                $binStripCount++
                return
            }
            $rel = $_.FullName.Substring($binDir.Length)
            $dest = Join-Path $destBin $rel
            $destParent = Split-Path -Parent $dest
            if (-not (Test-Path $destParent)) { New-Item -ItemType Directory -Path $destParent -Force | Out-Null }
            Copy-Item $_.FullName -Destination $dest -Force
            $binCount++
        }
    Write-Host "    $binCount binary files included (no .pdb, no .patch_*, $binStripCount stripped: $($StrippedModules -join ', '))" -ForegroundColor Green
} else {
    Write-Host "    WARNING: No Binaries/ found - Blueprint-only users will need to compile" -ForegroundColor Red
}

# --- Step 4: Patch and package ---
Write-Host "`n  [4/4] Packaging..." -ForegroundColor Yellow

# Set Installed=true for Blueprint-only users
$upluginPath = Join-Path $TempDir "Monolith.uplugin"
$content = Get-Content $upluginPath -Raw
$content = $content -replace '"Installed":\s*false', '"Installed": true'

# Strip non-redistributable module entries from the "Modules" array.
# Matches an optional preceding comma, the module object block, and its trailing comma (if present).
$upluginStrips = 0
foreach ($mod in $StrippedModules) {
    # Match the module object + its trailing comma (if present). Do NOT consume the leading
    # comma -- that belongs to the previous entry and must stay. If the stripped module was
    # the LAST array entry (no trailing comma), the previous entry's trailing comma is
    # orphaned; the ",]" cleanup below catches that.
    $escMod = [regex]::Escape($mod)
    $pattern = "(?s)\{\s*""Name"":\s*""$escMod"".*?\}\s*,?\s*"
    $before = $content.Length
    $content = [regex]::Replace($content, $pattern, "")
    if ($content.Length -ne $before) { $upluginStrips++ }
}

# Strip any trailing comma immediately before a closing array bracket.
$content = $content -replace ',(\s*\])', '$1'

Set-Content $upluginPath $content -NoNewline
Write-Host "    Installed=true set in .uplugin, $upluginStrips module entries stripped" -ForegroundColor Green

# Create zip
if (Test-Path $OutputZip) { Remove-Item $OutputZip -Force }
Compress-Archive -Path "$TempDir\*" -DestinationPath $OutputZip -Force

# Clean temp
Remove-Item $TempDir -Recurse -Force

$fileSize = [math]::Round((Get-Item $OutputZip).Length / 1MB, 1)
Write-Host "`nRelease complete: $OutputZip" -ForegroundColor Green
Write-Host "Size: ${fileSize}MB" -ForegroundColor Green
Write-Host "`nVerify: optional deps should be OFF in the binaries." -ForegroundColor Cyan
Write-Host "  WITH_BLUEPRINT_ASSIST=0, WITH_GBA=0, WITH_COMMONUI=0" -ForegroundColor Cyan
Write-Host "  WITH_COMBOGRAPH=0, WITH_LOGICDRIVER=0, WITH_METASOUND=0" -ForegroundColor Cyan
Write-Host "  WITH_GAMEPLAYABILITIES=0, WITH_GAMEPLAYBEHAVIORS=0" -ForegroundColor Cyan
Write-Host "  WITH_MASSENTITY=0, WITH_ZONEGRAPH=0" -ForegroundColor Cyan
Write-Host "  WITH_STATETREE=0, WITH_SMARTOBJECTS=0   (after F22 lands)" -ForegroundColor Cyan
Write-Host "  Your next editor build will auto-detect deps normally." -ForegroundColor DarkGray

# --- Step 5: Post-build hard-link smoke (defense against issue #30) ---
# Issue #30 (v0.14.0): MonolithMesh.dll hard-linked GeometryScriptingCore.dll because
# MonolithMesh.Build.cs missed the MONOLITH_RELEASE_BUILD=1 bypass on its GeometryScripting
# probe. End-user editors failed to load Monolith with "missing import" errors.
#
# This step dumps the import table of every UnrealEditor-Monolith*.dll in the release zip
# and refuses if any imports a sentinel module — a module from a non-default-enabled UE
# plugin that should NEVER appear in a release-built Monolith binary.
Write-Host "`n  [5/5] Post-build hard-link smoke (issue #30 defense)..." -ForegroundColor Yellow

# Sentinel modules: their presence in a Monolith DLL's imports = build-time gate failure.
# Add new sentinels when adding new optional plugin integrations.
$LeakSentinels = @(
    "GeometryScriptingCore", "CommonUI", "CommonInput", "BlueprintAssist",
    "GameplayAbilities", "GameplayBehaviorsModule", "MassEntity", "ZoneGraph",
    "StateTreeModule", "SmartObjectsModule", "ComboGraphRuntime", "LogicDriver",
    "MetaSoundEngine", "MetaSoundFrontend"
)

# Locate dumpbin.exe — ships with Visual Studio Build Tools. Try common locations
# before giving up. If not found, skip the smoke (warn, don't fail) so the script
# remains usable on dev machines without VS BuildTools.
$Dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
if (-not $Dumpbin) {
    $VSCommonPaths = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC"
    )
    foreach ($vsBase in $VSCommonPaths) {
        if (Test-Path $vsBase) {
            $candidate = Get-ChildItem -Path $vsBase -Directory -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending |
                ForEach-Object { Join-Path $_.FullName "bin\HostX64\x64\dumpbin.exe" } |
                Where-Object { Test-Path $_ } |
                Select-Object -First 1
            if ($candidate) { $Dumpbin = $candidate; break }
        }
    }
}

if (-not $Dumpbin) {
    Write-Host "    [SKIP] dumpbin.exe not found — install Visual Studio Build Tools to enable hard-link smoke." -ForegroundColor Yellow
} else {
    # Re-extract the just-built zip into a scratch dir to inspect the actual shipped DLLs
    # (not the dev binaries we may have overwritten before zipping).
    $SmokeDir = Join-Path $env:TEMP "Monolith_Release_${Version}_Smoke"
    if (Test-Path $SmokeDir) { Remove-Item $SmokeDir -Recurse -Force }
    New-Item -ItemType Directory -Path $SmokeDir | Out-Null
    Expand-Archive -Path $OutputZip -DestinationPath $SmokeDir -Force

    $MonolithDlls = @(Get-ChildItem -Path $SmokeDir -Recurse -Filter "UnrealEditor-Monolith*.dll")
    $LeakingDlls = @()
    foreach ($dllItem in $MonolithDlls) {
        $imports = & $Dumpbin /imports $dllItem.FullName 2>$null | Out-String
        foreach ($sentinel in $LeakSentinels) {
            # Match "UnrealEditor-<Sentinel>.dll" in the import table
            if ($imports -match "UnrealEditor-$([regex]::Escape($sentinel))\.dll") {
                $LeakingDlls += [PSCustomObject]@{
                    Dll      = $dllItem.Name
                    Sentinel = $sentinel
                }
            }
        }
    }

    # Cleanup smoke dir regardless of outcome
    Remove-Item $SmokeDir -Recurse -Force -ErrorAction SilentlyContinue

    if ($LeakingDlls.Count -gt 0) {
        Write-Host "`n  [FAIL] Hard-link smoke found $($LeakingDlls.Count) sentinel import(s) in shipped DLLs:" -ForegroundColor Red
        $LeakingDlls | ForEach-Object {
            Write-Host "    $($_.Dll) imports UnrealEditor-$($_.Sentinel).dll" -ForegroundColor Red
        }
        Write-Host "`n  This is the issue #30 failure mode. The Build.cs for the affected module" -ForegroundColor Red
        Write-Host "  is not honouring MONOLITH_RELEASE_BUILD=1. Fix the Build.cs probe before shipping." -ForegroundColor Red
        Write-Host "`n  Refusing to publish v$Version. Delete $OutputZip after fixing Build.cs and re-run." -ForegroundColor Red
        exit 1
    }
    Write-Host "    No sentinel imports found in $($MonolithDlls.Count) Monolith DLLs (clean)" -ForegroundColor Green
}

# --- SHA256 hash for release notes (Issue #38) ---
# Marker token is `Monolith-SHA256:` (not bare `SHA256:`) so the auto-updater's
# regex never collides with prose mentions of the word SHA256 elsewhere in the
# release body. The parser anchors on this exact sentinel.
if (Test-Path $OutputZip) {
    $Hash = (Get-FileHash -Algorithm SHA256 -Path $OutputZip).Hash.ToLower()
    Write-Host ""
    Write-Host "================================================================" -ForegroundColor Cyan
    Write-Host "SHA256: $Hash" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Paste this exact line into the GitHub Release notes body:" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  Monolith-SHA256: $Hash" -ForegroundColor White
    Write-Host ""
    Write-Host "The auto-updater parses this exact marker and refuses to install" -ForegroundColor Yellow
    Write-Host "if the downloaded zip's hash does not match. Do not rename or"     -ForegroundColor Yellow
    Write-Host "reformat the marker -- the prefix and a single space before the"   -ForegroundColor Yellow
    Write-Host "hex string are required."                                          -ForegroundColor Yellow
    Write-Host "================================================================" -ForegroundColor Cyan
}
