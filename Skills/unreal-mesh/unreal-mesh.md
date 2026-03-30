---
name: unreal-mesh
description: Use when working with Unreal Engine meshes, scene spatial queries, level blockout, actor manipulation, 3D awareness, horror spatial analysis, accessibility validation, GeometryScript mesh operations, lighting analysis, audio/acoustics, performance budgeting, decal/detail placement, level design (lights, volumes, sublevels, prefabs), tech art (import, LOD, texel density, collision), context-aware prop placement (surface scatter, disturbance, physics sleep), procedural geometry (furniture, structures, mazes, pipes, terrain), procedural town generation (buildings, facades, roofs, streets, city blocks, floor plans, spatial registry, foundations, balconies, fire escapes, room furnishing, debug views, validation), genre presets, encounter design, or accessibility reports via Monolith MCP. Triggers on mesh, StaticMesh, SkeletalMesh, blockout, spatial, raycast, overlap, scene, actor, spawn, LOD, collision, UV, triangle, bounds, scan volume, scatter, navmesh, sightline, hiding, horror, tension, accessibility, wheelchair, lighting, dark, audio, acoustic, surface, footstep, reverb, performance, budget, draw call, decal, blood trail, light, volume, trigger, sublevel, prefab, spline, import, texel, instancing, HISM, material swap, parametric, structure, maze, pipe, terrain, fragment, preset, encounter, patrol, safe room, accessibility report, prop kit, disturbance, town, city, block, building, facade, roof, street, floor plan, archetype, lot, foundation, balcony, porch, fire escape, ramp, railing, furnish, furniture, bookmark, section view, validate building.
---

# Unreal Mesh & Spatial Workflows

