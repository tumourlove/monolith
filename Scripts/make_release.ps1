# Monolith Release Zip Builder
# Creates per-engine release zips with "Installed": true for Blueprint-only compatibility.
# Automatically builds with optional dependencies disabled (MONOLITH_RELEASE_BUILD=1).
#
# Usage: powershell -ExecutionPolicy Bypass -File Scripts\make_release.ps1 -Version "0.10.0"
#
# What it does (per engine, UE5.7 in this project + UE5.8 in the FIVEPOINT8 project):
#   1. Sets MONOLITH_RELEASE_BUILD=1 (forces BA/GBA optional deps OFF in Build.cs)
#   2. Touches every Source/*/*.Build.cs (mtime bump) to force a clean recompile so the
#      incremental release build cannot reuse stale dev .obj/DLLs (issue #71 mode 1)
#   3. Runs that engine's UBT (-DisableUnity) to produce clean release binaries
#   4. Runs the full-unity collision gate against that engine (issue #68 defense)
#   5. Harvests that engine's Binaries/Win64, strips .pdb/.patch_*/sibling/.claude
#   6. Packages tracked files (ONE shared git ls-files copy) + that engine's binaries into
#      Monolith-v<X.Y.Z>-UE5.<minor>.zip with Installed=true
#   7. Runs the mandatory dumpbin hard-link import smoke against that zip's DLLs
#   8. Prints the per-engine SHA256 marker for the release notes
#   Finally: copies the UE5.7 zip to Monolith-v<X.Y.Z>.zip (legacy bridge for old updaters)
#   and prints the Monolith-SHA256-v2: marker (= the UE5.7 hash). All markers use the
#   "v2" names -- pre-v0.21.1 updaters hard-crash on the old names (#90/#94).
#
# Source users (GitHub clones) are unaffected -- Build.cs auto-detects at compile time.
#
# Non-redistributable sibling modules live outside this repo. The strip phase below
# is defense-in-depth for accidental re-merges into Plugins/Monolith/.
#
# IMPORTANT: ALL editors (both this project and FIVEPOINT8) must be CLOSED -- UBT fails
# with LIVE_CODING_BLOCKED otherwise. The FIVEPOINT8 Plugins/Monolith clone MUST be at the
# same commit as this repo's HEAD (the script asserts this and aborts on mismatch), since
# both zips ship the SAME tracked content (git ls-files from THIS repo) with only the
# per-engine Binaries/Win64 set differing.

param(
    [Parameter(Mandatory=$true)]
    [string]$Version,
    [switch]$SkipBuild,
    # Allow releasing with a dirty working tree. DANGEROUS: WIP modifications to tracked
    # files end up in the zip because this script copies the working-tree content, not the
    # committed HEAD. Only use if you know exactly what dirty files you're shipping.
    [switch]$AllowDirtyTree,
    # Allow releasing when the hard-link import smoke cannot run (dumpbin.exe not found).
    # DANGEROUS: skips the only check that proves shipped DLLs do not hard-link optional
    # plugins (issue #30 / #71 failure mode). Real releases NEVER set this -- it exists
    # only so a dev machine without VS Build Tools can produce a non-shippable test zip.
    [switch]$AllowUnverifiedImports
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
# Sibling plugins live outside Plugins/Monolith/ at the project's Plugins/ level.
# They are naturally excluded from the release zip by `git ls-files` scope (which
# only sees files inside Plugins/Monolith/). This array exists purely as
# defense-in-depth: if someone ever accidentally re-merges sibling source back into
# Plugins/Monolith/Source/ (refactor mistake, copy-paste, etc.), the strip filter
# catches it before it ships.
#
# Auto-discover all "Monolith*" sibling folders alongside Plugins/Monolith/ -- every new
# sibling gets protected automatically without script maintenance. Excludes Monolith
# itself.
$ProjectPluginsDir = Join-Path $ProjectDir "Plugins"
$StrippedModules = @(Get-ChildItem -Path $ProjectPluginsDir -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like "Monolith*" -and $_.Name -ne "Monolith" } |
    Select-Object -ExpandProperty Name)
if ($StrippedModules.Count -gt 0) {
    Write-Host "  [strip-list] Auto-discovered $($StrippedModules.Count) sibling plugin(s) to defend against: $($StrippedModules -join ', ')" -ForegroundColor DarkGray
}

$TempDir = Join-Path $env:TEMP "Monolith_Release_$Version"

# --- Engine matrix -------------------------------------------------------------------
# Two full per-engine release builds harvested from the SAME source commit. UE5.7 is
# built here (the Monolith dev-master); UE5.8 is built in the FIVEPOINT8 project, whose
# Plugins/Monolith is a clone of this repo pinned to the same HEAD. Each entry drives a
# full build + collision gate + Binaries harvest + zip + import smoke.
#
# UBT is invoked directly (the exe, not Build.bat) -- the prior single-engine script did
# the same, and the exe lives at the same relative path under every UE 5.x install.
$LegacyZip = Join-Path $ProjectDir "Monolith-v$Version.zip"  # legacy bridge (= UE5.7 copy)

# --- Non-Monolith prototyping plugins to temporarily DISABLE during each engine's release
#     build. The dev-master project also hosts gameplay-prototyping siblings (HordeForge,
#     OptimizedGASP) OUTSIDE Plugins/Monolith. The editor target compiles them too, so a
#     -DisableUnity compile error in a sibling (e.g. HordeForge's unity-leak: a log category
#     used without its declaring header) would abort the Monolith release -- which must NOT
#     be hostage to sibling compile health. We disable these for the build, then restore the
#     EXACT original .uproject bytes (see Disable-ProjectPlugins / the build wrapper).
#     FIVEPOINT8 does not contain these, so the toggle is a harmless no-op there.
$ExcludeProjectPlugins = @('HordeForge', 'OptimizedGASP')

$EngineMatrix = @(
    [PSCustomObject]@{
        Tag        = "UE5.7"                               # asset/marker engine tag
        UBT        = 'C:\Program Files (x86)\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe'
        Target     = "MonolithEditor"
        ProjectDir = $ProjectDir                            # this Monolith dev-master
        UProject   = (Join-Path $ProjectDir "Monolith.uproject")
        PluginDir  = $PluginDir                             # this repo's Plugins/Monolith
        Zip        = (Join-Path $ProjectDir "Monolith-v$Version-UE5.7.zip")
        IsLegacy   = $true                                  # this zip seeds the legacy bridge
    },
    [PSCustomObject]@{
        Tag        = "UE5.8"
        UBT        = 'C:\Program Files (x86)\UE_5.8\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe'
        Target     = "FIVEPOINT8Editor"
        ProjectDir = 'D:\Unreal Projects\FIVEPOINT8'
        UProject   = 'D:\Unreal Projects\FIVEPOINT8\FIVEPOINT8.uproject'
        PluginDir  = 'D:\Unreal Projects\FIVEPOINT8\Plugins\Monolith'  # the UE5.8 clone (Binaries source)
        Zip        = (Join-Path $ProjectDir "Monolith-v$Version-UE5.8.zip")
        IsLegacy   = $false
    }
)

Write-Host "Building Monolith v$Version release zips (per engine: $(( $EngineMatrix | ForEach-Object { $_.Tag }) -join ', '))..." -ForegroundColor Cyan

