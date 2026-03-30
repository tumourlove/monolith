# Monolith — Technical Specification

**Version:** 0.11.0 (Beta)
**Wiki:** https://github.com/tumourlove/monolith/wiki
**Engine:** Unreal Engine 5.7+
**Platform:** Windows, macOS, Linux
**License:** MIT
**Author:** tumourlove
**Repository:** https://github.com/tumourlove/monolith

---

## 1. Overview

Monolith is a unified Unreal Engine editor plugin that consolidates 9 separate MCP (Model Context Protocol) servers and 4 C++ plugins into a single plugin with an embedded HTTP MCP server. It reduces ~220 individual tools down to 15 MCP tools (815 total actions across 12 domains; 770 active by default — 45 experimental town gen actions disabled), cutting AI assistant context consumption by ~95%.

### What It Replaces

| Original Server/Plugin | Actions | Replaced By |
|------------------------|---------|-------------|
| unreal-blueprint-mcp + BlueprintReader | 46 | MonolithBlueprint |
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
  MonolithBlueprint     — Blueprint inspection, variable/component/graph CRUD, node operations, compile (86 actions)
  MonolithMaterial      — Material inspection + graph editing + CRUD + function suite (57 actions)
  MonolithAnimation     — Animation sequences, montages, ABPs, curves, notifies, skeletons, PoseSearch (115 actions)
  MonolithNiagara       — Niagara particle systems, HLSL module/function creation, DI config, event handlers, sim stages, NPC, effect types (96 actions)
  MonolithEditor        — Build triggers, live compile, log capture, compile output, crash context, scene capture, texture import (19 actions)
  MonolithConfig        — Config/INI resolution and search (6 actions)
  MonolithIndex         — SQLite FTS5 deep project indexer, 14 internal indexers (7 MCP actions)
  MonolithSource        — Engine source + API lookup (11 actions)
  MonolithUI            — Widget blueprint CRUD, templates, styling, animation, settings scaffolding, accessibility (42 actions)
  MonolithMesh          — Mesh inspection, scene manipulation, spatial queries, level blockout, GeometryScript ops, horror/accessibility, lighting, audio/acoustics, performance, decals, level design, tech art, context props, procedural geometry (sweep walls, auto-collision, proc mesh caching, blueprint prefabs), genre presets, encounter design, accessibility reports (197 core actions) + EXPERIMENTAL procedural town generator (45 actions, disabled by default via bEnableProceduralTownGen) = 242 total
  MonolithGAS           — Gameplay Ability System integration: abilities, attributes, effects, ASC, tags, cues, targets, input, inspection, scaffolding (130 actions). Conditional on #if WITH_GBA
  MonolithBABridge      — Optional IModularFeatures bridge for Blueprint Assist integration. Exposes IMonolithGraphFormatter; enables BA-powered auto_layout across blueprint, material, animation, and niagara modules when Blueprint Assist is present (0 MCP actions — integration only)
```

### Discovery/Dispatch Pattern

All domain modules register actions with `FMonolithToolRegistry` (central singleton). Each domain exposes a single `{namespace}_query(action, params)` MCP tool. The 4 core tools (`monolith_discover`, `monolith_status`, `monolith_reindex`, `monolith_update`) are standalone. Conditional modules (e.g. MonolithGAS) gate registration on compile-time defines (`#if WITH_GBA`).

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
| All others (11) | Default | Editor |
| MonolithBABridge | Default | Editor (optional) |

### Plugin Dependencies

- Niagara
- SQLiteCore
- EnhancedInput
- EditorScriptingUtilities
- PoseSearch
- IKRig
- ControlRig
- RigVM
- GeometryScripting (optional — enables Tier 5 mesh operations)
- GameplayAbilities (optional — enables MonolithGAS module; `#if WITH_GBA` compile guard)

---

## 3. Module Reference

### 3.1 MonolithCore

**Dependencies:** Core, CoreUObject, Engine, HTTP, HTTPServer, Json, JsonUtilities, Slate, SlateCore, DeveloperSettings, Projects, AssetRegistry, EditorSubsystem, UnrealEd

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithCoreModule` | IModuleInterface. Starts HTTP server, registers core tools, owns `TUniquePtr<FMonolithHttpServer>` |
| `FMonolithHttpServer` | Embedded MCP HTTP server. JSON-RPC 2.0 dispatch over HTTP. Fully stateless (no session tracking). `tools/list` response embeds per-action param schemas in the `params` property description (`*name(type)` format, `*` = required) so AI clients see param names without calling `monolith_discover` first |
| `FMonolithToolRegistry` | Central singleton action registry. `TMap<FString, FRegisteredAction>` keyed by "namespace.action". Thread-safe — releases lock before executing handlers. Validates required params from schema before dispatch (skips `asset_path` — `GetAssetPath()` handles aliases itself). Returns descriptive error listing missing + provided keys |
| `FMonolithJsonUtils` | Static JSON-RPC 2.0 helpers. Standard error codes (-32700 through -32603). Declares `LogMonolith` category |
| `FMonolithAssetUtils` | Asset loading with 4-tier fallback: StaticLoadObject(resolved) -> PackageName.ObjectName -> FindObject+_C suffix -> ForEachObjectWithPackage |
| `UMonolithSettings` | UDeveloperSettings (config=Monolith). ServerPort, bAutoUpdateEnabled, DatabasePathOverride, EngineSourceDBPathOverride, EngineSourcePath, 10 module enable toggles + `bEnableProceduralTownGen` (experimental, default false) (functional — checked at registration time), LogVerbosity. Settings UI customized via `FMonolithSettingsCustomization` (IDetailCustomization) with re-index buttons for project and source databases |
| `UMonolithUpdateSubsystem` | UEditorSubsystem. GitHub Releases auto-updater. Shows dialog window with full release notes on update detection. Downloads zip, cross-platform extraction (PowerShell on Windows, unzip on Mac/Linux). Stages to Saved/Monolith/Staging/, hot-swaps on editor exit via FCoreDelegates::OnPreExit. Current version always from compiled MONOLITH_VERSION (version.json only stores pending/staging state). Release zips include pre-compiled DLLs. |
| `FMonolithCoreTools` | Registers 4 core actions |

#### Actions (4 — namespace: "monolith")

| Action | MCP Tool | Description |
|--------|----------|-------------|
| `discover` | `monolith_discover` | List available tool namespaces and their actions. Optional `namespace` filter |
| `status` | `monolith_status` | Server health: version, uptime, port, action count, engine_version, project_name |
| `update` | `monolith_update` | Check/install updates from GitHub Releases. `action`: "check" or "install" |
| `reindex` | `monolith_reindex` | Trigger project re-index. Defaults to incremental (hash-based delta); pass `force=true` for full wipe-and-rebuild (via reflection to MonolithIndex, no hard dependency) |

---

### 3.2 MonolithBlueprint

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, BlueprintGraph, Json, JsonUtilities

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithBlueprintModule` | Registers 86 blueprint actions |
| `FMonolithBlueprintActions` | Static handlers. Uses `FMonolithAssetUtils::LoadAssetByPath<UBlueprint>` |
| `MonolithBlueprintInternal` | Helpers: AddGraphArray, FindGraphByName, PinTypeToString, SerializePin/Node, TraceExecFlow, FindEntryNode |

#### Actions (86 — namespace: "blueprint")

**Read Actions (13)**
| Action | Params | Description |
|--------|--------|-------------|
| `list_graphs` | `asset_path` | List all graphs with name/type/node_count. Graph types: event_graph, function, macro, delegate_signature |
| `get_graph_summary` | `asset_path`, `graph_name` | Lightweight graph overview: node id/class/title + exec connections only (~10KB vs 172KB for full data) |
| `get_graph_data` | `asset_path`, `graph_name`, `node_class_filter` | Full graph with all nodes, pins (17+ type categories), connections, positions. Optional class filter |
| `get_variables` | `asset_path` | All NewVariables: name, type (with container prefix), default (from CDO), category, flags (editable, read_only, expose_on_spawn, replicated, transient) |
| `get_execution_flow` | `asset_path`, `entry_point` | Linearized exec trace from entry point. Handles branching (multiple exec outputs). MaxDepth=100 |
| `search_nodes` | `asset_path`, `query` | Case-insensitive search by title, class name, or function name |
| `get_components` | `asset_path` | List all components in the component hierarchy |
| `get_component_details` | `asset_path`, `component_name` | Full property reflection for a named component |
| `get_functions` | `asset_path` | List all functions with signatures, access, and purity flags |
| `get_event_dispatchers` | `asset_path` | List all event dispatchers with parameter signatures |
| `get_parent_class` | `asset_path` | Return the parent class of the Blueprint |
| `get_interfaces` | `asset_path` | List all implemented interfaces |
| `get_construction_script` | `asset_path` | Get the construction script graph |

**Variable CRUD (7)**
| Action | Params | Description |
|--------|--------|-------------|
| `add_variable` | `asset_path`, `variable_name`, `variable_type` | Add a new variable to the Blueprint |
| `remove_variable` | `asset_path`, `variable_name` | Remove a variable by name |
| `rename_variable` | `asset_path`, `old_name`, `new_name` | Rename a variable |
| `set_variable_type` | `asset_path`, `variable_name`, `variable_type` | Change a variable's type |
| `set_variable_defaults` | `asset_path`, `variable_name`, `default_value` | Set a variable's default value |
| `add_local_variable` | `asset_path`, `function_name`, `variable_name`, `variable_type` | Add a local variable inside a function graph |
| `remove_local_variable` | `asset_path`, `function_name`, `variable_name` | Remove a local variable from a function graph |

