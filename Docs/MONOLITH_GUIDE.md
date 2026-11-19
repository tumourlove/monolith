# Monolith Editorial Guide

> Served by the `monolith_guide` MCP action. Call `monolith_guide()` for this whole map, or `monolith_guide(section="recipes")` to pull one section and bound your context cost. Each `## ` header below is a section key.
>
> This guide is **editorial** — it teaches cross-namespace workflows, decisions, and recovery. It does **not** restate the per-namespace action catalog (see `Docs/API_REFERENCE.md`) or the pipeline chains (see `Docs/SPEC_CORE.md` §13 Pipelines). It assumes you have only the Monolith plugin installed and a running Unreal Editor — no other scaffolding.

## onboarding

You are an AI agent driving an Unreal Engine editor through Monolith's MCP tools. Three calls orient you before you touch anything:

1. **`monolith_discover()`** — no arguments. Returns the namespace inventory: every namespace, its action count, and which optional namespaces are gated off in this project (e.g. `gas`, `combograph`, `logicdriver` only register when their plugin is present). This response also carries a `guide_hint` pointing back here. Call `monolith_discover("<namespace>")` to get the full action list for one namespace (terse by default: action names + one-line descriptions) — do this before calling any action you have not used. For an action's full **parameter schema**, call `describe_query("action_schema", ...)` (or pass `detail=true` to `monolith_discover` to inline every action's schema), rather than guessing argument names.
2. **`monolith_status()`** — confirms the editor is reachable, reports the plugin version and the live total action count. If this fails or the MCP connection drops, the editor is down — nothing else will work until it is back up.
3. **`monolith_guide(section="recipes")`** — pull the worked cross-namespace examples once you know what you want to build.

Each `monolith_discover("<namespace>")` costs real tokens — even terse (names + one-line descriptions), a large namespace's listing is sizeable, and `detail=true` (full schemas) is far heavier. Spend that cost intentionally: discover the one or two namespaces your task needs, not all of them, and reach for `describe_query("action_schema", ...)` when you only need one action's schema. The surface is large — roughly 1,500+ actions across 27 in-tree namespaces — and shifts between releases, so never rely on an exact count written in prose: trust the live `monolith_discover()` / `monolith_status()` figure over any number you read here or in the docs.

The golden rule underneath all of this: **discover before you guess.** Action names, parameter names, and which namespaces exist are all answerable at runtime. Fabricating any of them wastes a round-trip on a guaranteed error.

## recipes

These are cross-namespace flows that no single namespace's docs cover. Each step names `namespace.action` and ends with a verify call. For single-asset authoring chains (one material, one Sound Cue, one state machine), use the pipelines in `Docs/SPEC_CORE.md` §13 instead — the recipes here are the multi-namespace cases.

**Pointer — spec builders.** Several namespaces expose a `build_*_from_spec` family (`build_material_graph` with a `graph_spec`, `build_sm_from_spec`, `build_sound_cue_from_spec`, `build_ui_from_spec`, and others). These are transactional: one call populates a whole graph with validation, connection resolution, and rollback. Prefer them over hand-sequencing `create → add → connect`. Discover the exact spec shape with `monolith_discover("<namespace>")` and read the schema for the builder action.

**Recipe 1 — Ship a melee ability with audio and HUD feedback.**
1. `gas.create_gameplay_ability` — author the ability asset (gated on `WITH_GBA`).
2. `gas.create_gameplay_effect` — the damage/cost effect the ability applies.
3. `audio.create_sound_cue` (or `audio.build_sound_cue_from_spec`) — the swing/impact sound.
4. `gas.add_gameplay_cue` — bind the cue tag so the ability triggers the sound.
5. `ui.create_widget_blueprint` then `ui.bind_widget_to_attribute` — wire a cooldown/health readout to the attribute set.
6. **Verify:** `gas_query("get_ability_info", ...)` confirms the ability's effects/cues; `ui_query("compile_widget", ...)` confirms the HUD compiles clean.

**Recipe 2 — Build a horror prop with a particle effect and impact material.**
1. `material.create_material` then `material.build_material_graph` (pass a `graph_spec`) — the surface shader.
2. `niagara.create_niagara_system` — the ambient/impact VFX.
3. `mesh.spawn_actor` (or place a Static Mesh) and assign the material — set its Outliner folder via the action's folder parameter so the scene stays organized.
4. **Verify:** `material_query("recompile_material", ...)` returns no errors; `niagara_query("get_system_info", ...)` confirms emitters; `mesh_query` overlap/raycast confirms placement.

