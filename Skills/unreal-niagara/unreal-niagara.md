---
name: unreal-niagara
description: Use when creating, editing, or inspecting Niagara particle systems via Monolith MCP. Covers systems, emitters, modules, parameters, renderers, DI, and HLSL. Triggers on Niagara, particle, VFX, emitter, particle system.
---

# Unreal Niagara VFX Workflows

You have access to **Monolith** with 108 Niagara actions via `niagara_query()`.

## Discovery

```
monolith_discover({ namespace: "niagara" })
```

## Asset Path Conventions

All asset paths follow UE content browser format (no .uasset extension):

| Location | Path Format | Example |
|----------|------------|--------|
| Project Content/ | `/Game/Path/To/Asset` | `/Game/VFX/NS_Sparks` |
| Project Plugins/ | `/PluginName/Path/To/Asset` | `/MassProjectile/VFX/NS_Example` |
| Engine Plugins | `/PluginName/Path/To/Asset` | `/Niagara/DefaultAssets/SystemAssets/NS_Default` |

## Key Parameter Names

- `asset_path` — the Niagara system asset path (NOT `system` or `asset`)
- `emitter` — emitter name (string)
- `module_node` — module GUID returned by `get_ordered_modules` (NOT module display name)

## Cross-Namespace Capabilities

These already-working features span multiple Monolith namespaces and are useful during Niagara workflows:

### Texture/Material Preview
Agents can call `material_query("render_preview", { "asset_path": "/Game/SomeTexture" })` to preview textures or materials before assigning them to renderers. Works with any texture or material asset path.

### Module Stage Info from get_ordered_modules
`get_ordered_modules` already returns a `usage` field per module indicating which stage it belongs to (e.g. `"Emitter Update"`, `"Particle Spawn"`). There is no need for a separate stage query — the stage is included in the standard module listing.

### Available Parameters with Usage Filter
`get_available_parameters` with `usage: "particle"` lists all particle attributes including compiled emitter attributes. This is the fastest way to discover what bindings are available without inspecting individual modules.

### CustomHlsl Module Input Fallback
`set_module_input_value` automatically detects CustomHlsl modules and uses pin defaults directly as a fallback path. This means you can set inputs on custom HLSL modules the same way as built-in modules — no special handling required.

## Action Reference

### System Management (9)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_system` | `asset_path` | Create a new Niagara system |
| `add_emitter` | `asset_path`, `emitter` | Add an emitter to a system |
| `remove_emitter` | `asset_path`, `emitter` | Remove an emitter |
| `duplicate_emitter` | `asset_path`, `emitter` | Duplicate an emitter |
| `set_emitter_enabled` | `asset_path`, `emitter`, `enabled` | Enable/disable an emitter |
| `reorder_emitters` | `asset_path`, `order` | Change emitter evaluation order |
| `set_emitter_property` | `asset_path`, `emitter`, `property`, `value` | Modify emitter settings |
| `get_system_property` | `asset_path`, `property` | Read a system-level property. Same aliases as set (warmup_time, determinism, random_seed, max_pool_size, etc.) |
| `set_system_property` | `asset_path`, `property`, `value` | Set system-level properties (WarmupTime, bDeterminism, bFixedTickDelta, RandomSeed, MaxPoolSize, etc.). Snake_case aliases supported |
| `request_compile` | `asset_path` | Force recompile the system |

