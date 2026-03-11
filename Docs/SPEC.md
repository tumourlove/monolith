# Monolith — Technical Specification

**Version:** 0.7.0 (Beta)
**Wiki:** https://github.com/tumourlove/monolith/wiki
**Engine:** Unreal Engine 5.7+
**Platform:** Windows, macOS, Linux
**License:** MIT
**Author:** tumourlove
**Repository:** https://github.com/tumourlove/monolith

---

## 1. Overview

Monolith is a unified Unreal Engine editor plugin that consolidates 9 separate MCP (Model Context Protocol) servers and 4 C++ plugins into a single plugin with an embedded HTTP MCP server. It reduces ~219 individual tools down to 12 MCP tools (172 total actions), cutting AI assistant context consumption by ~95%.

### What It Replaces

| Original Server/Plugin | Actions | Replaced By |
|------------------------|---------|-------------|
| unreal-blueprint-mcp + BlueprintReader | 6 | MonolithBlueprint |
| unreal-material-mcp + MaterialMCPReader | 46 | MonolithMaterial |
| unreal-animation-mcp + AnimationMCPReader | 62 | MonolithAnimation (62 actions) |
| unreal-niagara-mcp + NiagaraMCPBridge | 70 | MonolithNiagara |
| unreal-editor-mcp | 11 | MonolithEditor |
| unreal-config-mcp | 6 | MonolithConfig |
| unreal-project-mcp | 17 | MonolithIndex |
| unreal-source-mcp (concept from Codeturion) | 9 | MonolithSource |
| unreal-api-mcp | — | MonolithSource |

---

## 2. Architecture

```
Monolith.uplugin
  MonolithCore          — HTTP server, tool registry, discovery, settings, auto-updater
  MonolithBlueprint     — Blueprint graph reading (6 actions)
  MonolithMaterial      — Material inspection + graph editing + CRUD (25 actions)
  MonolithAnimation     — Animation sequences, montages, ABPs, curves, notifies, skeletons, PoseSearch (62 actions)
  MonolithNiagara       — Niagara particle systems (41 actions)
  MonolithEditor        — Build triggers, live compile, log capture, compile output, crash context (13 actions)
  MonolithConfig        — Config/INI resolution and search (6 actions)
  MonolithIndex         — SQLite FTS5 deep project indexer, 14 internal indexers (5 MCP actions)
  MonolithSource        — Engine source + API lookup (10 actions)
```

### Discovery/Dispatch Pattern

All domain modules register actions with `FMonolithToolRegistry` (central singleton). Each domain exposes a single `{namespace}_query(action, params)` MCP tool. The 4 core tools (`monolith_discover`, `monolith_status`, `monolith_reindex`, `monolith_update`) are standalone.

### MCP Protocol

- **Protocol version:** Echoes client's requested version; supports both `2024-11-05` and `2025-03-26` (defaults to `2025-03-26`)
- **Transport:** HTTP with JSON-RPC 2.0 (POST for requests, GET for SSE stub, OPTIONS for CORS). Transport type in `.mcp.json` varies by client: `"http"` for Claude Code, `"streamableHttp"` for Cursor/Cline
- **Endpoint:** `http://localhost:{port}/mcp` (default port 9316)
- **Batch support:** Yes (JSON-RPC arrays)
- **Session management:** None — server is fully stateless (session tracking removed; no per-session state was ever stored)
- **CORS:** `Access-Control-Allow-Origin: *`

### Module Loading

| Module | Loading Phase | Type |
|--------|--------------|------|
| MonolithCore | PostEngineInit | Editor |
| All others (8) | Default | Editor |

### Plugin Dependencies

- Niagara
- SQLiteCore
- EnhancedInput

---

## 3. Module Reference

### 3.1 MonolithCore

