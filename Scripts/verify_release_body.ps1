# ==============================================================================
# verify_release_body.ps1 -- ship-blocking gate for a DRAFT GitHub release
# ==============================================================================
#
# Run this AFTER 'gh release create --draft' has uploaded its assets and BEFORE
# 'gh release edit --draft=false'. It exits non-zero on any failure, so the
# publish flow must stop rather than flip a bad release live.
#
# WHY DRAFT-THEN-FLIP AT ALL
#   'gh release create <tag> <zips>' creates the release PUBLISHED and only then
#   uploads the assets. During that upload window GET /releases/latest returns
#   the new tag with zero or partial assets. Monolith clients from v0.14.7
#   through v0.21.2 fall back to GitHub's generated source zipball when a
#   release carries no usable binary zip, and that archive has no compiled
#   DLLs -- so a client polling inside the window installs a broken plugin over
#   a working one. Those clients are already deployed and no change to HEAD can
#   reach them. Publishing as a draft closes the window for ALL of them,
#   because GET /releases/latest is documented to return "the most recent
#   non-prerelease, non-draft release". (Prereleases are excluded too, so never
#   use --prerelease for a staged rollout expecting clients to see it.)
#
# NOTE ON ENCODING: this file must stay ASCII-only. Windows PowerShell 5.1
# falls back to Windows-1252 when a .ps1 has no UTF-8 BOM, which mis-tokenises
# non-ASCII punctuation and produces parse errors at unrelated line numbers.
#
# Usage:
#   .\verify_release_body.ps1 -Version "0.21.3"
#   .\verify_release_body.ps1 -Version "0.21.3" -ArtifactDir <dir>   # if the zips are elsewhere
# ==============================================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]  [string] $Version,
    [Parameter(Mandatory = $false)] [string] $Repo        = "tumourlove/monolith",
    # Defaults to the host project root, which is where make_release.ps1 writes the
    # zips: this script lives at <ProjectRoot>\Plugins\Monolith\Scripts\, so three
    # levels up is the project root. Derived rather than hardcoded so the script
    # works from any clone.
    [Parameter(Mandatory = $false)] [string] $ArtifactDir = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
)

$ErrorActionPreference = "Stop"
$script:Failures = @()

function Fail([string] $Message) {
    $script:Failures += $Message
    Write-Host "  [FAIL] $Message" -ForegroundColor Red
}
function Pass([string] $Message) {
    Write-Host "  [ OK ] $Message" -ForegroundColor Green
}

$Tag = "v$Version"
Write-Host ""
Write-Host "Verifying draft release $Tag in $Repo" -ForegroundColor Cyan
Write-Host ""

# --- Fetch the release --------------------------------------------------------
# --jq is deliberately avoided; ConvertFrom-Json keeps this readable on PS 5.1.
$raw = & gh release view $Tag --repo $Repo --json isDraft,body,assets 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "  [FAIL] Could not read release $Tag. gh said:" -ForegroundColor Red
    Write-Host "    $raw" -ForegroundColor Yellow
    exit 1
}
$rel = $raw | ConvertFrom-Json

# --- 1. It must still be a draft ---------------------------------------------
# If this is already published the gate is pointless -- the window is open.
if ($rel.isDraft -ne $true) {
    Fail "Release $Tag is ALREADY PUBLISHED. This gate must run while it is still a draft."
} else {
    Pass "Release is a draft"
}

# --- 2. Exactly three assets, all fully uploaded ------------------------------
$expected = @(
    "Monolith-v$Version-UE5.7.zip",
    "Monolith-v$Version-UE5.8.zip",
    "Monolith-v$Version.zip"
)
$actual = @($rel.assets | ForEach-Object { $_.name })

if ($actual.Count -ne 3) {
    Fail "Expected 3 assets, found $($actual.Count): $($actual -join ', ')"
} else {
    Pass "Asset count is 3"
}
foreach ($name in $expected) {
    if ($actual -notcontains $name) {
        Fail "Missing expected asset: $name"
    }
}
foreach ($name in $actual) {
    if ($expected -notcontains $name) {
        Fail "Unexpected asset present: $name (would be offered to updaters)"
    }
}
foreach ($a in $rel.assets) {
    if ($a.state -ne "uploaded") {
        Fail "Asset $($a.name) is in state '$($a.state)', not 'uploaded'"
    }
    if ([int64]$a.size -le 0) {
        Fail "Asset $($a.name) has size $($a.size)"
    }
}
if ($script:Failures.Count -eq 0) { Pass "All 3 assets present, uploaded, non-empty" }

