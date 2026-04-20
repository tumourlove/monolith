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

# Modules stripped from every public release zip.
# - MonolithSteamBridge: solo-dev only, Steam Integration Kit bridge -- not public
# (MonolithISX was extracted to a sibling plugin at Plugins/MonolithISX/ on 2026-04-21 —
#  it no longer lives in this repo and therefore does not need stripping here.)
$StrippedModules = @("MonolithSteamBridge")

$ProjectDir = Split-Path -Parent (Split-Path -Parent $PluginDir)
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
Write-Host "  WITH_BLUEPRINT_ASSIST=0, WITH_GBA=0" -ForegroundColor Cyan
Write-Host "  Your next editor build will auto-detect deps normally." -ForegroundColor DarkGray
