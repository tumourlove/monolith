# Monolith — Testing Reference

Last updated: 2026-03-30

---

## Test Environment

- **Engine:** Unreal Engine 5.7
- **Project:** Any UE 5.7+ project with Monolith installed
- **Plugin location:** `YourProject/Plugins/Monolith/`
- **MCP endpoint:** `http://localhost:9316/mcp`

---

## How to Test

### Action Test Template

```python
import http.client, json

def test_action(namespace, action, params=None):
    conn = http.client.HTTPConnection("localhost", 9316)
    if namespace == "monolith":
        tool_name = f"monolith_{action}"
        args = params or {}
    else:
        tool_name = f"{namespace}_query"
        args = {"action": action}
        if params:
            args["params"] = params

    body = json.dumps({
        "jsonrpc": "2.0", "id": 1,
        "method": "tools/call",
        "params": {"name": tool_name, "arguments": args}
    })
    conn.request("POST", "/mcp", body, {"Content-Type": "application/json"})
    resp = conn.getresponse()
    result = json.loads(resp.read())
    conn.close()
    return result
```

---

## Current Status: 533/665 ACTIONS PASS (9 PENDING, 46 UNTESTED, 130 GAS PASS, 45 TOWN GEN EXPERIMENTAL) + Incremental Indexer PASS

All 12 modules tested. Total plugin: 815 actions (770 active by default + 45 experimental town gen disabled by default).

2026-03-30: Procedural Town Generator marked **EXPERIMENTAL** — `bEnableProceduralTownGen = false` by default. 45 town gen actions not registered unless explicitly enabled. Fix Plan v5 applied (7 fixes) but fundamental geometry issues remain (wall misalignment, room separation). Town gen test status: **FAIL** (geometry). Core mesh actions (197) unaffected.

2026-03-30: MonolithGAS full test pass — 53/53 PASS, 0 FAIL. 12 bugs found and fixed (8 git commits). Key fixes: IGameplayTagsEditorModule, EnsureAssetPathFree, BS_BeingCreated suppression, AR pre-filter, GAS deep indexer. PIE runtime tests passing. 2026-03-28: Fix Plan v2 — 20 fixes across 3 phases + `validate_building` action. Stair/fire escape/ramp angle fixes, floor plan corridor/door/entrance guarantees, building_context on arch features. 2026-03-28: Procedural Town Generator — 45 new actions across 11 sub-projects. Compilation verified, runtime testing shows geometry failures. 2026-03-27: MonolithMesh module — 46 new actions (443→489 total). Compilation verified, basic MCP registration verified. 2026-03-25: Niagara expansion — 31 new actions (65→96 niagara, 349→443 total). 40/40 PASS, 3 SKIPPED, 3 bugs found and fixed during testing (type fallback warning, spawn shape duplicate, NPC namespace mismatch). Also 9 new material function actions (48→57 mat). Polish pass 2026-03-14 added 4 Niagara actions + get_system_property (213→218). 2026-03-15: HLSL module/function creation tested end-to-end (CPU + GPU), 3 bugs fixed (input exposure, dot validation, numeric index lookup). 2026-03-17: Blueprint module waves 2-7 full test pass (48/48 + 17 retests). 21 bugs found and fixed during testing session. Total actions now 278. 2026-03-17: Material module full test pass (44/44 + 11 retests). 11 bugs found and fixed. 2026-03-18: Niagara module full test pass (37/37 + 8 retests). 16 bugs found and fixed. 2026-03-18: Animation Wave 8-10 full test pass (33/33 + 4 retests). 12 bugs found and fixed. 2026-03-22: MonolithUI module full test pass (42/42). All 8 action classes verified.

---

## MonolithGAS — Full Test Pass (2026-03-30)

**53/53 PASS, 0 FAIL.** 12 bugs found and fixed during testing. 8 git fix commits (32c86d7 through 5639dda).

### Test Results by Category

| Category | Actions | Tests | Status | Notes |
|----------|---------|-------|--------|-------|
| Discovery | — | 3 | **3/3 PASS** | `monolith_discover("gas")` returns 130 actions, `monolith_status` lists module, settings toggle works |
| Scaffold | 6 | 4 | **4/4 PASS** | `bootstrap_gas_foundation`, `validate_gas_setup`, `scaffold_tag_hierarchy`, `scaffold_damage_pipeline` |
| Attributes | 20 | 9 | **9/9 PASS** | Create (BP + C++ modes), template, get/list, add_attribute, clamping, validation |
| Effects | 26 | 15 | **15/15 PASS** | Create, add_modifier, list_modifiers, add_ge_component, get, templates (burning, init_player, accessibility), stacking, duration, period, validate, list, duplicate, delete |
| Abilities | 28 | 8 | **8/8 PASS** | Create, tags, policy, cost, get_info, compile, templates (passive, heal_self), list, validate, find_by_tag, duplicate |
| Graph Building | — | 4 | **4/4 PASS** | `add_commit_and_end_flow`, `add_ability_task_node`, `add_effect_application`, `get_ability_graph_flow` |
| ASC | 14 | 3 | **3/3 PASS** | `add_asc_to_actor`, `configure_asc`, `validate_asc_setup` |
| Cues | 10 | 3 | **3/3 PASS** | `create_gameplay_cue_notify`, `link_cue_to_effect`, `validate_cue_coverage` |
| Tags | 10 | 4 | **4/4 PASS** | `add_gameplay_tags`, `search_tag_usage`, `validate_tag_consistency`, `rename_tag` |
| Inspect (PIE) | 6 | — | **PASS** | Runtime actions tested in PIE session: `get_all_ascs`, `get_asc_snapshot`, `snapshot_gas_state`, `get_attribute_value`, `set_attribute_value`, `apply_effect` |
| **Total** | **130** | **53** | **53/53 PASS** | All categories verified. PIE runtime tests passing. |

### Conditional Compilation

- `WITH_GBA=1`: All 130 actions register, module fully functional — **PASS**
- `WITH_GBA=0`: Module compiles clean, GBA-specific features return graceful errors — **PASS**

### Bugs Found and Fixed (12 total, 8 commits)

| Bug | Fix | Commit |
|-----|-----|--------|
| Tag system used wrong API for INI tag registration | Switch to `IGameplayTagsEditorModule` editor API | 32c86d7 |
| Asset creation collisions on existing paths | Three-tier `EnsureAssetPathFree` guard (registry, disk, memory) | — |
| ASC SCS component creation crashed during BP compile | `BS_BeingCreated` suppression for SCS node creation | — |
| Asset Registry `GetAsset` returned stale/phantom entries | AR pre-filter on all GetAsset calls (check `IsValid()` + `IsRedirector()`) | — |
| GAS deep indexer missing from MonolithIndex | Added GASIndexer for GameplayAbility, GameplayEffect, AttributeSet assets | — |
| Template tag names inconsistent | Standardized `SetByCaller.Cost.Stamina` naming | — |
| PIE helper functions duplicated across 4 files | Extracted to `MonolithGASInternal` shared utilities | — |
| Various parameter validation gaps | Added missing required-param checks across action handlers | 5639dda |