**Recipe 3 — Author an AI encounter that reacts to sound.**
1. `ai.create_behavior_tree` (or State Tree) — the patrol/chase logic (gated on `WITH_STATETREE` / `WITH_SMARTOBJECTS`).
2. `ai.add_perception_to_actor` — give the AI hearing/sight.
3. `audio.bind_sound_to_perception` — make a Sound Cue emit a perception stimulus the AI hears.
4. `logicdriver.build_sm_from_spec` — optional higher-level state flow (gated on `WITH_LOGICDRIVER`).
5. **Verify:** `ai_query("get_bt_graph", ...)` confirms the tree; trigger PIE via `editor_query("start_pie")` and watch the perception fire.

**Recipe 4 — Stand up a settings menu.**
1. `ui.scaffold_settings_panel_with_tabs` — generates the tabbed panel shell.
2. `ui.build_menu_from_spec` — populate options from a JSON spec.
3. `ui.apply_token_binding` — bind to a design-token style set if you use one.
4. **Verify:** `ui_query("compile_widget", ...)` returns empty `errors[]`; `ui_query("audit_focus_chain", ...)` confirms gamepad navigation.

**Recipe 5 — Visual introspection: going beyond thumbnails.** Default content-browser thumbnails are 256² and rendered with editor defaults — fine for identification, useless for tech-art inspection. Three `editor::` actions raise fidelity.
1. `editor.capture_scene_preview` — render ONE asset on a preview mesh at the resolution and angle you choose. `asset_type` accepts `material`, `niagara`, `static_mesh`, `skeletal_mesh` (with optional `animation_path` + `seek_time` for a posed-frame capture), and `widget` (UMG via `FWidgetRenderer`, with `scale` DPI multiplier). Example: `editor_query("capture_scene_preview", {asset_type: "skeletal_mesh", asset_path: "/Game/Characters/SK_Hero", animation_path: "/Game/Characters/Anims/AS_Idle", seek_time: 0.5, output_path: "D:/captures/hero_idle.png", width: 1024, height: 1024})`.
2. `editor.capture_with_overlay` — same single-asset capture path, but renders under an engine debug-view show flag for tech-art inspection. Modes: `wireframe`, `normals`, `uv_density`, `lightmap_density`, `shader_complexity`. Example: `editor_query("capture_with_overlay", {asset_path: "/Game/Meshes/SM_Rock", overlay_mode: "shader_complexity", output_path: "D:/captures/rock_complexity.png"})`.
3. `editor.capture_material_grid` — render N material instances side-by-side under shared lighting in a single PNG. Auto-grid via `ceil(sqrt(N))`; pass `columns` to override. Example: `editor_query("capture_material_grid", {material_instances: ["/Game/Materials/MI_Stone_A", "/Game/Materials/MI_Stone_B", "/Game/Materials/MI_Stone_C", "/Game/Materials/MI_Stone_D"], output_path: "D:/captures/stones_grid.png"})`.

All three are editor-only, render-thread-enqueue style, and return `{success, output_path, width, height}`.

**Recipe 6 — Reading asset structure without rendering.** When you need structured data (parameter names, channel statistics, packing detection) rather than a picture, two `editor::` inspect actions walk the asset reflectively — pure JSON, no render-thread cost.
1. `editor.inspect_material_pbr` — walks a material's texture parameter list, classifies each by PBR role (`base_color`, `normal`, `roughness`, `metallic`, `ao`, `emissive`, `orm`, `arm`, `mra`, etc.), and detects ORM / ARM / MRA channel-packing on the source textures. Returns `{slots: {base_color_texture: ..., normal_texture: ..., ...}, packed_channels: [{texture, packing}], unmapped_parameters: [...]}`. Example: `editor_query("inspect_material_pbr", {asset_path: "/Game/Materials/M_Concrete"})`.
2. `editor.inspect_texture_channels` — locks the source mip read-only and reports per-channel statistics (R/G/B/A min, max, mean) plus the source format and dimensions. Optional `emit_splits=true` writes one PNG per channel for visual diff. Returns `{width, height, format, channel_stats: {r: {min, max, mean}, g: {...}, b: {...}, a: {...}}, split_paths?}`. Example: `editor_query("inspect_texture_channels", {asset_path: "/Game/Textures/T_Concrete_ORM", emit_splits: true, output_dir: "D:/splits/"})`.

