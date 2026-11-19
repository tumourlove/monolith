# Auto-Updater

Monolith includes a built-in auto-updater that checks GitHub Releases for new versions. **Off by default as of v0.14.6** — opt in via Editor Preferences > Plugins > Monolith > Auto Update Enabled.

## How It Works

1. **On editor startup** (5-second delay, only if opted in) — queries the GitHub API for the latest release
2. **Shows update dialog** — if a newer version is found, shows release notes and an Install button
3. **Selects the build for your engine** — each release ships one flat plugin zip per Unreal Engine version (`Monolith-vX.Y.Z-UE5.7.zip`, `Monolith-vX.Y.Z-UE5.8.zip`). The updater detects the engine the editor is running on and picks the matching asset automatically. If a release has no build for your engine, the updater tells you and installs nothing rather than staging a wrong-engine build (fail-closed). A legacy `Monolith-vX.Y.Z.zip` (the UE 5.7 build) is the fallback for older updaters that don't do engine-aware selection
4. **Downloads and verifies** — downloads the selected zip to `Saved/Monolith/Staging/` and verifies its SHA256 hash against the per-engine `Monolith-SHA256-v2-UE5.x:` marker in the release notes (matching the engine it downloaded for). The hash is computed with Monolith's own portable SHA-256 implementation — not the engine's platform hook, which has no Windows implementation and fatally asserts (the v0.21.0 updater-crash bug, #90/#94, fixed in v0.21.1). Mismatch aborts the install; a per-engine release missing its v2 marker also aborts (fail-closed). Releases with no checksum marker at all log a warning and proceed without integrity check (legacy release fallback).

   > **Why "v2" markers?** Updaters older than v0.21.1 crash the editor when they recognize a checksum marker in the release notes (the platform-hook assert above). Modern releases therefore publish markers under names old updaters don't recognize — old versions fail safe (a "missing integrity marker" notification instead of a crash; update manually), while v0.21.1+ verifies normally.
5. **Swaps on editor exit** — a detached batch script runs after the editor closes:
   - Polls `tasklist` until `UnrealEditor.exe` is gone
   - Asks for confirmation (Y/N)
   - Renames the old plugin folder to `Monolith_Backup`
   - Copies the new version into place
   - Preserves `.git/`, `.gitignore`, `.github/` for developer workflows
   - Cleans up backup and staging folders

## After Updating

- **C++ projects:** Rebuild your project before launching the editor (run UBT or build from your IDE)
- **Blueprint-only projects:** Launch the editor immediately — precompiled DLLs are included

## If the Plugin Fails to Load

If Monolith fails to load with a "modules are missing or built with a different engine version" error after a fresh install or update, you most likely have a **wrong-engine build** — the prebuilt binaries are locked to one engine version's ABI, so a 5.7 zip won't load in 5.8 and vice versa. The same error can appear if your engine's patch version bumped the ABI. To fix it, install the zip matching your engine (see [Installation](Installation)) or build Monolith from source — a source build works on either engine.

## Windows File Lock Handling

The updater retries the folder rename up to 10 times with 3-second gaps. This handles transient file locks from Windows Defender, Search Indexer, and shell extensions that can briefly hold DLL handles after the editor exits.

If all retries fail, the script reports the error and suggests checking for processes locking the folder. Adding the plugin folder to Windows Defender exclusions eliminates this issue permanently:

```powershell
Add-MpPreference -ExclusionPath "D:\YourProject\Plugins\Monolith"
```

## Manual Check

Use the `monolith_update` MCP tool:
- `action: "check"` — compare local version against latest release
- `action: "install"` — download and stage an update

## Disabling

Toggle off in **Project Settings > Plugins > Monolith > Auto Update Enabled**.

## Rollback

If the copy step fails, the updater automatically restores from the backup. Your original plugin folder is never deleted until the new version is fully copied and verified.
