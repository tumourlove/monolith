# Monolith — MonolithEditor Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.21.3 (Beta)

---

## MonolithEditor

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, Json, JsonUtilities, MessageLog, EnhancedInput, LiveCoding (Win64 only)

> **`EnhancedInput` dep (Gap 4, 2026-06-10):** added for `pie_inject_input_action` (`UEnhancedInputLocalPlayerSubsystem::InjectInputForAction`). It is a stock, always-enabled engine plugin module — release-build safe, so it carries no `WITH_*` gate (same rationale as the existing `AIModule` dep).

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithEditorModule` | Creates FMonolithLogCapture, attaches to GLog, subscribes a `FCoreDelegates::PreSlateModal` watcher (see Module-Level Modal Watcher below), registers ~50 actions (20 base + 2 Phase J F8 map actions + 2 v0.14.8 PR #48 automation + 2 v0.14.9 Issue #50 scripting + 3 v0.14.10 PR #54 PIE/console + 4 v0.16.0 preview & inspection + 7 test/profiling harness Wave 1 + 1 test/profiling harness Wave 2 `list_errored_blueprints` + 1 test/profiling harness Wave 3 `capture_anim_frames` + 1 (2026-06-07) `author_map_settings` + introspection-gap pass 2026-06-10: 2 live-PIE object (`pie_get_object_properties`, `pie_call_function`, Gap 8) + 3 PIE input/control (Gap 4) + 1 stat readout (Gap 10). `sample_pie_timeseries` (Gap 9) is implemented here but registered under the `animation` namespace, so it is not counted in this module's `editor`-namespace total.) |
| `FMonolithLogCapture` | FOutputDevice subclass. Ring buffer (10,000 entries max). Thread-safe. Tracks counts by verbosity |
| `FMonolithEditorActions` | Static handlers for build and log operations. Hooks into `ILiveCodingModule::GetOnPatchCompleteDelegate()` to capture compile results and timestamps |
| `FMonolithSettingsCustomization` | IDetailCustomization for UMonolithSettings. Adds re-index buttons for project and source databases in Project Settings UI |

### Actions (~50 — namespace: "editor")

> **Counts are approximate.** Exact integers are no longer tracked to the unit — query `monolith_discover("editor")` for the live figure.

**Base (22 — v0.14.7 baseline + Phase J F8)**

| Action | Description |
|--------|-------------|
| `trigger_build` | Live Coding compile. `wait` param for synchronous. Windows-only. Auto-enables Live Coding |
| `live_compile` | Trigger Live Coding hot-reload compile. Alternative to trigger_build |
| `get_build_errors` | Build errors/warnings from log capture, scoped to a time window and bucketed into `compile_errors` (LogLiveCoding/LogCompile/LogLinker) vs `other_errors`. **Window params** (precedence high→low): `since_marker` (errors after the latest log line containing this token — UE_LOG a marker right before compiling; reports `marker_found`), `since_iso` (absolute ISO-8601 cutoff), `since_seconds` / `since` (legacy alias; last N seconds), else last compile. `clear_baseline` (default false) stamps a fresh `now` baseline and returns immediately — the "I just kicked off a build, ignore prior noise" reset. `exclude_categories` (default `[LogPython, LogMonolith]`) bucketed under `other_errors` and kept OUT of the headline `error_count` (never hidden). `category` / `compile_only` narrow the query. Max 500 entries. **`fix_hints[]` (Phase 3, item 8 — ADDITIVE):** see Fix Hints below |

### `get_build_errors` Fix Hints (Phase 3, item 8 — ADDITIVE)

`get_build_errors` appends a deterministic error→fix-hint pattern table. The output is **purely additive** — every pre-existing field (`error_count`, `errors[]`, `compile_errors[]`, `other_errors[]`, `warnings[]`, `since`, `since_source`, `excluded_categories[]`, and each error object's `message` / `category` / `verbosity`) is **byte-identical** to the pre-Phase-3 response. Two additions:

- **Top-level `fix_hints[]`** — one object per matched error: `{ error_index, pattern, hint }`.
- **Per-error `fix_hint`** — a string stamped onto each matched error object in `errors[]` (the ONLY new field on those objects).

Pattern table:

| `pattern` | Matches | Hint |
|-----------|---------|------|
| `LNK2019_Z_Construct` | `LNK2019` on a `Z_Construct_*` symbol | Resolves the referenced UClass's owning module via the **borrowed** source DB and names the module to add to Build.cs (generic hint when the DB is unavailable). |
| `LNK2019_DeveloperSettings` | `LNK2019` referencing `UDeveloperSettings`/`DeveloperSettings` | "Lives in the 'DeveloperSettings' module, not 'Engine'." |
| `C4996_deprecation` | `C4996` | Recovers the deprecated identifier, attaches its `symbol_deprecations` message (`GetDeprecationsBatch`); generic hint otherwise. |
| `generated_h_not_last` | message contains `generated.h` + `last` | "The `*.generated.h` include must be the LAST `#include`." |

