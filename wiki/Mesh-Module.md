# Mesh Module

**194 core actions (+45 work-in-progress town gen, off by default and not in the public count). 22 capability tiers. The biggest module in Monolith by a wide margin.**

MonolithMesh started as basic mesh inspection and scene actor manipulation, then kept growing. It now covers the entire spatial side of Unreal level design — from reading triangle counts to procedurally generating entire building shells with furniture inside them. Plus 45 procedural town generation actions for full city-block-scale generation (**work-in-progress** — disabled by default, enable via `bEnableProceduralTownGen` in Project Settings > Plugins > Monolith). Unless you're willing to dig in and help improve it, it's best left alone for now. If it involves 3D space, placed actors, or level geometry, there's probably a mesh action for it.

---

## Capability Tiers

Here's the full breakdown of what's available, grouped by tier. For exact action signatures and parameters, see [[Tool Reference]].

### Tier 1: Mesh Inspection (12 actions)

Read-only queries on any StaticMesh or SkeletalMesh asset. Get triangle/vertex counts, bounds, materials, LODs, collision shapes, UV channel info, and quality analysis. `compare_meshes` does side-by-side delta with percentages. `search_meshes_by_size` finds assets by dimension range from the indexed mesh catalog — handy for blockout asset matching.

### Tier 2: Scene Manipulation (8 actions)

Actor CRUD on placed level actors. Spawn actors from mesh paths or class names, move/duplicate/delete them, group into folders, set properties (mobility, physics, shadows, tags). `batch_execute` wraps up to 200 actions in a single undo transaction.

### Tier 3: Scene Spatial Queries (11 actions)

Physics-based world queries that work in-editor without a play session. Line traces (single and multi-hit), radial sweeps (sonar-style compass output), overlap tests, nearest-actor search, line-of-sight checks, volume actor enumeration, scene bounds, statistics, spatial relationship detection (on_top_of, adjacent, near...), and navmesh path queries.

### Tier 4: Level Blockout (15 actions)

The blockout system uses standard actor tags on `ABlockingVolume` — zero runtime footprint. Tag a volume with room type and density, drop in blockout primitives, auto-match them to real assets from your project's mesh catalog by size, then atomically replace the whole blockout with production meshes. Export/import layouts as JSON. Scan volumes with a raycaster that maps walls, floors, ceilings, and openings. Poisson disk prop scatter with collision avoidance.

**Tag convention:**
```
Monolith.Blockout                — sentinel (required)
Monolith.Room:Kitchen            — room type
Monolith.Tag:Furniture.Kitchen   — asset matching tag
Monolith.Density:Normal          — Sparse/Normal/Dense/Cluttered
Monolith.AllowPhysics            — presence = true
Monolith.Owner:BV_Kitchen        — on primitives, links to volume
Monolith.BlockoutPrimitive       — marks as blockout primitive
Monolith.Label:Counter_North     — human-readable label
```

**Typical workflow:**
```
get_blockout_volumes -> scan_volume -> create_blockout_primitives_batch -> match_all_in_volume -> apply_replacement
```

### Tier 5: GeometryScript Mesh Operations (12 actions)

Requires `WITH_GEOMETRYSCRIPT`. Handle-based workflow — load a mesh into an editable handle, operate on it (boolean, simplify, remesh, LOD generation, hole filling, UV projection, mirroring), then save the result as a new StaticMesh asset. Handles are named and tracked with tri counts and last-access timestamps.

```
create_handle -> [mesh_simplify | mesh_boolean | compute_uvs | ...] -> save_handle -> release_handle
```

### Tier 6: Horror Spatial Analysis (8 actions) — *Work-in-progress: functional but still being refined*

Sightline analysis with claustrophobia scoring (blocked % at 5/10/20m ranges). Grid-sampled hiding spot detection from viewpoints. Ambush point finding with surprise angles. Choke point analysis via navmesh path width. Escape route scoring by threat exposure. Zone tension classification (calm/uneasy/tense/dread/panic composite). Pacing curve analysis along player paths with scare point detection. Dead-end detection via navmesh flood-fill.