**Component CRUD (6)**
| Action | Params | Description |
|--------|--------|-------------|
| `add_component` | `asset_path`, `component_class`, `component_name` | Add a component to the Blueprint |
| `remove_component` | `asset_path`, `component_name` | Remove a component by name |
| `rename_component` | `asset_path`, `old_name`, `new_name` | Rename a component |
| `reparent_component` | `asset_path`, `component_name`, `new_parent` | Change a component's parent in the hierarchy |
| `set_component_property` | `asset_path`, `component_name`, `property_name`, `value` | Set a property on a component via reflection |
| `duplicate_component` | `asset_path`, `component_name`, `new_name` | Duplicate a component with all its settings |

**Graph Management (9)**
| Action | Params | Description |
|--------|--------|-------------|
| `add_function` | `asset_path`, `function_name` | Add a new function graph |
| `remove_function` | `asset_path`, `function_name` | Remove a function graph |
| `rename_function` | `asset_path`, `old_name`, `new_name` | Rename a function graph |
| `add_macro` | `asset_path`, `macro_name` | Add a new macro graph |
| `add_event_dispatcher` | `asset_path`, `dispatcher_name` | Add a new event dispatcher |
| `set_function_params` | `asset_path`, `function_name`, `params` | Set input/output parameters on a function |
| `implement_interface` | `asset_path`, `interface_class` | Add an interface to the Blueprint |
| `remove_interface` | `asset_path`, `interface_class` | Remove an interface from the Blueprint |
| `reparent_blueprint` | `asset_path`, `new_parent_class` | Change the Blueprint's parent class |

**Node & Pin Operations (6)**
| Action | Params | Description |
|--------|--------|-------------|
| `add_node` | `asset_path`, `graph_name`, `node_class`, `position` | Add a node to a graph. Accepts common aliases (e.g. `CallFunction`, `VariableGet`) and tries `K2_` prefix fallback for function calls |
| `remove_node` | `asset_path`, `graph_name`, `node_id` | Remove a node by ID |
| `connect_pins` | `asset_path`, `graph_name`, `source_node`, `source_pin`, `target_node`, `target_pin` | Connect two pins |
| `disconnect_pins` | `asset_path`, `graph_name`, `source_node`, `source_pin`, `target_node`, `target_pin` | Disconnect two pins |
| `set_pin_default` | `asset_path`, `graph_name`, `node_id`, `pin_name`, `default_value` | Set a pin's default value |
| `set_node_position` | `asset_path`, `graph_name`, `node_id`, `x`, `y` | Set a node's position in the graph |

**Compile & Create (5)**
| Action | Params | Description |
|--------|--------|-------------|
| `compile_blueprint` | `asset_path` | Compile the Blueprint and return errors/warnings |
| `validate_blueprint` | `asset_path` | Validate Blueprint without full compile — checks for broken references and missing overrides |
| `create_blueprint` | `save_path`, `parent_class` | Create a new Blueprint asset |
| `duplicate_blueprint` | `asset_path`, `new_path` | Duplicate a Blueprint asset to a new path |
| `get_dependencies` | `asset_path` | List all hard and soft asset dependencies |

**Layout (1)**
| Action | Params | Description |
|--------|--------|-------------|
| `auto_layout` | `asset_path`, `graph_name`?, `formatter`? | Auto-arrange nodes in a Blueprint graph. `formatter`: `"auto"` (default) — uses Blueprint Assist if available, falls back to built-in hierarchical layout; `"blueprint_assist"` — requires BA, errors if not present; `"builtin"` — built-in layout only |

---

### 3.3 MonolithMaterial

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, MaterialEditor, EditorScriptingUtilities, RenderCore, RHI, Slate, SlateCore, Json, JsonUtilities

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithMaterialModule` | Registers 57 material actions |
| `FMonolithMaterialActions` | Static handlers + helpers for loading materials and serializing expressions |

#### Actions (57 — namespace: "material")

**Read Actions (10)**
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
| `get_compilation_stats` | Sampler count, texture estimates, UV scalars, blend mode, expression count, vertex/pixel shader instruction counts (`num_vertex_shader_instructions`, `num_pixel_shader_instructions` via `UMaterialEditingLibrary::GetStatistics`) |

**Write Actions (15)**
| Action | Description |
|--------|-------------|
| `create_material` | Create new UMaterial at path with configurable defaults (blend mode, shading model, material domain) |
| `create_material_instance` | Create UMaterialInstanceConstant from parent material with optional parameter overrides |
| `set_material_property` | Set material properties (blend_mode, shading_model, two_sided, etc.) via UMaterialEditingLibrary |
| `build_material_graph` | Build entire graph from JSON spec in single undo transaction (4 phases: standard nodes, Custom HLSL, wires, output properties). The spec must be passed as `{ "graph_spec": { "nodes": [...], "connections": [...], ... } }` — not as a bare object |
| `disconnect_expression` | Disconnect inputs or outputs on a named expression (supports expr→expr and expr→material property; supports targeted single-connection disconnection via optional `input_name`/`output_name` params) |
| `delete_expression` | Delete expression node by name from material graph |
| `create_custom_hlsl_node` | Create Custom HLSL expression with inputs, outputs, and code |
| `set_expression_property` | Set properties on expression nodes (e.g., DefaultValue on scalar param). Calls `PostEditChangeProperty` with the actual `FProperty*` so `MaterialGraph->RebuildGraph()` fires and the editor display updates correctly |
| `connect_expressions` | Wire expression outputs to expression inputs or material property inputs. Returns blend mode validation warnings (e.g. Opacity on Opaque/Masked, OpacityMask on non-Masked) |
| `set_instance_parameter` | Set scalar/vector/texture/static switch parameters on material instances |
| `duplicate_material` | Duplicate material asset to new path |
| `recompile_material` | Force material recompile |
| `import_material_graph` | Import graph from JSON. Mode: "overwrite" (clear+rebuild) or "merge" (offset +500 X) |
| `begin_transaction` | Begin named undo transaction for batching edits |
| `end_transaction` | End current undo transaction |

**Material Function Actions (9)**
| Action | Description |
|--------|-------------|
| `export_function_graph` | Full graph export of a material function — nodes, connections, properties, inputs, outputs, static switch details |
| `set_function_metadata` | Update material function description, categories, and library exposure settings |
| `delete_function_expression` | Remove expression(s) from a material function graph |
| `update_material_function` | Recompile a material function and cascade changes to all referencing materials/instances |
| `create_function_instance` | Create a MaterialFunctionInstance with parent reference and optional parameter overrides |
| `set_function_instance_parameter` | Set parameter overrides on a MaterialFunctionInstance (supports 11 parameter types) |
| `get_function_instance_info` | Read MFI parent chain and all parameter overrides (11 types: scalar, vector, texture, font, static switch, static component mask, and more) |
| `layout_function_expressions` | Auto-arrange material function graph layout |
| `rename_function_parameter_group` | Rename a parameter group across all parameters in a material function |
| `auto_layout` | Auto-arrange expression nodes in a material graph. `formatter`: `"auto"` (default) — uses Blueprint Assist if available, falls back to built-in layout; `"blueprint_assist"` — requires BA; `"builtin"` — built-in only |

**Extended Actions (1)**
| Action | Change |
|--------|--------|
| `create_material_function` | Added `type` parameter — supports `MaterialLayer` and `MaterialLayerBlend` in addition to standard material functions |

---

### 3.4 MonolithAnimation

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AnimGraph, AnimGraphRuntime, BlueprintGraph, AnimationBlueprintLibrary, PoseSearch, AnimationModifiers, EditorScriptingUtilities, Json, JsonUtilities

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithAnimationModule` | Registers 115 animation actions |
| `FMonolithAnimationActions` | Static handlers organized in 15 groups |

#### Actions (115 — namespace: "animation")

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

**Layout (1)**
| Action | Description |
|--------|-------------|
| `auto_layout` | Auto-arrange nodes in an Animation Blueprint graph. `formatter`: `"auto"` (default) — uses Blueprint Assist if available, falls back to built-in hierarchical layout; `"blueprint_assist"` — requires BA; `"builtin"` — built-in only. Optional `graph_name` to target a specific graph |

---