**Source DB borrow:** the hint builder borrows `FMonolithSourceDatabase::GetRawHandle()` under `FScopeLock(GetLock())` on the game thread (the same one-way Editor→Source borrow the RI adapter uses); the prepared statement is finalized before the lock releases, never a second handle on `EngineSource.db`. It degrades gracefully (generic hint) when the DB is not yet indexed.

**No offline parity:** `fix_hints` is **NOT offline-served** — `get_build_errors` reads the in-process log capture ring buffer, which the offline `monolith_query.exe` / `monolith_offline.py` cannot reach. (Items 7 and 9 in the `source` namespace ARE offline-served; item 8 is live-only.)
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
| `delete_assets` | Delete one or more assets by path. Params: `asset_paths[]` (required), `allowed_prefixes[]` (optional path guard), `force` (optional bool, default `false`). Before deleting each target the action clears its package dirty flag and closes any open asset editor, then runs the delete inside an **unattended-script guard** so the engine never raises a blocking "asset in use" / "save changes" modal (which would freeze an unattended MCP session). `force=false` soft-deletes after closing editors; `force=true` calls `ForceDeleteObjects`, nulling referencers. Per-asset failures are reported in a `failed_to_delete[]` response array rather than aborting the call. **Known limitation:** a NiagaraScript created + compiled in the same session can't be deleted until its compile state clears — a transient compilation/traversal graph holds a reference (the engine's own Content Browser hits the same wall); it becomes deletable after the state clears (e.g. editor restart). |
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
| `load_level` | Wraps `ULevelEditorSubsystem::LoadLevel(AssetPath)`. Single-arg map swap. **Dirty-current-map guard (fail-closed):** `LoadLevel` runs unattended (no save prompt), so if the current world's packages (persistent level, loaded streaming levels, or their loaded external OFPA/actor/data-layer packages) are dirty, the action REFUSES with the dirty package list; pass `dirty_policy:"discard"` to proceed and lose them explicitly (echoed in `discarded_dirty_packages`). **World-leak guard:** if a PIE world is still resident, the action REFUSES while a smoke session is running (caller must `stop_pie_smoke` first), else it drives PIE teardown to completion (`RequestEndPlayMap` + bounded synchronous `EndPlayMap`) and forces `CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS)` before loading — without this the deferred EndPlayMap leaves the PIE world rooted and `EditorDestroyWorld` asserts `World Memory Leaks: N`. **Stale-resident-target guard:** if a rooted in-memory copy of the TARGET world survives a standalone-flag-clear + GC pass (e.g. a Python variable still references it), the action REFUSES instead of tripping the same fatal assert. |

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

**Test/Profiling Harness — Wave 1 (7 — save / async-PIE smoke / nav harness)**

