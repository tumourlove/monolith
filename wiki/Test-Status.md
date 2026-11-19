# Test Status

Last updated: 2026-06-07 (formal test-pass dates inline) | Version: 0.18.1 (Beta)

Legend: **PASS** = Verified working | **UNTESTED** = Not yet verified | **SKIP** = Stub/unimplemented

**Current baseline is approximate: 1,500+ in-tree actions across 27 namespaces (public, in-tree only; all active by default, plus +45 experimental town-gen actions when registered).** Query `monolith_discover()` for the exact live figure. Live `monolith_status` reports a higher total when extension plugins outside the release zip are loaded — those advisory totals do not affect the public baseline. The per-namespace tables below capture the formal test results from the 2026-04-01 audit pass — most have not been re-tested since, but the underlying action counts are now updated to v0.17.0 reality. CommonUI (51 actions) shipped v0.14.0 (2026-04-19) and was tested M0.5.1 (2026-04-25) with 8 bugs fixed. MonolithGAS: 53/53 PASS (12 bugs fixed). MonolithMesh: action-level testing in progress. MonolithUI: 36/42 PASS for the UMG baseline; CommonUI tested M0.5.1; 0.15.0 gap-closure actions PIE-smoke-tested at release. MonolithAI / LogicDriver / ComboGraph: functional through integration testing, per-action matrices in progress. 1 Editor action SKIP (`trigger_build`). 2 Animation actions UNTESTED (no AnimComposite assets in the test project). v0.14.9 added 4 new actions PIE-smoke-tested at release. v0.14.10 added 12 MetaSound document introspection actions in `audio_query` (all 12 happy-path + 11 negative-path tests PASS) plus `FMetaSoundIndexer`. v0.15.0 added the `bulk_fill` + `describe` ergonomics framework, the blueprint dataset read/edit pack, UI/Blueprint gap-closure actions, and `monolith_guide` — smoke-tested at release. v0.16.0 added 4 editor preview/inspection actions plus 3 new `capture_scene_preview` asset types — 9 automation tests, all PASS at release; plus persistence fixes (Issue #63) and the MCP `initialize` schema-discovery hint (Issue #62) — non-blocking smoke at release.

**v0.17.0 — Reflection Intelligence automation tests:** `Monolith.ReflectionIntel.Decision.*` (4 tests — `SchemaBootstrap`, `HeuristicAccuracy`, `SupersessionChain`, `StalenessFlag`) PASS. `Monolith.ReflectionIntel.Risk.*` (6 tests — `RiskSchemaBootstrap`, `ChurnAggregation`, `CoChangePairSymmetry`, `HotspotScoreFormula`, `ConditionalGateSweep`, `MonsterCommitSuppression`) PASS. `Monolith.ReflectionIntel.ModuleDepReality.*` (tests covering Build.cs parsing, symbol resolution, implicit-deps whitelist, template extraction) PASS. `Monolith.ReflectionIntel.CppReflect.*` (4 tests — `CppReflectSchemaBootstrap`, `UClassFixtureExtraction`, `InterfaceImplResolution`, `AssetGraphJoin`) PASS. Phase 4a (network + pipeline + 4 cross-namespace audits) — manual smoke only, automation tests deferred.

**Reflection Intelligence — network-completeness workstream:** new `reflect` namespace with `reflect_query("rebuild_reflection_index")` (WRITE/maintenance, no params) — manual smoke. `list_replicated_classes` bare-`Replicated` capture (via `CPF_Net`) **verified E2E** against a real project's replicated character/attribute classes (15 replicated classes with project plugins in scope). `list_rpc_functions` switched to specifier-based detection and, with the plugin scan-scope expansion (`IPluginManager`-driven, project plugins default-on), now **verified E2E** — returned the project's project-plugin Server RPCs. The prior "empty because game-module-only" status is resolved. E2E artefact counts: game module alone ~30 → with project plugins on 337 → with marketplace flag also on 927 (745 UClasses).

**v0.17.0 — MCP LLM Ergonomics Pack automation tests:** Phase 1.0 added +11 tests under `Monolith.ResponseShaping.*` and `Monolith.ParamKind.*` PASS. Phase 2 added +10 under `Monolith.FuzzyMatch.*` and `Monolith.CursorPagination.*` PASS. Phase 1.1 (87-file AssetPath/DiskPath sweep) verified by UBT clean exit + manual smoke at release. Niagara temporal-control surface (9 actions) + stateless-emitter factory (1 action) PIE-smoke-tested at release.

> **Note on counts in the per-module tables below.** The action counts in the table headers reflect the v0.17.0 ground truth. The PASS/UNTESTED/SKIP rows are from the 2026-04-01 test pass and may have been superseded by actions added in subsequent versions (Phase J F1/F4/F8/F11/F17/F18/F22 in particular added 5+ actions across `editor`, `gas`, `ai`, and `audio`; v0.14.8 added Editor +2 + LevelSequence module; v0.14.9 added Editor +2 + Animation +1; v0.14.10 added Audio +12 MetaSound document introspection; v0.15.0 added the bulk_fill/describe framework, blueprint dataset pack +20, UI gap-closure +5, describe `action_schema` +1, and `monolith_guide`; v0.16.0 added Editor +4 preview/inspection actions; v0.17.0 added 5 new namespaces — `decision` 5, `risk` 5, `cppreflect` 5, `network` 4, `pipeline` 2 — plus Niagara +10, plus 4 cross-namespace audit actions on `material` / `niagara` / `blueprint` / `project`, plus the `source_query("audit_module_dep_reality")` cross-namespace audit; the `cppreflect_query("list_class_specifiers")` follow-up then brought `cppreflect` to 6; and the network-completeness `reflect_query("rebuild_reflection_index")` added a 25th namespace `reflect` 1).

---

## MonolithCore

| Action | Status | Notes |
|--------|--------|-------|
| `discover` | **PASS** | 25 namespaces, 1387 in-tree actions. Terse by default (names + one-line descriptions); full schemas via `detail=true` or `describe_query("action_schema")` |
| `discover(namespace)` | **PASS** | All 25 namespaces, correct action counts |
| `status` | **PASS** | Version, port 9316, action count, engine version, project name. Reports extension-plugin union when loaded |
| `update(check)` | **PASS** | GitHub Releases query with SHA256 marker verification |
| `update(install)` | **PASS** | Full end-to-end: download, verify SHA256, stage, swap on exit. Retry loop handles Defender file locks. |
| `reindex` | **PASS** | Triggers successfully |

## MonolithBlueprint (111 actions)

| Action | Status | Notes |
|--------|--------|-------|
| `list_graphs` | **PASS** | 700 bytes, 14 graphs. Clean overview with asset class, parent class |
| `get_graph_summary` | **PASS** | 29KB for EventGraph (5.7x smaller than get_graph_data). Returns all graphs when graph_name empty |
| `get_graph_data` | **PASS** | 164KB full, 8KB with node_class_filter. Filter works well |
| `get_variables` | **PASS** | 8KB, 40 vars. CDO defaults confirmed populated (37/40 non-trivial) |
| `get_execution_flow` | **PASS** | 0.7-2KB. Two-pass FindEntryNode: prefers events/functions, skips comments |
| `search_nodes` | **PASS** | 3.5KB. Case-insensitive across all graphs |
| `get_cdo_properties` | **PASS** | Reads UPROPERTY defaults from Blueprint CDO or UObject asset |
| `batch_execute` | **PASS** | Multiple Blueprint operations in a single call |
| `resolve_node` | **PASS** | Resolves node reference to target |
| `search_functions` | **PASS** | Search functions and events by name |
| `get_node_details` | **PASS** | Full detail dump for a single node |
| `add_nodes_bulk` | **PASS** | Add multiple nodes in one call |
| `connect_pins_bulk` | **PASS** | Connect multiple pin pairs in one call |
| `set_pin_defaults_bulk` | **PASS** | Set multiple pin defaults in one call |
| `scaffold_interface_implementation` | **PASS** | Auto-generate interface stub events |
| `add_timeline` | **PASS** | Add Timeline node to a graph |
| `add_event_node` | **PASS** | Add named event node to a graph |
| `add_comment_node` | **PASS** | Add comment box to a graph |
| `get_function_signature` | **PASS** | Param list and return type for a function |
| `get_blueprint_info` | **PASS** | Comprehensive Blueprint summary |
| `get_event_dispatcher_details` | **PASS** | Full detail for a single event dispatcher |
| `remove_event_dispatcher` | **PASS** | Remove event dispatcher from Blueprint |
| `set_event_dispatcher_params` | **PASS** | Change event dispatcher parameter signature |
| `promote_pin_to_variable` | **PASS** | Promote pin value to Blueprint variable |
| `add_replicated_variable` | **PASS** | Add replicated variable with RepNotify |

## MonolithMaterial (63 actions)

| Action | Status | Notes |
|--------|--------|-------|
| `get_all_expressions` | **PASS** | 8.7K chars, 43 expressions with class, position, params |
| `get_expression_details` | **PASS** | Full property dump with inputs, outputs, UE properties |
| `get_full_connection_graph` | **PASS** | 10K chars, 60 connections |
| `disconnect_expression` | **PASS** | Disconnects material output pins (expr→material property + expr→expr). Tested on Metallic output |
| `build_material_graph` | **PASS** | FindObject→FindFirstObject fix. Short class names work. Tested: 2 nodes + 2 outputs |
| `begin_transaction` | **PASS** | Returns transaction name + status "begun" |
| `end_transaction` | **PASS** | Returns status "ended". Closes transaction cleanly |
| `export_material_graph` | **PASS** | 44K default, 13K with include_properties=false (71% reduction) |
| `import_material_graph` | **PASS** | Both modes: "overwrite" (4 nodes, 3 connections round-trip) and "merge" (adds without clearing) |
| `validate_material` | **PASS** | 0 false positive islands. Added MP_MaterialAttributes + 6 properties, MaterialAttributeLayers BFS seed |
| `render_preview` | **PASS** | Saves PNG to Saved/Monolith/previews/, 68KB at 256x256 |
| `get_thumbnail` | **PASS** | save_to_file=true returns path (164 chars vs 91K base64). Both modes work |
| `create_custom_hlsl_node` | **PASS** | 2 inputs (Time, Speed), output_type Float1, HLSL code, position. Returns expression_name |
| `get_layer_info` | **PASS** | Correctly rejects non-layer materials with clear error |
| `create_material` | **PASS** | Created /Game/Test/M_MonolithTest with Opaque/DefaultLit/Surface defaults |
| `create_material_instance` | **PASS** | Created MIC from test material parent |
| `set_material_property` | **PASS** | Changed blend_mode to Masked |
| `delete_expression` | **PASS** | Deleted custom HLSL expression by name |
| `get_material_parameters` | **PASS** | Returns scalar/vector/texture/static_switch arrays with values |
| `set_instance_parameter` | **PASS** | Set scalar parameter on MIC. Supports scalar, vector, texture, static switch |
| `recompile_material` | **PASS** | Forced recompile via UMaterialEditingLibrary |
| `duplicate_material` | **PASS** | Duplicated material to new asset path |
| `get_compilation_stats` | **PASS** | Returns sampler count, texture estimates, expression count |
| `set_expression_property` | **PASS** | Set DefaultValue on scalar param, verified via get_expression_details |
| `connect_expressions` | **PASS** | Wired expr-to-expr and expr-to-material-property |
| `auto_layout` | **PASS** | Auto-arrange expression nodes in material graph |
| `duplicate_expression` | **PASS** | Duplicate expression node in-place |
| `replace_expression` | **PASS** | Swap expression type, preserving connections |
| `list_expression_classes` | **PASS** | List all available expression class names |
| `get_expression_connections` | **PASS** | All connections into/out of an expression |
| `get_expression_pin_info` | **PASS** | Pin names, types, connection state |
| `move_expression` | **PASS** | Move expression to new graph position |
| `rename_expression` | **PASS** | Rename expression parameter name |
| `get_material_properties` | **PASS** | Material-level properties (blend mode, shading model, etc.) |
| `get_instance_parameters` | **PASS** | List all parameter overrides on material instance |
| `set_instance_parameters` | **PASS** | Set multiple parameters on material instance in one call |
| `set_instance_parent` | **PASS** | Reparent material instance to different material |
| `clear_instance_parameter` | **PASS** | Clear parameter override (revert to parent) |
| `list_material_instances` | **PASS** | Find all material instances derived from a material |
| `save_material` | **PASS** | Explicitly save a material asset |
| `update_custom_hlsl_node` | **PASS** | Update HLSL code on CustomHLSL expression |
| `create_material_function` | **PASS** | Create new UMaterialFunction asset |
| `build_function_graph` | **PASS** | Build material function node graph from spec |
| `get_function_info` | **PASS** | Inputs, outputs, description of material function |
| `batch_set_material_property` | **PASS** | Set property on multiple materials in one call |
| `batch_recompile` | **PASS** | Recompile multiple materials in one call |
| `import_texture` | **PASS** | Import image file as UTexture2D asset |

## MonolithAnimation (119 actions: 96 baseline + 13 PoseSearch + 5 ABP write + 3 ControlRig write + 1 layout + 1 v0.14.9 `copy_bone_pose_between_sequences`)

| Action | Status | Notes |
|--------|--------|-------|
| `add_montage_section` | **PASS** | Created "TestSection_Monolith" at index 1, time 0.5 |
| `delete_montage_section` | **PASS** | Deletes by index. Guards against deleting last section |
| `set_section_next` | **PASS** | Linked TestSection_Monolith → Default |
| `set_section_time` | **PASS** | Moved section time to 0.3 |
| `add_blendspace_sample` | **PASS** | Added sample at (99,99). Skeleton mismatch returns descriptive error |
| `edit_blendspace_sample` | **PASS** | Moved sample from (99,99) to (88,88) |
| `delete_blendspace_sample` | **PASS** | Invalid index returns descriptive error |
| `get_state_machines` | **PASS** | Clean names (no \n). from_type/to_type on transitions |
| `get_state_info` | **PASS** | Validates required params — clear error on empty machine_name |
| `get_transitions` | **PASS** | All from/to resolved. from_type/to_type distinguishes state vs conduit |
| `get_blend_nodes` | **PASS** | 6KB. 27 nodes with pin connections |
| `get_linked_layers` | **PASS** | Found self-layer correctly |
| `get_graphs` | **PASS** | 7KB. 66 graphs, clean names, node/anim_node/SM counts |
| `get_nodes` | **PASS** | 21KB unfiltered, 8KB with graph_name, 450B with class filter |
| `set_notify_time` | **PASS** | UAnimSequenceBase fix — montages now accepted. Tested on A_UE4_FP_Sprint |
| `set_notify_duration` | **PASS** | Same fix. Error message includes (total: N) |
| `set_bone_track_keys` | **PASS** | 2 keys written (position/rotation/scale arrays) |
| `add_bone_track` | **PASS** | Added "root" track successfully |
| `remove_bone_track` | **PASS** | Non-existent bone returns removed_count=0 gracefully |
| `add_virtual_bone` | **PASS** | Validates source/target bones exist. Previously crashed editor |
| `remove_virtual_bones` | **PASS** | Returns error for non-existent bone names |
| `get_skeleton_info` | **PASS** | 27KB. 262 bones + 12 virtual bones with parent chains |
| `get_skeletal_mesh_info` | **PASS** | 8KB. Materials, 181 morph targets, sockets, LODs |
| `get_sequence_info` | **PASS** | Duration, frames, sample rate, root motion, additive, compression |
| `get_sequence_notifies` | **PASS** | All notifies with name, time, duration, class, track |
| `get_bone_track_keys` | **PASS** | Position/rotation/scale key arrays |
| `get_sequence_curves` | **PASS** | Float/transform curves with key counts |
| `get_montage_info` | **PASS** | Sections, slots, blend in/out, rate scale |
| `get_blend_space_info` | **PASS** | Samples + axis config (min/max/name/grid) |
| `get_skeleton_sockets` | **PASS** | Socket name, bone, location/rotation/scale |
| `get_abp_info` | **PASS** | Skeleton, parent class, state machines, graphs, variables |
| `add_notify` | **PASS** | Added PlaySound at t=0.01s. Validates time range |
| `add_notify_state` | **PASS** | Creates state notify with duration |
| `remove_notify` | **PASS** | Removes by index, returns remaining count |
| `set_notify_track` | **PASS** | Moves notify to different track |
| `list_curves` | **PASS** | Full CRUD cycle verified |
| `add_curve` | **PASS** | Added float curve via IAnimationDataController |
| `remove_curve` | **PASS** | Removed test curve cleanly |
| `set_curve_keys` | **PASS** | Set 3 keys. Param is keys_json (string) |
| `get_curve_keys` | **PASS** | Returns time/value/interp_mode per key |
| `add_socket` | **PASS** | Created socket on hand_r. Full CRUD cycle verified |
| `remove_socket` | **PASS** | Removed test socket cleanly |
| `set_socket_transform` | **PASS** | Updated location and rotation |
| `get_skeleton_curves` | **PASS** | 15 curves on Manny skeleton |
| `set_blend_space_axis` | **PASS** | Changed X axis min/max, verified, restored |
| `set_root_motion_settings` | **PASS** | Toggled root motion on/off |
| `create_sequence` | **PASS** | Created asset with skeleton. Bad skeleton returns error |
| `duplicate_sequence` | **PASS** | Duplicated idle loop |
| `create_montage` | **PASS** | Created with Default section + DefaultSlot |
| `set_montage_blend` | **PASS** | Set blend_in=0.5, blend_out=0.3 |
| `add_montage_slot` | **PASS** | Added UpperBody slot. Duplicate name returns error |
| `set_montage_slot` | **PASS** | Renamed slot by index |
| `apply_anim_modifier` | **PASS** | Applied AnimationModifier base class |
| `list_anim_modifiers` | **PASS** | Found BP_CopyCurves_C on Idle_Loop |
| `get_composite_info` | **PASS** | Returns clear error on non-composite |
| `add_composite_segment` | UNTESTED | No AnimComposite assets in project |
| `remove_composite_segment` | UNTESTED | No AnimComposite assets in project |
| `get_pose_search_schema` | **PASS** | sample_rate 30, 2 channels, schema_cardinality 34 |
| `get_pose_search_database` | **PASS** | 2 sequences, schema ref, enabled/sampling data |
| `add_database_sequence` | **PASS** | Added sequence to PoseSearch database |
| `remove_database_sequence` | **PASS** | Removed sequence from PoseSearch database |
| `get_database_stats` | **PASS** | 368 poses, PCAKDTree mode, is_valid=true |
| `get_ikrig_info` | **PASS** | Chains, goals, solvers, retarget root |
| `add_ik_solver` | **PASS** | Added PBIK solver to IKRig |
| `get_retargeter_info` | **PASS** | Source/target rigs, chain mappings, auto-map status |
| `set_retarget_chain_mapping` | **PASS** | Set/update chain mapping on IK Retargeter |
| `get_control_rig_info` | **PASS** | Hierarchy: bones, controls, nulls, curves |
| `get_control_rig_variables` | **PASS** | Variables on Control Rig Blueprint |
| `add_control_rig_element` | **PASS** | Add bone, control, or null to hierarchy |
| `get_abp_variables` | **PASS** | Variables with type info and default values |
| `get_abp_linked_assets` | **PASS** | Assets linked to Animation Blueprint |
| `add_state_to_machine` | **PASS** | Add new state to AnimBP state machine |
| `add_transition` | **PASS** | Add transition between two states |
| `set_transition_rule` | **PASS** | Set condition expression on transition |

## MonolithNiagara (109 actions)

| Action | Status | Notes |
|--------|--------|-------|
| `add_emitter` | **PASS** | Requires valid NiagaraEmitter asset path. Returns handle_id |
| `remove_emitter` | **PASS** | Removes by handle ID or name |
| `duplicate_emitter` | **PASS** | Duplicates with all settings (sim_target, renderers) |
| `set_emitter_enabled` | **PASS** | Enable/disable/re-enable cycle verified |
| `reorder_emitters` | **PASS** | Param is `order`. Verified reorder + restore |
| `set_emitter_property` | **BUG** | Bare JSON primitives fail UE's `FJsonSerializer::Deserialize`. Workaround: use object-wrapped values |
| `request_compile` | **PASS** | Triggers system recompile |
| `create_system` | **PASS** | Creates empty system at `save_path` |
| `get_ordered_modules` | **PASS** | Usage filter + shorthands ("spawn", "update"). Invalid = error with valid list |
| `get_module_inputs` | **PASS** | All input types: floats, vectors, colors, DIs, enums, bools, positions, quaternions |
| `get_module_graph` | **PASS** | Full node graph with pins and links |
| `add_module` | **PASS** | Pre-call graph validation. Adds to specified stage |
| `remove_module` | **PASS** | Splice via ParameterMap pins |
| `move_module` | **PASS** | Remove+re-add strategy via ParameterMap pins |
| `set_module_enabled` | **PASS** | Enables/disables module in stack |
| `set_module_input_value` | **PASS** | Sets literal values. BreakAllPinLinks guard for bound inputs |
| `set_module_input_binding` | **PASS** | Binds inputs to parameters. Static switch guard. Accepts short names |
| `set_module_input_di` | **PASS** | Validates input exists and is DI type. Full input enumeration via engine API |
| `create_module_from_hlsl` | **PASS** | Creates .niagaraFunction asset. Inputs overridable via set_module_input_value. Dot validation rejects dotted I/O names with clear error |
| `create_function_from_hlsl` | **PASS** | Creates function-usage .niagaraFunction asset. Same param schema and HLSL body rules as create_module_from_hlsl |
| `get_all_parameters` | **PASS** | Filtering: scope, emitter, combined |
| `get_user_parameters` | **PASS** | Clean, compact output |
| `get_parameter_value` | **PASS** | Both User.X and X formats work |
| `get_parameter_type` | **PASS** | Returns size, primitiveness, is_di, is_enum |
| `trace_parameter_binding` | **PASS** | Both prefix formats work |
| `add_user_parameter` | **PASS** | Fallback param names + empty-name guard |
| `remove_user_parameter` | **PASS** | Case-insensitive comparison |
| `set_parameter_default` | **PASS** | Case-insensitive comparison |
| `set_curve_value` | **PASS** | All 3 DI curve types: float, color, vector2D |
| `add_renderer` | **PASS** | Rejects abstract base class |
| `remove_renderer` | **PASS** | Removes by index |
| `set_renderer_material` | **PASS** | Sets material on renderer |
| `set_renderer_property` | **PASS** | Sets property by name |
| `get_renderer_bindings` | **PASS** | Clean JSON: name/bound_to/type |
| `set_renderer_binding` | **PASS** | Binds renderer properties to parameters |
| `batch_execute` | **PASS** | All 23 write ops in dispatch table. Per-op results array |
| `create_system_from_spec` | **PASS** | Creates system with emitters from spec JSON. Validates emitter paths |
| `list_emitters` | **PASS** | Name, index, enabled, sim_target, renderer_count |
| `list_renderers` | **PASS** | Renderer class, index, enabled, material |
| `list_module_scripts` | **PASS** | Keyword search across available Niagara module scripts |
| `list_renderer_properties` | **PASS** | Editable properties on a renderer by emitter + renderer index |
| `set_system_property` | **PASS** | WarmupTime, bDeterminism, bFixedTickDelta, RandomSeed, MaxPoolSize. Snake_case aliases supported |
| `set_static_switch_value` | **PASS** | Bool, enum (by value name), or int static switch inputs on modules |
| `get_di_functions` | **PASS** | Simple name lookup works |
| `get_compiled_gpu_hlsl` | **PASS** | Auto-compiles if HLSL not cached |
| `get_system_summary` | **PASS** | Emitter count, renderer count, param count, compile status |
| `get_emitter_summary` | **PASS** | Module count, renderer count, sim target, module names |
| `list_emitter_properties` | **PASS** | Editable UPROPERTY fields grouped by category |
| `get_module_input_value` | **PASS** | Current value of a single module input |
| `configure_curve_keys` | **PASS** | Set full key list on curve DI in one call |
| `configure_data_interface` | **PASS** | Set multiple DI properties in one call |
| `duplicate_system` | **PASS** | Deep-copies system with override table |
| `set_fixed_bounds` | **PASS** | Fixed world-space bounds on Niagara system |
| `set_effect_type` | **PASS** | Assign effect type asset to system |
| `create_emitter` | **PASS** | Create standalone Niagara emitter from scratch |
| `export_system_spec` | **PASS** | Export system spec as JSON |
| `add_dynamic_input` | **PASS** | Add dynamic input module to module input slot |
| `set_dynamic_input_value` | **PASS** | Set value on dynamic input module |
| `search_dynamic_inputs` | **PASS** | Search available dynamic input scripts |
| `add_event_handler` | **PASS** | Add event handler stage to emitter, returns handler index |
| `validate_system` | **PASS** | GPU+Light conflicts, missing materials, bounds warnings |
| `add_simulation_stage` | **PASS** | Add simulation stage to GPU emitter |

## MonolithEditor (33 actions)

| Action | Status | Notes |
|--------|--------|-------|
| `trigger_build` | **SKIP** | Not tested to avoid disruption |
| `live_compile` | **PASS** | Compiled 3 modules |
| `get_build_errors` | **PASS** | error_count + warning list |
| `get_build_status` | **PASS** | Live coding status fields |
| `get_build_summary` | **PASS** | Summary with counts |
| `search_build_output` | **PASS** | Pattern search works |
| `get_recent_logs` | **PASS** | Default 100, max/count params work |
| `search_logs` | **PASS** | Pattern, category, verbosity filters |
| `tail_log` | **PASS** | Returns 50 formatted lines |
| `get_log_categories` | **PASS** | 134 categories |
| `get_log_stats` | **PASS** | Total/fatal/error/warning/log/verbose counts |
| `get_compile_output` | **PASS** | Structured compile report |
| `get_crash_context` | **PASS** | Returns recent_errors |

## MonolithConfig (6 actions)

| Action | Status | Notes |
|--------|--------|-------|
| `resolve_setting` | **PASS** | Engine/Game/Input categories |
| `explain_setting` | **PASS** | Convenience + explicit modes, layer breakdown |
| `diff_from_default` | **PASS** | Section filter, change_type |
| `search_config` | **PASS** | Category filter works |
| `get_section` | **PASS** | Category resolution works |
| `get_config_files` | **PASS** | 38 files, 3 hierarchy levels |

## MonolithIndex / Project (7 actions)

| Action | Status | Notes |
|--------|--------|-------|
| `search` | **PASS** | FTS5 ranking, match highlighting, default limit 50 |
| `find_references` | **PASS** | Bidirectional deps/referencers |
| `find_by_type` | **PASS** | 5 types tested, pagination works |
| `get_stats` | **PASS** | 11 tables, 9,571 assets, 80,991 nodes |
| `get_asset_details` | **PASS** | Deep data: nodes, vars, refs |

## MonolithSource (11 actions)

| Action | Status | Notes |
|--------|--------|-------|
| `read_source` | **PASS** | Full source, 137K chars for AActor |
| `read_source` (members_only) | **PASS** | Members with bodies omitted |
| `find_references` | **PASS** | 50+ type references |
| `find_callers` | **PASS** | 50+ callers with file/line info |
| `find_callees` | **PASS** | Call graph with function names |
| `search_source` | **PASS** | Symbol + source line matches |
| `get_class_hierarchy` | **PASS** | 62K definitions, 37K inheritance links |
| `get_module_info` | **PASS** | Path, type, file count, key classes |
| `get_symbol_context` | **PASS** | Implementation + 20 lines context |
| `read_file` | **PASS** | Relative path suffix matching |
| `trigger_project_reindex` | UNTESTED | New in v0.8.0 — incremental project C++ re-index |

---

## MonolithGAS (135 actions: 131 gas + 4 ui aliases)

53/53 tests PASS for the original suite. 12 bugs fixed across 8 commits. All 4 implementation phases complete. Phase J F4 added a baseline vitals AttributeSet template (untested at this writing — no formal coverage yet).

| Category | Actions | Status | Notes |
|----------|---------|--------|-------|
| Abilities | 14 | **53/53 PASS** | Tested via PIE runtime. All activation policies verified |
| Attributes | 16 | | C++ and Blueprint (GBA) attribute sets tested |
| Effects | 18 | | Duration, stacking, period, conditional application all verified |
| ASC | 12 | | Grant/revoke/apply/remove cycles tested |
| Tags | 10 | | Hierarchical queries, container ops verified |
| Cues | 10 | | Create, trigger, inspect verified |
| Targeting | 12 | | Trace and actor targeting verified |
| Input | 8 | | Enhanced Input binding verified |
| Inspect | 16 | | Runtime debugging tools verified |
| Scaffold | 14 | | Template generation verified |

Key bugs fixed: `IGameplayTagsEditorModule` API compatibility, `EnsureAssetPathFree` 3-tier guard, `BS_BeingCreated` suppression, Asset Registry pre-filter, GAS deep indexer registration. `WITH_GBA=1` and `WITH_GBA=0` compile clean.

## MonolithAI (221 actions)

Verified functional through integration testing. Per-action test matrix in progress. Phase J F1 hardened BT crash handling. Phase J F8 added `add_perception_to_actor` and `get_bt_graph`. Phase J F22 retrofit moved `WITH_STATETREE` and `WITH_SMARTOBJECTS` to proper conditional gating.

| Category | Actions | Status | Notes |
|----------|---------|--------|-------|
| Behavior Trees | ~30 | Functional | BT CRUD, node management, build_behavior_tree_from_spec verified |
| Blackboards | ~15 | Functional | Key CRUD with all types verified |
| State Trees | ~25 | Functional | ST CRUD, build_state_tree_from_spec verified. Requires WITH_STATETREE |
| EQS | ~20 | Functional | Query/generator/test CRUD verified |
| Smart Objects | ~15 | Functional | Definition/slot management verified. Requires WITH_SMARTOBJECTS |
| AI Controllers | ~15 | Functional | Controller creation, BT/BB assignment verified |
| Perception | ~20 | Functional | Sense configuration verified |
| Navigation | ~15 | Functional | Path queries, filter management verified |
| Runtime/PIE | ~20 | Functional | Start/stop BT, BB inspection verified in PIE |
| Scaffolding | ~20 | Functional | Template generation verified |
| Discovery | ~15 | Functional | Asset listing and search verified |
| Advanced | ~19 | Functional | Verified through usage |

## MonolithLogicDriver (66 actions)

Verified functional through integration testing. Per-action test matrix in progress. Conditional on `#if WITH_LOGICDRIVER`.

| Category | Actions | Status | Notes |
|----------|---------|--------|-------|
| SM CRUD | 7 | Functional | Create, inspect, compile, list verified |
| Graph Read/Write | 12 | Functional | State/transition management verified |
| Node Config | 8 | Functional | Property editing via reflection verified |
| Runtime/PIE | 6 | Functional | Start/stop, state queries verified in PIE |
| JSON Spec | 5 | Functional | build_sm_from_spec verified |
| Scaffolding | 10 | Functional | Template generation verified |
| Components | 5 | Functional | Component management verified |
| Text Graph | 3 | Functional | Visualization verified |
| Discovery | 10 | Functional | Asset listing verified |

## MonolithComboGraph (13 actions)

Verified functional through integration testing. Per-action test matrix in progress. Conditional on `#if WITH_COMBOGRAPH`.

| Category | Actions | Status | Notes |
|----------|---------|--------|-------|
| Read | 4 | Functional | Graph inspection verified |
| Create | 5 | Functional | Node/edge creation verified |
| Scaffold | 4 | Functional | Ability scaffolding, auto_layout verified |

## MonolithMesh (194 core actions, +45 experimental town gen off by default)

Core mesh functionality verified through integration testing. Per-action test matrix in progress.

| Tier | Actions | Status | Notes |
|------|---------|--------|-------|
| Mesh Inspection | 12 | Functional | Verified through usage |
| Scene Manipulation | 8 | Functional | Verified through usage |
| Spatial Queries | 11 | Functional | Verified through usage |
| Level Blockout | 15 | Functional | Verified through usage |
| GeometryScript | 12 | Functional | Requires GeometryScriptingEditor plugin |
| Horror Analysis | 8 | Functional | Verified through usage |
| Accessibility | 6 | Functional | Verified through usage |
| Lighting | 5 | Functional | Verified through usage |
| Audio/Acoustics | 14 | Functional | Verified through usage |
| Performance | 5 | Functional | Verified through usage |
| Decals | 4 | Functional | Verified through usage |
| Level Design | 9 | Functional | Verified through usage |
| Volumes | 7 | Functional | Verified through usage |
| Horror Intelligence | 4 | Functional | Verified through usage |
| Tech Art | 7 | Functional | Verified through usage |
| Advanced Level Design | 8 | Functional | Verified through usage |
| Context Props | 8 | Functional | Verified through usage |
| Procedural Geometry | 8 | Functional | Verified through usage |
| Genre Presets | 8 | Functional | Verified through usage |
| Encounter Design | 8 | Functional | Verified through usage |
| Quality & Polish | 9 | Functional | Verified through usage |
| Town Gen (experimental) | 45 | **EXPERIMENTAL** | Known geometry issues — wall misalignment, room separation |

## MonolithBABridge

| Test | Status | Notes |
|------|--------|-------|
| WITH_BLUEPRINT_ASSIST=0 compile | **PASS** | Empty shell, no impact on core |
| WITH_BLUEPRINT_ASSIST=1 compile | **PASS** | BA formatter registered |
| auto_layout fallback | **PASS** | Uses Sugiyama when BA absent |

---

## Integration Tests

| Test | Status | Notes |
|------|--------|-------|
| Plugin loads without errors | **PASS** | No LogMonolith errors on startup |
| MCP server starts on configured port | **PASS** | tools/list returns 23 tools |
| All 19 namespaces register actions | **PASS** | discover returns all 19 in-tree namespaces |
| Project auto-indexes on first launch | **PASS** | Deferred to OnFilesLoaded, indexes 9,560+ assets |
| Stateless server | **PASS** | No session tracking |
| Cross-platform update extraction | **PASS** | Windows verified, macOS/Linux untested |
| Hot-swap plugin on editor exit | **PASS** | v0.4.0→v0.5.0 full cycle |
| editor.live_compile | **PASS** | Compiled 3 modules via MCP |
| Per-action param schemas | **PASS** | Lazy-fetch via `describe_query("action_schema")` or `discover(detail=true)`; discover is terse by default |
| MCP proxy auto-reconnect | **PASS** | Survives editor restart, tools/list_changed sent |
| GAS PIE runtime tests | **PASS** | 53/53 pass, abilities activate and effects apply |
| `MONOLITH_RELEASE_BUILD=1` strips optional deps | **PASS** | All `WITH_*` macros forced to 0; release zip loads cleanly without GBA/ComboGraph/LogicDriver/CommonUI/MetaSound/StateTree/SmartObjects/MassEntity/ZoneGraph/BlueprintAssist |
| MonolithAI module loads | **PASS** | 221 actions registered when WITH_STATETREE + WITH_SMARTOBJECTS |
| MonolithLogicDriver module loads | **PASS** | 66 actions registered when Logic Driver Pro installed |
| MonolithComboGraph module loads | **PASS** | 13 actions registered when ComboGraph installed |
| MonolithUI CommonUI actions register | **PASS** | 51 CommonUI actions (M0.5.1, 2026-04-25) when WITH_COMMONUI=1 |
| Extension plugin registry sharing | **PASS** | External extension plugins register into shared FMonolithToolRegistry |
| ReflectionIntel decision_query | **PASS** | 4 automation tests pass (SchemaBootstrap, HeuristicAccuracy, SupersessionChain, StalenessFlag) |
| ReflectionIntel risk_query | **PASS** | 6 automation tests pass (RiskSchemaBootstrap, ChurnAggregation, CoChangePairSymmetry, HotspotScoreFormula, ConditionalGateSweep, MonsterCommitSuppression) |
| ReflectionIntel cppreflect_query | **PASS** | 4 automation tests pass (CppReflectSchemaBootstrap, UClassFixtureExtraction, InterfaceImplResolution, AssetGraphJoin) |
| ReflectionIntel source audit | **PASS** | Build.cs parsing, EngineSource.db symbol resolution, implicit-deps whitelist, template-argument extraction all covered |
| ReflectionIntel network_query | **UNTESTED** | Phase 4a manual smoke at release; automation tests deferred |
| ReflectionIntel pipeline_query | **UNTESTED** | Phase 4a manual smoke at release; automation tests deferred |
| MCP LLM Ergonomics response shaping | **PASS** | 11 tests pass (Monolith.ResponseShaping.* + Monolith.ParamKind.*) |
| MCP LLM Ergonomics fuzzy match | **PASS** | 5 tests pass (Monolith.FuzzyMatch.*) |
| MCP LLM Ergonomics cursor pagination | **PASS** | 5 tests pass (Monolith.CursorPagination.*) |
| monolith_proxy.exe | **PASS** | Stdio proxy keeps session alive across editor restart |
| monolith_query.exe | **PASS** | All 14 offline actions return correct results |

---

## Summary

| Module | Actions (v0.17.0) | PASS (2026-04-01 audit) | UNTESTED | BUG | SKIP |
|--------|-------------------|--------------------------|----------|-----|------|
| Core (`monolith` meta) | 5 | 4 | 1 (`guide`) | 0 | 0 |
| `bulk_fill` framework | 2 | — | — | 0 | 0 |
| `describe` framework | 3 | — | — | 0 | 0 |
| Blueprint | 112 | 86 | 26 | 0 | 0 |
| Material | 64 | 57 | 7 | 0 | 0 |
| Animation | 125 | 113 | 12 | 0 | 0 |
| Niagara | 120 | 96 | 24 | 0 | 0 |
| Mesh | 194 core (+45 town gen experimental) | — | — | 0 | 0 |
| AI | 221 | — | — | 0 | 0 |
| Editor | 33 | 18 | 14 | 0 | 1 |
| Config | 6 | 6 | 0 | 0 | 0 |
| Project | 8 | 7 | 1 | 0 | 0 |
| Source | 12 | 11 | 1 | 0 | 0 |
| UI | 138 | 36 (UMG) + 51 (CommonUI M0.5.1) | 51 | 0 | 0 |
| GAS | 135 | 53 | — | 0 | 0 |
| LogicDriver | 66 | — | — | 0 | 0 |
| ComboGraph | 13 | — | — | 0 | 0 |
| Audio | 98 | — | — | 0 | 0 |
| LevelSequence | 8 | — | — | 0 | 0 |
| BA Bridge | 0 (integration) | — | — | 0 | 0 |
| **ReflectionIntel: decision** | **5** | **5** (4 automation + manual smoke) | 0 | 0 | 0 |
| **ReflectionIntel: risk** | **5** | **5** (6 automation + manual smoke) | 0 | 0 | 0 |
| **ReflectionIntel: cppreflect** | **6** | **5** (4 automation + manual smoke) | 1 (`list_class_specifiers`, untested) | 0 | 0 |
| **ReflectionIntel: network** | **4** | `list_replicated_classes` (15 replicated classes) + `list_rpc_functions` (project-plugin RPCs) verified E2E (plugin scan scope) | 2 (Phase 4a manual smoke — `list_onrep_handlers`, `audit_unbalanced_onreps`) | 0 | 0 |
| **ReflectionIntel: pipeline** | **2** | — | 2 (Phase 4a manual smoke only) | 0 | 0 |
| **ReflectionIntel: reflect** | **1** | manual smoke (`rebuild_reflection_index`) | 0 | 0 | 0 |
| **Total** | **1387 in-tree** | **578+ verified** | growing | **0** | **1** |

> **Note:** The 577+ formally verified PASS count is the union of the 2026-04-01 test audit (487) plus the M0.5.1 CommonUI test pass (50) plus the C++ tests landed for the bridge plugins plus the v0.14.9 PIE smoke tests (4 actions) plus the v0.14.10 PIE smoke matrix (12 MetaSound document introspection — 12 happy + 11 negative) plus the v0.16.0 automation suite (4 editor preview/inspection — 9 tests) plus v0.17.0's Reflection Intelligence Phase 1-3a automation suites (Decision 4 + Risk 6 + ModuleDepReality + CppReflect 4) plus the MCP LLM Ergonomics test suites (Response Shaping 11 + Fuzzy Match 5 + Cursor Pagination 5). UNTESTED counts grew because action counts grew between 0.12.0 (the audit baseline) and 0.17.0 — the new actions haven't all gone through formal test passes yet. MonolithMesh (194 core actions), MonolithAI (221 actions), MonolithLogicDriver (66 actions), MonolithComboGraph (13 actions), MonolithAudio (98 actions), and MonolithLevelSequence (8 actions) are verified functional through integration testing but per-action test matrices remain in progress. The v0.15.0–v0.17.0 additions are smoke-tested at release; full per-action matrices remain in progress. 1 Editor SKIP (`trigger_build` — not tested to avoid build disruption). Town gen (45 actions) is experimental with known geometry issues — disabled by default. Phase 4a network + pipeline actions shipped with manual smoke only — automation tests deferred.