**Dependencies:** Core, CoreUObject, Engine, HTTP, HTTPServer, Json, JsonUtilities, Slate, SlateCore, DeveloperSettings, Projects, AssetRegistry, EditorSubsystem, UnrealEd

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithCoreModule` | IModuleInterface. Starts HTTP server, registers core tools, owns `TUniquePtr<FMonolithHttpServer>` |
| `FMonolithHttpServer` | Embedded MCP HTTP server. JSON-RPC 2.0 dispatch over HTTP. Fully stateless (no session tracking) |
| `FMonolithToolRegistry` | Central singleton action registry. `TMap<FString, FRegisteredAction>` keyed by "namespace.action". Thread-safe — releases lock before executing handlers |
| `FMonolithJsonUtils` | Static JSON-RPC 2.0 helpers. Standard error codes (-32700 through -32603). Declares `LogMonolith` category |
| `FMonolithAssetUtils` | Asset loading with 4-tier fallback: StaticLoadObject(resolved) -> PackageName.ObjectName -> FindObject+_C suffix -> ForEachObjectWithPackage |
| `UMonolithSettings` | UDeveloperSettings (config=Monolith). ServerPort, bAutoUpdateEnabled, DatabasePathOverride, EngineSourceDBPathOverride, EngineSourcePath, 8 module enable toggles (functional — checked at registration time), LogVerbosity. Settings UI customized via `FMonolithSettingsCustomization` (IDetailCustomization) with re-index buttons for project and source databases |
| `UMonolithUpdateSubsystem` | UEditorSubsystem. GitHub Releases auto-updater. Shows dialog window with full release notes on update detection. Downloads zip, cross-platform extraction (PowerShell on Windows, unzip on Mac/Linux). Stages to Saved/Monolith/Staging/, hot-swaps on editor exit via FCoreDelegates::OnPreExit. Current version always from compiled MONOLITH_VERSION (version.json only stores pending/staging state). Release zips include pre-compiled DLLs. |
| `FMonolithCoreTools` | Registers 4 core actions |

#### Actions (4 — namespace: "monolith")

| Action | MCP Tool | Description |
|--------|----------|-------------|
| `discover` | `monolith_discover` | List available tool namespaces and their actions. Optional `namespace` filter |
| `status` | `monolith_status` | Server health: version, uptime, port, action count, engine_version, project_name |
| `update` | `monolith_update` | Check/install updates from GitHub Releases. `action`: "check" or "install" |
| `reindex` | `monolith_reindex` | Trigger full project re-index (via reflection to MonolithIndex, no hard dependency) |

---

### 3.2 MonolithBlueprint

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, BlueprintGraph, Json, JsonUtilities

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithBlueprintModule` | Registers 6 blueprint actions |
| `FMonolithBlueprintActions` | Static handlers. Uses `FMonolithAssetUtils::LoadAssetByPath<UBlueprint>` |
| `MonolithBlueprintInternal` | Helpers: AddGraphArray, FindGraphByName, PinTypeToString, SerializePin/Node, TraceExecFlow, FindEntryNode |

#### Actions (6 — namespace: "blueprint")

| Action | Params | Description |
|--------|--------|-------------|
| `list_graphs` | `asset_path` | List all graphs with name/type/node_count. Graph types: event_graph, function, macro, delegate_signature |
| `get_graph_summary` | `asset_path`, `graph_name` | Lightweight graph overview: node id/class/title + exec connections only (~10KB vs 172KB for full data) |
| `get_graph_data` | `asset_path`, `graph_name`, `node_class_filter` | Full graph with all nodes, pins (17+ type categories), connections, positions. Optional class filter |
| `get_variables` | `asset_path` | All NewVariables: name, type (with container prefix), default (from CDO), category, flags (editable, read_only, expose_on_spawn, replicated, transient) |
| `get_execution_flow` | `asset_path`, `entry_point` | Linearized exec trace from entry point. Handles branching (multiple exec outputs). MaxDepth=100 |
| `search_nodes` | `asset_path`, `query` | Case-insensitive search by title, class name, or function name |

---

### 3.3 MonolithMaterial

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, MaterialEditor, EditorScriptingUtilities, RenderCore, RHI, Slate, SlateCore, Json, JsonUtilities

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithMaterialModule` | Registers 25 material actions |
| `FMonolithMaterialActions` | Static handlers + helpers for loading materials and serializing expressions |

#### Actions (25 — namespace: "material")

**Read Actions (14)**
| Action | Description |
|--------|-------------|
| `get_all_expressions` | Get all expression nodes in a base material |
| `get_expression_details` | Full property reflection, inputs, outputs for a single expression |
| `get_full_connection_graph` | Complete connection graph (all wires) of a material |
| `export_material_graph` | Export complete graph to JSON (round-trippable with build_material_graph) |
| `validate_material` | BFS reachability check — detects islands, broken textures, missing functions, duplicate params, unused params, high expression count (>200). Optional auto-fix |
| `render_preview` | Save preview PNG to Saved/Monolith/previews/ |
| `get_thumbnail` | Return thumbnail as base64 PNG or save to file |
| `get_layer_info` | Material Layer / Material Layer Blend info |
| `get_material_parameters` | List all parameter types (scalar, vector, texture, static switch) with values. Works on UMaterial and UMaterialInstanceConstant |
| `get_compilation_stats` | Sampler count, texture estimates, UV scalars, blend mode, expression count |

**Write Actions (11)**
| Action | Description |
|--------|-------------|
| `create_material` | Create new UMaterial at path with configurable defaults (blend mode, shading model, material domain) |
| `create_material_instance` | Create UMaterialInstanceConstant from parent material with optional parameter overrides |
| `set_material_property` | Set material properties (blend_mode, shading_model, two_sided, etc.) via UMaterialEditingLibrary |
| `build_material_graph` | Build entire graph from JSON spec in single undo transaction (4 phases: standard nodes, Custom HLSL, wires, output properties) |
| `disconnect_expression` | Disconnect inputs or outputs on a named expression (supports expr→expr and expr→material property) |
| `delete_expression` | Delete expression node by name from material graph |
| `create_custom_hlsl_node` | Create Custom HLSL expression with inputs, outputs, and code |
| `set_expression_property` | Set properties on expression nodes (e.g., DefaultValue on scalar param) |
| `connect_expressions` | Wire expression outputs to expression inputs or material property inputs |
| `set_instance_parameter` | Set scalar/vector/texture/static switch parameters on material instances |
| `duplicate_material` | Duplicate material asset to new path |
| `recompile_material` | Force material recompile |
| `import_material_graph` | Import graph from JSON. Mode: "overwrite" (clear+rebuild) or "merge" (offset +500 X) |
| `begin_transaction` | Begin named undo transaction for batching edits |
| `end_transaction` | End current undo transaction |

---

### 3.4 MonolithAnimation

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AnimGraph, AnimGraphRuntime, BlueprintGraph, AnimationBlueprintLibrary, PoseSearch, AnimationModifiers, EditorScriptingUtilities, Json, JsonUtilities

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithAnimationModule` | Registers 62 animation actions (57 animation + 5 PoseSearch) |
| `FMonolithAnimationActions` | Static handlers organized in 15 groups |

