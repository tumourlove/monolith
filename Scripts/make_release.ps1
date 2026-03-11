# Monolith Release Zip Builder
# Creates a release zip with "Installed": true for Blueprint-only compatibility.
# Usage: powershell -ExecutionPolicy Bypass -File Scripts\make_release.ps1 -Version "0.7.1"

param(
    [Parameter(Mandatory=$true)]
    [string]$Version
)

$ErrorActionPreference = "Stop"

$PluginDir = Split-Path -Parent $PSScriptRoot
$OutputZip = Join-Path (Split-Path -Parent $PluginDir) "Monolith-v$Version.zip"
$TempDir = Join-Path $env:TEMP "Monolith_Release_$Version"

Write-Host "Building Monolith v$Version release zip..." -ForegroundColor Cyan

# Clean temp
if (Test-Path $TempDir) { Remove-Item $TempDir -Recurse -Force }
New-Item -ItemType Directory -Path $TempDir | Out-Null

# Use git ls-files to get only tracked files (respects .gitignore)
# This ensures internal docs, plans, TESTING.md, and build artifacts don't leak into releases
Write-Host "  Copying tracked files..." -ForegroundColor Yellow
Push-Location $PluginDir
$trackedFiles = git ls-files
foreach ($file in $trackedFiles) {
    $destPath = Join-Path $TempDir $file
    $destDir = Split-Path -Parent $destPath
    if (-not (Test-Path $destDir)) {
        New-Item -ItemType Directory -Path $destDir -Force | Out-Null
    }
    Copy-Item $file -Destination $destPath -Force
}
Pop-Location

# Copy Binaries (gitignored but needed for Blueprint-only users)
# Exclude .pdb (debug symbols) and .patch_* (Live Coding artifacts)
$binDir = Join-Path $PluginDir "Binaries"
if (Test-Path $binDir) {
    $destBin = Join-Path $TempDir "Binaries"
    New-Item -ItemType Directory -Path $destBin -Force | Out-Null
    Get-ChildItem $binDir -Recurse -File |
        Where-Object { $_.Extension -ne '.pdb' -and $_.Name -notmatch '\.patch_' } |
        ForEach-Object {
            $rel = $_.FullName.Substring($binDir.Length)
            $dest = Join-Path $destBin $rel
            $destParent = Split-Path -Parent $dest
            if (-not (Test-Path $destParent)) { New-Item -ItemType Directory -Path $destParent -Force | Out-Null }
            Copy-Item $_.FullName -Destination $dest -Force
        }
    Write-Host "  Included Binaries/ for Blueprint-only compatibility" -ForegroundColor Green
} else {
    Write-Host "  WARNING: No Binaries/ found" -ForegroundColor Yellow
}

# Patch .uplugin: set "Installed": true for Blueprint-only users
$upluginPath = Join-Path $TempDir "Monolith.uplugin"
$content = Get-Content $upluginPath -Raw
$content = $content -replace '"Installed":\s*false', '"Installed": true'
Set-Content $upluginPath $content -NoNewline

Write-Host "  Set Installed=true in release .uplugin" -ForegroundColor Green

# Create zip
if (Test-Path $OutputZip) { Remove-Item $OutputZip -Force }
Compress-Archive -Path "$TempDir\*" -DestinationPath $OutputZip -Force

# Clean temp
Remove-Item $TempDir -Recurse -Force

$fileSize = [math]::Round((Get-Item $OutputZip).Length / 1MB, 1)
Write-Host "Release complete: $OutputZip" -ForegroundColor Green
Write-Host "Size: $fileSize megabytes" -ForegroundColor Green