Prefer the inspect actions over capture when downstream logic needs to branch on the data (e.g. "if this is ORM-packed, route channel R to AO; if not, treat the slot as standalone Roughness"). Prefer capture when a human or a vision model needs to look at the pixels.

**Recipe 7 — Tuning Niagara timing without hand-walking the module stack.** Authoring a system's warmup, loop topology, sim-stage iteration counts, and particle lifetime used to mean per-property `set_system_property` / `set_static_switch_value` / `set_module_input_value` round-trips against `EmitterState` and `InitializeParticle`. The temporal-control surface collapses that into intent-named composite writers.
1. `niagara.get_system_timing` — bundled read of `WarmupTime` / `WarmupTickCount` / `WarmupTickDelta` / `bFixedTickDelta` / `FixedTickDeltaTime` / `bRequireCurrentFrameData` in one call. Use this to inspect before you write — and to observe the engine-resolved triple after `set_warmup_profile`, since `UNiagaraSystem::SetWarmupTime` snaps `WarmupTickCount` via `ResolveWarmupTickCount`.
2. `niagara.set_warmup_profile` — composite write that dispatches `SetWarmupTime` + `SetWarmupTickDelta` and returns the engine-resolved triple. Pair with `set_fixed_tick_delta` / `set_require_current_frame_data` for the rest of the system-level timing knobs.
3. `niagara.set_emitter_loop_profile` — composite write of EmitterState loop topology (`loop_behavior` / `loop_duration` / `loop_delay` / `loop_count` / `loop_delay_enabled`). For standalone `UNiagaraStatelessEmitter` (Lightweight Emitter) assets, pass the optional `loop_duration_mode` (`"Fixed"` / `"Infinite"`); the action detects stateless assets via class-name match and dispatches into the reflection-based write path against the protected `EmitterState` `FNiagaraEmitterStateData`.
4. `niagara.set_sim_stage_iteration_count` / `niagara.set_sim_stage_execute_behavior` — sim-stage aliases over `set_simulation_stage_property` using PR #65's `stage_index` / `stage_name` selector.
5. `niagara.set_particle_lifetime` — convenience write to `InitializeParticle`. `min` only → Direct mode constant `Lifetime`; `min` + `max` → Random mode `Lifetime Min` + `Lifetime Max`.
6. **Verify:** `niagara_query("get_emitter_timing_summary", ...)` — read aggregator returning loop topology + `sim_stages[]` + `InitializeParticle` lifetime fields. On standalone stateless assets it returns `stateless: true` with `null` lifetime fields and `sim_stages: []`.

**Recipe 8 — Authoring a Lightweight (stateless) emitter from scratch.** `UNiagaraStatelessEmitter` lives outside the System wrapper and uses Internal/Stateless/ headers most modules can't link to. `niagara.create_stateless_emitter` factors that out.
1. `niagara.create_stateless_emitter` — author the standalone asset at `save_path`. Uses `FindObject<UClass>(nullptr, "/Script/Niagara.NiagaraStatelessEmitter")` + type-erased `NewObject` so callers don't need a hard module dep on Niagara internals.
2. `niagara.set_emitter_loop_profile` — same action as the stateful path; pass `loop_duration_mode` (`"Fixed"` / `"Infinite"`) to set the stateless-only knob. Stateful systems emit a warning if `loop_duration_mode` is supplied because the stateful `EmitterState` module has no equivalent input.
3. **Verify:** `niagara_query("get_emitter_timing_summary", {asset_path: "/Game/.../SLE_Foo"})` returns `stateless: true` and the topology you set.

**Recipe 9 — Authoring custom (GAS-free) primary data assets.** When your project defines its own native `UPrimaryDataAsset` (or any non-Actor, non-Blueprint `UObject`) subclass, you do **not** need the `gas` namespace or any GAS plugin — `blueprint.seed_data_asset` creates the asset and reflection-populates its `UPROPERTY` fields from a JSON tree, with dry-run validation. It accepts any concrete class that is not abstract, deprecated, Actor-derived, or a Blueprint class, so a native `UPrimaryDataAsset` subclass passes the class guard with zero GAS coupling.
1. **Dry-run first** — validate the tree against the class CDO without writing anything:
   `blueprint seed_data_asset { save_path: "/Game/Data/DA_Foo", class_name: "YourDataAssetClass", tree: { ... }, dry_run: true }`
   The response reports each would-be field write (`field_writes[]`) and `would_apply`. Fix any rejected paths before committing.