# --- Preflight: validate every engine in the matrix BEFORE doing any work --------------
# Fail fast (and TOGETHER) if any engine's toolchain, project, or clone is wrong. A
# half-built release (one engine's zip present, the other missing/aborted) is worse than
# none -- so we assert all preconditions up front.
foreach ($eng in $EngineMatrix) {
    if (-not (Test-Path $eng.UBT)) {
        throw "[$($eng.Tag)] UnrealBuildTool.exe not found at $($eng.UBT). Is UE $($eng.Tag) installed?"
    }
    if (-not (Test-Path $eng.UProject)) {
        throw "[$($eng.Tag)] .uproject not found at $($eng.UProject)."
    }
    if (-not (Test-Path (Join-Path $eng.PluginDir "Monolith.uplugin"))) {
        throw "[$($eng.Tag)] Monolith.uplugin not found under $($eng.PluginDir). Is the clone present?"
    }
}

# The FIVEPOINT8 clone MUST be at the same commit as this repo's HEAD, else the two zips
# would ship content that does not match -- the tracked content copy comes from THIS repo
# but the UE5.8 Binaries come from the clone, so a commit skew = mismatched ship.
$ThisHead = (& git -C $PluginDir rev-parse HEAD).Trim()
foreach ($eng in $EngineMatrix) {
    if ($eng.PluginDir -eq $PluginDir) { continue }  # this repo IS the reference
    $cloneHead = (& git -C $eng.PluginDir rev-parse HEAD).Trim()
    if ($cloneHead -ne $ThisHead) {
        Write-Host "`n  [FAIL] [$($eng.Tag)] clone HEAD does not match this repo's HEAD." -ForegroundColor Red
        Write-Host "    this repo ($PluginDir): $ThisHead" -ForegroundColor Red
        Write-Host "    clone ($($eng.PluginDir)): $cloneHead" -ForegroundColor Red
        Write-Host "`n  Both zips ship the SAME tracked content from this repo but per-engine binaries" -ForegroundColor Red
        Write-Host "  from each clone. Check out $ThisHead in the $($eng.Tag) clone, then re-run." -ForegroundColor Red
        exit 1
    }
    Write-Host "  [precheck] $($eng.Tag) clone at $cloneHead (matches this repo)" -ForegroundColor DarkGray
}

# =====================================================================================
# Per-engine functions. Each is engine-parameterised so the UE5.7 and UE5.8 passes run
# the IDENTICAL gate logic against their own toolchain/binaries.
# =====================================================================================

# Touch every Source/*/*.Build.cs so the incremental release build cannot reuse stale dev
# .obj/DLLs compiled with WITH_*=1 (issue #71 mode 1). mtime-only: clean-tree-safe.
function Invoke-TouchBuildCs {
    param([string]$EnginePluginDir, [string]$Tag)
    $srcDir = Join-Path $EnginePluginDir "Source"
    $now = Get-Date
    $touched = 0
    Get-ChildItem -Path $srcDir -Recurse -Filter "*.Build.cs" -ErrorAction SilentlyContinue |
        ForEach-Object { $_.LastWriteTime = $now; $touched++ }
    Write-Host "    [$Tag] Touched $touched Build.cs file(s) to force clean recompile (anti stale-obj)" -ForegroundColor DarkGray
}

# Force every plugin in $ExcludeProjectPlugins to Enabled:false in the given .uproject so
# the editor target does NOT compile those non-Monolith prototyping siblings during the
# release build. They are EnabledByDefault and may not be listed in the Plugins array, so
# we ADD an explicit disabled entry; if already listed, we set Enabled:false on it. A
# disabled entry for a plugin that does not exist under the project is harmless, so we add
# unconditionally (FIVEPOINT8 has none of these -> the entries are inert there). The caller
# is responsible for restoring the EXACT original bytes afterward (byte-exact backup).
function Disable-ProjectPlugins {
    param([string]$UProjectPath, [string[]]$PluginNames, [string]$Tag)

    if (-not $PluginNames -or $PluginNames.Count -eq 0) { return }

    $json = Get-Content $UProjectPath -Raw | ConvertFrom-Json
    # Ensure a Plugins array exists (PSCustomObject from ConvertFrom-Json may lack it).
    if (-not ($json.PSObject.Properties.Name -contains 'Plugins') -or $null -eq $json.Plugins) {
        $json | Add-Member -NotePropertyName 'Plugins' -NotePropertyValue @() -Force
    }
    # Normalize to a mutable list of entries.
    $plugins = @($json.Plugins)
    foreach ($name in $PluginNames) {
        $existing = $plugins | Where-Object { $_.Name -eq $name } | Select-Object -First 1
        if ($existing) {
            $existing.Enabled = $false
        } else {
            $plugins += [PSCustomObject]@{ Name = $name; Enabled = $false }
        }
    }
    $json.Plugins = $plugins
    # Re-serialize. The original bytes are restored from the backup later regardless, so
    # exact formatting here does not matter -- only that UBT reads valid JSON.
    $out = $json | ConvertTo-Json -Depth 50
    Set-Content -Path $UProjectPath -Value $out -Encoding utf8
    Write-Host "    [$Tag] Temporarily disabled project plugin(s) for build: $($PluginNames -join ', ')" -ForegroundColor DarkGray
}