### Read / Inspection (7 + 4 summary + 3 new)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `list_emitters` | `asset_path` | List all emitters (name, index, enabled, sim_target, renderer_count, GUID) |
| `list_renderers` | `asset_path`, `emitter` | List renderers (`type` short name, index, enabled, material) |
| `list_module_scripts` | `query`, `include_metadata`? | Search module scripts. `include_metadata: true` loads each to get ModuleUsageBitmask, Category, Description |
| `list_systems` | `search`?, `path`?, `limit`? | Search/list NiagaraSystem assets, optional path and keyword filter |
| `list_renderer_properties` | `asset_path`, `emitter`, `renderer` | List editable properties on a renderer |
| `list_emitter_properties` | `asset_path`, `emitter` | Discover what set_emitter_property accepts (reflection-based) |
| `get_ordered_modules` | `asset_path`, `emitter` | Get modules with GUIDs and `usage` stage field (needed for module actions) |
| `get_system_diagnostics` | `asset_path` | Compile errors, warnings, incompatibility checks |
| `get_system_summary` | `asset_path` | One-call overview: emitters, user params, module counts, renderer types |
| `get_emitter_summary` | `asset_path`, `emitter` | Deep emitter view: modules per stage, renderers, event handlers |
| `get_module_input_value` | `asset_path`, `emitter`, `module_node`, `input` | Read current override value (literal, bound, DI, or dynamic input) |
| `get_module_script_inputs` | `script_path` | Pre-add introspection: query module inputs, valid stages, metadata WITHOUT adding |
| `validate_system` | `asset_path` | Pre-compile validation: GPU+Light error, missing materials, bounds warnings |

### System Management (9 + 5 new)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_system` | `asset_path` | Create a new Niagara system |
| `add_emitter` | `asset_path`, `emitter` | Add an emitter to a system |
| `remove_emitter` | `asset_path`, `emitter` | Remove an emitter |
| `duplicate_emitter` | `asset_path`, `emitter` | Duplicate an emitter |
| `duplicate_system` | `asset_path`, `save_path` | Clone entire system asset |
| `create_emitter` | `asset_path`, `name`, `sim_target`? | Add truly empty emitter (Minimal template) |
| `set_fixed_bounds` | `asset_path`, `emitter`?, `min`, `max` | Set fixed bounds on system or emitter |
| `set_effect_type` | `asset_path`, `effect_type` | Assign effect type for scalability |
| `export_system_spec` | `asset_path`, `include_values`? | Reverse-engineer system to create_system_from_spec JSON |
| `set_emitter_enabled` | `asset_path`, `emitter`, `enabled` | Enable/disable an emitter |
| `reorder_emitters` | `asset_path`, `order` | Change emitter evaluation order |
| `set_emitter_property` | `asset_path`, `emitter`, `property`, `value` | Modify emitter settings |
| `get_system_property` | `asset_path`, `property` | Read a system-level property |
| `set_system_property` | `asset_path`, `property`, `value` | Set system-level properties |
| `request_compile` | `asset_path` | Force recompile |
| `get_module_graph` | `asset_path`, `emitter`, `module_node` | Get module's internal graph |

### Module Editing (10)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_module_inputs` | `asset_path`, `emitter`, `module_node` | List all inputs on a module (includes DI curve data; returns short names) |
| `add_module` | `asset_path`, `emitter`, `module_path`, `stage` | Add a module to an emitter stage |
| `remove_module` | `asset_path`, `emitter`, `module_node` | Remove a module |
| `clear_emitter_modules` | `asset_path`, `emitter`, `usage`? | Remove ALL modules from an emitter (optionally filter by stage) |
| `move_module` | `asset_path`, `emitter`, `module_node`, `new_index` | Reorder a module (preserves input overrides) |
| `set_module_enabled` | `asset_path`, `emitter`, `module_node`, `enabled` | Enable/disable a module |
| `set_module_input_value` | `asset_path`, `emitter`, `module_node`, `input`, `value` | Set a module input to a literal value. Auto-detects CustomHlsl modules and uses pin defaults directly |
| `set_module_input_binding` | `asset_path`, `emitter`, `module_node`, `input`, `binding` | Bind a module input to a parameter |
| `set_module_input_di` | `asset_path`, `emitter`, `module_node`, `input`, `di_class` | Set a data interface on a module input. Auto-resolves DI class names |
| `set_static_switch_value` | `asset_path`, `emitter`, `module_node`, `input`, `value` | Set a static switch value (bool: true/false, enum: value name, int: number) |