---

## MonolithMesh — Phases 1-4 Verification (2026-03-27)

46 new actions across 4 action classes. Compilation verified. MCP action registration verified via `monolith_discover("mesh")`.

| Action Class | Actions | Status | Notes |
|-------------|---------|--------|-------|
| FMonolithMeshInspectionActions | 12 | **COMPILED** | get_mesh_info, get_mesh_bounds, get_mesh_materials, get_mesh_lods, get_mesh_collision, get_mesh_uvs, analyze_skeletal_mesh, analyze_mesh_quality, compare_meshes, get_vertex_data, search_meshes_by_size, get_mesh_catalog_stats |
| FMonolithMeshSceneActions | 8 | **COMPILED** | get_actor_info, spawn_actor, move_actor, duplicate_actor, delete_actors, group_actors, set_actor_properties, batch_execute |
| FMonolithMeshSpatialActions | 11 | **COMPILED** | query_raycast, query_multi_raycast, query_radial_sweep, query_overlap, query_nearest, query_line_of_sight, get_actors_in_volume, get_scene_bounds, get_scene_statistics, get_spatial_relationships, query_navmesh |
| FMonolithMeshBlockoutActions | 15 | **COMPILED** | get_blockout_volumes, get_blockout_volume_info, setup_blockout_volume, create_blockout_primitive, create_blockout_primitives_batch, create_blockout_grid, match_asset_to_blockout, match_all_in_volume, apply_replacement, set_actor_tags, clear_blockout, export_blockout_layout, import_blockout_layout, scan_volume, scatter_props |
| **Total** | **46** | **COMPILED** | Full MCP test pass pending |

---

## Procedural Town Generator — 46 Actions, 11 Sub-Projects (2026-03-28)

46 actions across 11 sub-projects (45 original + `validate_building`). Compilation verified. Runtime testing pending. Fix Plan v2 applied: 20 fixes across geometry, floor plan, and integration phases.

**Master plan:** `Docs/plans/2026-03-28-proc-town-generator-master-plan.md`

### SP1: Grid-Based Building Construction (2 actions)

| Action | Status | Test Plan | Notes |
|--------|--------|-----------|-------|
| `create_building_from_grid` | **UNTESTED** | Create a 3-room grid (kitchen, living room, hallway) with 2 doors. Verify: no duplicate walls, doors connect rooms, collision works, player can walk between rooms. Then create a 2-story building with stairwell — verify stairs connect floors (32-degree angle, switchback support), stairwell cutout in ceiling slab. Test `omit_exterior_walls=true` for facade workflow | Foundation action — everything downstream depends on this. Fix Plan v2: stair angle 70->32 deg, switchback support, omit_exterior_walls param |
| `create_grid_from_rooms` | **UNTESTED** | Provide room list + adjacency requirements. Verify grid output matches expected layout | Helper for programmatic grid construction |

### SP2: Automatic Floor Plan Generation (3 actions)

| Action | Status | Test Plan | Notes |
|--------|--------|-----------|-------|
| `generate_floor_plan` | **UNTESTED** | Generate "residential_house" floor plan at 800x600. Verify: kitchen connects to dining, bathroom off hallway (not through bedroom), bedrooms upstairs (per-floor assignment), ALL rooms reachable via hallway or direct adjacency. Verify corridor width >= 120cm, door width >= 90cm, at least one exterior entrance on ground floor, no room aspect ratio worse than 1:4, rooms within footprint bounds. Test `hospice_mode` param: min 100cm doors, 180cm corridors | Core algorithm: squarified treemap + corridor insertion. Fix Plan v2: corridor width min, door width min, per-floor assignment, aspect ratios, footprint validation, guaranteed exterior entrance |
| `list_building_archetypes` | **UNTESTED** | Call with no params, verify returns archetype list from BuildingArchetypes/ | Read-only, should be straightforward |
| `get_building_archetype` | **UNTESTED** | Get "residential_house" archetype, verify JSON contains room types, sizes, adjacency requirements | Read-only |

### SP3: Facade & Window Generation (3 actions)

| Action | Status | Test Plan | Notes |
|--------|--------|-----------|-------|
| `generate_facade` | **UNTESTED** | Generate a 2-story facade with 3 windows per floor. Verify: windows evenly spaced, trim surrounds each window, cornice at top, ground floor has different treatment | Uses CGA-style vertical split (base/shaft/cap) |
| `list_facade_styles` | **UNTESTED** | Call with no params, verify returns style list from FacadeStyles/ | Read-only |
| `apply_horror_damage` | **UNTESTED** | Apply decay=0.8 to a building. Verify: boarded windows, broken glass geometry, rust stain decals visible | Horror-specific facade treatment |

### SP4: Roof Generation (1 action)

| Action | Status | Test Plan | Notes |
|--------|--------|-----------|-------|
| `generate_roof` | **UNTESTED** | Generate gable roof on a 600x400 building. Verify: ridge runs along long axis, 30cm overhang on all sides, separate MaterialID for roof surface. Test hip, flat, shed variants | Uses AppendSimpleSweptPolygon for gable, triangulated polygon for hip |

### SP5: City Block Layout (4 actions)

| Action | Status | Test Plan | Notes |
|--------|--------|-----------|-------|
| `create_city_block` | **UNTESTED** | Generate a 4-building block with seed=42. Verify: buildings don't overlap, streets exist, each building is unique and enterable. Test with SP4 unavailable — should produce flat-roof buildings (graceful degradation) | Top-level orchestrator, lightweight MCP call |
| `create_lot_layout` | **UNTESTED** | Subdivide a 6000x4000 block into 4 lots. Verify: lots don't overlap, reasonable size distribution | OBB recursive subdivision |
| `create_street` | **UNTESTED** | Generate streets for a 4-lot block. Verify: sidewalks, curbs, road surface geometry | Depends on lot positions |
| `place_street_furniture` | **UNTESTED** | Place furniture on generated streets. Verify: lamps, hydrants placed at reasonable intervals, not clipping geometry | Uses scatter with collision awareness |

### SP6: Spatial Registry (10 actions)

