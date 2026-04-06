---
name: unreal-materials
description: Use when creating, editing, or inspecting Unreal Engine materials via Monolith MCP. Covers PBR setup, graph building, material instances, templates, HLSL nodes, validation, and previews. Triggers on material, shader, PBR, texture, material instance, material graph.
---

# Unreal Material Workflows

You have access to **Monolith** with 63 material actions via `material_query()`.

## Discovery

```
monolith_discover({ namespace: "material" })
```

## Asset Path Conventions

All asset paths follow UE content browser format (no .uasset extension):

| Location | Path Format | Example |
|----------|------------|---------|
| Project Content/ | `/Game/Path/To/Asset` | `/Game/Materials/M_Rock` |
| Project Plugins/ | `/PluginName/Path/To/Asset` | `/MassProjectile/Materials/M_Example` |
| Engine Plugins | `/PluginName/Path/To/Asset` | `/Niagara/DefaultAssets/SystemAssets/NS_Default` |

## Key Parameter Names

- `asset_path` — the material asset path (NOT `asset`)

## Action Reference (63 actions)

### Read Actions (21)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_all_expressions` | `asset_path` | List all expression nodes in a material |
| `get_expression_details` | `asset_path`, `expression_name` | Inspect a specific node's properties and pins |
| `get_expression_connections` | `asset_path`, `expression_name` | Get input and output connections for a single expression |
| `get_full_connection_graph` | `asset_path` | Complete node/wire topology |
| `get_material_parameters` | `asset_path` | List all scalar/vector/texture parameters |
| `get_material_properties` | `asset_path` | Read all material settings: blend_mode, shading_model, domain, two_sided, usage_flags, expression_count. Works on UMaterial and UMaterialInstance |
| `get_compilation_stats` | `asset_path` | Instruction counts (vertex + pixel shader), sampler usage, blend mode, compile status |
| `get_layer_info` | `asset_path` | Inspect material layer/blend stack |
| `list_expression_classes` | `filter`?, `category`? | List all available expression classes with pin counts. Cached after first call |
| `get_expression_pin_info` | `class_name` | Query pin names/types for an expression class without creating an instance in a material |
| `list_material_instances` | `parent_path`, `recursive`? | Find all instances of a parent material. Recursive walks instance-of-instance trees |
| `get_function_info` | `asset_path` | Read material function inputs, outputs, description, expression list |
| `export_material_graph` | `asset_path`, `include_properties`?, `include_positions`? | Serialize graph as JSON. Pass `include_properties: false` to reduce ~70% |
| `export_function_graph` | `asset_path`, `include_properties`?, `include_positions`? | Full material function graph export — nodes, connections, properties, inputs, outputs, switch details |
| `get_function_instance_info` | `asset_path` | Read MFI parent chain, all 11 parameter override types, inputs/outputs |
| `get_thumbnail` | `asset_path`, `save_to_file`? | Get thumbnail. Use `save_to_file: true` — inline base64 wastes context |
| `validate_material` | `asset_path`, `fix_issues`? | Check for broken connections, unused nodes, errors |
| `render_preview` | `asset_path`, `uv_tiling`?, `preview_mesh`? | Trigger material compilation and preview. UV tiling for repetition checks |
| `get_texture_properties` | `asset_path` | sRGB, dimensions, compression, filter, address modes, recommended_sampler_type |
| `check_tiling_quality` | `asset_path` | Detect tiling issues: direct UV usage, missing anti-tiling, no macro variation |
| `preview_texture` | `asset_path`, `resolution`?, `output_path`? | Render texture thumbnail + return full metadata |

### Instance Actions (5)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_instance_parameters` | `asset_path` | Read all parameter overrides from a MIC — scalar, vector, texture, static switch with override detection |
| `set_instance_parameters` | `asset_path`, `parameters` (array of `{name, type, value}`) | Batch-set multiple instance parameters in one call. Single recompile at end |
| `set_instance_parent` | `asset_path`, `new_parent` | Reparent a material instance. Reports lost/kept parameters |
| `clear_instance_parameter` | `asset_path`, `parameter_name`, `parameter_type`? | Remove a single override (reverts to parent). Use type `"all"` to clear everything |
| `save_material` | `asset_path`, `only_if_dirty`? | Save material to disk. One-liner |

