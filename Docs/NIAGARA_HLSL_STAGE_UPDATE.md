# Niagara MCP Update Notes

## Scope

This update adds four practical Niagara authoring improvements for MCP-driven workflows:

1. `create_module_from_hlsl` / `create_function_from_hlsl` now preserve Niagara Data Interface input types instead of silently degrading unknown types to `float`.
2. Existing module stack actions now support `ParticleSimulationStageScript` via selectors on the existing APIs instead of introducing separate stage-only actions.
3. `add_simulation_stage` now materializes a matching stage output node in the emitter graph, so simulation-stage stack actions have a real graph anchor.
4. New script-level actions expose `CustomHlsl` node text for direct read/write iteration.

This update now generates a `ParameterMapGet -> CustomHlsl -> ParameterMapSet` bridge for HLSL-created module scripts so they participate in module-style ParameterMap history instead of only using direct typed pins.

## Current Status

Shipped in the current build:

- DI input type preservation for HLSL-created Niagara scripts.
- Strict failure on unknown HLSL pin types instead of implicit `float` fallback.
- ParameterMap bridge generation for `create_module_from_hlsl` module scripts.
- Simulation-stage selectors on existing module stack actions.
- Stage output-node materialization after `add_simulation_stage`.
- `get_custom_hlsl_text` / `set_custom_hlsl_text` for direct script-level HLSL iteration.

Not yet shipped in the current build:

- Full simulation-stage + DI round-trip in `export_system_spec` / `import_system_spec`.
- Node-level compile diagnostics such as `node_guid` and HLSL line mapping.

## New Niagara Actions

### `niagara.get_custom_hlsl_text`

Reads the `CustomHlsl` string from a `UNiagaraNodeCustomHlsl` inside a Niagara script asset.

Params:

- `script_path` (required): Niagara script asset path.
- `node_guid` (optional): specific `CustomHlsl` node GUID when the script contains multiple HLSL nodes.

Behavior:

- If the script has exactly one `CustomHlsl` node, `node_guid` is optional.
- If the script has multiple `CustomHlsl` nodes and `node_guid` is omitted, the action returns an error listing available GUIDs.

Result fields:

- `script_path`
- `node_guid`
- `hlsl`

### `niagara.set_custom_hlsl_text`

Overwrites the `CustomHlsl` string on a `UNiagaraNodeCustomHlsl` inside a Niagara script asset.

Params:

- `script_path` (required): Niagara script asset path.
- `hlsl` (required): replacement HLSL source body.
- `node_guid` (optional): specific `CustomHlsl` node GUID when the script contains multiple HLSL nodes.

Behavior:

- Marks the node dirty via `MarkNodeRequiresSynchronization`.
- Marks the script package dirty.
- Requests script compile via `Script->RequestCompile(Script->GetExposedVersion().VersionGuid, false)`.

Result fields:

- `success`
- `script_path`
- `node_guid`
- `length`

## Updated Existing Actions

### `create_module_from_hlsl` / `create_function_from_hlsl`

Input/output pin type parsing is now strict for HLSL script creation.

For `create_module_from_hlsl`, module scripts now generate an explicit bridge graph in the style of engine-authored modules:

- hidden `InputMap` entry node
- `ParameterMapGet` reads for `Module.*` inputs
- `CustomHlsl`
- `ParameterMapSet` writes for declared outputs
- module `OutputNode`

Previous behavior:

- Unknown type names silently fell back to `float`.
- This broke boids-style modules because `NeighborGrid3D` / `ParticleRead` inputs were created as scalar pins.
- Module scripts used a simplified graph where typed inputs were wired directly into `CustomHlsl`, bypassing ParameterMap-backed module semantics.

Current behavior:

- Base Niagara types still resolve normally: `float`, `vec2`, `vec3`, `vec4`, `int`, `bool`, `color`, `position`, `quat`, `matrix`.
- Data Interface class-like type names now resolve to `FNiagaraTypeDefinition(UClass*)` when a matching `UNiagaraDataInterface` subclass can be found.
- Unknown types now fail the action with an explicit error instead of degrading.
- Module scripts expose inputs through `Module.*` ParameterMap reads and declared outputs through `ParameterMapSet` writes, which is the required shape for normal stack-style module input discovery and override binding.

Examples of intended type names:

- `NeighborGrid3D`
- `ParticleRead`
- `NiagaraDataInterfaceNeighborGrid3D`
- `UNiagaraDataInterfaceParticleRead`

### Module stack actions now support simulation stages