#### Actions (62 — namespace: "animation")

**Sequence Info (4) — read-only**
| Action | Description |
|--------|-------------|
| `get_sequence_info` | Get sequence metadata (duration, frames, root motion, compression, etc.) |
| `get_sequence_notifies` | Get all notifies on an animation asset (sequence, montage, composite) |
| `get_bone_track_keys` | Get position/rotation/scale keys for a bone track (with optional frame range) |
| `get_sequence_curves` | Get float and transform curves on an animation sequence |

**Bone Track Editing (3)**
| Action | Description |
|--------|-------------|
| `set_bone_track_keys` | Set position/rotation/scale keys (JSON arrays) |
| `add_bone_track` | Add a bone track to an animation sequence |
| `remove_bone_track` | Remove a bone track (with optional `include_children`) |

**Notify Operations (6)**
| Action | Description |
|--------|-------------|
| `add_notify` | Add a point notify to an animation asset |
| `add_notify_state` | Add a state notify (with duration) to an animation asset |
| `remove_notify` | Remove a notify by index |
| `set_notify_time` | Set trigger time of an animation notify |
| `set_notify_duration` | Set duration of a state animation notify |
| `set_notify_track` | Move a notify to a different track |

**Curve Operations (5)**
| Action | Description |
|--------|-------------|
| `list_curves` | List all animation curves on a sequence (optional `include_keys`) |
| `add_curve` | Add a float or transform curve to an animation sequence |
| `remove_curve` | Remove a curve from an animation sequence |
| `set_curve_keys` | Set keys on a float curve (replaces existing keys) |
| `get_curve_keys` | Get all keys from a float curve |

**BlendSpace Operations (5)**
| Action | Description |
|--------|-------------|
| `get_blend_space_info` | Get blend space samples and axis settings |
| `add_blendspace_sample` | Add a sample to a blend space |
| `edit_blendspace_sample` | Edit sample position and optionally its animation |
| `delete_blendspace_sample` | Delete a sample by index |
| `set_blend_space_axis` | Configure axis (name, range, grid divisions, snap, wrap) |

**ABP Graph Reading (8) — read-only**
| Action | Description |
|--------|-------------|
| `get_abp_info` | Get ABP overview (skeleton, graphs, state machines, variables, interfaces) |
| `get_state_machines` | Get all state machines with full topology |
| `get_state_info` | Detailed info about a state in a state machine |
| `get_transitions` | All transitions (supports empty machine_name for ALL state machines) |
| `get_blend_nodes` | Blend nodes in an ABP graph |
| `get_linked_layers` | Linked animation layers |
| `get_graphs` | All graphs in an ABP |
| `get_nodes` | Animation nodes with optional class and graph_name filters |

**Montage Operations (8)**
| Action | Description |
|--------|-------------|
| `get_montage_info` | Get montage sections, slots, blend settings |
| `add_montage_section` | Add a section to an animation montage |
| `delete_montage_section` | Delete a section by index |
| `set_section_next` | Set the next section for a montage section |
| `set_section_time` | Set start time of a montage section |
| `set_montage_blend` | Set blend in/out times and auto blend out |
| `add_montage_slot` | Add a slot track to a montage |
| `set_montage_slot` | Rename a slot track by index |

**Skeleton Operations (9)**
| Action | Description |
|--------|-------------|
| `get_skeleton_info` | Skeleton bone hierarchy, virtual bones, and sockets |
| `get_skeletal_mesh_info` | Mesh info: morph targets, sockets, LODs, materials |
| `get_skeleton_sockets` | Get sockets from a skeleton or skeletal mesh |
| `get_skeleton_curves` | Get all registered animation curve names from a skeleton |
| `add_virtual_bone` | Add a virtual bone to a skeleton |
| `remove_virtual_bones` | Remove virtual bones (specific names) |
| `add_socket` | Add a socket to a skeleton |
| `remove_socket` | Remove a socket from a skeleton |
| `set_socket_transform` | Set the transform of a skeleton socket |