### Function Actions (9)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_material_function` | `asset_path`, `type`?, `description`?, `expose_to_library`? | Create material function, layer, or layer blend (`type`: MaterialFunction/MaterialLayer/MaterialLayerBlend) |
| `build_function_graph` | `asset_path`, `graph_spec` (with `inputs`, `outputs`, `nodes`, `connections`) | Build function graph with typed I/O. Same node spec as build_material_graph |
| `set_function_metadata` | `asset_path`, `description`?, `expose_to_library`?, `library_categories`? | Update function description, categories, library exposure |
| `delete_function_expression` | `asset_path`, `expression_name` | Remove expression(s) from function. Comma-separated names for batch. Rejects MFI paths |
| `update_material_function` | `asset_path` | Recompile function and cascade to all referencing materials/instances |
| `create_function_instance` | `asset_path`, `parent`, `scalar_overrides`?, `vector_overrides`?, `texture_overrides`?, `static_switch_overrides`? | Create MFI with parent + optional param overrides |
| `set_function_instance_parameter` | `asset_path`, `parameter_name`, `scalar_value`?/`vector_value`?/`texture_value`?/`switch_value`? | Set param override on MFI |
| `layout_function_expressions` | `asset_path` | Auto-arrange function graph layout. Rejects MFI paths |
| `rename_function_parameter_group` | `asset_path`, `old_group`, `new_group` | Rename param group across all parameters |

### Write Actions (23)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `auto_layout` | `asset_path` | Topological-sort layout on all expressions. Works on UMaterial and UMaterialFunction |
| `create_material` | `asset_path`, `blend_mode`?, `shading_model`?, `two_sided`? | Create a new empty material with properties |
| `build_material_graph` | `asset_path`, `graph_spec`, `clear_existing`? | Build entire graph from JSON spec (fastest path). Emits blend mode validation warnings |
| `create_custom_hlsl_node` | `asset_path`, `code`, `inputs`?, `additional_outputs`? | Add a Custom HLSL expression |
| `update_custom_hlsl_node` | `asset_path`, `expression_name`, `code`?, `inputs`?, `additional_outputs`? | Edit existing Custom HLSL node without rebuild |
| `replace_expression` | `asset_path`, `expression_name`, `new_class`, `preserve_connections`? | Swap node type in-place. Reconnects by pin name match, index fallback with warnings |
| `rename_expression` | `asset_path`, `expression_name`, `new_desc` | Set user-visible label (Desc property) on an expression |
| `duplicate_expression` | `asset_path`, `expression_name`, `offset_x`?, `offset_y`? | Duplicate a node with position offset. Connections NOT duplicated |
| `move_expression` | `asset_path`, `expression_name` OR `expressions` (array), `pos_x`, `pos_y`, `relative`? | Reposition expressions. Batch mode with array. `relative: true` for offsets |
| `set_material_property` | `asset_path`, `blend_mode`?, `shading_model`?, `two_sided`?, etc. | Set material-level properties. Accepts both short (`"Additive"`) and prefixed (`"BLEND_Additive"`) enum forms |
| `set_expression_property` | `asset_path`, `expression_name`, `property_name`, `value` | Set a property on an existing expression |
| `connect_expressions` | `asset_path`, `from_expression`, `to_expression`/`to_property` | Wire two expressions or wire to material output |
| `disconnect_expression` | `asset_path`, `expression_name`, `input_name`?, `target_expression`?, `output_index`? | Remove connections. Use `target_expression` + `output_index` for targeted disconnection |
| `delete_expression` | `asset_path`, `expression_name` | Delete an expression node |
| `delete_expressions` | `asset_path`, `expression_names[]` | Batch delete multiple expression nodes |
| `clear_graph` | `asset_path`, `preserve_parameters`? | Remove all expressions (optionally keep parameter nodes) |
| `create_material_instance` | `asset_path`, `parent_material` | Create a material instance |
| `set_instance_parameter` | `asset_path`, `parameter_name`, `scalar_value`/`vector_value`/`texture_value` | Set instance parameter |
| `duplicate_material` | `source_path`, `dest_path` | Duplicate a material asset |
| `recompile_material` | `asset_path` | Force recompile |
| `import_material_graph` | `asset_path`, `graph_json`, `mode`? | Deserialize graph from JSON |
| `begin_transaction` | `transaction_name` | Start an undo group |
| `end_transaction` | — | End an undo group |

### Batch Actions (4)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `batch_set_material_property` | `asset_paths` (array), `properties` | Apply same properties to multiple materials in one call |
| `batch_recompile` | `asset_paths` (array) | Recompile multiple materials, returns per-material VS/PS instruction counts |
| `import_texture` | `source_file`, `dest_path`, `compression`?, `srgb`?, `max_size`? | Import texture from disk with compression/sRGB settings |
| `preview_textures` | `asset_paths[]`, `per_texture_size`?, `output_path`? | Render contact sheet grid of multiple textures with per-texture metadata |