### Parameters (9)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_all_parameters` | `asset_path` | All parameters (system, emitter, particle) |
| `get_user_parameters` | `asset_path` | User-exposed parameters |
| `get_parameter_value` | `asset_path`, `parameter` | Get a parameter's current value |
| `get_parameter_type` | `asset_path`, `parameter` | Get a parameter's type info |
| `trace_parameter_binding` | `asset_path`, `parameter` | Follow parameter binding chain |
| `add_user_parameter` | `asset_path`, `name`, `type` | Add a user parameter |
| `remove_user_parameter` | `asset_path`, `name` | Remove a user parameter |
| `set_parameter_default` | `asset_path`, `parameter`, `value` | Set parameter default value |
| `set_curve_value` | `asset_path`, `emitter`, `module_node`, `input`, `keys` | Set curve keys on a module input |

### Composite Helpers (1)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `set_spawn_shape` | `asset_path`, `emitter`, `shape` | Set spawn location shape. Shapes: Cylinder, Sphere, Box, Cone, Torus, Grid, GridV2, Line, Ring, Disc, Wedge, CurlNoise, SkelMesh, StaticMesh, Shape (V2). Line/Ring/Disc use ShapeLocation V2 |

### DI & Curve Configuration (2 new)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `configure_curve_keys` | `asset_path`, `emitter`, `module_node`, `input`, `keys`, `interp`? | Set keys on curve DI (float/color/vector). Auto-creates override if needed |
| `configure_data_interface` | `asset_path`, `emitter`, `module_node`, `input`, `properties` | Set arbitrary DI properties via reflection |

### Dynamic Inputs (8)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_dynamic_input` | `asset_path`, `emitter`, `module_node`, `input`, `dynamic_input_script` | Attach dynamic input to module pin. Returns node GUID + inputs |
| `set_dynamic_input_value` | `asset_path`, `emitter`, `dynamic_input_node`, `input`, `value` | Set value on a dynamic input node |
| `search_dynamic_inputs` | `query`?, `input_type`?, `limit`? | Browse available dynamic input scripts |
| `list_dynamic_inputs` | `asset_path`, `emitter`, `module_node` | List all dynamic inputs attached to a module's pins |
| `get_dynamic_input_tree` | `asset_path`, `emitter`, `module_node`, `max_depth`? | Recursive tree of all inputs showing dynamic input structure |
| `remove_dynamic_input` | `asset_path`, `emitter`, `module_node`+`input` OR `dynamic_input_node` | Remove a dynamic input and all sub-nodes |
| `get_dynamic_input_value` | `asset_path`, `emitter`, `dynamic_input_node`, `input` | Read back a value on a dynamic input sub-pin |
| `get_dynamic_input_inputs` | `script_path` | Discover inputs on an unattached dynamic input script |

### Event Handlers (4)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_event_handler` | `asset_path`, `emitter`, `event_name`, `source_emitter`? | Add inter-emitter event handler |
| `get_event_handlers` | `asset_path`, `emitter` | Read all event handlers with full properties |
| `set_event_handler_property` | `asset_path`, `emitter`, `handler_index`/`usage_id`, `property`, `value` | Modify event handler property |
| `remove_event_handler` | `asset_path`, `emitter`, `handler_index`/`usage_id` | Remove an event handler |

### Simulation Stages (4)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_simulation_stage` | `asset_path`, `emitter`, `name`, `iteration_source`? | Add GPU simulation stage |
| `get_simulation_stages` | `asset_path`, `emitter` | Read all simulation stages with properties |
| `set_simulation_stage_property` | `asset_path`, `emitter`, `stage_index`/`stage_name`, `property`, `value` | Modify sim stage property |
| `remove_simulation_stage` | `asset_path`, `emitter`, `stage_index`/`stage_name` | Remove a simulation stage |

### NPC (Niagara Parameter Collections) (5)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_npc` | `save_path`, `namespace` | Create a Niagara Parameter Collection asset |
| `get_npc` | `asset_path` | Read NPC parameters and defaults |
| `add_npc_parameter` | `asset_path`, `name`, `type` | Add parameter to NPC |
| `remove_npc_parameter` | `asset_path`, `name` | Remove parameter from NPC |
| `set_npc_default` | `asset_path`, `name`, `value` | Set default value for NPC parameter |