| Action | Description |
|--------|-------------|
| `list_dirty_packages` | Report loaded packages with unsaved changes (`UPackage::IsDirty`), optionally scoped to one or more `/Game` path prefixes. Returns per-package `{ package, is_map, disk_path, transient }`. Params: `scope_paths[]` (omit for all dirty), `include_transient` (default false), `include_maps` (default true), `include_content` (default true). Use to audit what a `save_packages` call would touch. |
| `save_packages` | Save the requested packages to disk (`UPackage::SavePackage` + `FSavePackageArgs`). Returns per-package save status. Params: `packages[]` (required, long package names), `fail_on_unrequested_dirty` (default false — when true, aborts saving nothing if a dirty package exists outside the request set, bounded by `scope_paths` if given), `scope_paths[]`, `dry_run` (default false — reports per-package `would_save` without writing). |
| `run_pie_smoke` | Start an **async** PIE smoke session on a map and return immediately with `{ session_id, status: "running", started: true }`. Loads the map, starts PIE synchronously, emits a `UE_LOG` marker, and registers a session that the editor's real frame loop advances over real frames, sampling the target pawn's AnimInstance vars. Params: `map` (omit = current level), `marker` (post-marker log-pattern matching counts only lines after it; pass any token — see source-leak note below re: the current default value), `duration` (seconds, clamped 0–120, default 5), `sample_vars[]` (default `[GroundSpeed, bShouldMove, DesiredYawDelta]`), `pawn_class` (substring match; omit = first PC's pawn), `console_script[]`, `python_script`, `log_patterns` (**flat array** = legacy `must_absent`, OR **object** `{must_absent, must_present, observe_only, warn}` — see grouped-pattern note below), `ignore_after_pattern` (teardown-boundary substring, default `"BeginTearingDown"`; empty disables the split), `teardown_allowed` (default true — teardown-bucket `must_absent` hits never affect `ok`), `probe_scripts[]` (`[{at_seconds, python?, console?[]}]` delayed in-session probes fired once against the live PIE world), `stages` (staged startup hooks `{pre_pie, on_begin_play, after_n_ticks:{n,...}, before_capture}` fired at lifecycle moments — `pre_pie` runs synchronously against the editor BEFORE PIE start, `on_begin_play` on the first observer tick after `HasBegunPlay`, `after_n_ticks` after N observer ticks, `before_capture` is the clip variant's pre-frame-grab hook; complements the start-time `python_script`; reported under `stages`), `on_compile_errors` (`"refuse"` default / `"suppress"`). **Gap-closure additions (2026-06-07):** `actor_setup` (declarative actor staging — see § Structured `actor_setup` below), `csv_profile` (bool — bracket a CSV profiler capture to exactly the PIE window, report `csv_path`), `trace_channels` (string[] — start a trace with the given channels for the PIE window, report `trace_path`); both profilers are stopped/flushed on EVERY session-end path (success, failure, abort). Does NOT block the editor frame. Poll with `poll_pie_smoke`; force-end with `stop_pie_smoke`. **World-leak guard on map load:** see `load_level`; identical policy is applied before the pre-PIE `LoadLevel`. **`on_compile_errors` policy:** before starting PIE the action pre-flights for errored Blueprints (the same `BS_Error && bDisplayCompilePIEWarning` condition `list_errored_blueprints` reports). `"refuse"` (default, safe) returns a structured error listing the offending `{name, path}` and does NOT start PIE — this prevents the engine's blocking PIE compile-error modal, which would freeze the editor and starve the in-process MCP server. `"suppress"` wraps the play-session request in `TGuardValue<bool>(GIsRunningUnattendedScript, true)` so the engine skips the prompt and starts PIE anyway. |
| `poll_pie_smoke` | Poll an async PIE-smoke session by `session_id` (required). Returns `{ status (running/complete/stopped/error), lifecycle, elapsed_seconds, sample_count, pie_active, per-var min/max/last summary, grouped_counts, post_marker_counts:{pattern:count} (legacy), total_matches (legacy), warnings[], missing_must_present[], probes[] }`. **`lifecycle`** (#11) is the explicit session state — one of `running` \| `capture-complete-pie-open` \| `teardown-started` \| `teardown-complete` \| `stopped-by-tool` — disambiguating "capture done but PIE still open" from "running" (which `status` alone conflates). **`grouped_counts`** (#3/#10) splits matches into `active_runtime` (the `ok`-bearing bucket) and `teardown` buckets at the first `ignore_after_pattern` hit, each with per-group/per-pattern counts. **`ok` rule:** `status==complete && pie_ready && (active must_absent total == 0) && (every must_present matched >=1)`; teardown-bucket hits affect `ok` only when `teardown_allowed=false`. When `status==complete` includes the full report (all samples + captured frame paths for the clip variant). `include_samples` (default false) appends the full per-frame sample array even before completion. Does not advance PIE — the editor frame loop does that. |
| `stop_pie_smoke` | Force-stop a PIE-smoke session (`RequestEndPlayMap` + mark stopped) and return its final report. With no `session_id`, stops ALL running sessions. Also serves as cleanup — the shared frame observer self-unregisters once no sessions remain running. |
| `capture_pie_movement_clip` | Start an async PIE-smoke session (same model as `run_pie_smoke`) that ALSO captures a PIE viewport frame every `capture_interval` seconds into `output_path`, plus per-frame AnimInstance sampling. Returns `{ session_id, status: "running", started: true }` immediately; `poll_pie_smoke` returns sampled values + captured frame paths. Params mirror `run_pie_smoke` (incl. grouped `log_patterns`, `ignore_after_pattern`, `teardown_allowed`, `probe_scripts[]`, and the same world-leak guard before any pre-PIE map load) plus `marker` (a log marker token; pass any value — see source-leak note below), `capture_interval` (seconds, clamped 0.05–5, default 0.25), `output_path` (default `Saved/Screenshots/Monolith/PieClip/<timestamp>/`). **Capture-validity / framing additions (Wave 3):** `view_target_actor` (name-, **Outliner-label-** (2026-06-07), or class-substring of a PIE actor to `APlayerController::SetViewTarget` on at session begin so captured frames frame the intended subject — resolution now compares `GetActorLabel()` alongside `GetName()` and class-name; the active view target + per-frame validity are reported under `view_target` / `capture_validity`); `discard_first_frames` (int, default 1, 2026-06-07 — warm-up policy: discard the first N captured frames and/or force a render flush before the initial read so a uniform/black first frame is not counted as invalid); `stages` (staged startup hooks `{pre_pie, on_begin_play, after_n_ticks:{n,...}, before_capture}` — `before_capture` fires immediately before the first frame grab; reported under `stages`); `expected_anim_class` (when set, asserts the live mesh `AnimClass` path CONTAINS this substring each sampled tick — a mismatch is reported under `runtime_identity.expected_mismatch`, never crashes). poll also surfaces a `runtime_identity` report (resolved pawn/mesh/AnimClass). If viewport capture is unavailable during PIE the session continues and poll reports `capture_deferred`. |
| `create_nav_harness_map` | Build a navigation test map from a JSON spec: blank `UWorld`, floor, nav bounds, camera, target points, and BP/actor instances with UPROPERTY defaults (incl. `FSoftObjectPath`). All spawned actors get a `SetFolderPath`. Rebuilds + validates nav via runtime `ai` dispatch (`ai.rebuild_navigation`) and saves. Params: `path` (required, throwaway map path), `floor` `{location, scale, mesh}`, `nav_bounds` `{location, extent}`, `camera` `{location, rotation}`, `target_points[]` `{name, location}` (spawned as `ATargetPoint`, also used as nav validation points), `actors[]` `{class, location, rotation, folder, properties{}}`, `validate_pairs[]` `{from, to}` (target-point name pairs that must have a nav path), `nav_timeout` (seconds, default 30). **Gap-closure additions (2026-06-07):** `game_mode_override` (class path → set `AWorldSettings::DefaultGameMode`), `player_starts[]` (array of transforms → spawn `APlayerStart`s), and improved typed-property coverage in `actors[].properties` — a dedicated `FSoftClassProperty` / `FClassProperty` branch (normalizes to the `_C` generated-class form and imports as a class ref, instead of mis-importing an object path into a class prop) plus `FArrayProperty`-of-object / -soft-ref support. |

**Async PIE design rationale.** `run_pie_smoke` / `capture_pie_movement_clip` are an async session family rather than a single blocking call because a synchronous in-handler engine pump is impossible: the MCP handler runs *inside* the editor's frame, so calling `GEditor->Tick` / `UWorld::Tick` / `ProcessAsyncLoading` from within the handler re-enters the engine tick and trips re-entrancy asserts (the prior synchronous pump re-entered `UWorld::Tick` and crashed). The async design sidesteps this: `run` starts PIE and registers a frame observer, then returns a `session_id` immediately; the editor's own real frame loop advances PIE and accumulates AnimInstance samples + post-marker log-pattern counts; `poll` reads the accumulated state; `stop` force-ends and cleans up (the shared observer self-unregisters when no sessions remain). `capture_pie_movement_clip` reuses the same session model, adding per-interval viewport capture.

**Grouped log patterns + teardown bucketing.** `log_patterns` accepts either the legacy **flat array** (back-compat — every entry is treated as `must_absent`) or a **grouped object** `{ must_absent, must_present, observe_only, warn }`. `must_absent`: any match fails `ok` (default-set substrings are always added here). `must_present`: every pattern must match `>=1` for `ok`. `observe_only`: counted + reported, never affects `ok`. `warn`: surfaces a `warnings[]` list, never affects `ok`. Independently, post-marker entries are split at the first `ignore_after_pattern` (default `"BeginTearingDown"`) hit into an **active-runtime** bucket (before) and a **teardown** bucket (after); `ok` consumes the active-runtime bucket only, and with `teardown_allowed=true` (default) teardown-bucket hits never affect `ok`. This fixes the known false-fail where a post-`BeginTearingDown` `"Unable to find RecastNavMesh..."` warning wrongly failed an otherwise-clean smoke.

**Delayed in-session probes (`probe_scripts`).** Start-time `console_script` / `python_script` run once against the PIE world the instant PIE is requested; an external `editor.run_python` issued later races teardown. `probe_scripts` (`[{ at_seconds, python?, console?[] }]`) instead fires each probe ONCE from the per-frame observer (which already holds the live PIE world each frame) when session elapsed reaches `at_seconds`, using the same `IPythonScriptPlugin::ExecPythonCommandEx` / `PlayerController::ConsoleCommand` mechanism. Each probe's fire time + python output/error are reported under `probes[]`.

**Test/Profiling Harness — Wave 2 (1 — compile-error pre-flight)**

| Action | Description |
|--------|-------------|
| `list_errored_blueprints` | Read-only scan of all loaded `UBlueprint`s for unresolved compile errors (`Status == BS_Error && bDisplayCompilePIEWarning`) — the exact condition the engine tests before raising its blocking PIE compile-error modal. Returns `{ count, blueprints: [{ name, path }] }`. Run before `run_pie_smoke` to know in advance whether PIE will be refused or blocked. No params. |

**Test/Profiling Harness — Wave 3 (1 — skeletal-animation preview capture)**

| Action | Description |
|--------|-------------|
| `capture_anim_frames` | Preview + capture a SKELETAL ANIMATION asset (`UAnimSequence` \| `UBlendSpace` \| `UAnimBlueprint`) to PNG frames in an isolated `FPreviewScene` — **NO PIE**. For each time sample the pose is evaluated (single-node `SetPosition` for sequences; blend-param + `SetPosition` for blendspaces; AnimBlueprint mode + `TickComponent` for ABPs), then rendered via the shared scene-capture→PNG path. The anim counterpart to `capture_sequence_frames` (which is Niagara-only). Params: `asset_path` (required), `skeletal_mesh` (omit = the asset's skeleton preview mesh), `time_samples[]` (explicit sample seconds; omit to use `count` + `duration`), `count` (evenly-spaced samples, default 8), `duration` (default = sequence length, else 1.0), `blend_params` (BlendSpace only — `[X, Y]`, default `[0,0]`), `camera` (`{location, rotation, fov}`, default front framing), `resolution` (default `[512,512]`), `output_dir`, `filename_prefix` (default `frame`). |

**`/Game` virtual-path output resolution (Wave 3).** `capture_anim_frames`' `output_dir` (and `capture_pie_movement_clip`'s `output_path`) accept a disk-relative, absolute, OR a virtual `/Game/...` path — a `/Game/...` value is resolved to `<project>/Content/...` and the resolved absolute directory is echoed back as `resolved_output_dir`. A virtual path that cannot be written is a **hard error** (no silent no-op).

**Test/Profiling Harness — Gap-Closure Pass (1 — map authoring; 2026-06-07)**

| Action | Description |
|--------|-------------|
| `author_map_settings` | Apply WorldSettings + PlayerStart settings to ANY currently-open map (not locked to the nav-harness path). Params: `game_mode_override` (class path → `AWorldSettings::DefaultGameMode`, assigned after `WorldSettings->Modify()`), `player_starts[]` (array of transforms → spawn `APlayerStart`s, each `SetFolderPath`'d). Map mutation dirties the package by design; only dirties on an actual change, and never leaves the map half-authored on a mid-apply failure. |

**Live-PIE object read/call (Gap 8 — 2 — `MonolithPieObjectActions.cpp`)**

| Action | Description |
|--------|-------------|
| `pie_get_object_properties` | Read live UPROPERTY values off a resolved PIE object by dotted member path. Resolve target via `actor_label` (exact label) / `object_name` (exact name) / `class_name` (class-name substring, first match), optionally hopping to a `component_name` or (`anim_instance=true`) the skeletal mesh's active anim instance. `properties[]` are dotted paths (e.g. `CharacterProperties.OrientationIntent`) resolved through the shared `MonolithStructField` resolver (hoisted to MonolithCore) — descends nested structs and matches `UUserDefinedStruct` members by friendly (authored) name, reading the GUID-suffixed internal fields editor python cannot. Returns each path's JSON value (scalars directly; enums/structs/vectors as export text via the same `ReadResolvedValue` conventions as `sample_pie_anim_instance`). **Read-only.** Clean error when PIE is not running. |
| `pie_call_function` | Call a BlueprintCallable UFunction/event on a resolved PIE object (same target resolution as `pie_get_object_properties`). Resolves the function via `FindFunctionByName`; requires `FUNC_BlueprintCallable` unless `allow_non_callable=true`; **rejects `FUNC_Net` (replicated) and latent (`Latent` metadata) functions** with clean errors. Allocates a `ParmsSize` parameter frame, `InitializeValue`/`DestroyValue` per-property via an RAII scoped helper, marshals JSON `args` by parameter name (primitives via `ImportText_Direct`; structs incl. `UUserDefinedStruct` by resolving each friendly field to its internal property then per-field import), invokes `ProcessEvent`, and reads back `CPF_OutParm`/`CPF_ReturnParm` into `out_params`/`return_value`. v1 scope: top-level args + ONE level of nested UDS (deeper rejected). **MUTATES LIVE PIE STATE** — not read-only. Clean error when PIE is not running. |

**Time-series PIE sampling (Gap 9 — registered under the `animation` namespace; `MonolithPieTimeseries.cpp` + `MonolithEditorActions::StartTimeseriesSession`)**

`sample_pie_timeseries` is IMPLEMENTED in MonolithEditor (it reuses the async PIE-smoke session machinery — `FPieSmokeSessionManager`, the shared frame observer, `poll_pie_smoke`/`stop_pie_smoke`) but REGISTERED under the **`animation`** namespace string (the registry is namespace-string-keyed; verification ergonomics match `sample_pie_anim_instance`). Async lifecycle identical to `run_pie_smoke`: returns `{session_id, status:'running'}`; poll/stop via the existing PIE-smoke actions. Per sample tick it reads dotted UDS-friendly `variables[]` off the resolved target (gated by `sample_interval`, capped by `max_samples`) and fires typed `provocations[]` once each when session-elapsed crosses `time`: `set_control_rotation` (`APlayerController::SetControlRotation`), `add_movement_input` (`APawn::AddMovementInput`), `jump` (`ACharacter::Jump` — target must be a Character), `console_command` (PIE console exec). `poll_pie_smoke` returns the full per-sample `[{t, vars:{...}}]` under `timeseries` (gated by completion / `include_samples`) and the provocation fire log under `provocations`. World-validity + `BeginTearingDown` teardown guards are inherited from the smoke session.

**Deterministic PIE input / control driving (Gap 4 — 3 — `MonolithPieInputActions.cpp`; 2026-06-10)**

Scripted, camera-independent PIE driving. Adds the `EnhancedInput` Build.cs dep (see Dependencies note above). All three **mutate live PIE state** and no-op with a clean error when PIE is not running.

| Action | Description |
|--------|-------------|
| `pie_set_control_rotation` | Set the control rotation on a PIE player controller (`APlayerController::SetControlRotation`). Params: `pitch` / `yaw` / `roll` (degrees, omitted components default to 0), optional `player_index` (default 0). `hold_frames` (default 0) re-applies the rotation each frame for that many frames so it can outlast a per-tick camera/control system that re-writes `ControlRotation` — **best-effort, not frame-perfect**: a camera director that runs later in the frame can still win that frame. The re-apply hook clears itself on PIE end. |
| `pie_inject_input_action` | Inject a value for an Enhanced Input action into a live PIE local player (`UEnhancedInputLocalPlayerSubsystem::InjectInputForAction`), running that action's modifiers and triggers as if real input arrived. `input_action` is a UInputAction asset path (`/Game/...`) or short asset name (registry-resolved, first match). `value` maps to `FInputActionValue` by JSON shape: bool → Boolean, number → Axis1D, array[2] → Axis2D, array[3] → Axis3D. Optional `player_index` (default 0); `repeat_frames` (default 1) re-injects each frame. |
| `pie_possess_spectator_free` | Detach a PIE player controller to a free-fly spectator pawn, or re-possess the original. `enable=true` stores + unpossesses the current pawn and enters the Spectating state (`ChangeState(NAME_Spectating)`, which spawns a spectator pawn from the game mode's `SpectatorClass`); `enable=false` re-possesses the stored original (`ChangeState(NAME_Playing)` + Possess). The original is held as a weak pointer, cleared on PIE end. Note: free-fly spawning depends on the game mode providing a `SpectatorClass`; with none configured the controller still enters Spectating but no pawn is created. Optional `player_index` (default 0). |

**Stat-group counter readout (Gap 10 — 1 — `MonolithStatActions.cpp`; 2026-06-10)**

| Action | Description |
|--------|-------------|
| `get_stat_group_values` | Read a stats group programmatically into a structured response. Enables high-performance collection for `group_name` (full `STATGROUP_Anim` or short `Anim` form; project-defined groups work the same), reads the most recent settled stat frame(s) from the stats thread, and returns each stat's counter value (int64/double) and cycle-stat timing in milliseconds. `sample_frames` (default 1; >1 aggregates per-stat min/avg/max over the last N already-settled frames). A group this action enables is disabled again on completion (a group already collecting is left as-is). **`#if STATS`-gated** — compiled into Development editor builds but NOT Shipping/Test; off-gate the handler returns a clean "stats system not compiled in" error (the registration itself is unconditional). Reads the LIVE stats stream, which only produces frames while overall collection is primary-enabled — for reliable data have stats already active (PIE running with an on-screen stat, or call `run_console_command("stat <group>")` first). If no settled frame exists yet it returns `settled=false` with zero stats; retry once the editor/PIE has advanced a frame with collection active. |

#### Structured `actor_setup` (on the PIE-smoke actions; 2026-06-07)

`run_pie_smoke` / `capture_pie_movement_clip` accept an optional **`actor_setup`** block — declarative actor staging against the live PIE world, without free-form Python:

```
actor_setup: [
  {
    class: "/Game/.../BP_Thing"        // BP or native class path
    count: 3                            // default 1
    locations: [[x,y,z], ...]          // optional; else origin/grid
    apply_data_asset: "/Game/.../DA_X" // optional; reflectively applies the DataAsset's matching fields to each actor
    move_to: [x,y,z]                   // optional; AAIController::MoveToLocation
  }
]
```

Executes against the transient PIE world only (`World->SpawnActor`; reflective field apply via the shared reflection walker; `AAIController::MoveToLocation`). `apply_data_asset` is a **generic DataAsset path** — fields the actor also declares are copied; the result reports a **structured** `applied: [field…]` / `unmatched: [field…]` split so callers can distinguish partial from full apply. Spawned actors are cleaned up on session end and never leak into the editor world.

#### PIE-smoke `marker` default

The `run_pie_smoke` / `capture_pie_movement_clip` `marker` param defaults to a generic token (`MONOLITH_SMOKE` / `MONOLITH_CLIP`). Post-marker log-pattern matching counts only lines after the marker, so callers may override it with any value when running concurrent or distinguishable sessions.

### Module-Level Modal Watcher

`FMonolithEditorModule::StartupModule` subscribes a watcher to `FCoreDelegates::PreSlateModal`. Immediately before the engine enters any blocking modal loop, the watcher emits, on the game thread, a single structured log line:

```
LogMonolith: Warning: MODAL_OPEN ts=<timestamp> title=<window-title> text=<message-text>
```

The purpose is recovery: when a blocking modal opens, the in-process MCP server is the strangled thread and cannot respond, but an external agent tailing the editor log can read this line and recover context (what dialog opened, when). `title` and `text` are **best-effort** — they are empty when the modal window is not yet on the modal stack at broadcast time.

**Honest scope limitation:** this watcher provides notification only. Synchronous live modal-read and programmatic modal dismissal are **NOT** provided and are architecturally unsound here — the in-process server is the very thread the modal blocks, so it cannot service a request to read or close the dialog while the modal loop owns the thread. Prevention (`run_pie_smoke`'s `on_compile_errors="refuse"`, `list_errored_blueprints`) is the supported path; the watcher is the fallback for modals that open despite prevention.

---
