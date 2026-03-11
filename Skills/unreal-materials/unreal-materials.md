---
name: unreal-materials
description: Use when creating, editing, or inspecting Unreal Engine materials via Monolith MCP. Covers PBR setup, graph building, material instances, templates, HLSL nodes, validation, and previews. Triggers on material, shader, PBR, texture, material instance, material graph.
---

# Unreal Material Workflows

You have access to **Monolith** with 14 material actions via `material_query()`.

## Discovery

```
monolith_discover({ namespace: "material" })
```

## Asset Path Conventions

All asset paths follow UE content browser format (no .uasset extension):

| Location | Path Format | Example |
|----------|------------|---------|
| Project Content/ | `/Game/Path/To/Asset` | `/Game/Materials/M_Rock` |
| Project Plugins/ | `/PluginName/Path/To/Asset` | `/CarnageFX/Materials/M_Blood` |
| Engine Plugins | `/PluginName/Path/To/Asset` | `/Niagara/DefaultAssets/SystemAssets/NS_Default` |

## Key Parameter Names

- `asset_path` — the material asset path (NOT `asset`)

## Action Reference (14 actions)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_all_expressions` | `asset_path` | List all expression nodes in a material |
| `get_expression_details` | `asset_path`, `expression` | Inspect a specific node's properties and pins |
| `get_full_connection_graph` | `asset_path` | Complete node/wire topology |
| `build_material_graph` | `asset_path`, `nodes`, `connections` | Create an entire graph from a JSON spec (fastest path) |
| `disconnect_expression` | `asset_path`, `expression` | Remove connections from a node |
| `create_custom_hlsl_node` | `asset_path`, `code`, `inputs`, `outputs` | Add a Custom HLSL expression |
| `begin_transaction` | `asset_path`, `description` | Start an undo group |
| `end_transaction` | `asset_path` | End an undo group |
| `export_material_graph` | `asset_path`, `include_properties`?, `include_positions`? | Serialize graph as JSON. Pass `include_properties: false` to reduce payload by ~70% |
| `import_material_graph` | `asset_path`, `graph` | Deserialize graph from JSON |
| `validate_material` | `asset_path` | Check for broken connections, unused nodes, errors |
| `render_preview` | `asset_path` | Trigger material compilation and preview |
| `get_thumbnail` | `asset_path`, `save_to_file`? | Get material thumbnail image. Use `save_to_file: true` to save to disk instead of inline base64 |
| `get_layer_info` | `asset_path` | Inspect material layer/blend stack |

## PBR Material Workflow

### 1. Create a material and build the full graph in one call
```
material_query({ action: "build_material_graph", params: {
  asset_path: "/Game/Materials/M_Rock",
  create_if_missing: true,
  nodes: [
    { type: "TextureSample", name: "BaseColor", params: { Texture: "/Game/Textures/T_Rock_D" } },
    { type: "TextureSample", name: "Normal", params: { Texture: "/Game/Textures/T_Rock_N", SamplerType: "Normal" } },
    { type: "TextureSample", name: "ORM", params: { Texture: "/Game/Textures/T_Rock_ORM" } }
  ],
  connections: [
    { from: "BaseColor.RGB", to: "Material.BaseColor" },
    { from: "Normal.RGB", to: "Material.Normal" },
    { from: "ORM.R", to: "Material.AmbientOcclusion" },
    { from: "ORM.G", to: "Material.Roughness" },
    { from: "ORM.B", to: "Material.Metallic" }
  ]
}})
```

### 2. Validate after changes
```
material_query({ action: "validate_material", params: { asset_path: "/Game/Materials/M_Rock" } })
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

## Rules

- **Graph editing only works on base Materials**, not MaterialInstanceConstants
- The primary asset param is `asset_path` (not `asset`)
- Always call `validate_material` after graph changes
- `build_material_graph` is the fastest way to create complex graphs — single JSON spec for all nodes + wires
- Use `export_material_graph` to snapshot a graph before making destructive changes
- Use `get_all_expressions` + `get_full_connection_graph` for inspection. Only use `export_material_graph` for round-tripping. Pass `include_properties: false` to reduce payload by ~70%
- Use `render_preview` or `get_thumbnail` with `save_to_file: true` — inline base64 wastes context window
- There are exactly 14 material actions — use `monolith_discover("material")` to see them all