### Effect Types & Scalability (5)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_effect_type` | `save_path`, `cull_reaction`?, `max_distance`? | Create UNiagaraEffectType asset |
| `get_effect_type` | `asset_path` | Read all scalability settings |
| `set_effect_type_property` | `asset_path`, `property`, `value` | Set effect type property |
| `get_scalability_settings` | `asset_path` | Read per-quality-level scalability configs from an effect type |
| `set_scalability_settings` | `asset_path`, `settings` | Set per-quality-level scalability configs (distance, instance counts, proxies) |

### Advanced (6)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `rename_emitter` | `asset_path`, `emitter`, `name` | Rename an emitter |
| `get_emitter_property` | `asset_path`, `emitter`, `property` | Read a single emitter property |
| `get_module_output_parameters` | `asset_path`, `emitter`, `module_node` | What attributes a module writes to |
| `get_available_parameters` | `asset_path`, `emitter`?, `usage`? | List all bindable parameters. Use `usage: "particle"` to get all particle attributes including compiled emitter attributes |
| `diff_systems` | `asset_path_a`, `asset_path_b`, `detail_level`? | Compare two systems |
| `save_emitter_as_template` | `asset_path`, `emitter`, `save_path` | Extract emitter to standalone asset |
| `clone_module_overrides` | `asset_path`, `source_emitter`, `source_module`, `target_emitter`, `target_module` | Copy input overrides between modules |
| `duplicate_module` | `asset_path`, `source_emitter`, `source_module_node`, `target_emitter`?, `target_usage`?, `target_index`? | Duplicate a module with all overrides (composite of add_module + clone_module_overrides) |
| `get_emitter_parent` | `asset_path`, `emitter` | Get parent emitter asset path (read-only) |
| `rename_user_parameter` | `asset_path`, `old_name`, `new_name` | Rename user param and update all module bindings. HLSL string refs NOT updated |
| `preview_system` | `asset_path`, `seek_time`?, `resolution`? | Capture preview screenshot |
| `save_system` | `asset_path`, `only_if_dirty`? | Save any Niagara asset to disk |
| `get_static_switch_value` | `asset_path`, `emitter`, `module_node`, `input`? | Get static switch value(s) — omit input to list all |
| `import_system_spec` | `asset_path`, `spec`, `mode`? | Overwrite existing system with JSON spec |

### Renderers (6)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_renderer` | `asset_path`, `emitter`, `type` | Add a renderer to an emitter |
| `remove_renderer` | `asset_path`, `emitter`, `renderer` | Remove a renderer |
| `set_renderer_material` | `asset_path`, `emitter`, `renderer`, `material` | Assign material to renderer |
| `set_renderer_property` | `asset_path`, `emitter`, `renderer`, `property`, `value` | Modify renderer settings |
| `get_renderer_bindings` | `asset_path`, `emitter`, `renderer` | Get renderer's attribute bindings |
| `set_renderer_binding` | `asset_path`, `emitter`, `renderer`, `binding`, `value` | Set a renderer attribute binding |

### Batch Operations (2)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `batch_execute` | `asset_path`, `commands` | Execute multiple actions in sequence |
| `create_system_from_spec` | `spec` | Create a complete system from a JSON specification |