### Compound Actions (1)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_pbr_material_from_disk` | `material_path`, `texture_folder`, `maps`, `blend_mode`?, `shading_model`?, `material_domain`?, `two_sided`?, `max_texture_size`?, `opacity_from_alpha`?, `replace_existing`? | Import PBR textures from disk, create material, build graph, and compile in one call. Keys in maps: basecolor, normal, roughness, metallic, ao, height, emissive, opacity. Set opacity_from_alpha=true for decals. |

## PBR Material Workflow

### 1. Create material, set properties, then build the graph

**CRITICAL:** `build_material_graph` requires a `graph_spec` wrapper object. The spec goes INSIDE `graph_spec`, not at the top level.

```
// Step 1: Create with properties
material_query({ action: "create_material", params: {
  asset_path: "/Game/Materials/M_Rock",
  shading_model: "DefaultLit"
}})

// Step 2: Build the graph (note the graph_spec wrapper!)
material_query({ action: "build_material_graph", params: {
  asset_path: "/Game/Materials/M_Rock",
  clear_existing: true,
  graph_spec: {
    nodes: [
      { id: "TexBC", class: "TextureSample", props: { Texture: "/Game/Textures/T_Rock_D" }, pos: [-400, 0] },
      { id: "TexN", class: "TextureSample", props: { Texture: "/Game/Textures/T_Rock_N", SamplerType: "Normal" }, pos: [-400, 200] },
      { id: "TexORM", class: "TextureSample", props: { Texture: "/Game/Textures/T_Rock_ORM" }, pos: [-400, 400] }
    ],
    connections: [
      { from: "TexORM", to: "TexBC", from_pin: "R", to_pin: "AmbientOcclusion" }
    ],
    outputs: [
      { from: "TexBC", from_pin: "RGB", to_property: "BaseColor" },
      { from: "TexN", from_pin: "RGB", to_property: "Normal" },
      { from: "TexORM", from_pin: "G", to_property: "Roughness" },
      { from: "TexORM", from_pin: "B", to_property: "Metallic" }
    ],
    custom_hlsl_nodes: []
  }
}})
```

**graph_spec fields:**
- `nodes[]` — `{ id, class, props?, pos? }` — standard expression nodes
- `custom_hlsl_nodes[]` — `{ id, code, description?, output_type?, inputs?, additional_outputs?, pos? }` — Custom HLSL nodes
- `connections[]` — `{ from, to, from_pin?, to_pin? }` — inter-node wires (IDs from nodes/custom_hlsl_nodes)
- `outputs[]` — `{ from, from_pin?, to_property }` — wires to material output pins (BaseColor, Normal, Roughness, etc.)

**ID format:** Use short descriptive IDs. After creation, `id_to_name` in the response maps your IDs to UE object names.

**clear_existing: true** clears all expressions but preserves material-level properties (BlendMode, ShadingModel, etc.).

**Blend mode validation:** `build_material_graph` and `connect_expressions` warn when you wire to an output that is inactive for the current blend mode — e.g. wiring to `Opacity` on an Opaque material, or `OpacityMask` on a non-Masked material. These are warnings, not errors, but the connection will have no effect until blend mode is changed.

### 2. Validate after changes
```
material_query({ action: "validate_material", params: { asset_path: "/Game/Materials/M_Rock" } })
```

### One-Shot PBR from Disk (SIGIL Integration)

For importing pre-generated PBR maps from disk and creating a complete material in a single call:

```
material_query({ action: "create_pbr_material_from_disk", params: {
  material_path: "/Game/Materials/SIGIL/M_BloodConcrete",
  texture_folder: "/Game/Textures/SIGIL/BloodConcrete",
  maps: {
    basecolor: "D:/Projects/SIGIL/output/basecolor.png",
    normal: "D:/Projects/SIGIL/output/normal.png",
    roughness: "D:/Projects/SIGIL/output/roughness.png"
  },
  max_texture_size: 2048
}})
```

For decals (single RGBA with alpha transparency):
```
material_query({ action: "create_pbr_material_from_disk", params: {
  material_path: "/Game/Materials/SIGIL/Decals/M_BloodSplatter",
  texture_folder: "/Game/Textures/SIGIL/Decals",
  maps: { basecolor: "D:/output/blood_splatter.png" },
  blend_mode: "Translucent",
  material_domain: "DeferredDecal",
  opacity_from_alpha: true
}})
```

## Editing Existing Materials

Always inspect before modifying:
```
material_query({ action: "get_all_expressions", params: { asset_path: "/Game/Materials/M_Skin" } })
material_query({ action: "get_full_connection_graph", params: { asset_path: "/Game/Materials/M_Skin" } })
```

Wrap modifications in transactions for undo support:
```
material_query({ action: "begin_transaction", params: { asset_path: "/Game/Materials/M_Skin", description: "Add emissive" } })
// ... make changes ...
material_query({ action: "end_transaction", params: { asset_path: "/Game/Materials/M_Skin" } })
```