**Root Motion (1)**
| Action | Description |
|--------|-------------|
| `set_root_motion_settings` | Configure root motion settings (enable, lock mode, force root lock) |

**Asset Creation (3)**
| Action | Description |
|--------|-------------|
| `create_sequence` | Create a new empty animation sequence |
| `duplicate_sequence` | Duplicate an animation sequence to a new path |
| `create_montage` | Create a new animation montage with skeleton |

**Anim Modifiers (2)**
| Action | Description |
|--------|-------------|
| `apply_anim_modifier` | Apply an animation modifier class to a sequence |
| `list_anim_modifiers` | List animation modifiers applied to a sequence |

**Composites (3)**
| Action | Description |
|--------|-------------|
| `get_composite_info` | Get segments and metadata from an animation composite |
| `add_composite_segment` | Add a segment to an animation composite |
| `remove_composite_segment` | Remove a segment from an animation composite by index |

**PoseSearch (5)**
| Action | Description |
|--------|-------------|
| `get_pose_search_schema` | Get PoseSearch schema config and channels |
| `get_pose_search_database` | Get PoseSearch database sequences and schema reference |
| `add_database_sequence` | Add an animation sequence to a PoseSearch database |
| `remove_database_sequence` | Remove a sequence from a PoseSearch database by index |
| `get_database_stats` | Get PoseSearch database statistics (pose count, search mode, costs) |

---

### 3.5 MonolithNiagara

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, Niagara, NiagaraCore, NiagaraEditor, Json, JsonUtilities, AssetTools, Slate, SlateCore

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithNiagaraModule` | Registers 41 Niagara actions |
| `FMonolithNiagaraActions` | Static handlers + extensive private helpers |
| `MonolithNiagaraHelpers` | 6 reimplemented NiagaraEditor functions (non-exported APIs) |

#### Reimplemented NiagaraEditor Helpers

These exist because Epic's `FNiagaraStackGraphUtilities` functions lack `NIAGARAEDITOR_API`:

1. `GetOrderedModuleNodes` — Module execution order
2. `GetStackFunctionInputOverridePin` — Override pin lookup
3. `GetModuleIsEnabled` — Module enabled state
4. `RemoveModuleFromStack` — Module removal
5. `GetParametersForContext` — System user store params
6. `GetStackFunctionInputs` — Full input enumeration via engine's `FNiagaraStackGraphUtilities::GetStackFunctionInputs` with `FCompileConstantResolver`. Returns all input types (floats, vectors, colors, data interfaces, enums, bools) — not just static switch pins

#### Actions (41 — namespace: "niagara")

> **Note:** All Niagara actions accept `asset_path` (preferred) or `system_path` (backward compatible) for the system asset path parameter.
>
> **Input name conventions:** `get_module_inputs` returns short names (no `Module.` prefix). All write actions that accept input names (`set_module_input_value`, `set_module_input_binding`, `set_module_input_di`, `set_curve_value`) accept both short names and `Module.`-prefixed names.
>
> **Emitter name matching:** `FindEmitterHandleIndex` does NOT auto-select a single emitter when a specific non-matching name is passed. If a name is provided it must match exactly (case-insensitive).

**System (10)**
| Action | Description |
|--------|-------------|
| `add_emitter` | Add an emitter (UE 5.7: takes FGuid VersionGuid) |
| `remove_emitter` | Remove an emitter |
| `duplicate_emitter` | Duplicate an emitter within a system. Accepts `emitter` as alias for `source_emitter` |
| `set_emitter_enabled` | Enable/disable an emitter |
| `reorder_emitters` | Reorder emitters (direct handle assignment + PostEditChange + MarkPackageDirty for proper change notifications) |
| `set_emitter_property` | Set property: SimTarget, bLocalSpace, bDeterminism, RandomSeed, AllocationMode, PreAllocationCount, bRequiresPersistentIDs, MaxGPUParticlesSpawnPerFrame |
| `request_compile` | Request system compilation |
| `create_system` | Create new system (blank or from template via DuplicateAsset) |
| `list_emitters` | List all emitters with name, index, enabled, sim_target, renderer_count |
| `list_renderers` | List all renderers across emitters with class, index, enabled, material |

**Module (12)**
| Action | Description |
|--------|-------------|
| `get_ordered_modules` | Get ordered modules in a script stage |
| `get_module_inputs` | Get all inputs (floats, vectors, colors, data interfaces, enums, bools) with override values and linked params. Uses engine's `FNiagaraStackGraphUtilities::GetStackFunctionInputs`. Returns short names (no `Module.` prefix) |
| `get_module_graph` | Node graph of a module script |
| `add_module` | Add module to script stage (uses FNiagaraStackGraphUtilities) |
| `remove_module` | Remove module from stack |
| `move_module` | Move module to new index (remove+re-add — **loses input overrides**) |
| `set_module_enabled` | Enable/disable a module |
| `set_module_input_value` | Set input value (float, int, bool, vec2/3/4, color, string) |
| `set_module_input_binding` | Bind input to a parameter |
| `set_module_input_di` | Set data interface on input. Validates input exists and is DataInterface type. `config` param accepts a JSON object. Accepts both short names and `Module.`-prefixed names |
| `create_module_from_hlsl` | **STUB — returns error** (NiagaraEditor APIs not exported) |
| `create_function_from_hlsl` | **STUB — returns error** |

**Parameter (9)**
| Action | Description |
|--------|-------------|
| `get_all_parameters` | All parameters (user + per-emitter rapid iteration) |
| `get_user_parameters` | User-exposed parameters only |
| `get_parameter_value` | Get a parameter value |
| `get_parameter_type` | Type info (size, is_float, is_DI, is_enum, struct) |
| `trace_parameter_binding` | Find all usage sites of a parameter |
| `add_user_parameter` | Add user parameter with optional default |
| `remove_user_parameter` | Remove a user parameter |
| `set_parameter_default` | Set parameter default value |
| `set_curve_value` | Set curve keys on a module input |

**Renderer (6)**
| Action | Description |
|--------|-------------|
| `add_renderer` | Add renderer (Sprite, Mesh, Ribbon, Light, Component) |
| `remove_renderer` | Remove a renderer |
| `set_renderer_material` | Set renderer material (per-type handling) |
| `set_renderer_property` | Set property via reflection (float, double, int, bool, string, enum, byte, object) |
| `get_renderer_bindings` | Get attribute bindings via reflection |
| `set_renderer_binding` | Set attribute binding (ImportText with fallback format) |

**Batch (2)**
| Action | Description |
|--------|-------------|
| `batch_execute` | Execute multiple operations in one undo transaction (23 sub-op types — all write ops including: remove_user_parameter, set_parameter_default, set_module_input_di, set_curve_value, reorder_emitters, duplicate_emitter, set_renderer_binding, request_compile) |
| `create_system_from_spec` | Full declarative system builder from JSON spec. Uses `UNiagaraSystemFactoryNew::InitializeSystem` for proper system creation |

**Data Interface (1)**
| Action | Description |
|--------|-------------|
| `get_di_functions` | Get data interface function signatures |

**HLSL (1)**
| Action | Description |
|--------|-------------|
| `get_compiled_gpu_hlsl` | Get compiled GPU HLSL for an emitter |

#### UE 5.7 Compatibility Fixes (6 sites)

All marked with "UE 5.7 FIX" comments:
1. `AddEmitterHandle` takes `FGuid VersionGuid`
2-5. `GetOrCreateStackFunctionInputOverridePin` uses 5-param version (two FGuid params)
6. `RapidIterationParameters` accessed via direct UPROPERTY (no getter)

---

### 3.6 MonolithEditor

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, Json, JsonUtilities, MessageLog, LiveCoding (Win64 only)

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithEditorModule` | Creates FMonolithLogCapture, attaches to GLog, registers 13 actions |
| `FMonolithLogCapture` | FOutputDevice subclass. Ring buffer (10,000 entries max). Thread-safe. Tracks counts by verbosity |
| `FMonolithEditorActions` | Static handlers for build and log operations. Hooks into `ILiveCodingModule::GetOnPatchCompleteDelegate()` to capture compile results and timestamps |
| `FMonolithSettingsCustomization` | IDetailCustomization for UMonolithSettings. Adds re-index buttons for project and source databases in Project Settings UI |