For full workflows, see [[Horror Level Design]].

### Tier 7: Accessibility Analysis (6 actions)

Built for players with diverse accessibility needs. Validates wheelchair clearance (120cm default), navigation complexity (cognitive difficulty), visual contrast (WCAG-inspired), rest point spacing along paths, interactive reach (height/distance/obstruction), and generates full accessibility reports with motor/vision/cognitive profiles and A-F grading.

### Tier 8: Lighting Analysis (5 actions) — *Work-in-progress: functional but still being refined*

Hybrid scene capture and analytic approach. Sample light levels at arbitrary points. Find dark corners via ortho capture and flood-fill. Flag harsh bright-to-dark transitions (>4:1 ratio). Map coverage percentage (lit/shadow/dark) with light inventory. Suggest placement for mood presets: `horror_dim`, `safe_room`, `clinical`.

### Tier 9: Audio & Acoustics (14 actions) — *Work-in-progress: functional but still being refined*

Physical material-aware spatial audio. Enumerate AudioVolumes. Detect surface material breakdown (% concrete, tile, metal, etc.). Estimate footstep loudness by surface. Sabine RT60 reverb estimation from room surfaces. Sound propagation analysis with material-based occlusion. Dangerous surface detection (metal, glass, gravel). Image-source reflection path tracing. AI hearing simulation with surface awareness. Per-cell stealth maps. Quiet path routing via navmesh. Auto-suggest and create AudioVolumes from room materials. Surface type overrides. Bootstrap the whole system with `create_surface_datatable` (12 horror surface types).

**Horror audio workflow:**
```
create_surface_datatable (first time) -> get_surface_materials -> analyze_room_acoustics -> get_stealth_map -> can_ai_hear_from -> suggest_audio_volumes
```

### Tier 10: Performance Analysis (5 actions) — *Work-in-progress: functional but still being refined*

Budget-aware placement tools. Get region performance (tri count, draw calls, lights, shadow casters). Pre-spawn cost estimation without actually spawning anything. Overdraw hotspot detection for translucent screen-space overlap. Shadow cost analysis that flags small props casting shadows pointlessly. Frustum-culled LOD-aware triangle budget vs target.

### Tier 11: Decal & Detail Placement (4 actions)

Environmental storytelling tools. Place surface-aligned decals with Poisson scatter or at specific locations. Place assets along Catmull-Rom spline paths (blood trails, footprints, drag marks). Analyze prop density vs target per grid cell. 5 horror storytelling presets: `violence`, `abandoned`, `dragged`, `medical`, `corruption`.

### Tier 12: Level Design (9 actions)

Core level design utilities. Spawn point/spot/rect/directional lights. Modify existing light properties. Assign materials to placed actors. Bulk material replacement across the level. Find and replace mesh instances. Set LOD transition screen sizes. Find instancing candidates (repeated meshes). Convert StaticMeshActors to HierarchicalInstancedStaticMesh. Read arbitrary UPROPERTYs from actor components.

### Tier 13: Volumes & Properties (7 actions)

Spawn volumes (trigger, kill, pain, blocking, audio, post_process). Read and copy actor properties via reflection. Trigger navmesh rebuild. Control editor selection and camera focus. Snap actors to surfaces with normal alignment. Set collision presets.

### Tier 14: Horror Intelligence (4 actions)

Player path prediction with 4 strategies (shortest, safest, curious, cautious). Spawn point evaluation with composite quality scores (S-F grade). Optimal scare position suggestion along player paths. Encounter pacing evaluation for spacing, intensity, and rest periods.

### Tier 15: Tech Art Pipeline (7 actions)

FBX/glTF mesh import with settings. GeometryScript mesh repair (degenerate tris, vertex welding, hole fill, normal recompute). One-shot LOD chain generation. Texel density analysis. Shader complexity analysis per placed mesh. Collision authoring on mesh assets. Lightmap resolution and UV density analysis.

### Tier 16: Advanced Level Design (8 actions)