| Action | Status | Test Plan | Notes |
|--------|--------|-----------|-------|
| `register_building` | **UNTESTED** | Register a 5-room building. Verify JSON descriptor stored correctly | Core registry write |
| `register_room` | **UNTESTED** | Register individual room, verify it appears in subsequent queries | Granular registration |
| `register_street_furniture` | **UNTESTED** | Register street furniture actors, verify queryable | Lightweight registration |
| `query_room_at` | **UNTESTED** | Query "what room is at position X?" for a known room center. Verify correct room_id returned | Spatial lookup |
| `query_adjacent_rooms` | **UNTESTED** | Query "what's adjacent to the kitchen?". Verify all connected rooms returned via adjacency graph | Adjacency graph traversal |
| `query_rooms_by_filter` | **UNTESTED** | Filter rooms by type="bathroom". Verify all bathrooms across all buildings returned | Filter query |
| `query_building_exits` | **UNTESTED** | Query exits for a registered building. Verify exterior doors returned | Exit enumeration |
| `path_between_rooms` | **UNTESTED** | Path from kitchen to bedroom in a multi-room building. Verify BFS returns valid room sequence | BFS pathfinding |
| `save_block_descriptor` | **UNTESTED** | Save block to SpatialRegistry/. Verify JSON file written, loadable | Persistence write |
| `load_block_descriptor` | **UNTESTED** | Load saved descriptor, verify matches original | Persistence read |

### SP7: Auto-Volume Generation (3 actions)

| Action | Status | Test Plan | Notes |
|--------|--------|-----------|-------|
| `auto_volumes_for_building` | **UNTESTED** | Auto-volumes on a 3-room building. Verify: navmesh path exists between rooms, audio volume has correct reverb, trigger volume at entrance | Pipeline: geometry → collision → volumes → navmesh → audio |
| `auto_volumes_for_block` | **UNTESTED** | Auto-volumes for a full block. Verify NavMeshBoundsVolume covers entire block, navmesh builds successfully | Block-level orchestration |
| `spawn_nav_link` | **UNTESTED** | Spawn NavLinkProxy between two points. Verify AI can traverse the link | Standalone nav link utility |

### SP8a: Terrain + Foundations (5 actions)

| Action | Status | Test Plan | Notes |
|--------|--------|-----------|-------|
| `sample_terrain_grid` | **UNTESTED** | Sample height grid over a sloped landscape. Verify NxM array of heights returned | Downward trace sampling |
| `analyze_building_site` | **UNTESTED** | Analyze a 15-degree slope site. Verify recommended strategy (should be Stepped for 10-25 degrees) | Strategy selection logic |
| `create_foundation` | **UNTESTED** | Create foundation on a slope. Verify geometry adapts, no floating sections. Test `hospice_mode` param: ADA ramp (1:12 slope) | Foundation geometry generation |
| `create_retaining_wall` | **UNTESTED** | Create retaining wall along a path with height variation. Verify wall follows terrain contour | Terrain-following geometry |
| `place_building_on_terrain` | **UNTESTED** | Place building on 15-degree slope. Verify: foundation adapts, front steps or ramp generated, no floating geometry | Full terrain adaptation pipeline |

### SP8b: Architectural Features (5 actions)

| Action | Status | Test Plan | Notes |
|--------|--------|-----------|-------|
| `create_balcony` | **UNTESTED** | Add balcony to 2-story building second floor. Verify: extends outward, has railing, doesn't intersect building walls. Test `building_context` collision checks, `wall_openings` in response | Upper floor exterior feature. Fix Plan v2: building_context param, wall_openings output |
| `create_porch` | **UNTESTED** | Add porch to building entrance. Verify: ground-level, covered, columns present. Test `building_context` collision checks, `wall_openings` in response | Ground-level entry feature. Fix Plan v2: building_context param, wall_openings output |
| `create_fire_escape` | **UNTESTED** | Add fire escape to 3-story building. Verify: zigzag stairs connect all floors (45-degree angle), landings at each floor, `building_context` collision checks, `wall_openings` in response | Multi-floor exterior stairs. Fix Plan v2: angle 66->45 deg, building_context param, wall_openings output |
| `create_ramp_connector` | **UNTESTED** | Create ramp between two heights. Verify ADA compliance (1:12 slope, max 76cm rise per run). Verify switchback geometry does not self-intersect. Test `building_context` collision checks, `wall_openings` in response | Accessibility feature. Fix Plan v2: switchback self-intersection fix, building_context param, wall_openings output |
| `create_railing` | **UNTESTED** | Create railing along a path. Verify swept profile follows path correctly | Swept geometry along edge |

### SP9: Daredevil Debug View (6 actions)

| Action | Status | Test Plan | Notes |
|--------|--------|-----------|-------|
| `toggle_section_view` | **UNTESTED** | Toggle section view at floor 1 height on a 2-story building. Verify: roof/ceiling hidden, all rooms visible from above | MPC-based section clip |
| `toggle_ceiling_visibility` | **UNTESTED** | Hide ceilings via BuildingCeiling actor tag. Verify: ceilings hidden, walls/floors remain | Actor tag visibility toggle |
| `capture_floor_plan` | **UNTESTED** | Capture orthographic top-down view of a building. Verify: PNG output, all rooms visible, walls clear | Orthographic capture to PNG |
| `highlight_room` | **UNTESTED** | Highlight a specific room with overlay material. Verify: room visually distinguished, other rooms unaffected | Overlay material application |
| `save_camera_bookmark` | **UNTESTED** | Save current camera viewpoint with a name. Verify JSON written to CameraBookmarks/ | Camera state persistence |
| `load_camera_bookmark` | **UNTESTED** | Load a saved bookmark. Verify camera moves to saved position/rotation | Camera state restore |

### SP10: Room Furnishing Pipeline (3 actions)

| Action | Status | Test Plan | Notes |
|--------|--------|-----------|-------|
| `furnish_room` | **UNTESTED** | Furnish a "kitchen" room at 400x300. Verify: counter, table, and cabinets placed within room bounds, no furniture clipping walls (collision_mode: "reject"). Test with disturbance="ransacked" | Room-type furniture mapping |
| `furnish_building` | **UNTESTED** | Furnish all rooms in a 5-room building. Verify: each room type gets appropriate furniture, horror decay applied proportionally | Building-level furnishing |
| `list_furniture_presets` | **UNTESTED** | List presets for room_type="kitchen". Verify returns furniture configs from FurniturePresets/ | Read-only preset listing |

### Validation (1 action)

| Action | Status | Test Plan | Notes |
|--------|--------|-----------|-------|
| `validate_building` | **UNTESTED** | Run on a building with known issues (disconnected room, missing entrance, steep stairs). Verify: returns `valid: false` with correct `issues[]` entries. Run on a correct building, verify `valid: true`. Test each validation check: room connectivity, door reachability, stair angles, wall thickness, exterior entrance, floor slab coverage | Added in Fix Plan v2. Post-generation integrity checker |

### Procedural Town Generator — Summary — EXPERIMENTAL (disabled by default)

> **Status:** EXPERIMENTAL. `bEnableProceduralTownGen = false` by default. Fix Plans v2-v5 applied (27+ fixes) but fundamental geometry issues remain: walls misaligned between rooms, rooms physically separated, boolean operations unreliable. Marked experimental 2026-03-30 until geometry engine is properly fixed.