### Data Interface & HLSL (3)
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_di_functions` | `di_class` | List functions available on a data interface class |
| `get_di_properties` | `di_class` | Inspect editable properties + function signatures on a DI class via CDO reflection |
| `get_compiled_gpu_hlsl` | `asset_path`, `emitter` | Get the compiled GPU HLSL for an emitter (auto-compiles if needed) |

### Custom HLSL Module/Function Creation (2)
| Action | Key Params | Description |
|--------|-----------|-------------|
| `create_module_from_hlsl` | `name`, `save_path`, `hlsl`, `inputs[]`, `outputs[]` | Create a standalone Niagara module from custom HLSL |
| `create_function_from_hlsl` | `name`, `save_path`, `hlsl`, `inputs[]`, `outputs[]` | Create a reusable Niagara function from custom HLSL |

**Input/output format:** `[{"name": "InValue", "type": "float"}, {"name": "Velocity", "type": "vec3"}]`
**Supported types:** `float`, `int`, `bool`, `vec2`, `vec3`, `vec4`, `color`, `position`, `quat`, `matrix`

**HLSL body rules:**
- Use bare input names in HLSL (e.g. `InValue`, NOT `Module.InValue`)
- **Outputs must use bare names** (e.g. `OutValue`, NOT `Particles.Color`) — dots in output names generate broken HLSL (`Out_Particles.Color` → struct member access parse error)
- To write particle attributes, write directly to `Particles.X` in the HLSL body (ParameterMap resolution handles it) — don't put them in the outputs array
- The compiler generates `In_X` for inputs and `Out_X` for outputs internally
- Can't swizzle ParameterMap variables directly (`Particles.Color.xyz` is one token) — assign to local first: `float4 C = Particles.Color;`

## Common Workflows

### Inspect a system
```
niagara_query({ action: "list_emitters", params: { asset_path: "/Game/VFX/NS_Sparks" } })
niagara_query({ action: "get_ordered_modules", params: { asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain" } })
niagara_query({ action: "get_module_inputs", params: { asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain", module_node: "<GUID from get_ordered_modules>" } })
```

### Create a system and add an emitter
```
niagara_query({ action: "create_system", params: { asset_path: "/Game/VFX/NS_Sparks" } })
niagara_query({ action: "add_emitter", params: { asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain" } })
```

### Set a module input value
```
niagara_query({ action: "set_module_input_value", params: {
  asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain",
  module_node: "<GUID>", input: "Lifetime", value: 2.0
}})
```

### Add a renderer with material
```
niagara_query({ action: "add_renderer", params: { asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain", type: "SpriteRenderer" } })
niagara_query({ action: "set_renderer_material", params: {
  asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain",
  renderer: "SpriteRenderer", material: "/Game/Materials/M_Particle"
}})
```

### Find and add a module script
```
niagara_query({ action: "list_module_scripts", params: { query: "ShapeLocation" } })
// Space-separated terms also work — "Gravity Force" matches "GravityForce"
niagara_query({ action: "list_module_scripts", params: { query: "Gravity Force" } })
niagara_query({ action: "add_module", params: { asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain", module_path: "<path from list_module_scripts>", stage: "Particle Spawn" } })
```

### Inspect renderer properties before setting them
```
niagara_query({ action: "list_renderer_properties", params: { asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain", renderer: "SpriteRenderer" } })
niagara_query({ action: "set_renderer_property", params: { asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain", renderer: "SpriteRenderer", property: "SubImageSize", value: "4,4" } })
```

### Preview a texture before using it
```
material_query({ action: "render_preview", params: { asset_path: "/Game/VFX/Textures/T_Smoke" } })
```

## Working with Particle Materials

When creating VFX that need custom materials, the **material agent creates materials FIRST**, then you assign them to renderers.

### Assigning Materials to Renderers
```
niagara_query({ action: "set_renderer_material", params: {
  asset_path: "/Game/VFX/NS_Fire", emitter: "Fire",
  renderer: "SpriteRenderer", material: "/Game/VFX/Materials/M_FireParticle"
}})
```

### What to Request from Material Agent
When collaborating with the material agent, specify:
1. **Blend mode:** Additive (fire, glow, sparks) or Translucent (smoke, fog, dust)
2. **Shading model:** Unlit (always for particles)
3. **Particle Color support:** Material must multiply by `Particle Color` node so you can drive `Particles.Color`
4. **Dynamic Parameters:** Request if you need per-particle material control (erosion, intensity)
5. **Soft edges:** Procedural radial gradient (Custom HLSL) for textureless soft particles
6. **Depth fade:** For translucent particles that intersect geometry

### Material Conventions
- Particle materials live at `/Game/VFX/Materials/M_<Effect>Particle`
- Fire/glow: Additive blend, emissive × 3-5, tight radial gradient (power 2-3)
- Smoke/fog: Translucent blend, opacity × 0.3-0.5, DepthFade 50-100u
- Always verify material exists before assigning: use `project_query("search", { query: "M_FireParticle" })`

### Driving Material from Niagara
- **Color:** Set `Particles.Color` via `Color` module or `set_module_input_value` — material's `Particle Color` node reads this automatically
- **Dynamic params:** Bind `Particles.DynamicMaterialParameter` to drive material's `DynamicParameter` node channels (R/G/B/A)
- **SubUV:** For sprite sheets, set renderer's `SubImageSize` property and use `SubUV Animation` module

## GPU vs CPU Sim — Compatibility Rules

When setting up emitters, these compatibility rules are enforced by the engine. Violating them produces errors/warnings that block or break the effect:

### GPU Sim (`GPUCompute Sim`)
- **Bounds:** MUST use `Fixed` bounds mode, NOT `Dynamic`. GPU emitters can't read back particle positions for dynamic bounds. Set fixed bounds via `set_emitter_property`.
- **Light Renderer:** NOT compatible with GPU sim. Light Renderer requires CPU sim to read particle positions for light placement. Use `SpriteRenderer` or `MeshRenderer` instead.
- **Ribbon Renderer:** NOT compatible with GPU sim.
- **Best for:** High particle counts (1000+), simple particle behavior, fire/sparks/debris

### CPU Sim (`CPUSim`)
- **Bounds:** Can use `Dynamic` bounds mode (default)
- **All renderers supported** including Light Renderer and Ribbon Renderer
- **Best for:** Low particle counts, complex behavior, effects that need Light Renderer, smoke/fog

### Common Pitfall
When creating fire+light effects, do NOT put the Light Renderer on a GPU emitter. Instead:
- Fire sprites → GPU emitter with SpriteRenderer
- Dynamic light → Separate CPU emitter with Light Renderer (low spawn rate, 1-3 lights)

## Known Issues

- **Emitter display name vs handle ID:** `list_renderers`, `get_ordered_modules`, `get_renderer_bindings` may fail with "Emitter not found" when passed the display name (e.g. "Fire") instead of the handle ID (e.g. "Emitter_0"). Always use `list_emitters` first to get the correct emitter identifier, and try the handle ID if the display name fails.
- **set_curve_value for DI curves:** `set_curve_value` is for inline float curves. For DataInterface curve inputs (e.g. `NiagaraDataInterfaceCurve`), use `set_module_input_di` instead with a `config` object containing FRichCurve keys.
- **UseVelDistribution=true ignores Velocity vector:** When `UseVelDistribution=true`, the `Velocity` vector input is ignored — speed comes from `Velocity Speed` instead. Set `UseVelDistribution=false` when using a direct velocity vector.

## Rules

- Use `monolith_discover("niagara")` to see per-action param schemas — there are 108 actions
- The primary asset param is `asset_path`, NOT `system` or `asset`
- Module actions require `module_node` (a GUID) — get it from `get_ordered_modules`
- Module stages: `Emitter Spawn`, `Emitter Update`, `Particle Spawn`, `Particle Update`, `Render`
- User parameters are the main interface for Blueprint/C++ control of effects
- Parameter actions now accept the `User.` prefix (e.g. `User.MyParam`) in addition to bare names
- `di_class` for `set_module_input_di` accepts both `UNiagaraDataInterfaceCurve` and `NiagaraDataInterfaceCurve` — U prefix is optional (auto-resolved)
- `create_module_from_hlsl` / `create_function_from_hlsl`: use bare input/output names (no dots — `InColor` not `Module.InColor`). Write to particle attributes via `Particles.X` in the HLSL body. Inputs are fully overridable via `set_module_input_value` after adding to a system.
- When creating VFX, always dispatch material agent FIRST, then assign materials after they're created
- Verify materials exist before assigning them to renderers

## VFX Training & Recipes

For procedural VFX creation workflows, additional skills are available:
- **vfx-patterns** — Recipe library with verified Niagara specs and parameter values
- **vfx-references** — Physical reference data (fire temps, HLSL functions, performance budgets)
- **vfx-training** — Training rubric and evaluation procedures