### 3.5 MonolithNiagara

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, Niagara, NiagaraCore, NiagaraEditor, Json, JsonUtilities, AssetTools, Slate, SlateCore

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithNiagaraModule` | Registers 96 Niagara actions |
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

#### Actions (96 — namespace: "niagara")

> **Note:** All Niagara actions accept `asset_path` (preferred) or `system_path` (backward compatible) for the system asset path parameter.
>
> **Input name conventions:** `get_module_inputs` returns short names (no `Module.` prefix). All write actions that accept input names (`set_module_input_value`, `set_module_input_binding`, `set_module_input_di`, `set_curve_value`) accept both short names and `Module.`-prefixed names. For CustomHlsl modules, `get_module_inputs` and `set_module_input_value` fall back to reading/writing the FunctionCall node's typed input pins directly (CustomHlsl inputs don't appear in the ParameterMap history).
>
> **Param name aliases:** The canonical param names registered in schemas are `module_node` and `input`. All module write actions also accept these aliases: `module_node` → `module_name`, `module`; `input` → `input_name`. Use the canonical names when possible — aliases exist for backward compatibility.
>
> **Emitter name matching:** `FindEmitterHandleIndex` does NOT auto-select a single emitter when a specific non-matching name is passed. If a name is provided it must match exactly (case-insensitive). Numeric index strings (`"0"`, `"1"`, etc.) are also accepted as a fallback.

**System (14)**
| Action | Description |
|--------|-------------|
| `add_emitter` | Add an emitter (UE 5.7: takes FGuid VersionGuid) |
| `remove_emitter` | Remove an emitter |
| `duplicate_emitter` | Duplicate an emitter within a system. Accepts `emitter` as alias for `source_emitter` |
| `set_emitter_enabled` | Enable/disable an emitter |
| `reorder_emitters` | Reorder emitters (direct handle assignment + PostEditChange + MarkPackageDirty for proper change notifications) |
| `set_emitter_property` | Set property: SimTarget, bLocalSpace, bDeterminism, RandomSeed, AllocationMode, PreAllocationCount, bRequiresPersistentIDs, MaxGPUParticlesSpawnPerFrame, CalculateBoundsMode |
| `set_system_property` | Set a system-level property (WarmupTime, bDeterminism, etc.) |
| `request_compile` | Request system compilation. Params: `force` (bool), synchronous (bool) |
| `create_system` | Create new system (blank or from template via DuplicateAsset) |
| `list_emitters` | List all emitters with name, index, enabled, sim_target, renderer_count, GUID |
| `list_renderers` | List all renderers across emitters with class (short `type` name), index, enabled, material |
| `list_module_scripts` | Search available Niagara module scripts by keyword. Returns matching script asset paths |
| `list_renderer_properties` | List editable properties on a renderer. Params: `asset_path`, `emitter`, `renderer` |
| `get_system_diagnostics` | Compile errors, warnings, renderer/SimTarget incompatibility, GPU+dynamic bounds warnings, per-script stats (op count, registers, compile status). Added 2026-03-13 |

**Module (13)**
| Action | Description |
|--------|-------------|
| `get_ordered_modules` | Get ordered modules in a script stage |
| `get_module_inputs` | Get all inputs (floats, vectors, colors, data interfaces, enums, bools) with override values, linked params, and actual DI curve data. Uses engine's `FNiagaraStackGraphUtilities::GetStackFunctionInputs`. Returns short names (no `Module.` prefix). LinearColor/vector defaults deserialized from JSON string if needed |
| `get_module_graph` | Node graph of a module script |
| `add_module` | Add module to script stage (uses FNiagaraStackGraphUtilities) |
| `remove_module` | Remove module from stack |
| `move_module` | Move module to new index (remove+re-add — **loses input overrides**) |
| `set_module_enabled` | Enable/disable a module |
| `set_module_input_value` | Set input value (float, int, bool, vec2/3/4, color, string) |
| `set_module_input_binding` | Bind input to a parameter |
| `set_module_input_di` | Set data interface on input. Required: `di_class` (class name — `U` prefix optional, e.g. `NiagaraDataInterfaceCurve` or `UNiagaraDataInterfaceCurve`), optional `config` object (supports FRichCurve keys for curve DIs). Validates input exists and is DataInterface type. Accepts both short names and `Module.`-prefixed names |
| `set_static_switch_value` | Set a static switch value on a module |
| `create_module_from_hlsl` | Create a Niagara module script from custom HLSL. Params: `name`, `save_path`, `hlsl` (body), optional `inputs[]`/`outputs[]` (`{name, type}` objects), `description`. **HLSL body rules:** use bare input/output names (no `Module.` prefix — compiler adds `In_`/`Out_` automatically). Write particle attributes via `Particles.X` ParameterMap tokens directly in the body. No swizzle via dot on map variables. |
| `create_function_from_hlsl` | Create a Niagara function script from custom HLSL. Same params as `create_module_from_hlsl`. Script usage is set to `Function` instead of `Module`. |

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
| `set_curve_value` | Set curve keys on a module input. Params: `emitter`, `module_node`, `input`, `keys` (array of `{time, value}` objects) |

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

**Dynamic Inputs (5)**
| Action | Description |
|--------|-------------|
| `list_dynamic_inputs` | List all dynamic inputs on a module |
| `get_dynamic_input_tree` | Get the full tree structure of a dynamic input |
| `remove_dynamic_input` | Remove a dynamic input from a module |
| `get_dynamic_input_value` | Get the current value of a dynamic input |
| `get_dynamic_input_inputs` | Get all sub-inputs of a dynamic input |

**Emitter Management (3)**
| Action | Description |
|--------|-------------|
| `rename_emitter` | Rename an emitter within a system |
| `get_emitter_property` | Get a property value from an emitter via reflection |
| `list_available_renderers` | List all available renderer classes that can be added |

**Renderer Configuration (3)**
| Action | Description |
|--------|-------------|
| `set_renderer_mesh` | Set the mesh asset on a mesh renderer |
| `configure_ribbon` | Configure ribbon renderer settings (width, facing, tessellation, etc.) |
| `configure_subuv` | Configure SubUV animation settings on a renderer |

**Event Handlers (3)**
| Action | Description |
|--------|-------------|
| `get_event_handlers` | Get all event handlers on an emitter |
| `set_event_handler_property` | Set a property on an event handler |
| `remove_event_handler` | Remove an event handler from an emitter |

**Simulation Stages (3)**
| Action | Description |
|--------|-------------|
| `get_simulation_stages` | Get all simulation stages on an emitter |
| `set_simulation_stage_property` | Set a property on a simulation stage |
| `remove_simulation_stage` | Remove a simulation stage from an emitter |

**Module Outputs (1)**
| Action | Description |
|--------|-------------|
| `get_module_output_parameters` | Get output parameters exposed by a module |

**Niagara Parameter Collections (NPC) (5)**
| Action | Description |
|--------|-------------|
| `create_npc` | Create a Niagara Parameter Collection asset |
| `get_npc` | Get NPC contents (parameters, defaults, namespace) |
| `add_npc_parameter` | Add a parameter to an NPC |
| `remove_npc_parameter` | Remove a parameter from an NPC |
| `set_npc_default` | Set the default value of an NPC parameter |

**Effect Types (3)**
| Action | Description |
|--------|-------------|
| `create_effect_type` | Create a Niagara Effect Type asset |
| `get_effect_type` | Get effect type settings (scalability, significance, budget) |
| `set_effect_type_property` | Set a property on an effect type |

**Utilities (5)**
| Action | Description |
|--------|-------------|
| `get_available_parameters` | List available parameters that can be bound to inputs |
| `preview_system` | Capture a preview image of a Niagara system |
| `diff_systems` | Compare two Niagara systems and return structural differences |
| `save_emitter_as_template` | Save an emitter as a reusable template asset |
| `clone_module_overrides` | Clone input overrides from one module to another |
| `auto_layout` | Auto-arrange nodes in a Niagara module script graph. `formatter`: `"auto"` (default) — uses Blueprint Assist if available, falls back to built-in layout; `"blueprint_assist"` — requires BA; `"builtin"` — built-in only |

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
| `FMonolithEditorModule` | Creates FMonolithLogCapture, attaches to GLog, registers 19 actions |
| `FMonolithLogCapture` | FOutputDevice subclass. Ring buffer (10,000 entries max). Thread-safe. Tracks counts by verbosity |
| `FMonolithEditorActions` | Static handlers for build and log operations. Hooks into `ILiveCodingModule::GetOnPatchCompleteDelegate()` to capture compile results and timestamps |
| `FMonolithSettingsCustomization` | IDetailCustomization for UMonolithSettings. Adds re-index buttons for project and source databases in Project Settings UI |

#### Actions (19 — namespace: "editor")

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
| `FMonolithIndexModule` | Registers 7 project actions |
| `FMonolithIndexDatabase` | RAII SQLite wrapper. 13 tables + 2 FTS5 + 6 triggers + 1 meta. DELETE journal mode, 64MB cache. Schema v2: `saved_hash` column (Blake3 `FIoHash` hex), `schema_version` meta key |
| `UMonolithIndexSubsystem` | UEditorSubsystem. 3-layer indexing (startup delta, live AR callbacks, full fallback). Hash-based startup catch-up. Live batched AR delegates on 2s timer. Deep asset indexing with game-thread batching. Batches every 100 assets. Progress notifications |
| `IMonolithIndexer` | Pure virtual interface: GetSupportedClasses(), IndexAsset(), GetName(), IsSentinel(), SupportsIncrementalIndex(), IndexScoped() |
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

#### Actions (7 — namespace: "project")

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

#### Incremental Indexing

The project indexer uses a 3-layer architecture to keep `ProjectIndex.db` in sync without costly full rebuilds:

**Layer 1 — Startup Catch-Up (hash-based delta)**

On editor startup, `UMonolithIndexSubsystem` runs a fast delta engine:
1. `EnumerateAllPackages()` collects all discoverable `.uasset` packages with their `FIoHash` (Blake3).
2. Hash comparison against the `saved_hash` column in the `assets` table identifies added, removed, and changed assets. Move detection uses a `TMultiMap<FIoHash, FString>` to match removed→added pairs with identical hashes.
3. Delta application (inserts, updates, deletes, renames) executes in a single SQLite transaction.
4. Hash updates are deferred until after commit for crash recovery — if the editor crashes mid-index, the next startup re-detects the delta.

Performance: ~14K assets compared in ~20ms. <1s total startup time with no changes.

**Layer 2 — Live Asset Registry Callbacks**

Four AR delegates are registered at startup:
- `OnAssetsAdded` — new assets
- `OnAssetsRemoved` — deleted assets
- `OnAssetRenamed` — moved/renamed assets
- `OnAssetsUpdatedOnDisk` — externally modified assets

Events are batched into a pending queue and drained on a 2-second timer tick. The drain deduplicates entries (same asset touched multiple times within the window) and applies changes in a single transaction.

**Layer 3 — Forced Full Reindex (fallback)**

`monolith_reindex()` defaults to incremental mode (Layer 1 logic). Passing `force=true` triggers a full wipe-and-rebuild: drops all table data, re-enumerates, and re-indexes every asset. Used when the DB is suspected corrupt or after schema migrations.

**Schema v2 Migration**

Schema v2 adds:
- `saved_hash TEXT` column on the `assets` table (stores Blake3 `FIoHash` as hex string)
- `schema_version` key in the `meta` table
- Index on `saved_hash` for fast lookup

Migration is automatic: on startup, `PRAGMA table_info(assets)` checks for the `saved_hash` column. If missing, `ALTER TABLE assets ADD COLUMN saved_hash TEXT` runs followed by index creation.

**IMonolithIndexer Interface Additions**

| Method | Purpose |
|--------|---------|
| `IsSentinel()` | Returns true if this indexer acts as a sentinel for a specific asset type (used by incremental path to decide which indexers to invoke) |
| `SupportsIncrementalIndex()` | Returns true if the indexer can process individual asset changes without a full rebuild |
| `IndexScoped()` | Index a specific set of assets (subset of full index). Default implementation falls back to `IndexAsset()` per asset |

**Plugin Content Scope Fix**

The `bInstalled` filter on plugin content paths was replaced with explicit path enumeration. This fixes discovery of project-local plugins (e.g., DrawCallReducer, NiagaraDestructionDriver) that previously reported `bInstalled=false` and were excluded from indexing. The `MeshCatalogIndexer` paths were also corrected to use the new enumeration.

---

### 3.9 MonolithSource

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, SQLiteCore, EditorSubsystem, UnrealEd, Json, JsonUtilities, Slate, SlateCore

**Note:** Module structure was flattened — the vestigial outer stub has been removed. MonolithSource registers 11 actions. The engine source indexer is a native C++ implementation (`UMonolithSourceSubsystem` builds `EngineSource.db` in-process). The legacy Python tree-sitter indexer (`Scripts/source_indexer/`) is no longer used.

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithSourceModule` | Registers 11 source actions |
| `UMonolithSourceSubsystem` | UEditorSubsystem. Owns engine source DB. Runs native C++ source indexer. Exposes `TriggerReindex()` (full engine re-index) and `TriggerProjectReindex()` (project C++ only, incremental) |
| `FMonolithSourceDatabase` | Read-only SQLite wrapper. Thread-safe via FCriticalSection. FTS queries with prefix matching |
| `FMonolithSourceActions` | 11 handlers. Helpers: IsForwardDeclaration (regex), ExtractMembers (smart class outline) |
| `UMonolithQueryCommandlet` | UCommandlet. Offline CLI — run via `UnrealEditor-Cmd.exe ProjectName -run=MonolithQuery`. Replaces `monolith_offline.py` for read/query operations without a full editor session |