# Step 1 + 1a for ONE engine: release build (-DisableUnity) then the full-unity collision
# gate. Both run with MONOLITH_RELEASE_BUILD=1. Any failure throws (aborts whole release).
function Invoke-EngineBuild {
    param([PSCustomObject]$Engine)

    $Tag = $Engine.Tag
    $UBT = $Engine.UBT

    # Disable the non-Monolith prototyping siblings (HordeForge/OptimizedGASP) for BOTH the
    # -DisableUnity build and the full-unity gate below -- the editor target compiles them
    # and a sibling compile error must not abort the Monolith release. We snapshot the EXACT
    # original .uproject bytes FIRST and restore them in the finally that wraps the whole
    # build body, so a build/gate failure can never leave the project's .uproject mutated.
    $UProjectBackup = [System.IO.File]::ReadAllBytes($Engine.UProject)
    try {
        Disable-ProjectPlugins -UProjectPath $Engine.UProject -PluginNames $ExcludeProjectPlugins -Tag $Tag

    # --- Step 1: Build with optional deps disabled ---
    Write-Host "`n  [$Tag 1/4] Building release binaries (optional deps OFF)..." -ForegroundColor Yellow

    # Anti stale-obj: force a clean recompile in THIS engine's plugin tree first.
    Invoke-TouchBuildCs -EnginePluginDir $Engine.PluginDir -Tag $Tag

    # Set env var so Build.cs files skip optional dependency detection
    $env:MONOLITH_RELEASE_BUILD = "1"
    Write-Host "    [$Tag] MONOLITH_RELEASE_BUILD=1 (BA/GBA/ComboGraph forced off)" -ForegroundColor DarkGray

    try {
        # Non-unity build catches missing includes and unity-only symbol collisions
        # before they reach public releases (feedback_non_unity_build_releases.md).
        & $UBT $Engine.Target Win64 Development "-Project=$($Engine.UProject)" -waitmutex -DisableUnity
        if ($LASTEXITCODE -ne 0) {
            throw "[$Tag] UBT failed with exit code $LASTEXITCODE. Is the editor closed?"
        }
        Write-Host "    [$Tag] Build succeeded" -ForegroundColor Green
    }
    finally {
        # Always unset -- even if build fails, don't poison future dev builds
        Remove-Item Env:\MONOLITH_RELEASE_BUILD -ErrorAction SilentlyContinue
        Write-Host "    [$Tag] MONOLITH_RELEASE_BUILD unset" -ForegroundColor DarkGray
    }

    # --- Step 1a: Full-unity collision gate (issue #68 defense) ---
    # The -DisableUnity build above compiles every .cpp as its own translation unit.
    # That catches missing includes but is STRUCTURALLY BLIND to duplicate file-local
    # symbols: anonymous-namespace functions or file-static functions/vars sharing a
    # name across two+ .cpp files in one module. With no concatenation they never
    # collide, so -DisableUnity cannot see them. That is exactly how issue #68 shipped.
    #
    # End users build with DEFAULT adaptive unity. On a fresh clone the adaptive
    # working set is empty, so everything concatenates (effectively full unity) and
    # they hit the collision on first compile. To catch this class against the EXACT
    # release configuration, we run a SECOND pass under FORCED full unity.
    #
    # We force full unity by temporarily flipping bUseAdaptiveUnityBuild=false in the
    # UBT BuildConfiguration.xml, then ALWAYS restore the original (back it up first,
    # restore in finally). We do NOT pass -DisableUnity here -- we WANT concatenation.
    # We do NOT pass -Clean: flipping the adaptive flag already invalidates the
    # makefile so UBT rebuilds the affected unity blobs, and a clean would wipe the
    # precompiled host-project binaries (requiring a dev restore afterward).
    #
    # A whole-project full-unity build also surfaces the host project's own latent
    # collisions. Those are NOT Monolith's concern -- we filter the captured log to
    # Plugins\Monolith\ paths only. Only Monolith-path collision errors ship-block.
    Write-Host "`n  [$Tag 1a] Full-unity collision gate (issue #68 defense)..." -ForegroundColor Yellow

    $BuildConfigDir = Join-Path $env:APPDATA "Unreal Engine\UnrealBuildTool"
    $BuildConfigXml = Join-Path $BuildConfigDir "BuildConfiguration.xml"
    $BuildConfigBackup = "$BuildConfigXml.monolith-release-bak"
    $HadBuildConfig = Test-Path $BuildConfigXml
    $UnityLog = Join-Path $env:TEMP "Monolith_FullUnity_${Version}_$Tag.log"

    # Collision error codes that full-unity concatenation surfaces but -DisableUnity
    # cannot: redefinition / multiple-definition / ambiguous-overload classes.
    #   C2084 - function already has a body
    #   C2011 - type redefinition
    #   C2086 - identifier redefinition
    #   C2027 - use of undefined type (often a downstream of conflicting decls)
    #   C2374 - redefinition / multiple initialization
    #   C2668 - ambiguous call to overloaded function
    $CollisionCodes = @("C2084", "C2011", "C2086", "C2027", "C2374", "C2668")

    $env:MONOLITH_RELEASE_BUILD = "1"
    Write-Host "    [$Tag] MONOLITH_RELEASE_BUILD=1 (release config)" -ForegroundColor DarkGray

    try {
        # Back up the existing BuildConfiguration.xml (if any) so we can restore it
        # verbatim in finally. Then write one that forces full unity.
        if (-not (Test-Path $BuildConfigDir)) {
            New-Item -ItemType Directory -Path $BuildConfigDir -Force | Out-Null
        }
        if ($HadBuildConfig) {
            Copy-Item $BuildConfigXml $BuildConfigBackup -Force
            Write-Host "    [$Tag] Backed up existing BuildConfiguration.xml" -ForegroundColor DarkGray
        }

        $ForceUnityXml = @"
<?xml version="1.0" encoding="utf-8" ?>
<Configuration xmlns="https://www.unrealengine.com/BuildConfiguration">
  <BuildConfiguration>
    <bUseAdaptiveUnityBuild>false</bUseAdaptiveUnityBuild>
  </BuildConfiguration>
</Configuration>
"@
        Set-Content -Path $BuildConfigXml -Value $ForceUnityXml -Encoding utf8
        Write-Host "    [$Tag] Forced bUseAdaptiveUnityBuild=false (full unity)" -ForegroundColor DarkGray
        Write-Host "    [$Tag] Building (no -DisableUnity, no -Clean -- host-binary hazard)..." -ForegroundColor DarkGray

        # Capture the full UBT output so we can scan it for Monolith-path collisions.
        # We do NOT throw on a non-zero UBT exit here directly -- a collision is
        # itself a non-zero exit, and we want the filtered diagnostic, not a bare
        # "exit code" message. The collision scan below is the real ship-gate.
        & $UBT $Engine.Target Win64 Development "-Project=$($Engine.UProject)" -waitmutex 2>&1 |
            Tee-Object -FilePath $UnityLog | Out-Null
        $unityExit = $LASTEXITCODE

        # Scan the captured log for collision error codes on Plugins\Monolith\ paths.
        # MSVC emits errors like:
        #   D:\...\Plugins\Monolith\Source\...\Foo.cpp(42): error C2084: ...
        $logLines = if (Test-Path $UnityLog) { Get-Content $UnityLog } else { @() }
        $codeAlt = ($CollisionCodes -join "|")
        $monolithCollisions = @($logLines | Where-Object {
            $_ -match "Plugins\\Monolith\\" -and $_ -match "error\s+($codeAlt)\b"
        })

        if ($monolithCollisions.Count -gt 0) {
            Write-Host "`n  [FAIL] [$Tag] Full-unity gate found $($monolithCollisions.Count) Monolith-path collision error(s):" -ForegroundColor Red
            $monolithCollisions | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
            Write-Host "`n  This is the issue #68 failure mode: duplicate file-local symbols" -ForegroundColor Red
            Write-Host "  (anonymous-namespace or file-static names shared across .cpp files in" -ForegroundColor Red
            Write-Host "  one module) that -DisableUnity cannot see. End users build with adaptive" -ForegroundColor Red
            Write-Host "  unity and hit this on a fresh clone. Rename or hoist the colliding symbols" -ForegroundColor Red
            Write-Host "  before shipping. Refusing to publish v$Version." -ForegroundColor Red
            throw "[$Tag] Full-unity collision gate failed: $($monolithCollisions.Count) Monolith-path collision(s)."
        }

        # No Monolith-path collisions. If UBT still failed, surface that -- a release
        # build that does not compile under full unity is not shippable either.
        if ($unityExit -ne 0) {
            Write-Host "`n  [FAIL] [$Tag] Full-unity build exited $unityExit (no Monolith-path collisions, but build failed)." -ForegroundColor Red
            Write-Host "    See full log: $UnityLog" -ForegroundColor Yellow
            throw "[$Tag] Full-unity build failed with exit code $unityExit. Is the editor closed? See $UnityLog."
        }

        Write-Host "    [$Tag] Full-unity gate passed (no Monolith-path collisions)" -ForegroundColor Green
    }
    finally {
        # ALWAYS restore the BuildConfiguration.xml to its original state, even on
        # failure -- otherwise the dev's next build silently runs under full unity.
        if ($HadBuildConfig) {
            if (Test-Path $BuildConfigBackup) {
                Copy-Item $BuildConfigBackup $BuildConfigXml -Force
                Remove-Item $BuildConfigBackup -Force -ErrorAction SilentlyContinue
                Write-Host "    [$Tag] Restored original BuildConfiguration.xml" -ForegroundColor DarkGray
            }
        } else {
            # There was no BuildConfiguration.xml before -- remove the one we wrote.
            Remove-Item $BuildConfigXml -Force -ErrorAction SilentlyContinue
            Write-Host "    [$Tag] Removed temporary BuildConfiguration.xml (none existed before)" -ForegroundColor DarkGray
        }
        Remove-Item Env:\MONOLITH_RELEASE_BUILD -ErrorAction SilentlyContinue
        Remove-Item $UnityLog -Force -ErrorAction SilentlyContinue
        Write-Host "    [$Tag] MONOLITH_RELEASE_BUILD unset" -ForegroundColor DarkGray
    }
    }
    finally {
        # Restore the EXACT original .uproject bytes (byte-for-byte), regardless of build or
        # gate outcome -- so a failure (or success) never leaves the project's .uproject with
        # the temporary plugin-disable edits. WriteAllBytes overwrites with the verbatim
        # snapshot taken before any mutation, so formatting/encoding is preserved exactly.
        [System.IO.File]::WriteAllBytes($Engine.UProject, $UProjectBackup)
        Write-Host "    [$Tag] Restored original .uproject (byte-exact)" -ForegroundColor DarkGray
    }
}

