# Monolith — MonolithAI Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.1 (Beta)

---

## MonolithAI

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AIModule, GameplayTasks, NavigationSystem, Json, JsonUtilities
**Namespace:** `ai` | **Tool:** `ai_query(action, params)` | **Actions:** 229
**Conditional:** State Trees (`#if WITH_STATETREE`) and Smart Objects (`#if WITH_SMARTOBJECTS`) are required dependencies. Mass Entity (`#if WITH_MASSENTITY`) and Zone Graph (`#if WITH_ZONEGRAPH`) are optional extensions. When required deps are absent, the module compiles to an empty stub (0 actions registered).
**Settings toggle:** `bEnableAI` (default: True)

MonolithAI provides comprehensive MCP coverage of Unreal Engine's AI framework. It covers Behavior Trees, Blackboards, State Trees, Environment Query System (EQS), Smart Objects, AI Controllers, AI Perception, Navigation, runtime/PIE control, scaffolding templates, discovery, and advanced AI operations.

### Action Categories

| Category | Actions | Description |
|----------|---------|-------------|
| Behavior Trees | ~40 | BT CRUD, node management, decorator/service/task creation, composite nodes, spec-based generation |
| Blackboards | ~20 | BB CRUD, key management, key types, inheritance, inspection |
| State Trees | ~30 | ST CRUD, state/transition management, conditions, tasks, spec-based generation. Conditional on `#if WITH_STATETREE` |
| EQS | ~25 | EQS query CRUD, generator/test management, contexts, debugging |
| Smart Objects | ~20 | SO definition CRUD, slot configuration, behavior binding. Conditional on `#if WITH_SMARTOBJECTS` |
| AI Controllers | ~15 | Controller configuration, team assignment, focus management |
| Perception | ~20 | Sight/hearing/damage/team sense configuration, stimulus management |
| Navigation | ~15 | NavMesh queries, path finding, nav link management, nav modifier volumes |
| Runtime/PIE | ~15 | Runtime BT/ST inspection, active task queries, blackboard value read/write in PIE |
| Scaffolding | ~15 | Pre-built AI patterns: patrol, guard, investigate, flee, horror stalker, search area |
| Discovery | ~10 | AI asset overview, explain, compare, validate, search |
| Advanced | ~4 | Batch operations, cross-module integration |

### Key Actions

> **`build_behavior_tree_from_spec` (power action).** Creates a complete behavior tree from a JSON specification. Handles composite/decorator/service/task node creation, wiring, and compilation in a single call.
>
> **`build_state_tree_from_spec` (power action).** Creates a complete state tree from a JSON specification. Handles state/transition/condition/task creation and compilation.
>
> **Scaffolding templates.** Pre-built AI patterns for common game scenarios including patrol routes, guard behavior, investigation, flee response, and horror-specific stalker AI.

### Notes

> **24K lines of C++ across 30 files.** MonolithAI is the largest domain module by code volume.
>
> **Multi-plugin conditional compilation.** Unlike single-guard modules (GAS, ComboGraph, LogicDriver), MonolithAI uses multiple compile-time guards. State Trees and Smart Objects are required; Mass Entity and Zone Graph are optional extensions that unlock additional actions when present.

---