#### Actions (11 — namespace: "source")

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
| `trigger_reindex` | none | Trigger full C++ engine source re-index (replaces entire EngineSource.db) |
| `trigger_project_reindex` | none | Trigger incremental project-only C++ source re-index (updates project symbols in EngineSource.db without a full rebuild) |

**DB Location:** `Plugins/Monolith/Saved/EngineSource.db`

---

### 3.10 MonolithUI

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, UMGEditor, UMG, Slate, SlateCore, Json, JsonUtilities

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithUIModule` | Registers 42 UI actions |
| `FMonolithUIActions` | Widget blueprint CRUD: create, inspect, add/remove widgets, property writes, compile |
| `FMonolithUISlotActions` | Layout slot operations: slot properties, anchor presets, widget movement |
| `FMonolithUITemplateActions` | High-level HUD/menu/panel scaffold templates (8 templates) |
| `FMonolithUIStylingActions` | Visual styling: brush, font, color scheme, text, image, batch style |
| `FMonolithUIAnimationActions` | UMG widget animation CRUD: list, inspect, create, add/remove keyframes |
| `FMonolithUIBindingActions` | Event/property binding inspection, list view setup, widget binding queries |
| `FMonolithUISettingsActions` | Settings/save/audio/input remapping subsystem scaffolding (5 scaffolds) |
| `FMonolithUIAccessibilityActions` | Accessibility subsystem scaffold, audit, colorblind mode, text scale |

#### Actions (42 — namespace: "ui")

**Widget CRUD (7)**
| Action | Params | Description |
|--------|--------|-------------|
| `create_widget_blueprint` | `save_path`, `parent_class` | Create a new Widget Blueprint asset |
| `get_widget_tree` | `asset_path` | Get the full widget hierarchy tree |
| `add_widget` | `asset_path`, `widget_class`, `parent_slot` | Add a widget to the widget tree |
| `remove_widget` | `asset_path`, `widget_name` | Remove a widget from the widget tree |
| `set_widget_property` | `asset_path`, `widget_name`, `property_name`, `value` | Set a property on a widget via reflection |
| `compile_widget` | `asset_path` | Compile the Widget Blueprint and return errors/warnings |
| `list_widget_types` | none | List all available widget classes that can be instantiated |

**Slot Operations (3)**
| Action | Params | Description |
|--------|--------|-------------|
| `set_slot_property` | `asset_path`, `widget_name`, `property_name`, `value` | Set a layout slot property (padding, alignment, size, etc.) |
| `set_anchor_preset` | `asset_path`, `widget_name`, `preset` | Apply an anchor preset to a Canvas Panel slot |
| `move_widget` | `asset_path`, `widget_name`, `new_parent`, `slot_index` | Move a widget to a different parent slot |

**Templates (8)**
| Action | Params | Description |
|--------|--------|-------------|
| `create_hud_element` | `save_path`, `element_type` | Scaffold a common HUD element (health bar, crosshair, ammo counter, etc.) |
| `create_menu` | `save_path`, `menu_type` | Scaffold a menu Widget Blueprint (main menu, pause menu, etc.) |
| `create_settings_panel` | `save_path` | Scaffold a settings panel with common option categories |
| `create_dialog` | `save_path`, `dialog_type` | Scaffold a dialog Widget Blueprint (confirmation, info, input prompt) |
| `create_notification_toast` | `save_path` | Scaffold a notification/toast Widget Blueprint |
| `create_loading_screen` | `save_path` | Scaffold a loading screen Widget Blueprint with progress bar |
| `create_inventory_grid` | `save_path`, `columns`, `rows` | Scaffold a grid-based inventory Widget Blueprint |
| `create_save_slot_list` | `save_path` | Scaffold a save slot list Widget Blueprint |

**Styling (6)**
| Action | Params | Description |
|--------|--------|-------------|
| `set_brush` | `asset_path`, `widget_name`, `brush_property`, `texture_path` | Set a brush/image property on a widget |
| `set_font` | `asset_path`, `widget_name`, `font_asset`, `size` | Set the font and size on a text widget |
| `set_color_scheme` | `asset_path`, `color_map` | Apply a color scheme (name→LinearColor map) across the widget |
| `batch_style` | `asset_path`, `style_operations` | Apply multiple styling operations in a single transaction |
| `set_text` | `asset_path`, `widget_name`, `text` | Set display text on a text widget |
| `set_image` | `asset_path`, `widget_name`, `texture_path` | Set the texture on an image widget |

**Animation (5)**
| Action | Params | Description |
|--------|--------|-------------|
| `list_animations` | `asset_path` | List all UMG animations on a Widget Blueprint |
| `get_animation_details` | `asset_path`, `animation_name` | Get tracks and keyframes for a named animation |
| `create_animation` | `asset_path`, `animation_name` | Create a new UMG widget animation |
| `add_animation_keyframe` | `asset_path`, `animation_name`, `widget_name`, `property`, `time`, `value` | Add a keyframe to a widget animation track |
| `remove_animation` | `asset_path`, `animation_name` | Remove a UMG widget animation |

**Bindings (4)**
| Action | Params | Description |
|--------|--------|-------------|
| `list_widget_events` | `asset_path` | List all bindable events on a Widget Blueprint |
| `list_widget_properties` | `asset_path`, `widget_name` | List all bindable properties on a widget |
| `setup_list_view` | `asset_path`, `list_view_name`, `entry_widget_path` | Configure a List View widget with an entry widget class |
| `get_widget_bindings` | `asset_path` | Get all active property and event bindings on a Widget Blueprint |

**Settings Scaffolding (5)**
| Action | Params | Description |
|--------|--------|-------------|
| `scaffold_game_user_settings` | `save_path`, `class_name` | Scaffold a UGameUserSettings subclass with common settings properties |
| `scaffold_save_game` | `save_path`, `class_name` | Scaffold a USaveGame subclass with save slot infrastructure |
| `scaffold_save_subsystem` | `save_path`, `class_name` | Scaffold a save game subsystem (UGameInstanceSubsystem) |
| `scaffold_audio_settings` | `save_path`, `class_name` | Scaffold an audio settings manager with volume/mix controls |
| `scaffold_input_remapping` | `save_path`, `class_name` | Scaffold an input remapping system backed by Enhanced Input |

**Accessibility (4)**
| Action | Params | Description |
|--------|--------|-------------|
| `scaffold_accessibility_subsystem` | `save_path`, `class_name` | Scaffold a UGameInstanceSubsystem implementing accessibility features |
| `audit_accessibility` | `asset_path` | Audit a Widget Blueprint for common accessibility issues (missing tooltips, low contrast, small text) |
| `set_colorblind_mode` | `asset_path`, `mode` | Apply a colorblind-safe palette mode (deuteranopia, protanopia, tritanopia) |
| `set_text_scale` | `asset_path`, `scale` | Apply a global text scale factor to all text widgets in the blueprint |

---

### 3.11 MonolithMesh

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, MonolithIndex, SQLiteCore, UnrealEd, EditorSubsystem, MeshDescription, StaticMeshDescription, MeshConversion, PhysicsCore, NavigationSystem, RenderCore, EditorScriptingUtilities, Json, JsonUtilities, Slate, SlateCore. Optional: GeometryScriptingCore, GeometryFramework (Tier 5 mesh ops)

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithMeshModule` | Registers 197 core mesh actions across 30+ action classes (+ GeometryScript ops conditional). 45 additional experimental town gen actions registered only when `bEnableProceduralTownGen = true` (default: false). Total: 242 |
| `FMonolithMeshInspectionActions` | Mesh asset inspection: geometry stats, LODs, UVs, materials, collision, quality analysis, catalog (12 actions) |
| `FMonolithMeshSceneActions` | Scene actor manipulation: spawn, move, duplicate, delete, group, batch execute (8 actions) |
| `FMonolithMeshSpatialActions` | Spatial queries: raycasts, sweeps, overlaps, nearest, line of sight, navmesh, scene bounds/stats (11 actions) |
| `FMonolithMeshBlockoutActions` | Level blockout: volumes, primitives, grids, asset matching, replacement, layout import/export, prop scatter (15 actions) |
| `FMonolithMeshProceduralActions` | Procedural geometry: parametric furniture, structures, mazes, pipes, terrain, horror props, sweep-based walls, auto-collision, human-scale defaults, door/window trim frames (8 actions) |
| `FMonolithMeshCacheActions` | Procedural mesh caching: hash-based manifest, list/clear/validate/stats (4 actions) |
| `FMonolithMeshPrefabActions` | Blueprint prefabs: dialog-free HarvestBlueprintFromActors (1 action) |
| `FMonolithMeshBuildingActions` | Grid-based building construction: grid → geometry + Building Descriptor (2 actions) |
| `FMonolithMeshFloorPlanGenerator` | Automatic floor plan generation: treemap layout, archetype loading, corridor insertion (3 actions) |
| `FMonolithMeshFacadeActions` | Facade & window generation: window placement, trim profiles, horror damage (3 actions) |
| `FMonolithMeshRoofActions` | Roof generation: gable, hip, flat/parapet, shed, gambrel (1 action) |
| `FMonolithMeshCityBlockActions` | City block layout: lot subdivision, street geometry, orchestration (4 actions) |
| `FMonolithMeshSpatialRegistry` | Spatial registry: hierarchical JSON descriptor, adjacency graph, BFS pathfinding (10 actions) |
| `FMonolithMeshAutoVolumeActions` | Auto-volume generation: NavMesh, blocking, audio, trigger volumes (3 actions) |
| `FMonolithMeshTerrainActions` | Terrain adaptation: height sampling, foundations, retaining walls (5 actions) |
| `FMonolithMeshArchFeatureActions` | Architectural features: balconies, porches, fire escapes, railings (5 actions) |
| `FMonolithMeshDebugViewActions` | Daredevil debug view: section clip, floor plan capture, camera bookmarks (6 actions) |
| `FMonolithMeshFurnishingActions` | Room furnishing: room-type furniture mapping, placement rules (3 actions) |
| `FMonolithMeshBuildingTypes` | Shared structs: FBuildingGrid, FRoomDef, FDoorDef, FStairwellDef, FBuildingDescriptor |
| `FMonolithMeshCatalog` | Mesh catalog database for search_meshes_by_size and get_mesh_catalog_stats |
| `FMonolithMeshUtils` | Shared helpers for mesh loading, bounds calculation, actor queries |

