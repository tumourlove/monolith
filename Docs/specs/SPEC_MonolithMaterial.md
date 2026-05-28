# Monolith — MonolithMaterial Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.7 (Beta)

---

## MonolithMaterial

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, MaterialEditor, EditorScriptingUtilities, RenderCore, RHI, Slate, SlateCore, Json, JsonUtilities

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithMaterialModule` | Registers 63 material actions |
| `FMonolithMaterialActions` | Static handlers + helpers for loading materials and serializing expressions |

### Actions (63 — namespace: "material")

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

### Bulk Fill & Describe Surface (2026-05-11)

`MonolithMaterialBulkFillAdapter` registers under `FMonolithBulkFillRegistry` for the `material` namespace, exposed via the framework-level `bulk_fill_query("apply", ...)` and `describe_query("schema", ...)` dispatchers. Phase 5 of the MCP ergonomics rollout (design spec `Docs/plans/2026-05-11-monolith-mcp-ergonomics-design.md`).

**Surface summary.** `bulk_fill_query("apply", target_namespace="material", target="<asset_path>", tree={...})` covers two distinct fanout paths: MIC parameter sheets (the RL_LWSkin_Array_DCR class of pain — 80-120 params per MIC) and a v1 audit-only wrapper over `build_material_graph`. `describe_query("schema", target_namespace="material", target="<asset_path>")` returns the writable parameter surface — groups, ranges, defaults, sampler types per MIC parent.

**fill_kind catalogue (2 — enumerated against `MonolithMaterialBulkFillAdapter.cpp`):**

| `fill_kind` | Target shape | Walks |
|---|---|---|
| `MICParameters` | `UMaterialInstanceConstant` | Four canonical param maps written in one transaction: `scalars:{}`, `vectors:{}`, `textures:{}`, `switches:{}`. Replaces the 30-80 JSON round-trips per MIC |
| `BuildMaterialGraph` | `UMaterial` | **v1 audit-only** wrapper for `material_query("build_material_graph")`. Surfaces the silent-drop list (`VectorParameter.DefaultValue` ignored, `material_outputs` block no-ops, `clear_existing:false` sometimes clears) in the dry-run report. Writes still flow through the existing `build_material_graph` action — this fill_kind does NOT replace it |

**Sample tree (MICParameters, design spec Appendix B.5):**

```json
{
  "target": "/Game/Characters/RL_LWSkin_Array_DCR_Body",
  "tree": {
    "fill_kind": "MICParameters",
    "scalars": {"SubsurfaceRadius": 0.5, "Roughness": 0.45},
    "vectors": {"SubsurfaceColor": [0.9, 0.7, 0.6, 1.0]},
    "textures": {"BaseColorMap": "/Game/Characters/T_BodyAlbedo"},
    "switches": {"UseDetailNormal": true}
  },
  "dry_run": true
}
```

**Adapter-specific quirks.**

- **`build_material_graph` wrapper requires nested `graph_spec`.** Existing wrapper-shape gotcha carries: `material_query("build_material_graph")` requires `{ "graph_spec": { ... } }` not bare spec. The `BuildMaterialGraph` fill_kind preserves the same wrapper — `tree:{ "fill_kind":"BuildMaterialGraph", "graph_spec":{ ... } }` — and the dry-run surfaces the wrapper validator's diagnostics.
- **Silent-drop catalogue surfaced in dry-run.** `VectorParameter.DefaultValue` writes that the underlying action ignores show up as `silent_drops:[{path: "...", reason: "VectorParameter.DefaultValue ignored by build_material_graph"}]`. Same treatment for `material_outputs` block no-ops and `clear_existing:false` partial-clear cases — the audit-only fill_kind catches these without committing changes.
- **`MaterialAttributeLayers` reflection-hostile — `(WISHLIST-rejected)`.** Per design Cross-Cutting Engine Quirks, MaterialAttributeLayers cannot be walked via reflection in v1. Schema returns `unsupported: true` for any layer fields.
- **MaterialX permutation invisible — `(WISHLIST)`.** MaterialX-derived permutations of base materials are invisible to the reflection walker; schema flags the parent material with `materialx_permutations_invisible: true` if any are detected.
- **`v1 BuildMaterialGraph fill_kind is audit-only — write still through existing `material_query("build_material_graph")`.** Cited verbatim from the design spec; the adapter explicitly refuses to commit graph writes and points callers at the existing action.

**Limitations / v1.1 follow-ups.**

- CSV / JSON-array / sibling-MIC bulk-fill from template source — covered minimally by MICParameters' four-map shape in v1; CSV path stub `(WISHLIST v1.1)` per Q2.
- Graph-write fill_kind (non-audit) — `(v1.1)` blocked until `build_material_graph`'s silent-drop set is fully enumerated and the wrapper can refuse-on-drop in strict mode.
- Diff_material_graph companion — `(WISHLIST)` per design Three Framework Primitives dry-run section.

---