## Checking Shader Instruction Counts

`get_compilation_stats` returns both vertex and pixel shader instruction counts:
```
material_query({ action: "get_compilation_stats", params: { asset_path: "/Game/Materials/M_Rock" } })
// Returns: num_vertex_shader_instructions, num_pixel_shader_instructions, num_samplers, blend_mode, etc.
```

Use this after graph changes to catch runaway instruction counts before they hit the profiler.

## Particle / VFX Material Conventions

When creating materials for Niagara particle systems, follow these conventions so the Niagara agent can use them correctly:

### Material Setup
- **Shading Model:** `Unlit` — particles shouldn't receive scene lighting
- **Blend Mode:** `Additive` for fire, glow, sparks. `Translucent` for smoke, fog, dust.
- **Two Sided:** Always enabled for particles

### Required Nodes
- **Particle Color:** Always multiply final color by `Particle Color` node — this lets Niagara control color per-particle via `Particles.Color`
- **Dynamic Parameter:** Add `DynamicParameter` node if the effect needs runtime control (e.g. erosion, intensity). Niagara drives these via `Particles.DynamicMaterialParameter`

### Soft Particle Edges (No Textures)
Use procedural radial gradients instead of texture samples for fully procedural particles:
```
Custom HLSL: "float2 c = TexCoords - 0.5; return saturate(1.0 - length(c) * 2.0);"
```
- Input: `TextureCoordinate` node (UV0)
- Output feeds into opacity (smoke) or emissive intensity (fire)
- For fire: power the gradient by 2-3 for tighter cores
- For smoke: multiply opacity by 0.3-0.5 for transparency

### Depth Fade
- Add `DepthFade` (50-100 units) on translucent particles to prevent hard intersections with geometry

### Naming Convention
- Particle materials: `M_<EffectName>Particle` (e.g. `M_FireParticle`, `M_SmokeParticle`)
- Save to `/Game/VFX/Materials/` alongside the Niagara systems

## Collaborating with Niagara Agent

When building materials for VFX, the material agent runs FIRST. The Niagara agent runs AFTER and assigns materials to renderers.

**What Niagara needs from you:**
1. Materials saved and compiled at known paths
2. `Particle Color` node wired in so Niagara can drive color
3. `DynamicParameter` node if Niagara needs per-particle material control
4. Correct blend mode (Additive vs Translucent) for the effect type
5. Unlit shading so particles aren't affected by scene lighting

**What to document in your response:**
- The asset path of each material created
- What blend mode was used and why
- Whether Dynamic Parameters are available and what they control
- Any special UV or texture coordinate requirements

## Tiling Quality Checklist

Before finalizing any material that uses tiling textures, verify ALL of these:

1. **Macro variation applied?** Add world-space noise overlay on BaseColor (strength 0.1-0.3) and Roughness (multiply by 0.8-1.2 range). Use FluidNinja `T_LowResBlurredNoise_sRGB` at UV scale WorldPosition * 0.0003-0.001.
2. **UVs broken with noise offset or world-position blend?** Base UVs must not feed directly into TextureSample without transformation.
3. **Previewed at 3x tiling?** Use `render_preview` to check appearance at high repetition count. Tiling should not be obvious at 3x3.
4. **`MF_AntiTile_IqOffset` used for organic/terrain textures?** Apply Iq's 2-sample offset technique (cheapest proper anti-tiling, ~15 instructions). See `Docs/references/materials/anti-tiling.md` for HLSL and alternatives (hex tiling for large surfaces).
5. **FluidNinja noise textures used for macro variation?** Recommended: `/Game/FluidNinjaLive/Textures/T_LowResBlurredNoise_sRGB` (color), `/Game/FluidNinjaLive/Textures/T_MultilevelNoise1` (roughness).

## Rules

- **Graph editing only works on base Materials**, not MaterialInstanceConstants
- The primary asset param is `asset_path` (not `asset`)
- Always call `validate_material` after graph changes
- `build_material_graph` is the fastest way to create complex graphs — single JSON spec for all nodes + wires
- Use `export_material_graph` to snapshot a graph before making destructive changes
- Use `get_all_expressions` + `get_full_connection_graph` for inspection. Only use `export_material_graph` for round-tripping. Pass `include_properties: false` to reduce payload by ~70%
- Use `render_preview` or `get_thumbnail` with `save_to_file: true` — inline base64 wastes context window
- Blend mode warnings from `connect_expressions` / `build_material_graph` are informational — the connection is made, but the output pin is inactive unless the material's blend mode matches
- There are exactly 63 material actions — use `monolith_discover("material")` to see them all