The following existing actions can now target `ParticleSimulationStageScript` without new stage-specific actions:

- `niagara.get_ordered_modules`
- `niagara.add_module`
- `niagara.move_module`
- `niagara.duplicate_module`

Use:

- `usage: "particle_simulation_stage"`

Additional selectors when usage is `particle_simulation_stage`:

- `usage_id`
- `stage_name`
- `stage_index`

Selection rules:

- If only one simulation stage exists, selector fields may be omitted.
- If multiple simulation stages exist, callers should pass one of `usage_id`, `stage_name`, or `stage_index`.
- `add_simulation_stage` now also materializes the matching stage output node in the emitter graph. This is intended to remove the previous `No output node` failure mode when a stage existed as data but not as a graph output.

`get_ordered_modules` behavior:

- With no `usage`, it still returns the four standard stages.
- If the emitter has simulation stages, it now also appends each simulation stage module list separately with:
  - `usage: "particle_simulation_stage"`
  - `stage_name`
  - `usage_id`

### `add_simulation_stage`

Previous behavior:

- A simulation stage object could be created and visible through `get_simulation_stages`.
- The corresponding graph output node was not guaranteed to exist immediately afterward.
- This could leave later `add_module(usage="particle_simulation_stage")` calls with no graph target.

Current behavior:

- After the stage is added to the emitter, the plugin now materializes the matching `ParticleSimulationStageScript` output node in the emitter graph using the new stage `usage_id`.

Practical effect:

- Stage-targeted stack operations now have the correct graph anchor to search against.
- If `add_module` still fails for a simulation stage after this update, the remaining blocker is no longer the missing stage output-node creation step.

## Recommended Test Flow

### HLSL type preservation

1. Call `create_module_from_hlsl` with inputs including `NeighborGrid3D` and `ParticleRead`.
2. Add the module to a simulation stage or regular stage as needed.
3. Call `get_module_inputs` on the added module.
4. Confirm those inputs report as Data Interface types rather than `float`.
5. Call `set_module_input_di` or `configure_data_interface` against those inputs.

Expected outcome:

- The action should not silently create scalar pins.
- DI-specific follow-up actions should no longer fail because the input type is wrong.

### Simulation stage module stack

1. Create or open an emitter with one or more simulation stages.
2. Call `get_simulation_stages` and record `usage_id` / `name`.
3. Call `add_module` with:
   - `usage: "particle_simulation_stage"`
   - one of `usage_id` or `stage_name`
4. Call `get_ordered_modules` with the same selector.
5. Call `move_module` and `duplicate_module` on the inserted node.

Expected outcome:

- Stage-local modules should now have a real output node to target after `add_simulation_stage`.
- If insertion still fails, capture the exact `add_module` error plus the `get_simulation_stages` response because the missing output-node creation step is now expected to be fixed.

### Direct HLSL edit loop

1. Create or load a Niagara module/function script that contains a `CustomHlsl` node.
2. Call `get_custom_hlsl_text`.
3. Modify the body.
4. Call `set_custom_hlsl_text`.
5. Re-read via `get_custom_hlsl_text` and compile/diagnose as needed.

## Known Limitations

- `get_custom_hlsl_text` / `set_custom_hlsl_text` operate at the script asset level, not system-module-instance level.
- If a script contains multiple `CustomHlsl` nodes, callers must disambiguate with `node_guid`.
- The new bridge generation has only been compile-verified in this update. The full fish-boids fresh-system workflow should still be re-run to validate DI auto-binding behavior end-to-end.
- `export_system_spec` / `import_system_spec` are still not complete for simulation-stage round-trip and DI override round-trip.
- Diagnostics are still system/script level; they do not yet return dedicated `node_guid` or HLSL line metadata.

## Suggested Agent Prompts

### Read HLSL

```json
{
  "action": "get_custom_hlsl_text",
  "params": {
    "script_path": "/Game/VFX/Modules/NM_Boids.NM_Boids"
  }
}
```

### Write HLSL

```json
{
  "action": "set_custom_hlsl_text",
  "params": {
    "script_path": "/Game/VFX/Modules/NM_Boids.NM_Boids",
    "hlsl": "/* replacement body */"
  }
}
```

### Add module to simulation stage

```json
{
  "action": "add_module",
  "params": {
    "asset_path": "/Game/VFX/NS_Fish.NS_Fish",
    "emitter": "Fish",
    "usage": "particle_simulation_stage",
    "stage_name": "BoidsStage",
    "module_script": "/Game/VFX/Modules/NM_Boids.NM_Boids"
  }
}
```
