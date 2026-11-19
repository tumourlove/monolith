# Tool Reference

Monolith exposes a small set of namespace-dispatch MCP tools instead of ~1,500+ individual tools. Call `monolith_discover()` to list available actions, then use `{namespace}_query(action, params)` to invoke them. `monolith_discover` is terse by default — it returns each action's name plus a one-line description, not the full per-action param schema. Fetch a single action's full param schema on demand with `describe_query("action_schema")`, or pass `detail=true` (alias `verbose=true`) to `monolith_discover` to get the full schemas inline. New AI clients can also call `monolith_guide()` to self-onboard.

> **In-tree action count is approximate: ~1,500+ actions across 27 in-tree namespaces** (public, in-tree only; all active by default, plus +45 experimental town-gen actions when `bEnableProceduralTownGen=true`). The per-namespace numbers in the table below are kept for structure, not precision — query `monolith_discover()` (its `total_actions` field) for the exact live figure at any moment.

## Namespace Overview

| Namespace | Tool | Actions | Domain |
|-----------|------|---------|--------|
| `monolith` | `monolith_discover` | — | Discovery and introspection |
| `monolith` | `monolith_status` | — | Server health, version, index status |
| `monolith` | `monolith_reindex` | — | Trigger full project re-index |
| `monolith` | `monolith_update` | — | Check or install updates |
| `monolith` | `monolith_guide` | — | Section-keyed onboarding guide for your AI (onboarding / recipes / decisions / errors / skills_map / gotchas) with a live registry overlay |
| `blueprint` | `blueprint_query` | 112+ | Full Blueprint lifecycle — CRUD, node graphs, bulk ops, auto-layout, compile, validation, dataset read/edit pack (DataTable/CurveTable/StringTable + `seed_data_asset`), cross-class property access, parent-function overrides, `audit_cdo_drift` (Phase 4a). **v0.19.0:** `find_variable_references` (variable read/write census) + variable-contract reconciliation (`compare_class_variable_contract` / `promote_variables_to_parent`) |
| `material` | `material_query` | 64 | Materials, instances, functions, graph building, HLSL, texture import, previews, tiling quality, batch ops, `audit_orphan_materials` (Phase 4a) |
| `animation` | `animation_query` | 145+ | Sequences, montages, blend spaces (incl. baking + interpolation control), ABP state machines + structural writes (incl. custom anim-graph nodes, state/transition/entry teardown, compound expression transition rules), PoseSearch, IK Rigs (incl. solver removal), Control Rig + structural writes, skeleton management, layout. **v0.19.0:** anim-node function + pin binding read/write (`get`/`set_anim_node_function_bindings` / `get`/`set_anim_node_pin_binding(s)`) and time-series PIE sampling (`sample_pie_timeseries`). Also an AnimGraph-authoring pack (14 — `add_apply_additive` / `add_apply_mesh_space_additive` / `add_slot_node` / `add_save_cached_pose` / `add_use_cached_pose` / `set_output_pose_source` / `set_state_result_source` / `add_blend_by_int` / `add_blend_by_enum` / `set_sync_group` / `set_layered_blend_bones` / `add_anim_control_rig_node` / `add_linked_anim_layer` / `add_conduit`), `set_transition_rule` gained an `expression` kind (compound multi-term conditions), `set_anim_node_pin_binding` now bootstraps the binding object on unbound nodes, `auto_layout` gained a Blueprint-Assist-free `builtin` formatter, plus `bake_blend_space` / `set_blend_space_interpolation`, `remove_anim_state` / `set_anim_entry_state` / `remove_anim_transition`, `remove_ik_solver`, and `add_anim_layer_graph` (ABP-native animation layer graphs — no `UAnimLayerInterface` asset required; `add_linked_anim_layer` resolves them as self layers) |
| `niagara` | `niagara_query` | 129 | Particle systems, module stacks, dynamic inputs, HLSL modules, direct HLSL-node read/write (`get_custom_hlsl_text` / `set_custom_hlsl_text`, v0.18.0 PR #65), sim-stage + event-handler selectors, event handlers, sim stages, renderers, scalability, auto_layout, event-aware summaries + `validate_system` event-chain reasoning, temporal-control composite writers + read aggregators, stateless-emitter factory, search & discovery pack (`search_by_parameter` / `query_niagara` / `find_similar_systems` / …), `audit_cross_asset_refs` |
| `mesh` | `mesh_query` | 194 (+45) | Mesh inspection, scene manipulation, spatial queries, blockout, GeometryScript, horror analysis, lighting, audio, performance, procedural geometry, encounter design, mesh import (incl. skeletal + animation). Town gen 45 actions registered only when `bEnableProceduralTownGen=true` (not in the public count) |
| `ai` | `ai_query` | 221 | BT, BB, State Trees, EQS, Smart Objects, Controllers, Perception, Navigation, runtime debugging, scaffolding. Conditional on `WITH_STATETREE` + `WITH_SMARTOBJECTS` (Phase J F22) |
| `gas` | `gas_query` | 135 | Gameplay Ability System — abilities, effects, attributes (incl. a baseline vitals AttributeSet template), ASC, tags, cues, targeting, input, inspect, scaffold. Conditional on `WITH_GBA` for Blueprint AttributeSets |
| `logicdriver` | `logicdriver_query` | 66 | Logic Driver Pro state machines — SM CRUD, graph read/write, JSON spec, scaffolding, components. Conditional on `WITH_LOGICDRIVER` |
| `combograph` | `combograph_query` | 13 | ComboGraph combo trees — graph CRUD, nodes, edges, effects, cues, ability scaffolding. Conditional on `WITH_COMBOGRAPH` |
| `audio` | `audio_query` | 98 | Sound asset CRUD, Sound Cue + MetaSound graph building (Builder API write-side), MetaSound document introspection (read-side, v0.14.10 +12 from PR #18 by @alakangas), batch ops, audio health checks, templates, sine test wave (Phase J F18), AI perception binding. MetaSound features conditional on `WITH_METASOUND` |
| `ui` | `ui_query` | 138 | UMG widget CRUD + Animation v1+v2 + EffectSurface + Spec Builder + Type Registry + hoisted Design Import + CommonUI activatable widgets/styles/input (51, conditional on `WITH_COMMONUI`) + headline scaffolders + navigation/conversion gap-closure + 4 GAS attribute-binding aliases + UMG MVVM (8, v0.25.0, only when the UMG Viewmodel plugin is enabled in a source build) |
| `editor` | `editor_query` | 33+ | Build/compile triggers (`get_build_errors` now appends deterministic `fix_hints[]`, v0.19.0), log ring buffer, crash context, scene capture (incl. v0.16.0 `static_mesh` / `skeletal_mesh` / `widget` asset types), material-grid + debug-overlay capture, PBR + texture-channel inspection, GIF capture, texture import, blank-map factory + module status (Phase J F8), automation test list/run (v0.14.8 PR #48 by @MaxenceEpitech), Python escape-hatch + map swap (v0.14.9 Issue #50 ported from @JCSopko's fork). **v0.19.0:** live-PIE introspection/driving (`pie_get_object_properties` / `pie_call_function` / `pie_set_control_rotation` / `pie_inject_input_action` / `pie_possess_spectator_free`) + programmatic stat-group readout (`get_stat_group_values`) |
| `config` | `config_query` | 6 | INI resolution with multi-layer breakdown, diff, search |
| `project` | `project_query` | 8+ | FTS5 full-text search across all indexed assets including marketplace plugins, `audit_orphan_assets` (Phase 4a). **v0.19.0:** `export_asset_text` (T3D native-text export with object/grep filters) |
| `source` | `source_query` | 20 | 1M+ C++ symbols — source reading, call graphs, class hierarchy, no Python required. Auto-reindex on hot-reload (Phase J F17), `audit_module_dep_reality` (Phase 2). **v0.19.0:** +8 LLM C++ ergonomics lookups (`get_include_path` / `get_signature` / `check_deprecations` / `verify_symbols` / `find_example_usage` / `suggest_build_cs_deps` / `lint_header` / `generate_class_stub`) and an allman-brace parser fix that tripled the symbol index |
| `level_sequence` | `level_sequence_query` | 8 | Cinematic / sequencer asset authoring — create sequences, manage tracks/shots/bindings (incl. UE 5.7 custom-binding awareness), dedicated SQLite indexer (v0.14.8 PR #45 by @yashabogdanoff) |
| `gameplay_tags` | `gameplay_tags_query` | 14 | **New v0.25.0.** Tag references from the Asset Registry (no reindex), unused/broken audits, ini tag add/update/remove/rename/move. Gated by `bEnableGameplayTags`, independent of GAS |
| `input` | `input_query` | 15 | **New v0.23.0.** Enhanced Input — `UInputAction` / `UInputMappingContext` inspection and authoring. Works with GAS switched off |
| `localization` | `localization_query` | 4 | **New v0.23.0.** Read-only culture discovery and StringTable inspection |
| `chooser` | `chooser_query` | 16 | Chooser Table inspection (6 read-only, ungated) and authoring (10, need the Chooser plugin) |
| `bulk_fill` | `bulk_fill_query` | 2 | Reflection-walker bulk property fill across 12 per-namespace adapters — `apply` (dry-run-able tree write), `list_namespaces` |
| `describe` | `describe_query` | 3 | Read-only schema introspection for the same 12 adapters — `schema`, `list_targets`, `action_schema` (any registered action's full param schema) |
| `decision` | `decision_query` | 5 | **New v0.17.0.** Architectural decision-record harvest from markdown sources (specs, plans, CHANGELOG, `.claude/rules/`). Three heuristic tiers (YAML frontmatter 0.90, ADR headers 0.85, header + rationale paragraph 0.65). Deterministic — zero LLM calls. |
| `risk` | `risk_query` | 5 | **New v0.17.0.** Repo-level risk signals — per-file churn, co-change pairs, blended hotspot scores (`0.6 * churn + 0.4 * loc`), release-window thresholds, conditional gates (`#if WITH_*` macros + `bHas*` probes + `MONOLITH_RELEASE_BUILD` bypasses). Mines git via `FPlatformProcess::CreateProc`. |
| `cppreflect` | `cppreflect_query` | 6 | **New v0.17.0.** UE 5.7 reflection-edge queries from UHT artefacts (`Intermediate/Build/.../Inc/<Module>/UHT/*.gen.cpp`) cross-joined with `IAssetRegistry::GetDependencies`. UCLASSes, UPROPERTYs, UFUNCTIONs, UINTERFACEs, interface impls, plus `cpp_asset_edges` mapping assets → C++ classes, plus specifier discovery. No tree-sitter dep. |
| `network` | `network_query` | 4 | **New v0.17.0.** UE 5.7 replication inspection — replicated classes (captures bare `UPROPERTY(Replicated)` via `CPF_Net` as of v0.19.0, verified E2E), RPC functions by specifier (covers project plugins by default, returns the project's project-plugin Server RPCs), OnRep handlers paired with their properties, audit for unbalanced `ReplicatedUsing=OnRep_X` declarations whose handler doesn't exist. The reflection indexers scan project plugins by default; marketplace plugins are gated behind a setting. |
| `pipeline` | `pipeline_query` | 2 | **New v0.17.0.** Read-only composer actions — `pr_review` (per-changed-file risk + decision + module-dep audit + CDO drift), `release_readiness` (status + stale decisions + release-window hotspots + sentinel + CHANGELOG audit). Both fan out other read-only actions serially. |
| `reflect` | `reflect_query` | 1 | **New v0.19.0.** Index maintenance — `rebuild_reflection_index` (no params), a project-only force-rebuild of the RI reflection tables (`reflect_uclasses` / `reflect_uproperties` / `reflect_ufunctions` / `reflect_uinterfaces` / `reflect_uinterface_impls` / `cpp_asset_edges` + `reflect_replicated_properties`). WRITE/maintenance, idempotent, non-destructive. Exists because after an indexer code change there's no other clean repopulation trigger. |

---

## MonolithCore (monolith meta 5 + bulk_fill 2 + describe 3 actions)

| Action | Description |
|--------|-------------|
| `discover` | List all namespaces and their actions (terse: names + one-line descriptions) |
| `discover(namespace)` | List actions for a specific namespace (terse by default — see params below) |
| `status` | Server health, version, action count, engine info |
| `update(check/install)` | Check for or install updates from GitHub Releases |
| `reindex` | Trigger full project re-index |
| `guide` | Section-keyed onboarding guide for your AI — `guide(section=...)` to pull one of `onboarding` / `recipes` / `decisions` / `errors` / `skills_map` / `gotchas`. Hand-authored markdown + a live registry overlay so counts match the running build (new in 0.15.0) |

#### `discover(namespace)` parameters

`monolith_discover` is **terse by default**: for a namespace it returns each action's name and a one-line description (first sentence, else hard-capped at 150 chars on a word boundary with a `...` suffix), not the full param schema. The response always carries a `total` field and, in terse mode, a `schema_hint` pointing at the lazy-fetch path. To get a single action's full param schema, call `describe_query("action_schema")`.

| Param | Type | Default | Effect |
|-------|------|---------|--------|
| `detail` | bool | `false` | `true` emits the full per-action param schemas inline (reproduces the pre-change shape) |
| `verbose` | bool | `false` | Alias for `detail` |
| `filter` | string | — | Case-insensitive substring match on action name OR full description |
| `offset` | int | `0` | Skip the first N actions (pagination opt-in) |
| `limit` | int | `0` | Max actions to return; `0` = ALL. A positive `limit` with more remaining adds `next_offset` to the response |

By default (no `limit`) discover returns the COMPLETE action list. `discover()` with no namespace is unchanged.

### Bulk-fill + describe ergonomics framework (new in 0.15.0)

Two framework namespaces also live in MonolithCore. They route through a reflection walker to 12 per-namespace adapters (blueprint, gas, ui, ai, niagara, material, audio, mesh, animation, logicdriver, combograph, plus one extension-plugin adapter), so your AI can populate or inspect deeply-nested property trees with one mental model.

| Action | Description |
|--------|-------------|
| `bulk_fill_query("apply")` | Write a nested property tree to a target. `dry_run:true` returns a per-field report (field writes, silent drops, clamps, errors) before persisting; `strict` controls drop-vs-error behaviour |
| `bulk_fill_query("list_namespaces")` | List the registered bulk-fill adapter namespaces |
| `describe_query("schema")` | Return the recursive schema descriptor for a target (type names, ImportText sample forms, ranges, enum values, conditional discriminators) |
| `describe_query("list_targets")` | List the describe-able targets for a namespace |
| `describe_query("action_schema")` | Return any registered action's full param schema by `target_namespace` + `action` — names, types, required, defaults, aliases, descriptions |

## MonolithBlueprint (111 actions)

Complete Blueprint lifecycle management. Create Blueprints from any parent class, then build them out entirely through MCP — add variables (including replicated with RepNotify), components with hierarchy, functions with full signatures, macros, event dispatchers. Wire up node graphs with typed pins, set defaults, connect execution flow. Bulk operations let you add dozens of nodes and wire them in a single call. Sugiyama auto-layout cleans up messy graphs. Interface scaffolding auto-generates stub event nodes for unimplemented interfaces. CDO property reading gives reflection-level access to any UPROPERTY default. Validation catches unused variables, disconnected nodes, duplicate events, and unimplemented interfaces without triggering a full compile.

### Read (17)

| Action | Description |
|--------|-------------|
| `list_graphs` | Event graphs, function graphs, macros |
| `get_graph_summary` | Lightweight graph overview — id/class/title + exec connections only (~10KB vs 172KB full) |
| `get_graph_data` | Full graph topology with pin serialization. Optional `node_class_filter` param |
| `get_variables` | Variables with types, containers, replication flags, CDO default values |
| `get_execution_flow` | Execution trace through branching nodes |
| `search_nodes` | Case-insensitive node search |
| `search_functions` | Search functions and events by name across a Blueprint |
| `get_components` | Component hierarchy — names, classes, parent-child tree |
| `get_component_details` | Full property reflection for a specific component |
| `get_functions` | Functions with inputs, outputs, metadata, local vars |
| `get_function_signature` | Return param list and return type for a Blueprint function |
| `get_event_dispatchers` | Event dispatchers with parameter signatures |
| `get_event_dispatcher_details` | Full detail for a single event dispatcher |
| `get_parent_class` | Parent class, blueprint type, status, capabilities |
| `get_interfaces` | Implemented interfaces (direct + inherited) |
| `get_construction_script` | Construction script graph data |
| `get_blueprint_info` | Comprehensive Blueprint summary (class, interfaces, components, variable count) |

### Variable CRUD (9)

| Action | Description |
|--------|-------------|
| `add_variable` | Add member variable with type, default, category, flags |
| `remove_variable` | Remove member variable |
| `rename_variable` | Rename variable (updates all references) |
| `set_variable_type` | Change variable type |
| `set_variable_defaults` | Update variable metadata and flags |
| `add_local_variable` | Add local variable to a function |
| `remove_local_variable` | Remove local variable from a function |
| `add_replicated_variable` | Add a replicated variable with configurable RepNotify |
| `promote_pin_to_variable` | Promote a pin's value to a Blueprint variable |

### Component CRUD (6)

| Action | Description |
|--------|-------------|
| `add_component` | Add SCS component with optional parent/socket |
| `remove_component` | Remove component (optional promote children) |
| `rename_component` | Rename component variable |
| `reparent_component` | Move component in hierarchy |
| `set_component_property` | Set property via reflection |
| `duplicate_component` | Duplicate a component |

### Graph Management (12)

| Action | Description |
|--------|-------------|
| `add_function` | Create function graph with purity, const, static, access flags |
| `remove_function` | Remove function graph |
| `rename_function` | Rename function graph |
| `add_macro` | Create macro graph |
| `add_event_dispatcher` | Create event dispatcher |
| `remove_event_dispatcher` | Remove an event dispatcher from a Blueprint |
| `set_event_dispatcher_params` | Change the parameter signature of an event dispatcher |
| `set_function_params` | Set function input/output parameters |
| `implement_interface` | Add interface to Blueprint |
| `remove_interface` | Remove interface (optional preserve functions) |
| `scaffold_interface_implementation` | Auto-generate stub event nodes for an unimplemented interface |
| `reparent_blueprint` | Change Blueprint parent class |

### Node & Pin Operations (14)

| Action | Description |
|--------|-------------|
| `add_node` | Add node (CallFunction, VariableGet/Set, CustomEvent, Branch, Sequence, MacroInstance, SpawnActorFromClass, CastTo, Timeline, Event, Comment) |
| `remove_node` | Remove a node from graph |
| `connect_pins` | Wire two pins together |
| `disconnect_pins` | Break pin connections |
| `set_pin_default` | Set pin default value |
| `set_node_position` | Move node to [x, y] position |
| `get_node_details` | Full detail dump for a single node (pins, defaults, metadata) |
| `resolve_node` | Resolve a node reference to its target (function, variable, etc.) |
| `add_nodes_bulk` | Add multiple nodes to a graph in one call |
| `connect_pins_bulk` | Connect multiple pin pairs in one call |
| `set_pin_defaults_bulk` | Set multiple pin default values in one call |
| `add_timeline` | Add a Timeline node to a graph |
| `add_event_node` | Add a named event node to a graph |
| `add_comment_node` | Add a comment box to a graph |

### Compile & Create (5)

| Action | Description |
|--------|-------------|
| `compile_blueprint` | Full compile — returns errors, warnings, status |
| `validate_blueprint` | Lint without compiling — unused vars, disconnected nodes, unimplemented interfaces, duplicate events |
| `create_blueprint` | Create new Blueprint asset with parent class |
| `duplicate_blueprint` | Duplicate Blueprint to new path |
| `get_dependencies` | Asset dependencies (depends_on/referenced_by/both) |

### Inspection & Bulk (3)

| Action | Description |
|--------|-------------|
| `get_cdo_properties` | Read `UPROPERTY` defaults from any Blueprint CDO or `UObject` asset |
| `batch_execute` | Dispatch multiple Blueprint operations in a single call |
| `search_functions` | Search functions and events by name across a Blueprint |

### Variable census + contract reconciliation (v0.19.0)

| Action | Description |
|--------|-------------|
| `find_variable_references` | Find every graph node that reads or writes a member variable (event graphs, functions, macros, delegate signatures, interface graphs, thread-safe Property Access nodes). Per match: `graph`, `node_id`, `node_title`, `access` (`read`/`write`/`other`), plus a `summary` count block. `include_inherited` matches parent-scoped variables too |
| `compare_class_variable_contract` | Pure read/report diff of two classes' variable contracts (BP path or native class name per side) — base type, container kind, enum/struct subtype, object class, and a `mismatch` classification. Catches enum-subtype mismatches the KismetCompiler hides and BP-locals shadowing a native parent property |
| `promote_variables_to_parent` | Reconcile a Blueprint's named local variables against its native parent. `verify` (default) reports parity; `remove_shadowed` deletes redundant BP-local duplicates that pass parity. Authors no C++ |

## MonolithMaterial (63 actions)

Full material authoring pipeline. Create materials, material instances, and material functions from scratch. Build complete material graphs from a declarative JSON spec — one call to define expressions, wire connections, and set properties. Or work incrementally: add expressions, connect them, set properties, recompile. Custom HLSL expression nodes with typed inputs/outputs. Material instances with full parameter override lifecycle — scalar, vector, texture, static switch. Import textures from disk directly into UE assets. Render material previews to PNG or base64. Validate graphs for islands, broken references, and duplicate parameters. Export/import graphs as JSON for templating, migration, or version control. Batch operations across multiple materials. Compilation stats give you shader instruction counts and sampler usage.

### Inspection (10)

| Action | Description |
|--------|-------------|
| `get_all_expressions` | All expression nodes in a material |
| `get_expression_details` | Detailed info for a specific expression |
| `get_expression_connections` | All connections into/out of a specific expression |
| `get_expression_pin_info` | Pin names, types, and connection state for an expression |
| `get_full_connection_graph` | All wire connections |
| `get_material_parameters` | List all parameters (scalar, vector, texture, static switch) |
| `get_material_properties` | Material-level properties (blend mode, shading model, two-sided, etc.) |
| `get_compilation_stats` | Vertex + pixel shader instruction counts, sampler usage, blend mode, compile status |
| `get_layer_info` | Material Layer and Blend info |
| `list_expression_classes` | List all available material expression class names |

### Editing (14)

| Action | Description |
|--------|-------------|
| `connect_expressions` | Wire expressions together or to material outputs |
| `disconnect_expression` | Disconnect input or output |
| `build_material_graph` | Build a complete material from JSON spec |
| `set_expression_property` | Set properties on expression nodes |
| `move_expression` | Move an expression node to a new graph position |
| `rename_expression` | Rename an expression node's parameter name |
| `duplicate_expression` | Duplicate an expression node in-place |
| `replace_expression` | Swap an expression node for a different type, preserving connections |
| `delete_expression` | Delete an expression node by name |
| `create_custom_hlsl_node` | Custom HLSL expression with inputs/outputs |
| `update_custom_hlsl_node` | Update the HLSL code or description on a CustomHLSL expression |
| `auto_layout` | Auto-arrange expression nodes in the material graph |
| `begin_transaction` / `end_transaction` | Undo-friendly transactions |
| `set_material_property` | Change material properties (blend mode, shading model, etc.) |

### Create, Import & Export (8)

| Action | Description |
|--------|-------------|
| `create_material` | Create new material at specified path |
| `duplicate_material` | Duplicate material to new asset path |
| `save_material` | Explicitly save a material asset (bypass auto-save) |
| `recompile_material` | Force material recompilation |
| `import_texture` | Import an image file as a UTexture2D asset |
| `export_material_graph` | JSON export. Optional `include_properties`, `include_positions` params |
| `import_material_graph` | JSON import with "overwrite" or "merge" modes |
| `validate_material` | Check for islands, broken refs, duplicate params |

### Material Instances (6)

| Action | Description |
|--------|-------------|
| `create_material_instance` | Create material instance with parent and parameter overrides |
| `get_instance_parameters` | List all parameter overrides on a material instance |
| `set_instance_parameter` | Set a single parameter value on a material instance |
| `set_instance_parameters` | Set multiple parameters on a material instance in one call |
| `set_instance_parent` | Reparent a material instance to a different material |
| `clear_instance_parameter` | Clear a parameter override (revert to parent value) |
| `list_material_instances` | Find all material instances derived from a material |

### Material Functions (3)

| Action | Description |
|--------|-------------|
| `create_material_function` | Create a new UMaterialFunction asset |
| `build_function_graph` | Build a material function's node graph from a declarative spec |
| `get_function_info` | Return inputs, outputs, and description of a material function |

### Previews (2)

| Action | Description |
|--------|-------------|
| `render_preview` | PNG preview to disk |
| `get_thumbnail` | Base64 PNG or save to file (`save_to_file` param) |

### Bulk Operations (2)

| Action | Description |
|--------|-------------|
| `batch_set_material_property` | Set a property on multiple materials in one call |
| `batch_recompile` | Recompile multiple materials in one call |

## MonolithAnimation (145 actions)

The most comprehensive animation tooling available through any MCP server. Covers the entire UE animation pipeline — from raw sequences and bone-level key editing through montages, blend spaces, and composites, all the way up to Animation Blueprint state machines with structural writes. PoseSearch integration for motion matching workflows. IK Rig and Retargeter configuration. Control Rig hierarchy manipulation with structural writes. Skeleton management with socket CRUD, virtual bones, and curve editing. Notify systems with track management and duration control. This isn't read-only inspection — you can add states to ABP state machines, wire transitions, set transition rule expressions, add and connect anim graph nodes, and set state animations programmatically. 5 ABP write actions + 3 ControlRig write actions + 1 layout action ride alongside the 96 baseline + 13 PoseSearch entries.

| Action | Description |
|--------|-------------|
| `add/delete/set_section_*` | Montage section management |
| `add/edit/delete_blendspace_sample` | Blend space sample points |
| `get_state_machines` / `get_state_info` / `get_transitions` | ABP state machine inspection (conduit support, from_type/to_type) |
| `get_blend_nodes` / `get_linked_layers` / `get_graphs` / `get_nodes` | ABP graph reading. `get_nodes` accepts optional `graph_name` filter |
| `set_notify_time` / `set_notify_duration` | Animation notify editing |
| `add/remove_bone_track` / `set_bone_track_keys` | Bone track editing |
| `add/remove_virtual_bones` | Virtual bone management |
| `get_skeleton_info` / `get_skeletal_mesh_info` | Skeleton and mesh inspection |
| `get_sequence_info` | Duration, frames, sample rate, root motion, additive, compression |
| `get_sequence_notifies` | All notifies with name, time, duration, class, track |
| `get_bone_track_keys` | Position/rotation/scale key arrays for a bone |
| `get_sequence_curves` | Float/transform curves with key counts |
| `get_montage_info` | Sections, slots, blend in/out, rate scale |
| `get_blend_space_info` | Samples + axis config (min/max/name/grid) |
| `get_skeleton_sockets` | Socket name, bone, location/rotation/scale |
| `get_abp_info` | Skeleton, parent class, state machines, graphs, variables |
| `add/remove_notify` / `add_notify_state` / `set_notify_track` | Notify CRUD operations |
| `list_curves` / `add/remove_curve` / `set_curve_keys` / `get_curve_keys` | Animation curve CRUD |
| `add/remove_socket` / `set_socket_transform` | Skeleton socket management |
| `get_skeleton_curves` | Curves on a skeleton asset |
| `set_blend_space_axis` | Modify blend space axis min/max |
| `set_root_motion_settings` | Toggle root motion on/off |
| `create_sequence` / `duplicate_sequence` | Sequence creation and duplication |
| `create_montage` / `set_montage_blend` / `add/set_montage_slot` | Montage creation and configuration |
| `apply_anim_modifier` / `list_anim_modifiers` | Animation modifier management |
| `get_composite_info` / `add/remove_composite_segment` | AnimComposite inspection and editing |
| `get_abp_variables` | Variables defined in an Animation Blueprint (with types and defaults) |
| `get_abp_linked_assets` | Assets linked to an Animation Blueprint (skeletons, rigs, etc.) |
| `add_state_to_machine` | Add a new state to an AnimBP state machine |
| `add_transition` | Add a transition between two states in a state machine (accepts names or indices) |
| `set_transition_rule` | Set the condition expression on a state machine transition |

### Blend space baking + state-machine teardown + IK solver removal

| Action | Description |
|--------|-------------|
| `bake_blend_space` | Rebuild a blend space's triangulation (`FBlendSpaceData`) via `ResampleData()` and mark it dirty — repairs blend spaces authored externally or before this release's auto-bake fix. Returns `has_blendspace_data`, `sample_count`, `baked`, and a `warning` when a 2D blend space has fewer than 3 samples. Works on 1D and 2D blend spaces |
| `set_blend_space_interpolation` | Set input-interpolation settings (`use_grid` toggles `bInterpolateUsingGrid`; `preferred_triangulation_direction` of `None`/`Tangential`/`Radial`), then resample + dirty. In grid mode the triangulation is intentionally empty, so `has_blendspace_data` is `false` — correct, not a failure |
| `remove_anim_state` | Remove a state from a state machine and tear down its inner anim graph. `remove_dependent_transitions` (default `true`) also sweeps transitions that referenced the state. Refuses to remove the machine's current entry state — re-point with `set_anim_entry_state` first |
| `set_anim_entry_state` | Re-point a state machine's Entry node at an existing state. Returns the previous entry target; reports `unchanged` when the named state is already the entry |
| `remove_anim_transition` | Remove the transition from one state to another (`from_state` → `to_state`). Reports `matched_transition_count` |
| `remove_ik_solver` | Remove a solver from an IK Rig's solver stack by 0-based `solver_index`, validated against the current solver count (out-of-range returns a clear error naming the valid range). Returns `removed_index` and `solver_count_after` |

> **Blend space auto-bake fix.** The four blend space mutators — `add_blendspace_sample`, `edit_blendspace_sample`, `delete_blendspace_sample`, `set_blend_space_axis` — now call `ResampleData()` after each edit and mark the package dirty, so MCP-authored blend spaces no longer ship with empty triangulation and play the bind/reference pose at runtime. Repair existing broken blend spaces in place with `bake_blend_space`.

### AnimGraph-authoring pack (14)

| Action | Description |
|--------|-------------|
| `add_apply_additive` | Add an Apply Additive node (Base / Additive / Alpha pins) |
| `add_apply_mesh_space_additive` | Add an Apply Mesh-Space Additive node |
| `add_slot_node` | Add a Slot node; validates the slot name against the skeleton's slot groups |
| `add_save_cached_pose` | Add a Save Cached Pose node (`cache_name`) |
| `add_use_cached_pose` | Add a Use Cached Pose node (`cache_name`; pairs by name with the matching save) |
| `set_output_pose_source` | Wire a node's pose output into the anim graph's Output / Root result pin |
| `set_state_result_source` | Wire a node's pose output into a state machine state's result pin |
| `add_blend_by_int` | Add a Blend Poses by Int node grown to `num_poses` pose pins |
| `add_blend_by_enum` | Add a Blend Poses by Enum node bound to a `UEnum` (`enum_path`): one pose pin per exposed enumerator plus a Default/else pin, skipping the auto `_MAX` sentinel and `Hidden` enumerators. Optional `enumerators` exposes a subset; optional `graph_name` / `state_name` target a specific graph or state |
| `set_sync_group` | Set a player node's sync group name / role / method |
| `set_layered_blend_bones` | Set per-bone branch filters (bone + blend depth) on a Layered Blend Per Bone node |
| `add_anim_control_rig_node` | Add a Control Rig anim-graph node (`control_rig_class`; IO pins regenerate) |
| `add_linked_anim_layer` | Add a Linked Anim Layer node (`layer_name`, optional `interface_class` / `instance_class`). Resolves interface-declared layers first; falls back to the ABP's own layer graphs and binds a self layer (see below). Interface layers take precedence; `interface_class` disables the native fallback; `instance_class` is rejected for a self layer |
| `add_conduit` | Add a conduit node to a state machine (its bound graph is a transition-logic graph, not an anim graph) |

> **AnimGraph-authoring extensions.** `set_transition_rule` gained an `expression` kind for compound multi-term transition conditions — `terms: [{ lhs, op, rhs, abs?, negate? }]` folded through Boolean AND/OR via `combine: "and" | "or"` — extending the existing `bool` / `auto` / `compare` kinds; `get_transition_rule` decodes it back. `set_anim_node_pin_binding` now bootstraps the binding object when a node has none, so it works on previously-unbound nodes instead of refusing them. `auto_layout` gained a `formatter:"builtin"` (also the `auto` fallback) that lays out anim graphs by dependency-aware layering WITHOUT Blueprint Assist, so layout works in release builds where Blueprint Assist is compiled out.

### ABP-native animation layers

| Action | Description |
|--------|-------------|
| `add_anim_layer_graph` | Create an ABP-native animation layer: a `UAnimationGraph` on the animation graph schema in the Animation Blueprint's own function graphs — what the editor's My Blueprint → **+** → Animation Layer button produces. No `UAnimLayerInterface` asset needed, so variant ABPs need not share a layer signature. The schema is what makes the animation compiler emit a real `FAnimBlueprintFunction` (`blueprint add_function` produces an inert K2 graph instead), and the Output Pose root node is created automatically. Params: `asset_path`, `layer_name`, optional `input_poses` (pose names only — `["InPose"]` or `[{"name": "InPose"}]`, capped at 16), optional `compile` (default `true`). Returns `graph_name`, `graph_class`, `schema_class`, `node_count`, `input_poses`, `compiled`, `saved`. Refuses a duplicate graph name (never renames or replaces the incumbent), the reserved name `AnimGraph`, child ABPs, macro libraries, and interface Blueprints |

> **Reading a native layer back.** It appears in `blueprint list_graphs` as `type: "function"` — as does the stock `AnimGraph`, since that classification is by array membership, not schema — and never in `blueprint get_interfaces`. `animation get_linked_layers` shows the consumer node title `"<Layer>\nAnim Layer (self)"`, and `add_linked_anim_layer` reports `interface_class: "<self>"` with `guid_resolved: false` for a self bind.
>
> **Ordering constraint.** Self-layer pose pins resolve against the ABP's generated skeleton class, so `add_anim_layer_graph(compile=false)` followed immediately by `add_linked_anim_layer` places a node with no pose pins. Keep the default `compile=true`, or recompile before placing. Placing the node against a compiled layer regenerates its pins: an output `Pose` plus one input pin per declared input pose.

### Anim-node bindings + time-series sampling (v0.19.0)

| Action | Description |
|--------|-------------|
| `get_anim_node_function_bindings` / `set_anim_node_function_binding` | Read/write the per-node On Initial Update / On Become Relevant / On Update function slots. The write mirrors the engine's `ValidateFunctionRef` (signature + thread-safe check) before setting + recompiling; empty `function_name` clears |
| `get_anim_node_pin_bindings` / `set_anim_node_pin_binding(s)` | Read/write the per-pin property-access bindings in a node's `PropertyBindings` map. The write rebuilds the binding, `ReconstructNode()`s, and recompiles; empty `path` clears. `get_nodes` also emits compact additive `bindings` / `pin_bindings` per node |
| `sample_pie_timeseries` | Start an ASYNC PIE session sampling a target's dotted, UDS-friendly `variables` each tick (gated by `sample_interval`, capped by `max_samples`) and firing typed `provocations` (`set_control_rotation` / `add_movement_input` / `jump` / `console_command`) at their `time`. Poll with `poll_pie_smoke`, force-end with `stop_pie_smoke` |
| `sample_pie_anim_instance` (`compare_to_actor`) | Optional lockstep parity comparison — samples a second PIE actor's AnimInstance and emits a per-variable delta + per-type tolerance pass/fail roll-up |

## MonolithPoseSearch (5 actions)

Motion matching support via Epic's PoseSearch plugin. Read schema configurations (channels, sample rates, cardinality), inspect databases (sequences, sampling data), and manage database contents — add or remove animation sequences and query stats like pose count and search mode. Useful for setting up and iterating on motion matching configurations without touching the editor UI.

| Action | Description |
|--------|-------------|
| `get_pose_search_schema` | Schema channels, sample rate, cardinality |
| `get_pose_search_database` | Sequences, schema ref, enabled/sampling data |
| `add_database_sequence` | Add sequence to PoseSearch database |
| `remove_database_sequence` | Remove sequence from PoseSearch database |
| `get_database_stats` | Pose count, search mode, validity |

## IKRig & Retargeter (5 actions)

Inspect and configure IK Rigs and IK Retargeters. Read chain definitions, goals, solvers, and retarget roots from IK Rigs. Add or remove solvers (Full Body IK, PBIK, TwoBone, etc.). Inspect Retargeter chain mappings between source and target rigs, including auto-map status per chain. Set or update chain mappings programmatically — essential for retargeting workflows where you need to map skeletons with different bone structures.

| Action | Description |
|--------|-------------|
| `get_ikrig_info` | IKRig asset info: chains, goals, solvers, retarget root, goal offsets and weights |
| `add_ik_solver` | Add a solver to an IKRig. `solver_type` resolves against the live solver-struct table (friendly alias such as `fullbodyik`/`fbik`, `limb`, `pole`; exact struct name; or unique substring — ambiguous input returns the candidate list). Full Body IK now adds correctly (`root_bone` is meaningful for FBIK) |
| `remove_ik_solver` | Remove a solver from an IK Rig's solver stack by 0-based `solver_index`, validated against the current solver count. Returns `removed_index` and `solver_count_after` |
| `get_retargeter_info` | IK Retargeter info: source/target rigs, chain mappings, auto-map status per chain |
| `set_retarget_chain_mapping` | Set or update a chain mapping on an IK Retargeter |

## Control Rig (3 actions)

Read and modify Control Rig Blueprints. Inspect the full hierarchy — bones, controls, nulls, and curves. Read variables defined on the Control Rig. Add new elements (bones, controls, nulls) to the hierarchy with parent specification. Useful for procedural rig setup and bulk configuration.

| Action | Description |
|--------|-------------|
| `get_control_rig_info` | Control Rig hierarchy: bones, controls, nulls, curves |
| `get_control_rig_variables` | Variables on a Control Rig Blueprint |
| `add_control_rig_element` | Add a bone, control, or null to a Control Rig hierarchy |

## MonolithNiagara (129 actions)

Deep Niagara control — from system creation to individual module input configuration. Build complete particle systems from declarative JSON specs, or work incrementally through the module stack. Dynamic inputs get full lifecycle support: search available scripts, attach them to module inputs, configure their sub-inputs, read the tree, remove them. Event handler and simulation stage CRUD for advanced GPU particle workflows. Create custom Niagara modules and functions directly from HLSL source — define typed inputs/outputs, write the shader body, and it produces a proper `.niagaraFunction` asset that appears in the module stack with overridable pins. **New in v0.18.0 (PR #65 by @middle233):** read and overwrite the HLSL source on an existing `CustomHlsl` node in place (`get_custom_hlsl_text` / `set_custom_hlsl_text`), target shared-graph simulation-stage and event scripts directly via `usage` selectors on the module-stack actions, and a search & discovery pack for finding systems by parameter, data interface, material, or structural similarity. Renderer management with mesh, ribbon, and SubUV presets. Data interface configuration supports JSON arrays and structs. Export any system as a complete JSON spec for diffing, templating, or version control. System validation catches common issues like GPU+Light renderer conflicts. NPC particle system support with 5 dedicated actions. Override-preserving module reorder.

### Overview & Discovery

| Action | Description |
|--------|-------------|
| `get_system_summary` | High-level system overview (emitter count, renderer count, param count, compile status) |
| `get_emitter_summary` | High-level emitter overview (module count, renderer count, sim target, module names) |
| `list_emitters` | All emitters with name, index, enabled, sim_target, renderer_count, GUID |
| `list_renderers` | Renderers on an emitter with type (short class name), index, enabled, material |
| `list_emitter_properties` | All editable UPROPERTY fields on an emitter, grouped by category |
| `list_module_scripts` | Search available Niagara module scripts by keyword |
| `list_renderer_properties` | List editable properties on a renderer |

### System & Emitter Management

| Action | Description |
|--------|-------------|
| `create_system` / `request_compile` | System creation and compilation |
| `duplicate_system` | Duplicate a Niagara system asset to a new path |
| `create_emitter` | Create a standalone Niagara emitter asset from scratch |
| `add/remove/duplicate/reorder_emitter(s)` | Emitter management |
| `set_emitter_enabled` / `set_emitter_property` | Emitter configuration |
| `set_system_property` | Set system-level properties (WarmupTime, bDeterminism, bFixedTickDelta, RandomSeed, MaxPoolSize) |
| `set_fixed_bounds` | Set fixed world-space bounds on a Niagara system |
| `set_effect_type` | Assign an effect type asset to a Niagara system |
| `export_system_spec` | Export a system's full spec as JSON (reverse of `create_system_from_spec`) |
| `create_system_from_spec` | Declarative system builder from JSON |
| `validate_system` | Check for GPU+Light renderer conflicts, missing materials, bounds warnings |

### Module Stack

| Action | Description |
|--------|-------------|
| `get_ordered_modules` | Module stack. Usage filter with shorthands ("spawn", "update") |
| `add/remove/move_module` | Module stack operations. `move_module` warns about input override loss |
| `get_module_inputs` / `get_module_graph` | Module inspection with real types |
| `get_module_input_value` | Read the current value of a single module input |
| `set_module_input_value` / `set_module_input_binding` / `set_module_input_di` | Module input configuration. DI class names auto-resolve U prefix |
| `set_static_switch_value` | Set a static switch on a module (bool: true/false, enum: value name, int: number) |

### Dynamic Inputs

| Action | Description |
|--------|-------------|
| `add_dynamic_input` | Add a dynamic input module to a module's input slot |
| `set_dynamic_input_value` | Set an input value on a dynamic input module |
| `search_dynamic_inputs` | Search available dynamic input scripts by keyword (supports multi-word) |

### Event Handlers & Simulation Stages

| Action | Description |
|--------|-------------|
| `add_event_handler` | Add an event handler stage to an emitter. Returns `handler_index` + `usage_id` + `usage`. Rejects unresolved inter-emitter source emitters instead of silently creating an empty `SourceEmitterID` (v0.18.0) |
| `add_simulation_stage` | Add a simulation stage to a GPU emitter. Materializes the matching `particle_simulation_stage` output node and returns `usage_id`, `stage_id`, `graph_outputs` (v0.18.0) |

> **Sim-stage / event selectors (v0.18.0).** `get_ordered_modules` / `add_module` / `move_module` / `duplicate_module` now accept `usage: "particle_simulation_stage"` (selectors `usage_id` / `stage_name` / `stage_index`) and `usage: "particle_event"` (selectors `usage_id` / `handler_index`), so you can target shared-graph simulation-stage and event scripts directly.

### Parameters

| Action | Description |
|--------|-------------|
| `get_all_parameters` / `get_user_parameters` | Parameter listing with optional `emitter`/`scope` filters |
| `get_parameter_value` / `get_parameter_type` / `trace_parameter_binding` | Parameter inspection. `User.` prefix handled transparently |
| `add/remove_user_parameter` / `set_parameter_default` | Parameter management |
| `set_curve_value` / `configure_curve_keys` | Curve DI data management |
| `configure_data_interface` | Set multiple properties on a data interface in one call |

### Renderers

| Action | Description |
|--------|-------------|
| `get_renderer_bindings` / `set_renderer_binding` | Clean JSON binding inspection and editing |
| `add/remove_renderer` / `set_renderer_material` / `set_renderer_property` | Renderer configuration |

### HLSL & Advanced

| Action | Description |
|--------|-------------|
| `get_di_functions` | Data Interface function inspection |
| `get_compiled_gpu_hlsl` | Compiled GPU HLSL (auto-compiles if needed) |
| `get_custom_hlsl_text` | **v0.18.0 (PR #65).** Read the HLSL source from a `CustomHlsl` node via public UPROPERTY reflection. Params: `script_path` (required), optional `node_guid` to disambiguate multi-node scripts |
| `set_custom_hlsl_text` | **v0.18.0 (PR #65).** Overwrite a `CustomHlsl` node's HLSL source under a `Modify()` + transaction with a recompile. Params: `script_path` (required), `hlsl` (required), optional `node_guid` |
| `create_module_from_hlsl` | Create a reusable Niagara module script from HLSL source. **v0.18.0:** generates a ParameterMap bridge graph, preserves Data-Interface input types (NeighborGrid3D / Grid3D / ParticleRead), and strictly validates HLSL input/output types (unknown types hard-fail instead of degrading to `float`) |
| `create_function_from_hlsl` | Create a Niagara function script from HLSL source |
| `batch_execute` | Multiple operations in one transaction |

> All Niagara actions accept `asset_path` as the primary param (`system_path` still works for backward compatibility).

### Search & Discovery (v0.18.0, issue #64 Tranche 2)

| Action | Description |
|--------|-------------|
| `search_by_parameter` | Find systems exposing a user parameter whose name (case-insensitive) contains the query. Optional type filter |
| `search_by_data_interface` | Find systems using a Data Interface whose class name contains the query. Per-system traversal via `FNiagaraDataInterfaceUtilities::ForEachDataInterface` |
| `search_by_material` | Find systems whose emitter renderers reference a given material (Sprite/Ribbon/Mesh bindings) |
| `query_niagara` | Structured-filter query over all systems. Conditions joined by AND: `emitters>N` / `emitters<N` / `emitters=N`, `sim_target=GPU\|CPU`, `has_renderer=<name>`. Deterministic DSL, not natural language |
| `find_similar_systems` | Rank systems by structural similarity to a reference. Score blends emitter-count proximity + renderer-class-set Jaccard + module-name-set Jaccard |
| `find_niagara_references` | Find all assets that reference a given Niagara asset via `IAssetRegistry::GetReferencers` |
| `list_system_data_interfaces` | Enumerate the Data Interfaces actually used by a system (distinct from `get_di_properties`, which is CDO-class reflection only) |

---

### HLSL Module Creation

Two actions let you create reusable Niagara scripts directly from HLSL source without touching the editor. Both work on CPU and GPU sim targets.

#### `create_module_from_hlsl`

Creates a Niagara module script (usage: `Module`) and saves it as a `.niagaraFunction` asset. Module scripts run in the emitter stack — they read and write particle attributes.

**Params:**

| Param | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | yes | Script asset name (no extension) |
| `save_path` | string | yes | Content path, e.g. `/Game/VFX/Niagara/Modules/MyModule` |
| `hlsl` | string | yes | HLSL function body |
| `inputs` | array | no | Input pin definitions — `[{"name": "InValue", "type": "float"}]` |
| `outputs` | array | no | Output pin definitions — `[{"name": "OutValue", "type": "float"}]` |
| `description` | string | no | Tooltip shown in the module stack |

**Supported types:** `float`, `int`, `bool`, `vec2`, `vec3`, `vec4`, `color`, `position`, `quat`, `matrix`

**Example — scale particle color by speed:**

```json
{
  "action": "create_module_from_hlsl",
  "params": {
    "name": "M_ScaleColorBySpeed",
    "save_path": "/Game/VFX/Niagara/Modules/M_ScaleColorBySpeed",
    "description": "Tints particle color based on velocity magnitude",
    "inputs": [
      {"name": "InSpeedScale", "type": "float"},
      {"name": "InTintColor", "type": "color"}
    ],
    "hlsl": "float Speed = length(float3(Particles.Velocity.x, Particles.Velocity.y, Particles.Velocity.z));\nfloat4 C = Particles.Color;\nfloat4 Tint = InTintColor;\nParticles.Color = float4(C.rgb * Tint.rgb * (1.0 + Speed * InSpeedScale), C.a);"
  }
}
```

After creating the module, add it to a system and set its inputs:

```json
{"action": "add_module", "params": {"asset_path": "/Game/VFX/NS_Fire", "emitter_name": "Emitter", "module_path": "/Game/VFX/Niagara/Modules/M_ScaleColorBySpeed", "stage": "update"}}
```

```json
{"action": "set_module_input_value", "params": {"asset_path": "/Game/VFX/NS_Fire", "emitter_name": "Emitter", "module_name": "M_ScaleColorBySpeed", "input_name": "InSpeedScale", "value": "0.05", "type": "float"}}
```

#### `create_function_from_hlsl`

Creates a Niagara function script (usage: `Function`) — a pure utility that takes typed inputs and returns typed outputs. Functions have no implicit access to particle attributes; all data flows through their explicit signature.

**Params:** Same as `create_module_from_hlsl`.

**Example — remap a float from one range to another:**

```json
{
  "action": "create_function_from_hlsl",
  "params": {
    "name": "F_Remap",
    "save_path": "/Game/VFX/Niagara/Functions/F_Remap",
    "description": "Remaps InValue from [InMin, InMax] to [OutMin, OutMax]",
    "inputs": [
      {"name": "InValue", "type": "float"},
      {"name": "InMin",   "type": "float"},
      {"name": "InMax",   "type": "float"},
      {"name": "OutMin",  "type": "float"},
      {"name": "OutMax",  "type": "float"}
    ],
    "hlsl": "float T = saturate((InValue - InMin) / (InMax - InMin));\nfloat Result = lerp(OutMin, OutMax, T);"
  }
}
```

#### HLSL body rules

A few things will burn you if you're not expecting them:

- **Use bare input names** — write `InValue`, not `Module.InValue`. The compiler auto-qualifies with the module namespace. A dot in the name makes it a struct member access and you'll get an undeclared identifier.
- **Same rule for outputs** — bare names only, or skip the output signature entirely and write directly to particle attributes.
- **Write to particle attributes directly in the body** via ParameterMap tokens (`Particles.Color`, `Particles.Velocity`, etc.) — this is the right way for module scripts.
- **Can't swizzle ParameterMap variables directly** — `Particles.Color.xyz` is parsed as one token (the dot is not a separator in Niagara's HLSL tokenizer). Assign to a local first: `float4 C = Particles.Color; float3 RGB = float3(C.x, C.y, C.z);`
- **Inputs are overridable after the fact** — `set_module_input_value` and `set_module_input_binding` work on CustomHlsl modules the same as any other module. The inputs appear in the stack's details panel and as overridable pins.

#### Numeric emitter lookup

All Niagara actions that take `emitter_name` also accept a numeric string index (e.g. `"0"`, `"1"`) as a fallback when no emitter matches the name exactly. Useful for inspecting emitters with auto-generated or non-obvious names.

## MonolithEditor (33+ actions)

Build and diagnostic tools. Trigger UBT builds or Live Coding compiles directly from your AI assistant. Read structured build errors, compile output, and crash context — no need to scroll through Output Log manually. The log system maintains a 10,000-entry ring buffer with full-text search, category filtering, and verbosity-level control. Scene capture saves viewport screenshots to disk. Texture import brings image files into UE as UTexture2D assets. Flipbook stitching combines sprite sheets into flipbook textures. Phase J F8 added a blank-map factory and module-status query for diagnosing missing-plugin/module-load issues from the AI side. v0.14.8 added `list_automation_tests` + `run_automation_tests` for in-editor automation regression runs (PR #48 by @MaxenceEpitech). v0.14.9 added `run_python` + `load_level` (Issue #50, ported from @JCSopko's fork). v0.14.10 added PIE control (PR #54 by @MaxenceEpitech). **v0.16.0 added 4 new preview/inspection actions** plus 3 new `capture_scene_preview` asset types. **v0.19.0** adds live-PIE object introspection/driving, deterministic input injection, and a programmatic stat-group readout (see below).

| Action | Description |
|--------|-------------|
| `trigger_build` | Trigger full UBT build |
| `live_compile` | Trigger Live Coding compile |
| `get_build_errors` / `get_build_status` / `get_build_summary` | Build diagnostics |
| `search_build_output` | Pattern search in build output |
| `get_recent_logs` / `search_logs` / `tail_log` | Log ring buffer (10K entries) |
| `get_log_categories` / `get_log_stats` | Log metadata |
| `get_compile_output` | Structured compile report |
| `get_crash_context` | Recent errors and crash info |
| `capture_scene_preview` | Render an asset offscreen via `FAdvancedPreviewScene` + `USceneCaptureComponent2D`. **v0.16.0 widening:** `asset_type` enum now accepts `material` (existing), `niagara` (existing), `static_mesh`, `skeletal_mesh` (with optional `animation_path` + `seek_time` for posed-frame capture), and `widget` (UMG via `FWidgetRenderer` with `scale` DPI multiplier) |
| `capture_sequence_frames` | Multi-frame temporal capture at specified timestamps |
| `capture_system_gif` | Capture a Niagara system as a sequence of PNG frames with optional GIF encoding |
| `capture_material_grid` (**v0.16.0**) | Render N material instances side-by-side under shared lighting; auto-grid via `ceil(sqrt(N))` with optional `columns` override |
| `capture_with_overlay` (**v0.16.0**) | Capture a static mesh under one of 5 engine debug show-flag modes: `wireframe`, `normals`, `uv_density`, `lightmap_density`, `shader_complexity` |
| `inspect_material_pbr` (**v0.16.0**) | Classify a material's texture parameters by PBR slot (basecolor/normal/roughness/metallic) and detect ORM / ARM / MRA channel-packing conventions. Pure reflection, no rendering |
| `inspect_texture_channels` (**v0.16.0**) | Per-channel R/G/B/A min/max/mean statistics on `UTexture2D` source mip 0. Optional `emit_splits=true` writes 4 grayscale-replicated PNGs |
| `import_texture` / `stitch_flipbook` | Texture pipeline |
| `delete_assets` | Delete assets by path. Optional `allowed_prefixes[]` guard + `force` bool (v0.18.0). Closes open asset editors + clears dirty flags before deleting, and runs inside an unattended-script guard so it never pops a blocking modal that would freeze an unattended session. `force=false` soft-deletes; `force=true` calls `ForceDeleteObjects`. Per-asset failures returned in a `failed_to_delete[]` array |
| `get_viewport_info` | Active viewport camera location, rotation, FOV, resolution |
| `create_empty_map` | Create a blank UWorld at a path (Phase J F8) |
| `get_module_status` | Query loaded/loading/unloaded state of any UE module (Phase J F8) |
| `list_automation_tests` / `run_automation_tests` | Enumerate + run UE automation tests by full-path prefix (v0.14.8 PR #48 by @MaxenceEpitech) |
| `run_python` | Execute Python via `IPythonScriptPlugin::ExecPythonCommandEx` (v0.14.9 Issue #50) |
| `load_level` | Wraps `ULevelEditorSubsystem::LoadLevel(AssetPath)` (v0.14.9 Issue #50) |
| `start_pie` / `stop_pie` / `run_console_command` | PIE session control (v0.14.10 PR #54 by @MaxenceEpitech) |

### Live-PIE introspection + stat readout (v0.19.0)

| Action | Description |
|--------|-------------|
| `pie_get_object_properties` | Read live UPROPERTY values off a resolved PIE object by dotted member path. Resolves via `actor_label` / `object_name` / `class_name`, optional `component_name` or `anim_instance`. Descends nested structs and matches UserDefinedStruct members by friendly (authored) name. Read-only |
| `pie_call_function` | Call a BlueprintCallable function/event on a resolved PIE object — marshals JSON `args` into the parameter frame and invokes via `ProcessEvent`. Rejects replicated/latent functions. **Mutates live PIE state** |
| `pie_set_control_rotation` | Set a player controller's control rotation (`pitch`/`yaw`/`roll`); `hold_frames` re-applies each frame to outlast a per-tick camera system (best-effort) |
| `pie_inject_input_action` | Inject an Enhanced Input action value into a live PIE local player (runs its modifiers + triggers). `value` shape maps to bool/Axis1D/Axis2D/Axis3D |
| `pie_possess_spectator_free` | `enable=true` detaches the controller to a free-fly spectator pawn; `enable=false` re-possesses the original. Needs a `SpectatorClass` on the game mode |
| `get_stat_group_values` | Read a stats group (`STATGROUP_Anim` or short `Anim`) into a structured response — counter values + cycle-stat timing in ms. `sample_frames>1` aggregates min/avg/max. `#if STATS`-gated (not in Shipping/Test) |

## MonolithConfig (6 actions)

INI config resolution and debugging. UE's config system stacks multiple INI files (Base, Default, Saved, platform overrides) and it's not always obvious which layer wins. These actions resolve any setting to its effective value, show a multi-layer breakdown of where the value comes from, diff your project against engine defaults, and search across all config files. Essential for debugging "why isn't this setting taking effect" situations.

| Action | Description |
|--------|-------------|
| `resolve_setting` | Resolve a config key to its effective value |
| `explain_setting` | Multi-layer breakdown of how a value is determined |
| `diff_from_default` | Show what changed from engine defaults |
| `search_config` | Search across all config files |
| `get_section` | Read a full INI section |
| `get_config_files` | List all config files with hierarchy info |

## MonolithIndex / Project (8 actions)

Deep project search powered by SQLite FTS5. The plugin automatically indexes every asset in your project on editor startup — including marketplace plugins — into a searchable database. Full-text search with ranking and match highlighting. Bidirectional dependency tracing (what does this asset depend on, what references it). Filter by asset type with pagination. Deep asset details pull nodes, variables, and connections. Covers 9,500+ assets out of the box with zero configuration.

| Action | Description |
|--------|-------------|
| `search` | FTS5 full-text search across all indexed assets |
| `find_references` | Bidirectional dependency tracing |
| `find_by_type` | Filter assets by type with pagination |
| `get_stats` | Index statistics (tables, counts) |
| `get_asset_details` | Deep asset data (nodes, vars, connections) |
| `export_asset_text` (**v0.19.0**) | Export an asset to its native T3D text dump (or grepped excerpts) and return it directly — the escape hatch for surfaces no typed read exposes. Scope with `object_filter` (sub-object), narrow with `grep_pattern`, bounded by `max_bytes`. Prefer the typed read actions first |

## MonolithSource (20 actions)

1M+ C++ symbols indexed natively in C++ — no Python, no external tools, no network access required. The engine source indexer runs automatically on startup and builds a local SQLite database of every class, function, enum, struct, and macro in UE. Read full class source or just the member list. Traverse call graphs in both directions (callers and callees). Walk class hierarchies with configurable depth. Search symbols by name across the entire engine codebase. Read raw source files by relative path. Optional project C++ indexing (via `Scripts/index_project.py`) adds your own source code alongside engine symbols, enabling `find_callers`/`find_callees`/`get_class_hierarchy` results that span both engine and project code.

> **v0.19.0 — symbol index tripled.** A parser fix that indexes allman-brace (next-line `{`) plain classes/structs — the Epic coding-standard layout — took the engine `symbols` table from ~301K to ~967K rows. Types like `FCollisionShape`, `FSceneView`, `FScopeLock`, `FPaths`, and ~40K other exported non-reflected engine types now have rows and extracted members, so `get_include_path`, `read_source`, `get_class_hierarchy`, `verify_symbols`, `get_signature`, and `find_example_usage` resolve them. Existing `EngineSource.db` files enrich on the next full `trigger_reindex`.

| Action | Description |
|--------|-------------|
| `read_source` | Full source or members-only for any class |
| `find_references` | Type references across engine source |
| `find_callers` / `find_callees` | Call graph traversal |
| `search_source` | Full-text source search (cursor-paginated) |
| `get_class_hierarchy` | Ancestors and descendants |
| `get_module_info` | Module path, type, file count, key classes |
| `get_symbol_context` | Definition with surrounding context |
| `read_file` | Raw source file by path |
| `trigger_reindex` | Re-run the engine source indexer |
| `trigger_project_reindex` | Incremental re-index of project C++ source (`Source/` + `Plugins/`): purges the previously indexed project rows, then re-indexes them, so deleted files and symbols drop out. Engine rows untouched. Also runs automatically after Live Coding / hot reload |
| `audit_module_dep_reality` | Find UE type references whose declaring module isn't in the file's Build.cs deps (the LNK2019 bug class) |

### LLM C++ authoring ergonomics (v0.19.0, 8 actions)

Read-only / offline-served lookups so an AI agent can resolve an include, signature, deprecation, or Build.cs dep in one round-trip instead of reading raw source.

| Action | Description |
|--------|-------------|
| `get_include_path` | Canonical `#include` for a symbol, derived from its indexed header. Reports `includable`, owning module, and a `build_cs_note`; a `Class::Method` resolves via the owning class |
| `get_signature` | Exact overload signature(s) for a function/method, read from the declaration line (engine class-body methods aren't indexed as symbols). Overloads returned separately |
| `check_deprecations` | Batch `UE_DEPRECATED` status for a `symbols[]` list — `{deprecated, version, message, kind}` per symbol. Returns `index_state:"empty"` (never a false clean bill) before the first reindex |
| `verify_symbols` | Batch pre-flight verdict — composes existence + include + signature + deprecation per symbol into one record. A missing symbol reports `exists:false`, no error |
| `find_example_usage` | Ranked real call-site examples via source-line FTS (`prefer` engine/project), ±3 lines of context, cursor-paginated |
| `suggest_build_cs_deps` | Required Build.cs modules + missing deps for a file (or symbol list). Reads the declaring module's `Build.cs` from disk — works on uncommitted files |
| `lint_header` | Regex-level UHT-gotcha lint of a single header — **works on unindexed files you just wrote**. Flags missing `GENERATED_BODY()`, misplaced `*.generated.h`, missing `<MODULE>_API`, reflection macros in non-reflected types, invalid specifiers |
| `generate_class_stub` | UCLASS-derived `.h`/`.cpp` stub pair returned as TEXT (never writes to disk) — correct include order, `<MODULE>_API`, `GENERATED_BODY()`, and the right constructor overload |

## MonolithUI (138 actions)

The always-on actions cover the full widget Blueprint lifecycle plus the Phase A–L architecture expansion (2026-04-26): widget tree CRUD, templates, styling, Animation v1 (deprecated) + v2 (hoisted Bezier/spring/event tracks), settings/save scaffolding, accessibility, EffectSurface widget + 10 sub-bag setters, Spec Builder + Schema + Serializer (`build_ui_from_spec`/`dump_ui_spec_schema`/`dump_ui_spec`), Type Registry diagnostic, and hoisted Design Import verbs (5). Templates are the headline feature: call one action and get a fully wired-up HUD, main menu, settings screen, dialog box, loading screen, inventory grid, save/load menu, or toast notification system. EffectSurface action handlers return `-32010 ErrOptionalDepUnavailable` when the optional EffectSurface provider is absent (decoupled 2026-04-27). Built-in accessibility audit checks your UI against common accessibility standards.

51 CommonUI actions (conditional on `WITH_COMMONUI`, shipped v0.14.0 across 9 categories + 1 Style Service diagnostic `dump_style_cache_stats`) cover activatable widgets, button styles, input routing, hardware visibility, and the class-as-data pattern for style creators. The default button class is auto-created at `/Game/Monolith/CommonUI/MonolithDefaultCommonButton`.

0.15.0 closed a big chunk of the MCP gap here. Close-the-loop primitives: `rename_widget`, `add_widget_variable`, `audit_focus_chain`, `apply_token_binding`, `list_widget_property_enums`, `convert_textblock_to_common`, `set_action_bar_button_class`, `dump_blueprint_compile_log`. Headline scaffolders: `scaffold_main_menu`, `scaffold_settings_panel_with_tabs`, `scaffold_pause_menu`, `build_menu_from_spec`. Navigation + conversion gap-closure: `set_widget_navigation_bulk`, `dump_widget_navigation`, `convert_border_to_common`, `reparent_widget_root`, `set_widget_is_variable`. The CommonUI-surface conversions are `#if WITH_COMMONUI`-gated. `compile_widget` now surfaces `errors[]` / `warnings[]` arrays instead of a bare boolean, and `set_widget_property` accepts `value` as an alias for `property_value`.

4 GAS attribute-binding actions (`bind_widget_to_attribute`, `unbind_widget_attribute`, `list_attribute_bindings`, `clear_widget_attribute_bindings`) are aliased into the `ui` namespace so widget work doesn't need to namespace-hop into `gas`. They dispatch to the same handlers as their canonical `gas::` versions.

For the full action list, call `monolith_discover("ui")` (add `detail=true` for inline param schemas, or `describe_query("action_schema")` for one action) or check `Docs/API_REFERENCE.md`.

## MonolithMesh (194 core actions, +45 experimental town gen off by default)

The largest module in Monolith. 22 capability tiers covering mesh inspection, scene manipulation, spatial queries, level blockout, GeometryScript mesh editing, horror spatial analysis, accessibility validation, lighting analysis, audio/acoustics, performance budgeting, decal/detail placement, level design, tech art pipeline (mesh import now supports optional skeletal-mesh + animation import via `import_mesh` — PR #58 by @4698to), advanced level design, context-aware prop placement, procedural geometry, genre presets, encounter design, and quality/polish. Plus 45 procedural town generation actions (work-in-progress, disabled by default via `bEnableProceduralTownGen`, not in the public count — unless you're willing to dig in and help improve it, best left alone for now). For a narrative walkthrough, see [[Mesh Module]].

### Mesh Inspection (12)

| Action | Description |
|--------|-------------|
| `get_mesh_info` | Tri/vert count, bounds, materials, LODs, collision, Nanite, vertex colors |
| `get_mesh_bounds` | AABB, extent, sphere radius, volume (cm^3), surface area |
| `get_mesh_materials` | Per-section material paths + tri counts |
| `get_mesh_lods` | Per-LOD tri/vert counts + screen sizes |
| `get_mesh_collision` | Collision type, shape counts (box/sphere/capsule/convex) |
| `get_mesh_uvs` | UV channels, island count, overlap % |
| `analyze_skeletal_mesh` | Quality analysis: bone weights, degenerate tris, LOD delta |
| `analyze_mesh_quality` | Non-manifold edges, degenerate tris, loose verts, quality score |
| `compare_meshes` | Side-by-side delta with percentages |
| `get_vertex_data` | Paginated vertex positions + normals (max 5000) |
| `search_meshes_by_size` | Find meshes by dimension range from catalog |
| `get_mesh_catalog_stats` | Total indexed meshes, category + size breakdown |

### Scene Manipulation (8)

| Action | Description |
|--------|-------------|
| `get_actor_info` | Class, transform, mesh, materials, mobility, tags, components |
| `spawn_actor` | Spawn StaticMeshActor (path starts with `/`) or class |
| `move_actor` | Set or offset actor transform |
| `duplicate_actor` | Clone actor with optional offset |
| `delete_actors` | Delete placed actors (NOT asset files) |
| `group_actors` | Move actors to folder |
| `set_actor_properties` | Set mobility, physics, shadows, tags, mass |
| `batch_execute` | Multiple actions in single undo transaction (cap 200) |

### Scene Spatial Queries (11)

| Action | Description |
|--------|-------------|
| `query_raycast` | Line trace — hit actor, location, normal, distance, phys material |
| `query_multi_raycast` | Multi-hit trace sorted by distance |
| `query_radial_sweep` | Sonar sweep — compass direction summary (cap 512 rays) |
| `query_overlap` | Overlap test (box/sphere/capsule) |
| `query_nearest` | Find nearest actors (broadphase) |
| `query_line_of_sight` | Visibility check — bool + blocking actor |
| `get_actors_in_volume` | All actors in a named volume |
| `get_scene_bounds` | World AABB enclosing all actors |
| `get_scene_statistics` | Actor counts, total tris, lights, navmesh status |
| `get_spatial_relationships` | Neighbors with relationships (on_top_of, adjacent, near...) |
| `query_navmesh` | Navigation path query — points, distance, reachable |

### Level Blockout (15)

| Action | Description |
|--------|-------------|
| `get_blockout_volumes` | List all Monolith.Blockout tagged volumes |
| `get_blockout_volume_info` | Volume details, primitive list, other actors |
| `setup_blockout_volume` | Apply Monolith tags to a BlockingVolume |
| `create_blockout_primitive` | Spawn tagged blockout primitive |
| `create_blockout_primitives_batch` | Batch placement (cap 200, single undo) |
| `create_blockout_grid` | Floor grid in volume |
| `match_asset_to_blockout` | Find size-matched assets from catalog |
| `match_all_in_volume` | Batch match all primitives in volume |
| `apply_replacement` | Atomic replace blockouts with real assets (pivot-adjusted) |
| `set_actor_tags` | Batch apply tags |
| `clear_blockout` | Remove blockout primitives by owner tag |
| `export_blockout_layout` | Export as JSON (positions relative 0-1, sizes absolute) |
| `import_blockout_layout` | Import layout (scales positions, keeps sizes) |
| `scan_volume` | Daredevil scan — walls, floor, ceiling, openings |
| `scatter_props` | Poisson disk scatter with collision avoidance |

### Mesh Operations / GeometryScript (12)

Requires `WITH_GEOMETRYSCRIPT`. Handle-based workflow — load a mesh into a handle, operate on it, save when done.

| Action | Description |
|--------|-------------|
| `create_handle` | Load mesh into editable handle (asset path or "primitive:box") |
| `release_handle` | Free a handle |
| `list_handles` | All handles with tri count, last access time |
| `save_handle` | Commit handle to new StaticMesh asset |
| `mesh_boolean` | Union/subtract/intersect |
| `mesh_simplify` | Reduce triangle count |
| `mesh_remesh` | Isotropic remeshing |
| `generate_collision` | Convex decomp/auto shapes |
| `generate_lods` | LOD chain via repeated simplification |
| `fill_holes` | Automatic hole detection + fill |
| `compute_uvs` | Auto-unwrap/box/planar/cylinder projection |
| `mirror_mesh` | Mirror across X/Y/Z |

### Horror Spatial Analysis (8) — *Work-in-progress: functional but still being refined*

| Action | Description |
|--------|-------------|
| `analyze_sightlines` | Claustrophobia score, blocked % at 5/10/20m |
| `find_hiding_spots` | Grid-sample concealment from viewpoints |
| `find_ambush_points` | Concealed positions with surprise angles |
| `analyze_choke_points` | Navmesh path width, flank possibility |
| `analyze_escape_routes` | Paths to exits scored by threat exposure |
| `classify_zone_tension` | Calm/uneasy/tense/dread/panic composite |
| `analyze_pacing_curve` | Tension along path, scare point detection |
| `find_dead_ends` | Navmesh flood-fill for single-exit regions |

### Accessibility Analysis (6)

| Action | Description |
|--------|-------------|
| `validate_path_width` | Wheelchair clearance (120cm default) |
| `validate_navigation_complexity` | Cognitive difficulty scoring |
| `analyze_visual_contrast` | WCAG-inspired contrast for interactables |
| `find_rest_points` | Safe room spacing along path |
| `validate_interactive_reach` | Height/distance/obstruction checks |
| `generate_accessibility_report` | Motor/vision/cognitive profile, A-F grade |

### Lighting Analysis (5) — *Work-in-progress: functional but still being refined*

| Action | Description |
|--------|-------------|
| `sample_light_levels` | Per-point luminance via scene capture or analytic |
| `find_dark_corners` | Ortho capture + flood-fill dark zones |
| `analyze_light_transitions` | Flag harsh bright-to-dark (>4:1 ratio) |
| `get_light_coverage` | % lit/shadow/dark, light inventory, histogram |
| `suggest_light_placement` | Inverse-square placement for horror_dim/safe_room/clinical |

### Audio & Acoustics (14) — *Work-in-progress: functional but still being refined*

| Action | Description |
|--------|-------------|
| `get_audio_volumes` | Enumerate AudioVolumes with reverb settings |
| `get_surface_materials` | Physical material breakdown (% concrete, tile, etc.) |
| `estimate_footstep_sound` | Surface type to loudness factor (0-1) |
| `analyze_room_acoustics` | Sabine RT60 reverb estimation from surfaces |
| `analyze_sound_propagation` | Material-based occlusion, wall count, dB reduction |
| `find_loud_surfaces` | Dangerous surfaces (metal, glass, gravel) |
| `find_sound_paths` | Image-source reflection paths with attenuation |
| `can_ai_hear_from` | Monster hearing detection with surface awareness |
| `get_stealth_map` | Per-cell loudness + detection radius heatmap |
| `find_quiet_path` | Navmesh path preferring quiet surfaces |
| `suggest_audio_volumes` | Auto-suggest reverb settings from room materials |
| `create_audio_volume` | Spawn AudioVolume with suggested settings |
| `set_surface_type` | Set physical material surface override |
| `create_surface_datatable` | Bootstrap acoustic system (12 horror surfaces) |

### Performance Analysis (5) — *Work-in-progress: functional but still being refined*

| Action | Description |
|--------|-------------|
| `get_region_performance` | Tri count, draw calls, lights, shadow casters |
| `estimate_placement_cost` | Pre-spawn budgeting without spawning |
| `find_overdraw_hotspots` | Translucent screen-space overlap |
| `analyze_shadow_cost` | Flag small props casting shadows pointlessly |
| `get_triangle_budget` | Frustum-culled LOD-aware count vs budget |

### Decal & Detail Placement (4)

| Action | Description |
|--------|-------------|
| `place_decals` | Surface-aligned decals, Poisson scatter |
| `place_along_path` | Blood trails, footprints, drag marks (Catmull-Rom) |
| `analyze_prop_density` | Grid-cell density vs target |
| `place_storytelling_scene` | 5 horror presets (violence/abandoned/dragged/medical/corruption) |

### Level Design (9)

| Action | Description |
|--------|-------------|
| `place_light` | Spawn point/spot/rect/directional lights |
| `set_light_properties` | Modify existing light |
| `set_actor_material` | Assign material to placed actor |
| `swap_material_in_level` | Bulk replace material X with Y |
| `find_replace_mesh` | Swap all instances of mesh X with Y |
| `set_lod_screen_sizes` | Set LOD transition thresholds |
| `find_instancing_candidates` | Find repeated meshes for HISM conversion |
| `convert_to_hism` | Convert StaticMeshActors to HISM |
| `get_actor_component_properties` | Read arbitrary UPROPERTYs |

### Volumes & Properties (7)

| Action | Description |
|--------|-------------|
| `spawn_volume` | Trigger/kill/pain/blocking/audio/post_process volumes |
| `get_actor_properties` | Read properties via reflection |
| `copy_actor_properties` | Copy settings between actors |
| `build_navmesh` | Trigger navmesh rebuild |
| `select_actors` | Control editor selection + camera focus |
| `snap_to_surface` | Trace + normal alignment |
| `set_collision_preset` | Set collision profile |

### Horror Intelligence (4)

| Action | Description |
|--------|-------------|
| `predict_player_paths` | 4 strategies: shortest/safest/curious/cautious |
| `evaluate_spawn_point` | Composite spawn quality score (S-F grade) |
| `suggest_scare_positions` | Optimal scripted event positions along path |
| `evaluate_encounter_pacing` | Spacing/intensity/rest period analysis |

### Tech Art Pipeline (7)

| Action | Description |
|--------|-------------|
| `import_mesh` | FBX/glTF import with settings |
| `fix_mesh_quality` | GeometryScript repair (degenerate/weld/holes/normals) |
| `auto_generate_lods` | One-shot LOD chain generation |
| `analyze_texel_density` | UV area x texture res = texels/cm |
| `analyze_material_cost_in_region` | Shader complexity per placed mesh |
| `set_mesh_collision` | Write collision to mesh asset |
| `analyze_lightmap_density` | Lightmap resolution + UV density |

### Advanced Level Design (8)

| Action | Description |
|--------|-------------|
| `manage_sublevel` | Create/add/remove/move_actors_to |
| `place_blueprint_actor` | Spawn BP with exposed property values |
| `place_spline` | Spawn spline actor with mesh segments |
| `create_prefab` / `spawn_prefab` | Level Instance creation + placement |
| `randomize_transforms` | Batch random offset/rotation/scale |
| `get_level_actors` | Filtered actor enumeration |
| `measure_distance` | Euclidean/horizontal/navmesh distance |

### Context-Aware Props (8)

| Action | Description |
|--------|-------------|
| `scatter_on_surface` | Props ON shelves/tables (not just floors) |
| `set_room_disturbance` | Orderly/slightly_messy/ransacked/abandoned |
| `configure_physics_props` | SimulatePhysics + sleep state |
| `settle_props` | Trace-snap with random tilt |
| `create_prop_kit` / `place_prop_kit` | Themed prop group authoring + placement |
| `scatter_on_walls` | Horizontal trace wall placement |
| `scatter_on_ceiling` | Upward trace ceiling placement |

### Procedural Geometry (8)

See [[Procedural Geometry]] for a full tutorial.

| Action | Description |
|--------|-------------|
| `create_parametric_mesh` | 15 furniture types (chair, table, shelf, desk, bed, cabinet, dresser, nightstand, bookcase, wardrobe, couch, bathtub, sink, toilet, mirror) |
| `create_horror_prop` | 7 horror types (barricade, boarded_window, debris_pile, broken_furniture, vent_grate, cage, chain_link) |
| `create_structure` | Room/corridor/L/T-junction with openings |
| `create_building_shell` | Multi-story from 2D polygon |
| `create_maze` | 3 algorithms (recursive_backtrack, prims, ellers) + layout JSON |
| `create_pipe_network` | Swept polygon with ball joints |
| `create_fragments` | Plane-slice mesh fragmentation |
| `create_terrain_patch` | Perlin noise heightmap mesh |

### Genre Presets (8)

See [[Genre Presets]] for how to create preset packs for any genre.

| Action | Description |
|--------|-------------|
| `list_storytelling_patterns` | Browse available storytelling patterns |
| `create_storytelling_pattern` | Author horror/fantasy/sci-fi storytelling presets |
| `list_acoustic_profiles` | Browse available acoustic profiles |
| `create_acoustic_profile` | Genre-specific surface acoustics |
| `create_tension_profile` | Genre-specific tension factor weights |
| `list_genre_presets` | Browse available preset packs |
| `export_genre_preset` | Bundle presets to JSON file |
| `import_genre_preset` | Load preset pack with merge mode |

### Encounter Design (8)

See [[Horror Level Design]] for workflows and accessibility considerations.

| Action | Description |
|--------|-------------|
| `design_encounter` | Capstone: spawn+patrol+exits+sightlines composed |
| `suggest_patrol_route` | Stalker/patrol/ambusher navmesh routes |
| `analyze_ai_territory` | Hiding/patrol/ambush heatmap |
| `evaluate_safe_room` | Defensibility + accessibility amenity scoring |
| `analyze_level_pacing_structure` | Macro tension-to-release mapping |
| `generate_scare_sequence` | 4 styles: slow_burn/escalating/relentless/single_peak |
| `validate_horror_intensity` | Accessibility intensity cap audit |
| `generate_hospice_report` | 5-section accessibility audit for players with diverse needs, A-F grade |

### Quality & Polish (9)

| Action | Description |
|--------|-------------|
| `validate_naming_conventions` | Flag assets missing SM_/M_/T_ prefixes |
| `batch_rename_assets` | Find/replace with reference fixup |
| `generate_proxy_mesh` | Merge meshes into simplified proxy |
| `setup_hlod` | Configure HLOD layers |
| `analyze_texture_budget` | Streaming pool usage + top textures |
| `analyze_framing` | Rule-of-thirds composition scoring |
| `evaluate_monster_reveal` | Silhouette/distance/backlight scoring |
| `analyze_co_op_balance` | Coverage blind spots (P3 placeholder) |
| `integration_hooks_stub` | AI Director/GAS/telemetry stubs |

### Procedural Town Generation (45 actions) — Work-in-Progress

> **Disabled by default.** Enable via `bEnableProceduralTownGen` in Project Settings > Plugins > Monolith. Known geometry issues — wall misalignment, room separation. This is very much a work-in-progress — unless you're willing to dig in and help improve it, it's best left alone for now.

Floor plan generation, building facades, roofs, city blocks, spatial registry, auto-volumes, terrain/foundations, architectural features, room furnishing, and debug views. The pipeline: `generate_floor_plan` -> `create_building_from_grid` -> `generate_facade` -> `generate_roof` -> `register_building`. Or use `create_city_block` for the full pipeline in one call.

---

## MonolithGAS (135 actions)

Complete Gameplay Ability System integration. 131 GAS-namespace actions plus 4 widget attribute-binding actions also aliased into the `ui` namespace for ergonomic UI work. Works with the engine's built-in GameplayAbilities plugin — no marketplace dependencies required for core functionality. Blueprint-based AttributeSets require the optional GBA (Blueprint Attributes) plugin from Fab. Phase J shipped a baseline vitals AttributeSet template (F4) so projects without GBA still get a working starter set.

All 4 implementation phases complete, 53/53 tests PASS. 12 bugs fixed across 8 commits.

### Abilities (14)

| Action | Description |
|--------|-------------|
| `create_ability` | Create a new Gameplay Ability Blueprint |
| `get_ability_info` | Inspect ability properties — tags, cooldown, cost, activation |
| `set_ability_property` | Set ability properties (activation policy, cooldown, costs, tags) |
| `list_abilities` | List all Gameplay Ability assets in project |
| `grant_ability` | Grant ability to an ASC at runtime |
| `revoke_ability` | Revoke ability from an ASC |
| `activate_ability` | Activate an ability on an ASC |
| `cancel_ability` | Cancel a running ability |
| `list_granted_abilities` | List abilities granted to an ASC |
| `get_ability_state` | Query activation state of a specific ability |
| `set_ability_tags` | Configure ability tags (cancel, block, activation required/blocked) |
| `create_ability_from_template` | Scaffold from common patterns (melee, projectile, buff, toggle, passive) |
| `duplicate_ability` | Duplicate an ability to a new path |
| `validate_ability` | Check for common configuration issues |

### Attributes (16)

| Action | Description |
|--------|-------------|
| `create_attribute_set` | Create AttributeSet (C++ scaffold or Blueprint via GBA) |
| `add_attribute` | Add attribute to an AttributeSet |
| `remove_attribute` | Remove attribute from an AttributeSet |
| `get_attribute_set_info` | Inspect attributes, defaults, and clamping |
| `list_attribute_sets` | List all AttributeSet classes in project |
| `set_attribute_default` | Set default value for an attribute |
| `set_attribute_clamp` | Set min/max clamping (GBA Blueprint sets) |
| `get_attribute_value` | Read current attribute value at runtime |
| `set_attribute_value` | Set attribute value at runtime |
| `initialize_from_datatable` | Initialize attribute defaults from a DataTable |
| `create_attribute_datatable` | Create a DataTable for attribute initialization |
| `list_attribute_types` | List available attribute types |
| `get_attribute_modifiers` | List active modifiers on an attribute |
| `batch_set_attributes` | Set multiple attribute values in one call |
| `export_attribute_set` | Export AttributeSet definition as JSON |
| `import_attribute_set` | Import AttributeSet definition from JSON |

### Effects (18)

| Action | Description |
|--------|-------------|
| `create_effect` | Create a new Gameplay Effect Blueprint |
| `get_effect_info` | Inspect effect modifiers, duration, stacking, period |
| `set_effect_property` | Set effect properties |
| `add_modifier` | Add attribute modifier to an effect |
| `remove_modifier` | Remove modifier from an effect |
| `set_duration_policy` | Set instant/infinite/has_duration with duration value |
| `set_stacking` | Configure stacking (aggregate by source/target, stack limit, overflow) |
| `set_period` | Configure periodic effects with interval and execute_on_apply |
| `add_granted_ability` | Add ability granted by this effect |
| `set_immunity` | Configure immunity tag requirements |
| `add_conditional_effect` | Add conditional child effect |
| `list_effects` | List all Gameplay Effect assets |
| `apply_effect` | Apply effect to an ASC at runtime |
| `remove_effect` | Remove active effect from an ASC |
| `list_active_effects` | List active effects on an ASC |
| `duplicate_effect` | Duplicate effect to a new path |
| `create_effect_from_template` | Scaffold from patterns (damage, heal, buff, debuff, DOT, HOT) |
| `validate_effect` | Check for common configuration issues |

### ASC Management (12)

| Action | Description |
|--------|-------------|
| `get_asc_info` | Inspect ASC — granted abilities, active effects, owned tags |
| `initialize_asc` | Initialize ASC with abilities and attributes |
| `grant_ability_set` | Grant multiple abilities at once |
| `apply_effect_set` | Apply multiple effects at once |
| `clear_all_abilities` | Remove all granted abilities |
| `clear_all_effects` | Remove all active effects |
| `get_owned_tags` | Query gameplay tags on an ASC |
| `has_matching_tag` | Check if ASC has a specific tag |
| `initialize_from_datatable` | Initialize abilities and attributes from DataTables |
| `get_asc_debug_info` | Comprehensive ASC debug dump |
| `list_asc_actors` | Find all actors with ASCs in the level |
| `configure_replication` | Set ASC replication mode |

### Tags (10)

| Action | Description |
|--------|-------------|
| `query_tags` | Check tag container state |
| `add_loose_tag` | Add a loose gameplay tag |
| `remove_loose_tag` | Remove a loose gameplay tag |
| `has_tag` | Check for exact tag match |
| `has_any_tags` | Check for any matching tags |
| `has_all_tags` | Check for all matching tags |
| `get_tag_container` | Get full tag container contents |
| `search_tags` | Search registered gameplay tags |
| `create_tag` | Register a new gameplay tag |
| `get_tag_hierarchy` | Get parent/child tag relationships |

### Cues (10)

| Action | Description |
|--------|-------------|
| `create_cue` | Create a Gameplay Cue notify actor or static |
| `trigger_cue` | Trigger a cue on an ASC |
| `get_cue_info` | Inspect cue properties |
| `list_cues` | List all Gameplay Cue assets |
| `set_cue_property` | Set cue properties |
| `add_cue_to_effect` | Associate a cue with a Gameplay Effect |
| `remove_cue_from_effect` | Remove cue association |
| `list_cue_tags` | List registered GameplayCue tags |
| `validate_cue` | Check for missing assets or tag mismatches |
| `duplicate_cue` | Duplicate a cue to a new path |

### Targeting (12)

| Action | Description |
|--------|-------------|
| `create_target_data` | Create target data from trace or actor selection |
| `create_targeting_task` | Create a targeting task Blueprint |
| `get_targeting_info` | Inspect targeting task configuration |
| `set_targeting_property` | Configure targeting parameters |
| `trace_target` | Perform line/sphere/capsule trace for targets |
| `actor_target` | Select specific actors as targets |
| `radius_target` | Find all targets in radius |
| `list_targeting_tasks` | List all targeting task assets |
| `validate_targeting` | Check targeting configuration |
| `create_target_filter` | Create target filter for ability targeting |
| `get_targets_in_range` | Query targets at runtime |
| `duplicate_targeting_task` | Duplicate targeting task |

### Input (8)

| Action | Description |
|--------|-------------|
| `bind_ability_to_input` | Bind ability activation to Enhanced Input action |
| `unbind_ability_input` | Remove input binding |
| `get_input_bindings` | List all ability-input bindings |
| `create_input_config` | Create input configuration for abilities |
| `set_input_binding_property` | Configure input binding details |
| `list_input_actions` | List available Enhanced Input actions |
| `auto_bind_abilities` | Auto-bind abilities to input based on naming conventions |
| `validate_input_bindings` | Check for conflicts or missing input actions |

### Inspect / Debug (16)

| Action | Description |
|--------|-------------|
| `get_active_abilities` | Query active ability instances on an ASC |
| `get_active_effects` | Query active effect instances with remaining duration |
| `get_cooldown_remaining` | Check cooldown time remaining for an ability |
| `get_blocking_tags` | Find tags blocking ability activation |
| `get_attribute_aggregator` | Inspect modifier aggregation for an attribute |
| `get_effect_stack_count` | Query current stack count for an effect |
| `can_activate_ability` | Check if ability can be activated (tags, cost, cooldown) |
| `get_ability_spec` | Inspect ability spec handle details |
| `trace_effect_application` | Debug why an effect failed to apply |
| `get_tag_count` | Query tag count on a container |
| `list_blocked_abilities` | List abilities blocked by current tags |
| `get_granted_effects` | List effects granted by abilities |
| `snapshot_asc_state` | Save ASC state for comparison |
| `diff_asc_states` | Compare two ASC snapshots |
| `get_prediction_key` | Query prediction key state (multiplayer debugging) |
| `log_gas_debug` | Write GAS debug info to editor log |

### Scaffold (14)

| Action | Description |
|--------|-------------|
| `scaffold_character_abilities` | Generate standard character ability set |
| `scaffold_attribute_set` | Generate attribute set from template (RPG, FPS, survival) |
| `scaffold_effect_library` | Generate common effects (damage types, buffs, debuffs) |
| `scaffold_input_config` | Generate input configuration for ability sets |
| `scaffold_targeting_setup` | Generate targeting tasks for common patterns |
| `scaffold_cue_library` | Generate cue assets for an effect library |
| `scaffold_full_gas_setup` | One-shot: abilities + attributes + effects + input + cues |
| `scaffold_hospice_effects` | Generate accessibility-focused infinite-duration GEs |
| `create_combo_system` | Scaffold combo ability chain |
| `create_passive_system` | Scaffold passive ability set with attribute listeners |
| `create_cooldown_system` | Scaffold shared cooldown groups |
| `create_resource_system` | Scaffold mana/stamina/energy resource with regen |
| `export_gas_setup` | Export full GAS configuration as JSON |
| `import_gas_setup` | Import GAS configuration from JSON |

---

## MonolithAI (221 actions)

The most comprehensive AI tooling available through any MCP server. 221 actions across 15 categories covering every major UE AI subsystem. Crown jewel actions: `build_behavior_tree_from_spec` and `build_state_tree_from_spec` — hand the AI a JSON description and it builds the entire asset. Conditional on State Tree and Smart Objects plugins (gated via `WITH_STATETREE` + `WITH_SMARTOBJECTS` after Phase J F22 — both ship with UE). Optional Mass Entity (`WITH_MASSENTITY`) and Zone Graph (`WITH_ZONEGRAPH`) integration for large-scale AI. Phase J F1 hardened BT crash-handling; F8 added `add_perception_to_actor` and `get_bt_graph` helpers.

For the full action list, call `monolith_discover("ai")` (add `detail=true` for inline param schemas, or `describe_query("action_schema")` for one action) or check `Docs/API_REFERENCE.md`.

### Behavior Trees (BT)

Create, inspect, edit, and compile Behavior Trees. Add tasks, decorators, services, and composite nodes. Wire them into tree structures. Configure node properties. `build_behavior_tree_from_spec` creates complete BTs from a JSON specification.

### Blackboards (BB)

Full Blackboard CRUD. Create Blackboard assets, add/remove/edit keys with type configuration (bool, int, float, string, name, vector, rotator, enum, object, class). Set key descriptions and sync modes.

### State Trees (ST)

Create and edit State Trees (UE's newer alternative to BTs). Add states, transitions, conditions, tasks, and evaluators. Configure state parameters and transition rules. `build_state_tree_from_spec` creates complete State Trees from JSON. Requires `#if WITH_STATETREE`.

### Environment Query System (EQS)

Create EQS queries with generators and tests. Configure generator parameters (grid, ring, points around actor, pathfinding grid). Add and configure tests (distance, dot product, trace, overlap, gameplay tags, pathfinding). Full query template management.

### Smart Objects (SO)

Create and manage Smart Object Definitions. Add slots with behavior configurations, activity tags, and claim policies. Configure slot transforms and user conditions. Requires `#if WITH_SMARTOBJECTS`.

### AI Controllers

Create and configure AI Controller Blueprints. Set BT and BB assets. Configure perception components, team assignments, and pathfinding settings.

### AI Perception

Configure Perception components with multiple senses: sight, hearing, damage, touch. Set sense parameters (range, angle, detection by affiliation). Configure stimulus sources on actors.

### Navigation

Navigation mesh queries, path filtering, navigation invokers, query filters, and avoidance configuration. Works with NavMesh at editor time (no PIE required for path queries).

### Runtime / PIE

Start and stop BTs, query active nodes, inspect Blackboard values, debug perception state — all during a Play-in-Editor session. Essential for runtime AI debugging.

### Scaffolding

Generate complete AI setups from templates: patrol AI, combat AI, companion AI, guard AI. Each scaffold creates the BT, BB, Controller, and optionally EQS queries and Smart Object definitions.

### Discovery

List available node classes (tasks, decorators, services), BT assets, BB assets, ST assets, EQS templates, SO definitions, and AI Controllers in the project.

---

## MonolithLogicDriver (66 actions)

Full integration with Logic Driver Pro, a marketplace state machine plugin. Reflection-only integration (no direct C++ API linkage) — works with any Logic Driver Pro version. Conditional on `#if WITH_LOGICDRIVER` with 3-location Build.cs detection.

For the full action list, call `monolith_discover("logicdriver")` (add `detail=true` for inline param schemas, or `describe_query("action_schema")` for one action) or check `Docs/API_REFERENCE.md`.

### SM CRUD

| Action | Description |
|--------|-------------|
| `create_state_machine` | Create a new state machine Blueprint |
| `get_state_machine_info` | Inspect SM — states, transitions, variables, compile status |
| `compile_state_machine` | Compile a state machine |
| `delete_state_machine` | Delete a state machine asset |
| `list_state_machines` | List all state machine assets in the project |
| `duplicate_state_machine` | Duplicate a state machine to a new path |
| `validate_state_machine` | Check for common configuration issues |

### Graph Read/Write

Add states, transitions, configure properties, set transition rules, manage state graph structure.

### Node Configuration

Configure state nodes (entry actions, update logic, exit actions), conduit nodes, and transition events. Set node properties via reflection.

### Runtime / PIE

Start, stop, query active states, trigger transitions, inspect variable values — all during Play-in-Editor.

### JSON Spec

`build_sm_from_spec` creates complete state machines from a JSON specification in one call. `export_sm_spec` and `import_sm_spec` for templating and version control.

### Scaffolding

Generate common patterns from templates: door controller, health system, AI patrol, dialogue system, elevator, puzzle, inventory.

### Components

Add and configure Logic Driver components on actors. Set up component properties and initial state machine assignments.

### Text Graph

Text-based visualization of state machine structure for debugging and documentation.

### Discovery

List available node classes, state types, and templates.

---

## MonolithComboGraph (13 actions)

Integration with the ComboGraph marketplace plugin for visual combo tree editing. Reflection-only integration, conditional on `#if WITH_COMBOGRAPH`. EdGraph sync keeps runtime and editor graphs in sync.

For the full action list, call `monolith_discover("combograph")` (add `detail=true` for inline param schemas, or `describe_query("action_schema")` for one action) or check `Docs/API_REFERENCE.md`.

| Category | Actions | Description |
|----------|---------|-------------|
| Read | 4 | List graphs, inspect structure, read node effects, validate integrity |
| Create | 5 | Create graphs, add nodes with montages, add edges, set effects, set cues |
| Scaffold | 4 | Create combo abilities, link abilities to graphs, scaffold from montage lists, auto-layout |

---

## MonolithAudio (98 actions)

Editor-time audio asset authoring. 82 baseline audio-namespace actions plus 4 perception-binding actions (`bind_sound_to_perception`, `unbind_sound_from_perception`, `get_sound_perception_binding`, `list_perception_bound_sounds`) that bridge audio assets into AI perception sound stimuli plus 12 v0.14.10 MetaSound document introspection actions (PR #18 by @alakangas — read-side complement to the existing 25 Builder API write-side actions). Covers the 5 configurable audio asset types (SoundAttenuation, SoundClass, SoundMix, SoundConcurrency, SoundSubmix), read-only SoundWave inspection, Sound Cue node graph construction, and MetaSound Builder API integration plus document-walk inspection. Phase J F11 hardened the audio asset validator. Phase J F18 added `create_test_wave` for generating sine-wave SoundWaves on demand. MetaSound features conditional on `#if WITH_METASOUND` — when MetaSound is absent, MetaSound actions are inert but Sound Cue, CRUD, batch, perception-binding, and query actions all function normally. Companion `FMetaSoundIndexer` deep indexer in MonolithIndex (also v0.14.10 from PR #18) walks all project MetaSounds at reindex time.

For the full action list, call `monolith_discover("audio")` (add `detail=true` for inline param schemas, or `describe_query("action_schema")` for one action) or check `Docs/API_REFERENCE.md`.

### Asset CRUD (15)

| Action | Description |
|--------|-------------|
| `create_sound_attenuation` | Create USoundAttenuation with optional settings |
| `get_attenuation_settings` | Read all FSoundAttenuationSettings fields |
| `set_attenuation_settings` | Partial update any attenuation fields |
| `create_sound_class` | Create USoundClass with optional parent/properties |
| `get_sound_class_properties` | Read FSoundClassProperties + parent + children |
| `set_sound_class_properties` | Update class properties |
| `create_sound_mix` | Create USoundMix with EQ + class adjusters + timing |
| `get_sound_mix_settings` | Read EQ bands, class adjusters, timing |
| `set_sound_mix_settings` | Update mix settings |
| `create_sound_concurrency` | Create USoundConcurrency |
| `get_concurrency_settings` | Read MaxCount, ResolutionRule, ducking |
| `set_concurrency_settings` | Update concurrency |
| `create_sound_submix` | Create USoundSubmix with optional parent/effect chain |
| `get_submix_properties` | Read effect chain, volume, hierarchy |
| `set_submix_properties` | Update submix |

### Query & Search (10)

| Action | Description |
|--------|-------------|
| `list_audio_assets` | List by type (SoundWave/SoundCue/MetaSoundSource/All/...) |
| `search_audio_assets` | FTS name search across audio assets |
| `get_sound_wave_info` | Duration, channels, sample rate, compression, class, attenuation |
| `get_sound_class_hierarchy` | Recursive tree traversal |
| `get_submix_hierarchy` | Routing tree with effect chains |
| `find_audio_references` | Bidirectional reference scan |
| `find_unused_audio` | Zero-reference audio assets |
| `find_sounds_without_class` | SoundBases missing a sound class |
| `find_unattenuated_sounds` | Missing attenuation |
| `get_audio_stats` | Counts by type, sizes, compression breakdown |

### Batch Operations (10)

| Action | Description |
|--------|-------------|
| `batch_assign_sound_class` | Set class on N assets |
| `batch_assign_attenuation` | Set attenuation on N assets |
| `batch_set_compression` | Set compression on N SoundWaves |
| `batch_set_submix` | Set submix on N assets |
| `batch_set_concurrency` | Set concurrency on N assets |
| `batch_set_looping` | Set looping flag |
| `batch_set_virtualization` | Restart / PlayWhenSilent / Disabled |
| `batch_rename_audio` | Rename with prefix/suffix/find/replace |
| `batch_set_sound_wave_properties` | Multi-property reflection set |
| `apply_audio_template` | Apply class + attenuation + compression + submix + concurrency in one pass |

### Sound Cue Graph (21)

| Action | Description |
|--------|-------------|
| `create_sound_cue` | Create cue, auto-Random if multiple waves |
| `get_sound_cue_graph` | Full graph: nodes, connections, first_node |
| `add_sound_cue_node` | Add any of 22 node types with properties |
| `remove_sound_cue_node` | Remove node |
| `connect_sound_cue_nodes` | Wire nodes via `ChildNodes[]` |
| `set_sound_cue_first_node` | Set root output |
| `set_sound_cue_node_property` | Set any node property via reflection |
| `list_sound_cue_node_types` | All 22 types with max_children |
| `find_sound_waves_in_cue` | All WavePlayer references |
| `validate_sound_cue` | Check for issues |
| `build_sound_cue_from_spec` | **Crown jewel** — declarative JSON-to-graph in one call |
| `create_random_sound_cue` | Template: Random node with weights |
| `create_layered_sound_cue` | Template: Mixer node |
| `create_looping_ambient_cue` | Template: Loop + Random + Delay |
| `create_distance_crossfade_cue` | Template: Distance crossfade bands |
| `create_switch_sound_cue` | Template: Switch node |
| `duplicate_sound_cue` | Clone cue |
| `delete_audio_asset` | Delete any audio asset |
| `preview_sound` | Editor playback |
| `stop_preview` | Stop preview |
| `get_sound_cue_duration` | Cached duration |

### MetaSound Graph (25, requires `WITH_METASOUND`)

| Action | Description |
|--------|-------------|
| `create_metasound_source` | Create via Builder API (format, one_shot) |
| `create_metasound_patch` | Create reusable subgraph |
| `add_metasound_node` | Add from registered node classes (NS, Name, Variant) |
| `remove_metasound_node` | Remove node |
| `connect_metasound_nodes` | Wire by input/output name |
| `disconnect_metasound_nodes` | Disconnect by input/output name |
| `add_metasound_input` | Graph-level input with data type + default |
| `add_metasound_output` | Graph-level output |
| `set_metasound_input_default` | Set graph input default |
| `add_metasound_interface` | UE.Source, UE.OutputFormat.Mono, etc. |
| `get_metasound_graph` | Full graph: nodes, edges, inputs, outputs |
| `list_metasound_connections` | All edges |
| `list_available_metasound_nodes` | Enumerate registered node classes |
| `get_metasound_node_info` | Inputs/outputs/types for a class |
| `find_metasound_node_inputs` | Node inputs |
| `find_metasound_node_outputs` | Node outputs |
| `get_metasound_input_names` | Graph inputs with types/defaults |
| `build_metasound_from_spec` | **Crown jewel** — declarative JSON-to-graph in one call |
| `create_metasound_preset` | Preset from existing MetaSound |
| `create_oneshot_sfx` | Template: WavePlayer → Out |
| `create_looping_ambient_metasound` | Template: LFO modulated |
| `create_synthesized_tone` | Template: Osc → Filter → ADSR |
| `create_interactive_metasound` | Template: Crossfade / Switch |
| `add_metasound_variable` | Graph variable |
| `set_metasound_node_location` | Editor layout position |

### Notes

- **Sound Cue connection semantics.** `from` is the child (data source), `to` is the parent (consumer). This matches the `ChildNodes[]` model where the parent holds references to its inputs.
- **MetaSound Builder lifecycle.** For multi-step operations, the builder is cached via `FindOrBeginBuilding()`. If the editor restarts, the builder is lost and individual mutation actions return `METASOUND_BUILDER_LOST`. Use `build_metasound_from_spec` for full graph creation in one call.
- **SoundWave is read-only.** MonolithAudio does not create SoundWaves (they are imported assets). `get_sound_wave_info` reads properties; `batch_set_sound_wave_properties` modifies UPROPERTY fields via reflection.
- **No overlap with runtime audio playback plugins.** A runtime audio plugin owns footstep/surface/movement audio playback; MonolithAudio owns editor-time asset creation, management, and inspection.

---

## MonolithReflectionIntel — `decision` (5 actions)

**New in v0.17.0.** Deterministic markdown decision-record harvest from `Docs/`, plans, `CHANGELOG.md`, `.claude/rules/`. Three heuristic tiers with distinct confidence floors: YAML frontmatter `decision: true` / `status:` (0.90), `## ADR-N` / `## Architectural Decision` headers (0.85), markdown header followed within 8 lines by a paragraph containing `because` / `rationale` / `evidence` / `decision:` (0.65). Writes into two new SQLite tables (`decision_records`, `decision_supersedes`) on top of `EngineSource.db`. Lazy bootstrap on first call; `FCoreUObjectDelegates::ReloadCompleteDelegate` refresh on Live Coding / UBT hot-reload. All five actions are read-only + idempotent; `path_filter` carries `EMonolithParamKind::DiskPath` for automatic `\` → `/` rewrite.

| Action | Description |
|--------|-------------|
| `list_decisions` | List harvested records. Filterable by `path_filter` substring + `min_confidence` floor. Cursor-paginated. |
| `get_decision` | Return a single record by stable id |
| `list_stale` | Records whose source markdown has gone untouched past a configurable mtime cutoff. Cursor-paginated. |
| `find_supersession_chain` | Transitive walk over `decision_supersedes` outward from a starting record |
| `find_referent_decisions` | Inverse — records that explicitly supersede the given id |

---

## MonolithReflectionIntel — `risk` (5 actions)

**New in v0.17.0.** Repo-level risk signals mined from git via `FPlatformProcess::CreateProc` against each nested repo's `.git/`. Hotspot score formula is deterministic and traceable: `0.6 * normalised_churn + 0.4 * normalised_loc`. Co-change pair detection caps per-commit file count at `MaxCommitFileCount` (default 50) to suppress tree-wide refactor + initial-import noise. Writes into four new SQLite tables: `git_file_churn`, `git_cochange_pairs`, `risk_hotspot_scores`, `reflect_conditional_gates`. All five actions are read-only + idempotent.

| Action | Description |
|--------|-------------|
| `get_hotspot_score` | Per-file blended churn × LOC score |
| `get_cochange_pairs` | Files that frequently change together with a given anchor file. Cursor-paginated. |
| `get_file_churn` | Per-file commit count + line delta + first/last commit timestamp |
| `get_release_window_hotspots` | Files whose score exceeds a configurable threshold. Cursor-paginated. |
| `list_conditional_gates` | All `#if WITH_*` macros + `bHas*` 3-location probes + `MONOLITH_RELEASE_BUILD` bypasses across the project. Cursor-paginated. |

**Plus `source_query("audit_module_dep_reality")`** (registered cross-module onto the `source` namespace) — catches the LNK2019 bug class where UPROPERTY (or any reflection-touching declaration) references a foreign-module type whose owning module is missing from the declaring module's `Build.cs` `Private/PublicDependencyModuleNames`. Regex-parses every `*.Build.cs` for declared deps and every `*.h` / `*.cpp` reflection declaration for type references, resolves against `EngineSource.db`'s symbol → owning module map, flags violations. Cursor-paginated with `module_filter` substring scope.

---

## MonolithReflectionIntel — `cppreflect` (6 actions)

**New in v0.17.0 (Phase 3a); `list_class_specifiers` added in v0.19.0.** UE 5.7 reflection-edge queries driven by direct reads of UHT artefacts (`Intermediate/Build/Win64/.../Inc/<Module>/UHT/*.gen.cpp`) cross-joined with `IAssetRegistry::GetDependencies`. **No tree-sitter dependency, no ThirdParty vendoring** — substrate is deterministic file IO plus 8 regex patterns derived from real `.gen.cpp` inspection. Writes into six new SQLite tables: `reflect_uclasses`, `reflect_uproperties`, `reflect_ufunctions`, `reflect_uinterfaces`, `reflect_uinterface_impls`, `cpp_asset_edges`. All six actions are read-only + idempotent; the list-style actions are cursor-paginated.

**Scan scope (v0.19.0):** the indexers no longer scan the project game module alone — scope is driven by `IPluginManager::GetEnabledPlugins()` along a game-module → project-plugin → marketplace ladder. `bIndexProjectPluginReflection` (default **`true`**) walks every enabled `LoadedFrom == Project` plugin (your project plugins); `bIndexMarketplacePluginReflection` (default **`false`**) also walks enabled engine-installed marketplace plugins (installed marketplace plugins). Epic engine built-ins stay excluded (separate `bIndexEnginePluginReflection`, default off). Both settings live under Category "CppReflect" on `UMonolithReflectionIntelSettings`.

| Action | Description |
|--------|-------------|
| `get_uclass` | UHT-derived UCLASS record — parent class, specifiers, source path |
| `list_uproperties` | Per-class UPROPERTY surface with type, blueprint_visibility, specifiers. Cursor-paginated. |
| `list_ufunctions` | Per-class UFUNCTION surface with raw `EFunctionFlags` + return type + per-param JSON. Cursor-paginated. |
| `find_interface_impls` | Every C++ UCLASS implementing a given UINTERFACE (Blueprint impls handled separately via the asset graph). Cursor-paginated. |
| `find_class_specifier` | Every UCLASS carrying a given specifier. Matches the `flags` column of `reflect_uclasses`, which stores UHT metadata keys (`IsBlueprintBase`, `BlueprintType`, `Abstract`), NOT raw C++ specifiers. v0.19.0 forgiveness: alias map (`Blueprintable` → `IsBlueprintBase`), honest not-captured note for specifiers UHT drops (`MinimalAPI`, `NotBlueprintable`), case-insensitive. Cursor-paginated. |
| `list_class_specifiers` | **v0.19.0.** Returns the distinct universe of tokens stored in the `flags` column of `reflect_uclasses`, each with a per-token class count. Discovery companion — tells you what `find_class_specifier` can actually match. No params. |

**Phase 3a known limitations** (documented in `Docs/specs/SPEC_MonolithReflectionIntel.md` §5.7):
- `source_path` on the new tables is UHT's `ModuleRelativePath`, not the canonical project-relative path the Phase 1+2 tables use.
- `source_line` is `0` everywhere — UHT discards the original-header line during code generation.
- `cpp_asset_edges.edge_kind` is coarse `'package_dep'`.
- `reflect_uproperties.blueprint_visibility` / `.specifiers` are empty strings — Phase 3b populates fully.

---

## MonolithReflectionIntel — `network` (4 actions)

**New in v0.17.0 (Phase 4a).** UE 5.7 replication inspection driven by a second UHT-artefact regex sweep (independent of Phase 3a's reader) over per-property `MetaData` blocks plus the `CPF_Net` property-flag emission, cross-joined with Phase 3a's `reflect_ufunctions`. Writes into one new SQLite table: `reflect_replicated_properties`. All four actions are read-only + idempotent; all four cursor-paginated. As of the v0.19.0 scan-scope ladder, the sweep covers project plugins by default (marketplace plugins when enabled), so replicated classes and RPCs declared in project plugins are in scope — not just the game module.

| Action | Description |
|--------|-------------|
| `list_replicated_classes` | UCLASSes carrying at least one replicated property. Captures bare `UPROPERTY(Replicated)` + `DOREPLIFETIME` (via `CPF_Net`) in addition to `ReplicatedUsing` — verified E2E against a real project's replicated character/attribute classes. Sortable by replicated-property count. Cursor-paginated. |
| `list_rpc_functions` | Filter `reflect_ufunctions` by replication specifier (`reflect_ufunctions.specifiers` from `EFunctionFlags`) as of v0.19.0. **Covers project plugins by default** (v0.19.0 scan-scope ladder), so it returns the project's actual RPCs — verified E2E (the project's project-plugin Server RPCs). Cursor-paginated. |
| `list_onrep_handlers` | Every `OnRep_*` UFUNCTION paired with the property it covers via `reflect_replicated_properties.rep_notify_func` join. Cursor-paginated. |
| `audit_unbalanced_onreps` | Catches typo + rename drift — `ReplicatedUsing=OnRep_X` declarations whose `OnRep_X` function does not exist. Cursor-paginated. |

**Known status notes** (documented in `Docs/specs/SPEC_MonolithReflectionIntel.md` §6.5):
- Bare `UPROPERTY(Replicated)` capture **WORKS** as of the v0.19.0 network-completeness workstream — `list_replicated_classes` picks it up via `CPF_Net`, verified E2E. (Was a Phase 4b WISHLIST item; now landed.)
- `list_rpc_functions` **covers project plugins by default** as of the v0.19.0 scan-scope ladder — detection is specifier-based (`EFunctionFlags`) and the indexer now walks enabled `LoadedFrom == Project` plugins, so the project's actual RPCs (verified E2E against the project's project-plugin RPCs) are in scope. The prior "empty because game-module-only" limitation is resolved. Enabled marketplace plugins are covered when `bIndexMarketplacePluginReflection` is on; Epic engine built-ins remain excluded by default.
- Multi-condition replication (`COND_OwnerOnly`, `COND_SkipOwner`, etc.) not surfaced — `rep_kind` carries `'rep_notify'` (and post-workstream `'bare_replicated'` for `CPF_Net`-only properties).

---

## MonolithReflectionIntel — `pipeline` (2 composer actions)

**New in v0.17.0 (Phase 4a).** Read-only composer actions that fan out other registered Monolith actions serially on the game thread and aggregate results into a single payload. Both composers are read-only end-to-end — every action they invoke is itself `readOnlyHint: true`. Fan out is serial; no `ParallelFor`, no async dispatch.

| Action | Description |
|--------|-------------|
| `pr_review` | Changed-files PR review composer. For each path in `changed_files[]`, calls `risk_query("get_hotspot_score")` + `risk_query("get_cochange_pairs")` + `decision_query("list_decisions", path_filter=path)` + `source_query("audit_module_dep_reality")` + optional `blueprint_query("audit_cdo_drift")`, aggregates per-path. Hard cap 100 paths per call. |
| `release_readiness` | Release pre-flight composer. Bundles `monolith_status()` + `decision_query("list_stale")` + `risk_query("get_release_window_hotspots")` + the sentinel-list audit + CHANGELOG completeness audit specced in `.claude/rules/scoped/monolith-release.md`. |

---

## MonolithReflectionIntel — `reflect` (1 action)

**New in v0.19.0 (network-completeness workstream).** One WRITE/maintenance action for repopulating the RI reflection tables. Exists because after an RI indexer code change there's no other clean repopulation trigger — the lazy bootstrap only fires on table-absence, `OnReloadComplete` only on Live Coding, and `source_query("trigger_reindex")` is the heavyweight full-engine reindex.

| Action | Description |
|--------|-------------|
| `rebuild_reflection_index` | **No params.** Project-only force-rebuild of the RI reflection tables — `reflect_uclasses`, `reflect_uproperties`, `reflect_ufunctions`, `reflect_uinterfaces`, `reflect_uinterface_impls`, `cpp_asset_edges`, and `reflect_replicated_properties`. Re-runs the RI indexers (`FCppReflectIndexer` + `FNetworkIndexer`) over PROJECT UHT artefacts only (Epic engine built-ins excluded). As of the v0.19.0 scan-scope ladder, "project" includes enabled `LoadedFrom == Project` plugins by default (and marketplace plugins when enabled), so a rebuild repopulates project-plugin reflection — `network_query("list_rpc_functions")` returns the project's RPCs after a rebuild. WRITE/maintenance (NOT read-only), idempotent (wipe-and-rewrite per indexer in a single transaction), non-destructive (regenerates deterministically from on-disk artefacts). Returns a per-table row-count summary. |

---

## Gameplay Tags, Input, Localization and Chooser

These namespaces don't have their own sections on this page yet. Their full action lists and params are in [API_REFERENCE.md](https://github.com/tumourlove/monolith/blob/master/Docs/API_REFERENCE.md) (sections `gameplay_tags`, `input`, `localization`, `chooser`), and `monolith_discover("<namespace>")` lists them live.

---

## Cross-namespace audit actions (Phase 4a)

**New in v0.17.0.** Owned by `MonolithReflectionIntel` but registered onto the host namespace's adapter for caller ergonomics — agents already discover the host namespace first. All four are read-only + cursor-paginated; `path_prefix` carries `EMonolithParamKind::AssetPath` for automatic `\` → `/` rewrite.

| Host namespace | Action | Description |
|----------------|--------|-------------|
| `material` | `audit_orphan_materials` | `/Game/` path scan via `IAssetRegistry::GetReferencers` for materials with zero inbound references |
| `niagara` | `audit_cross_asset_refs` | Broken/stale asset reference scan over Niagara systems/emitters joined against Phase 3a's `cpp_asset_edges` |
| `blueprint` | `audit_cdo_drift` | Blueprint child CDOs that override a native C++ parent's default value — catches drift when a native default changes upstream and BP children silently keep the stale override |
| `project` | `audit_orphan_assets` | Project-wide zero-reference scan across `IAssetRegistry::GetReferencers` cross-validated against Phase 3a's `cpp_asset_edges`. Surfaces assets referenced only from C++ but not from BP / asset graph. |