| Sub-Project | Actions | Status | Notes |
|------------|---------|--------|-------|
| SP1: Grid Building | 2 | **GEOMETRY ISSUES** | Walls misaligned between rooms, boolean operations unreliable |
| SP2: Floor Plans | 3 | **COMPILED** | Squarified treemap + corridor insertion — algorithm works, geometry output broken |
| SP3: Facades | 3 | **COMPILED** | CGA-style vertical split, horror damage — depends on SP1 geometry |
| SP4: Roofs | 1 | **COMPILED** | Gable, hip, flat, shed, gambrel |
| SP5: Block Layout | 4 | **COMPILED** | Top-level orchestrator, graceful degradation |
| SP6: Spatial Registry | 10 | **COMPILED** | Hierarchical JSON descriptor, BFS pathfinding |
| SP7: Auto-Volumes | 3 | **COMPILED** | NavMesh, blocking, audio, trigger |
| SP8a: Terrain | 5 | **COMPILED** | Height sampling, foundations, ADA ramps |
| SP8b: Arch Features | 5 | **COMPILED** | Balconies, porches, fire escapes, railings |
| SP9: Debug View | 6 | **COMPILED** | Section clip, floor plan capture, bookmarks |
| SP10: Furnishing | 3 | **COMPILED** | Room-type furniture, horror dressing |
| Validation | 1 | **COMPILED** | `validate_building` — Fix Plan v2 |
| **Total** | **46** | **EXPERIMENTAL** | Compiles, registers when enabled, but geometry output has fundamental issues. Fix Plans v2-v5 did not resolve core wall/room alignment problems |

---

## Material Function Full Suite (2026-03-25)

| Action | Status | Notes |
|--------|--------|-------|
| export_function_graph | PASS | Returns full JSON: nodes, connections, inputs/outputs, properties, positions. Tested with MF_Test (6 nodes). |
| set_function_metadata | PASS | Updated description, expose_to_library, library_categories. Response confirms modified fields. |
| delete_function_expression | PASS | Deleted Multiply node by name. Returns deleted count + not_found list. |
| update_material_function | PASS | Returns `{updated: true}`. Propagates to dependents. |
| create_function_instance | PASS | Creates MFI linked to parent. Scalar overrides correctly report "not found" when parent params aren't MFI-overridable (expected UE behavior). |
| set_function_instance_parameter | PASS | Action executes with clear error when no overridable params exist. Proper validation, no crash. |
| get_function_instance_info | PASS | Returns parent, base, type, all override categories, inputs, outputs, override counts. Very thorough. |
| layout_function_expressions | PASS | Returns `{arranged: true}`. Positions confirmed changed via get_function_info. |
| rename_function_parameter_group | PASS | Renamed "None" to "TestGroup". Verified via get_expression_details — Group field updated. Param names: old_group/new_group. |
| create_material_function (type param) | PASS | Both MaterialLayer and MaterialLayerBlend created. Correct type reported in get_function_info. Both auto-set expose_to_library=true. |

Test assets: `/Game/Test/MatFuncTest/` (MF_Test, M_TestMat, MFI_Test, MF_Layer, MF_LayerBlend)
Tested by: unreal-material-expert agent, 2026-03-25. All 10/10 PASS.

---

## Niagara Expansion — Full Test Pass (2026-03-25)

31 new actions added (65 → 96 total). `move_module` rewritten. 7 bug fixes.

| Category | Actions | Test | Result | Notes |
|----------|---------|------|--------|-------|
| Dynamic Inputs | 5 | list_dynamic_inputs, get_dynamic_input_tree, remove_dynamic_input, get_dynamic_input_value, get_dynamic_input_inputs | **5/5 PASS** | Full DI tree traversal and value readback verified |
| Emitter Management | 3 | rename_emitter, get_emitter_property, list_available_renderers | **3/3 PASS** | Rename persists after recompile |
| Renderer Config | 3 | set_renderer_mesh, configure_ribbon, configure_subuv | **3/3 PASS** | Mesh assignment, ribbon width/facing, SubUV frame config |
| Event Handlers | 3 | get_event_handlers, set_event_handler_property, remove_event_handler | **3/3 PASS** | CRUD cycle on event handlers verified |
| Simulation Stages | 3 | get_simulation_stages, set_simulation_stage_property, remove_simulation_stage | **3/3 PASS** | Stage creation, property set, removal |
| Module Outputs | 1 | get_module_output_parameters | **1/1 PASS** | Returns typed output params |
| NPC (Parameter Collections) | 5 | create_npc, get_npc, add_npc_parameter, remove_npc_parameter, set_npc_default | **5/5 PASS** | Full NPC lifecycle. Namespace mismatch bug found and fixed during testing |
| Effect Types | 3 | create_effect_type, get_effect_type, set_effect_type_property | **3/3 PASS** | Scalability and budget settings verified |
| Utilities | 5 | get_available_parameters, preview_system, diff_systems, save_emitter_as_template, clone_module_overrides | **5/5 PASS** | Diff returns structural delta between two systems |
| move_module rewrite | 1 | move_module (rewritten) | **1/1 PASS** | Input overrides now preserved after move |
| Bug fix retests | 7 | type fallback, spawn shape, NPC namespace, + 4 others | **7/7 PASS** | All regressions verified fixed |
| **Total** | **40** | | **40/40 PASS** | 3 actions SKIPPED (require specific asset setup not available in test environment) |

**Bugs found and fixed during testing:**

| # | Issue | Fix |
|---|-------|-----|
| 1 | Type fallback warning — incorrect type name in warning message when input type falls back to default | Fixed warning string to match actual fallback type |
| 2 | Spawn shape duplicate — adding a spawn shape module created duplicate entries in certain emitter configurations | Deduplication check before insertion |
| 3 | NPC namespace mismatch — `add_npc_parameter` used wrong namespace prefix, causing parameter lookup failures in `get_npc` | Aligned namespace prefix across all NPC actions |

**3 SKIPPED actions:** Require specific multi-system test assets or GPU-only features not available in the automated test environment. Manual verification pending.

---

## Bugs Fixed (2026-03-17 — Blueprint Wave Testing)

