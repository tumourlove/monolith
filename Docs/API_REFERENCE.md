# Monolith API Reference

**Version:** v0.14.7 (+ [Unreleased] editor +2) · **Last updated:** 2026-04-30

**In-tree action total: 1271** registered across **16 in-tree namespaces** (all 1271 active by default; 45 town-gen actions are experimental and disabled until you flip `bEnableProceduralTownGen=true`, which lifts the registry to 1316). The `ui` namespace re-exports 4 GAS UI binding actions as aliases, so the count of **distinct handlers is 1267**. The four `monolith_*` meta-tools (`discover`, `status`, `update`, `reindex`) live in their own namespace and bring the dispatcher count to 20.

Live editor introspection on a fully loaded project (with sibling plugins present) can report additional namespaces beyond the in-tree Monolith surface. Those actions ship in their owning sibling repositories and are documented separately — see [§Sibling Plugins](#sibling-plugins).

> Auto-generated and hand-curated. Each action is dispatched via HTTP POST to `http://localhost:<port>` with JSON body `{ "namespace": "<ns>", "action": "<action>", "params": { ... } }`, or via the MCP `tools/list` surface that AI clients see at session start.
>
> For the most current param schemas, call `monolith_discover("<namespace>")` at runtime — it returns live schemas straight out of the plugin. This document is a curated reference, not a source-of-truth substitute.

---

## Table of Contents

| Namespace | Actions | Description |
|-----------|---------|-------------|
| [monolith](#monolith) | 4 | Core server tools (discover, status, update, reindex) |
| [blueprint](#blueprint) | 89 | Blueprint read/write, variable/component/graph CRUD, node ops, compile, auto-layout, spawn actors |
| [material](#material) | 63 | Material graph editing, inspection, CRUD, material functions, PBR pipeline |
| [animation](#animation) | 118 | Curves, bone tracks, sync markers, root motion, compression, blend spaces, ABPs, montages, skeletons, PoseSearch, IKRig, Control Rig |
| [niagara](#niagara) | 109 | Niagara VFX (emitters, modules, params, renderers, HLSL, dynamic inputs, event handlers, sim stages, NPC, effect types) |
| [editor](#editor) | 24 | Live Coding builds, compile output capture, editor logs, scene capture, texture import, map creation, module status, automation test list/run |
| [config](#config) | 6 | INI config inspection and search |
| [project](#project) | 7 | Project-wide asset index (SQLite + FTS5) |
| [source](#source) | 11 | Unreal Engine C++ source code navigation |
| [mesh](#mesh) | 239 | Mesh inspection, scene manipulation, spatial queries, blockout, GeometryScript, procedural geo, lighting, audio, performance, town gen (experimental — +45 town gen registers only with `bEnableProceduralTownGen=true`) |
| [ui](#ui) | 121 | UMG widget CRUD, templates, styling, animation v1+v2, EffectSurface, Spec Builder, Type Registry, settings scaffolding, accessibility, CommonUI, GAS UI bindings |
| [gas](#gas) | 135 | Gameplay Ability System: abilities, attributes, effects, ASC, tags, cues, targeting, input, inspect, scaffold |
| [combograph](#combograph) | 13 | ComboGraph melee combo authoring (conditional on `WITH_COMBOGRAPH`) |
| [ai](#ai) | 221 | Behavior Trees, State Trees, EQS, Blackboards, AI Controllers, Perception, Smart Objects, Navigation, Mass, Zone Graph, runtime PIE inspection, scaffolds |
| [logicdriver](#logicdriver) | 66 | Logic Driver Pro state machines: graph CRUD, runtime PIE control, scaffolds, dialogue (conditional on `WITH_LOGICDRIVER`) |
| [audio](#audio) | 86 | Sound Cue + MetaSound graph CRUD, attenuation/class/mix/submix/concurrency, batch ops, Sound Cue templates, perception bindings |
| **In-tree subtotal** | **1271** | (all default-active; +45 experimental town gen → 1316 when registered) |
| [Sibling plugins](#sibling-plugins) | varies | Separate plugins, separate distribution |

---

## Recent API Changes (v0.14.0 → v0.14.7)

The Phase J retrofit cycle added five new actions and tightened param validation on several others. If you wrote integration code against v0.13.x or earlier, scan this list before upgrading.

| Action | Change | Reason |
|--------|--------|--------|
| `editor.create_empty_map` | **NEW** (Phase J F8) | Test scaffolding needed a blank UWorld factory that doesn't depend on engine templates. |
| `editor.get_module_status` | **NEW** (Phase J F8) | Lets clients query plugin/module load state without grepping logs. Wraps `IPluginManager` + `FModuleManager`. |
| `gas.grant_ability_to_pawn` | **NEW** (Phase J F8) | Convenience action for runtime ability grants. Earlier you had to grant via `apply_effect` or scaffold-side wiring. |
| `ai.add_perception_to_actor` | **NEW** (Phase J F8) | Direct perception attach without going through `add_perception_component` + manual wiring. |
| `ai.get_bt_graph` | **NEW** (Phase J F8) | Read-only graph dump distinct from `get_behavior_tree`'s structural inspection. |
| `audio.create_test_wave` | **NEW** (Phase J F18) | Procedurally synthesizes a 16-bit mono sine `USoundWave` for tests with zero asset deps. |
| `audio.bind_sound_to_perception` | Param validation tightened (Phase J F11) | `loudness <= 0`, `max_range < 0`, and unknown `sense_class` values now reject up-front instead of writing junk userdata. |
| `gas.bind_widget_to_attribute` (and 3 aliases) | Param validation tightened (Phase J F2/F3) | Empty `widget_path`, missing `attribute`, or unresolvable ASC now return structured errors before any reflection writes. |
| `ai` BT actions | Error message standardization (Phase J F15) | All BT-related actions now return `{ "error": "<code>", "detail": "<human>" }` instead of mixed prose. |
| `gas` UI binding response | Shape change (Phase J F5) | Returns `{ bindings: [...], count: N }` instead of a bare array. Wrap your client parsers. |

The aliased GAS UI binding actions live in **both** `ui::*` and `gas::*` namespaces — same handler, two callable paths. Pick whichever reads better from your client.

---

## monolith

Core server management and introspection.

### `monolith.discover`

List available tool namespaces and their actions. Pass `namespace` to filter; pass `category` to narrow further (e.g. `"CommonUI"` inside `ui`).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `namespace` | string | optional | Filter to a specific namespace |
| `category` | string | optional | Filter actions within the namespace by category |

**Returns:** Per-action param schemas for every registered action. AI clients also receive these in `tools/list` at session start, so most callers never need to call `discover` explicitly.

---

### `monolith.status`

Get Monolith server health: version, uptime, port, registered action count, namespace count, engine version, project name, module load status.

*No parameters.*

---

### `monolith.update`

Check for or install Monolith updates from GitHub Releases. Auto-updater hits `https://api.github.com/repos/tumourlove/monolith/releases/latest`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `action` | string | optional | `"check"` to compare versions, `"install"` to download and stage. Default: `"check"` |

---

### `monolith.reindex`

Re-index the Monolith project database. Incremental by default (delta only). Pass `force=true` for a full wipe + rebuild.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `force` | bool | optional | Full wipe + rebuild instead of incremental delta. Default: `false` |

---

## blueprint

Full read/write access to Blueprint graphs, variables, components, functions, nodes, pins, interfaces, timelines, comments, CDOs, and spawn-time actor placement. **89 actions.**

> For full param schemas, call `monolith_discover("blueprint")` at runtime. The action surface is too broad to enumerate here without bloat — high-traffic actions are documented below; the rest are listed and discoverable.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| Graph inspection | 9 | `list_graphs`, `get_graph_data`, `get_graph_summary`, `get_execution_flow`, `search_nodes`, `get_node_details` |
| Variables | 9 | `get_variables`, `add_variable`, `remove_variable`, `rename_variable`, `set_variable_type`, `set_variable_defaults`, `add_local_variable`, `remove_local_variable`, `add_replicated_variable` |
| Components | 7 | `get_components`, `get_component_details`, `add_component`, `remove_component`, `rename_component`, `reparent_component`, `set_component_property`, `duplicate_component` |
| Functions / Macros / Events | 12 | `get_functions`, `add_function`, `remove_function`, `rename_function`, `add_macro`, `remove_macro`, `rename_macro`, `add_event_dispatcher`, `remove_event_dispatcher`, `set_function_params`, `set_event_dispatcher_params`, `get_function_signature` |
| Interfaces | 4 | `implement_interface`, `remove_interface`, `get_interfaces`, `scaffold_interface_implementation` |
| Node ops | 11 | `add_node`, `remove_node`, `connect_pins`, `disconnect_pins`, `set_pin_default`, `set_node_position`, `resolve_node`, `add_event_node`, `add_comment_node`, `promote_pin_to_variable`, `add_nodes_bulk` |
| Bulk / batch | 4 | `batch_execute`, `add_nodes_bulk`, `connect_pins_bulk`, `set_pin_defaults_bulk` |
| Timelines | 4 | `add_timeline`, `add_timeline_track`, `set_timeline_keys`, `get_timeline_data` |
| Compile / validate | 3 | `compile_blueprint`, `validate_blueprint`, `get_dependencies` |
| Asset CRUD | 8 | `create_blueprint`, `duplicate_blueprint`, `save_asset`, `create_user_defined_struct`, `create_user_defined_enum`, `create_data_table`, `add_data_table_row`, `get_data_table_rows`, `create_data_asset` |
| CDO | 2 | `get_cdo_properties`, `set_cdo_property` |
| Templates / spec | 4 | `build_blueprint_from_spec`, `apply_template`, `list_templates`, `compare_blueprints` |
| Layout | 2 | `auto_layout`, `export_graph` |
| Spawn | 2 | `spawn_blueprint_actor`, `batch_spawn_blueprint_actors` |

**Header set most callers reach for first:**

### `blueprint.list_graphs`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |

Returns array of graphs with name, type (`event_graph` / `function` / `macro` / `delegate_signature`), and node count.

### `blueprint.get_graph_summary`

Lightweight overview with node id/class/title and exec connections only. ~10 KB vs ~172 KB for `get_graph_data` on complex graphs.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path |
| `graph_name` | string | optional | Defaults to first UbergraphPage |

### `blueprint.build_blueprint_from_spec`

The crown jewel — author an entire Blueprint (parent class, variables, components, functions, event graph nodes, connections) from a single JSON spec. Validates and compiles in one call. See `monolith_discover("blueprint")` for the full spec schema.

### `blueprint.spawn_blueprint_actor`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `blueprint_path` | string | **required** | Package path of the Blueprint to spawn |
| `location` | array | optional | `[x, y, z]` world location |
| `rotation` | array | optional | `[pitch, yaw, roll]` |
| `scale` | array | optional | `[x, y, z]` |
| `folder_path` | string | optional | Outliner folder. **Recommended** — all spawned actors should set a folder path |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithBlueprint.md` for the deep dive.

---

## material

Material graph editing, inspection, CRUD, material functions, instances, custom HLSL nodes, PBR pipeline. **63 actions.**

> For full param schemas, call `monolith_discover("material")` at runtime.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| Graph inspection | 7 | `get_all_expressions`, `get_expression_details`, `get_full_connection_graph`, `get_expression_pin_info`, `get_expression_connections`, `list_expression_classes`, `get_compilation_stats` |
| Graph CRUD | 12 | `build_material_graph`, `connect_expressions`, `disconnect_expression`, `delete_expression`, `delete_expressions`, `clear_graph`, `move_expression`, `duplicate_expression`, `replace_expression`, `rename_expression`, `set_expression_property`, `auto_layout` |
| Material assets | 7 | `create_material`, `create_material_instance`, `duplicate_material`, `save_material`, `set_material_property`, `get_material_properties`, `recompile_material` |
| Material instances | 6 | `get_material_parameters`, `get_instance_parameters`, `set_instance_parameter`, `set_instance_parameters`, `set_instance_parent`, `clear_instance_parameter`, `list_material_instances` |
| Material functions | 12 | `create_material_function`, `build_function_graph`, `get_function_info`, `export_function_graph`, `set_function_metadata`, `update_material_function`, `delete_function_expression`, `create_function_instance`, `set_function_instance_parameter`, `get_function_instance_info`, `layout_function_expressions`, `rename_function_parameter_group` |
| Custom HLSL | 2 | `create_custom_hlsl_node`, `update_custom_hlsl_node` |
| Spec / templates | 3 | `export_material_graph`, `import_material_graph`, `validate_material` |
| Preview / capture | 2 | `render_preview`, `get_thumbnail` |
| Textures | 5 | `import_texture`, `create_pbr_material_from_disk`, `get_texture_properties`, `preview_texture`, `preview_textures`, `check_tiling_quality` |
| Layers | 1 | `get_layer_info` |
| Batch | 2 | `batch_set_material_property`, `batch_recompile` |
| Transactions | 2 | `begin_transaction`, `end_transaction` |

### `build_material_graph` gotcha

This action **requires** the `{ "graph_spec": { ... } }` wrapper, not a bare spec. This trips people up:

```json
{ "graph_spec": { "expressions": [...], "connections": [...] } }
```

See `Plugins/Monolith/Docs/specs/SPEC_MonolithMaterial.md` for full graph_spec schema and the [§Pipelines](#pipelines) section for the canonical material build flow.

---

## animation

Animation curves, bone tracks, sync markers, root motion, compression, blend spaces, ABPs, montages, skeletons, PoseSearch, IKRig, Control Rig. **118 actions** total — 96 baseline + 13 PoseSearch + 5 ABP write + 3 Control Rig write + 1 layout.

> For full param schemas, call `monolith_discover("animation")` at runtime.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| Sequence ops | 12 | `get_sequence_info`, `get_sequence_notifies`, `set_sequence_properties`, `set_additive_settings`, `set_compression_settings`, `set_root_motion_settings`, `create_sequence`, `duplicate_sequence`, `build_sequence_from_poses` |
| Bone tracks | 4 | `add_bone_track`, `remove_bone_track`, `set_bone_track_keys`, `get_bone_track_keys` |
| Curves | 6 | `add_curve`, `remove_curve`, `set_curve_keys`, `get_curve_keys`, `list_curves`, `get_skeleton_curves` |
| Notifies | 9 | `add_notify`, `add_notify_state`, `remove_notify`, `set_notify_time`, `set_notify_duration`, `set_notify_track`, `set_notify_properties`, `bulk_add_notify`, `clone_notify_setup` |
| Sync markers | 4 | `get_sync_markers`, `add_sync_marker`, `remove_sync_marker`, `rename_sync_marker` |
| Skeleton | 5 | `get_skeleton_info`, `get_skeletal_mesh_info`, `add_socket`, `remove_socket`, `set_socket_transform`, `get_skeleton_sockets`, `add_virtual_bone`, `remove_virtual_bones`, `compare_skeletons` |
| Montages | 9 | `get_montage_info`, `create_montage`, `set_montage_blend`, `add_montage_section`, `delete_montage_section`, `set_section_next`, `set_section_time`, `add_montage_slot`, `set_montage_slot`, `add_montage_anim_segment`, `create_montage_from_sections` |
| Blend spaces | 8 | `get_blend_space_info`, `create_blend_space`, `create_blend_space_1d`, `create_aim_offset`, `create_aim_offset_1d`, `add_blendspace_sample`, `edit_blendspace_sample`, `delete_blendspace_sample`, `set_blend_space_axis` |
| ABPs | 9 | `get_abp_info`, `create_anim_blueprint`, `get_state_machines`, `get_state_info`, `get_transitions`, `get_blend_nodes`, `get_linked_layers`, `get_graphs`, `get_nodes`, `get_abp_variables`, `get_abp_linked_assets` |
| State machines (write) | 3 | `add_state_to_machine`, `add_transition`, `set_transition_rule` |
| ABP graph (write) | 5 | `add_anim_graph_node`, `connect_anim_graph_pins`, `set_state_animation`, `add_variable_get`, `set_anim_graph_node_property` |
| Composites | 3 | `get_composite_info`, `add_composite_segment`, `remove_composite_segment`, `create_composite` |
| IKRig / Retarget | 6 | `get_ikrig_info`, `add_ik_solver`, `get_retargeter_info`, `set_retarget_chain_mapping`, `add_retarget_chain`, `remove_retarget_chain`, `set_retarget_chain_bones` |
| Control Rig | 7 | `get_control_rig_info`, `get_control_rig_variables`, `add_control_rig_element`, `get_control_rig_graph`, `add_control_rig_node`, `connect_control_rig_pins` |
| Anim modifiers | 2 | `apply_anim_modifier`, `list_anim_modifiers` |
| Physics asset | 3 | `get_physics_asset_info`, `set_body_properties`, `set_constraint_properties` |
| PoseSearch | 13 | `get_pose_search_schema`, `get_pose_search_database`, `add_database_sequence`, `remove_database_sequence`, `get_database_stats`, `create_pose_search_schema`, `create_pose_search_database`, `set_database_sequence_properties`, `add_schema_channel`, `remove_schema_channel`, `set_channel_weight`, `rebuild_pose_search_index`, `set_database_search_mode` |
| Layout / batch | 2 | `auto_layout`, `batch_execute` |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithAnimation.md` for the deep dive.

---

## niagara

Niagara VFX system editing — emitters, modules, params, renderers, HLSL, dynamic inputs, event handlers, sim stages, NPC, effect types. **109 actions** (108 baseline + 1 layout).

> For full param schemas, call `monolith_discover("niagara")` at runtime.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| Systems | 11 | `create_system`, `create_system_from_spec`, `duplicate_system`, `validate_system`, `save_system`, `set_system_property`, `get_system_property`, `get_system_summary`, `get_system_diagnostics`, `set_fixed_bounds`, `set_effect_type`, `list_systems` |
| Emitters | 12 | `add_emitter`, `remove_emitter`, `duplicate_emitter`, `set_emitter_enabled`, `reorder_emitters`, `set_emitter_property`, `get_emitter_property`, `get_emitter_summary`, `list_emitters`, `list_emitter_properties`, `create_emitter`, `rename_emitter`, `save_emitter_as_template`, `clear_emitter_modules`, `get_emitter_parent` |
| Modules | 10 | `add_module`, `remove_module`, `move_module`, `set_module_enabled`, `get_ordered_modules`, `get_module_inputs`, `get_module_graph`, `get_module_input_value`, `get_module_output_parameters`, `get_module_script_inputs`, `set_module_input_value`, `set_module_input_binding`, `set_module_input_di`, `clone_module_overrides`, `duplicate_module`, `list_module_scripts` |
| HLSL / scripts | 2 | `create_module_from_hlsl`, `create_function_from_hlsl` |
| Parameters | 9 | `get_all_parameters`, `get_user_parameters`, `get_parameter_value`, `get_parameter_type`, `trace_parameter_binding`, `add_user_parameter`, `remove_user_parameter`, `set_parameter_default`, `set_curve_value`, `get_available_parameters`, `rename_user_parameter`, `set_static_switch_value`, `get_static_switch_value` |
| Renderers | 9 | `add_renderer`, `remove_renderer`, `set_renderer_material`, `set_renderer_property`, `get_renderer_bindings`, `set_renderer_binding`, `list_renderers`, `list_renderer_properties`, `list_available_renderers`, `set_renderer_mesh`, `configure_ribbon`, `configure_subuv` |
| Dynamic inputs | 7 | `add_dynamic_input`, `set_dynamic_input_value`, `search_dynamic_inputs`, `list_dynamic_inputs`, `get_dynamic_input_tree`, `remove_dynamic_input`, `get_dynamic_input_value`, `get_dynamic_input_inputs` |
| Event handlers / sim stages | 8 | `add_event_handler`, `get_event_handlers`, `set_event_handler_property`, `remove_event_handler`, `add_simulation_stage`, `get_simulation_stages`, `set_simulation_stage_property`, `remove_simulation_stage`, `set_spawn_shape` |
| NPC | 5 | `create_npc`, `get_npc`, `add_npc_parameter`, `remove_npc_parameter`, `set_npc_default` |
| Effect types | 3 | `create_effect_type`, `get_effect_type`, `set_effect_type_property` |
| Data interfaces | 4 | `get_di_functions`, `get_compiled_gpu_hlsl`, `configure_data_interface`, `get_di_properties` |
| Compile / preview | 4 | `request_compile`, `preview_system`, `diff_systems`, `get_scalability_settings`, `set_scalability_settings` |
| Curves / spec | 5 | `configure_curve_keys`, `import_system_spec`, `export_system_spec`, `batch_execute`, `auto_layout` |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithNiagara.md`.

---

## editor

Live Coding builds, compile output capture, editor log capture, scene capture, texture import, asset deletion, viewport info, GIF capture, **map creation** and **module status** (Phase J F8). **22 actions.**

### `editor.trigger_build` / `editor.live_compile`

Trigger a Live Coding compile. Aliased — they're the same handler.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `wait` | bool | optional | Block until compile finishes. Default: `false` |

### `editor.get_build_errors`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `since` | number | optional | Only errors from the last N seconds |
| `category` | string | optional | Filter to a specific log category |
| `compile_only` | bool | optional | Filter to compile categories only. Default: `false` |

### `editor.get_build_status`

Check compile status: `compiling`, `last_result`, `last_compile_time`, `errors_since_compile`, `patch_applied`. *No parameters.*

### `editor.get_build_summary` · `editor.search_build_output` · `editor.get_compile_output`

Build summary, search-build-log-by-pattern, structured compile report. See `monolith_discover("editor")` for params.

### `editor.get_recent_logs` · `editor.search_logs` · `editor.tail_log` · `editor.get_log_categories` · `editor.get_log_stats`

Editor log inspection. `search_logs` accepts `pattern`, `category`, `verbosity`, `limit`. `tail_log` and `get_recent_logs` take `count`.

### `editor.get_crash_context`

Get last crash/ensure context. *No parameters.*

### `editor.capture_scene_preview`

Render a Niagara system or material in a preview scene and screenshot it.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Asset to preview |
| `asset_type` | string | **required** | `niagara` or `material` |
| `preview_mesh` | string | optional | For materials: `plane`, `sphere`, `cube`. Default: `plane` |
| `seek_time` | number | optional | Niagara sim time (seconds). Default: `0.0` |
| `camera` | object | optional | `{location:[x,y,z], rotation:[p,y,r], fov:60}` |
| `resolution` | array | optional | `[width, height]`. Default: `[512, 512]` |
| `output_path` | string | optional | Output PNG path |

### `editor.capture_sequence_frames`

Capture multiple frames at specified timestamps. Same params as `capture_scene_preview` plus `timestamps[]`, `output_dir`, `filename_prefix`, `persistent`.

### `editor.import_texture`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `source_path` | string | **required** | Absolute path to source image (PNG, TGA, EXR, HDR) |
| `destination` | string | **required** | UE asset path |
| `settings` | object | optional | `{compression, srgb, tiling, max_size, lod_group}` |

### `editor.stitch_flipbook`

Stitch frame PNGs into a flipbook atlas. Used by the VFX training harness.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `frame_paths` | array | **required** | Ordered absolute paths to frame PNGs |
| `dest_path` | string | **required** | UE asset path for output texture |
| `grid` | array | **required** | `[columns, rows]` grid layout |
| `srgb` | bool | optional | sRGB color space. Default: `true` |
| `no_mipmaps` | bool | optional | Disable mipmaps to prevent atlas bleed. Default: `true` |
| `delete_sources` | bool | optional | Delete source PNGs after stitch. Default: `true` |
| `lod_group` | string | optional | Default: `TEXTUREGROUP_Effects` |

> **Experimental flag.** Designed for the VFX training harness. Treat as best-effort.

### `editor.delete_assets`

Delete UE assets by path. **Experimental.** Use the `allowed_prefixes` safety guard.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_paths` | array | **required** | UE asset paths to delete |
| `allowed_prefixes` | array | optional | Restrict to paths starting with one of these (e.g. `["/Game/AgentTraining/"]`) |

### `editor.get_viewport_info`

Current editor viewport camera position, rotation, FOV, resolution. *No parameters.*

### `editor.capture_system_gif`

Capture a Niagara system as a sequence of PNG frames with optional GIF encoding via ffmpeg or python.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Niagara system asset path |
| `duration_seconds` | number | optional | Default: `2.0` |
| `fps` | integer | optional | Default: `15` |
| `resolution` | integer | optional | Default: `256` |
| `output_path` | string | optional | Default: `Saved/Screenshots/Monolith/GIF_<timestamp>` |
| `encoder` | string | optional | `frames_only` (default), `ffmpeg`, or `python` |

### `editor.create_empty_map` · NEW in Phase J F8

Create a fully blank `UWorld` asset at the given `/Game/...` path. Saves immediately.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path` | string | **required** | Asset path under `/Game/...` (e.g. `/Game/Tests/Monolith/Audio/Map_Test`) |
| `map_template` | string | optional | `blank` (default). Reserved: `vr_basic`, `thirdperson_basic` — return error in v1; UE 5.7 templates are populated client-side, not via `UWorldFactory`. |

### `editor.get_module_status` · NEW in Phase J F8

Report plugin-enabled + module-loaded status for Monolith (or arbitrary) modules. Wraps `IPluginManager` + `FModuleManager`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `module_names` | array | optional | Module name strings. Omit to query all Monolith modules. Unknown names return `enabled=false, loaded=false` (no error). |

---

## config

INI config file inspection and search. **6 actions.** Read-only.

### `config.resolve_setting`

Get effective value of a config key across the full INI hierarchy.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file` | string | **required** | Config category (e.g. `Engine`, `Game`, `Input`) |
| `section` | string | **required** | Config section (e.g. `/Script/Engine.RendererSettings`) |
| `key` | string | **required** | Config key name |

### `config.explain_setting`

Show where a config value comes from across Base → Default → User layers.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file` | string | optional | Config category |
| `section` | string | optional | Config section |
| `key` | string | optional | Config key name |
| `setting` | string | optional | Convenience: search for this key across common categories |

### `config.diff_from_default`

Show project config overrides vs engine defaults for a category.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file` | string | **required** | Config category to diff |
| `section` | string | optional | Filter to a specific section |

### `config.search_config`

Full-text search across all config files.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | Search text |
| `category` | string | optional | Filter to a config category |

### `config.get_section`

Read an entire config section from a specific file.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file` | string | **required** | Config file name or category |
| `section` | string | **required** | Section name |

### `config.get_config_files`

List all config files with their hierarchy level.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `category` | string | optional | Filter to a specific category |

---

## project

Project-wide asset index backed by SQLite + FTS5. **7 actions.**

### `project.search`

Full-text search across all indexed project assets, nodes, variables, and parameters.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | FTS5 search query (supports `AND`, `OR`, `NOT`, `prefix*`) |
| `limit` | integer | optional | Default: `50` |

### `project.find_references`

Find all assets that reference or are referenced by the given asset.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path |

### `project.find_by_type`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_type` | string | **required** | Asset class name (e.g. `Blueprint`, `Material`, `StaticMesh`, `Texture2D`) |
| `module` | string | optional | Filter by plugin/module name |
| `limit` | integer | optional | Default: `100` |
| `offset` | integer | optional | Pagination. Default: `0` |

### `project.get_stats`

Project index stats — total counts by table and asset class breakdown. *No parameters.*

### `project.get_asset_details`

Deep details for a specific asset — nodes, variables, parameters, dependencies.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path |

### `project.list_gameplay_tags`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `prefix` | string | optional | Tag prefix filter (e.g. `Weapon.Melee`) |

### `project.search_gameplay_tags`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | Substring to search for in tag names |

---

## source

Unreal Engine C++ source code navigation. 1M+ symbols indexed. **11 actions.**

### `source.read_source`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Class, function, or struct name |
| `include_header` | bool | optional | Include the header declaration. Default: `false` |
| `max_lines` | integer | optional | Default: `500` |
| `members_only` | bool | optional | Only show class members. Default: `false` |

### `source.find_references` · `source.find_callers` · `source.find_callees`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Symbol or function name |
| `ref_kind` | string | optional | (`find_references` only) Filter by reference kind |
| `limit` | integer | optional | Default: `50` |

### `source.search_source`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | Search query |
| `scope` | string | optional | `all`, `engine`, `shaders` |
| `mode` | string | optional | `fts`, `regex`, `exact` |
| `module` | string | optional | Filter to a specific module |
| `path_filter` | string | optional | File path pattern |
| `symbol_kind` | string | optional | `class`, `function`, `enum`, etc. |
| `limit` | integer | optional | Default: `50` |

### `source.get_class_hierarchy`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Class name |
| `direction` | string | optional | `up` (parents), `down` (children), or `both`. Default: `both` |
| `depth` | integer | optional | Default: `5` |

### `source.get_module_info`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `module_name` | string | **required** | Module name |

### `source.get_symbol_context`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Symbol name |
| `context_lines` | integer | optional | Default: `10` |

### `source.read_file`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file_path` | string | **required** | Source file path |
| `start_line` | integer | optional | Default: `1` |
| `end_line` | integer | optional | Default: end of file |

### `source.trigger_reindex` · `source.trigger_project_reindex`

`trigger_reindex` does a full clean build (engine + shaders + project). `trigger_project_reindex` is incremental (project Source/ + Plugins/ only). Both take *no parameters*.

---

## mesh

Mesh inspection, scene manipulation, spatial queries, level blockout, GeometryScript, procedural geometry, lighting, audio, performance, and **experimental** procedural town generation. **240 actions** total — 195 core (always registered) + 45 experimental town gen (gated on `bEnableProceduralTownGen=true`, default `false`).

> For full param schemas, call `monolith_discover("mesh")` at runtime. The action surface is too broad for full enumeration — see categories below.

**Action categories (core, always registered):**

| Category | Examples |
|----------|----------|
| Mesh inspection | `get_mesh_info`, `get_mesh_bounds`, `get_mesh_materials`, `get_mesh_lods`, `get_mesh_collision`, `get_mesh_uvs`, `analyze_skeletal_mesh`, `analyze_mesh_quality`, `compare_meshes`, `get_vertex_data`, `search_meshes_by_size`, `get_mesh_catalog_stats` |
| Scene actors | `get_actor_info`, `spawn_actor`, `move_actor`, `duplicate_actor`, `delete_actors`, `group_actors`, `set_actor_properties`, `align_actors`, `snap_to_floor`, `manage_folders`, `set_actor_tags` |
| Spatial queries | `query_raycast`, `query_multi_raycast`, `query_radial_sweep`, `query_overlap`, `query_nearest`, `query_line_of_sight`, `get_actors_in_volume`, `get_scene_bounds`, `get_scene_statistics`, `get_spatial_relationships`, `query_navmesh` |
| Blockout | `get_blockout_volumes`, `setup_blockout_volume`, `create_blockout_primitive`, `create_blockout_primitives_batch`, `create_blockout_grid`, `match_asset_to_blockout`, `match_all_in_volume`, `apply_replacement`, `clear_blockout`, `export_blockout_layout`, `import_blockout_layout`, `scan_volume`, `scatter_props`, `create_blockout_blueprint` |
| Level analysis | `analyze_sightlines`, `find_hiding_spots`, `find_ambush_points`, `analyze_choke_points`, `analyze_escape_routes`, `classify_zone_tension`, `analyze_pacing_curve`, `find_dead_ends`, `validate_path_width`, `validate_navigation_complexity`, `analyze_visual_contrast`, `find_rest_points`, `validate_interactive_reach`, `generate_accessibility_report` |
| Performance | `get_region_performance`, `estimate_placement_cost`, `find_overdraw_hotspots`, `analyze_shadow_cost`, `get_triangle_budget`, `analyze_texel_density`, `analyze_material_cost_in_region`, `analyze_lightmap_density`, `find_instancing_candidates`, `convert_to_hism`, `setup_hlod`, `analyze_texture_budget`, `generate_proxy_mesh` |
| Lighting | `place_light`, `set_light_properties`, `sample_light_levels`, `find_dark_corners`, `analyze_light_transitions`, `get_light_coverage`, `suggest_light_placement` |
| Audio | `get_audio_volumes`, `get_surface_materials`, `estimate_footstep_sound`, `analyze_room_acoustics`, `analyze_sound_propagation`, `find_loud_surfaces`, `find_sound_paths`, `can_ai_hear_from`, `get_stealth_map`, `find_quiet_path`, `suggest_audio_volumes`, `create_audio_volume`, `set_surface_type` |
| Decals / scatter | `place_decals`, `place_along_path`, `analyze_prop_density`, `place_storytelling_scene`, `scatter_on_surface`, `scatter_on_walls`, `scatter_on_ceiling`, `randomize_transforms` |
| Encounter design | `analyze_ai_territory`, `evaluate_safe_room`, `predict_player_paths`, `evaluate_spawn_point`, `suggest_scare_positions`, `evaluate_encounter_pacing`, `design_encounter`, `suggest_patrol_route`, `analyze_level_pacing_structure`, `generate_scare_sequence`, `validate_horror_intensity`, `evaluate_monster_reveal`, `analyze_co_op_balance` |
| Templates / presets | `list_room_templates`, `get_room_template`, `apply_room_template`, `create_room_template`, `list_storytelling_patterns`, `create_storytelling_pattern`, `list_acoustic_profiles`, `create_acoustic_profile`, `create_tension_profile`, `list_genre_presets`, `export_genre_preset`, `import_genre_preset` |
| Validation | `validate_game_ready`, `suggest_lod_strategy`, `batch_validate`, `compare_lod_chain`, `validate_naming_conventions`, `batch_rename_assets` |
| GeometryScript | `mesh_boolean`, `mesh_simplify`, `mesh_remesh`, `generate_collision`, `generate_lods`, `fill_holes`, `compute_uvs`, `mirror_mesh` |
| Procedural meshes | `create_parametric_mesh`, `create_horror_prop`, `create_structure`, `create_building_shell`, `create_maze`, `create_pipe_network`, `create_fragments`, `create_terrain_patch` |
| Cache | `list_cached_meshes`, `clear_cache`, `validate_cache`, `get_cache_stats` |
| Handles | `create_handle`, `release_handle`, `list_handles`, `save_handle` |
| Prefabs | `create_prefab`, `create_blueprint_prefab`, `spawn_prefab`, `place_blueprint_actor`, `place_spline`, `create_prop_kit`, `place_prop_kit` |
| Hospice / accessibility | `generate_hospice_report`, `analyze_framing` |

**Action categories (experimental town gen, OFF by default):**

| Category | Examples |
|----------|----------|
| Floor plans | `generate_floor_plan`, `create_building_from_grid` |
| Facades / roofs | `generate_facade`, `generate_roof`, `generate_arch_features` |
| City blocks | `create_city_block`, `register_building`, `query_spatial_registry` |
| Auto volumes | `create_auto_volumes`, `adapt_terrain` |
| Furnishing | `furnish_room`, `validate_building` |
| Debug | Debug views and diagnostics |

> **Experimental — town gen has known geometry issues** (wall misalignment, room separation). Fix Plans v2-v5 applied 27+ fixes but fundamental issues remain. Core mesh actions (sweep walls, auto-collision, proc mesh caching, blueprint prefabs) work fine.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithMesh.md` for the full action catalog.

---

## ui

UMG widget Blueprint CRUD, templates, styling, animation, settings scaffolding, accessibility, **CommonUI**, and GAS UI bindings. **96 actions** = 42 UMG baseline + 50 CommonUI + 4 GAS UI binding aliases.

> For full param schemas, call `monolith_discover("ui")` at runtime.

**Action categories (UMG baseline, always registered):**

| Category | Actions | Examples |
|----------|---------|----------|
| Widget CRUD | 7 | `create_widget_blueprint`, `get_widget_tree`, `add_widget`, `remove_widget`, `set_widget_property`, `compile_widget`, `list_widget_types` |
| Slot / layout | 4 | `set_slot_property`, `set_anchor_preset`, `move_widget`, `set_brush` |
| Styling | 6 | `set_font`, `set_color_scheme`, `batch_style`, `set_text`, `set_image`, `setup_list_view` |
| Templates / scaffolds | 13 | `create_hud_element`, `create_menu`, `create_settings_panel`, `create_dialog`, `create_notification_toast`, `create_loading_screen`, `create_inventory_grid`, `create_save_slot_list`, `scaffold_game_user_settings`, `scaffold_save_game`, `scaffold_save_subsystem`, `scaffold_audio_settings`, `scaffold_input_remapping` |
| Animation | 5 | `list_animations`, `get_animation_details`, `create_animation`, `add_animation_keyframe`, `remove_animation` |
| Inspection | 3 | `list_widget_events`, `list_widget_properties`, `get_widget_bindings` |
| Accessibility | 5 | `scaffold_accessibility_subsystem`, `audit_accessibility`, `set_colorblind_mode`, `set_text_scale`, `apply_high_contrast_variant` |

**Action categories (CommonUI, registered when `WITH_COMMONUI=1`):**

| Category | Actions | Examples |
|----------|---------|----------|
| Activatable widgets | 8 | `create_activatable_widget`, `create_activatable_stack`, `create_activatable_switcher`, `configure_activatable`, `push_to_activatable_stack`, `pop_activatable_stack`, `get_activatable_stack_state`, `set_activatable_transition` |
| Common buttons / styles | 6 | `convert_button_to_common`, `configure_common_button`, `create_common_button_style`, `create_common_text_style`, `create_common_border_style`, `apply_style_to_widget`, `batch_retheme` |
| Common config | 2 | `configure_common_text`, `configure_common_border` |
| Input | 7 | `create_input_action_data_table`, `add_input_action_row`, `bind_common_action_widget`, `create_bound_action_bar`, `get_active_input_type`, `set_input_type_override`, `list_platform_input_tables` |
| Navigation / focus | 5 | `set_widget_navigation`, `set_initial_focus_target`, `force_focus`, `get_focus_path`, `request_refresh_focus`, `enforce_focus_ring` |
| Lists / tabs / groups | 4 | `setup_common_list_view`, `create_tab_list_widget`, `register_tab`, `create_button_group` |
| Carousels / switcher | 2 | `configure_animated_switcher`, `create_widget_carousel` |
| Hardware | 1 | `create_hardware_visibility_border` |
| Numeric / rotator | 2 | `configure_numeric_text`, `configure_rotator` |
| Lazy / load guard | 2 | `create_lazy_image`, `create_load_guard` |
| Modals / messages | 2 | `show_common_message`, `configure_modal_overlay` |
| Audit / report | 2 | `audit_commonui_widget`, `export_commonui_report` |
| Reload | 1 | `hot_reload_styles`, `dump_action_router_state` |
| Reduce motion | 1 | `wrap_with_reduce_motion_gate`, `set_text_scale_binding` |

**GAS UI binding aliases (4 — same handlers as `gas::*` versions):**

`bind_widget_to_attribute`, `unbind_widget_attribute`, `list_attribute_bindings`, `clear_widget_attribute_bindings`. These four are registered into `ui` from `MonolithGAS/Private/MonolithGASUIBindingActions.cpp`. Pick whichever namespace reads better in your call site — both dispatch to identical code.

> **Phase J F2/F3:** these four actions now reject empty `widget_path`, missing `attribute`, or unresolvable ASC up-front with structured errors instead of writing junk via reflection.
> **Phase J F5:** the response shape is `{ bindings: [...], count: N }`, not a bare array. Wrap your client parsers.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithUI.md` for the deep dive including style-creator-as-data Blueprint pattern and conditional CommonUI gating.

---

## gas

Gameplay Ability System integration. **135 actions** across 11 categories — covers the full GAS authoring pipeline. **Conditional on `#if WITH_GBA`** — projects without the GameplayAbilities plugin register 0 GAS actions.

> For full param schemas, call `monolith_discover("gas")` at runtime.

**Action categories:**

| Category | Actions | Description |
|----------|---------|-------------|
| Scaffold | 7 | `bootstrap_gas_foundation`, `validate_gas_setup`, `scaffold_gas_project`, `scaffold_damage_pipeline`, `scaffold_status_effect`, `scaffold_weapon_ability`, `scaffold_tag_hierarchy` |
| Attributes | 18 | `create_attribute_set`, `add_attribute`, `get_attribute_set`, `set_attribute_defaults`, `list_attribute_sets`, `configure_attribute_clamping`, `configure_meta_attributes`, `create_attribute_set_from_template`, `create_attribute_init_datatable`, `duplicate_attribute_set`, `configure_attribute_replication`, `link_datatable_to_asc`, `bulk_edit_attributes`, `validate_attribute_set`, `find_attribute_modifiers`, `diff_attribute_sets`, `get_attribute_dependency_graph`, `remove_attribute`, `get_attribute_value`, `set_attribute_value` |
| Effects | 22 | `create_gameplay_effect`, `get_gameplay_effect`, `list_gameplay_effects`, `add_modifier`, `set_modifier`, `remove_modifier`, `list_modifiers`, `add_ge_component`, `set_ge_component`, `remove_ge_component`, `set_effect_stacking`, `set_duration`, `set_period`, `create_effect_from_template`, `build_effect_from_spec`, `batch_create_effects`, `add_execution`, `duplicate_gameplay_effect`, `delete_gameplay_effect`, `validate_effect`, `get_effect_interaction_matrix`, `get_active_effects`, `get_effect_modifiers_breakdown`, `apply_effect`, `remove_effect`, `simulate_effect_stack` |
| Abilities | 24 | `create_ability`, `get_ability_info`, `list_abilities`, `compile_ability`, `set_ability_tags`, `get_ability_tags`, `set_ability_policy`, `set_ability_cost`, `set_ability_cooldown`, `set_ability_triggers`, `set_ability_flags`, `add_ability_task_node`, `add_commit_and_end_flow`, `add_effect_application`, `add_gameplay_cue_node`, `create_ability_from_template`, `build_ability_from_spec`, `batch_create_abilities`, `duplicate_ability`, `list_ability_tasks`, `get_ability_task_pins`, `wire_ability_task_delegate`, `get_ability_graph_flow`, `validate_ability`, `find_abilities_by_tag`, `get_ability_tag_matrix`, `validate_ability_blueprint`, `scaffold_custom_ability_task` |
| ASC | 13 | `add_asc_to_actor`, `configure_asc`, `setup_asc_init`, `setup_ability_system_interface`, `apply_asc_template`, `set_default_abilities`, `set_default_effects`, `set_default_attribute_sets`, `set_asc_replication_mode`, `validate_asc_setup`, `grant_ability`, `revoke_ability`, `get_asc_snapshot`, `get_all_ascs`, `grant_ability_to_pawn` *(NEW Phase J F8)* |
| Tags | 10 | `add_gameplay_tags`, `get_tag_hierarchy`, `search_tag_usage`, `scaffold_tag_hierarchy`, `rename_tag`, `remove_gameplay_tags`, `validate_tag_consistency`, `audit_tag_naming`, `export_tag_hierarchy`, `import_tag_hierarchy` |
| Cues | 10 | `create_gameplay_cue_notify`, `link_cue_to_effect`, `unlink_cue_from_effect`, `get_cue_info`, `list_gameplay_cues`, `set_cue_parameters`, `find_cue_triggers`, `validate_cue_coverage`, `batch_create_cues`, `scaffold_cue_library` |
| Targeting | 5 | `create_target_actor`, `configure_target_actor`, `add_targeting_to_ability`, `scaffold_fps_targeting`, `validate_targeting` |
| Input | 5 | `setup_ability_input_binding`, `bind_ability_to_input`, `batch_bind_abilities`, `get_ability_input_bindings`, `scaffold_input_binding_component` |
| Inspect (PIE) | 6 | `export_gas_manifest`, `snapshot_gas_state`, `get_tag_state`, `get_cooldown_state`, `trace_ability_activation`, `compare_gas_states` |
| UI bindings | 4 | `bind_widget_to_attribute`, `unbind_widget_attribute`, `list_attribute_bindings`, `clear_widget_attribute_bindings` *(also aliased into `ui` namespace — same handlers)* |

### `gas.grant_ability_to_pawn` · NEW in Phase J F8

Grant a `UGameplayAbility` to a pawn's `UAbilitySystemComponent` directly without scaffold-side wiring or `apply_effect` ceremony. See `monolith_discover("gas")` for params.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithGAS.md` for the deep dive.

---

## combograph

ComboGraph melee combo authoring. **13 actions.** **Conditional on `#if WITH_COMBOGRAPH`** — requires the ComboGraph marketplace plugin. Reflection-only (no direct C++ API linkage).

| Action | Params |
|--------|--------|
| `list_combo_graphs` | `path_filter?` |
| `get_combo_graph_info` | `asset_path` |
| `get_combo_node_effects` | `asset_path`, `node_index` |
| `validate_combo_graph` | `asset_path` |
| `create_combo_graph` | `save_path` |
| `add_combo_node` | `asset_path`, `animation_asset`, `node_type?`, `parent_node_index?`, `play_rate?` |
| `add_combo_edge` | `asset_path`, `from_node_index`, `to_node_index`, `input_action?`, `trigger_event?`, `transition_behavior?` |
| `set_combo_node_effects` | `asset_path`, `node_index`, `effects` (gameplay tag → container map) |
| `set_combo_node_cues` | `asset_path`, `node_index`, `cues` (gameplay tag → cue map) |
| `create_combo_ability` | `save_path`, `combo_graph?`, `initial_input?`, `parent_class?` |
| `link_ability_to_combo_graph` | `ability_path`, `combo_graph` |
| `scaffold_combo_from_montages` | `save_path`, `montages[]`, `input_action?`, `transition_behavior?` |
| `layout_combo_graph` | `asset_path`, `horizontal_spacing?`, `vertical_spacing?` |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithComboGraph.md`.

---

## ai

Behavior Trees, State Trees, EQS, Blackboards, AI Controllers, Perception, Smart Objects, Navigation, Mass Entity, Zone Graph, runtime PIE inspection, and a deep library of scaffolds. **221 actions** — the largest single conditional namespace.

**Conditional on `#if WITH_STATETREE` + `#if WITH_SMARTOBJECTS`** — projects missing either plugin register 0 AI actions.

> For full param schemas, call `monolith_discover("ai")` at runtime.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| Blackboards | 12 | `create_blackboard`, `get_blackboard`, `list_blackboards`, `delete_blackboard`, `duplicate_blackboard`, `add_bb_key`, `remove_bb_key`, `rename_bb_key`, `get_bb_key_details`, `batch_add_bb_keys`, `set_bb_parent`, `compare_blackboards` |
| Behavior Trees | 25 | `create_behavior_tree`, `get_behavior_tree`, `list_behavior_trees`, `delete_behavior_tree`, `duplicate_behavior_tree`, `set_bt_blackboard`, `list_bt_node_classes`, `add_bt_node`, `remove_bt_node`, `move_bt_node`, `add_bt_decorator`, `remove_bt_decorator`, `add_bt_service`, `remove_bt_service`, `set_bt_node_property`, `get_bt_node_properties`, `reorder_bt_children`, `add_bt_run_eqs_task`, `add_bt_smart_object_task`, `add_bt_use_ability_task`, `build_behavior_tree_from_spec`, `export_bt_spec`, `import_bt_spec`, `validate_behavior_tree`, `clone_bt_subtree`, `auto_arrange_bt`, `compare_behavior_trees`, `create_bt_task_blueprint`, `create_bt_decorator_blueprint`, `create_bt_service_blueprint`, `generate_bt_diagram`, `get_bt_graph` *(NEW Phase J F8)* |
| State Trees | 28 | `create_state_tree`, `get_state_tree`, `list_state_trees`, `delete_state_tree`, `duplicate_state_tree`, `compile_state_tree`, `set_st_schema`, `add_st_state`, `remove_st_state`, `rename_st_state`, `move_st_state`, `set_st_state_properties`, `add_st_task`, `remove_st_task`, `set_st_task_property`, `add_st_enter_condition`, `remove_st_enter_condition`, `add_st_transition`, `remove_st_transition`, `add_st_property_binding`, `remove_st_property_binding`, `get_st_bindings`, `get_st_bindable_properties`, `list_st_task_types`, `list_st_condition_types`, `add_st_transition_condition`, `add_st_consideration`, `configure_st_consideration`, `validate_state_tree`, `list_st_extension_types`, `add_st_extension`, `build_state_tree_from_spec`, `export_st_spec`, `generate_st_diagram`, `auto_arrange_st` |
| EQS | 21 | `create_eqs_query`, `get_eqs_query`, `list_eqs_queries`, `delete_eqs_query`, `duplicate_eqs_query`, `add_eqs_generator`, `remove_eqs_generator`, `configure_eqs_generator`, `add_eqs_test`, `remove_eqs_test`, `configure_eqs_test`, `configure_eqs_scoring`, `configure_eqs_filter`, `list_eqs_generator_types`, `list_eqs_test_types`, `list_eqs_contexts`, `validate_eqs_query`, `reorder_eqs_tests`, `build_eqs_query_from_spec`, `create_eqs_from_template` |
| AI Controllers | 8 | `create_ai_controller`, `get_ai_controller`, `list_ai_controllers`, `set_ai_controller_bt`, `set_pawn_ai_controller_class`, `set_ai_controller_flags`, `set_ai_team`, `get_ai_team`, `spawn_ai_actor`, `get_ai_actors` |
| Perception | 11 | `add_perception_component`, `get_perception_config`, `configure_sight_sense`, `configure_hearing_sense`, `configure_damage_sense`, `configure_touch_sense`, `remove_sense`, `add_stimuli_source_component`, `configure_stimuli_source`, `validate_perception_setup`, `get_ai_system_config`, `add_perception_to_actor` *(NEW Phase J F8)* |
| Smart Objects | 14 | `create_smart_object_definition`, `get_smart_object_definition`, `list_smart_object_definitions`, `delete_smart_object_definition`, `add_so_slot`, `remove_so_slot`, `configure_so_slot`, `add_so_behavior_definition`, `remove_so_behavior_definition`, `set_so_tags`, `add_smart_object_component`, `place_smart_object_actor`, `find_smart_objects_in_level`, `validate_smart_object_definition`, `create_so_from_template`, `duplicate_smart_object_definition` |
| Navigation | 19 | `get_nav_system_config`, `get_navmesh_config`, `set_navmesh_config`, `get_navmesh_stats`, `add_nav_bounds_volume`, `list_nav_bounds_volumes`, `build_navigation`, `get_nav_build_status`, `list_nav_areas`, `create_nav_area`, `add_nav_modifier_volume`, `add_nav_link_proxy`, `configure_nav_link`, `list_nav_links`, `find_path`, `test_path`, `project_point_to_navigation`, `get_random_navigable_point`, `navigation_raycast`, `configure_nav_agent`, `add_nav_invoker_component`, `get_crowd_manager_config`, `set_crowd_manager_config`, `analyze_navigation_coverage` |
| Runtime PIE | 13 | `runtime_get_bb_value`, `runtime_set_bb_value`, `runtime_clear_bb_value`, `runtime_get_bt_state`, `runtime_start_bt`, `runtime_stop_bt`, `runtime_get_bt_execution_path`, `runtime_get_perceived_actors`, `runtime_check_perception`, `runtime_report_noise`, `runtime_get_st_active_states`, `runtime_send_st_event`, `runtime_find_smart_objects`, `runtime_run_eqs_query` |
| Scaffolds | 21 | `hello_world_ai`, `scaffold_complete_ai_character`, `scaffold_perception_to_blackboard`, `scaffold_team_system`, `scaffold_patrol_investigate_ai`, `scaffold_enemy_ai`, `scaffold_eqs_move_sequence`, `create_bt_from_template`, `create_st_from_template`, `scaffold_ai_controller_blueprint`, `scaffold_companion_ai`, `scaffold_boss_ai`, `scaffold_ambient_npc`, `scaffold_horror_stalker`, `scaffold_horror_ambush`, `scaffold_horror_presence`, `scaffold_horror_mimic`, `scaffold_stealth_game_ai`, `scaffold_turret_ai`, `scaffold_group_coordinator`, `scaffold_flying_ai` |
| Validation / lint | 9 | `batch_validate_ai_assets`, `validate_ai_controller`, `get_ai_overview`, `list_ai_node_types`, `search_ai_assets`, `validate_ai_data_flow`, `find_eqs_references`, `find_so_references`, `lint_behavior_tree`, `lint_state_tree`, `detect_ai_circular_references`, `export_ai_manifest`, `get_ai_behavior_summary` |
| Mass Entity | 8 | `list_mass_entity_configs`, `get_mass_entity_config`, `create_mass_entity_config`, `add_mass_trait`, `remove_mass_trait`, `list_mass_traits`, `list_mass_processors`, `validate_mass_entity_config`, `get_mass_entity_stats` |
| Zone Graph | 3 | `list_zone_graphs`, `query_zone_lanes`, `get_zone_lane_info` |

> **Phase J F15:** all BT-related actions now return `{ "error": "<code>", "detail": "<human>" }` instead of mixed prose. Update your error parsers.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithAI.md` for the deep dive — it's a long one.

---

## logicdriver

Logic Driver Pro state machines: graph CRUD, node configuration, runtime PIE control, scaffolds, dialogue, text graph extraction. **66 actions.** **Conditional on `#if WITH_LOGICDRIVER`** — requires the Logic Driver Pro marketplace plugin. Reflection-only (precompiled marketplace plugin).

> For full param schemas, call `monolith_discover("logicdriver")` at runtime.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| State Machine assets | 5 | `create_state_machine`, `get_state_machine`, `list_state_machines`, `delete_state_machine`, `duplicate_state_machine` |
| Node Blueprints | 3 | `create_node_blueprint`, `get_node_blueprint`, `list_node_blueprints` |
| Inspection | 5 | `get_sm_structure`, `get_node_details`, `get_node_connections`, `find_nodes_by_type`, `find_nodes_by_class`, `get_sm_statistics` |
| Graph CRUD | 11 | `add_state`, `add_transition`, `add_conduit`, `add_state_machine_node`, `add_any_state_node`, `remove_node`, `set_node_properties`, `set_initial_state`, `set_end_state`, `set_node_class`, `rename_node`, `move_node`, `auto_arrange_graph` |
| Configuration | 6 | `configure_state`, `configure_transition`, `configure_conduit`, `configure_state_machine_node`, `set_transition_condition`, `set_state_tags`, `get_exposed_properties`, `set_exposed_property` |
| Compile | 1 | `compile_state_machine` |
| Runtime PIE | 7 | `runtime_get_sm_state`, `runtime_start_sm`, `runtime_stop_sm`, `runtime_restart_sm`, `runtime_switch_state`, `runtime_evaluate_transitions`, `runtime_get_state_history` |
| Spec / import / export | 5 | `build_sm_from_spec`, `export_sm_spec`, `export_sm_json`, `import_sm_json`, `compare_state_machines` |
| Scaffolds | 7 | `scaffold_hello_world_sm`, `scaffold_weapon_sm`, `scaffold_horror_encounter_sm`, `scaffold_game_flow_sm`, `scaffold_dialogue_sm`, `scaffold_quest_sm`, `scaffold_interactable_sm` |
| Project scan | 4 | `get_sm_overview`, `validate_state_machine`, `find_sm_references`, `find_node_class_usages` |
| Visualization | 2 | `visualize_sm_as_text` (ASCII / Mermaid / DOT), `explain_state_machine` |
| Components | 3 | `get_sm_component_config`, `add_sm_component`, `configure_sm_component` |
| Dialogue | 2 | `get_text_graph_content`, `get_dialogue_flow` |

The crown jewel is `build_sm_from_spec` — create a complete state machine (states, transitions, conduits, nested SMs, initial/end markers) from a single JSON spec, then compile, in one call.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithLogicDriver.md`.

---

## audio

Sound Cue + MetaSound graph CRUD, attenuation/class/mix/submix/concurrency, batch ops, Sound Cue templates, perception bindings, and a small batch of test helpers. **86 actions.**

> For full param schemas, call `monolith_discover("audio")` at runtime. MetaSound graph actions are conditional on `#if WITH_METASOUND` — projects without MetaSound get Sound Cue + CRUD + batch actions but no MetaSound graph building.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| Sound assets CRUD | 15 | `create_sound_attenuation`, `get_attenuation_settings`, `set_attenuation_settings`, `create_sound_class`, `get_sound_class_properties`, `set_sound_class_properties`, `create_sound_mix`, `get_sound_mix_settings`, `set_sound_mix_settings`, `create_sound_concurrency`, `get_concurrency_settings`, `set_concurrency_settings`, `create_sound_submix`, `get_submix_properties`, `set_submix_properties` |
| Test helpers | 1 | `create_test_wave` *(NEW Phase J F18)* — procedurally synthesizes a 16-bit mono sine `USoundWave` for tests with zero asset deps |
| Asset listing / search | 4 | `list_audio_assets`, `search_audio_assets`, `get_sound_wave_info`, `get_audio_stats` |
| Hierarchy | 2 | `get_sound_class_hierarchy`, `get_submix_hierarchy` |
| References / unused | 4 | `find_audio_references`, `find_unused_audio`, `find_sounds_without_class`, `find_unattenuated_sounds` |
| Batch ops | 9 | `batch_assign_sound_class`, `batch_assign_attenuation`, `batch_set_compression`, `batch_set_submix`, `batch_set_concurrency`, `batch_set_looping`, `batch_set_virtualization`, `batch_rename_audio`, `batch_set_sound_wave_properties` |
| Templates | 1 | `apply_audio_template` |
| Sound Cue graph | 9 | `create_sound_cue`, `get_sound_cue_graph`, `add_sound_cue_node`, `remove_sound_cue_node`, `connect_sound_cue_nodes`, `set_sound_cue_first_node`, `set_sound_cue_node_property`, `list_sound_cue_node_types`, `find_sound_waves_in_cue`, `validate_sound_cue` |
| Sound Cue spec / templates | 8 | `build_sound_cue_from_spec`, `create_random_sound_cue`, `create_layered_sound_cue`, `create_looping_ambient_cue`, `create_distance_crossfade_cue`, `create_switch_sound_cue`, `duplicate_sound_cue`, `delete_audio_asset` |
| Preview | 3 | `preview_sound`, `stop_preview`, `get_sound_cue_duration` |
| Perception bindings | 4 | `bind_sound_to_perception`, `unbind_sound_from_perception`, `get_sound_perception_binding`, `list_perception_bound_sounds` |
| MetaSound assets | 3 | `create_metasound_source`, `create_metasound_patch`, `create_metasound_preset` |
| MetaSound graph | 12 | `add_metasound_node`, `remove_metasound_node`, `connect_metasound_nodes`, `disconnect_metasound_nodes`, `add_metasound_input`, `add_metasound_output`, `set_metasound_input_default`, `add_metasound_interface`, `get_metasound_graph`, `list_metasound_connections`, `add_metasound_variable`, `set_metasound_node_location` |
| MetaSound discovery | 6 | `list_available_metasound_nodes`, `get_metasound_node_info`, `find_metasound_node_inputs`, `find_metasound_node_outputs`, `get_metasound_input_names` |
| MetaSound spec / templates | 6 | `build_metasound_from_spec`, `create_oneshot_sfx`, `create_looping_ambient_metasound`, `create_synthesized_tone`, `create_interactive_metasound` |

### `audio.create_test_wave` · NEW in Phase J F18

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path` | string | **required** | Destination asset path under `/Game/` |
| `frequency_hz` | number | optional | Sine frequency (20.0 to 20000.0). Default: `440.0` |
| `duration_seconds` | number | optional | Clip length (0.05 to 5.0). Default: `0.5` |
| `sample_rate` | integer | optional | Allowlist `{22050, 44100, 48000}`. Default: `44100` |
| `amplitude` | number | optional | Peak amplitude in `(0.0, 1.0]`. Default: `0.5` |

### `audio.bind_sound_to_perception`

Stamp a `UMonolithSoundPerceptionUserData` onto a `USoundBase` (Cue / MetaSoundSource / Wave). Runtime `UWorldSubsystem` fires `AActor::MakeNoise` when this sound plays through a `UAudioComponent` owned by an actor.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | `USoundBase` asset path |
| `loudness` | number | optional | `FAINoiseEvent::Loudness` multiplier. Default: `1.0` |
| `max_range` | number | optional | Per-event max range in cm; `0` = use listener's `HearingRange`. Default: `0` |
| `tag` | string | optional | `FName` tag for downstream filtering |
| `sense_class` | string | optional | Sense class name (only `"Hearing"` supported in v1) |
| `enabled` | boolean | optional | Master switch. Default: `true` |
| `fire_on_fade_in` | boolean | optional | Also fire on `FadingIn`, not just `Playing`. Default: `true` |
| `require_owning_actor` | boolean | optional | Skip 2D / no-owner sounds. Default: `true` |

> **Phase J F11:** `loudness <= 0`, `max_range < 0`, and unknown `sense_class` values now reject up-front instead of writing junk userdata.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithAudio.md`.

---

<a id="sibling-plugins"></a>

## Sibling Plugins

Sibling plugins live **beside** Monolith (not inside it) and register their own namespaces into Monolith's MCP action registry at startup. They ship as **separate plugins with separate releases** — they are not bundled in the Monolith zip.

If you're building a sibling plugin yourself, read `Plugins/Monolith/Docs/SIBLING_PLUGIN_GUIDE.md` for the architectural pattern, build setup, and reflection requirements.

| Sibling plugin | Namespace | Actions | Status | Repo |
|---|---|---|---|---|
| External sibling plugin | Custom | Varies | Registers its own namespace at startup and ships through its own repo/channel. | Outside `Plugins/Monolith/` |

**Why these aren't in the in-tree count:** the in-tree 1271/16 figure counts only modules shipped inside the public `Monolith-vX.Y.Z.zip` release. Sibling plugins live in their own folders, ship via their own channels (or stay private), and may or may not be installed in any given consumer's project. Their absence is not a degraded state — Monolith is fully functional without them.

Private sibling bridges are intentionally omitted from the public API reference. Their action rosters, namespaces, and release notes belong in their own repos/channels; Monolith must not publish them as part of the public API surface.

---

<a id="pipelines"></a>

## Pipelines — Cross-Module Workflow Chains

Authoring complex assets typically requires calling a sequence of actions in order. The chains below are the canonical flows. Where a "build from spec" shortcut exists, prefer it — spec-based builders are transactional and handle validation, connection resolution, and rollback in a single call.

### Materials

```
create_material → build_material_graph → connect_expressions → recompile_material
```

**Shortcut:** `build_material_graph` accepts a `graph_spec` that can populate the entire graph (expressions + connections) in one call. Follow with `recompile_material`.

### State Machines (LogicDriver)

```
create_state_machine → add_state (×N) → add_transition (×N) → compile_state_machine
```

**Shortcut:** `build_sm_from_spec` builds the whole graph and compiles in a single call.

### Sound Cues

```
create_sound_cue → add_sound_cue_node (×N) → connect_sound_cue_nodes (×N) → set_sound_cue_first_node
```

**Shortcuts:** `build_sound_cue_from_spec` for arbitrary graphs; `create_random_sound_cue`, `create_layered_sound_cue`, `create_looping_ambient_cue`, `create_distance_crossfade_cue`, `create_switch_sound_cue` for canonical templates.

### MetaSounds

```
create_metasound_source → add_metasound_input/output → add_metasound_node (×N) → connect_metasound_nodes (×N)
```

**Shortcut:** `build_metasound_from_spec` does it all in one call (interfaces, inputs, outputs, nodes, connections).

### Behavior Trees

```
create_blackboard → add_bb_key (×N) → create_behavior_tree → set_bt_blackboard → add_bt_node (×N) → add_bt_decorator/service (×N)
```

**Shortcut:** `build_behavior_tree_from_spec` and the AI scaffold library (`scaffold_enemy_ai`, `scaffold_horror_stalker`, etc.) are usually the right entry points.

### State Trees

```
create_state_tree → set_st_schema → add_st_state (×N) → add_st_task (×N) → add_st_transition (×N) → compile_state_tree
```

**Shortcut:** `build_state_tree_from_spec`.

### Gameplay Abilities (GAS)

```
bootstrap_gas_foundation → create_attribute_set → add_attribute → create_gameplay_effect → add_modifier → create_ability → set_ability_tags → set_ability_triggers → compile_ability
```

**Shortcuts:** `scaffold_gas_project`, `scaffold_damage_pipeline`, `scaffold_status_effect`, `scaffold_weapon_ability`, `build_ability_from_spec`, `build_effect_from_spec`.

### Town Generation (experimental)

```
generate_floor_plan → create_building_from_grid → generate_facade → generate_roof → furnish_room → validate_building
```

> **Experimental.** `bEnableProceduralTownGen=true` required. Known geometry issues remain.

---

## Discovery Pattern (use this first)

Before writing any client code:

1. `monolith_discover()` — list all namespaces and their actions.
2. `monolith_discover("<namespace>")` — get full param schemas for one namespace.
3. `project_query("search", {query: "..."})` — find assets by name/type.
4. `source_query("search_source", {query: "..."})` — verify UE 5.7 API signatures.

**Golden rule:** never fabricate action names. The cogitator will be displeased.

---

## Offline Fallback (editor not running)

When the editor is closed but you still need to query Monolith:

- **`Plugins/Monolith/Binaries/monolith_query.exe`** — standalone C++ tool, read-only. Same actions for read-only namespaces (project, source, config).
- **`python Plugins/Monolith/Saved/monolith_offline.py`** — same actions, stdlib-only.

Both invoke the same SQLite indexes the live MCP uses.

---

## Transport Notes

- Claude Code's MCP transport is `"http"`, not `"streamableHttp"`.
- Some clients serialize nested `params` objects to a JSON **string** instead of a nested object — detect and deserialize back.
- The HTTP server lives on `http://localhost:<port>`. Port is published in `monolith_status` output.
- For Claude Code specifically, the **MCP auto-reconnect proxy** at `Scripts/monolith_proxy.py` survives editor restarts. See `Plugins/Monolith/Docs/Installation.md` for setup.

---

## Conditional Module Gating Reference

| Module | Gate | Actions when ungated |
|--------|------|----------------------|
| MonolithGAS | `WITH_GBA` (GameplayAbilities plugin) | 0 |
| MonolithComboGraph | `WITH_COMBOGRAPH` (ComboGraph marketplace plugin) | 0 |
| MonolithLogicDriver | `WITH_LOGICDRIVER` (Logic Driver Pro marketplace plugin) | 0 |
| MonolithAI | `WITH_STATETREE` + `WITH_SMARTOBJECTS` (engine plugins) | 0 |
| MonolithUI CommonUI | `WITH_COMMONUI` | 42 (UMG baseline only) |
| MonolithAudio MetaSound | `WITH_METASOUND` | Sound Cue + CRUD + batch (no MetaSound graph) |
| MonolithMesh town gen | `bEnableProceduralTownGen` (Editor Preferences, default `false`) | 195 (core mesh only) |

---

## Cross-References

- **SPEC_CORE.md** — Master Monolith spec, action count audit, pipelines, architecture
- **SPEC_Monolith*.md** — Per-module deep specs in `Plugins/Monolith/Docs/specs/`
- **SIBLING_PLUGIN_GUIDE.md** — How to build a sibling plugin against `FMonolithToolRegistry`
- **CHANGELOG.md** — Release-by-release change history (Keep a Changelog format)
- **Wiki** — User-facing tutorials at `https://github.com/tumourlove/monolith/wiki`

---

*The Omnissiah's blessing be upon every action call. May your discovery never return an empty array.*
