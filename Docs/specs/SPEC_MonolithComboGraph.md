# Monolith — MonolithComboGraph Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.1 (Beta)

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

### Notes

> **Precompiled plugin integration.** ComboGraph is a marketplace plugin with precompiled binaries. MonolithComboGraph uses UObject reflection (`FindPropertyByName`, `FProperty::GetValue_InContainer`) and `UComboGraphFactory` (discovered via reflection) rather than linking against ComboGraph headers. This makes the integration version-agnostic as long as property names are stable.
>
> **EdGraph sync.** ComboGraph assets contain both runtime and editor graphs. All write actions update both representations so changes are visible in the ComboGraph editor without manual refresh.
>
> **GAS integration.** The `create_combo_ability` and `link_ability_to_combo_graph` actions require both ComboGraph and GameplayAbilities plugins to be present.

---