#### Actions (242 — namespace: "mesh")

> **Note:** 197 core actions (Phases 1-22 + Proc Geo Overhaul) always registered + 45 experimental Procedural Town Generator actions (SP1-SP10 + `validate_building`) registered only when `bEnableProceduralTownGen = true` (default: false). Town gen has known geometry issues (wall misalignment, room separation) — very much a work-in-progress. Unless you're willing to dig in and help improve it, it's best left alone for now. Fix Plans v2-v5 addressed 27+ issues but fundamental geometry problems remain.

**Inspection (12)**
| Action | Params | Description |
|--------|--------|-------------|
| `get_mesh_info` | `asset_path` | Full mesh info: vertex/triangle counts, bounds, LODs, materials, collision |
| `get_mesh_bounds` | `asset_path` | Bounding box dimensions and center |
| `get_mesh_materials` | `asset_path` | Material slot names and assigned materials |
| `get_mesh_lods` | `asset_path` | LOD details: vertex/triangle counts, screen sizes |
| `get_mesh_collision` | `asset_path` | Collision geometry: type, complexity, body count |
| `get_mesh_uvs` | `asset_path` | UV channel info: channel count, bounds per channel |
| `analyze_skeletal_mesh` | `asset_path` | Skeletal mesh analysis: bones, sockets, morph targets, physics bodies |
| `analyze_mesh_quality` | `asset_path` | Quality metrics: degenerate triangles, UV distortion, overdraw estimate |
| `compare_meshes` | `asset_path_a`, `asset_path_b` | Side-by-side comparison of two meshes |
| `get_vertex_data` | `asset_path`, `lod`, `section` | Raw vertex data for a mesh section |
| `search_meshes_by_size` | `min_size`, `max_size`, `limit` | Search indexed meshes by bounding box size range |
| `get_mesh_catalog_stats` | none | Mesh catalog database statistics |

**Scene Manipulation (8)**
| Action | Params | Description |
|--------|--------|-------------|
| `get_actor_info` | `actor_name` | Full actor details: class, transform, components, tags |
| `spawn_actor` | `class_name`, `location`, `rotation`, `label` | Spawn an actor in the current level |
| `move_actor` | `actor_name`, `location`, `rotation`, `scale` | Set actor transform |
| `duplicate_actor` | `actor_name`, `offset` | Duplicate an actor with optional offset |
| `delete_actors` | `actor_names` | Delete one or more actors by name |
| `group_actors` | `actor_names`, `group_name` | Group actors under a folder |
| `set_actor_properties` | `actor_name`, `properties` | Set properties on an actor via reflection |
| `batch_execute` | `operations` | Execute multiple scene operations in a single transaction |

**Spatial Queries (11)**
| Action | Params | Description |
|--------|--------|-------------|
| `query_raycast` | `start`, `end`, `channel` | Single-hit raycast with collision response |
| `query_multi_raycast` | `start`, `end`, `channel` | Multi-hit raycast returning all intersections |
| `query_radial_sweep` | `center`, `radius`, `channel` | Radial sphere sweep around a point |
| `query_overlap` | `location`, `extent`, `channel` | Box overlap test at location |
| `query_nearest` | `location`, `radius`, `class_filter` | Find nearest actor of a given class within radius |
| `query_line_of_sight` | `from`, `to`, `ignore_actors` | Line-of-sight check between two points |
| `get_actors_in_volume` | `volume_name` | Get all actors inside a named volume |
| `get_scene_bounds` | none | Get the total bounds of all actors in the level |
| `get_scene_statistics` | none | Scene stats: actor count, triangle count, draw calls, texture memory |
| `get_spatial_relationships` | `actor_name`, `radius` | Get nearby actors and their spatial relationships |
| `query_navmesh` | `start`, `end` | Query navigation mesh for path between two points |

**Level Blockout (15)**
| Action | Params | Description |
|--------|--------|-------------|
| `get_blockout_volumes` | none | List all blockout volumes in the level |
| `get_blockout_volume_info` | `volume_name` | Detailed info about a blockout volume |
| `setup_blockout_volume` | `location`, `extent`, `name`, `tags` | Create a blockout volume for level design |
| `create_blockout_primitive` | `type`, `location`, `scale`, `material` | Create a blockout primitive (box, cylinder, sphere, etc.) |
| `create_blockout_primitives_batch` | `primitives` | Batch-create multiple blockout primitives |
| `create_blockout_grid` | `origin`, `cell_size`, `rows`, `columns` | Create a grid of blockout primitives |
| `match_asset_to_blockout` | `blockout_actor`, `asset_path` | Match a production asset to replace a blockout primitive |
| `match_all_in_volume` | `volume_name`, `asset_mapping` | Match all blockout primitives in a volume to production assets |
| `apply_replacement` | `blockout_actor`, `asset_path` | Replace a blockout actor with a production mesh |
| `set_actor_tags` | `actor_name`, `tags` | Set tags on an actor for blockout categorization |
| `clear_blockout` | `volume_name` | Remove all blockout primitives in a volume |
| `export_blockout_layout` | `volume_name`, `save_path` | Export blockout layout to JSON |
| `import_blockout_layout` | `file_path` | Import a blockout layout from JSON |
| `scan_volume` | `volume_name` | Scan a volume and report contents |
| `scatter_props` | `volume_name`, `asset_paths`, `density`, `seed` | Scatter props randomly within a volume |

**Procedural Mesh Caching (4)** — Hash-based manifest at `Saved/Monolith/ProceduralCache/manifest.json`
| Action | Params | Description |
|--------|--------|-------------|
| `list_cached_meshes` | `type_filter`?, `limit`? (default 100) | List cached procedural mesh entries with asset_path, action, type, dimensions, triangle_count, created_utc |
| `clear_cache` | `type_filter`? | Clear cached meshes — all or filtered by type. Returns cleared_count |
| `validate_cache` | none | Remove stale cache entries where the asset no longer exists on disk. Returns removed_count |
| `get_cache_stats` | none | Cache statistics: total_entries and per-type breakdown |