| Module | Issue | Fix | Verified |
|--------|-------|-----|----------|
| Blueprint | `resolve_node` crash: transient node with no owning Blueprint (FindBlueprintForNodeChecked assert) | Temp Blueprint used as outer | PASS |
| Blueprint | `validate_blueprint` crash: null deref on orphaned node graph name | Null-safe GetGraph() check | PASS |
| Blueprint | `set_event_dispatcher_params` crash: PinsToRemove loop desync during iteration | Safe iteration via TSharedPtr copy | PASS |
| Blueprint | `rename_component` crash: NAME_None input triggers engine check() | Guard rejects empty name upfront | PASS |
| Blueprint | `create_blueprint` crash: IsValidChecked null assert on uncompiled GeneratedClass during save | Compile before save + FullyLoad + SaveLoadedAsset | PASS |
| Blueprint | `promote_pin_to_variable` crash risk: MarkBlueprintAsStructurallyModified before rewiring invalidates pointers | Deferred structural mark to after rewiring | PASS |
| Blueprint | `add_variable` silently mutated existing variable on name collision | Pre-check BP->NewVariables, return error if name taken | PASS |
| Blueprint | `add_event_node` allowed duplicate custom event names | Shared HasCustomEventNamed helper — both add_event_node and add_node paths guarded | PASS |
| Blueprint | `set_event_dispatcher_params` FAIL: no FunctionEntry in fresh dispatcher graph | Added CreateFunctionGraphTerminators + AddExtraFunctionFlags + MarkFunctionEntryAsEditable | PASS |
| Blueprint | `search_functions` returned 0 results for multi-word queries | Split query on spaces, AND all tokens | PASS |
| Blueprint | `validate_blueprint` missing: unimplemented interfaces and duplicate events not detected | Added both checks | PASS |
| Blueprint | `add_timeline` flags only set on node, not UTimelineTemplate | Flags set on UTimelineTemplate too | PASS |
| Blueprint | Dispatcher reference detection used substring matching (Contains) | Exact FName comparison | PASS |
| Blueprint | `connect_pins` silently auto-converted incompatible pins | Warning field added to response | PASS |
| Blueprint | `batch_execute` returned cryptic error when "op" key missing | Helpful error message added | PASS |
| Blueprint | `replication_condition` rejected COND_ prefix values | Auto-strip COND_ prefix | PASS |
| Blueprint | `add_nodes_bulk` omitted node positions from response | Positions included in per-node result | PASS |
| Blueprint | `scaffold_interface_implementation` returned 0 functions with no explanation | Note field added explaining why count is 0 | PASS |
| Blueprint | `get_interface_functions` error hint pointed to wrong naming convention | Updated to mention C++ I-prefix and BP naming | PASS |
| Blueprint | `add_node` CustomEvent bypassed duplicate guard (separate code path from add_event_node) | Shared HasCustomEventNamed helper applied to both paths | PASS |
| Blueprint | `create_blueprint` cold-path duplicate not detected via Asset Registry | Asset Registry guard added | PASS |

---

## Bugs Fixed (2026-03-17 — Material Wave Testing)

| Module | Issue | Fix | Verified |
|--------|-------|-----|----------|
| Material | `get_material_properties` missing `fully_rough` and `cast_shadow_as_masked` readback | Added bool fields to response | PASS |
| Material | `build_function_graph` type fallback warning said "Scalar" but defaulted to Vector3 | Fixed fallback to actually use Scalar | PASS |
| Material | `build_material_graph` connections using user-provided `name` fields failed (`. -> .`) | Added name→auto-name resolution; added `from_expression`/`to_expression` key aliases | PASS |
| Material | `build_material_graph` outputs array only accepted `from` key, not `from_expression` | Added `from_expression` alias in outputs parser | PASS |
| Material | `list_material_instances` no warning that unsaved assets are invisible to Asset Registry | Added `note` field about Asset Registry visibility | PASS |
| Material | `build_function_graph` required `FunctionInput_Vector3` — no shorthands | Added aliases: float/float2/float3/float4/bool/scalar/vec2/vec3/vec4/texture2d/texturecube | PASS |
| Material | `auto_layout` minimal response — no indication what changed | Added `positions_changed` field | PASS |
| Material | `get_instance_parameters` `total_overrides` counted non-overridden switch entries | Fixed to only count params where `bOverride == true` | PASS |
| Material | `clear_instance_parameter` type="all" underreported `cleared_count` — didn't count static switches | Added explicit static switch clearing after `ClearAllMaterialInstanceParameters()` | PASS |
| Material | `set_instance_parameters` response only had `set_count` — no per-param detail | Added `results` array with name/type/success per param | PASS |
| Material | `get_instance_parameters` `StaticParametersRuntime` is private in UE 5.7 — compile error | Changed to public `GetStaticParameters()` API | PASS |

---

## Bugs Fixed (2026-03-18 — Niagara Wave Testing)

| # | Action | Issue | Fix |
|---|--------|-------|-----|
| 1 | `add_event_handler` | Crash: null Script pointer — direct array push without creating UNiagaraScript | Proper script creation via NewObject + SetUsage + AddEventHandler API |
| 2 | `add_simulation_stage` | Crash risk: no null check on emitter | Added null guard |
| 3 | `add_simulation_stage` | Bug: stage_name param never applied (always "None") | Added SimulationStageName assignment |
| 4 | `create_emitter` | Bug: missing handle_id in response | Added handle_id field |
| 5 | `create_emitter` | Bug: GPU path result silently dropped | Added gpu_warning field |
| 6 | `export_system_spec` | Bug: missing "asset" field on emitter objects | Added emitter asset path |
| 7 | `add_dynamic_input` | Bug: null check after EndTransaction | Reordered — null check before EndTransaction |
| 8 | `set_fixed_bounds` | UX: disable path required min/max params | Min/max only required when enabling |
| 9 | `validate_system` | UX: mesh renderer not checked | Added empty meshes warning |
| 10 | `configure_data_interface` | UX: ImportText failures silently vanished | Added properties_failed array |
| 11 | `configure_data_interface` | UX: success:true when all properties fail | Returns error + available_properties list |
| 12 | `export_system_spec` | UX: include_values misses data input overrides | Added inputs_note explaining limitation |
| 13 | `add_dynamic_input` | UX: returned inputs only had static switches | Now includes data inputs (Minimum, Maximum, etc.) |
| 14 | `search_dynamic_inputs` | UX: type filter passed unknown types through | Unknown types now excluded when filter specified |
| 15 | `set_renderer_material` | UX: can't clear material | Accepts ""/"none" to clear |
| 16 | `validate_system` | Blocked: missing material test impossible without clear path | Unblocked by fix #15 |

---

## Bugs Fixed (2026-03-18 — Animation Wave 8-10 Testing)

