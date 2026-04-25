# Monolith — MonolithMaterial Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.1 (Beta)

---

## MonolithMaterial

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, MaterialEditor, EditorScriptingUtilities, RenderCore, RHI, Slate, SlateCore, Json, JsonUtilities

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithMaterialModule` | Registers 63 material actions |
| `FMonolithMaterialActions` | Static handlers + helpers for loading materials and serializing expressions |

### Actions (57 — namespace: "material")

**Read Actions (10)**
| Action | Description |
|--------|-------------|
| `get_all_expressions` | Get all expression nodes in a base material |
| `get_expression_details` | Full property reflection, inputs, outputs for a single expression |
| `get_full_connection_graph` | Complete connection graph (all wires) of a material |
| `export_material_graph` | Export complete graph to JSON (round-trippable with build_material_graph) |
| `validate_material` | BFS reachability check — detects islands, broken textures, missing functions, duplicate params, unused params, high expression count (>200). Optional auto-fix |
| `render_preview` | Save preview PNG to Saved/Monolith/previews/ |
| `get_thumbnail` | Return thumbnail as base64 PNG or save to file |
| `get_layer_info` | Material Layer / Material Layer Blend info |
| `get_material_parameters` | List all parameter types (scalar, vector, texture, static switch) with values. Works on UMaterial and UMaterialInstanceConstant |
| `get_compilation_stats` | Sampler count, texture estimates, UV scalars, blend mode, expression count, vertex/pixel shader instruction counts (`num_vertex_shader_instructions`, `num_pixel_shader_instructions` via `UMaterialEditingLibrary::GetStatistics`) |

**Write Actions (15)**
| Action | Description |
|--------|-------------|
| `create_material` | Create new UMaterial at path with configurable defaults (blend mode, shading model, material domain) |
| `create_material_instance` | Create UMaterialInstanceConstant from parent material with optional parameter overrides |
| `set_material_property` | Set material properties (blend_mode, shading_model, two_sided, etc.) via UMaterialEditingLibrary |
| `build_material_graph` | Build entire graph from JSON spec in single undo transaction (4 phases: standard nodes, Custom HLSL, wires, output properties). The spec must be passed as `{ "graph_spec": { "nodes": [...], "connections": [...], ... } }` — not as a bare object |
| `disconnect_expression` | Disconnect inputs or outputs on a named expression (supports expr→expr and expr→material property; supports targeted single-connection disconnection via optional `input_name`/`output_name` params) |
| `delete_expression` | Delete expression node by name from material graph |
| `create_custom_hlsl_node` | Create Custom HLSL expression with inputs, outputs, and code |
| `set_expression_property` | Set properties on expression nodes (e.g., DefaultValue on scalar param). Calls `PostEditChangeProperty` with the actual `FProperty*` so `MaterialGraph->RebuildGraph()` fires and the editor display updates correctly |
| `connect_expressions` | Wire expression outputs to expression inputs or material property inputs. Returns blend mode validation warnings (e.g. Opacity on Opaque/Masked, OpacityMask on non-Masked) |
| `set_instance_parameter` | Set scalar/vector/texture/static switch parameters on material instances |
| `duplicate_material` | Duplicate material asset to new path |
| `recompile_material` | Force material recompile |
| `import_material_graph` | Import graph from JSON. Mode: "overwrite" (clear+rebuild) or "merge" (offset +500 X) |
| `begin_transaction` | Begin named undo transaction for batching edits |
| `end_transaction` | End current undo transaction |

**Material Function Actions (9)**
| Action | Description |
|--------|-------------|
| `export_function_graph` | Full graph export of a material function — nodes, connections, properties, inputs, outputs, static switch details |
| `set_function_metadata` | Update material function description, categories, and library exposure settings |
| `delete_function_expression` | Remove expression(s) from a material function graph |
| `update_material_function` | Recompile a material function and cascade changes to all referencing materials/instances |
| `create_function_instance` | Create a MaterialFunctionInstance with parent reference and optional parameter overrides |
| `set_function_instance_parameter` | Set parameter overrides on a MaterialFunctionInstance (supports 11 parameter types) |
| `get_function_instance_info` | Read MFI parent chain and all parameter overrides (11 types: scalar, vector, texture, font, static switch, static component mask, and more) |
| `layout_function_expressions` | Auto-arrange material function graph layout |
| `rename_function_parameter_group` | Rename a parameter group across all parameters in a material function |
| `auto_layout` | Auto-arrange expression nodes in a material graph. `formatter`: `"auto"` (default) — uses Blueprint Assist if available, falls back to built-in layout; `"blueprint_assist"` — requires BA; `"builtin"` — built-in only |

**Extended Actions (1)**
| Action | Change |
|--------|--------|
| `create_material_function` | Added `type` parameter — supports `MaterialLayer` and `MaterialLayerBlend` in addition to standard material functions |

---