#### Actions (13 — namespace: "editor")

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

---

### 3.7 MonolithConfig

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, Json, JsonUtilities

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithConfigModule` | Registers 6 config actions |
| `FMonolithConfigActions` | Static handlers. Helpers: ResolveConfigFilePath, GetConfigHierarchy (5 layers: Base -> Default -> Project -> User -> Saved). Uses GConfig API for reliable resolution |

#### Actions (6 — namespace: "config")

| Action | Description |
|--------|-------------|
| `resolve_setting` | Get effective value via `GConfig->GetString`. Params: `file` (category), `section`, `key` |
| `explain_setting` | Show where value comes from across Base->Default->User layers. Auto-searches Engine/Game/Input/Editor if only `setting` given |
| `diff_from_default` | Compare config layers using GConfig API. Supports 5 INI layers (Base, Default, Project, User, Saved). Reports modified + added. Optional `section` filter |
| `search_config` | Full-text search across all config files. Max 100 results. Optional `file` filter |
| `get_section` | Read entire config section from a file |
| `get_config_files` | List all .ini files with hierarchy level and sizes. Optional `category` filter |

---

### 3.8 MonolithIndex

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AssetRegistry, Json, JsonUtilities, SQLiteCore, Slate, SlateCore, BlueprintGraph, KismetCompiler, EditorSubsystem

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithIndexModule` | Registers 5 project actions |
| `FMonolithIndexDatabase` | RAII SQLite wrapper. 13 tables + 2 FTS5 + 6 triggers + 1 meta. DELETE journal mode, 64MB cache |
| `UMonolithIndexSubsystem` | UEditorSubsystem. Incremental + full indexing via FRunnable. Asset Registry callbacks for add/remove/rename. Deep asset indexing with game-thread batching. Batches every 100 assets. Progress notifications |
| `IMonolithIndexer` | Pure virtual interface: GetSupportedClasses(), IndexAsset(), GetName() |
| `FBlueprintIndexer` | Blueprint, WidgetBlueprint, AnimBlueprint — graphs, nodes, variables |
| `FMaterialIndexer` | Material, MaterialInstanceConstant, MaterialFunction — expressions, params, connections |
| `FAnimationIndexer` | AnimSequence, AnimMontage, BlendSpace, AnimBlueprint — tracks, notifies, slots, state machines |
| `FNiagaraIndexer` | NiagaraSystem, NiagaraEmitter — emitters, modules, parameters, renderers |
| `FDataTableIndexer` | DataTable — row names, struct type, column info |
| `FLevelIndexer` | World/MapBuildData — actors, components, sublevel references |
| `FGameplayTagIndexer` | GameplayTag containers — tag hierarchies and references |
| `FConfigIndexer` | Config/INI files — sections, keys, values across config hierarchy |
| `FCppIndexer` | C++ source files — classes, functions, includes (project-level source) |
| `FGenericAssetIndexer` | StaticMesh, SkeletalMesh, Texture2D, SoundWave, etc. — metadata nodes |
| `FDependencyIndexer` | Hard + Soft package dependencies (runs after all other indexers) |
| `FMonolithIndexNotification` | Slate notification bar with throbber + percentage |