**Blueprint Prefabs (1)** — Dialog-free blueprint creation from placed actors
| Action | Params | Description |
|--------|--------|-------------|
| `create_blueprint_prefab` | `*actor_names`, `*save_path`, `center_pivot`? (default true), `keep_source_actors`? (default true) | Create a Blueprint from selected actors via HarvestBlueprintFromActors. Returns blueprint_path, asset_name, source_actor_count, component_count |

> **Procedural Geometry Overhaul (2026-03-28):** The proc gen actions (`create_parametric_mesh`, `create_structure`, `create_horror_prop`, etc.) now feature sweep-based thin walls (`wall_mode: "sweep"` default), auto snap-to-floor (`snap_to_floor` param), auto-collision on all saved meshes (`collision: auto/box/convex/complex_as_simple/none`), human-scale defaults (stairs 90/28/18cm, doors 90cm, floor 3cm), door/window/vent trim frames (`add_trim` param), and vent openings via `create_structure`. Collision-aware prop placement uses `collision_mode: none/warn/reject/adjust` on scatter actions with SweepSingle box traces for floor finding. All proc gen actions support `use_cache` and `auto_save` params for the caching system.

#### Procedural Town Generator (45 gated + 1 always-registered actions — 11 sub-projects) — WORK-IN-PROGRESS

> **Status:** Work-in-progress, disabled by default (`bEnableProceduralTownGen = false`). Fix Plans v2-v5 addressed 27+ issues but fundamental geometry problems remain (wall misalignment, room separation). Very much a WIP — unless you're willing to dig in and help improve it, it's best left alone for now.

Procedural city block generation from a single MCP call. 11 sub-projects composing into a pipeline: grid-based buildings with connected rooms, roofs, facades, furniture, lighting, horror dressing, navmesh, and volumes, all adaptive to terrain. The critical interface is the **Building Descriptor** — a JSON contract that SP1 outputs and SP2-SP10 consume. All building specs are generated server-side (not sent over MCP wire).

**Master plan:** `Docs/plans/2026-03-28-proc-town-generator-master-plan.md`

**Building Descriptor Contract (Critical Interface)**

SP1's `create_building_from_grid` returns a JSON descriptor consumed by all downstream SPs. Key fields:
- `building_id`, `asset_path`, `actors[]` — building identity and spawned actors
- `footprint_polygon` — 2D building outline for roof generation
- `floors[]` — per-floor data: `rooms[]` (room_id, room_type, grid_cells, world_bounds), `doors[]` (connects, wall, width), `stairwells[]`
- `exterior_faces[]` — wall segments for facade decoration (normal, width, height, is_exterior)
- `grid_cell_size`, `wall_thickness`, `materials_assigned`, `tags_applied`

