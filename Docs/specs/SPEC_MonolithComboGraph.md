# Monolith — MonolithComboGraph Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.7 (Beta)

---

## MonolithComboGraph

**Dependencies:** Core, CoreUObject, Engine, MonolithCore
**Namespace:** `combograph` | **Tool:** `combograph_query(action, params)` | **Actions:** 13
**Conditional:** ComboGraph plugin features wrapped in `#if WITH_COMBOGRAPH`. When ComboGraph is absent, the module compiles to an empty stub (0 actions registered). Uses UObject reflection only — no direct C++ API linkage against ComboGraph binaries.
**Settings toggle:** `bEnableComboGraph` (default: True)

MonolithComboGraph provides MCP coverage of the ComboGraph marketplace plugin. It covers combo graph CRUD, node and edge management, gameplay effect and cue assignment per node, ability creation/linking, and full-graph scaffolding from montage lists.

### Action Categories

| Category | Actions | Description |
|----------|---------|-------------|
| Read | 4 | List combo graphs, inspect graph structure (nodes/edges/effects), read node effects, validate graph integrity |
| Create | 5 | Create combo graphs, add nodes with montages, add transition edges, set node effects, set node cues |
| Scaffold | 3 | Create combo abilities, link abilities to graphs, scaffold complete graphs from ordered montage lists |
| Layout | 1 | Auto-arrange combo graph nodes |

### Bulk Fill & Describe Surface (2026-05-11)

`MonolithComboGraphBulkFillAdapter` registers under `FMonolithBulkFillRegistry` for the `combograph` namespace, exposed via the framework-level `bulk_fill_query("apply", ...)` and `describe_query("schema", ...)` dispatchers. Phase 5 of the MCP ergonomics rollout (design spec `Docs/plans/2026-05-11-monolith-mcp-ergonomics-design.md`). Combograph's adapter was deliberately scheduled LAST in the per-namespace pilot order (Phase 5+) because of the `TargetType` non-UPROPERTY problem documented below.

**Surface summary.** `bulk_fill_query("apply", target_namespace="combograph", target="<combo_graph_asset>", tree={...})` walks effect-container arrays and edges. `describe_query("schema", target_namespace="combograph", target="<combo_graph_asset>")` returns the writable surface plus an EXPLICIT unsupported-field annotation for `TargetType` so callers see the limitation up-front.

**fill_kind catalogue (2 — enumerated against `MonolithComboGraphBulkFillAdapter.cpp`):**

| `fill_kind` | Target shape | Walks |
|---|---|---|
| `EffectContainers` | `UComboGraph` | `containers:[]` array against each node's effect-container slot — `TargetType` writes are rejected with the explicit unsupported-field error (see below) |
| `Edges` | `UComboGraph` | `edges:[]` keyed by composite `{from_id, to_id}` per row — covers the edge CRUD gap from first-fanout Appendix A |

**Sample tree (EffectContainers):**

```json
{
  "target": "/Game/Combat/CG_BasicCombo",
  "tree": {
    "fill_kind": "EffectContainers",
    "containers": [
      {"node_id": "Strike1", "duration": 0.4, "magnitude": 12.0},
      {"node_id": "Strike2", "duration": 0.5, "magnitude": 18.0},
      {"node_id": "Finisher", "duration": 0.8, "magnitude": 35.0}
    ]
  },
  "dry_run": true
}
```

**Adapter-specific quirks.**

- **`#if WITH_COMBOGRAPH` stub-adapter pattern.** The adapter ALWAYS registers under `FMonolithBulkFillRegistry`. When `WITH_COMBOGRAPH=0`, both fill_kinds return the clean error `"combograph adapter: ComboGraph not available — WITH_COMBOGRAPH=0 in this build."` and `describe_schema` returns an equivalent stub. Discover-surface symmetry preserved across configurations (matches the 3-location Build.cs probe + release-build strip).
- **`TargetType` writes return an EXPLICIT unsupported-field error — NOT a silent no-op.** Per design Cross-Cutting Engine Quirks ("TargetType field on combograph effect containers is NOT a UPROPERTY — custom serialisation required"), the adapter pre-scans every incoming JSON tree (recursively walks objects + arrays) for any `TargetType` key. If found, bulk_fill rejects with the verbatim error: `"combograph adapter: TargetType field is NOT a UPROPERTY — bulk_fill cannot write it via reflection. Tracked under WISHLIST item 'ComboGraph TargetType custom serialiser'. Use existing combograph_query actions for TargetType writes in the meantime."`. The error path is the Step-8 post-review invariant called out in the adapter source comments (`MakeTargetTypeUnsupportedReason`). `describe_schema` annotates `TargetType` with `(non-UPROPERTY — custom-serialisation only)` and inlines the same error string in `importtext_form` so callers see the limitation before attempting a write.
- **Nested JSON pre-scan walks objects + arrays recursively.** The pre-scan does NOT only check top-level keys — it descends into containers, arrays, and array-of-objects so a `TargetType` buried in `containers[2].nested.TargetType` is caught with the dotted path returned in the error.
- **EdGraph requires editor-open before `layout_combo_graph`.** ComboGraph's `EdGraph` is `WITH_EDITORONLY_DATA` and lazily materialises — `layout_combo_graph` silently no-ops unless the asset was opened in the editor at least once. The dry-run report flags this with `"asset not opened — layout will no-op"` for any tree that writes graph layout fields. Bulk_fill of `EffectContainers` / `Edges` does NOT depend on the editor-open state (the runtime graph is always materialised), but the schema descriptor surfaces the editor-open dependency for the layout-coupled fields.
- **Edges array lives on graph root, not nodes.** The `Edges` fill_kind targets `UComboGraph::Edges` directly — the composite `{from_id, to_id}` row identifier handles the edge CRUD gap from Appendix A. Per-node edge endpoints are NOT writable directly via reflection.
- **GAS integration unaffected.** The `create_combo_ability` / `link_ability_to_combo_graph` actions still require both ComboGraph + GameplayAbilities; bulk_fill does NOT replace them.

**Limitations / v1.1 follow-ups.**

- `TargetType` custom-serialisation hook — `(v1.1)` — primary WISHLIST item for combograph, blocked on engine-level changes or a custom serialiser path.
- `scaffold_combo_from_montages` dry-run preview — `(WISHLIST v1.1)` — preview-before-commit on the existing scaffold action.
- Full edge CRUD via reflection-walker — covered minimally by the `Edges` fill_kind in v1; richer edge metadata `(WISHLIST v1.1)`.

### Notes

> **Precompiled plugin integration.** ComboGraph is a marketplace plugin with precompiled binaries. MonolithComboGraph uses UObject reflection (`FindPropertyByName`, `FProperty::GetValue_InContainer`) and `UComboGraphFactory` (discovered via reflection) rather than linking against ComboGraph headers. This makes the integration version-agnostic as long as property names are stable.
>
> **EdGraph sync.** ComboGraph assets contain both runtime and editor graphs. All write actions update both representations so changes are visible in the ComboGraph editor without manual refresh.
>
> **GAS integration.** The `create_combo_ability` and `link_ability_to_combo_graph` actions require both ComboGraph and GameplayAbilities plugins to be present.

---
