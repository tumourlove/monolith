# Monolith — MonolithLogicDriver Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.7 (Beta)

---

## MonolithLogicDriver

**Dependencies:** Core, CoreUObject, Engine, MonolithCore
**Namespace:** `logicdriver` | **Tool:** `logicdriver_query(action, params)` | **Actions:** 66
**Conditional:** Logic Driver Pro plugin features wrapped in `#if WITH_LOGICDRIVER`. When Logic Driver Pro is absent, the module compiles to an empty stub (0 actions registered). Uses UObject reflection only — no direct C++ API linkage against Logic Driver binaries. Build.cs detection at 3 locations (project plugins, engine marketplace, engine plugins).
**Settings toggle:** `bEnableLogicDriver` (default: True)

MonolithLogicDriver provides MCP coverage of the Logic Driver Pro marketplace plugin. It covers state machine asset CRUD, graph read/write, node configuration, runtime/PIE control, JSON spec-based generation, scaffolding templates, discovery, component management, and text-based graph visualization.

### Action Categories

| Category | Actions | Description |
|----------|---------|-------------|
| Asset CRUD | 8 | Create, list, delete, compile, duplicate, rename state machines |
| Graph Read/Write | 20 | Get structure, add/remove/connect states and transitions, get/set node properties, auto-arrange graph |
| Node Config | 8 | Configure state classes, transition rules, conduits, node colors, entry points |
| Runtime/PIE | 7 | Start/stop/step SM in PIE, get active states, set variables, inspect runtime context |
| JSON/Spec | 5 | Build SM from JSON spec, export/import SM as JSON, validate spec, diff specs |
| Scaffolding | 7 | scaffold_hello_world_sm, scaffold_horror_encounter_sm, scaffold_patrol_sm, scaffold_dialogue_sm, scaffold_health_sm, scaffold_interaction_sm, scaffold_quest_sm |
| Discovery | 6 | get_sm_overview, list_state_machines, explain_state_machine, compare_state_machines, validate_state_machine, search_state_machines |
| Component | 3 | Add SM component to actor, configure component, get component info |
| Text Graph | 2 | visualize_sm_as_text (Mermaid output), export_sm_as_dot (Graphviz DOT) |

### Key Actions

> **`build_sm_from_spec` (power action).** Creates a complete state machine from a JSON specification in a single call. The spec defines states, transitions, initial state, transition rules, and metadata. Handles EdGraph node creation, layout, and compilation automatically.
>
> **Scaffolding templates (7).** Pre-built SM patterns for common game scenarios: hello world (3-state tutorial), horror encounter (7-state with escape/lose-interest paths), patrol, dialogue, health management, interaction, and quest progression.
>
> **`visualize_sm_as_text`.** Generates Mermaid diagram syntax from an SM asset, including `[*]` initial state markers. Useful for documentation and debugging without opening the editor.
>
> **`auto_arrange_graph`.** Automatically lays out SM nodes in the editor graph for readability.

### Bulk Fill & Describe Surface (2026-05-11)

`MonolithLogicDriverBulkFillAdapter` registers under `FMonolithBulkFillRegistry` for the `logicdriver` namespace, exposed via the framework-level `bulk_fill_query("apply", ...)` and `describe_query("schema", ...)` dispatchers. Phase 5 of the MCP ergonomics rollout (design spec `Docs/plans/2026-05-11-monolith-mcp-ergonomics-design.md`).

**Surface summary.** `bulk_fill_query("apply", target_namespace="logicdriver", target="<sm_blueprint>", tree={...})` walks the state / transition surface in one transaction. `describe_query("schema", target_namespace="logicdriver", target="<sm_blueprint>")` returns the exposed-property surface per state class plus the transition predicate surface — replaces the per-property guess-and-check pain.

**fill_kind catalogue (2 — enumerated against `MonolithLogicDriverBulkFillAdapter.cpp`):**

| `fill_kind` | Target shape | Walks |
|---|---|---|
| `StatePropertiesBulk` | SM Blueprint | `states:{}` keyed by state-name → property map. Wildcard `"*"` key fans out to all states uniformly |
| `TransitionPredicatesBulk` | SM Blueprint | `transitions:{}` keyed by transition identifier → predicate property map |

