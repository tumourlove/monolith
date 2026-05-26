# Niagara Common Interface HLSL API Reference

Grid 2D and 3D APIs follow the same pattern, differing only in coordinate dimensions.

## API Naming Convention
`<Operation><Previous?><Grid?><Type><Value/AtIndex>`

**Coordinate Parameters**:
- Grid 2D: `(int IndexX, int IndexY)` or `(float2 Unit)`
- Grid 3D: `(int IndexX, int IndexY, int IndexZ)` or `(float3 Unit)`

**Type Suffixes**: None/`Float`=`float`, `Vector2D`/`Vector2`=`float2`, `Vector`/`Vector3`=`float3`, `Vector4`=`float4`

## Core API

### 1. Configuration
```hlsl
bool SetNumCells(int NumCellsX, int NumCellsY[, int NumCellsZ], bool Execute=true) // CPU function
```

### 2. Write(Set) - Write to current frame buffer
```hlsl
void Set<Type>Value(int Index..., <type> Value, Attribute)
void SetValueAtIndex(int Index..., int AttributeIndex, float Value)
```

### 3. Read(GetPrevious) - Read exact value from previous frame buffer
```hlsl
<type> GetPrevious<Type>Value(int Index..., Attribute)
float GetPreviousValueAtIndex(int Index..., int AttributeIndex)
```

### 4. Linear Interpolation(Sample) - Bilinear(2D)/Trilinear(3D), 4/8 samples
```hlsl
<type> SamplePreviousGrid<Type>Value(float2/3 Unit, Attribute)
```

### 5. Cubic Interpolation(CubicSample) - Bicubic(2D)/Tricubic(3D), 16/64 samples, fallback to linear at boundaries
```hlsl
<type> CubicSamplePreviousGrid<Type>Value(float2/3 Unit, Attribute)
```

### 6. Cell Operations
```hlsl
void ClearCell(int Index...) // Clear all attributes
void CopyPreviousToCurrentForCell(int Index...) // Copy from previous frame
void CopyMaskedPreviousToCurrentForCell(int Index..., int NumAttributesSet, int AttributeMask) // 3D only
```

### 7. Attribute Index Query
```hlsl
int GetFloatAttributeIndex(Attribute)
int GetVector2DAttributeIndex(Attribute)
int GetVectorAttributeIndex(Attribute)
int GetVector4AttributeIndex(Attribute)
```

## Key Concepts

**Coordinate Systems**:
- Index Coords: Integer `[0,NumCells-1]` for exact access
- Normalized Coords: Float `[0,1]` for interpolated sampling
- Conversion: `Unit=(World-GridMin)/GridSize`, `Index=floor(Unit*NumCells)`

**Double Buffering**: Previous Buffer read-only(Get/Sample access), Current Buffer write-only(Set access), current frame writes readable next frame

**Interpolation Comparison**:
| Method | Samples | Performance | Use Case |
|--------|---------|-------------|----------|
| Get | 1 | Fastest | Exact access |
| Sample | 4/8 | Fast | General interpolation |
| CubicSample | 16/64 | Slow | High-quality smooth(fluids) |

Cubic Interpolation: 2D uses Bicubic(Bridson/Fedkiw), 3D uses Tricubic Lagrange, fallback to linear at boundaries

## Code Examples

**Grid 2D Blur**:
```hlsl
float sum=Grid.GetPreviousValueAtIndex(IndexX,IndexY); int count=1;
for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++){
  if(dx==0&&dy==0)continue; int nx=IndexX+dx,ny=IndexY+dy;
  if(nx>=0&&nx<NumCellsX&&ny>=0&&ny<NumCellsY){sum+=Grid.GetPreviousValueAtIndex(nx,ny);count++;}}
Grid.SetValueAtIndex(IndexX,IndexY,sum/count);
```

**Grid 3D Velocity Advection**:
```hlsl
float3 vel=Grid.GetPreviousVector3ValueAtIndex(IndexX,IndexY,IndexZ,"Velocity");
float3 unitPos=(float3(IndexX,IndexY,IndexZ)+0.5)/float3(NumCellsX,NumCellsY,NumCellsZ);
float3 prevPos=saturate(unitPos-vel*DeltaTime/GridSize);
float3 advectedVel=Grid.SamplePreviousGridVector3AtIndex(prevPos,"Velocity");
Grid.SetVector3ValueAtIndex(IndexX,IndexY,IndexZ,advectedVel,"Velocity");
```