# Harvest ONE engine's Binaries/Win64 into the staged content (already populated with the
# shared tracked files), patch the .uplugin, and zip to that engine's per-engine zip path.
# The tracked content is identical across engines -- only the Binaries differ -- so we copy
# the shared $TempDir into a per-engine scratch dir, drop in this engine's binaries, then
# zip. Returns nothing; throws on failure.
function Invoke-EnginePackage {
    param([PSCustomObject]$Engine, [string]$SharedContentDir)

    $Tag = $Engine.Tag
    Write-Host "`n  [$Tag pkg] Packaging $($Engine.Zip | Split-Path -Leaf)..." -ForegroundColor Yellow

    # Per-engine scratch: a fresh copy of the shared tracked content. We do NOT mutate the
    # shared dir so the next engine reuses it untouched.
    $StageDir = Join-Path $env:TEMP "Monolith_Release_${Version}_$Tag"
    if (Test-Path $StageDir) { Remove-Item $StageDir -Recurse -Force }
    New-Item -ItemType Directory -Path $StageDir | Out-Null
    Copy-Item -Path (Join-Path $SharedContentDir '*') -Destination $StageDir -Recurse -Force

    # --- Copy this engine's binaries (gitignored but needed for Blueprint-only users) ---
    # FLAT normal layout under Binaries\Win64 -- NOT engine subfolders. The Binaries source
    # is THIS engine's plugin clone's Binaries\Win64 (UE5.8 binaries come from the FIVEPOINT8
    # clone). We copy the ENTIRE Win64 dir (UnrealEditor-*.dll + the required
    # UnrealEditor.modules manifest, which is per-engine and tells UE where to load each
    # plugin module) but skip .pdb / .patch_* / .claude as everywhere else, and skip stripped
    # sibling DLLs. The engine-agnostic top-level offline tools (Binaries\monolith_query.exe,
    # monolith_proxy.exe) are NOT under Win64 and were already staged into the shared content
    # from THIS repo's freshly-built, parity-verified exe -- so the UE5.8 clone's possibly-
    # stale offline exe never overwrites the verified one.
    $binWin64 = Join-Path $Engine.PluginDir "Binaries\Win64"
    if (Test-Path $binWin64) {
        $destWin64 = Join-Path $StageDir "Binaries\Win64"
        if (-not (Test-Path $destWin64)) { New-Item -ItemType Directory -Path $destWin64 -Force | Out-Null }
        $binCount = 0
        $binStripCount = 0
        # Build a regex that matches any stripped module's binary.
        $stripModuleRegex = "(" + (($StrippedModules | ForEach-Object { [regex]::Escape($_) }) -join "|") + ")"
        # Exclude gitignored dot-directories (e.g. .claude) that may exist physically under
        # Binaries/. git ls-files never tracks them, but the Binaries copy below walks the
        # physical directory -- without this guard such a directory and its contents could
        # ship in the public zip. No legitimate Binaries content lives under a dot-directory.
        Get-ChildItem $binWin64 -Recurse -File |
            Where-Object { $_.Extension -ne '.pdb' -and $_.Name -notmatch '\.patch_' -and $_.FullName -notmatch '[\\/]\.claude[\\/]' } |
            ForEach-Object {
                if ($StrippedModules.Count -gt 0 -and $_.Name -match "UnrealEditor-$stripModuleRegex\.") {
                    $binStripCount++
                    return
                }
                $rel = $_.FullName.Substring($binWin64.Length)
                $dest = Join-Path $destWin64 $rel
                $destParent = Split-Path -Parent $dest
                if (-not (Test-Path $destParent)) { New-Item -ItemType Directory -Path $destParent -Force | Out-Null }
                Copy-Item $_.FullName -Destination $dest -Force
                $binCount++
            }
        Write-Host "    [$Tag] $binCount Win64 binary file(s) included (no .pdb, no .patch_*, $binStripCount stripped)" -ForegroundColor Green
    } else {
        Write-Host "    [$Tag] WARNING: No Binaries\Win64 found at $binWin64 - Blueprint-only users will need to compile" -ForegroundColor Red
    }

    # --- Patch the .uplugin: Installed=true + strip sibling module entries ---
    # NOTE: we intentionally do NOT add an EngineVersion key. Per-engine zips are
    # distinguished by their asset name (-UE5.7 / -UE5.8) and the updater selects on that;
    # pinning EngineVersion in the .uplugin would block source users on adjacent patches.
    $upluginPath = Join-Path $StageDir "Monolith.uplugin"
    $content = Get-Content $upluginPath -Raw
    $content = $content -replace '"Installed":\s*false', '"Installed": true'

    # Strip non-redistributable module entries from the "Modules" array.
    $upluginStrips = 0
    foreach ($mod in $StrippedModules) {
        # Match the module object + its trailing comma (if present). Do NOT consume the
        # leading comma -- that belongs to the previous entry and must stay. If the stripped
        # module was the LAST array entry (no trailing comma), the previous entry's trailing
        # comma is orphaned; the ",]" cleanup below catches that.
        $escMod = [regex]::Escape($mod)
        $pattern = "(?s)\{\s*""Name"":\s*""$escMod"".*?\}\s*,?\s*"
        $before = $content.Length
        $content = [regex]::Replace($content, $pattern, "")
        if ($content.Length -ne $before) { $upluginStrips++ }
    }
    # Strip any trailing comma immediately before a closing array bracket.
    $content = $content -replace ',(\s*\])', '$1'
    Set-Content $upluginPath $content -NoNewline
    Write-Host "    [$Tag] Installed=true set in .uplugin, $upluginStrips module entries stripped (no EngineVersion key)" -ForegroundColor Green

    # --- Create the per-engine zip ---
    if (Test-Path $Engine.Zip) { Remove-Item $Engine.Zip -Force }
    Compress-Archive -Path "$StageDir\*" -DestinationPath $Engine.Zip -Force

    # Clean this engine's scratch
    Remove-Item $StageDir -Recurse -Force -ErrorAction SilentlyContinue

    $fileSize = [math]::Round((Get-Item $Engine.Zip).Length / 1MB, 1)
    Write-Host "    [$Tag] Zip built: $($Engine.Zip) (${fileSize}MB)" -ForegroundColor Green
}