#### Actions (5 — namespace: "project")

| Action | Params | Description |
|--------|--------|-------------|
| `search` | `query` (required), `limit` (50) | FTS5 full-text search across all indexed assets, nodes, variables, parameters |
| `find_references` | `asset_path` (required) | Bidirectional dependency lookup |
| `find_by_type` | `asset_type` (required), `limit` (100), `offset` (0) | Filter assets by class with pagination |
| `get_stats` | none | Row counts for all 11 tables + asset class breakdown (top 20) |
| `get_asset_details` | `asset_path` (required) | Deep inspection: nodes, variables, references for a single asset |

#### Database Schema

**13 Tables:** assets, nodes, connections, variables, parameters, dependencies, actors, tags, tag_references, configs, cpp_symbols, datatable_rows, meta

**2 FTS5 Virtual Tables:**
- `fts_assets` — content=assets, tokenize='porter unicode61', columns: asset_name, asset_class, description, package_path
- `fts_nodes` — content=nodes, tokenize='porter unicode61', columns: node_name, node_class, node_type

**DB Location:** `Plugins/Monolith/Saved/ProjectIndex.db`

---

### 3.9 MonolithSource

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, SQLiteCore, EditorSubsystem, UnrealEd, Json, JsonUtilities, Slate, SlateCore