Sublevel management (create, add, remove, move actors to). Spawn Blueprint actors with exposed property values. Place spline actors with optional mesh segments. Level Instance (prefab) creation and placement. Batch randomize transforms. Filtered actor enumeration. Distance measurement (Euclidean, horizontal, navmesh).

### Tier 17: Context-Aware Props (8 actions)

Scatter props ON surfaces — shelves, tables, countertops, not just floors. Apply room disturbance levels (orderly, slightly_messy, ransacked, abandoned). Configure physics simulation and sleep states. Settle props with trace-snap and random tilt. Author and place themed prop kits. Scatter on walls (horizontal trace) and ceilings (upward trace).

### Tier 18: Procedural Geometry (8 actions)

GeometryScript-powered parametric mesh generation. See [[Procedural Geometry]] for a full tutorial.

- **Furniture:** 15 types (chair, table, shelf, desk, bed, cabinet, dresser, nightstand, bookcase, wardrobe, couch, bathtub, sink, toilet, mirror)
- **Horror props:** 7 types (barricade, boarded_window, debris_pile, broken_furniture, vent_grate, cage, chain_link)
- **Structures:** Room, corridor, L-junction, T-junction with configurable openings
- **Buildings:** Multi-story shells from 2D polygon footprints
- **Mazes:** 3 algorithms (recursive_backtrack, prims, ellers) with layout JSON output
- **Pipes:** Swept polygon with ball joints along path points
- **Fragmentation:** Plane-slice mesh destruction
- **Terrain:** Perlin noise heightmap patches

### Tier 19: Genre Presets (8 actions)

Horror ships as the default, but the preset system is genre-agnostic. Author storytelling patterns, acoustic profiles, and tension profiles for any genre. Export/import complete preset packs as JSON. See [[Genre Presets]] for how to create your own.

### Tier 20: Encounter Design (8 actions)

The capstone tier. `design_encounter` is a composite action that evaluates spawn points, suggests patrol routes, analyzes exits and sightlines, and delivers a complete encounter design for a region. Also includes patrol route suggestion (stalker, patrol, ambusher archetypes), AI territory heatmaps, safe room evaluation with defensibility + accessibility amenity scoring, level pacing structure analysis, scare sequence generation (4 styles), horror intensity validation, and full accessibility reports. See [[Horror Level Design]].

### Tier 21-22: Quality & Polish (9 actions)

Naming convention validation (SM_/M_/T_ prefixes). Batch rename with reference fixup. Proxy mesh generation (merge meshes into simplified proxy). HLOD configuration. Texture budget analysis. Composition framing analysis (rule of thirds). Monster reveal scoring (silhouette, distance, backlight). Co-op balance placeholder. Integration hook stubs for AI Director, GAS, and telemetry.

---

## Gotchas

A few things that will burn you if you're not expecting them:

- **`spawn_actor` does NOT spawn `ABlockingVolume`** — use the editor for volumes, then tag them via `setup_blockout_volume`
- **`delete_actors` deletes placed actors, NOT asset files** — use `editor_query("delete_assets")` for that
- **`batch_execute` rejects nested `batch_execute`** and caps at 200 actions per call
- **`set_actor_properties`: Mobility must be "Movable" BEFORE enabling SimulatePhysics**
- **`query_radial_sweep` hard cap:** `ray_count * vertical_angles <= 512`
- **`search_meshes_by_size` requires `monolith_reindex()`** to have populated the mesh catalog first
- **All spatial queries work in editor** WITHOUT a play session — no PIE needed
- **`query_` prefix = active physics queries. `get_` prefix = reads stored data**
- **GeometryScript actions require the GeometryScriptingEditor plugin** — it's bundled with UE 5.7 but must be enabled

---

## Related Pages

- [[Tool Reference]] — Full action tables with descriptions
- [[Genre Presets]] — Creating preset packs for any genre
- [[Procedural Geometry]] — Tutorial for the 8 procedural geometry actions
- [[Horror Level Design]] — Horror analysis, encounter design, and accessibility considerations
