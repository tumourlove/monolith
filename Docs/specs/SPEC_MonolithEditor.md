# Monolith — MonolithEditor Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.16.0 (Beta)

---

## MonolithEditor

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, Json, JsonUtilities, MessageLog, LiveCoding (Win64 only)

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithEditorModule` | Creates FMonolithLogCapture, attaches to GLog, registers 33 actions (20 base + 2 Phase J F8 map actions + 2 v0.14.8 PR #48 automation + 2 v0.14.9 Issue #50 scripting + 3 v0.14.10 PR #54 PIE/console + 4 v0.16.0 preview & inspection) |
| `FMonolithLogCapture` | FOutputDevice subclass. Ring buffer (10,000 entries max). Thread-safe. Tracks counts by verbosity |
| `FMonolithEditorActions` | Static handlers for build and log operations. Hooks into `ILiveCodingModule::GetOnPatchCompleteDelegate()` to capture compile results and timestamps |
| `FMonolithSettingsCustomization` | IDetailCustomization for UMonolithSettings. Adds re-index buttons for project and source databases in Project Settings UI |

### Actions (33 — namespace: "editor")

**Base (22 — v0.14.7 baseline + Phase J F8)**

| Action | Description |
|--------|-------------|
| `trigger_build` | Live Coding compile. `wait` param for synchronous. Windows-only. Auto-enables Live Coding |
| `live_compile` | Trigger Live Coding hot-reload compile. Alternative to trigger_build |
| `get_build_errors` | Build errors/warnings from log capture. Max 500 entries |
| `get_build_status` | Live Coding availability, started, enabled, compiling status |
| `get_build_summary` | Total error/warning counts + compile status |
| `search_build_output` | Search build log by `pattern`. Default limit 100 |
| `get_recent_logs` | Recent log entries. Default 100, max 1000 |
| `search_logs` | Search by `pattern`, `category`, `verbosity`, `limit` (max 2000) |
| `tail_log` | Last N lines formatted `[category][verbosity] message`. Default 50, max 500 |
| `get_log_categories` | List all active log categories seen in ring buffer |
| `get_log_stats` | Log stats: total, fatal, error, warning, log, verbose counts |
| `get_compile_output` | Structured compile report: result, time, log lines from compile categories (LogLiveCoding, LogCompile, LogLinker), error/warning counts, patch status. Time-windowed to last compile |
| `get_crash_context` | CrashContext.runtime-xml + Ensures.log + 20 recent errors. Truncated at 4096 chars |
| `capture_scene_preview` | Capture screenshot of Niagara or material asset in preview scene. Params: `asset_path`, `asset_type`, `seek_time`, `camera`, `resolution`, `output_path` |
| `capture_sequence_frames` | Multi-frame temporal capture at specified timestamps. Returns array of frame PNGs. Params: `asset_path`, `timestamps[]`, `camera`, `resolution` |
| `import_texture` | Import external image (PNG/TGA/EXR/HDR) as UTexture2D with settings (compression, sRGB, tiling, LOD group). Params: `source_path`, `destination`, `settings` |
| `stitch_flipbook` | Stitch multiple texture assets into a flipbook atlas. Params: `frames[]`, `columns`, `save_path` |
| `delete_assets` | Delete one or more assets by path. Params: `asset_paths[]`, `force` |
| `get_viewport_info` | Get active editor viewport camera location, rotation, FOV, resolution, realtime state |
| `create_empty_map` | **Phase J F8.** Create a fully blank UWorld asset at `path` and save the package. v1 supports `map_template="blank"` only. Errors cleanly on path collision, malformed package path, factory/save failure |
| `get_module_status` | **Phase J F8.** Report `{ module_name, plugin_name, enabled, loaded, is_runtime, version? }` for the named modules (or all Monolith modules if `module_names` is omitted). Unknown modules return `enabled=false / loaded=false / plugin_name=""` without error |

**Capture (1 additional from v0.14.7+)**

| Action | Description |
|--------|-------------|
| `capture_system_gif` | Capture a Niagara system as a sequence of PNG frames with optional GIF encoding via ffmpeg or Python. Params: `asset_path`, `duration_seconds`, `fps`, `resolution`, `output_path`, `encoder` (frames_only/ffmpeg/python). |

**Automation (2 — v0.14.8, PR #48 by @MaxenceEpitech)**

| Action | Description |
|--------|-------------|
| `list_automation_tests` | List all registered automation tests, optionally filtered by `prefix`. |
| `run_automation_tests` | Run automation tests by `prefix` in the running editor (no PIE, no separate process). Returns `{ success, total, passed, failed, skipped, results[] }`. Latent / async tests reported as `skipped`. |

**Scripting (2 — v0.14.9, Issue #50 ported from @JCSopko)**

| Action | Description |
|--------|-------------|
| `run_python` | Execute Python via `IPythonScriptPlugin::ExecPythonCommandEx`. Modes: `execute_file`, `execute_statement`, `evaluate_statement`. Returns success, captured Python log output (typed: info/warning/error), and evaluated result for `evaluate_statement`. |
| `load_level` | Wraps `ULevelEditorSubsystem::LoadLevel(AssetPath)`. Single-arg map swap; closes current persistent level without saving. |

**PIE Control (3 — v0.14.10, PR #54 by @MaxenceEpitech)**

| Action | Description |
|--------|-------------|
| `start_pie` | Begin a PIE session pinned to in-viewport mode (`EPlaySessionWorldType::PlayInEditor` + first active level viewport via `FLevelEditorModule::GetFirstActiveViewport`). Independent of the user's `LastExecutedPlayModeType` toolbar choice. Returns `started: true, mode: 'in_viewport'`. Refuses to queue duplicates when PIE is already running. |
| `stop_pie` | End the active PIE session via `GUnrealEd->RequestEndPlayMap()`. No-op (returns `stopped: false`) if PIE not active. |
| `run_console_command` | Execute a console command. Routes to the first PIE PlayerController found (multi-client PIE not disambiguated); falls back to `GEngine->Exec` (with null-guard) when no PIE session is active. |

**Preview & Inspection (4 — v0.16.0)**

| Action | Description |
|--------|-------------|
| `capture_material_grid` | Render N material instances side-by-side under shared HDRI/floor/lighting in a single `FAdvancedPreviewScene` + `USceneCaptureComponent2D` capture pass. Auto-grid layout via `ceil(sqrt(N))` with optional `columns` override. Params: `material_paths[]`, `output_path`, `resolution`, `columns`, `preview_mesh`, `camera`. Failed loads logged + skipped; `material_count` reflects successes only. |
| `capture_with_overlay` | Single-asset capture under one of 5 engine debug show-flags: `wireframe`, `normals`, `uv_density`, `lightmap_density`, `shader_complexity`. Toggles the matching `FEngineShowFlags` setter before `CaptureScene`. Params: `asset_path`, `mode`, `output_path`, `resolution`, `camera`. UE 5.7 has no public normal-visualiser setter — `normals` mode falls back to `SetMeshEdges`; external name preserved for future engine versions. |
| `inspect_material_pbr` | Reflective walk of a material's texture parameter list, classifying each by PBR slot (basecolor/normal/roughness/metallic) and detecting ORM / ARM / MRA channel-packing conventions by substring. Pure JSON, no rendering. Returns scalar/vector/texture parameter lists per slot plus packed-channel flags. `material_class` distinguishes Material / MaterialInstance / MaterialInstanceConstant / MaterialInstanceDynamic. |
| `inspect_texture_channels` | `LockMipReadOnly` on `UTexture2D` source mip 0; walks pixel buffer per channel for min/max/mean statistics. Returns width, height, runtime pixel format, sRGB flag, has_alpha flag. Optional `emit_splits=true` writes 4 grayscale-replicated BGRA8 PNGs to `output_dir` for per-channel visual diff. Non-BGRA8 source returns a clean warning payload rather than mis-decoded bytes. |

Plus `capture_scene_preview` (in Base section above) was **extended** in v0.16.0: `asset_type` enum now also accepts `static_mesh`, `skeletal_mesh` (with optional `animation_path` + `seek_time` for posed-frame capture), and `widget` (UMG via `FWidgetRenderer` with `scale` DPI multiplier). No new action — schema widening only.

---