**Note:** Module structure was flattened — the vestigial outer stub has been removed. MonolithSource now properly registers 10-11 actions (read_source, find_references, find_callers, find_callees, search_source, get_class_hierarchy, get_module_info, get_symbol_context, read_file, trigger_reindex).

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithSourceModule` | Registers 10 source actions (properly registers all actions after stub removal fix) |
| `UMonolithSourceSubsystem` | UEditorSubsystem. Owns read-only engine source DB. Manages Python indexer subprocess |
| `FMonolithSourceDatabase` | Read-only SQLite wrapper. Thread-safe via FCriticalSection. FTS queries with prefix matching |
| `FMonolithSourceActions` | 10 handlers. Helpers: IsForwardDeclaration (regex), ExtractMembers (smart class outline) |

#### Actions (10 — namespace: "source")

| Action | Params | Description |
|--------|--------|-------------|
| `read_source` | `symbol`, `include_header`, `max_lines`, `members_only` | Get source code for a class/function/struct. FTS fallback if exact match fails |
| `find_references` | `symbol`, `ref_kind`, `limit` | Find all usage sites |
| `find_callers` | `symbol`, `limit` | All functions that call the given function |
| `find_callees` | `symbol`, `limit` | All functions called by the given function |
| `search_source` | `query`, `scope`, `limit`, `mode`, `module`, `path_filter`, `symbol_kind` | Dual search: symbol FTS + source line FTS |
| `get_class_hierarchy` | `class_name`, `direction`, `depth` | Inheritance tree (both/ancestors/descendants, max 80 shown) |
| `get_module_info` | `module_name` | Module stats: file count, symbol counts, key classes |
| `get_symbol_context` | `symbol`, `context_lines` | Definition with surrounding context |
| `read_file` | `file_path`, `start_line`, `end_line` | Read source lines by path (absolute -> DB exact -> DB suffix match) |
| `trigger_reindex` | none | Trigger Python indexer subprocess |

**DB Location:** `Plugins/Monolith/Saved/EngineSource.db`

---

## 4. Python Source Indexer

**Location:** `Scripts/source_indexer/`
**Entry point:** `python -m source_indexer --source PATH --db PATH [--shaders PATH]`
**Dependencies:** tree-sitter>=0.21.0, tree-sitter-cpp>=0.21.0, Python 3.10+

### Pipeline (IndexingPipeline)

1. **Module Discovery** — Walks Runtime, Editor, Developer, Programs under Engine/Source + Engine/Plugins. Optionally Engine/Shaders
2. **File Processing** — C++ files -> CppParser (tree-sitter AST) -> symbols, includes. Shader files -> ShaderParser (regex) -> symbols, includes
3. **Source Line FTS** — Chunks source in batches of 10 lines into source_fts table
4. **Finalization** — Resolves inheritance, runs ReferenceBuilder for call/type cross-references

### Parsers

| Parser | Technology | Handles |
|--------|-----------|---------|
| CppParser | tree-sitter-cpp | Classes, structs, enums, functions, variables, macros, typedefs. UE macro awareness (UCLASS, USTRUCT, UENUM, UFUNCTION, UPROPERTY). 3 fallback strategies |
| ShaderParser | Regex | #include, #define, struct, function declarations in .usf/.ush |
| ReferenceBuilder | tree-sitter-cpp (2nd pass) | Call references, type references, local variable type resolution |

### Source DB Schema

| Table | Purpose |
|-------|---------|
| `modules` | id, name, path, module_type, build_cs_path |
| `files` | id, path, module_id, file_type, line_count, last_modified |
| `symbols` | id, name, qualified_name, kind, file_id, line_start, line_end, parent_symbol_id, access, signature, docstring, is_ue_macro |
| `inheritance` | id, child_id, parent_id |
| `references` | id, from_symbol_id, to_symbol_id, ref_kind, file_id, line |
| `includes` | id, file_id, included_path, line |
| `symbols_fts` | FTS5 on name, qualified_name, docstring |
| `source_fts` | FTS5 on text (file_id, line_number UNINDEXED) |
| `meta` | key, value |

---

## 5. Skills (9 bundled)

| Skill | Trigger Words | Entry Point | Actions |
|-------|--------------|-------------|---------|
| unreal-animation | animation, montage, ABP, blend space, notify, curves, compression, PoseSearch | `animation_query()` | 67 |
| unreal-blueprints | Blueprint, BP, event graph, node, variable | `blueprint_query()` | 6 |
| unreal-build | build, compile, Live Coding, hot reload, rebuild | `editor_query()` | 13 |
| unreal-cpp | C++, header, include, UCLASS, Build.cs, linker error | `source_query()` + `config_query()` | 10+6 |
| unreal-debugging | build error, crash, log, debug, stack trace | `editor_query()` | 13 |
| unreal-materials | material, shader, PBR, texture, material graph | `material_query()` | 25 |
| unreal-niagara | Niagara, particle, VFX, emitter | `niagara_query()` | 41 |
| unreal-performance | performance, optimization, FPS, frame time | Cross-domain | config + material + niagara |
| unreal-project-search | find asset, search project, dependencies | `project_query()` | 5 |

All skills follow a common structure: YAML frontmatter, Discovery section, Asset Path Conventions table, action tables, workflow examples, and rules.

---

## 6. Configuration

**Settings location:** Editor Preferences > Plugins > Monolith
**Config file:** `Config/MonolithSettings.ini` section `[/Script/MonolithCore.MonolithSettings]`

| Setting | Default | Description |
|---------|---------|-------------|
| ServerPort | 9316 | MCP HTTP server port |
| bAutoUpdateEnabled | True | GitHub Releases auto-check on startup |
| DatabasePathOverride | (empty) | Override default DB path (Plugins/Monolith/Saved/) |
| EngineSourceDBPathOverride | (empty) | Override engine source DB path |
| EngineSourcePath | (empty) | Override engine source directory |
| bBlueprintEnabled | True | Enable Blueprint module |
| bMaterialEnabled | True | Enable Material module |
| bAnimationEnabled | True | Enable Animation module |
| bNiagaraEnabled | True | Enable Niagara module |
| bEditorEnabled | True | Enable Editor module |
| bConfigEnabled | True | Enable Config module |
| bIndexEnabled | True | Enable Index module |
| bSourceEnabled | True | Enable Source module |
| LogVerbosity | 3 (Log) | 0=Silent, 1=Error, 2=Warning, 3=Log, 4=Verbose |

**Note:** Module enable toggles are functional — each module checks its toggle at registration time and skips action registration if disabled.

---

## 7. Templates

| File | Purpose |
|------|---------|
| `Templates/.mcp.json.example` | Minimal MCP config. Transport type varies by client: `"http"` for Claude Code, `"streamableHttp"` for Cursor/Cline. URL: `http://localhost:9316/mcp` |
| `Templates/CLAUDE.md.example` | Project instructions template with tool reference, workflow, asset path conventions, and rules |

---

## 8. File Structure