2. **Commit** — re-issue with `dry_run: false` to create and save the asset.
3. **Object-reference fields and arrays are supported.** A hard object-ref field (`TObjectPtr<USomeClass>`) takes an asset-path **string** (resolved via `StaticLoadObject`); an array of refs (`TArray<TObjectPtr<USomeClass>>`) takes a JSON **array of asset-path strings** — the reflection walker dispatches each element through the same object-ref resolver. Soft refs (`TSoftObjectPtr<...>`) take their path string too. Example tree fragment: `{ "Icon": "/Game/UI/T_Icon", "RelatedItems": ["/Game/Data/DA_Bar", "/Game/Data/DA_Baz"] }`.
4. **Read the field shape first** if unsure — `class_name` resolution tries the name as-is, then with a `U`/`A` prefix; pass a full `/Script/Module.ClassName` path to disambiguate. Use `blueprint_query("get_cdo_properties", ...)` to inspect the writable fields before building the tree.
5. **Verify the write landed** — `blueprint.seed_data_asset` takes `read_back_values: true` to echo the saved field values straight from the just-written asset, and `blueprint_query("get_cdo_properties", ...)` reads them live from the asset object. Prefer either over `project_query("get_asset_details", ...)`, which serves a **stale indexed snapshot** (it lags the actual asset until the index re-scans) and will happily report old values after a fresh write.

**Recipe 10 — Prove a nested chooser remap from readback (not from the write call).** A write that reports success only means the call returned — to prove a chooser row actually points where you intend, read the resolved table back.
1. `chooser.inspect_chooser` with `recursive: true` — returns the full table including **nested** chooser tables inline, so a row that delegates to a sub-chooser is resolved in one call rather than chasing references by hand.
2. For chooser tables embedded in an AnimGraph: `animation.get_anim_graph_choosers` lists the choosers a given AnimBP's graph references, and `animation.get_nodes` with `include_anim_graph: true` surfaces the AnimGraph nodes (chooser players, blend nodes) that consume them.
3. **Verify:** compare the readback's resolved output class/asset per row against your intended remap. Trust the readback, not the write call's `success` flag.

**Recipe 11 — Author and confirm an AnimBP transition rule.** Transition rules between states can be authored as a structured comparison rather than a hand-wired Boolean graph.
1. `animation.set_transition_rule` with `kind: "compare"` — sets the rule as a typed comparison (a variable/property compared against a value with an operator), avoiding manual node wiring.
2. **Verify:** `animation.get_transition_rule` reads the rule back in structured form so you can confirm the operator, operands, and that it landed on the intended transition.

**Recipe 12 — Inspect inherited native components on a Blueprint.** Components added in a C++ base class are easy to miss because they are not in the Blueprint's own SimpleConstructionScript.
1. `get_blueprint_info` — its `native_component_count` tells you how many components come from native base classes (vs Blueprint-added).
2. `blueprint.get_component_details` — resolves a component's details, falling back to the inherited-native lookup when the component lives on a C++ base rather than in the Blueprint graph, so inherited components report their real class and properties.

**Recipe 13 — Profile a PIE session and capture movement.** Drive a Play-In-Editor session and collect timing + a movement clip without leaving the MCP surface.
1. `editor.actor_setup` — spawn an actor, apply a DataAsset to it, and/or move it into position for the scenario you want to profile.
2. `editor.csv_profile` (start/stop a CSV profiling capture) with `trace_channels` to scope which trace channels the session records — keeps the capture focused instead of recording everything.
3. `editor.capture_pie_movement_clip` with `discard_first_frames: N` — records a movement clip while dropping the first N frames (PIE warm-up / hitch frames that would skew the data); the capture is label-aware and follows the view target you set.
4. **Verify:** inspect the returned CSV/clip paths; the discarded-frame count and trace-channel set are echoed back so you can confirm the capture was scoped as intended.

**Recipe 14 — Author map settings (GameMode override, PlayerStarts).** Configure a world's settings and spawn points in one transactional call.
1. `editor.author_map_settings` (or `editor.create_nav_harness_map` for a fresh harness map) — set `game_mode_override` (a typed `AGameModeBase`-subclass path), `player_starts` (an array of `{location, rotation}` objects), and other typed/array world-settings props in one call.
2. **Idempotent by design:** re-applying the same `game_mode_override` is a no-op — the call only dirties the package when the class actually differs, and the response reports `game_mode_override_changed: false` when nothing changed. Use that flag to confirm whether a re-run mutated anything.