# Run the mandatory dumpbin hard-link import smoke against ONE engine's zip. Returns the
# lowercased SHA256 of the smoked zip (pinned to the verified bytes). Throws/exits on a
# sentinel leak or (unless -AllowUnverifiedImports) on a missing dumpbin.
function Invoke-EngineSmoke {
    param([PSCustomObject]$Engine, [string]$DumpbinPath, [string[]]$Sentinels)

    $Tag = $Engine.Tag
    Write-Host "`n  [$Tag smoke] Post-build hard-link smoke (issue #30 defense)..." -ForegroundColor Yellow

    if (-not $DumpbinPath) {
        if ($AllowUnverifiedImports) {
            Write-Host "    [$Tag] [WARN] dumpbin.exe not found AND -AllowUnverifiedImports set." -ForegroundColor Yellow
            Write-Host "    [$Tag] [WARN] Hard-link import smoke SKIPPED -- this zip is NOT shippable." -ForegroundColor Yellow
            Write-Host "    [$Tag] [WARN] Imports are UNVERIFIED. Do NOT publish this build (issue #71 risk)." -ForegroundColor Yellow
            return $null  # caller treats null as 'unverified' (no pinned hash)
        } else {
            Write-Host "`n  [FAIL] dumpbin.exe not found -- cannot run the mandatory hard-link import smoke." -ForegroundColor Red
            Write-Host "  Install Visual Studio Build Tools (provides dumpbin.exe) and re-run." -ForegroundColor Red
            Write-Host "  Shipping without this check is how issue #71 (MassSpawner/ZoneGraph hard-link) escaped." -ForegroundColor Red
            Write-Host "  Bypass ONLY for a non-shippable local test zip: re-run with -AllowUnverifiedImports." -ForegroundColor Red
            exit 1
        }
    }

    # Re-extract the just-built zip into a scratch dir to inspect the actual shipped DLLs
    # (not the dev binaries we may have overwritten before zipping).
    $SmokeDir = Join-Path $env:TEMP "Monolith_Release_${Version}_${Tag}_Smoke"
    if (Test-Path $SmokeDir) { Remove-Item $SmokeDir -Recurse -Force }
    New-Item -ItemType Directory -Path $SmokeDir | Out-Null
    Expand-Archive -Path $Engine.Zip -DestinationPath $SmokeDir -Force

    $MonolithDlls = @(Get-ChildItem -Path $SmokeDir -Recurse -Filter "UnrealEditor-Monolith*.dll")
    $LeakingDlls = @()
    foreach ($dllItem in $MonolithDlls) {
        $imports = & $DumpbinPath /imports $dllItem.FullName 2>$null | Out-String
        foreach ($sentinel in $Sentinels) {
            # Match "UnrealEditor-<Sentinel>.dll" in the import table
            if ($imports -match "UnrealEditor-$([regex]::Escape($sentinel))\.dll") {
                $LeakingDlls += [PSCustomObject]@{ Dll = $dllItem.Name; Sentinel = $sentinel }
            }
        }
    }

    # Cleanup smoke dir regardless of outcome
    Remove-Item $SmokeDir -Recurse -Force -ErrorAction SilentlyContinue

    if ($LeakingDlls.Count -gt 0) {
        Write-Host "`n  [FAIL] [$Tag] Hard-link smoke found $($LeakingDlls.Count) sentinel import(s) in shipped DLLs:" -ForegroundColor Red
        $LeakingDlls | ForEach-Object {
            Write-Host "    $($_.Dll) imports UnrealEditor-$($_.Sentinel).dll" -ForegroundColor Red
        }
        Write-Host "`n  This is the issue #30 failure mode. The Build.cs for the affected module" -ForegroundColor Red
        Write-Host "  is not honouring MONOLITH_RELEASE_BUILD=1. Fix the Build.cs probe before shipping." -ForegroundColor Red
        Write-Host "`n  Refusing to publish v$Version. Delete $($Engine.Zip) after fixing Build.cs and re-run." -ForegroundColor Red
        exit 1
    }
    Write-Host "    [$Tag] No sentinel imports found in $($MonolithDlls.Count) Monolith DLLs (clean)" -ForegroundColor Green

    # Pin the verified bytes by hash so the printed SHA can be asserted to come from THIS
    # smoked artifact and not a post-smoke manual re-zip.
    return (Get-FileHash -Algorithm SHA256 -Path $Engine.Zip).Hash.ToLower()
}

# =====================================================================================
# Shared, engine-agnostic prep (runs ONCE): offline CLI build + parity gate, then the
# shared tracked-content copy. Both zips ship the SAME tracked content.
# =====================================================================================

# --- Build both engines BEFORE packaging so a build/gate failure on EITHER engine aborts
#     the whole release with no zip produced (no partial release). ---
if (-not $SkipBuild) {
    foreach ($eng in $EngineMatrix) {
        Invoke-EngineBuild -Engine $eng
    }
} else {
    Write-Host "`n  [build] Skipping ALL engine builds (-SkipBuild flag)" -ForegroundColor DarkGray
    Write-Host "    WARNING: Ensure each engine's binaries were built with MONOLITH_RELEASE_BUILD=1" -ForegroundColor Red
}

# --- Offline CLI build + parity gate (engine-agnostic; tracked source, runs ONCE) ---
# The offline tool Binaries/monolith_query.exe is built from tracked source
# Tools/MonolithQuery/monolith_query.cpp via a standalone cl.exe build (NOT UBT).
# Binaries/ is gitignored, so without this step the release would ship whatever
# stale exe happened to sit on disk. Rebuild it here so the shipped exe matches
# the shipped source, then hard-gate the exe-vs-py parity guard. A drifted exe
# must never ship -- both the build failure and a parity FAIL abort the release.
# This exe is identical for both engines (it is not an engine binary), so the freshly
# built exe under THIS repo's Binaries/ is staged into the shared content copy below.
Write-Host "`n  [offline] Building offline CLI fresh + parity gate..." -ForegroundColor Yellow

$ToolDir = Join-Path $PluginDir "Tools\MonolithQuery"
$ToolBuildBat = Join-Path $ToolDir "build.bat"
if (-not (Test-Path $ToolBuildBat)) {
    throw "Offline CLI build script not found at $ToolBuildBat"
}

# Locate vcvars64.bat via vswhere so cl.exe is on PATH for build.bat.
$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $VsWhere)) {
    throw "vswhere.exe not found at $VsWhere. Visual Studio is required to build the offline CLI."
}
$VsInstallPath = & $VsWhere -latest -property installationPath
if (-not $VsInstallPath) {
    throw "vswhere could not locate a Visual Studio installation."
}
$VcVars = Join-Path $VsInstallPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $VcVars)) {
    throw "vcvars64.bat not found at $VcVars."
}

