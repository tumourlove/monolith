# Procedural Geometry

**8 actions that generate meshes from parameters. No modeling software needed.**

All procedural geometry actions use GeometryScript under the hood. They create mesh handles that you can further modify (boolean, simplify, remesh) before saving as StaticMesh assets. This is great for rapid prototyping, blockout-to-production workflows, and generating filler props that don't justify opening Blender.

> **Prerequisite:** GeometryScriptingEditor plugin must be enabled (it ships with UE 5.7).

---

## The 8 Actions

### `create_parametric_mesh` — Furniture (15 types)

Generates parametric furniture meshes. Each type has sensible defaults but accepts dimension overrides.

**Available types:** `chair`, `table`, `shelf`, `desk`, `bed`, `cabinet`, `dresser`, `nightstand`, `bookcase`, `wardrobe`, `couch`, `bathtub`, `sink`, `toilet`, `mirror`

```json
mesh_query({
  "action": "create_parametric_mesh",
  "params": {
    "type": "table",
    "params": {
      "width": 120,
      "depth": 80,
      "height": 75,
      "leg_thickness": 5
    }
  }
})
```

Returns a mesh handle. The mesh is in memory until you `save_handle` it.

### `create_horror_prop` — Horror Props (7 types)

Horror-specific environmental props with randomization via seed.

**Available types:** `barricade`, `boarded_window`, `debris_pile`, `broken_furniture`, `vent_grate`, `cage`, `chain_link`

```json
mesh_query({
  "action": "create_horror_prop",
  "params": {
    "type": "barricade",
    "params": {
      "width": 150,
      "height": 200
    },
    "seed": 42
  }
})
```

The `seed` parameter controls randomized elements (board angles on barricades, debris distribution on piles, etc.). Same seed = same result.

### `create_structure` — Architectural Pieces

Generates room shells, corridors, and junction pieces with configurable openings.

**Available types:** `room`, `corridor`, `l_junction`, `t_junction`

```json
mesh_query({
  "action": "create_structure",
  "params": {
    "type": "room",
    "params": {
      "width": 500,
      "depth": 400,
      "height": 300,
      "openings": [
        {"wall": "north", "position": 0.5, "width": 120, "height": 220},
        {"wall": "east", "position": 0.3, "width": 100, "height": 220}
      ],
      "wall_thickness": 15
    }
  }
})
```

Openings are specified per-wall with position as a 0-1 fraction along the wall length.

### `create_building_shell` — Multi-Story Buildings

Extrudes a 2D polygon footprint into a multi-story building with floor slabs and optional window openings.

```json
mesh_query({
  "action": "create_building_shell",
  "params": {
    "floors": 3,
    "footprint": [
      [0, 0], [1000, 0], [1000, 800], [500, 800], [500, 500], [0, 500]
    ],
    "floor_height": 300,
    "wall_thickness": 20
  }
})
```

The footprint is a 2D polygon (array of [X, Y] points). L-shaped, T-shaped, irregular — anything that forms a valid polygon.

### `create_maze` — Maze Generation

Three algorithms, each with different characteristics:

- **`recursive_backtrack`** — Long winding corridors, single solution path, good for horror
- **`prims`** — More branching, feels more organic
- **`ellers`** — Memory-efficient, generates row by row, produces wider mazes

```json
mesh_query({
  "action": "create_maze",
  "params": {
    "algorithm": "recursive_backtrack",
    "grid_size": [10, 10],
    "cell_size": 300,
    "wall_height": 250,
    "wall_thickness": 15,
    "seed": 12345
  }
})
```

Returns both a mesh handle AND a layout JSON with the maze grid (which cells connect to which), so you can place props, lights, or enemies programmatically based on the maze structure.

### `create_pipe_network` — Pipes Along Paths

Sweeps a polygon along path points with ball joints at each vertex.