## decisions

When two tools overlap, pick by intent:

- **`build_material_graph` (with `graph_spec`) vs `bulk_fill_query("apply")`.** Use `build_material_graph` to author a material's expression graph — it understands material nodes and connections. Use `bulk_fill_query("apply", target_namespace=..., target=...)` to set many *properties* on an existing asset in one reflection-driven write (e.g. populating a DataAsset or a component's fields). Builders shape graphs; bulk_fill shapes property trees. Read the writable field tree first with `describe_query("schema", ...)`.
- **`live_compile` vs UBT.** `editor_query("live_compile")` hot-patches `.cpp`-only changes into the running editor — fast, in-memory, lost on restart. Any **header** change (new class, changed signature, new `UPROPERTY`/`UFUNCTION`) needs a full UnrealBuildTool build plus an editor restart; Live Coding cannot pick up new compiled symbols. If a live_compile "succeeds" but the new symbol isn't found, you changed a header — rebuild.
- **`source_query` vs `project_query`.** `source_query` searches **C++ engine/plugin source** (signatures, includes, class hierarchies) — use it to verify an API before writing code. `project_query` searches **content assets** (Blueprints, materials, meshes by path/name/type) — use it to find or confirm an asset exists before referencing it.
- **`monolith_discover` vs `monolith_guide`.** `monolith_discover` is the machine surface — exact action names + one-line descriptions (terse by default; pass `detail=true` or call `describe_query("action_schema", ...)` for an action's full param schema). `monolith_guide` (this tool) is the editorial layer — *why* and *in what order*. Discover tells you the verbs; the guide tells you the sentences.
- **`bulk_fill_query` vs `describe_query`.** `describe_query("schema", ...)` is read-only: it returns the authoritative field tree (paths, ImportText grammar, ranges, enum values) for an adapter namespace. `bulk_fill_query("apply", ...)` performs the write. Describe to learn the shape, bulk_fill to commit it; `bulk_fill_query("list_namespaces")` shows which adapters are available.
- **`capture_scene_preview` vs `capture_material_grid` vs `inspect_material_pbr`.** Three actions that look adjacent but solve different problems:
  - `editor.capture_scene_preview` — ONE material/asset rendered on a preview mesh, returns a single PNG. Use when you need a higher-fidelity thumbnail than the content-browser default.
  - `editor.capture_material_grid` — N material instances side-by-side in a shared scene, returns ONE composite PNG. Use when comparing variations (stone palette, skin tones, decal alphabet) is the goal.
  - `editor.inspect_material_pbr` — NO rendering. Walks the parameter list and returns structured JSON (PBR slot classification + ORM/ARM/MRA channel-packing detection). Use when downstream logic needs to branch on the data rather than look at pixels.

## errors

Workflow-level recovery — the map from a symptom to the fix. (Individual action error messages already carry inline recovery hints; this is the higher-level view.)

- **`LIVE_CODING_BLOCKED` / "Unable to build while Live Coding is active."** The editor is open and holding the build lock. Close the editor, run the UBT build, then reopen. For `.cpp`-only changes you can instead `editor_query("live_compile")` without closing.
- **"Unknown namespace" / a namespace you expected is missing from `monolith_discover()`.** That namespace is gated behind an optional plugin and the gate is off in this project. `gas` needs GameplayAbilities (`WITH_GBA`), `combograph` needs ComboGraph, `logicdriver` needs Logic Driver Pro, `ai` needs StateTree + SmartObjects, MetaSound audio actions need MetaSound. Install/enable the plugin and rebuild, or use a different namespace.
- **MCP connection dropped mid-session / tools stop responding.** Monolith runs in-process inside the editor. A dropped connection means the editor closed or crashed. Restart the editor; `monolith_status()` confirms when it is back. While the editor is down you are not blind: run read-only queries through the offline tools. `Binaries/monolith_query.exe` is the canonical standalone executable, and `Scripts/monolith_offline.py` is a stdlib-only fallback kept byte-identical to it. Both serve the `source`, `project`, and `monolith` subcommands plus all 20 Reflection-Intelligence actions (cppreflect, network, decision, risk) against the on-disk databases, byte-identical to the live server. They are read-only — write operations still require the editor.
- **"Plugin 'X' not found" / "Unable to find plugin."** A referenced sibling plugin is not installed. Monolith's sibling plugins ship separately and are **not** in the Monolith release zip. Install the sibling, or drop the dependency.
- **"Unknown action" on a namespace you do have.** You guessed the action name. Call `monolith_discover("<namespace>")` to list the real action names — never invent them.
- **Reflective-write errors from `bulk_fill_query("apply")`** (bad field path, type mismatch, out-of-range value). Call `describe_query("schema", target_namespace=..., target=...)` first to read the exact field paths, ImportText grammar, enum values, and clamp ranges, then re-issue the write to match.

## skills_map

Monolith ships a set of Claude Code **skills** — task-scoped instruction files that load automatically when their trigger words appear in your conversation. They live in the plugin's `Skills/` directory and load through Claude Code **independently of the MCP action surface**: the skill teaches workflow, the MCP namespace executes it. If you are not running through Claude Code, read the skill markdown directly.

| Namespace | Skill | Path |
|-----------|-------|------|
| blueprint | unreal-blueprints | `Skills/unreal-blueprints/unreal-blueprints.md` |
| material | unreal-materials | `Skills/unreal-materials/unreal-materials.md` |
| material (reference) | material-reference | `Skills/material-reference/material-reference.md` |
| animation | unreal-animation | `Skills/unreal-animation/unreal-animation.md` |
| niagara | unreal-niagara | `Skills/unreal-niagara/unreal-niagara.md` |
| niagara (reference) | niagara-reference | `Skills/niagara-reference/niagara-reference.md` |
| mesh | unreal-mesh | `Skills/unreal-mesh/unreal-mesh.md` |
| editor (diagnostics) | unreal-debugging | `Skills/unreal-debugging/unreal-debugging.md` |
| editor (build) | unreal-build | `Skills/unreal-build/unreal-build.md` |
| source + config | unreal-cpp | `Skills/unreal-cpp/unreal-cpp.md` |
| project | unreal-project-search | `Skills/unreal-project-search/unreal-project-search.md` |
| gas | unreal-gas | `Skills/unreal-gas/unreal-gas.md` |
| combograph | unreal-combograph | `Skills/unreal-combograph/unreal-combograph.md` |
| logicdriver | unreal-logicdriver | `Skills/unreal-logicdriver/unreal-logicdriver.md` |
| audio | unreal-audio | `Skills/unreal-audio/unreal-audio.md` |
| ui | unreal-ui | `Skills/unreal-ui/unreal-ui.md` |
| level_sequence | unreal-level-sequences | `Skills/unreal-level-sequences/unreal-level-sequences.md` |
| cross-domain (config/material/mesh) | unreal-performance | `Skills/unreal-performance/unreal-performance.md` |

`unreal-performance` spans namespaces (config audit, material shader stats, mesh draw-call analysis) rather than mapping one-to-one. The skill set grows between releases — read the plugin's `Skills/` directory for the current roster rather than relying on a fixed count here.

## gotchas

Monolith-specific traps not covered by general Unreal documentation:

- **`build_material_graph` and `bulk_fill_query` want a wrapper object.** `build_material_graph` requires `{ "graph_spec": { ... } }` — a bare spec is rejected. Likewise read each spec builder's schema; the payload is nested under a named key, not passed flat.
- **MCP transport is `"http"`, not `"streamableHttp"`.** When configuring a client, use `"http"`. Nested `params` may serialize to a JSON string in transit — detect that and deserialize back to an object before reading fields.
- **Sibling plugins are absent from the release zip.** The Monolith release zip contains only Monolith. Its sibling plugins (separate repos, separate lifecycles) are not bundled — a live editor with siblings present will report more namespaces than the plugin you installed actually ships. Treat `monolith_discover()` on your own install as the truth for what you have.
- **Optional namespaces register conditionally.** `gas`, `combograph`, `logicdriver`, `ai`, and MetaSound audio actions only appear when their plugin/gate is enabled. A namespace missing from `monolith_discover()` is gated off, not broken — see the **errors** section.
- **For Unreal Engine 5.7 API gotchas** (deprecations, changed signatures, kinematic-velocity asymmetry, package-reuse semantics, and similar), verify against the live engine source via `source_query("search_source", ...)` before writing C++. These are engine-level, not Monolith-specific, so they are not restated here — the source index is the source of truth.
