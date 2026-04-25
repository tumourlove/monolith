# Monolith — MonolithLogicDriver Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.1 (Beta)

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

### Notes

> **Precompiled plugin integration.** Logic Driver Pro is a marketplace plugin with precompiled binaries. MonolithLogicDriver uses UObject reflection (`FindPropertyByName`, `FProperty::GetValue_InContainer`) and factory classes discovered via reflection rather than linking against Logic Driver headers. The 3-location Build.cs detection finds SMSystem/SMSystemEditor modules whether installed as a project plugin, engine marketplace plugin, or engine plugin.
>
> **Reflection-only architecture.** All property access goes through `FindPropertyByName` + `GetValue_InContainer`. State/transition classes are resolved via `FindObject<UClass>`. This makes the integration version-agnostic as long as property names and class hierarchies are stable.
>
> **EdGraph sync.** State machine assets contain both runtime and editor graph representations. All write actions update both so changes are visible in the Logic Driver editor without manual refresh.

---
