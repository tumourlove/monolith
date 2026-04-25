# Monolith — MonolithMesh Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.1 (Beta)

---

## MonolithMesh

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, MonolithIndex, SQLiteCore, UnrealEd, EditorSubsystem, MeshDescription, StaticMeshDescription, MeshConversion, PhysicsCore, NavigationSystem, RenderCore, RHI, EditorScriptingUtilities, Json, JsonUtilities, Slate, SlateCore, AssetRegistry, AssetTools, MeshReductionInterface, MeshMergeUtilities, LevelInstanceEditor, ImageCore. Optional: GeometryScriptingCore, GeometryFramework, GeometryCore (Tier 5 mesh ops, gates `WITH_GEOMETRYSCRIPT`)

**Build.cs notes — conditional GeometryScripting (v0.14.1):** The Build.cs probes `Engine/Plugins/Runtime/GeometryScripting` and adds `GeometryScriptingCore`, `GeometryFramework`, `GeometryCore` + `WITH_GEOMETRYSCRIPT=1` only when found. **Release escape hatch:** setting `MONOLITH_RELEASE_BUILD=1` (env var) short-circuits detection so `WITH_GEOMETRYSCRIPT=0` regardless — the released DLL no longer carries a hard import on `UnrealEditor-GeometryScriptingCore.dll`. This fixes #26 / #30 where users without GeometryScripting enabled in their `.uproject` were hitting `GetLastError=126` at module load. Mirrors the canonical `MonolithBABridge.Build.cs` pattern (and matches the `MonolithUI` CommonUI detection). Source-tree users with GeometryScripting enabled still get full Tier 5 functionality.

### Classes

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

### Actions (242 — namespace: "mesh")

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

### Procedural Town Generator (45 gated + 1 always-registered actions — 11 sub-projects) — WORK-IN-PROGRESS

> **Status:** Work-in-progress, disabled by default (`bEnableProceduralTownGen = false`). Fix Plans v2-v5 addressed 27+ issues but fundamental geometry problems remain (wall misalignment, room separation). Very much a WIP — unless you're willing to dig in and help improve it, it's best left alone.

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

### Fix Plan v2 Changes (2026-03-28)

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