# --- 3. Pre-v2 SHA markers are a PERMANENT ship-blocker -----------------------
# Updaters shipped in v0.14.7 through v0.21.0 call FPlatformMisc::GetSHA256Signature
# when they RECOGNISE a marker. That function has no Windows implementation and its
# generic fallback is a fatal checkf, so a recognised marker in a release body
# hard-crashes every un-upgraded Windows client that clicks Install (issues #90/#94).
# The v2 names are invisible to those parsers, so old clients fail safe.
$body = [string]$rel.body
$preV2 = [regex]::Matches($body, '(?m)^\s*Monolith-SHA256(-UE5\.[0-9]+)?:')
if ($preV2.Count -gt 0) {
    Fail "Body contains $($preV2.Count) PRE-V2 SHA marker(s). These hard-crash every deployed v0.14.7-v0.21.0 Windows client that clicks Install. Remove them."
} else {
    Pass "No pre-v2 SHA markers"
}

# --- 4. Exactly the three v2 markers -----------------------------------------
$markerMap = @{
    "Monolith-SHA256-v2-UE5.7:" = "Monolith-v$Version-UE5.7.zip"
    "Monolith-SHA256-v2-UE5.8:" = "Monolith-v$Version-UE5.8.zip"
    "Monolith-SHA256-v2:"       = "Monolith-v$Version.zip"
}
$markerValues = @{}
foreach ($prefix in $markerMap.Keys) {
    # The updater anchors on the literal prefix followed by a single space.
    $pattern = '(?m)^' + [regex]::Escape($prefix) + ' ([0-9a-fA-F]{64})(?![0-9a-fA-F])'
    $m = [regex]::Matches($body, $pattern)
    if ($m.Count -ne 1) {
        Fail "Expected exactly 1 '$prefix' marker, found $($m.Count)"
    } else {
        $markerValues[$prefix] = $m[0].Groups[1].Value.ToLower()
    }
}
if ($markerValues.Count -eq 3) { Pass "All 3 v2 markers present and well-formed" }

# --- 5. Each marker must match the actual artifact ----------------------------
# This is the check that would have caught a stale marker copied from a prior run.
foreach ($prefix in $markerMap.Keys) {
    if (-not $markerValues.ContainsKey($prefix)) { continue }
    $zip = Join-Path $ArtifactDir $markerMap[$prefix]
    if (-not (Test-Path $zip)) {
        Fail "Cannot verify '$prefix': artifact not found at $zip"
        continue
    }
    $actualSha = (Get-FileHash -Path $zip -Algorithm SHA256).Hash.ToLower()
    if ($actualSha -ne $markerValues[$prefix]) {
        Fail "SHA mismatch for $($markerMap[$prefix]): body says $($markerValues[$prefix]), file is $actualSha"
    } else {
        Pass "SHA matches for $($markerMap[$prefix])"
    }
}

# The legacy bridge zip is a byte copy of the UE5.7 zip, so their hashes must agree.
if ($markerValues.ContainsKey("Monolith-SHA256-v2:") -and $markerValues.ContainsKey("Monolith-SHA256-v2-UE5.7:")) {
    if ($markerValues["Monolith-SHA256-v2:"] -ne $markerValues["Monolith-SHA256-v2-UE5.7:"]) {
        Fail "Legacy marker does not equal the UE5.7 marker. The bridge zip is supposed to be a copy of the UE5.7 zip."
    } else {
        Pass "Legacy bridge marker equals UE5.7 marker"
    }
}

# --- 6. No AI attribution in a public release body ---------------------------
# The single-character classes below ([l], [w], [n]) are deliberate. They match
# exactly as the plain letters would, but they stop this file from containing the
# literal phrases it searches for -- this script ships inside the release zip, and
# the zip-contents scan asserts that no shipped text payload contains them. Without
# the classes, the detector would trip on itself.
$attribution = [regex]::Matches($body, '(?i)co-authored.{0,20}c[l]aude|generated [w]ith claude|[n]oreply@anthropic')
if ($attribution.Count -gt 0) {
    Fail "Release body contains AI-attribution text. All public content is attributed solely to the maintainer."
} else {
    Pass "No AI-attribution text in body"
}

# --- Verdict ------------------------------------------------------------------
Write-Host ""
if ($script:Failures.Count -gt 0) {
    Write-Host "GATE FAILED -- $($script:Failures.Count) problem(s). Do NOT publish." -ForegroundColor Red
    Write-Host "The release is still a draft, so nothing is visible to clients yet." -ForegroundColor Yellow
    Write-Host ""
    exit 1
}

Write-Host "GATE PASSED. Safe to publish:" -ForegroundColor Green
Write-Host "  gh release edit $Tag --repo $Repo --draft=false" -ForegroundColor Cyan
Write-Host ""
exit 0