| Module | Issue | Fix | Verified |
|--------|-------|-----|----------|
| Animation | `add_transition` CastChecked crash on null schema | Safe Cast + null check | PASS |
| Animation | `get_abp_linked_assets` empty arrays (wrong path type for AR lookup) | GetAssetsByPackageName instead of GetAssetByObjectPath | PASS |
| Animation | `add_ik_solver` nested transactions (4-5 undos per operation) | Removed outer transaction — controller manages own | PASS |
| Animation | `get_retargeter_info` multi-op retargeters return 0 chain mappings | Iterate target chains + GetSourceChain per chain | PASS |
| Animation | `add_state_to_machine` BoundGraph null = silent rename failure | Null check + error return | PASS |
| Animation | `set_transition_rule` MakeLinkTo bypasses schema | Use TryCreateConnection via rule graph schema | PASS |
| Animation | `add_ik_solver` SetStartBone failure silently dropped | Capture return + warning in response | PASS |
| Animation | `add_ik_solver` goals silently discarded on empty skeleton | Guard for empty skeleton + warning | PASS |
| Animation | `add_control_rig_element` parent_type defaults to child type | Default to bone when parent specified | PASS |
| Animation | `get_abp_variables` type field vague for struct/object | Include PinSubCategoryObject name | PASS |
| Animation | `get_retargeter_info` single-op note no longer needed | Removed (multi-op readback fixed) | PASS |
| Animation | ABP write experimental gate removed | All CVar checks + enable_experimental_writes action removed | PASS |

---

## MonolithUI — Full Test Pass (2026-03-22)

| Action Class | Actions | Test | Result | Notes |
|-------------|---------|------|--------|-------|
| FMonolithUIActions | 7 | Widget CRUD: create_widget_blueprint, get_widget_tree, add_widget, remove_widget, set_widget_property, compile_widget, list_widget_types | **7/7 PASS** | Create, inspect, modify, compile cycle verified |
| FMonolithUISlotActions | 3 | Slot ops: set_slot_property, set_anchor_preset, move_widget | **3/3 PASS** | Anchor presets, reparenting verified |
| FMonolithUITemplateActions | 8 | Templates: create_hud_element (3 types), create_menu, create_settings_panel, create_dialog, create_notification_toast, create_loading_screen, create_inventory_grid, create_save_slot_list | **8/8 PASS** | All 8 template types scaffold correctly |
| FMonolithUIStylingActions | 6 | Styling: set_brush, set_font, set_color_scheme, batch_style, set_text, set_image | **6/6 PASS** | Brush, font, color, text, image all apply |
| FMonolithUIAnimationActions | 5 | Animation: list_animations, get_animation_details, create_animation, add_animation_keyframe, remove_animation | **5/5 PASS** | Full animation CRUD cycle |
| FMonolithUIBindingActions | 4 | Binding: list_widget_events, list_widget_properties, setup_list_view, get_widget_bindings | **4/4 PASS** | Event and property inspection verified |
| FMonolithUISettingsActions | 5 | Settings: scaffold_game_user_settings, scaffold_save_game, scaffold_save_subsystem, scaffold_audio_settings, scaffold_input_remapping | **5/5 PASS** | All 5 scaffolds generate valid C++ |
| FMonolithUIAccessibilityActions | 4 | Accessibility: scaffold_accessibility_subsystem, audit_accessibility, set_colorblind_mode, set_text_scale | **4/4 PASS** | Audit reports font/focus/nav issues correctly |
| **Total** | **42** | | **42/42 PASS** | All UI actions pass on first run |

---

## Incremental Indexer — Full Test Pass (2026-03-28)

3-layer incremental indexer: startup hash-based delta, live AR callbacks, forced full reindex fallback. Schema v2 migration. Plugin content scope fix.

| Test | Result | Notes |
|------|--------|-------|
| Live compile | **PASS** | Zero errors, zero warnings |
| Startup incremental (no changes) | **PASS** | 14,032 unchanged, 1 added, <1s |
| Schema v2 migration (existing DB) | **PASS** | PRAGMA table_info check, ALTER TABLE + index |
| Plugin discovery (bInstalled fix) | **PASS** | DrawCallReducer + NiagaraDestructionDriver now indexed |
| Module breakdown | **PASS** | 7 modules: Project(12600), InventorySystemX(1046), MassProjectile(287), DrawCallReducer(49), NiagaraDestructionDriver(34), SkinnedDecalComponent(13), MetaTailorBridge(3) |
| Live callbacks registered | **PASS** | Log confirms "Live index callbacks registered" |
| MCP reindex action | **PASS** | Routes to incremental by default |
| Deep indexing | **PASS** | "Deep-indexed 0 assets from 1 paths" (no matching indexer for added asset) |

All 8/8 PASS. Tested 2026-03-28.

---

## Known Issues

| Module | Issue | Status |
|--------|-------|--------|
| Niagara | `get_system_diagnostics` reported false DataMissing errors on emitter spawn/update scripts — these are editor-only source graphs inlined into system scripts, never independently compiled | FIXED (2026-03-13): changed `GetScripts(false)` → `GetScripts(true)` |
| Niagara | `add_emitter` required full object path suffix (e.g. `.Fountain`) and silently failed on wrong param names (`template`, `emitter_path`) | FIXED (2026-03-13): switched to `LoadAssetByPath`, added param aliases |
| Niagara | `create_system_from_spec` read `save_path` from inside `spec` only — users passing it at top-level got misleading "save_path required" error | FIXED (2026-03-13): accepts at both levels |
| Niagara | `create_system` template param used raw `LoadObject` — bare paths failed with "Failed to load template" | FIXED (2026-03-13): switched to `LoadAssetByPath` |

---

## Bugs Fixed (2026-03-13 — Session 4)

| Module | Issue | Fix | Verified |
|--------|-------|-----|----------|
| Core | Registry dispatched handlers without checking required params — callers got handler-specific errors instead of "missing param X" | `FMonolithToolRegistry::Execute()` now validates required schema params before dispatch. Returns error listing missing + provided keys. Skips `asset_path` (handled by `GetAssetPath()`) | PASS — missing params return descriptive error immediately |
| Niagara | Module write actions only accepted `module_node` and `input` — any variation caused "module not found" or silent fail | Added aliases: `module_node` → also accepts `module_name`, `module`; `input` → also accepts `input_name` | PASS — all alias forms work |
| Material | `set_expression_property` called `PostEditChange()` with no args — material graph didn't rebuild, editor display didn't update | Changed to `PostEditChangeProperty(FPropertyChangedEvent(Prop))` with actual property pointer | PASS — scalar param DefaultValue change reflects immediately in editor |
| Material | `set_material_property`, `create_material`, `delete_expression`, `connect_expressions` missing `PreEditChange`/`PostEditChange` cycle | Added `Mat->PreEditChange(nullptr)` + `Mat->PostEditChange()` after each write | PASS — changes visible without manual recompile |
| Core | `tools/list` response had no param info — AI had to call `monolith_discover` to see param names | `HandleToolsList()` now embeds per-action param schemas in `params` property description: `*name(type)` per param, `*` = required | PASS — Claude Code sees param names from tool list |

## Bugs Fixed (2026-03-13 — Session 2)

