# Monolith — MonolithNiagara Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.21.3 (Beta)

---

## MonolithNiagara

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, Niagara, NiagaraCore, NiagaraEditor, Json, JsonUtilities, AssetTools, Slate, SlateCore

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithNiagaraModule` | Registers 129 Niagara actions (108 baseline in `MonolithNiagaraActions.cpp` + 1 layout in `MonolithNiagaraLayoutActions.cpp` + 9 timing actions in `MonolithNiagaraTimingActions.cpp` + 1 stateless-emitter factory in `MonolithNiagaraActions.cpp` + 7 issue #64 Tranche 2 search/discovery + 2 PR #65 CustomHlsl-text read/write). The 2 CustomHlsl-text actions ride public UPROPERTY reflection and are always registered regardless of `WITH_NIAGARA_WIZARD_PRIVATE` |
| `FMonolithNiagaraActions` | Static handlers + extensive private helpers |
| `FMonolithNiagaraLayoutActions` | `auto_layout` Blueprint Assist bridge for Niagara graphs |
| `MonolithNiagaraHelpers` | 6 reimplemented NiagaraEditor functions (non-exported APIs) |

### Reimplemented NiagaraEditor Helpers

These exist because Epic's `FNiagaraStackGraphUtilities` functions lack `NIAGARAEDITOR_API`:

1. `GetOrderedModuleNodes` — Module execution order
2. `GetStackFunctionInputOverridePin` — Override pin lookup
3. `GetModuleIsEnabled` — Module enabled state
4. `RemoveModuleFromStack` — Module removal
5. `GetParametersForContext` — System user store params
6. `GetStackFunctionInputs` — Full input enumeration via engine's `FNiagaraStackGraphUtilities::GetStackFunctionInputs` with `FCompileConstantResolver`. Returns all input types (floats, vectors, colors, data interfaces, enums, bools) — not just static switch pins

### Actions (129 — namespace: "niagara")

> **Audit note (2026-04-26):** detailed per-category tables below sum to roughly 96 — the remainder are post-design-doc additions (NPC, effect types, scalability, layout, advanced query helpers) that have not yet been threaded into the per-category tables. The header count is the source-of-truth.

> **Note:** All Niagara actions accept `asset_path` (preferred) or `system_path` (backward compatible) for the system asset path parameter.
>
> **Input name conventions:** `get_module_inputs` returns short names (no `Module.` prefix). All write actions that accept input names (`set_module_input_value`, `set_module_input_binding`, `set_module_input_di`, `set_curve_value`) accept both short names and `Module.`-prefixed names. For CustomHlsl modules, `get_module_inputs` and `set_module_input_value` fall back to reading/writing the FunctionCall node's typed input pins directly (CustomHlsl inputs don't appear in the ParameterMap history).
>
> **Param name aliases:** The canonical param names registered in schemas are `module_node` and `input`. All module write actions also accept these aliases: `module_node` → `module_name`, `module`; `input` → `input_name`. Use the canonical names when possible — aliases exist for backward compatibility.
>
> **Emitter name matching:** `FindEmitterHandleIndex` does NOT auto-select a single emitter when a specific non-matching name is passed. If a name is provided it must match exactly (case-insensitive). Numeric index strings (`"0"`, `"1"`, etc.) are also accepted as a fallback.
>
> **Selector-based stage targeting (PR #65):** `get_ordered_modules`, `add_module`, `move_module`, and `duplicate_module` accept two selector-driven `usage` values for shared-graph stages in addition to the standard stages: `usage: "particle_simulation_stage"` (resolved by `usage_id` / `stage_name` / `stage_index`) and `usage: "particle_event"` (resolved by `usage_id` / `handler_index`). This lets callers target the simulation-stage and event scripts that don't appear under the canonical Emitter/Particle Spawn/Update stages.
>
> **Event handler note (PR #65):** `add_event_handler` only creates the event handler and its `ParticleEventScript` container. It does **not** auto-add `ReceiveDeathEvent` / `ReceiveLocationEvent` modules, and it now **rejects** an inter-emitter handler whose `source_emitter` cannot be resolved (instead of creating an empty `SourceEmitterID`). It returns `handler_index` + `usage_id` + `usage`. To consume source payloads such as `Position`, `Velocity`, or `Color`, target the handler with `usage: "particle_event"` plus `usage_id` or `handler_index`, add the matching `Receive<Event>` module, then set the required payload switches to `Apply`. For fireworks and other death-triggered bursts, `Position` usually must be `Apply`.

**System (14)**
| Action | Description |
|--------|-------------|
| `add_emitter` | Add an emitter (UE 5.7: takes FGuid VersionGuid) |
| `remove_emitter` | Remove an emitter |
| `duplicate_emitter` | Duplicate an emitter within a system. Accepts `emitter` as alias for `source_emitter` |
| `set_emitter_enabled` | Enable/disable an emitter |
| `reorder_emitters` | Reorder emitters (direct handle assignment + PostEditChange + MarkPackageDirty for proper change notifications) |
| `set_emitter_property` | Set property: SimTarget, bLocalSpace, bDeterminism, RandomSeed, AllocationMode, PreAllocationCount, bRequiresPersistentIDs, MaxGPUParticlesSpawnPerFrame, CalculateBoundsMode |
| `set_system_property` | Set a system-level property (WarmupTime, bDeterminism, etc.) |
| `request_compile` | Request system compilation. Params: `force` (bool), synchronous (bool) |
| `create_system` | Create new system (blank or from template via DuplicateAsset) |
| `list_emitters` | List all emitters with name, index, enabled, sim_target, renderer_count, GUID |
| `list_renderers` | List all renderers across emitters with class (short `type` name), index, enabled, material |
| `list_module_scripts` | Search available Niagara module scripts by keyword. Returns matching script asset paths |
| `list_renderer_properties` | List editable properties on a renderer. Params: `asset_path`, `emitter`, `renderer` |
| `get_system_diagnostics` | Compile errors, warnings, renderer/SimTarget incompatibility, GPU+dynamic bounds warnings, per-script stats (op count, registers, compile status). Added 2026-03-13 |

**Module (15)**
| Action | Description |
|--------|-------------|
| `get_ordered_modules` | Get ordered modules in a script stage. Supports standard stages plus `usage: "particle_event"` with `usage_id`/`handler_index` and `usage: "particle_simulation_stage"` with `usage_id`/`stage_name`/`stage_index` (PR #65) |
| `get_module_inputs` | Get all inputs (floats, vectors, colors, data interfaces, enums, bools) with override values, linked params, and actual DI curve data. Uses engine's `FNiagaraStackGraphUtilities::GetStackFunctionInputs`. Returns short names (no `Module.` prefix). LinearColor/vector defaults deserialized from JSON string if needed |
| `get_module_graph` | Node graph of a module script |
| `add_module` | Add module to script stage (uses FNiagaraStackGraphUtilities). Supports `usage: "particle_event"` with `usage_id`/`handler_index` and `usage: "particle_simulation_stage"` with `usage_id`/`stage_name`/`stage_index` (PR #65) |
| `remove_module` | Remove module from stack |
| `move_module` | Move module to new index (remove+re-add — **loses input overrides**). Accepts the `particle_event` / `particle_simulation_stage` selector forms (PR #65) |
| `set_module_enabled` | Enable/disable a module |
| `set_module_input_value` | Set input value (float, int, bool, vec2/3/4, color, string) |
| `set_module_input_binding` | Bind input to a parameter |
| `set_module_input_di` | Set data interface on input. Required: `di_class` (class name — `U` prefix optional, e.g. `NiagaraDataInterfaceCurve` or `UNiagaraDataInterfaceCurve`), optional `config` object (supports FRichCurve keys for curve DIs). Validates input exists and is DataInterface type. Accepts both short names and `Module.`-prefixed names |
| `set_static_switch_value` | Set a static switch value on a module |
| `create_module_from_hlsl` | Create a Niagara module script from custom HLSL. Params: `name`, `save_path`, `hlsl` (body), optional `inputs[]`/`outputs[]` (`{name, type}` objects), `description`. Generates a **ParameterMap bridge graph** (InputMap → ParameterMapGet → CustomHlsl → ParameterMapSet → OutputNode) and **preserves Data-Interface input types** (NeighborGrid3D / Grid3D / ParticleRead) rather than collapsing them (PR #65). HLSL input/output types are **strictly validated** — an unknown type now hard-fails instead of silently degrading to `float`. **HLSL body rules:** (1) Use bare input/output names (no `Module.` prefix — compiler adds `In_`/`Out_` automatically). (2) **GPU ONLY**: can write particle attributes via `Particles.X` ParameterMap tokens (e.g. `Particles.Velocity`, `Particles.Position`) — MUST wrap in `#if GPU_SIMULATION ... #endif`. CPU simulation does NOT support `Particles.*` syntax; use output parameters instead. (3) CAN access Data Interface functions if a DI is passed as input (e.g. a Grid3D input enables `GetPreviousValueAtIndex`, `SamplePreviousGridVector3Value`). (4) No swizzle via dot on map variables. |
| `create_function_from_hlsl` | Create a Niagara function script from custom HLSL. Same params and validation as `create_module_from_hlsl`. Script usage is set to `Function` instead of `Module`. |
| `get_custom_hlsl_text` | Read the HLSL source from a `CustomHlsl` node in a script via public UPROPERTY reflection. Params: `script_path` (required), optional `node_guid` to disambiguate multi-`CustomHlsl`-node scripts. Always available — does not depend on `WITH_NIAGARA_WIZARD_PRIVATE` (PR #65) |
| `set_custom_hlsl_text` | Overwrite a `CustomHlsl` node's HLSL source under `Modify()` + transaction with a recompile. Params: `script_path` (required), `hlsl` (required), optional `node_guid`. Always available — does not depend on `WITH_NIAGARA_WIZARD_PRIVATE` (PR #65) |

**Parameter (9)**
| Action | Description |
|--------|-------------|
| `get_all_parameters` | All parameters (user + per-emitter rapid iteration) |
| `get_user_parameters` | User-exposed parameters only |
| `get_parameter_value` | Get a parameter value |
| `get_parameter_type` | Type info (size, is_float, is_DI, is_enum, struct) |
| `trace_parameter_binding` | Find all usage sites of a parameter |
| `add_user_parameter` | Add user parameter with optional default |
| `remove_user_parameter` | Remove a user parameter |
| `set_parameter_default` | Set parameter default value |
| `set_curve_value` | Set curve keys on a module input. Params: `emitter`, `module_node`, `input`, `keys` (array of `{time, value}` objects) |

**Renderer (6)**
| Action | Description |
|--------|-------------|
| `add_renderer` | Add renderer (Sprite, Mesh, Ribbon, Light, Component) |
| `remove_renderer` | Remove a renderer |
| `set_renderer_material` | Set renderer material (per-type handling) |
| `set_renderer_property` | Set property via reflection (float, double, int, bool, string, enum, byte, object) |
| `get_renderer_bindings` | Get attribute bindings via reflection |
| `set_renderer_binding` | Set attribute binding (ImportText with fallback format) |

**Batch (2)**
| Action | Description |
|--------|-------------|
| `batch_execute` | Execute multiple operations in one undo transaction (23 sub-op types — all write ops including: remove_user_parameter, set_parameter_default, set_module_input_di, set_curve_value, reorder_emitters, duplicate_emitter, set_renderer_binding, request_compile) |
| `create_system_from_spec` | Full declarative system builder from JSON spec. Uses `UNiagaraSystemFactoryNew::InitializeSystem` for proper system creation |

**Data Interface (1)**
| Action | Description |
|--------|-------------|
| `get_di_functions` | Get data interface function signatures |

**HLSL (1)**
| Action | Description |
|--------|-------------|
| `get_compiled_gpu_hlsl` | Get compiled GPU HLSL for an emitter |

**Dynamic Inputs (5)**
| Action | Description |
|--------|-------------|
| `list_dynamic_inputs` | List all dynamic inputs on a module |
| `get_dynamic_input_tree` | Get the full tree structure of a dynamic input |
| `remove_dynamic_input` | Remove a dynamic input from a module |
| `get_dynamic_input_value` | Get the current value of a dynamic input |
| `get_dynamic_input_inputs` | Get all sub-inputs of a dynamic input |

**Emitter Management (3)**
| Action | Description |
|--------|-------------|
| `rename_emitter` | Rename an emitter within a system |
| `get_emitter_property` | Get a property value from an emitter via reflection |
| `list_available_renderers` | List all available renderer classes that can be added |

**Renderer Configuration (3)**
| Action | Description |
|--------|-------------|
| `set_renderer_mesh` | Set the mesh asset on a mesh renderer |
| `configure_ribbon` | Configure ribbon renderer settings (width, facing, tessellation, etc.) |
| `configure_subuv` | Configure SubUV animation settings on a renderer |

**Event Handlers (3)**
| Action | Description |
|--------|-------------|
| `add_event_handler` | Add an event handler. `source_emitter` must resolve for inter-emitter links — **unresolved handlers are now rejected** (PR #65). Returns `handler_index` + `usage_id` + `usage` for the new `ParticleEventScript`. Does NOT auto-add `Receive<Event>` modules |
| `get_event_handlers` | Get all event handlers on an emitter |
| `set_event_handler_property` | Set a property on an event handler |
| `remove_event_handler` | Remove an event handler from an emitter |

**Simulation Stages (3)**
| Action | Description |
|--------|-------------|
| `add_simulation_stage` | Add a generic simulation stage to an emitter. Now **materializes the matching `particle_simulation_stage` output node** in the emitter graph (PR #65) and returns `usage_id`, `stage_id`, and `graph_outputs` so callers can immediately target the stage via the `usage: "particle_simulation_stage"` selector form on the module-stack actions |
| `get_simulation_stages` | Get all simulation stages on an emitter |
| `set_simulation_stage_property` | Set a property on a simulation stage |
| `remove_simulation_stage` | Remove a simulation stage from an emitter |

**Temporal Control (9 — added 2026-05-28, Phases 1-4 of `plans/2026-05-28-niagara-timing-actions.md`)**

The temporal-control surface collapses the existing scattered timing edits (per-property `set_system_property` + `set_simulation_stage_property` + `set_module_input_value` against `EmitterState` / `InitializeParticle`) into composite, intent-named writers. Design rationale: `plans/2026-05-28-niagara-timing-actions-design.md`. Plan: `plans/2026-05-28-niagara-timing-actions.md`.

System-level reads + writes target `UNiagaraSystem` UPROPERTYs directly. Emitter-loop writes dispatch internally to `set_static_switch_value` (Loop Behavior, UseLoopDelay) and `set_module_input_value` (Loop Duration / Delay / Count) against the emitter's `EmitterState` module. For standalone `UNiagaraStatelessEmitter` assets (Lightweight Emitters — see § Stateless Emitters below), `set_emitter_loop_profile` and `get_emitter_timing_summary` instead dispatch into a reflection-based read/write against the protected `EmitterState` UPROPERTY (`FNiagaraEmitterStateData`) and tag responses with `stateless: true`. Sim-stage aliases reuse the `stage_index` / `stage_name` selector convention from PR #65's `set_simulation_stage_property`. `set_particle_lifetime` resolves to Direct mode (min only) or Random mode (min + max) on the `InitializeParticle` module.

| Action | Description |
|--------|-------------|
| `get_system_timing` | Bundled read of `WarmupTime`, `WarmupTickCount`, `WarmupTickDelta`, `bFixedTickDelta`, `FixedTickDeltaTime`, `bRequireCurrentFrameData`. One call replaces six `get_system_property` round-trips |
| `set_warmup_profile` | Composite write of `warmup_time` + `warmup_tick_delta`. Calls `UNiagaraSystem::SetWarmupTime` + `SetWarmupTickDelta` (the exposed setters internally call `ResolveWarmupTickCount`). Response returns the engine-resolved `(WarmupTime, WarmupTickCount, WarmupTickDelta)` triple so callers observe the snap |
| `set_fixed_tick_delta` | Toggle `bFixedTickDelta` with optional `fixed_delta_time` value (sets `FixedTickDeltaTime` when supplied) |
| `set_require_current_frame_data` | Toggle `bRequireCurrentFrameData` |
| `set_emitter_loop_profile` | Composite write of EmitterState loop topology: `loop_behavior` (Once / Infinite / Multiple), `loop_duration`, `loop_delay`, `loop_count`, `loop_delay_enabled`, plus optional `loop_duration_mode` (`"Fixed"` / `"Infinite"`, maps to `ENiagaraLoopDurationMode` — meaningful only on stateless emitters; stateful path warns if supplied). **Stateful systems:** internally dispatches `set_static_switch_value` (Loop Behavior, UseLoopDelay) + `set_module_input_value` (Loop Duration, Loop Delay, Loop Count) against the emitter's `EmitterState` module. **Standalone `UNiagaraStatelessEmitter` assets:** detected via `StaticLoadObject` + class-name match, dispatches into `WriteStatelessLoopProfile` (reflection-based UPROPERTY write on the protected `EmitterState` `FNiagaraEmitterStateData`). Response includes `stateless: true` flag on the stateless branch |
| `get_emitter_timing_summary` | Read aggregator: loop topology + `sim_stages[]` (name, `NumIterations`, `ExecuteBehavior`) + `InitializeParticle` lifetime fields, in one call. Optional `emitter` filter; omit for all emitters. **Standalone `UNiagaraStatelessEmitter` assets:** dispatches into the stateless reader — response includes `stateless: true`, all 4 `InitializeParticle` lifetime fields are `null`, and `sim_stages: []` (stateless emitters have no sim stages by design) |
| `set_sim_stage_iteration_count` | Alias over `set_simulation_stage_property` with `property=NumIterations`. Reuses `stage_index` / `stage_name` selectors. Internally formats the int as the FNiagaraParameterBindingWithValue struct-literal `(Value=N)` |
| `set_sim_stage_execute_behavior` | Alias for `set_simulation_stage_property` with `property=ExecuteBehavior`. Accepts `Always` / `OnSimulationReset` / `NotOnSimulationReset` |
| `set_particle_lifetime` | Convenience write to the `InitializeParticle` module. `min` only → Direct mode with constant `Lifetime`. `min` + `max` → Random mode with `Lifetime Min` + `Lifetime Max` |

**Stateless Emitters (1 — added 2026-05-28, Phases 0-3 of `plans/2026-05-28-niagara-stateless-timer.md`)**

`UNiagaraStatelessEmitter` (Lightweight Emitter) is a standalone emitter storage class distinct from the system-owned `UNiagaraEmitter` graph. The class lives behind `Engine/Plugins/FX/Niagara/Source/Niagara/Internal/Stateless/NiagaraStatelessEmitter.h` and is intentionally not exposed to dependent modules. To avoid a hard build-time coupling, asset creation uses `FindObject<UClass>(nullptr, "/Script/Niagara.NiagaraStatelessEmitter")` + a type-erased `NewObject` factory call. The existing `set_emitter_loop_profile` and `get_emitter_timing_summary` actions detect standalone stateless assets via `StaticLoadObject` + class-name match and dispatch into reflection-based read/write paths against the protected `EmitterState` (`FNiagaraEmitterStateData`) UPROPERTY — see the Temporal Control section above.

| Action | Description |
|--------|-------------|
| `create_stateless_emitter` | Creates a standalone `UNiagaraStatelessEmitter` (Lightweight Emitter) asset at `save_path`. Returns the asset path on success. Pairs with the stateless-aware branches of `set_emitter_loop_profile` (with optional `loop_duration_mode`) and `get_emitter_timing_summary` for programmatic Lightweight Emitter authoring without going through the Niagara System wrapper |

**Module Outputs (1)**
| Action | Description |
|--------|-------------|
| `get_module_output_parameters` | Get output parameters exposed by a module |

**Niagara Parameter Collections (NPC) (5)**
| Action | Description |
|--------|-------------|
| `create_npc` | Create a Niagara Parameter Collection asset |
| `get_npc` | Get NPC contents (parameters, defaults, namespace) |
| `add_npc_parameter` | Add a parameter to an NPC |
| `remove_npc_parameter` | Remove a parameter from an NPC |
| `set_npc_default` | Set the default value of an NPC parameter |

**Effect Types (3)**
| Action | Description |
|--------|-------------|
| `create_effect_type` | Create a Niagara Effect Type asset |
| `get_effect_type` | Get effect type settings (scalability, significance, budget) |
| `set_effect_type_property` | Set a property on an effect type |

**Utilities (5)**
| Action | Description |
|--------|-------------|
| `get_available_parameters` | List available parameters that can be bound to inputs |
| `preview_system` | Capture a preview image of a Niagara system |
| `diff_systems` | Compare two Niagara systems and return structural differences |
| `save_emitter_as_template` | Save an emitter as a reusable template asset |
| `clone_module_overrides` | Clone input overrides from one module to another |
| `auto_layout` | Auto-arrange nodes in a Niagara module script graph. `formatter`: `"auto"` (default) — uses Blueprint Assist if available, falls back to built-in layout; `"blueprint_assist"` — requires BA; `"builtin"` — built-in only |

### Bulk Fill & Describe Surface (2026-05-11)

`MonolithNiagaraBulkFillAdapter` registers under `FMonolithBulkFillRegistry` for the `niagara` namespace, exposed via the framework-level `bulk_fill_query("apply", ...)` and `describe_query("schema", ...)` dispatchers. Phase 5 of the MCP ergonomics rollout (design spec `Docs/plans/2026-05-11-monolith-mcp-ergonomics-design.md`).

**Surface summary.** `bulk_fill_query("apply", target_namespace="niagara", target="<system_asset_path>", tree={...}, dry_run=<bool>, strict=<bool>)` walks the JSON tree and commits atomically. `describe_query("schema", target_namespace="niagara", target="<system_asset_path>")` surfaces the writable parameter / curve / DI surface for the target system.

**fill_kind catalogue (3 — enumerated against `MonolithNiagaraBulkFillAdapter.cpp`):**

| `fill_kind` | Target shape | Walks |
|---|---|---|
| `DataInterfaceArray` | `UNiagaraSystem` | `rows:[]` payload written into a `User.*` Array Data Interface (e.g. Curl Noise Force, Skel Mesh Sample Pos). Replaces the 15-30 nested-field round-trip per DI |
| `Curve` | `UNiagaraSystem` | `keys:[]` (FRichCurveKey shape `{time, value, interp_mode, tangent}`) written into a Curve UPROPERTY on a module input. Replaces the 24-48 per-key atomic writes per curve |
| `ParameterOverrides` | `UNiagaraSystem` | Generic UPROPERTY tree walked against system / per-emitter rapid-iteration parameters. Covers RibbonRenderer's 40+ props in one transaction |

**Sample tree (Curve fill, design spec Appendix B.4):**

```json
{
  "target": "/Game/VFX/NS_Smoke",
  "tree": {
    "fill_kind": "Curve",
    "emitter": "Smoke",
    "module_node": "Initialize Particle",
    "input": "LifetimeCurve",
    "curve_name": "LifetimeCurve",
    "keys": [
      {"time": 0.0, "value": 1.0, "interp_mode": "Cubic"},
      {"time": 1.0, "value": 0.0, "interp_mode": "Cubic"}
    ]
  },
  "dry_run": true
}
```

**Adapter-specific quirks.**

- **GPU emitter introspection is one-way.** GPU-side runtime state is not readable. The adapter detects GPU emitters via a v1 **name-heuristic only** scan of `SimTarget` literal strings; reliable `SimTarget`-walk parity is `(v1.1)`. `describe_schema` flags GPU-mode emitters with `gpu_one_way: true` and refuses to surface read-back parameters.
- **`module_node` GUIDs change on duplicate.** When the target system was duplicated from another, module GUIDs are regenerated. The adapter falls back to module-name lookup if a GUID resolves to nothing; dry-run report flags the fallback so callers can re-bind to the new GUID. Schema surfaces `module_node` as `id_form: "guid_or_name"`.
- **Parent-emitter override vs inherit is invisible.** `get_module_inputs` doesn't flag override-vs-inherit. The adapter's `ParameterOverrides` handler walks both layers — the dry-run `field_writes` entries annotate each with `override_layer: "system" | "emitter" | "inherited"`.
- **`build_material_graph` fill_kind is v1 audit-only.** Cross-namespace materials authoring stays at `material_query("build_material_graph")` (Niagara emitters can reference materials but the BuildMaterialGraph fill_kind lives in the material adapter, not here).
- **CustomHlsl modules.** Inputs not in the ParameterMap history are routed to the FunctionCall node's typed input pins (matches the existing `get_module_inputs` / `set_module_input_value` fallback). Schema surfaces this via `read_path: "function_call_typed_pin"`.

**Limitations / v1.1 follow-ups.**

- Curl-Noise Force / Skel Mesh Sample DI 15-30 nested-field bulk fill — covered by `DataInterfaceArray` only for array DIs in v1; scalar-field DIs `(WISHLIST v1.1)`.
- GPU-sim `SimTarget` walker — `(v1.1)` to replace the v1 name heuristic.
- PIE-time parameter snooping for `User.*` — `(WISHLIST)` per cross-cutting PIE-bound quirks; cannot be unblocked without engine-level changes.
- CSV ingest for FRichCurve keys — `(WISHLIST v1.1)` per Q2.

### Blueprint-Callable Surface (issue #64)

`UMonolithNiagaraQueryLibrary` is a `UBlueprintFunctionLibrary` in the editor-only MonolithNiagara module (Type=Editor) that exposes read-only Niagara dispatcher actions as Blueprint-callable nodes for Blueprint utilities and Editor Utility Widgets. Because the module is editor-only, this surface carries **zero cost in packaged/runtime builds**. Tranches 1 + 2 of 2 SHIPPED — the BFL now exposes all **24 nodes** (17 Tranche 1 + 7 Tranche 2).

**Architecture.** Each node is a thin forwarder. A private static helper `ExecuteNiagaraActionAsJson(Action, Params, bool& bOutSuccess, FString& OutError)` calls `FMonolithToolRegistry::Get().ExecuteAction("niagara", Action, Params)`, guards against an empty registry, an unknown action, and a null result (returning `bSuccess=false` plus a descriptive `OutError` — never crashes), then serializes the result to a JSON `FString`. Every node returns an `FString` JSON payload plus `bool& bSuccess` and `FString& OutError` out-params.

**Categories:** `Monolith|Niagara|Inspection` and `Monolith|Niagara|Search`.

**Tranche 1 nodes (17, shipped):**

| Node | Backing `niagara` action |
|------|--------------------------|
| `GetNiagaraSystemInfo` | `get_system_summary` |
| `GetNiagaraEmitters` | `list_emitters` |
| `GetNiagaraEmitterSummary` | `get_emitter_summary` |
| `GetNiagaraModules` | `get_ordered_modules` |
| `GetNiagaraModuleInputs` | `get_module_inputs` |
| `GetNiagaraModuleGraph` | `get_module_graph` |
| `GetNiagaraParameters` | `get_all_parameters` |
| `GetNiagaraUserParameters` | `get_user_parameters` |
| `GetNiagaraRenderers` | `list_renderers` (+ `get_renderer_bindings` when `bIncludeBindings`) |
| `GetNiagaraEvents` | `get_event_handlers` |
| `GetNiagaraDIFunctions` | `get_di_functions` |
| `GetNiagaraSimulationStages` | `get_simulation_stages` |
| `GetNiagaraStats` | `get_system_diagnostics` |
| `GetNiagaraInventory` | `list_systems` |
| `GetNiagaraHLSLOutput` | `get_compiled_gpu_hlsl` |
| `SearchNiagaraSystems` | `list_systems` |
| `SearchNiagaraModules` | `list_module_scripts` |

**No action-count change for Tranche 1** — its 17 nodes are pure wrappers over existing actions.

**Tranche 2 nodes (7, shipped).** Seven new read-only `niagara` dispatcher actions plus their wrapper nodes. **Unlike Tranche 1, these add +7 to the `niagara` action count** (six new search/discovery actions + one per-system DI enumeration).

| Node | Backing `niagara` action | Description |
|------|--------------------------|-------------|
| `SearchNiagaraByParameter` | `search_by_parameter` | Find systems exposing a user parameter by case-insensitive substring name, optional type filter |
| `SearchNiagaraByDataInterface` | `search_by_data_interface` | Find systems using a DI whose class name matches (per-system `ForEachDataInterface` traversal) |
| `QueryNiagara` | `query_niagara` | Structured-filter DSL over all systems (AND-joined conditions: `emitters >/</= N`, `sim_target=GPU\|CPU`, `has_renderer=<name>`) |
| `FindSimilarNiagaraSystems` | `find_similar_systems` | Rank systems by structural similarity to a reference (weighted: emitter-count proximity + renderer-class Jaccard + module-name Jaccard; self=1.0) |
| `SearchNiagaraByMaterial` | `search_by_material` | Find systems whose emitter renderers reference a given material |
| `FindNiagaraReferences` | `find_niagara_references` | Find all assets referencing a given Niagara asset (Asset Registry referencer graph) |
| `GetNiagaraDataInterfaces` | `list_system_data_interfaces` | Enumerate DIs actually USED BY a given system (per-system traversal; distinct from CDO-only `get_di_properties`) |

These 7 actions land green (issue #64, Tranche 2). The `niagara` namespace count rose 120 → 127. PR #65 (`get_custom_hlsl_text` + `set_custom_hlsl_text`) then brought it 127 → **129** (see [SPEC_CORE.md §12](../SPEC_CORE.md#12-action-count-summary)).

### Build Gating — `WITH_NIAGARA_WIZARD_PRIVATE` (PR #65)

The ParameterMap bridge graph generated by `create_module_from_hlsl` / `create_function_from_hlsl` links against an engine-private NiagaraEditor wizard API. That linkage is gated behind the `WITH_NIAGARA_WIZARD_PRIVATE` flag in `MonolithNiagara.Build.cs`:

- **Dev builds:** ON — the full ParameterMap bridge path is available.
- **Release builds (`MONOLITH_RELEASE_BUILD=1`):** forced OFF — the wizard linkage is compiled out and an internal fallback path is used so the actions still register and behave gracefully.
- **`get_custom_hlsl_text` / `set_custom_hlsl_text`** ride public UPROPERTY reflection only and are therefore **always available**, independent of this flag.

### UE 5.7 Compatibility Fixes (6 sites)

All marked with "UE 5.7 FIX" comments:
1. `AddEmitterHandle` takes `FGuid VersionGuid`
2-5. `GetOrCreateStackFunctionInputOverridePin` uses 5-param version (two FGuid params)
6. `RapidIterationParameters` accessed via direct UPROPERTY (no getter)

### Unity-safe file-local helpers (#68)

Internal-linkage helpers (anonymous-namespace functions/types, file-`static`s) must carry file-unique names or live in per-file named namespaces — matching the MonolithUI model — so they don't collide when adaptive/full unity concatenates same-module `.cpp`s into one translation unit. (The previously-global `SuccessObj`/`GetAssetPath`/`SuccessStr` in `MonolithNiagaraActions.cpp` now use the `NA_` prefix.)

---
