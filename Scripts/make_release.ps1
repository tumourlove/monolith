# Monolith Release Zip Builder
# Creates a release zip with "Installed": true for Blueprint-only compatibility.
# Usage: powershell -ExecutionPolicy Bypass -File Scripts\make_release.ps1 -Version "0.5.0"

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

# Use git archive to export only tracked files (respects .gitignore)
# This ensures internal docs (plans/, TESTING.md) and build artifacts don't leak into releases
Write-Host "  Exporting tracked files via git archive..." -ForegroundColor Yellow
Push-Location $PluginDir
git archive HEAD --format=tar | tar -xf - -C $TempDir
Pop-Location

# Copy Binaries (gitignored but needed for Blueprint-only users)
$binDir = Join-Path $PluginDir "Binaries"
if (Test-Path $binDir) {
    Copy-Item $binDir -Destination (Join-Path $TempDir "Binaries") -Recurse -Force
    Write-Host "  Included Binaries/ for Blueprint-only compatibility" -ForegroundColor Green
} else {
    Write-Host "  WARNING: No Binaries/ found — C++ projects must rebuild" -ForegroundColor Yellow
}

# Patch .uplugin: set "Installed": true for Blueprint-only users
$upluginPath = Join-Path $TempDir "Monolith.uplugin"
$content = Get-Content $upluginPath -Raw
$content = $content -replace '"Installed":\s*false', '"Installed": true'
Set-Content $upluginPath $content -NoNewline

Write-Host "  Set 'Installed': true in release .uplugin" -ForegroundColor Green

# Create zip
if (Test-Path $OutputZip) { Remove-Item $OutputZip -Force }
Compress-Archive -Path "$TempDir\*" -DestinationPath $OutputZip -Force

# Clean temp
Remove-Item $TempDir -Recurse -Force

$sizeMB = [math]::Round((Get-Item $OutputZip).Length / 1MB, 1)
Write-Host "Done: $OutputZip ($sizeMB MB)" -ForegroundColor Green