**Particle Sampling Grid(Cubic)**:
```hlsl
float3 unitPos=(Particles.Position-GridMin)/GridSize;
float3 velocity=Grid.CubicSamplePreviousGridVector3AtIndex(unitPos,"Velocity");
Particles.Velocity+=velocity*Strength;
```

## Performance Optimization
1. Merge Sampling: Use `GetPreviousVectorValue` instead of 3x `GetPreviousFloatValue`
2. Choose Interpolation: Use `Get` when no smoothing needed, `Sample` for general, `CubicSample` for fluids
3. Boundary Checks: Skip checks for interior cells
4. Data Types: Choose Float/Vector2/Vector3/Vector4 as needed

## FAQ
**Q: Why can't I read just-written data?** A: Double buffering, current frame writes readable next frame, use Simulation Stage iteration for immediate reads
**Q: Why is CubicSample inaccurate at boundaries?** A: Cubic interpolation needs 4x4(2D) or 4x4x4(3D) neighbors, insufficient at boundaries causes fallback to linear
**Q: What is Attribute?** A: Named attribute defined in Grid Data Interface, replaced with index at compile time

## Console Variables
```
fx.Niagara.Grid2D.ResolutionMultiplier // Global resolution multiplier
fx.Niagara.Grid2D.CubicInterpMethod // 0=Basic,1=Monotonic
fx.Niagara.Grid3D.ResolutionMultiplier
```

## Grid 2D/3D Collection Reader API

**Purpose**: Cross-Emitter read-only access to other Emitter's Grid data

**Config Properties**:
- `EmitterName`: Source Emitter name
- `DIName`: Source Grid Data Interface name

**Available Functions**: Inherits all Grid 2D/3D read and sample functions, but **removes all write functions**:
- ✅ `GetPrevious*Value` - Exact read
- ✅ `SamplePreviousGrid*Value` - Linear interpolation
- ✅ `CubicSamplePreviousGrid*Value` - Cubic interpolation
- ✅ `Get*AttributeIndex` - Attribute index query
- ❌ `Set*Value` - Unavailable
- ❌ `ClearCell` - Unavailable
- ❌ `CopyPreviousToCurrentForCell` - Unavailable
- ❌ `SetNumCells` - Unavailable

**Use Case**: Emitter A writes Grid, Emitter B reads Grid data via Reader

## Neighbor Grid 3D API

**Purpose**: Particle neighbor queries, stores particle index list per cell

### Configuration
```hlsl
bool SetNumCells(int NumCellsX, int NumCellsY, int NumCellsZ, int MaxNeighborsPerCell) // CPU
```

### Query Functions
```hlsl
int MaxNeighborsPerCell() // Get max neighbors per cell
int NeighborGridIndexToLinear(int IndexX, int IndexY, int IndexZ, int Neighbor) // 3D index+neighbor index→linear index
```

### Neighbor Access
```hlsl
int GetParticleNeighbor(int Linear) // Get particle index at linear index
void SetParticleNeighbor(int Linear, int NeighborIndex) // Set neighbor particle index
int GetParticleNeighborCount(int Linear) // Get cell neighbor count
int SetParticleNeighborCount(int Linear, int Increment) // Atomic increment neighbor count, return old value
```

### Add Particle
```hlsl
bool AddParticle(int IndexX, int IndexY, int IndexZ, int ParticleIndex) // Add particle to cell, return success
```

**Workflow**:
1. Each particle calls `AddParticle` to add itself to its cell
2. Use `NeighborGridIndexToLinear` to calculate neighbor linear index
3. Use `GetParticleNeighbor` to iterate neighbor particles
4. Use `GetParticleNeighborCount` to get neighbor count

**Example-Find Neighbors**:
```hlsl
int3 cellIdx=floor((Particles.Position-GridMin)/CellSize);
int cellLinear=cellIdx.x+cellIdx.y*NumCellsX+cellIdx.z*NumCellsX*NumCellsY;
int neighborCount=Grid.GetParticleNeighborCount(cellLinear);
for(int i=0;i<neighborCount;i++){
  int linear=Grid.NeighborGridIndexToLinear(cellIdx.x,cellIdx.y,cellIdx.z,i);
  int neighborParticleIdx=Grid.GetParticleNeighbor(linear);
  // Process neighbor particle
}
```

**Limitation**: Neighbor count per cell cannot exceed `MaxNeighborsPerCell`, excess particles' `AddParticle` returns false

**Source**: `Engine/Plugins/FX/Niagara/Source/Niagara/Private/NiagaraDataInterfaceGrid*Collection.cpp`
**Shader**: `Engine/Plugins/FX/Niagara/Shaders/Private/NiagaraDataInterfaceGrid*Collection.ush`