**Sample tree (StatePropertiesBulk with wildcard fanout, design spec Appendix B.8):**

```json
{
  "target": "/Game/AI/SM_HorrorEncounter",
  "tree": {
    "fill_kind": "StatePropertiesBulk",
    "states": {
      "Idle":   {"PatrolSpeed": 200.0, "EnableLookAt": true},
      "Chase":  {"ChaseSpeed": 600.0, "GiveUpDistance": 4000.0},
      "*":      {"DebugDrawColor": "FLinearColor(1,0,0,1)"}
    }
  },
  "dry_run": true
}
```

**Adapter-specific quirks.**

- **`#if WITH_LOGICDRIVER` stub-adapter pattern.** The adapter ALWAYS registers under `FMonolithBulkFillRegistry` — body is conditionally compiled. When `WITH_LOGICDRIVER=0`, both fill_kinds return the clean error `"logicdriver adapter: LogicDriver not available — WITH_LOGICDRIVER=0 in this build."` and `describe_schema` returns an equivalent stub. This preserves discover-surface symmetry across configurations (matches the existing 3-location Build.cs detection pattern + release-build strip behaviour).
- **v1 wildcard `*` fanout routes through CDO walker uniformly.** The wildcard handler in `StatePropertiesBulk` enumerates every state in the SM Blueprint and applies the wildcard property map to each via the same reflection walker that handles named states. This is the v1 fanout shape — `(v1.1)` adds per-instanced-state-node fanout where each state instance can be addressed individually rather than by name.
- **`runtime_*` actions are PIE-only.** Schema descriptors flag the 7 runtime/PIE actions with `pie_blocked: true`. Bulk_fill v1 does NOT target runtime PIE state — only authoring-time SM Blueprint assets.
- **`compare_state_machines` is value-blind.** The existing `compare_state_machines` action returns structural diff but not exposed-property-value diff. Dry-run reports for `StatePropertiesBulk` partially close this gap by surfacing per-key value diff between current and proposed — but a full-fidelity value-aware `compare_state_machines` is `(WISHLIST)`.
- **`auto_arrange_graph` is BA-coupled.** Bulk_fill does NOT call layout — callers run `auto_arrange_graph` separately. Schema notes the BA-coupling under that action; bulk_fill stays orthogonal to layout.
- **Reflection-only architecture preserved.** The adapter uses `FindPropertyByName` + `GetValue_InContainer` against state CDOs and transition CDOs, matching the rest of MonolithLogicDriver's reflection-only stance. No direct linkage against Logic Driver headers.

**Limitations / v1.1 follow-ups.**

- Per-instanced-state-node fanout (vs. by-state-name) — `(v1.1)` — instanced sub-object GUID stability concerns from cross-cutting quirks.
- `TransitionPredicatesBulk` opacity for predicate types not registered in the reflection map — `(v1.1)` — `describe_exposed_property` companion needed.
- Value-aware `compare_state_machines` — `(WISHLIST)` — full-fidelity diff blocked at the existing action.
- `runtime_*` PIE-time bulk_fill — `(WISHLIST)` — runtime SM state mutation while PIE is running not in scope.

### Notes

> **Precompiled plugin integration.** Logic Driver Pro is a marketplace plugin with precompiled binaries. MonolithLogicDriver uses UObject reflection (`FindPropertyByName`, `FProperty::GetValue_InContainer`) and factory classes discovered via reflection rather than linking against Logic Driver headers. The 3-location Build.cs detection finds SMSystem/SMSystemEditor modules whether installed as a project plugin, engine marketplace plugin, or engine plugin.
>
> **Reflection-only architecture.** All property access goes through `FindPropertyByName` + `GetValue_InContainer`. State/transition classes are resolved via `FindObject<UClass>`. This makes the integration version-agnostic as long as property names and class hierarchies are stable.
>
> **EdGraph sync.** State machine assets contain both runtime and editor graph representations. All write actions update both so changes are visible in the Logic Driver editor without manual refresh.

---