# Run vcvars64.bat then build.bat in a single cmd session so cl.exe is in PATH.
# build.bat copies the freshly built exe to Plugins/Monolith/Binaries/ (the same
# Binaries dir the copy step below picks up), so the exe is staged before packaging.
Write-Host "    Using VS at $VsInstallPath" -ForegroundColor DarkGray
# Three robustness measures, learned the hard way during the v0.18.0 release:
#   1. Prepend the VS *Installer* dir to PATH. vcvars64.bat calls 'vswhere' by bare
#      name (expecting it on PATH); where the Installer dir is NOT on PATH it prints
#      "'vswhere.exe' is not recognized" to STDERR. vcvars still recovers and inits
#      x64 fine -- but that stray STDERR line is what bites under measure #3.
#   2. Redirect the cmd subprocess STDERR into STDOUT (2>&1) and capture to a log so any
#      real build failure is diagnosable from the captured tail.
#   3. THE ACTUAL FIX: relax $ErrorActionPreference to 'Continue' around the native call.
#      This script runs under EAP='Stop'. Under Stop, PS 5.1 promotes ANY native-command
#      STDERR to a TERMINATING error -- EVEN through a 2>&1 merge (verified: Stop throws on
#      the vswhere line, Continue exits 0) -- aborting the release BEFORE the $LASTEXITCODE
#      gate, even though build.bat itself exits 0. Foreground/interactive hosts default to
#      Continue, which masked this for every prior release. With EAP relaxed, the exit code
#      is the sole arbiter; restore the prior preference immediately after.
$VsInstallerDir = Split-Path $VsWhere -Parent
$CliBuildLog = Join-Path $env:TEMP "Monolith_OfflineCLIBuild_$Version.log"
$PrevEAP = $ErrorActionPreference
Push-Location $ToolDir
try {
    $env:PATH = "$VsInstallerDir;$env:PATH"
    $ErrorActionPreference = 'Continue'
    & cmd.exe /c "call `"$VcVars`" && call `"$ToolBuildBat`"" 2>&1 |
        Tee-Object -FilePath $CliBuildLog | Out-Null
    $CliExit = $LASTEXITCODE
    $ErrorActionPreference = $PrevEAP
    if ($CliExit -ne 0) {
        if (Test-Path $CliBuildLog) {
            Write-Host "    --- offline CLI build log (tail) ---" -ForegroundColor Yellow
            Get-Content $CliBuildLog -Tail 20 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
        }
        throw "Offline CLI build failed with exit code $CliExit. See $CliBuildLog."
    }
}
finally {
    $ErrorActionPreference = $PrevEAP
    Pop-Location
    Remove-Item $CliBuildLog -Force -ErrorAction SilentlyContinue
}
Write-Host "    Offline CLI built (fresh exe staged in Binaries/)" -ForegroundColor Green

# Hard-gate: the staged exe must not be stale relative to its source. This guard
# was documented in SPEC_CORE.md / API_REFERENCE.md but invoked by nothing, so a
# stale exe could ship with every other gate green. Note its scope: it hashes
# Tools/MonolithQuery/monolith_query.cpp ONLY, so it does NOT cover a build
# failure originating in ThirdParty/sqlite3.c, a ThirdParty header, or the
# toolchain -- build.bat's exit code is the sole cover for that class.
# Exit codes: 0 fresh / 1 stale / 4 preflight-fail; -ne 0 covers all of them.
# Placement is load-bearing: the exe has just been staged into Binaries/, the
# content copy that ships it has not run yet, and no zip exists -- so a throw
# here aborts with no artifact written.
Write-Host "    Running offline exe freshness guard (check_offline_exe_fresh.py)..." -ForegroundColor DarkGray
$FreshScript = Join-Path $PluginDir "Scripts\check_offline_exe_fresh.py"
& python $FreshScript
if ($LASTEXITCODE -ne 0) {
    throw "Offline exe is STALE relative to its source (exit $LASTEXITCODE). Refusing to ship."
}
Write-Host "    Offline exe freshness verified" -ForegroundColor Green

# Hard-gate: the freshly built exe must deep-equal its Python sibling across all
# RI actions. A non-zero exit means the two offline tools drifted -- abort.
Write-Host "    Running offline parity guard (verify_offline_parity.py)..." -ForegroundColor DarkGray
$ParityScript = Join-Path $PluginDir "Scripts\verify_offline_parity.py"
& python $ParityScript
if ($LASTEXITCODE -ne 0) {
    throw "Offline parity guard FAILED (exit $LASTEXITCODE). The exe drifted from its Python sibling. Refusing to ship a drifted offline CLI."
}
Write-Host "    Offline parity guard PASSED" -ForegroundColor Green