**SP1: Grid-Based Building Construction (2 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `create_building_from_grid` | `grid`, `rooms`, `doors`, `floors`, `cell_size`?, `materials`?, `omit_exterior_walls`? | Grid of room IDs → geometry + Building Descriptor. Auto-detects interior/exterior walls, generates floor/ceiling slabs, stairwell cutouts, trim frames, actor tags. `omit_exterior_walls` (default false) skips exterior wall generation for facade-only workflows |
| `create_grid_from_rooms` | `rooms`, `adjacency` | Room list + adjacency requirements → grid layout |

**SP2: Automatic Floor Plan Generation (3 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `generate_floor_plan` | `archetype`, `width`, `depth`, `floors`?, `hospice_mode`? | Building archetype + footprint → grid + rooms + doors. Squarified treemap with per-floor room assignment, aspect ratio enforcement, footprint boundary validation, and guaranteed exterior entrance on ground floor. Corridor width min 120cm, door width min 90cm. Accessibility mode (`hospice_mode`): 100cm doors, 180cm corridors, rest alcoves |
| `list_building_archetypes` | none | List available archetype definitions (residential, clinic, police_station, apartment, etc.) |
| `get_building_archetype` | `archetype` | Get archetype JSON: room types, sizes, adjacency requirements |

**SP3: Facade & Window Generation (3 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `generate_facade` | `building_descriptor`, `style`?, `damage`? | Exterior walls → windows, doors, trim, cornices, storefronts. CGA-style vertical split (base/shaft/cap). Optional horror damage |
| `list_facade_styles` | none | List available facade style presets (Victorian, Colonial, Brutalist, Abandoned) |
| `apply_horror_damage` | `building_descriptor`, `decay` | Apply horror damage to facades: boarded windows, broken glass, rust stains |

**SP4: Roof Generation (1 action)**
| Action | Params | Description |
|--------|--------|-------------|
| `generate_roof` | `building_descriptor`, `roof_type`?, `overhang`? | Footprint polygon → roof geometry (gable, hip, flat/parapet, shed, gambrel). Separate MaterialID for roof surface |

**SP5: City Block Layout (4 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `create_city_block` | `buildings`, `genre`?, `seed`?, `block_size`?, `decay`? | Top-level orchestrator. Subdivides block → generates buildings → facades → roofs → streets → horror decay. Graceful degradation if SPs unavailable |
| `create_lot_layout` | `block_size`, `lot_count`?, `seed`? | Subdivide block into lots (OBB recursive), return lot positions and footprint shapes |
| `create_street` | `block_bounds`, `lot_positions` | Generate street geometry: sidewalks, curbs, road surface |
| `place_street_furniture` | `street_bounds`, `density`?, `seed`? | Place lamps, hydrants, benches, trash cans along streets |

**SP6: Spatial Registry (10 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `register_building` | `building_descriptor` | Register a building in the spatial registry |
| `register_room` | `building_id`, `room` | Register an individual room |
| `register_street_furniture` | `block_id`, `actors` | Register street furniture actors |
| `query_room_at` | `position` | Query what room is at a world position |
| `query_adjacent_rooms` | `room_id` | Query rooms adjacent to a given room |
| `query_rooms_by_filter` | `filter` | Query rooms by type, floor, building, or tags |
| `query_building_exits` | `building_id` | Query all exit points from a building |
| `path_between_rooms` | `from_room`, `to_room` | BFS pathfinding between two rooms through the adjacency graph |
| `save_block_descriptor` | `block_id`, `save_path`? | Persist block descriptor to JSON |
| `load_block_descriptor` | `file_path` | Load a persisted block descriptor |

**SP7: Auto-Volume Generation (3 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `auto_volumes_for_building` | `building_descriptor` | Auto-spawn NavMeshBounds, BlockingVolume, AudioVolume (reverb by room size), TriggerVolume for a building |
| `auto_volumes_for_block` | `block_id` | Auto-volumes for all buildings in a block + navmesh build |
| `spawn_nav_link` | `location`, `left_point`, `right_point` | Spawn a NavLinkProxy between two points |

**SP8a: Terrain + Foundations (5 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `sample_terrain_grid` | `origin`, `extent`, `resolution` | Sample NxM height grid via downward traces |
| `analyze_building_site` | `footprint`, `terrain_grid` | Analyze site slope and recommend foundation strategy (Flat/CutAndFill/Stepped/Piers/WalkoutBasement) |
| `create_foundation` | `building_descriptor`, `strategy`?, `hospice_mode`? | Generate foundation geometry. ADA-compliant ramps when `hospice_mode` is enabled (1:12 slope, 76cm max rise, 150cm landings) |
| `create_retaining_wall` | `path`, `height`, `material`? | Generate retaining wall geometry along a path |
| `place_building_on_terrain` | `building_descriptor`, `terrain_grid` | Adapt a building to uneven terrain with auto-selected foundation |

**SP8b: Architectural Features (5 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `create_balcony` | `building_descriptor`, `floor`, `face`, `width`?, `depth`?, `building_context`? | Floor slab + railing extending from upper floor exterior. `building_context` enables collision checks against existing geometry. Returns `wall_openings` for facade integration |
| `create_porch` | `building_descriptor`, `face`, `depth`?, `columns`?, `building_context`? | Ground-level covered entry with columns. `building_context` enables collision checks. Returns `wall_openings` for facade integration |
| `create_fire_escape` | `building_descriptor`, `face`, `floors`?, `building_context`? | Zigzag exterior stairs between floor landings (45-degree angle). `building_context` enables collision checks. Returns `wall_openings` for facade integration |
| `create_ramp_connector` | `start`, `end`, `width`?, `slope`?, `building_context`? | ADA-compliant ramp between two heights with switchback support. Returns `wall_openings` for facade integration |
| `create_railing` | `path`, `height`?, `style`? | Swept profile railing along edge path |

**SP9: Daredevil Debug View (6 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `toggle_section_view` | `z_height`?, `enabled`? | MPC-based section clip — hide everything above a Z height |
| `toggle_ceiling_visibility` | `visible`?, `floor`? | Toggle ceiling/roof visibility via actor tags (BuildingCeiling, BuildingRoof) |
| `capture_floor_plan` | `building_descriptor`, `floor`?, `output_path`? | Orthographic top-down floor plan capture to PNG |
| `highlight_room` | `room_id`, `color`?, `duration`? | Room highlighting with overlay materials |
| `save_camera_bookmark` | `name`, `location`?, `rotation`? | Save current or specified camera viewpoint |
| `load_camera_bookmark` | `name` | Restore a saved camera viewpoint |

**SP10: Room Furnishing Pipeline (3 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `furnish_room` | `building_descriptor`, `room_id`, `preset`?, `disturbance`? | Place appropriate furniture per room type. Horror dressing via optional disturbance level (orderly/slightly_messy/ransacked/abandoned) |
| `furnish_building` | `building_descriptor`, `decay`? | Furnish all rooms in a building, applying horror decay per room |
| `list_furniture_presets` | `room_type`? | List available furniture preset configurations per room type |

**Validation (1 action)**
| Action | Params | Description |
|--------|--------|-------------|
| `validate_building` | `building_descriptor` | Post-generation validation: checks room connectivity, door reachability, stair angle limits, wall thickness, exterior entrance existence, floor slab coverage. Returns `valid` bool + `issues[]` with severity, location, and description per problem found |

#### Fix Plan v2 Changes (2026-03-28)

20 issues fixed across 3 phases targeting building generation correctness and playability:

**Phase 1 — Geometry Fixes:**
1. Building stairs angle reduced from 70 degrees to 32 degrees (standard residential)
2. Building stairs switchback support for multi-story buildings
3. Fire escape angle reduced from 66 degrees to 45 degrees
4. Ramp connector switchback self-intersection fix
5. Exterior wall omission via `omit_exterior_walls` param on `create_building_from_grid`
6. Wall thickness validation (minimum 10cm, maximum 60cm)

**Phase 2 — Floor Plan Fixes:**
7. Corridor minimum width enforced at 120cm
8. Door minimum width enforced at 90cm
9. Per-floor room assignment in `generate_floor_plan` (bedrooms upstairs, living areas ground floor)
10. Room aspect ratio enforcement (no rooms narrower than 1:4)
11. Footprint boundary validation (rooms cannot exceed building footprint)
12. Guaranteed exterior entrance on ground floor

**Phase 3 — Integration Fixes:**
13. `building_context` param on architectural feature actions (balcony, porch, fire escape, ramp)
14. `wall_openings` output on architectural feature actions for facade coordination
15. Stairwell ceiling cutout geometry correctness
16. Floor slab coverage validation (no gaps between rooms)
17. Room connectivity validation (all rooms reachable)
18. Door placement validation (doors on shared walls only)
19. `validate_building` action added for post-generation integrity checks
20. Graceful error reporting with per-issue severity levels

**Data Files (Procedural Town Generator)**
| Directory | Sub-Project | Contents |
|-----------|------------|----------|
| `Saved/Monolith/BuildingArchetypes/` | SP2 | JSON room catalogs per building type (residential_house, clinic, police_station, apartment) |
| `Saved/Monolith/FacadeStyles/` | SP3 | JSON facade presets (Victorian, Colonial, Brutalist, Abandoned) |
| `Saved/Monolith/BlockPresets/` | SP5 | JSON block configuration presets |
| `Saved/Monolith/SpatialRegistry/` | SP6 | Persisted block descriptors |
| `Saved/Monolith/CameraBookmarks/` | SP9 | Saved camera positions |
| `Saved/Monolith/FurniturePresets/` | SP10 | Room-type furniture configs per room type (kitchen, bedroom, bathroom, office, lobby, corridor) |

---

### 3.12 MonolithBABridge

**Dependencies:** Core, CoreUObject, Engine, MonolithCore (optional — loads only when both Monolith and Blueprint Assist are present)

MonolithBABridge is an **optional** editor module that bridges Blueprint Assist's graph formatter into Monolith's `auto_layout` actions. It registers no MCP actions of its own. Its sole job is to expose BA's layout logic via `IModularFeatures` so that blueprint, material, animation, and niagara modules can consume it without a hard dependency on Blueprint Assist.

#### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithBABridgeModule` | IModuleInterface. On startup, checks for Blueprint Assist via `FModuleManager::IsModuleLoaded("BlueprintAssist")` and registers `IMonolithGraphFormatter` impl via `IModularFeatures::Get().RegisterFeature()` |
| `FMonolithBAGraphFormatter` | Concrete `IMonolithGraphFormatter` impl. Delegates to BA's `FBAFormatterUtils` / `FBANodePositioner`. Reads cached node sizes from `FBACache` when available |

#### IMonolithGraphFormatter Interface

```cpp
class IMonolithGraphFormatter
{
public:
    virtual ~IMonolithGraphFormatter() = default;

    /** Feature name used with IModularFeatures */
    static const FName FeatureName;

    /**
     * Format a graph using the registered formatter.
     * @param Graph  Target graph to layout
     * @return true if layout was applied
     */
    virtual bool FormatGraph(UEdGraph* Graph) = 0;
};
```

Consumer pattern used by `auto_layout` actions in each domain module:

```cpp
// Check at call time — BA may not be loaded
if (IModularFeatures::Get().IsFeatureAvailable(IMonolithGraphFormatter::FeatureName))
{
    auto& Formatter = IModularFeatures::Get().GetFeature<IMonolithGraphFormatter>(
        IMonolithGraphFormatter::FeatureName);
    Formatter.FormatGraph(Graph);
}
```

#### `formatter` Parameter (three-mode behavior)

All four `auto_layout` actions accept an optional `formatter` param:

| Value | Behavior |
|-------|----------|
| `"auto"` (default) | Uses Blueprint Assist formatter if `IMonolithGraphFormatter` is registered; otherwise falls back to built-in hierarchical layout. Never errors |
| `"blueprint_assist"` | Forces BA formatter. Returns an error if MonolithBABridge is not loaded or BA is not present |
| `"builtin"` | Forces built-in layout regardless of BA presence |

#### `bEnableBlueprintAssist` Setting

`UMonolithSettings` exposes a toggle that controls whether MonolithBABridge attempts registration on startup:

| Setting | Default | Description |
|---------|---------|-------------|
| `bEnableBlueprintAssist` | True | When false, MonolithBABridge skips `IModularFeatures` registration even if Blueprint Assist is present. `formatter: "auto"` will fall back to built-in; `formatter: "blueprint_assist"` will error |

**Config key:** `bEnableBlueprintAssist` in `[/Script/MonolithCore.MonolithSettings]`

### 3.13 MonolithGAS

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, GameplayAbilities, GameplayTags
**Namespace:** `gas` | **Tool:** `gas_query(action, params)` | **Actions:** 130
**Conditional:** GBA (Blueprint Attributes) features wrapped in `#if WITH_GBA`. Core GAS engine modules (GameplayAbilities, GameplayTags, GameplayTasks) are always available. When GBA is absent, Blueprint AttributeSet creation is disabled but all 130 actions still register and compile cleanly. When `bEnableGAS` is disabled in settings, 0 actions registered.
**Settings toggle:** `bEnableGAS` (default: True)

MonolithGAS provides full MCP coverage of the Gameplay Ability System. It covers ability CRUD, attribute set management, gameplay effect authoring, ASC (Ability System Component) inspection and manipulation, gameplay tag operations, gameplay cue management, target data, input binding, runtime inspection, and scaffolding of common GAS patterns.

#### Action Categories

| Category | Actions | Description |
|----------|---------|-------------|
| Abilities | 28 | Create, edit, delete, list, grant, activate, cancel, query gameplay abilities. Includes spec handles, instancing policy, tags, costs, cooldowns |
| Attributes | 20 | Create/edit attribute sets, get/set attribute values, define derived attributes, attribute initialization, clamping, replication config |
| Effects | 26 | Create/edit gameplay effects, duration policies, modifiers, executions, stacking, conditional application, period, tags granted/removed |
| ASC | 14 | Inspect/configure Ability System Components, list granted abilities, active effects, attribute values, owned tags, replication mode |
| Tags | 10 | Query gameplay tag hierarchy, check tag matches, add/remove loose tags, tag containers, tag queries |
| Cues | 10 | Create/edit gameplay cue notifies (static and actor), cue tags, cue parameters, handler lookup |
| Targets | 5 | Target data handles, target actor selection, target data confirmation, custom target data types |
| Input | 5 | Bind abilities to Enhanced Input actions, input tag mapping, activation on input |
| Inspect | 6 | Runtime inspection of active abilities, applied effects, attribute snapshots, ability task state, prediction keys |
| Scaffold | 6 | Scaffold common GAS setups: init_attribute_set, init_asc_actor, init_ability_set, init_damage_pipeline, init_cooldown_system, init_stacking_effect |

#### Notes

> **Runtime actions (Inspect category) require PIE.** These actions query live game state and return errors if called outside a Play-In-Editor session.
>
> **GBA conditional support:** The `WITH_GBA` define is set automatically by the module's `Build.cs` when GameplayAbilities is found. Projects without GAS get zero compile overhead — the entire module compiles to an empty stub.

---

## 4. Source Indexer

### 4.1 C++ Indexer (current)

The engine source indexer is a native C++ implementation within `MonolithSource`. `UMonolithSourceSubsystem` builds and maintains `EngineSource.db` in-process. Indexing is triggered via:

- **`trigger_reindex`** — full engine source re-index
- **`trigger_project_reindex`** — incremental project-only C++ re-index (faster; only updates project symbols)

### 4.2 Python Source Indexer (legacy)

> **LEGACY:** The Python tree-sitter indexer in `Scripts/source_indexer/` has been superseded by the native C++ indexer. It is no longer invoked by MonolithSource and is retained only for reference.

**Location:** `Scripts/source_indexer/`
**Entry point:** `python -m source_indexer --source PATH --db PATH [--shaders PATH]`
**Dependencies:** tree-sitter>=0.21.0, tree-sitter-cpp>=0.21.0, Python 3.10+

#### Pipeline (IndexingPipeline)

1. **Module Discovery** — Walks Runtime, Editor, Developer, Programs under Engine/Source + Engine/Plugins. Optionally Engine/Shaders
2. **File Processing** — C++ files -> CppParser (tree-sitter AST) -> symbols, includes. Shader files -> ShaderParser (regex) -> symbols, includes
3. **Source Line FTS** — Chunks source in batches of 10 lines into source_fts table
4. **Finalization** — Resolves inheritance, runs ReferenceBuilder for call/type cross-references

#### Parsers

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

## 5. Offline CLI

Two options for offline access (no full editor session required):

### 5.1 MonolithQueryCommandlet (preferred)

**Class:** `UMonolithQueryCommandlet`
**Run via:**
```
"C:\Program Files (x86)\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" YourProject -run=MonolithQuery [args...]
```

Replaces `monolith_offline.py` as the primary offline access path. Uses the same C++ DB layer as the live MCP server, so query results are identical.

### 5.2 monolith_offline.py (legacy)

> **LEGACY:** `monolith_offline.py` is superseded by `MonolithQueryCommandlet`. It remains functional as a zero-dependency fallback requiring only Python stdlib and no UE installation.

**Location:** `Saved/monolith_offline.py`
**Dependencies:** Python stdlib only (sqlite3, argparse, json, re, pathlib) — no pip installs required
**Python version:** 3.8+

A companion CLI that queries `EngineSource.db` and `ProjectIndex.db` directly without the Unreal Editor running. Intended as a fallback when MCP is unavailable (editor down, CI environments, quick terminal lookups).

**Scope:** Read/query operations only. Write operations require the editor and MCP.

### Usage

```
python Saved/monolith_offline.py <namespace> <action> [args...]
```

### Namespaces and Actions

**Source (9 actions)** — mirrors `source_query` MCP tool:

| Action | Positional | Key Options | Description |
|--------|-----------|-------------|-------------|
| `search_source` | `query` | `--limit`, `--module`, `--kind` | FTS across symbols + source lines, BM25 ranked |
| `read_source` | `symbol` | `--max-lines`, `--members-only`, `--no-header` | Source for a class/function/struct; FTS fallback on no exact match |
| `find_references` | `symbol` | `--ref-kind`, `--limit` | All usage sites |
| `find_callers` | `symbol` | `--limit` | Functions that call the given function |
| `find_callees` | `symbol` | `--limit` | Functions called by the given function |
| `get_class_hierarchy` | `symbol` | `--direction up\|down\|both`, `--depth` | Inheritance tree traversal |
| `get_module_info` | `module_name` | — | File count, symbol counts by kind, key classes |
| `get_symbol_context` | `symbol` | `--context-lines` | Definition with surrounding context |
| `read_file` | `file_path` | `--start`, `--end` | Read source lines; resolves via absolute path → DB exact → DB suffix match |

**Project (5 actions)** — mirrors `project_query` MCP tool:

| Action | Positional | Key Options | Description |
|--------|-----------|-------------|-------------|
| `search` | `query` | `--limit` | FTS across assets FTS + nodes FTS, BM25 ranked |
| `find_by_type` | `asset_class` | `--limit`, `--offset` | Filter assets by class with pagination |
| `find_references` | `asset_path` | — | Bidirectional: depends_on + referenced_by |
| `get_stats` | — | — | Row counts for all tables + top 20 asset class breakdown |
| `get_asset_details` | `asset_path` | — | Nodes, variables, parameters for one asset |

### Implementation Notes

- Opens DBs with `PRAGMA query_only=ON` + `PRAGMA journal_mode=DELETE`. The DELETE journal mode override is mandatory — WAL mode silently returns 0 rows on Windows when opened in any read-only mode (same bug that affected the C++ module; see CLAUDE.md Key Lessons).
- FTS escaping mirrors `EscapeFTS()` in C++: `::` replaced with space, non-word chars stripped, each token wrapped as `"token"*` for prefix match.
- `read_source` defaults to `--header` (includes `.h` declarations). Pass `--no-header` to skip header files.
- `read_file` with `--end 0` (default) reads 200 lines from `--start`.
- Source output is plain text. Project output is JSON.

---

## 6. Skills (10 bundled)

| Skill | Trigger Words | Entry Point | Actions |
|-------|--------------|-------------|---------|
| unreal-animation | animation, montage, ABP, blend space, notify, curves, compression, PoseSearch | `animation_query()` | 115 |
| unreal-blueprints | Blueprint, BP, event graph, node, variable | `blueprint_query()` | 86 |
| unreal-build | build, compile, Live Coding, hot reload, rebuild | `editor_query()` | 19 |
| unreal-cpp | C++, header, include, UCLASS, Build.cs, linker error | `source_query()` + `config_query()` | 11+6 |
| unreal-debugging | build error, crash, log, debug, stack trace | `editor_query()` | 19 |
| unreal-materials | material, shader, PBR, texture, material graph | `material_query()` | 57 |
| unreal-niagara | Niagara, particle, VFX, emitter | `niagara_query()` | 96 |
| unreal-performance | performance, optimization, FPS, frame time | Cross-domain | config + material + niagara |
| unreal-project-search | find asset, search project, dependencies | `project_query()` | 7 |
| unreal-ui | UI, HUD, widget, menu, settings, save game, accessibility, font, toast, dialog | `ui_query()` | 42 |

All skills follow a common structure: YAML frontmatter, Discovery section, Asset Path Conventions table, action tables, workflow examples, and rules.

---

## 7. Configuration

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
| bUIEnabled | True | Enable UI module |
| bMeshEnabled | True | Enable Mesh module (core actions) |
| bGASEnabled | True | Enable GAS module (requires GameplayAbilities plugin; no-op if `WITH_GBA=0`) |
| bEnableProceduralTownGen | **False** | Enable Procedural Town Generator actions (45 actions). Requires `bMeshEnabled`. **Work-in-progress** — known geometry issues, disabled by default. Unless you're willing to dig in and help improve it, best left alone |
| bEnableBlueprintAssist | True | Allow MonolithBABridge to register IMonolithGraphFormatter when Blueprint Assist is present. Set false to force built-in layout for all auto_layout calls |
| LogVerbosity | 3 (Log) | 0=Silent, 1=Error, 2=Warning, 3=Log, 4=Verbose |

**Note:** Module enable toggles are functional — each module checks its toggle at registration time and skips action registration if disabled.

---

## 8. Templates

| File | Purpose |
|------|---------|
| `Templates/.mcp.json.example` | Minimal MCP config. Transport type varies by client: `"http"` for Claude Code, `"streamableHttp"` for Cursor/Cline. URL: `http://localhost:9316/mcp` |
| `Templates/CLAUDE.md.example` | Project instructions template with tool reference, workflow, asset path conventions, and rules |

---

## 9. File Structure

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
    unreal-build/unreal-build.md
    unreal-cpp/unreal-cpp.md
    unreal-debugging/unreal-debugging.md
    unreal-materials/unreal-materials.md
    unreal-niagara/unreal-niagara.md
    unreal-performance/unreal-performance.md
    unreal-project-search/unreal-project-search.md
    unreal-ui/unreal-ui.md
  Templates/
    .mcp.json.example
    CLAUDE.md.example
  Scripts/
    source_indexer/                (LEGACY: Python tree-sitter indexer — superseded by C++ indexer in MonolithSource)
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
    MonolithUI/                    (17 source files — 9 .cpp + 8 .h)
    MonolithGAS/                   (conditional on WITH_GBA — abilities, attributes, effects, ASC, tags, cues, targets, input, inspect, scaffold)
  Saved/
    .gitkeep
    monolith_offline.py              (Offline CLI — query DBs without the editor)
    EngineSource.db                  (Engine source index, ~1.8GB — not in git)
    ProjectIndex.db                  (Project asset index — not in git)
```

---

## 10. Deployment

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

## 11. Known Issues & Workarounds

See `TODO.md` for the full list. Key architectural constraints:

- **6 reimplemented NiagaraEditor helpers** — NiagaraEditor APIs not exported by Epic; Monolith reimplements them locally
- **SSE is stub-only** — GET endpoint returns single event and close, not full streaming
- **MaterialExpressionNoise fails on Lumen card passes** — Compiles for base pass but errors on Lumen card capture shaders ("function signature unavailable"). Engine limitation, not a Monolith bug. Workaround: use custom HLSL noise or pre-baked noise textures instead.
- **MaterialExpressionRadialGradientExponential does not exist** — Despite appearing in some community references, this expression class is not in UE 5.7. Use a Custom HLSL node with `pow(1.0 - saturate(length(UV - 0.5) * 2.0), Exponent)` instead.

---

## 12. Action Count Summary

| Module | Namespace | Actions |
|--------|-----------|---------|
| MonolithCore | monolith | 4 |
| MonolithBlueprint | blueprint | 86 |
| MonolithMaterial | material | 57 |
| MonolithAnimation | animation | 115 |
| MonolithNiagara | niagara | 96 |
| MonolithMesh | mesh | 242 (197 core + 45 experimental town gen) |
| MonolithEditor | editor | 19 |
| MonolithConfig | config | 6 |
| MonolithIndex | project | 7 |
| MonolithSource | source | 11 |
| MonolithUI | ui | 42 |
| MonolithGAS | gas | 130 |
| MonolithBABridge | — | 0 (integration only) |
| **Total** | | **815** (770 active by default) |

**Note:** MonolithMesh includes 197 core actions (always registered) plus 45 experimental Procedural Town Generator actions (registered only when `bEnableProceduralTownGen = true`, default: false — known geometry issues). MonolithGAS is conditional on `#if WITH_GBA` — projects without GameplayAbilities register 0 GAS actions. MonolithBABridge registers no MCP actions — it only provides the `IMonolithGraphFormatter` IModularFeatures bridge consumed by `auto_layout` in the blueprint, material, animation, and niagara modules. The original Python server had higher tool counts (~231 tools) due to fragmented action design — Monolith consolidates these into 15 MCP tools with namespaced actions.