```
YourProject/Plugins/Monolith/
  Monolith.uplugin
  README.md
  LICENSE                          (MIT)
  ATTRIBUTION.md                   (Credits: Codeturion concept, tumourlove originals)
  .gitignore
  Config/
    MonolithSettings.ini
  Docs/
    plans/
      2026-03-06-monolith-design.md
      2026-03-06-monolith-implementation-plan.md
      phase-3-animation-niagara.md
  Plans/
    Phase6_Skills_Templates_Polish.md
  Skills/
    unreal-animation/unreal-animation.md
    unreal-blueprints/unreal-blueprints.md
    unreal-cpp/unreal-cpp.md
    unreal-debugging/unreal-debugging.md
    unreal-materials/unreal-materials.md
    unreal-niagara/unreal-niagara.md
    unreal-performance/unreal-performance.md
    unreal-project-search/unreal-project-search.md
  Templates/
    .mcp.json.example
    CLAUDE.md.example
  Scripts/
    source_indexer/                (Python tree-sitter indexer)
      db/schema.py
      ...
  MCP/
    pyproject.toml                 (Package scaffold — CLI is unimplemented stub)
    src/monolith_source/
  Source/
    MonolithCore/                  (8 source files)
    MonolithBlueprint/             (4 source files)
    MonolithMaterial/              (4 source files)
    MonolithAnimation/             (6 source files — includes PoseSearch)
    MonolithNiagara/               (4 source files)
    MonolithEditor/                (4 source files)
    MonolithConfig/                (4 source files)
    MonolithIndex/                 (12+ source files)
    MonolithSource/                (8 source files)
  Saved/
    .gitkeep
```

---

## 9. Deployment

### Development & Release Workflow

Everything lives in one place: `YourProject/Plugins/Monolith/`

This folder is both the working copy and the git repo (`git@github.com:tumourlove/monolith.git`). Edit, build, commit, push, and release all happen here — no file copying.

#### Publishing a release

1. Bump version in `Source/MonolithCore/Public/MonolithCoreModule.h` (`MONOLITH_VERSION`) and `Monolith.uplugin` (`VersionName`)
2. Update `CHANGELOG.md`
3. UBT build (bakes version into DLLs)
4. `git add -A && git commit && git push origin master`
5. Create zip: `powershell -ExecutionPolicy Bypass -File Scripts/make_release.ps1 -Version "X.Y.Z"` (excludes Intermediate/Saved/.git, sets `"Installed": true` for BP-only users)
6. `gh release create vX.Y.Z "../Monolith-vX.Y.Z.zip" --title "..." --notes "..."`

**Important:** Release zips MUST include pre-compiled DLLs (`Binaries/Win64/*.dll`) so Blueprint-only users can use the plugin without rebuilding. The `make_release.ps1` script sets `"Installed": true` in the zip's `.uplugin` to suppress rebuild prompts. The local dev copy keeps `"Installed": false`.

#### Auto-updater flow

1. On editor startup (5s delay), checks `api.github.com/repos/tumourlove/monolith/releases/latest`
2. Compares `tag_name` semver against compiled `MONOLITH_VERSION`
3. If newer: shows a dialog window with full release notes + "Install Update" / "Remind Me Later"
4. Download stages to `Saved/Monolith/Staging/` (NOT Plugins/ — would cause UBT conflicts)
5. On editor exit, a detached swap script runs:
   - Polls `tasklist` for `UnrealEditor.exe` until it's gone (120s timeout)
   - Asks for user confirmation (Y/N)
   - `move` command with retry loop (10 attempts × 3s) to handle Defender/Indexer file locks
   - `xcopy /h` copies new version, preserves `.git/`, `.gitignore`, `.github/`
   - Rollback on failure: removes partial copy, restores backup
   - Shows conditional message: C++ users rebuild, BP-only users launch immediately

### Installation (for other projects)

1. Clone to `YourProject/Plugins/Monolith`
2. Copy `Templates/.mcp.json.example` to project root as `.mcp.json`
3. Launch editor — Monolith auto-starts and indexes
4. Optionally copy `Skills/*` to `~/.claude/skills/`

---

## 10. Known Issues & Workarounds

See `TODO.md` for the full list. Key architectural constraints:

- **Niagara HLSL creation stubs** — NiagaraEditor APIs not exported by Epic
- **6 reimplemented NiagaraEditor helpers** — Same non-export issue
- **SSE is stub-only** — GET endpoint returns single event and close, not full streaming

---

## 11. Action Count Summary

| Module | Namespace | Actions |
|--------|-----------|---------|
| MonolithCore | monolith | 4 |
| MonolithBlueprint | blueprint | 6 |
| MonolithMaterial | material | 25 |
| MonolithAnimation | animation | 62 |
| MonolithNiagara | niagara | 41 |
| MonolithEditor | editor | 13 |
| MonolithConfig | config | 6 |
| MonolithIndex | project | 5 |
| MonolithSource | source | 10 |
| **Total** | | **172** |

**Note:** All skill files now correctly reflect the C++ action counts (172 total). The original Python server had higher counts (~231 tools) due to fragmented action design.