# --- Shared tracked-content copy (ONCE) ---
# Both per-engine zips ship the IDENTICAL tracked content from THIS repo (git ls-files);
# only the per-engine Binaries/Win64 differs. So copy the tracked files once into $TempDir
# and reuse it for every engine's package step.
Write-Host "`n  [content] Copying tracked files (shared across engines)..." -ForegroundColor Yellow

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
    # Strip internal testing-execution records (Docs/testing/) -- per-feature test-pass
    # diaries with no downstream consumer value. Public-facing test artefacts live
    # elsewhere (SPEC sections, automation tests under Source/).
    if ($keep -and $path -like "Docs/testing/*") {
        $keep = $false
    }
    # Strip internal-only Docs that the project tracks in git but should not ship.
    # MISSING_FEATURES.md is the empirical gap log fed from real project work;
    # downstream consumers do not need it.
    if ($keep -and $path -eq "Docs/MISSING_FEATURES.md") {
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

# Stage the freshly built offline CLI exe + its sibling tools into the shared content's
# Binaries dir. These are engine-agnostic and live at the TOP LEVEL of THIS repo's
# Binaries/ (Binaries\monolith_query.exe, Binaries\monolith_proxy.exe). We copy ONLY the
# top-level files here (NOT -Recurse) so we never touch Binaries\Win64 -- the per-engine
# UnrealEditor-*.dll set AND the per-engine UnrealEditor.modules manifest are added under
# Win64 in each package step (Invoke-EnginePackage), so they correctly differ per engine.
# Skip .pdb / .patch_* / .claude as everywhere else.
$sharedBinSrc = Join-Path $PluginDir "Binaries"
if (Test-Path $sharedBinSrc) {
    $sharedBinDest = Join-Path $TempDir "Binaries"
    if (-not (Test-Path $sharedBinDest)) { New-Item -ItemType Directory -Path $sharedBinDest -Force | Out-Null }
    Get-ChildItem $sharedBinSrc -File |
        Where-Object {
            $_.Extension -ne '.pdb' -and $_.Name -notmatch '\.patch_' -and
            $_.FullName -notmatch '[\\/]\.claude[\\/]'
        } |
        ForEach-Object {
            Copy-Item $_.FullName -Destination (Join-Path $sharedBinDest $_.Name) -Force
        }
    Write-Host "    Shared engine-agnostic Binaries (top-level offline tools) staged" -ForegroundColor DarkGray
}

# =====================================================================================
# Sentinel list + drift assertion (engine-agnostic; the SAME optional-gated modules apply
# to both engines), then locate dumpbin once, then package + smoke EACH engine.
# =====================================================================================

# Sentinel modules: their presence in a Monolith DLL's imports = build-time gate failure.
# Add new sentinels when adding new optional plugin integrations.
#
# GameplayAbilities removed from sentinels in v0.14.7: it's declared as a hard
# dep in Monolith.uplugin (no Optional flag), so the engine auto-enables it on
# Monolith install and guarantees load order. MonolithGAS + MonolithIndex
# hard-link GameplayAbilities and that's functionally safe under this contract.
#
# Each entry is the EXACT module-name string a Build.cs adds under an optional gate.
# The smoke matches "UnrealEditor-<entry>.dll" in a shipped DLL's import table, so a
# casing/name mismatch here means a real leak slips past (issue #71: MassSpawner was
# missing entirely; MetaSoundEngine/MetaSoundFrontend had wrong casing -- the engine
# DLLs are UnrealEditor-MetasoundEngine.dll, lowercase 's'; ComboGraphRuntime and
# LogicDriver named no real module DLL). The $OptionalModuleUnion drift assertion below
# is the source of truth -- if it FAILs, add the missing name HERE (and keep it sorted
# by source module for review sanity).
$LeakSentinels = @(
    # MonolithMesh -- GeometryScripting (delay-loaded but still gated)
    "GeometryScriptingCore", "GeometryFramework", "GeometryCore",
    # MonolithUI -- CommonUI
    "CommonUI", "CommonInput",
    # MonolithUI -- MVVM / UMG Viewmodel (issue #173; engine plugin, EnabledByDefault:false)
    "ModelViewViewModel", "ModelViewViewModelBlueprint", "ModelViewViewModelEditor",
    # MonolithBABridge -- BlueprintAssist
    "BlueprintAssist",
    # MonolithAI -- GameplayBehaviors / MassEntity / ZoneGraph / StateTree / SmartObjects
    "GameplayBehaviorsModule", "MassEntity", "MassSpawner", "MassGameplayEditor", "ZoneGraph",
    "StateTreeModule", "StateTreeEditorModule", "GameplayStateTreeModule", "PropertyBindingUtils",
    "SmartObjectsModule", "SmartObjectsEditorModule",
    # MonolithGAS -- GBA (Blueprint Attributes). NOTE: GameplayAbilities is deliberately NOT a
    # sentinel -- it is a hard dep in Monolith.uplugin (Enabled, no Optional), so the engine
    # auto-enables it and guarantees its DLL is present; MonolithGAS/MonolithIndex hard-link it
    # safely (removed from sentinels in v0.14.7, see the rationale block above).
    "BlueprintAttributes",
    # MonolithIndex / MonolithAudio -- MetaSound (engine DLLs are 'Metasound', lowercase s)
    "MetasoundEngine", "MetasoundFrontend", "MetasoundEditor",
    # MonolithAnimation -- Chooser
    "Chooser",
    # MonolithComboGraph -- ComboGraph (was stale 'ComboGraphRuntime')
    "ComboGraph", "ComboGraphEditor",
    # MonolithLogicDriver -- Logic Driver Pro / SMSystem (was stale 'LogicDriver')
    "SMSystem", "SMSystemEditor"
)

# --- Drift assertion: $LeakSentinels must be a superset of every optional-gated module ---
# Single source of truth so the next optional dep cannot silently slip past the smoke
# (issue #71 root cause: MassSpawner was added to a Build.cs optional block but never to
# the sentinel list, so the leak shipped). We use an explicit expected-union array rather
# than parsing the .Build.cs files at release time: block-scoped PowerShell parsing of C#
# is brittle (multi-line AddRange, nested if, comments), and a false-negative parse would
# REINTRODUCE the exact silent-gap failure this guard exists to kill. The union below is
# the auditor-verified list of module strings each Source/*/*.Build.cs adds inside an
# optional/release-gated block (if (bHas...) / if (!bReleaseBuild...)). When you add a new
# optional dep to ANY Build.cs, add its module string(s) HERE and to $LeakSentinels above.
$OptionalModuleUnion = @(
    "GeometryScriptingCore", "GeometryFramework", "GeometryCore",   # MonolithMesh
    "CommonUI", "CommonInput",                                       # MonolithUI
    "ModelViewViewModel", "ModelViewViewModelBlueprint",             # MonolithUI (MVVM)
    "ModelViewViewModelEditor",                                      # MonolithUI (MVVM)
    "BlueprintAssist",                                               # MonolithBABridge
    "GameplayBehaviorsModule", "MassEntity", "MassSpawner",          # MonolithAI
    "MassGameplayEditor", "ZoneGraph",                              # MonolithAI
    "StateTreeModule", "StateTreeEditorModule",                     # MonolithAI
    "GameplayStateTreeModule", "PropertyBindingUtils",             # MonolithAI
    "SmartObjectsModule", "SmartObjectsEditorModule",              # MonolithAI
    # GameplayAbilities is optional-gated in MonolithAI but a HARD dep in Monolith.uplugin and
    # hard-linked unconditionally (and safely) in MonolithGAS/MonolithIndex -- so it is NOT a
    # sentinel and is excluded from this drift union (it is not an unsafe optional dep).
    "BlueprintAttributes",                                          # MonolithGAS
    "MetasoundEngine", "MetasoundFrontend", "MetasoundEditor",      # MonolithIndex / MonolithAudio
    "Chooser",                                                      # MonolithAnimation
    "ComboGraph", "ComboGraphEditor",                              # MonolithComboGraph
    "SMSystem", "SMSystemEditor"                                    # MonolithLogicDriver
)

$MissingSentinels = $OptionalModuleUnion | Where-Object { $LeakSentinels -notcontains $_ }
if ($MissingSentinels.Count -gt 0) {
    Write-Host "`n  [FAIL] Sentinel drift: optional-gated module(s) missing from `$LeakSentinels:" -ForegroundColor Red
    $MissingSentinels | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    Write-Host "`n  Every module added under an optional/release gate in a Source/*/*.Build.cs MUST" -ForegroundColor Red
    Write-Host "  be in `$LeakSentinels, or a hard-link leak can ship unflagged (issue #71)." -ForegroundColor Red
    Write-Host "  Add the missing name(s) to `$LeakSentinels above, then re-run." -ForegroundColor Red
    exit 1
}

# Locate dumpbin.exe -- ships with Visual Studio Build Tools. Try common locations,
# then a vswhere.exe lookup, before giving up. This smoke is MANDATORY: without it a
# release can ship with ZERO import verification (issue #71 root cause #1 -- the old
# code printed [SKIP] and continued, so a leak shipped). If dumpbin still can't be
# found we HARD FAIL; the only bypass is -AllowUnverifiedImports (never set for a real
# release -- it produces a NON-shippable test zip on a machine without VS Build Tools).
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

# Fallback: ask vswhere.exe (ships with every VS 2017+ installer) to locate dumpbin.exe
# anywhere a VS instance is installed -- covers non-2022 / non-default-edition layouts.
if (-not $Dumpbin) {
    $VSWhere2 = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $VSWhere2) {
        $vswhereHit = & $VSWhere2 -latest -find "**\dumpbin.exe" 2>$null | Select-Object -First 1
        if ($vswhereHit -and (Test-Path $vswhereHit)) { $Dumpbin = $vswhereHit }
    }
}

# =====================================================================================
# Package + smoke EACH engine. Collect per-engine pinned hashes for the SHA markers.
# Any smoke failure exits inside Invoke-EngineSmoke (no partial release).
# =====================================================================================
$EngineHashes = @{}   # Tag -> lowercased SHA256 of the smoked zip
$LegacyHash = $null