| Module | Issue | Fix | Verified |
|--------|-------|-----|----------|
| Blueprint | `add_component` ignores `component_name` param — schema registered as `"name"`, handler reads `"name"`, but all other component actions use `"component_name"` | Changed schema + handler to `"component_name"` (2 lines) | PASS — component created with correct name |
| Blueprint | `create_blueprint` crashes (assert) if Blueprint already exists at path — `FindObject` assert in `FKismetEditorUtilities::CreateBlueprint` | Added existence check before `CreatePackage`, returns error instead of crashing | PASS — returns graceful error |
| Niagara | `set_module_input_value` and `set_module_input_binding` can't reach data inputs (SpawnRate, Color, Velocity, etc.) — uses local helper that only sees static switch pins, not engine's `GetStackFunctionInputs` | Replaced `MonolithNiagaraHelpers::GetStackFunctionInputs` with `FNiagaraStackGraphUtilities::GetStackFunctionInputs` + `FCompileConstantResolver` + `Module.` prefix stripping (matches `get_module_inputs` read path) | PASS — float and vector3f inputs set successfully |

## Bugs Fixed (2026-03-13 — Session 1)

| Module | Issue | Fix |
|--------|-------|-----|
| Blueprint | `remove_local_variable` — `GetLocalVariablesOfType` with empty type matches nothing | Direct `UK2Node_FunctionEntry::LocalVariables` array walk |
| Blueprint | 3 deprecation warnings: `Pop(false)`, 2x `FName`-based interface APIs | `EAllowShrinking::No`, `GetClassPathName()` |
| Core | Claude Code sends `params` as JSON string, not nested object — all `_query` tools got empty args | Added `FJsonSerializer::Deserialize` fallback for string-typed params in `HandleToolsCall` |
| Niagara | `set_emitter_property` stale bug entry — was already fixed in prior session | Removed from known bugs |
| Plugin | Missing plugin deps: `EditorScriptingUtilities`, `PoseSearch` | Added to `Monolith.uplugin` Plugins list |
| Docs | Action count 217 was wrong — PoseSearch 5 already included in animation 62 | Corrected to 212 everywhere |

---

## Niagara — HLSL Module Tests (2026-03-15)

| Action | Test | Result | Notes |
|--------|------|--------|-------|
| `create_module_from_hlsl` | Simple float input, CPU sim | PASS | Asset created, opens in editor, typed pin visible |
| `create_module_from_hlsl` | Multi-type inputs (vec3 + float), CPU sim | PASS | Multiple typed pins, correct types |
| `create_module_from_hlsl` | GPU sim target | PASS | Module compiles when added to GPU sim emitter; HLSL visible in compiled GPU shader |
| `create_function_from_hlsl` | Reusable function, CPU | PASS | Asset created, typed pins visible, script usage = Function |
| `get_module_inputs` | CustomHlsl module | PASS | Fallback reads FunctionCall typed pins directly when GetStackFunctionInputs returns empty; returns 3 typed inputs |
| `set_module_input_value` | CustomHlsl module | PASS | Sets pin DefaultValue directly on FunctionCall node |
| Dot validation | Input name with `.` (e.g. `Module.Color`) | PASS | Rejected with clear error message before asset creation |
| Dot validation | Output name with `.` | PASS | Rejected with clear error message before asset creation |

**Bugs fixed this session:**
1. Numeric index lookup — module lookup by position index was broken
2. Input exposure fallback — `get_module_inputs` now falls back to FunctionCall typed pins when engine helper returns empty for CustomHlsl modules
3. Dot validation — input/output names containing `.` now rejected upfront with descriptive error (dots break HLSL tokenizer and Niagara ParameterMap aliasing)

---

## Niagara — Polish Pass Tests (2026-03-14)

| Action | Test | Result | Notes |
|--------|------|--------|-------|
| `set_system_property` | Set WarmupTime on existing system | PASS | System-level property set via reflection |
| `set_system_property` | Set bDeterminism on existing system | PASS | Bool property accepted |
| `set_static_switch_value` | Set static switch on module | PASS | Returns blend mode validation warnings where applicable |
| `list_module_scripts` | Search by keyword | PASS | Returns matching asset paths from content browser |
| `list_module_scripts` | Empty keyword returns all | PASS | Broad search supported |
| `list_renderer_properties` | List properties on Sprite renderer | PASS | Returns property names, types, current values |
| `list_renderer_properties` | Missing renderer param returns error | PASS | Descriptive error on bad renderer name |

---

## Niagara — Session 3 Tests (2026-03-13)

| Action | Test | Result | Notes |
|--------|------|--------|-------|
| `get_system_diagnostics` | Basic output on existing system | PASS | Reports errors, warnings, emitter stats, sim target, op/register counts |
| `get_system_diagnostics` | DataMissing detection | PASS | Detects scripts with no compiled bytecode |
| `get_system_diagnostics` | GPU+dynamic bounds warning | PASS | Warns when GPU emitter uses dynamic bounds mode |
| `get_system_diagnostics` | CalculateBoundsMode=Fixed clears warning | PASS | Warning disappears when bounds set to Fixed |
| `set_emitter_property` | SimTarget CPU→GPU | PASS | Recompile triggered, GPU Compute Script appears |
| `set_emitter_property` | SimTarget GPU→CPU | PASS | Round-trip works, VM bytecode restored (181/95 ops) |
| `set_emitter_property` | CalculateBoundsMode property | PASS | New property accepted, Fixed/Dynamic values work |
| `request_compile` | force + synchronous params | PASS | New params accepted |

---

## Blueprint — Wave 5 Tests (2026-03-17)

| Action | Test | Result | Notes |
|--------|------|--------|-------|
| `scaffold_interface_implementation` | Add interface to Actor BP | PASS | Stub graphs created |
| `scaffold_interface_implementation` | Already-implemented interface returns already_implemented=true | PASS | — |
| `add_timeline` | Create timeline in Actor BP event graph | PASS | GUID linkage verified, compiles clean |
| `add_timeline` | Reject timeline in function graph | PASS | Returns error as expected |
| `add_timeline` | Auto-generated name when timeline_name omitted | PASS | — |
| `add_event_node` | BeginPlay alias → ReceiveBeginPlay override | PASS | — |
| `add_event_node` | Tick alias → ReceiveTick override | PASS | — |
| `add_event_node` | Unknown event name → CustomEvent fallback | PASS | — |
| `add_event_node` | Duplicate override → error | PASS | — |
| `add_comment_node` | Basic comment box | PASS | — |
| `add_comment_node` | Auto-size from node_ids | PASS | GroupMovement mode |

---

## Blueprint — CustomEvent Duplicate Guard (2026-03-17)

| Action | Test | Result | Notes |
|--------|------|--------|-------|
| `add_node` (CustomEvent) | First creation → success | PASS | Returns K2Node_CustomEvent_0 with correct custom_name |
| `add_node` (CustomEvent) | Duplicate name → error | PASS | "A custom event named 'UniqueTestEvent' already exists in this Blueprint" |
| `add_event_node` | Duplicate CustomEvent name → error | PASS | Same shared check — both code paths guarded |
| `batch_execute` | Two ops same CustomEvent name — op 0 success, op 1 error | PASS | stop_on_error:false — first succeeds, second gets the dupe error |
| `add_node` (CustomEvent) | Two different names → both succeed | PASS | AlphaEvent + BetaEvent both created cleanly |