```json
mesh_query({
  "action": "create_pipe_network",
  "params": {
    "path_points": [
      [0, 0, 200], [500, 0, 200], [500, 300, 200], [500, 300, 0]
    ],
    "radius": 15,
    "segments": 8
  }
})
```

Great for industrial environments, basement levels, or sci-fi corridors where you need pipes running along walls and ceilings.

### `create_fragments` — Mesh Destruction

Plane-slice fragmentation that cuts a source mesh into pieces. Good for destructible props or debris.

```json
mesh_query({
  "action": "create_fragments",
  "params": {
    "source_handle": "my_vase",
    "count": 8,
    "seed": 99
  }
})
```

Requires a source handle (load one first with `create_handle`). Returns multiple handles — one per fragment. Each fragment is a valid closed mesh.

### `create_terrain_patch` — Heightmap Terrain

Generates a flat mesh with Perlin noise displacement. Good for outdoor areas, basements with uneven floors, or cave sections.

```json
mesh_query({
  "action": "create_terrain_patch",
  "params": {
    "size": [2000, 2000],
    "resolution": [64, 64],
    "noise": {
      "scale": 0.005,
      "amplitude": 200,
      "octaves": 4,
      "persistence": 0.5
    }
  }
})
```

---

## Combining Actions

The real power is in combining these. Here are some practical recipes.

### Furnished Room

```
1. create_structure (type: "room") -> room_handle
2. create_parametric_mesh (type: "table") -> table_handle
3. save_handle (room_handle -> /Game/Meshes/SM_Room)
4. save_handle (table_handle -> /Game/Meshes/SM_Table)
5. spawn_actor (SM_Room at origin)
6. spawn_actor (SM_Table at room center)
7. scatter_on_surface (table actor, small props)
```

### Maze With Furniture

```
1. create_maze (recursive_backtrack, 8x8) -> maze_handle + layout JSON
2. save_handle (maze_handle -> /Game/Meshes/SM_Maze)
3. spawn_actor (SM_Maze at origin)
4. For each dead-end cell in the layout JSON:
   - create_parametric_mesh (random furniture type)
   - save_handle + spawn at cell center
5. suggest_light_placement (for the maze volume)
6. scatter_props (debris in corridors)
```

### Destructible Prop

```
1. create_handle (source: "/Game/Meshes/SM_Crate") -> crate_handle
2. create_fragments (source_handle: "crate_handle", count: 6) -> fragment handles
3. For each fragment:
   - save_handle (-> /Game/Meshes/SM_Crate_Frag_N)
4. Use fragments as physics debris spawned at runtime
```

### Building With Interior

```
1. create_building_shell (3 floors, L-shaped footprint) -> building_handle
2. save_handle (-> /Game/Meshes/SM_Building)
3. spawn_actor (SM_Building)
4. For each floor:
   - create_structure (type: "room") for interior rooms
   - create_parametric_mesh for furniture
   - save + spawn everything
5. place_light (point lights in each room)
6. get_light_coverage (verify no pitch-black rooms)
```

---

## Tips

- **Always save handles when you're done.** Handles are in-memory only — they don't persist across editor restarts. If you want to keep the mesh, `save_handle` it to an asset path.
- **Release handles you're done with.** The handle pool has memory limits. `release_handle` or `list_handles` to see what's active.
- **Use seeds for reproducibility.** Mazes, horror props, fragments, and terrain all accept a `seed` parameter. Same seed = same output every time.
- **Combine with blockout workflow.** Generate a parametric structure, save it, then use it as a blockout volume's replacement mesh. The procedural geometry tools and the blockout system are designed to work together.
- **GeometryScript operations chain.** After `create_parametric_mesh`, you can `mesh_simplify` or `mesh_boolean` the result before saving. The handle is a live editable mesh.

---

## Related Pages

- [[Mesh Module]] — Full MonolithMesh overview
- [[Tool Reference]] — Action signatures and parameters
- [[Genre Presets]] — Authoring storytelling and prop presets for any genre
