# Niagara HLSL Authoring Guide

Complete reference for writing custom HLSL in Niagara via Monolith MCP.

## Table of Contents
1. [New MCP Actions](#new-mcp-actions)
2. [HLSL Script Creation Rules](#hlsl-script-creation-rules)
3. [Grid Data Interface API](#grid-data-interface-api)
4. [Module Stack Enhancements](#module-stack-enhancements)

---

## New MCP Actions

### `get_custom_hlsl_text`
Read HLSL source from a `CustomHlsl` node in a Niagara script.

**Params:**
- `script_path` (required): Niagara script asset path
- `node_guid` (optional): Specific node GUID when script has multiple HLSL nodes

**Returns:** `script_path`, `node_guid`, `hlsl`

### `set_custom_hlsl_text`
Overwrite HLSL source on a `CustomHlsl` node with auto-compile.

**Params:**
- `script_path` (required): Niagara script asset path
- `hlsl` (required): Replacement HLSL body
- `node_guid` (optional): Specific node GUID when script has multiple HLSL nodes

**Behavior:** Marks node dirty, requests compile, saves package.

---

## HLSL Script Creation Rules

### Critical Rules for `create_module_from_hlsl`

**1. Input/Output Variables Are Auto-Declared**
- Do NOT redeclare them in HLSL
- Niagara injects pins into shader scope automatically
- Use bare names: `InValue`, NOT `Module.InValue`

**2. Custom HLSL Is Injected Inside Function Body**
- Global variables, bare functions, namespaces are INVALID
- No `::` static calls allowed

**3. Functions Must Be Wrapped in Structs**
```hlsl
struct FMyMath {
    float3 ComputeForce(float3 pos, float strength) {
        return normalize(pos) * strength;
    }
};

// Instantiate before calling
FMyMath Math;
OutForce = Math.ComputeForce(Position, Strength);
```

**4. Struct Methods Cannot Access Outer Scope**
- Always pass ALL inputs, constants, and DIs as parameters
- No implicit capture of module inputs

**5. GPU-Only: Can Write Particle Attributes via `Particles.*`**
```hlsl
#if GPU_SIMULATION
float3 CurrentPos = Particles.Position;
Particles.Velocity = normalize(CurrentPos) * Strength;
#endif
```
- MUST wrap in `#if GPU_SIMULATION ... #endif`
- CPU simulation does NOT support `Particles.*` syntax

**6. Can Access Data Interface Functions**
- If a DI is passed as input, its API functions work
- Example: `Grid.SamplePreviousGridVector3Value(unitPos, "Velocity")`

**7. Grid Attribute Names Use Strings**
- `"Velocity"` in Grid API is a channel name, NOT a variable reference

**8. DI Sampling Reads Previous Frame**
- `SamplePrevious*` reads last frame's state, not current writes
- Use Simulation Stage iteration for immediate reads

**9. GPU-Only Code Needs Conditional Compilation**
```hlsl
OutVelocity = float3(0,0,0); // Default for CPU
#if GPU_SIMULATION
OutVelocity = Grid.SamplePreviousGridVector3Value(unitPos, "Velocity");
#endif
```

**10. Output Variables Need Default Values**
- Assign valid defaults BEFORE any `#if GPU_SIMULATION` block

**11. Strict Type Matching Required**
- Avoid implicit casts between `float`, `float2`, `float3`, `int`, `bool`

**12. Use Unique Struct Names**
- Avoid generic names like `FMath` to prevent symbol collisions

### Supported Input/Output Types
- Base: `float`, `int`, `bool`, `vec2`, `vec3`, `vec4`
- Special: `color`, `position`, `quat`, `matrix`
- Data Interfaces: `NeighborGrid3D`, `Grid3D`, `ParticleRead`, etc.

### Type Resolution
- Unknown types now FAIL explicitly (no silent float fallback)
- DI class names support fuzzy matching:
  - `NeighborGrid3D` → `UNiagaraDataInterfaceNeighborGrid3D`
  - `Grid3D` → `UNiagaraDataInterfaceGrid3DCollection`

### Module Script Graph Structure
`create_module_from_hlsl` now generates ParameterMap bridge:
```
InputMap → ParameterMapGet (Module.* reads) → CustomHlsl → ParameterMapSet (outputs) → OutputNode
```
This matches engine-authored module structure for proper stack integration.

---

## Grid Data Interface API

### API Naming Convention
`<Operation><Previous?><Grid?><Type><Value/AtIndex>`

**Coordinate Parameters:**
- Grid 2D: `(int IndexX, int IndexY)` or `(float2 Unit)`
- Grid 3D: `(int IndexX, int IndexY, int IndexZ)` or `(float3 Unit)`

**Type Suffixes:** `Float`=`float`, `Vector2`=`float2`, `Vector3`=`float3`, `Vector4`=`float4`

### Core Operations

#### 1. Write (Current Frame)
```hlsl
void Set<Type>Value(int Index..., <type> Value, Attribute)
void SetValueAtIndex(int Index..., int AttributeIndex, float Value)
```

#### 2. Read Exact (Previous Frame)
```hlsl
<type> GetPrevious<Type>Value(int Index..., Attribute)
float GetPreviousValueAtIndex(int Index..., int AttributeIndex)
```

#### 3. Linear Interpolation (4/8 samples)
```hlsl
<type> SamplePreviousGrid<Type>Value(float2/3 Unit, Attribute)
```

#### 4. Cubic Interpolation (16/64 samples)
```hlsl
<type> CubicSamplePreviousGrid<Type>Value(float2/3 Unit, Attribute)
```

#### 5. Cell Operations
```hlsl
void ClearCell(int Index...)
void CopyPreviousToCurrentForCell(int Index...)
```

### Coordinate Systems
- **Index Coords:** Integer `[0, NumCells-1]` for exact access
- **Normalized Coords:** Float `[0, 1]` for interpolated sampling
- **Conversion:** `Unit = (World - GridMin) / GridSize`

### Double Buffering
- **Previous Buffer:** Read-only (`Get`/`Sample` access)
- **Current Buffer:** Write-only (`Set` access)
- Current frame writes become readable next frame

### Interpolation Comparison
| Method | Samples | Performance | Use Case |
|--------|---------|-------------|----------|
| Get | 1 | Fastest | Exact access |
| Sample | 4/8 | Fast | General interpolation |
| CubicSample | 16/64 | Slow | Smooth fluids |

### Code Examples

**Grid 3D Velocity Advection:**
```hlsl
// Inputs: Grid3D, DeltaTime, GridSize, NumCellsX/Y/Z
// Outputs: (none, writes to Grid)

float3 vel = Grid.GetPreviousVector3Value(IndexX, IndexY, IndexZ, "Velocity");
float3 unitPos = (float3(IndexX, IndexY, IndexZ) + 0.5) / float3(NumCellsX, NumCellsY, NumCellsZ);
float3 prevPos = saturate(unitPos - vel * DeltaTime / GridSize);
float3 advectedVel = Grid.SamplePreviousGridVector3Value(prevPos, "Velocity");
Grid.SetVector3Value(IndexX, IndexY, IndexZ, advectedVel, "Velocity");
```

**Particle Sampling Grid (Cubic):**
```hlsl
// Inputs: Grid3D, GridMin, GridSize, Strength, Position
// Outputs: OutVelocity

OutVelocity = float3(0, 0, 0);
#if GPU_SIMULATION
float3 unitPos = (Position - GridMin) / GridSize;
float3 velocity = Grid.CubicSamplePreviousGridVector3Value(unitPos, "Velocity");
OutVelocity = velocity * Strength;
#endif
```

### Neighbor Grid 3D API

**Purpose:** Particle neighbor queries via spatial hashing.

**Key Functions:**
```hlsl
bool AddParticle(int IndexX, int IndexY, int IndexZ, int ParticleIndex)
int GetParticleNeighbor(int Linear)
int GetParticleNeighborCount(int Linear)
int NeighborGridIndexToLinear(int IndexX, int IndexY, int IndexZ, int Neighbor)
```

**Workflow:**
1. Each particle calls `AddParticle` to register itself
2. Calculate cell linear index from position
3. Use `GetParticleNeighborCount` to get neighbor count
4. Iterate neighbors with `GetParticleNeighbor`

**Example:**
```hlsl
int3 cellIdx = floor((Particles.Position - GridMin) / CellSize);
int cellLinear = cellIdx.x + cellIdx.y * NumCellsX + cellIdx.z * NumCellsX * NumCellsY;
int neighborCount = Grid.GetParticleNeighborCount(cellLinear);
for (int i = 0; i < neighborCount; i++) {
    int linear = Grid.NeighborGridIndexToLinear(cellIdx.x, cellIdx.y, cellIdx.z, i);
    int neighborIdx = Grid.GetParticleNeighbor(linear);
    // Process neighbor particle
}
```

---

## Module Stack Enhancements

### Simulation Stage Support

The following actions now support `particle_simulation_stage`:
- `get_ordered_modules`
- `add_module`
- `move_module`
- `duplicate_module`

**Usage:**
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

**Selectors:**
- `usage_id` - Stage GUID
- `stage_name` - Stage name string
- `stage_index` - Zero-based index

**Behavior:**
- If only one stage exists, selectors are optional
- `add_simulation_stage` now materializes matching output node in emitter graph

### Event Handler Support

Actions now support `particle_event` usage:
```json
{
  "action": "add_module",
  "params": {
    "usage": "particle_event",
    "handler_index": 0,
    "module_script": "/Script/Niagara.NiagaraScriptModule'/Niagara/Modules/Events/ReceiveDeathEvent.ReceiveDeathEvent'"
  }
}
```

**Important:**
- `add_event_handler` creates handler but does NOT auto-add `Receive<Event>` modules
- For death-triggered effects, manually add `ReceiveDeathEvent` and set `Position` payload to `Apply`
- Adding `GenerateDeathEvent` to `particle_update` auto-enables `requires_persistent_ids`

### get_ordered_modules Behavior

**Without `usage` param:** Returns four standard stages (spawn/update/event/sim)

**With simulation stages:** Also appends each stage separately with:
- `usage: "particle_simulation_stage"`
- `stage_name`
- `usage_id`

---

## Performance Tips

1. **Merge Sampling:** Use `GetPreviousVectorValue` instead of 3× `GetPreviousFloatValue`
2. **Choose Interpolation:** `Get` for exact, `Sample` for general, `CubicSample` for fluids
3. **Skip Boundary Checks:** For interior cells only
4. **Match Data Types:** Use Vector3 for 3D data, not 3× Float

## FAQ

**Q: Why can't I read just-written data?**
A: Double buffering. Current writes readable next frame. Use Simulation Stage iteration for immediate reads.

**Q: Why is CubicSample inaccurate at boundaries?**
A: Cubic needs 4×4×4 neighbors. Insufficient at boundaries causes linear fallback.

**Q: What happens to unknown HLSL types?**
A: Action now FAILS explicitly instead of silently degrading to float.

**Q: How do I debug HLSL compile errors?**
A: Use `get_system_diagnostics` for compile errors. Node-level diagnostics coming in future update.

---

## Source References
- Engine: `Engine/Plugins/FX/Niagara/Source/Niagara/Private/NiagaraDataInterfaceGrid*Collection.cpp`
- Shaders: `Engine/Plugins/FX/Niagara/Shaders/Private/NiagaraDataInterfaceGrid*Collection.ush`
- Spec: `Docs/specs/SPEC_MonolithNiagara.md`