You have access to **Monolith** with **242 Mesh actions** (197 core + 45 experimental town gen) via `mesh_query()`. Town gen actions require `bEnableProceduralTownGen = true` in Editor Preferences (disabled by default — work-in-progress with known geometry issues; unless you're willing to dig in and help improve it, best left alone for now).

### New in Overhaul (5 new actions + major feature upgrades):
- `create_blueprint_prefab` -- Dialog-free Blueprint prefab from world actors
- `list_cached_meshes` / `clear_cache` / `validate_cache` / `get_cache_stats` -- Proc mesh cache management
- **Sweep thin walls** -- `create_structure` now uses `wall_mode: "sweep"` (default) for proper architectural walls
- **Auto-collision** -- All `save_handle` calls auto-generate ConvexHulls collision (`collision` param)
- **Floor snap** -- All proc gen spawns auto-snap to floor (`snap_to_floor` param)
- **Collision-aware scatter** -- `collision_mode: "reject"/"adjust"` on all scatter actions
- **Trim frames** -- `add_trim: true` on `create_structure` for door/window/vent frames
- **Proc mesh caching** -- `use_cache`/`auto_save` on all proc gen actions, hash-based dedup

## World Outliner Organization (MANDATORY)

**Every actor spawned by Monolith MUST be placed in an Outliner folder.** Never leave actors loose at root level. Use the `folder` param, or the action should auto-assign one.

| Action | Default Folder |
|--------|---------------|
| `create_parametric_mesh` | `/Procedural/{Type}` |
| `create_structure` | `/Procedural/Structure` |
| `create_horror_prop` | `/Procedural/Horror` |
| `create_maze` | `/Procedural/Maze` |
| `create_building_shell` | `/Procedural/Building` |
| `create_pipe_network` | `/Procedural/Pipes` |
| `create_terrain_patch` | `/Procedural/Terrain` |
| `create_building_from_grid` | `/Procedural/Town/{BuildingName}` |
| `create_city_block` | `/Procedural/Town/{BlockName}` |
| `create_street` | `/Procedural/Town/Streets` |
| `place_street_furniture` | `/Procedural/Town/StreetFurniture` |
| `create_foundation` | `/Procedural/Town/Foundations` |
| `create_balcony` | `/Procedural/Town/Features` |
| `create_porch` | `/Procedural/Town/Features` |
| `create_fire_escape` | `/Procedural/Town/Features` |
| `furnish_room` | `/Procedural/Town/Furniture` |
| `furnish_building` | `/Procedural/Town/Furniture` |
| `scatter_props` | `/Scatter/{VolumeName}` |
| `scatter_on_surface` | `/Surface/{SurfaceActorName}` |
| `scatter_on_walls` | `/Scatter/{VolumeName}/Walls` |
| `scatter_on_ceiling` | `/Scatter/{VolumeName}/Ceiling` |
| `place_light` | `/Lights` |
| `spawn_actor` | `/Spawned` |
| `place_blueprint_actor` | `/Prefabs` |
| `place_prop_kit` | `/Props/Kits/{KitName}` |
| `place_storytelling_scene` | `/Storytelling/{Pattern}` |
| `place_along_path` | `/PathProps` |
| `place_decals` | `/Decals` |

When calling any spawn/place action, **always pass `folder`** if the action supports it. If orchestrating multiple related spawns, group them under a descriptive folder.

## Discovery

Always discover available actions first:
```
monolith_discover({ namespace: "mesh" })
```

## Key Parameter Names

- `asset_path` -- mesh asset path for inspection actions
- `actor_name` -- placed level actor name (label or internal name)
- `volume_name` -- name of a BlockingVolume with Monolith.Blockout tag
- `handle` -- named mesh handle (Phase 5, GeometryScript only)
- `building_id` -- registered building in spatial registry (town gen)
- `room_id` -- registered room in spatial registry (town gen)
- `block_id` -- city block identifier (town gen)

## Action Reference

### Mesh Inspection (12 actions) -- Read-only, query any StaticMesh/SkeletalMesh

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_mesh_info` | `asset_path` | Tri/vert count, bounds, materials, LODs, collision, Nanite, vertex colors |
| `get_mesh_bounds` | `asset_path` | AABB, extent, sphere radius, volume (cm3), surface area |
| `get_mesh_materials` | `asset_path` | Per-section material paths + tri counts |
| `get_mesh_lods` | `asset_path` | Per-LOD tri/vert counts + screen sizes |
| `get_mesh_collision` | `asset_path` | Collision type, shape counts (box/sphere/capsule/convex) |
| `get_mesh_uvs` | `asset_path`, `lod_index`?, `uv_channel`? | UV channels, island count, overlap % |
| `analyze_skeletal_mesh` | `asset_path` | Quality analysis: bone weights, degenerate tris, LOD delta |
| `analyze_mesh_quality` | `asset_path` | Non-manifold edges, degenerate tris, loose verts, quality score |
| `compare_meshes` | `asset_path_a`, `asset_path_b` | Side-by-side delta with percentages |
| `get_vertex_data` | `asset_path`, `offset`?, `limit`? | Paginated vertex positions + normals (max 5000) |
| `search_meshes_by_size` | `min_bounds`, `max_bounds`, `category`? | Find meshes by dimension range from catalog |
| `get_mesh_catalog_stats` | -- | Total indexed meshes, category + size breakdown |

### Scene Manipulation (8 actions) -- Actor CRUD on placed level actors

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_actor_info` | `actor_name` | Class, transform, mesh, materials, mobility, tags, components |
| `spawn_actor` | `class_or_mesh`, `location`, `rotation`?, `scale`?, `name`? | Spawn StaticMeshActor (path starts with `/`) or class |
| `move_actor` | `actor_name`, `location`?, `rotation`?, `scale`?, `relative`? | Set or offset actor transform |
| `duplicate_actor` | `actor_name`, `new_name`?, `offset`? | Clone actor with optional offset |
| `delete_actors` | `actor_names` | Delete placed actors (NOT asset files) |
| `group_actors` | `actor_names`, `group_name` | Move actors to folder |
| `set_actor_properties` | `actor_name`, `mobility`?, `simulate_physics`?, `tags`? | Set mobility, physics, shadows, tags, mass |
| `batch_execute` | `actions` | Multiple actions in single undo transaction (cap 200) |

### Scene Spatial Queries (11 actions) -- Physics-based world queries

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `query_raycast` | `start`, `end`, `channel`? | Line trace -- hit actor, location, normal, distance, phys material |
| `query_multi_raycast` | `start`, `end`, `max_hits`? | Multi-hit trace sorted by distance |
| `query_radial_sweep` | `origin`, `radius`?, `ray_count`?, `vertical_angles`? | Sonar sweep -- compass direction summary (cap 512 rays) |
| `query_overlap` | `location`, `shape`, `extent` | Overlap test (box/sphere/capsule) |
| `query_nearest` | `location`, `class_filter`?, `tag_filter`?, `radius`? | Find nearest actors (broadphase) |
| `query_line_of_sight` | `from`, `to` | Visibility check -- bool + blocking actor |
| `get_actors_in_volume` | `volume_name` | All actors in a named volume |
| `get_scene_bounds` | `class_filter`? | World AABB enclosing all actors |
| `get_scene_statistics` | `region_min`?, `region_max`? | Actor counts, total tris, lights, navmesh status |
| `get_spatial_relationships` | `actor_name`, `radius`?, `limit`? | Neighbors with relationships (on_top_of, adjacent, near...) |
| `query_navmesh` | `start`, `end`, `agent_radius`? | Navigation path query -- points, distance, reachable |

### Level Blockout (15 actions) -- Tag-based blockout volumes, asset matching, replacement

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_blockout_volumes` | -- | List all Monolith.Blockout tagged volumes |
| `get_blockout_volume_info` | `volume_name` | Volume details, primitive list, other actors |
| `setup_blockout_volume` | `volume_name`, `room_type`, `tags`?, `density`? | Apply Monolith tags to a BlockingVolume |
| `create_blockout_primitive` | `shape`, `location`, `scale`, `label`?, `volume_name`? | Spawn tagged blockout primitive |
| `create_blockout_primitives_batch` | `primitives`, `volume_name`? | Batch placement (cap 200, single undo) |
| `create_blockout_grid` | `volume_name`, `cell_size` | Floor grid in volume |
| `match_asset_to_blockout` | `blockout_actor`, `tolerance_pct`?, `top_n`? | Find size-matched assets from catalog |
| `match_all_in_volume` | `volume_name`, `tolerance_pct`?, `top_n`? | Batch match all primitives in volume |
| `apply_replacement` | `replacements` | Atomic replace blockouts with real assets (pivot-adjusted) |
| `set_actor_tags` | `actor_tags` | Batch apply tags |
| `clear_blockout` | `volume_name`, `keep_tagged`? | Remove blockout primitives by owner tag |
| `export_blockout_layout` | `volume_name` | Export as JSON (positions relative 0-1, sizes absolute) |
| `import_blockout_layout` | `volume_name`, `layout_json` | Import layout (scales positions, keeps sizes) |
| `scan_volume` | `volume_name`, `ray_density`? | Daredevil scan -- walls, floor, ceiling, openings |
| `scatter_props` | `volume_name`, `asset_paths`, `count`, `min_spacing`?, `seed`? | Poisson disk scatter with collision avoidance |

### Mesh Operations (12 actions) -- `#if WITH_GEOMETRYSCRIPT`, require handles

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_handle` | `source`, `handle` | Load mesh into editable handle (asset path or "primitive:box") |
| `release_handle` | `handle` | Free a handle |
| `list_handles` | -- | All handles with tri count, last access time |
| `save_handle` | `handle`, `target_path`, `overwrite`? | Commit handle to new StaticMesh asset |
| `mesh_boolean` | `handle_a`, `handle_b`, `operation`, `result_handle` | Union/subtract/intersect |
| `mesh_simplify` | `handle`, `target_triangles`? | Reduce triangle count |
| `mesh_remesh` | `handle`, `target_edge_length` | Isotropic remeshing |
| `generate_collision` | `handle`, `method` | Convex decomp/auto shapes |
| `generate_lods` | `handle`, `lod_count` | LOD chain via repeated simplification |
| `fill_holes` | `handle` | Automatic hole detection + fill |
| `compute_uvs` | `handle`, `method` | Auto-unwrap/box/planar/cylinder projection |
| `mirror_mesh` | `handle`, `axis` | Mirror across X/Y/Z |

### Horror Spatial Analysis (8 actions) — *Work-in-progress: functional but still being refined*

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `analyze_sightlines` | `location`, `fov`?, `ray_count`? | Claustrophobia score, blocked % at 5/10/20m |
| `find_hiding_spots` | `region_min/max`, `viewpoints` | Grid-sample concealment from viewpoints |
| `find_ambush_points` | `path_points`, `lateral_range`? | Concealed positions with surprise angles |
| `analyze_choke_points` | `start`, `end` | Navmesh path width, flank possibility |
| `analyze_escape_routes` | `location`, `exit_tags`? | Paths to exits scored by threat exposure |
| `classify_zone_tension` | `location`, `radius`? | Calm/uneasy/tense/dread/panic composite |
| `analyze_pacing_curve` | `path_points` | Tension along path, scare point detection |
| `find_dead_ends` | `region_min/max`? | Navmesh flood-fill for single-exit regions |

### Accessibility Analysis (6 actions) -- Accessibility validation

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `validate_path_width` | `start`, `end`, `min_width`? | Wheelchair clearance (120cm default) |
| `validate_navigation_complexity` | `start`, `end` | Cognitive difficulty scoring |
| `analyze_visual_contrast` | `location`, `forward`? | WCAG-inspired contrast for interactables |
| `find_rest_points` | `start`, `end`, `max_gap`? | Safe room spacing along path |
| `validate_interactive_reach` | `region`, `tags`? | Height/distance/obstruction checks |
| `generate_accessibility_report` | `start`, `end`, `profile`? | Motor/vision/cognitive profile, A-F grade |

### Lighting Analysis (5 actions) -- Hybrid scene capture + analytic — *Work-in-progress: functional but still being refined*

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `sample_light_levels` | `points`, `mode`? | Per-point luminance via scene capture or analytic |
| `find_dark_corners` | `volume_name` or `region`, `threshold`? | Ortho capture + flood-fill dark zones |
| `analyze_light_transitions` | `path_points`, `sample_interval`? | Flag harsh bright->dark (>4:1 ratio) |
| `get_light_coverage` | `volume_name` | % lit/shadow/dark, light inventory, histogram |
| `suggest_light_placement` | `volume_name`, `mood`? | Inverse-square placement for horror_dim/safe_room/clinical |

### Audio & Acoustics (14 actions) -- Physical material-aware spatial audio — *Work-in-progress: functional but still being refined*

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_audio_volumes` | -- | Enumerate AudioVolumes with reverb settings |
| `get_surface_materials` | `volume_name` or `region` | Physical material breakdown (% concrete, tile, etc.) |
| `estimate_footstep_sound` | `location` | Surface type -> loudness factor (0-1) |
| `analyze_room_acoustics` | `volume_name` | Sabine RT60 reverb estimation from surfaces |
| `analyze_sound_propagation` | `from`, `to` | Material-based occlusion, wall count, dB reduction |
| `find_loud_surfaces` | `volume_name` or `region` | Dangerous surfaces (metal, glass, gravel) |
| `find_sound_paths` | `from`, `to`, `max_bounces`? | Image-source reflection paths with attenuation |
| `can_ai_hear_from` | `ai_location`, `player_location` | Monster hearing detection with surface awareness |
| `get_stealth_map` | `volume_name`, `grid_size`? | Per-cell loudness + detection radius heatmap |
| `find_quiet_path` | `start`, `end` | Navmesh path preferring quiet surfaces |
| `suggest_audio_volumes` | `volume_name` | Auto-suggest reverb settings from room materials |
| `create_audio_volume` | `volume_name`, `reverb_preset` | Spawn AudioVolume with suggested settings |
| `set_surface_type` | `actor_name`, `surface_type` | Set physical material surface override |
| `create_surface_datatable` | `template`? | Bootstrap acoustic system (12 horror surfaces) |

### Performance Analysis (5 actions) -- Budget-aware placement — *Work-in-progress: functional but still being refined*

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_region_performance` | `region_min`, `region_max` | Tri count, draw calls, lights, shadow casters |
| `estimate_placement_cost` | `assets` (array of {path, count}) | Pre-spawn budgeting without spawning |
| `find_overdraw_hotspots` | `viewpoint`, `fov`? | Translucent screen-space overlap |
| `analyze_shadow_cost` | `region_min`, `region_max` | Flag small props casting shadows pointlessly |
| `get_triangle_budget` | `viewpoint`, `fov`?, `budget`? | Frustum-culled LOD-aware count vs budget |

### Decal & Detail Placement (4 actions) -- Environmental storytelling

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `place_decals` | `material`, `locations` or `region`+`count` | Surface-aligned decals, Poisson scatter |
| `place_along_path` | `path_points`, `pattern`? | Blood trails, footprints, drag marks (Catmull-Rom) |
| `analyze_prop_density` | `volume_name`, `target_density`? | Grid-cell density vs target |
| `place_storytelling_scene` | `location`, `pattern`, `intensity`? | 5 horror presets (violence/abandoned/dragged/medical/corruption) |

### Level Design (9 actions) -- Lights, materials, mesh swap, instancing

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `place_light` | `type`, `location`, `intensity`?, `color`? | Spawn point/spot/rect/directional lights |
| `set_light_properties` | `actor_name`, properties... | Modify existing light |
| `set_actor_material` | `actor_name`, `material`, `slot`? | Assign material to placed actor |
| `swap_material_in_level` | `source_material`, `target_material` | Bulk replace material X with Y |
| `find_replace_mesh` | `source_mesh`, `target_mesh` | Swap all instances of mesh X with Y |
| `set_lod_screen_sizes` | `asset_path`, `screen_sizes` | Set LOD transition thresholds |
| `find_instancing_candidates` | `min_count`? | Find repeated meshes for HISM conversion |
| `convert_to_hism` | `mesh`, `actors` | Convert StaticMeshActors to HISM |
| `get_actor_component_properties` | `actor_name`, `component_class`? | Read arbitrary UPROPERTYs |

### Volumes & Properties (7 actions) -- Trigger, kill, navmesh, selection

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `spawn_volume` | `type`, `location`, `extent` | Trigger/kill/pain/blocking/audio/post_process volumes |
| `get_actor_properties` | `actor_name`, `properties`? | Read properties via reflection |
| `copy_actor_properties` | `source_actor`, `target_actors` | Copy settings between actors |
| `build_navmesh` | -- | Trigger navmesh rebuild (one call) |
| `select_actors` | `actor_names`, `mode`? | Control editor selection + camera focus |
| `snap_to_surface` | `actor_names`, `direction`? | Trace + normal alignment |
| `set_collision_preset` | `actor_name`, `preset` | Set collision profile |

### Horror Intelligence (4 actions) -- Player path prediction, encounter scoring

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `predict_player_paths` | `start`, `end`, `strategies`? | 4 strategies: shortest/safest/curious/cautious |
| `evaluate_spawn_point` | `location`, `player_paths`? | Composite spawn quality score (S-F grade) |
| `suggest_scare_positions` | `path_points` | Optimal scripted event positions along path |
| `evaluate_encounter_pacing` | `encounters`, `path_points` | Spacing/intensity/rest period analysis |

### Tech Art Pipeline (7 actions) -- Import, LOD gen, texel density, collision

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `import_mesh` | `file_path`, `save_path` | FBX/glTF import with settings |
| `fix_mesh_quality` | `asset_path`, `ops`? | GeometryScript repair (degenerate/weld/holes/normals) |
| `auto_generate_lods` | `asset_path`, `lod_count`? | One-shot LOD chain generation |
| `analyze_texel_density` | `asset_path` or `region` | UV area x texture res = texels/cm |
| `analyze_material_cost_in_region` | `region` or `actors` | Shader complexity per placed mesh |
| `set_mesh_collision` | `asset_path`, `type`?, `auto_convex`? | Write collision to mesh asset |
| `analyze_lightmap_density` | `region` or `actors` | Lightmap resolution + UV density |

### Advanced Level Design (8 actions) -- Sublevels, prefabs, splines

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `manage_sublevel` | `action`, `sublevel`? | Create/add/remove/move_actors_to |
| `place_blueprint_actor` | `blueprint`, `location`, `properties`? | Spawn BP with exposed property values |
| `place_spline` | `points`, `mesh`? | Spawn spline actor with mesh segments |
| `create_prefab` / `spawn_prefab` | `actors` / `prefab`, `location` | Level Instance creation + placement |
| `randomize_transforms` | `actor_names`, ranges... | Batch random offset/rotation/scale |
| `get_level_actors` | filters... | Filtered actor enumeration |
| `measure_distance` | `from`, `to`, `mode`? | Euclidean/horizontal/navmesh distance |

### Context-Aware Props (8 actions) -- Surface scatter, disturbance, physics

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `scatter_on_surface` | `surface_actor`, `assets`, `count` | Props ON shelves/tables (not just floors) |
| `set_room_disturbance` | `volume_name`, `level` | Orderly/slightly_messy/ransacked/abandoned |
| `configure_physics_props` | `actor_names`, `sleep`? | SimulatePhysics + sleep state |
| `settle_props` | `actor_names`, `seed`? | Trace-snap with random tilt |
| `create_prop_kit` / `place_prop_kit` | kit JSON | Themed prop group authoring + placement |
| `scatter_on_walls` | `volume_name`, `assets`, `count` | Horizontal trace wall placement |
| `scatter_on_ceiling` | `volume_name`, `assets`, `count` | Upward trace ceiling placement |

### Procedural Geometry (8 actions) -- GeometryScript, requires handle pool

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_parametric_mesh` | `type`, `params` | 15 furniture types (chair->bathtub) |
| `create_horror_prop` | `type`, `params`, `seed`? | 7 horror types (barricade->vent_grate) |
| `create_structure` | `type`, `params` | Room/corridor/L/T-junction with openings |
| `create_building_shell` | `floors`, `footprint` | Multi-story from 2D polygon |
| `create_maze` | `algorithm`, `grid_size`, `seed`? | 3 algorithms + layout JSON |
| `create_pipe_network` | `path_points`, `radius`? | Swept polygon with ball joints |
| `create_fragments` | `source_handle`, `count`?, `seed`? | Plane-slice mesh fragmentation |
| `create_terrain_patch` | `size`, `noise`? | Perlin noise heightmap mesh |

### Procedural Town Generation (46 actions) -- Full building-to-block pipeline

Generates entire city blocks from archetype definitions through a layered pipeline: floor plans -> grid geometry -> facades -> roofs -> streets -> spatial registry -> volumes -> furnishing. Each sprint (SP) adds a layer.

#### SP1: Grid Building (2 actions)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_building_from_grid` | `grid`, `descriptor`?, `omit_exterior_walls`? | Grid -> geometry + Building Descriptor output. `omit_exterior_walls: true` defers walls to `generate_facade`. Door width default 110cm |
| `create_grid_from_rooms` | `rooms` | Room rects -> occupancy grid |

#### SP2: Floor Plan Generator (3 actions)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `generate_floor_plan` | `archetype`, `footprint`, `floor_index`? | Archetype + footprint -> grid/rooms/doors. Per-floor room assignment, aspect ratio constraints, footprint validation, guaranteed exterior entrance, corridor width 3 cells |
| `list_building_archetypes` | -- | List available archetype JSON definitions |
| `get_building_archetype` | `archetype` | Get a specific archetype definition |

#### SP3: Facade & Window Generation (3 actions)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `generate_facade` | `building_id`?, `faces`? | Exterior faces -> windows, doors, trim, cornices |
| `list_facade_styles` | -- | List available facade style presets |
| `apply_horror_damage` | `building_id`?, `actors`? | Procedural horror damage to facades |

#### SP4: Roof Generation (1 action)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `generate_roof` | `footprint`, `type` | Footprint + type -> roof geometry (gable/hip/flat/shed/gambrel). Works with `omit_exterior_walls` buildings |

#### SP5: City Block Layout (4 actions)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_city_block` | `block_size`, `archetype`?, `seed`? | Full pipeline: lots -> buildings -> facades -> roofs -> streets |
| `create_lot_layout` | `block_size`, `lot_rules`? | Subdivide block into buildable lots |
| `create_street` | `start`, `end`, `width`? | Street geometry (road, sidewalks, curbs) |
| `place_street_furniture` | `street_id`?, `region`? | Lamps, hydrants, benches, mailboxes |

#### SP6: Spatial Registry (10 actions)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `register_building` | `building_descriptor` | Register building descriptor in spatial registry |
| `register_room` | `room_descriptor` | Register individual room |
| `register_street_furniture` | `furniture_descriptor` | Register street furniture item |
| `query_room_at` | `location` | What room is at position X? |
| `query_adjacent_rooms` | `room_id` | What rooms connect to room X? |
| `query_rooms_by_filter` | `type`?, `floor`?, `min_area`? | Query rooms by type/floor/area |
| `query_building_exits` | `building_id` | Find exterior doors |
| `path_between_rooms` | `from_room`, `to_room` | BFS shortest path through room graph |
| `save_block_descriptor` | `block_id`, `path` | Persist registry to JSON |
| `load_block_descriptor` | `path` | Load saved registry from JSON |

#### SP7: Auto-Volume Generation (3 actions)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `auto_volumes_for_building` | `building_id` | NavMesh + audio + trigger volumes per building |
| `auto_volumes_for_block` | `block_id` | Volumes for entire city block |
| `spawn_nav_link` | `start`, `end`, `bidirectional`? | NavLinkProxy between two points |

#### SP8a: Terrain + Foundations (5 actions)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `sample_terrain_grid` | `origin`, `size`, `resolution` | NxN height grid via line traces |
| `analyze_building_site` | `footprint`, `terrain_grid`? | Determine foundation strategy from terrain |
| `create_foundation` | `footprint`, `strategy`? | Foundation geometry (slab/piers/stepped/stilts/basement) |
| `create_retaining_wall` | `path_points`, `height`? | Retaining wall along terrain cut |
| `place_building_on_terrain` | `building_id`, `location` | Full pipeline: sample -> analyze -> foundation -> adjust Z |

#### SP8b: Architectural Features (5 actions)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_balcony` | `location`, `size`?, `railing_style`?, `building_context`? | Floor slab + railing. Optional `building_context` for auto-orientation; emits `wall_openings` in result |
| `create_porch` | `location`, `size`?, `columns`?, `building_context`? | Covered entry with columns. Optional `building_context` for auto-orientation; emits `wall_openings` in result |
| `create_fire_escape` | `building_id`?, `floors`?, `building_context`? | Zigzag exterior stairs. Optional `building_context` for auto-orientation; emits `wall_openings` in result |
| `create_ramp_connector` | `start`, `end`, `width`?, `building_context`? | ADA-compliant ramp (wheelchair accessibility). Optional `building_context` for auto-orientation; emits `wall_openings` in result |
| `create_railing` | `path_points`, `style`?, `building_context`? | Railing along arbitrary path. Optional `building_context` for auto-orientation; emits `wall_openings` in result |

#### SP9: Daredevil Debug View (6 actions)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `toggle_section_view` | `z_height`?, `enabled`? | Hide geometry above Z height for inspection |
| `toggle_ceiling_visibility` | `building_id`?, `visible`? | Toggle ceiling/roof actor visibility |
| `capture_floor_plan` | `building_id`?, `floor`? | Orthographic PNG capture of floor layout |
| `highlight_room` | `room_id`, `color`? | Overlay material on room bounds |
| `save_camera_bookmark` | `name`, `location`?, `rotation`? | Save viewport camera position |
| `load_camera_bookmark` | `name` | Restore saved camera bookmark |

#### SP10: Room Furnishing (3 actions)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `furnish_room` | `room_id`, `preset`? | Place furniture by room type |
| `furnish_building` | `building_id`, `preset`? | Furnish all rooms in a building |
| `list_furniture_presets` | -- | List available furniture config presets |

#### Building Validation (1 action)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `validate_building` | `building_id` | Playability validation: capsule sweep walkability, BFS room connectivity, stair angle checks, window exterior raycasts. Returns pass/fail per check with details |

### Genre Presets (8 actions) -- Extensible preset system

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `list/create_storytelling_patterns` | `name`, `elements` | Author horror/fantasy/sci-fi storytelling |
| `list/create_acoustic_profiles` | `name`, `surfaces` | Genre-specific surface acoustics |
| `create_tension_profile` | `name`, `weights` | Genre-specific tension factor weights |
| `list_genre_presets` | -- | Browse available preset packs |
| `export/import_genre_preset` | `path`, `merge_mode`? | Bundle/load full preset packs |

### Encounter Design (8 actions) -- Horror advanced + accessibility

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `design_encounter` | `region`, `archetype`? | Capstone: spawn+patrol+exits+sightlines composed |
| `suggest_patrol_route` | `path_points`, `archetype` | Stalker/patrol/ambusher navmesh routes |
| `analyze_ai_territory` | `region` | Hiding/patrol/ambush heatmap |
| `evaluate_safe_room` | `volume_name` | Defensibility + accessibility amenity scoring |
| `analyze_level_pacing_structure` | `path_points` | Macro tension->release mapping |
| `generate_scare_sequence` | `path_points`, `style`? | 4 styles: slow_burn/escalating/relentless/single_peak |
| `validate_horror_intensity` | `path_points` | Accessibility intensity cap audit |
| `generate_hospice_report` | `start`, `end`, `profile`? | 5-section accessibility audit, A-F grade |

### Quality & Polish (9 actions)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `validate_naming_conventions` | `path`? | Flag assets missing SM_/M_/T_ prefixes |
| `batch_rename_assets` | `pattern`, `replacement` | Find/replace with reference fixup |
| `generate_proxy_mesh` | `actors` | Merge meshes into simplified proxy |
| `setup_hlod` | `layer_type`, `cell_size`? | Configure HLOD layers |
| `analyze_texture_budget` | -- | Streaming pool usage + top textures |
| `analyze_framing` | `viewpoint` | Rule-of-thirds composition scoring |
| `evaluate_monster_reveal` | `monster_location`, `player_location` | Silhouette/distance/backlight scoring |
| `analyze_co_op_balance` | `player_positions` | Coverage blind spots (P3 placeholder) |
| `integration_hooks_stub` | `hook_type` | AI Director/GAS/telemetry stubs |

## Blockout Tag Convention

Blockout uses standard actor tags (`TArray<FName>`) on `ABlockingVolume` -- zero runtime footprint:

```
Monolith.Blockout                -- sentinel (required)
Monolith.Room:Kitchen            -- room type
Monolith.Tag:Furniture.Kitchen   -- asset matching tag (one per entry)
Monolith.Density:Normal          -- Sparse/Normal/Dense/Cluttered
Monolith.AllowPhysics            -- presence = true
Monolith.Owner:BV_Kitchen        -- on primitives, links to volume
Monolith.BlockoutPrimitive       -- marks as blockout primitive
Monolith.Label:Counter_North     -- human-readable label
```

## Typical Workflows

### Blockout a Room
```
get_blockout_volumes -> scan_volume -> create_blockout_primitives_batch -> match_all_in_volume -> apply_replacement
```

### Inspect a Mesh
```
get_mesh_info -> analyze_mesh_quality -> compare_meshes
```

### Edit a Mesh (GeometryScript)
```
create_handle -> [mesh_simplify | mesh_boolean | compute_uvs | ...] -> save_handle -> release_handle
```

### Horror Level Design
```
analyze_sightlines -> find_hiding_spots -> analyze_escape_routes -> classify_zone_tension -> analyze_pacing_curve
```

### Accessibility Check
```
generate_accessibility_report (or individual validate_ actions)
```

### Horror Audio Design
```
create_surface_datatable (first time) -> get_surface_materials -> analyze_room_acoustics -> get_stealth_map -> can_ai_hear_from -> suggest_audio_volumes
```

### Lighting Audit
```
get_light_coverage -> find_dark_corners -> analyze_light_transitions -> suggest_light_placement
```

### Performance Budget
```
get_region_performance -> get_triangle_budget -> analyze_shadow_cost -> find_overdraw_hotspots
```

### Environmental Storytelling
```
place_storytelling_scene -> place_along_path -> scatter_props -> analyze_prop_density
```

### Understand a Scene
```
get_scene_statistics -> query_radial_sweep -> get_spatial_relationships
```

### Generate a Single Building (Town Gen)
```
list_building_archetypes -> generate_floor_plan -> create_building_from_grid -> generate_facade -> generate_roof -> register_building -> auto_volumes_for_building -> furnish_building -> validate_building
```

### Generate a City Block (Town Gen -- Full Pipeline)
```
create_city_block (does it all) OR step-by-step:
create_lot_layout -> [generate_floor_plan -> create_building_from_grid -> generate_facade -> generate_roof -> validate_building per lot] -> create_street -> place_street_furniture -> auto_volumes_for_block -> save_block_descriptor
```

### Place Building on Terrain (Town Gen)
```
sample_terrain_grid -> analyze_building_site -> create_foundation -> place_building_on_terrain
```

### Debug/Inspect a Building (Town Gen)
```
toggle_section_view -> toggle_ceiling_visibility -> capture_floor_plan -> highlight_room -> query_room_at -> query_adjacent_rooms
```

### Add Architectural Details (Town Gen)
```
create_balcony / create_porch / create_fire_escape / create_ramp_connector -> apply_horror_damage
```

## Gotchas

- `spawn_actor` does NOT spawn `ABlockingVolume` -- use the editor for volumes
- `delete_actors` deletes placed actors, NOT asset files (use `editor_query("delete_assets")` for that)
- `batch_execute` rejects nested `batch_execute` and caps at 200 actions
- `set_actor_properties`: Mobility must be "Movable" BEFORE enabling SimulatePhysics
- `query_radial_sweep` hard cap: `ray_count * vertical_angles <= 512`
- `search_meshes_by_size` requires `monolith_reindex()` to have populated the mesh catalog first
- All spatial queries work in editor WITHOUT a play session
- `query_` prefix = active physics queries. `get_` prefix = reads stored data
- `create_city_block` is a capstone action -- it calls the full pipeline internally. Use step-by-step actions for fine control
- `register_building` / `register_room` are required before spatial registry queries (`query_room_at`, etc.) will return results
- `save_block_descriptor` / `load_block_descriptor` persist to JSON, not uasset -- these are editor-time descriptors
- `create_ramp_connector` follows ADA slope guidelines by default -- override with `max_slope` param for non-accessible ramps
- `furnish_room` uses the spatial registry -- the room must be registered first
- Stairwells need minimum 4x6 cells (24 grid cells) for switchback stairs at 270cm floor height
- Use `omit_exterior_walls: true` when calling `generate_facade` to avoid double-wall issue
- Pass `building_context` to architectural features (`create_balcony`, `create_porch`, `create_fire_escape`, `create_ramp_connector`, `create_railing`) for auto-orientation from Building Descriptor's `exterior_faces`
- Run `validate_building` after generation to verify playability (capsule sweep, BFS connectivity, stair angles, window raycasts)