foreach ($eng in $EngineMatrix) {
    Invoke-EnginePackage -Engine $eng -SharedContentDir $TempDir
    $pinned = Invoke-EngineSmoke -Engine $eng -DumpbinPath $Dumpbin -Sentinels $LeakSentinels
    $EngineHashes[$eng.Tag] = $pinned
}

# --- Legacy bridge: copy the UE5.7 zip to Monolith-v<X.Y.Z>.zip so pre-cross-engine
#     auto-updaters (which fetch the first plain .zip and read the legacy SHA marker)
#     still get a valid, smoked artifact. ---
$LegacyEngine = $EngineMatrix | Where-Object { $_.IsLegacy } | Select-Object -First 1
if ($LegacyEngine) {
    if (Test-Path $LegacyZip) { Remove-Item $LegacyZip -Force }
    Copy-Item $LegacyEngine.Zip -Destination $LegacyZip -Force
    # The legacy zip is a byte-for-byte copy of the smoked UE5.7 zip, so its hash equals
    # the UE5.7 pinned hash. Recompute to be explicit (and to detect a copy that failed).
    $LegacyHash = (Get-FileHash -Algorithm SHA256 -Path $LegacyZip).Hash.ToLower()
    if ($EngineHashes[$LegacyEngine.Tag] -and $LegacyHash -ne $EngineHashes[$LegacyEngine.Tag]) {
        Write-Host "`n  [FAIL] Legacy bridge zip hash does not match the smoked UE5.7 zip." -ForegroundColor Red
        Write-Host "    UE5.7 smoked: $($EngineHashes[$LegacyEngine.Tag])" -ForegroundColor Red
        Write-Host "    legacy copy:  $LegacyHash" -ForegroundColor Red
        exit 1
    }
    Write-Host "`n  [legacy] Bridge zip written: $LegacyZip (= UE5.7 copy)" -ForegroundColor Green
}

# --- Cleanup shared temp ---
Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue

# =====================================================================================
# SHA256 markers for release notes (Issue #38 + per-engine contract). The updater anchors
# on the EXACT markers: "Monolith-SHA256-v2-UE5.7:" / "Monolith-SHA256-v2-UE5.8:" for
# engine-tagged assets, and "Monolith-SHA256-v2:" (= UE5.7 hash) for legacy-asset installs.
#
# "v2" GENERATION IS LOAD-BEARING (Issues #90/#94): updaters shipped in v0.14.7-v0.21.0
# fatally checkf-assert the editor mid-install when the release notes carry a marker they
# recognize (their integrity check calls FPlatformMisc::GetSHA256Signature, which has no
# Windows impl). The v2 names are invisible to those updaters, so they fail SAFE instead
# (per-engine parse aborts fail-closed; legacy parse proceeds unverified). NEVER emit the
# old "Monolith-SHA256:[-UE5.x]" names in any release body again.
# =====================================================================================
Write-Host ""
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host "Release complete: per-engine zips + legacy bridge" -ForegroundColor Green
foreach ($eng in $EngineMatrix) {
    $h = $EngineHashes[$eng.Tag]
    $sizeMb = [math]::Round((Get-Item $eng.Zip).Length / 1MB, 1)
    Write-Host "  $($eng.Zip | Split-Path -Leaf)  (${sizeMb}MB)" -ForegroundColor Green
    if ($h) {
        Write-Host "    SHA256: $h (imports verified)" -ForegroundColor DarkGray
    } else {
        Write-Host "    SHA256: UNVERIFIED (smoke bypassed -- NOT shippable)" -ForegroundColor Yellow
    }
}
if ($LegacyHash) {
    Write-Host "  $($LegacyZip | Split-Path -Leaf)  (legacy bridge = UE5.7 copy)" -ForegroundColor Green
}
Write-Host ""
Write-Host "Paste these EXACT lines into the GitHub Release notes body:" -ForegroundColor Yellow
Write-Host ""
foreach ($eng in $EngineMatrix) {
    $h = $EngineHashes[$eng.Tag]
    if ($h) {
        Write-Host "  Monolith-SHA256-v2-$($eng.Tag): $h" -ForegroundColor White
    } else {
        Write-Host "  Monolith-SHA256-v2-$($eng.Tag): <UNVERIFIED -- re-run without -AllowUnverifiedImports>" -ForegroundColor Yellow
    }
}
if ($LegacyHash) {
    Write-Host "  Monolith-SHA256-v2: $LegacyHash" -ForegroundColor White
}
Write-Host ""
Write-Host "The auto-updater (v0.21.1+) parses these exact markers and refuses to install" -ForegroundColor Yellow
Write-Host "if the downloaded zip's hash does not match. Engine-tagged assets require the" -ForegroundColor Yellow
Write-Host "matching Monolith-SHA256-v2-<tag>: marker; Monolith-SHA256-v2: (= the UE5.7" -ForegroundColor Yellow
Write-Host "hash) covers legacy-asset installs. Do not rename or reformat the markers --" -ForegroundColor Yellow
Write-Host "the prefix and a single space before the hex are required. NEVER emit the old" -ForegroundColor Yellow
Write-Host "pre-v2 marker names: v0.14.7-v0.21.0 updaters hard-crash on them (#90/#94)." -ForegroundColor Yellow
Write-Host "================================================================" -ForegroundColor Cyan

# =====================================================================================
# Dev-state restore reminder. The release builds left RELEASE-STRIPPED binaries on disk in
# BOTH projects (optional deps OFF). Each project's next editor launch would run stripped
# DLLs until a dev rebuild restores them. We do NOT auto-rebuild here (heavy + needs the
# editor closed in both); print the EXACT commands instead.
# =====================================================================================
Write-Host ""
Write-Host "================================================================" -ForegroundColor Magenta
Write-Host "RESTORE DEV BINARIES BEFORE RESUMING WORK (both projects)" -ForegroundColor Magenta
Write-Host "The release builds stripped optional deps (WITH_*=0) and left release DLLs on" -ForegroundColor Yellow
Write-Host "disk in BOTH projects. Rebuild WITHOUT MONOLITH_RELEASE_BUILD to restore them." -ForegroundColor Yellow
Write-Host "Touch a Build.cs first so UBT regenerates the makefile (it does not track the" -ForegroundColor Yellow
Write-Host "MONOLITH_RELEASE_BUILD env var and will otherwise report 'up to date')." -ForegroundColor Yellow
Write-Host ""
foreach ($eng in $EngineMatrix) {
    Write-Host "  # $($eng.Tag) dev restore:" -ForegroundColor Cyan
    Write-Host "  Get-ChildItem '$($eng.PluginDir)\Source' -Recurse -Filter *.Build.cs | ForEach-Object { `$_.LastWriteTime = Get-Date }" -ForegroundColor White
    Write-Host "  & '$($eng.UBT)' $($eng.Target) Win64 Development `"-Project=$($eng.UProject)`" -waitmutex" -ForegroundColor White
    Write-Host ""
}
Write-Host "(Ensure MONOLITH_RELEASE_BUILD is NOT set in your env when you run these.)" -ForegroundColor Yellow
Write-Host "================================================================" -ForegroundColor Magenta