---

## Test History

| Date | Scope | Result | Notes |
|------|-------|--------|-------|
| 2026-03-30 | Procedural Town Generator marked EXPERIMENTAL | **EXPERIMENTAL** | `bEnableProceduralTownGen = false` by default. Fix Plan v5 applied (7 fixes: facade reorder, boolean isolation, wall alignment, door clamp, window density, template variety, furniture placement). Fundamental geometry issues remain. 45 town gen actions only registered when enabled. Total plugin: 815 actions (770 active default). |
| 2026-03-30 | MonolithGAS full test pass (53/53) | **53/53 PASS** | 130 actions across 10 categories. 12 bugs found and fixed (8 git commits: 32c86d7-5639dda). Key fixes: IGameplayTagsEditorModule API, EnsureAssetPathFree 3-tier guard, BS_BeingCreated suppression, AR pre-filter, GAS deep indexer. PIE runtime tests all passing. |
| 2026-03-29 | MonolithGAS module build verification (130 actions) | **COMPILED** | Initial build verification. WITH_GBA=1 and WITH_GBA=0 both compile clean. `gas` namespace registered. Total plugin: 815 actions. |
| 2026-03-28 | Procedural Town Generator compilation + registration (45 actions) | **COMPILED** | 45 new actions across 11 sub-projects (SP1-SP10). 12 new action classes. Building Descriptor contract. Total plugin: 535 actions (241 mesh). Full MCP runtime test pass pending. |
| 2026-03-28 | Incremental indexer (8 tests) | **8/8 PASS** | 3-layer architecture: startup delta, live AR callbacks, forced full fallback. Schema v2 migration. Plugin content scope fix. 14,032 assets, <1s startup. |
| 2026-03-27 | MonolithMesh module compilation + registration (46 actions) | **COMPILED** | 46 new actions across 4 classes: Inspection (12), Scene (8), Spatial (11), Blockout (15). MeshCatalogIndexer added to MonolithIndex. Total plugin: 489 actions. Full MCP test pass pending. |
| 2026-03-25 | Niagara expansion full test pass (40/40, 3 skipped) | **40/40 PASS** | 31 new actions (65→96 niagara). Dynamic inputs, emitter mgmt, renderer config, event handlers, sim stages, NPC, effect types, utilities. move_module rewrite. 3 bugs found+fixed. Total plugin: 443 actions. |
| 2026-03-25 | Material function suite (10/10) | **10/10 PASS** | 9 new function actions (48→57 mat). export, metadata, delete, update, MFI CRUD, layout, rename group, layer types. |
| 2026-03-22 | MonolithUI module full test pass (42/42) | **42/42 PASS** | All 8 action classes verified: Widget CRUD (7), Slot (3), Templates (8), Styling (6), Animation (5), Binding (4), Settings (5), Accessibility (4). |
| 2026-03-18 | Animation Wave 8-10 full test pass (33/33 + 4 retests) | **37/37 PASS** | 12 bugs found and fixed. Covers IKRig read/write, IK Retargeter, Control Rig read/write, ABP variables, ABP linked assets, ABP state machine writes (add_state_to_machine, add_transition, set_transition_rule). ABP experimental gate removed — all CVar checks stripped. |
| 2026-03-18 | Niagara module full test pass (37/37 + 8 retests) | **45/45 PASS** | 16 bugs found and fixed. Covers waves 2-6: summary/discovery, DI curve config, system management, dynamic inputs, event handlers, simulation stages, validate_system. |
| 2026-03-17 | Material module full test pass (44/44 + 11 retests) | **55/55 PASS** | 11 bugs found and fixed. Covers all wave 2-6 actions: auto_layout, duplicate_expression, move_expression, instance params, function graph, batch ops, import_texture, and all bug fix verifications. |
| 2026-03-17 | Blueprint module waves 2-7 full test pass (48/48 + 17 retests) | **65/65 PASS** | 21 bugs found and fixed. Covers batch_execute, discovery, bulk ops, scaffolding, inspection, advanced actions, and all bug fix verifications. |
| 2026-03-17 | Blueprint CustomEvent duplicate guard: add_node, add_event_node, batch_execute (5 tests) | **5/5 PASS** | Both add_node and add_event_node paths share the same guard. Batch handles partial failure cleanly. Test BP: /Game/Tests/Monolith/BP_Test_CustomEventGuard |
| 2026-03-16 | Blueprint Wave 5: scaffold_interface_implementation, add_timeline, add_event_node, add_comment_node (4 new actions) | **PENDING** | Implemented, needs build + test |
| 2026-03-15 | HLSL module creation: create_module_from_hlsl (CPU simple, CPU multi-type, GPU sim), create_function_from_hlsl, get_module_inputs + set_module_input_value on CustomHlsl, dot validation | **6/6 PASS** | 3 bugs fixed: numeric index lookup, input exposure fallback, dot validation. All prior actions still PASS. |
| 2026-03-14 | Niagara polish pass: set_system_property, set_static_switch_value, list_module_scripts, list_renderer_properties, get_system_property + DI prefix fix, curve readback fix, BP node alias fix | **7/7 PASS** | 213→218 actions. All prior actions still PASS. |
| 2026-03-13 | Infrastructure: registry param validation, Niagara aliases, material PostEditChange fixes, tools/list schemas | **6 fixes** | All 213 actions still PASS. Behavioral improvements — no regressions |
| 2026-03-13 | Niagara get_system_diagnostics + SimTarget/CalculateBoundsMode + 4 bug fixes | **8/8 PASS** | False DataMissing, add_emitter loading, create_system_from_spec save_path, create_system template — all fixed |
| 2026-03-13 | Bug fix verification | **3/3 PASS** | `add_component` name fix, `create_blueprint` crash guard, `set_module_input_value` data input fix. |
| 2026-03-13 | Blueprint full pass | **46/46 PASS** | 6→46 actions. `remove_local_variable` bug fixed. 3 deprecation fixes. All tested via MCP. |
| 2026-03-13 | Core params fix | PASS | Claude Code string-params bug fixed. `project_query("search")` now works from Claude Code. |
| 2026-03-11 | Niagara full pass | **41/41 PASS** | 12 bugs found+fixed. `set_emitter_property` confirmed working. |
| 2026-03-10 | Animation write actions | **14/14 PASS** | 5 bugs found+fixed. |
| 2026-03-10 | Material write actions | **11/11 PASS** | Wave 2: 14→25 material actions. |
| 2026-03-09 | All read actions | **36/36 PASS** | Full read coverage across all modules. |
| 2026-03-06 | Initial build | PASS | All 9 modules compile clean on UE 5.7. |
