#include "MonolithMeshInspectionActions.h"
#include "Animation/Skeleton.h"
#include "MonolithMeshUtils.h"
#include "MonolithMeshCatalog.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexDatabase.h"

#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "MeshDescription.h"
#include "PhysicsEngine/BodySetup.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "StaticMeshResources.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "SQLiteDatabase.h"
#include "Editor.h"

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshInspectionActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("get_mesh_info"),
		TEXT("Get comprehensive info for a StaticMesh or SkeletalMesh (tri count, bounds, materials, LODs, collision, Nanite)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInspectionActions::GetMeshInfo),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the mesh"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("get_mesh_bounds"),
		TEXT("Get detailed bounding box info for a mesh (AABB, volume, surface area, sphere radius)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInspectionActions::GetMeshBounds),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the mesh"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("get_mesh_materials"),
		TEXT("Get material slot info with per-section triangle counts"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInspectionActions::GetMeshMaterials),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the mesh"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("get_mesh_lods"),
		TEXT("Get LOD details (tri/vert counts, section count, screen size per LOD)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInspectionActions::GetMeshLods),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the mesh"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("get_mesh_collision"),
		TEXT("Get collision setup details (simple shapes, complex, trace flag)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInspectionActions::GetMeshCollision),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the mesh"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("get_mesh_uvs"),
		TEXT("Get UV channel info with island count and overlap detection"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInspectionActions::GetMeshUvs),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the mesh"))
			.Optional(TEXT("lod_index"), TEXT("integer"), TEXT("LOD index to inspect"), TEXT("0"))
			.Optional(TEXT("uv_channel"), TEXT("integer"), TEXT("Specific UV channel (-1 = all)"), TEXT("-1"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("analyze_skeletal_mesh"),
		TEXT("Quality analysis for a SkeletalMesh (bone weights, degenerate tris, UV quality, LOD delta)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInspectionActions::AnalyzeSkeletalMesh),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the SkeletalMesh"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("analyze_mesh_quality"),
		TEXT("Deep quality analysis for a StaticMesh (non-manifold edges, degenerate tris, loose verts, UV overlap)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInspectionActions::AnalyzeMeshQuality),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the StaticMesh"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("compare_meshes"),
		TEXT("Compare two meshes side-by-side (triangle, vertex, bounds, material, LOD deltas)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInspectionActions::CompareMeshes),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path_a"), TEXT("First mesh asset path"))
			.RequiredAssetPath(TEXT("asset_path_b"), TEXT("Second mesh asset path"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("get_vertex_data"),
		TEXT("Get raw vertex positions and normals (paginated, max 5000 per call)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInspectionActions::GetVertexData),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path of the mesh"))
			.Optional(TEXT("lod_index"), TEXT("integer"), TEXT("LOD index"), TEXT("0"))
			.Optional(TEXT("offset"), TEXT("integer"), TEXT("Vertex offset"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max vertices to return (hard max 5000)"), TEXT("1000"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("search_meshes_by_size"),
		TEXT("Search the mesh catalog for meshes within a size range (requires indexer to have run)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInspectionActions::SearchMeshesBySize),
		FParamSchemaBuilder()
			.Required(TEXT("min_bounds"), TEXT("array"), TEXT("Minimum bounds [x, y, z] in cm"))
			.Required(TEXT("max_bounds"), TEXT("array"), TEXT("Maximum bounds [x, y, z] in cm"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Filter by category prefix"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results"), TEXT("20"))
			.Optional(TEXT("exclude_size_class"), TEXT("string"), TEXT("Exclude a size class"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("get_mesh_catalog_stats"),
		TEXT("Get aggregate statistics from the mesh catalog (total count, category breakdown, size distribution)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInspectionActions::GetMeshCatalogStats),
		FParamSchemaBuilder().Build());
}

// ============================================================================
// Helper: load any mesh (static or skeletal) and return JSON-friendly type info
// ============================================================================

namespace MeshInspectionHelpers
{
	/** Attempt to load as StaticMesh or SkeletalMesh; returns the loaded object or sets OutError */
	UObject* LoadAnyMesh(const FString& AssetPath, FString& OutError)
	{
		UObject* Obj = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
		if (!Obj)
		{
			OutError = FString::Printf(TEXT("Asset not found: %s"), *AssetPath);
			return nullptr;
		}

		if (!Cast<UStaticMesh>(Obj) && !Cast<USkeletalMesh>(Obj))
		{
			OutError = FString::Printf(TEXT("Expected StaticMesh or SkeletalMesh, got %s"), *Obj->GetClass()->GetName());
			return nullptr;
		}

		return Obj;
	}

	/** Build a JSON array from FVector */
	TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	/** Build a JSON array from FVector2D */
	TArray<TSharedPtr<FJsonValue>> Vector2DToJsonArray(const FVector2D& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		return Arr;
	}

	/** Get the index subsystem database, or nullptr */
	FSQLiteDatabase* GetCatalogDB()
	{
		UMonolithIndexSubsystem* IndexSub = GEditor ?
			GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
		if (!IndexSub || !IndexSub->GetDatabase())
		{
			return nullptr;
		}
		return IndexSub->GetDatabase()->GetRawDatabase();
	}

	/** Collision trace flag to string */
	FString CollisionFlagToString(ECollisionTraceFlag Flag)
	{
		switch (Flag)
		{
		case CTF_UseDefault:             return TEXT("UseDefault");
		case CTF_UseSimpleAndComplex:    return TEXT("UseSimpleAndComplex");
		case CTF_UseSimpleAsComplex:     return TEXT("UseSimpleAsComplex");
		case CTF_UseComplexAsSimple:     return TEXT("UseComplexAsSimple");
		default:                         return TEXT("Unknown");
		}
	}

	/** Collision type simplified string */
	FString CollisionTypeString(const UBodySetup* BodySetup)
	{
		if (!BodySetup) return TEXT("none");

		switch (BodySetup->CollisionTraceFlag)
		{
		case CTF_UseSimpleAndComplex: return TEXT("simple_and_complex");
		case CTF_UseSimpleAsComplex:  return TEXT("simple_only");
		case CTF_UseComplexAsSimple:  return TEXT("complex_only");
		default:                      return TEXT("default");
		}
	}
}

// ============================================================================
// 1. get_mesh_info
// ============================================================================

FMonolithActionResult FMonolithMeshInspectionActions::GetMeshInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	UObject* Obj = MeshInspectionHelpers::LoadAnyMesh(AssetPath, Error);
	if (!Obj)
	{
		return FMonolithActionResult::Error(Error);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	if (UStaticMesh* SM = Cast<UStaticMesh>(Obj))
	{
		Result->SetStringField(TEXT("type"), TEXT("StaticMesh"));

		FStaticMeshRenderData* RenderData = SM->GetRenderData();
		if (!RenderData || RenderData->LODResources.Num() == 0)
		{
			return FMonolithActionResult::Error(TEXT("Mesh has no render data (empty or corrupt)"));
		}

		const FStaticMeshLODResources& LOD0 = RenderData->LODResources[0];
		Result->SetNumberField(TEXT("triangles"), LOD0.GetNumTriangles());
		Result->SetNumberField(TEXT("vertices"), LOD0.GetNumVertices());

		// Bounds
		FBoxSphereBounds Bounds = SM->GetBounds();
		Result->SetObjectField(TEXT("bounds"), MonolithMeshUtils::BoundsToJson(Bounds));

		Result->SetNumberField(TEXT("material_slots"), SM->GetStaticMaterials().Num());
		Result->SetNumberField(TEXT("lod_count"), SM->GetNumLODs());

		// Collision
		UBodySetup* BodySetup = SM->GetBodySetup();
		Result->SetStringField(TEXT("collision"), MeshInspectionHelpers::CollisionTypeString(BodySetup));

		// Nanite
		Result->SetBoolField(TEXT("nanite_enabled"), SM->GetNaniteSettings().bEnabled != 0);

		// Lightmap UV index
		Result->SetNumberField(TEXT("lightmap_uv_index"), SM->GetLightMapCoordinateIndex());

		// Vertex colors
		Result->SetBoolField(TEXT("has_vertex_colors"), LOD0.bHasColorVertexData != 0);
	}
	else if (USkeletalMesh* SK = Cast<USkeletalMesh>(Obj))
	{
		Result->SetStringField(TEXT("type"), TEXT("SkeletalMesh"));

		FSkeletalMeshRenderData* RenderData = SK->GetResourceForRendering();
		if (!RenderData || RenderData->LODRenderData.Num() == 0)
		{
			return FMonolithActionResult::Error(TEXT("Mesh has no render data (empty or corrupt)"));
		}

		const FSkeletalMeshLODRenderData& LOD0 = RenderData->LODRenderData[0];
		Result->SetNumberField(TEXT("triangles"), LOD0.GetTotalFaces());
		Result->SetNumberField(TEXT("vertices"), LOD0.GetNumVertices());

		// Bone count
		if (SK->GetSkeleton())
		{
			Result->SetNumberField(TEXT("bone_count"), SK->GetSkeleton()->GetReferenceSkeleton().GetNum());
		}

		// Bounds
		FBoxSphereBounds Bounds = SK->GetBounds();
		Result->SetObjectField(TEXT("bounds"), MonolithMeshUtils::BoundsToJson(Bounds));

		Result->SetNumberField(TEXT("material_slots"), SK->GetMaterials().Num());
		Result->SetNumberField(TEXT("lod_count"), SK->GetLODNum());
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 2. get_mesh_bounds
// ============================================================================

FMonolithActionResult FMonolithMeshInspectionActions::GetMeshBounds(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	UObject* Obj = MeshInspectionHelpers::LoadAnyMesh(AssetPath, Error);
	if (!Obj)
	{
		return FMonolithActionResult::Error(Error);
	}

	FBoxSphereBounds Bounds;
	if (UStaticMesh* SM = Cast<UStaticMesh>(Obj))
	{
		Bounds = SM->GetBounds();
	}
	else if (USkeletalMesh* SK = Cast<USkeletalMesh>(Obj))
	{
		Bounds = SK->GetBounds();
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	FVector Min = Bounds.Origin - Bounds.BoxExtent;
	FVector Max = Bounds.Origin + Bounds.BoxExtent;
	FVector Extent = Bounds.BoxExtent * 2.0;

	// AABB
	auto AABB = MakeShared<FJsonObject>();
	AABB->SetArrayField(TEXT("min"), MeshInspectionHelpers::VectorToJsonArray(Min));
	AABB->SetArrayField(TEXT("max"), MeshInspectionHelpers::VectorToJsonArray(Max));
	Result->SetObjectField(TEXT("aabb"), AABB);

	Result->SetArrayField(TEXT("extent"), MeshInspectionHelpers::VectorToJsonArray(Extent));
	Result->SetArrayField(TEXT("center"), MeshInspectionHelpers::VectorToJsonArray(Bounds.Origin));
	Result->SetNumberField(TEXT("sphere_radius"), Bounds.SphereRadius);

	// Volume = full AABB
	double Volume = Extent.X * Extent.Y * Extent.Z;
	Result->SetNumberField(TEXT("volume_cm3"), Volume);

	// Surface area estimate of AABB
	double SurfaceArea = 2.0 * (Extent.X * Extent.Y + Extent.Y * Extent.Z + Extent.X * Extent.Z);
	Result->SetNumberField(TEXT("surface_area_cm2"), SurfaceArea);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 3. get_mesh_materials
// ============================================================================

FMonolithActionResult FMonolithMeshInspectionActions::GetMeshMaterials(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	UStaticMesh* SM = MonolithMeshUtils::LoadStaticMesh(AssetPath, Error);
	if (!SM)
	{
		return FMonolithActionResult::Error(Error);
	}

	FStaticMeshRenderData* RenderData = SM->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Mesh has no render data (empty or corrupt)"));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	const TArray<FStaticMaterial>& Materials = SM->GetStaticMaterials();
	const FStaticMeshLODResources& LOD0 = RenderData->LODResources[0];

	TArray<TSharedPtr<FJsonValue>> SlotsArray;
	for (int32 i = 0; i < Materials.Num(); i++)
	{
		auto Slot = MakeShared<FJsonObject>();
		Slot->SetNumberField(TEXT("index"), i);
		Slot->SetStringField(TEXT("name"), Materials[i].MaterialSlotName.ToString());

		FString MatPath;
		if (Materials[i].MaterialInterface)
		{
			MatPath = Materials[i].MaterialInterface->GetPathName();
		}
		Slot->SetStringField(TEXT("material_path"), MatPath);

		// Find triangle count for this material index across sections
		int32 SlotTris = 0;
		for (const FStaticMeshSection& Section : LOD0.Sections)
		{
			if (Section.MaterialIndex == i)
			{
				SlotTris += Section.NumTriangles;
			}
		}
		Slot->SetNumberField(TEXT("triangles"), SlotTris);

		SlotsArray.Add(MakeShared<FJsonValueObject>(Slot));
	}

	Result->SetArrayField(TEXT("material_slots"), SlotsArray);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 4. get_mesh_lods
// ============================================================================

FMonolithActionResult FMonolithMeshInspectionActions::GetMeshLods(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	UStaticMesh* SM = MonolithMeshUtils::LoadStaticMesh(AssetPath, Error);
	if (!SM)
	{
		return FMonolithActionResult::Error(Error);
	}

	FStaticMeshRenderData* RenderData = SM->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Mesh has no render data (empty or corrupt)"));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> LodsArray;
	for (int32 i = 0; i < RenderData->LODResources.Num(); i++)
	{
		const FStaticMeshLODResources& LOD = RenderData->LODResources[i];
		auto LodObj = MakeShared<FJsonObject>();
		LodObj->SetNumberField(TEXT("index"), i);
		LodObj->SetNumberField(TEXT("triangles"), LOD.GetNumTriangles());
		LodObj->SetNumberField(TEXT("vertices"), LOD.GetNumVertices());
		LodObj->SetNumberField(TEXT("sections"), LOD.Sections.Num());
		LodObj->SetNumberField(TEXT("screen_size"), RenderData->ScreenSize[i].Default);
		LodsArray.Add(MakeShared<FJsonValueObject>(LodObj));
	}

	Result->SetArrayField(TEXT("lods"), LodsArray);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 5. get_mesh_collision
// ============================================================================

FMonolithActionResult FMonolithMeshInspectionActions::GetMeshCollision(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	UStaticMesh* SM = MonolithMeshUtils::LoadStaticMesh(AssetPath, Error);
	if (!SM)
	{
		return FMonolithActionResult::Error(Error);
	}

	auto Result = MakeShared<FJsonObject>();

	UBodySetup* BodySetup = SM->GetBodySetup();
	if (!BodySetup)
	{
		Result->SetStringField(TEXT("collision_type"), TEXT("none"));
		auto Shapes = MakeShared<FJsonObject>();
		Shapes->SetNumberField(TEXT("boxes"), 0);
		Shapes->SetNumberField(TEXT("spheres"), 0);
		Shapes->SetNumberField(TEXT("capsules"), 0);
		Shapes->SetNumberField(TEXT("convex"), 0);
		Result->SetObjectField(TEXT("simple_shapes"), Shapes);
		Result->SetBoolField(TEXT("has_per_poly_collision"), false);
		Result->SetStringField(TEXT("collision_complexity"), TEXT("None"));
		return FMonolithActionResult::Success(Result);
	}

	Result->SetStringField(TEXT("collision_type"), MeshInspectionHelpers::CollisionTypeString(BodySetup));

	auto Shapes = MakeShared<FJsonObject>();
	Shapes->SetNumberField(TEXT("boxes"), BodySetup->AggGeom.BoxElems.Num());
	Shapes->SetNumberField(TEXT("spheres"), BodySetup->AggGeom.SphereElems.Num());
	Shapes->SetNumberField(TEXT("capsules"), BodySetup->AggGeom.SphylElems.Num());
	Shapes->SetNumberField(TEXT("convex"), BodySetup->AggGeom.ConvexElems.Num());
	Result->SetObjectField(TEXT("simple_shapes"), Shapes);

	bool bPerPoly = (BodySetup->CollisionTraceFlag == CTF_UseComplexAsSimple);
	Result->SetBoolField(TEXT("has_per_poly_collision"), bPerPoly);
	Result->SetStringField(TEXT("collision_complexity"), MeshInspectionHelpers::CollisionFlagToString(BodySetup->CollisionTraceFlag));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 6. get_mesh_uvs — UV analysis with island count and overlap detection
// ============================================================================

namespace UVAnalysis
{
	/** Flood-fill UV island counter using union-find on triangle adjacency */
	struct FUnionFind
	{
		TArray<int32> Parent;
		TArray<int32> Rank;

		void Init(int32 N)
		{
			Parent.SetNum(N);
			Rank.SetNum(N);
			for (int32 i = 0; i < N; i++)
			{
				Parent[i] = i;
				Rank[i] = 0;
			}
		}

		int32 Find(int32 X)
		{
			while (Parent[X] != X)
			{
				Parent[X] = Parent[Parent[X]]; // path compression
				X = Parent[X];
			}
			return X;
		}

		void Union(int32 A, int32 B)
		{
			A = Find(A);
			B = Find(B);
			if (A == B) return;
			if (Rank[A] < Rank[B]) Swap(A, B);
			Parent[B] = A;
			if (Rank[A] == Rank[B]) Rank[A]++;
		}
	};

	/**
	 * Count UV islands for a given UV channel using render data.
	 * Builds vertex-instance adjacency from the index buffer: two triangles sharing
	 * a vertex instance at the same UV coordinate are in the same island.
	 */
	int32 CountUVIslands(const FStaticMeshLODResources& LOD, int32 UVChannel)
	{
		const int32 NumVerts = LOD.GetNumVertices();
		const int32 NumTris = LOD.GetNumTriangles();
		if (NumTris == 0 || NumVerts == 0) return 0;

		// Get index buffer
		FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
		const int32 NumIndices = Indices.Num();
		if (NumIndices < 3) return 0;

		const FStaticMeshVertexBuffer& VertexBuffer = LOD.VertexBuffers.StaticMeshVertexBuffer;
		if (UVChannel >= (int32)VertexBuffer.GetNumTexCoords()) return 0;

		// Build UV-coordinate to vertex group mapping using a spatial hash
		// Two vertices at the same UV coordinate should be in the same island
		TMap<uint64, int32> UVHash; // hash of UV -> representative vertex index
		UVHash.Reserve(NumVerts);

		FUnionFind UF;
		UF.Init(NumVerts);

		// First pass: merge vertices with identical UVs
		for (int32 V = 0; V < NumVerts; V++)
		{
			FVector2f UV = VertexBuffer.GetVertexUV(V, UVChannel);
			// Quantize to avoid floating point issues (1/4096 precision)
			int32 QX = FMath::RoundToInt(UV.X * 4096.0f);
			int32 QY = FMath::RoundToInt(UV.Y * 4096.0f);
			uint64 Key = (static_cast<uint64>(static_cast<uint32>(QX)) << 32) | static_cast<uint32>(QY);

			if (int32* Existing = UVHash.Find(Key))
			{
				UF.Union(V, *Existing);
			}
			else
			{
				UVHash.Add(Key, V);
			}
		}

		// Second pass: merge vertices connected by triangles, and track which are referenced
		TSet<int32> ReferencedVerts;
		ReferencedVerts.Reserve(NumVerts);
		for (int32 T = 0; T < NumTris; T++)
		{
			int32 I0 = Indices[T * 3 + 0];
			int32 I1 = Indices[T * 3 + 1];
			int32 I2 = Indices[T * 3 + 2];
			UF.Union(I0, I1);
			UF.Union(I1, I2);
			ReferencedVerts.Add(I0);
			ReferencedVerts.Add(I1);
			ReferencedVerts.Add(I2);
		}

		// Count unique roots among referenced vertices only
		// (unreferenced verts in the buffer would inflate the island count)
		TSet<int32> Roots;
		Roots.Reserve(256);
		for (int32 V : ReferencedVerts)
		{
			Roots.Add(UF.Find(V));
		}

		return Roots.Num();
	}

	/**
	 * Estimate UV overlap by rasterizing triangles to a 256x256 grid.
	 * Returns overlap percentage (pixels hit more than once / total pixels hit).
	 * Skips if triangle count exceeds MaxTriangles.
	 */
	float EstimateUVOverlap(const FStaticMeshLODResources& LOD, int32 UVChannel, int32 MaxTriangles = 100000)
	{
		const int32 NumTris = LOD.GetNumTriangles();
		if (NumTris == 0 || NumTris > MaxTriangles) return -1.0f; // -1 means skipped

		const FStaticMeshVertexBuffer& VertexBuffer = LOD.VertexBuffers.StaticMeshVertexBuffer;
		if (UVChannel >= (int32)VertexBuffer.GetNumTexCoords()) return -1.0f;

		FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();

		constexpr int32 GridSize = 256;
		TArray<uint8> Grid;
		Grid.SetNumZeroed(GridSize * GridSize);

		int32 TotalHits = 0;
		int32 OverlapHits = 0;

		for (int32 T = 0; T < NumTris; T++)
		{
			FVector2f UV0 = VertexBuffer.GetVertexUV(Indices[T * 3 + 0], UVChannel);
			FVector2f UV1 = VertexBuffer.GetVertexUV(Indices[T * 3 + 1], UVChannel);
			FVector2f UV2 = VertexBuffer.GetVertexUV(Indices[T * 3 + 2], UVChannel);

			// Compute bounding box of triangle in grid space
			float MinX = FMath::Min3(UV0.X, UV1.X, UV2.X) * GridSize;
			float MaxX = FMath::Max3(UV0.X, UV1.X, UV2.X) * GridSize;
			float MinY = FMath::Min3(UV0.Y, UV1.Y, UV2.Y) * GridSize;
			float MaxY = FMath::Max3(UV0.Y, UV1.Y, UV2.Y) * GridSize;

			int32 IMinX = FMath::Clamp(FMath::FloorToInt(MinX), 0, GridSize - 1);
			int32 IMaxX = FMath::Clamp(FMath::CeilToInt(MaxX), 0, GridSize - 1);
			int32 IMinY = FMath::Clamp(FMath::FloorToInt(MinY), 0, GridSize - 1);
			int32 IMaxY = FMath::Clamp(FMath::CeilToInt(MaxY), 0, GridSize - 1);

			// Rasterize using barycentric test
			FVector2f E0 = UV1 - UV0;
			FVector2f E1 = UV2 - UV0;
			float Denom = E0.X * E1.Y - E0.Y * E1.X;
			if (FMath::Abs(Denom) < 1e-8f) continue; // degenerate
			float InvDenom = 1.0f / Denom;

			for (int32 PY = IMinY; PY <= IMaxY; PY++)
			{
				for (int32 PX = IMinX; PX <= IMaxX; PX++)
				{
					float SX = (static_cast<float>(PX) + 0.5f) / GridSize;
					float SY = (static_cast<float>(PY) + 0.5f) / GridSize;
					FVector2f P(SX, SY);
					FVector2f V = P - UV0;

					float U = (V.X * E1.Y - V.Y * E1.X) * InvDenom;
					float W = (E0.X * V.Y - E0.Y * V.X) * InvDenom;

					if (U >= 0.0f && W >= 0.0f && (U + W) <= 1.0f)
					{
						int32 Idx = PY * GridSize + PX;
						if (Grid[Idx] > 0)
						{
							OverlapHits++;
						}
						else
						{
							TotalHits++;
						}
						// Saturate at 255 to avoid wrap
						if (Grid[Idx] < 255) Grid[Idx]++;
					}
				}
			}
		}

		if (TotalHits + OverlapHits == 0) return 0.0f;
		return (static_cast<float>(OverlapHits) / static_cast<float>(TotalHits + OverlapHits)) * 100.0f;
	}
}

FMonolithActionResult FMonolithMeshInspectionActions::GetMeshUvs(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	int32 LodIndex = 0;
	if (Params->HasField(TEXT("lod_index")))
	{
		LodIndex = static_cast<int32>(Params->GetNumberField(TEXT("lod_index")));
	}

	int32 TargetChannel = -1;
	if (Params->HasField(TEXT("uv_channel")))
	{
		TargetChannel = static_cast<int32>(Params->GetNumberField(TEXT("uv_channel")));
	}

	FString Error;
	UStaticMesh* SM = MonolithMeshUtils::LoadStaticMesh(AssetPath, Error);
	if (!SM)
	{
		return FMonolithActionResult::Error(Error);
	}

	FStaticMeshRenderData* RenderData = SM->GetRenderData();
	if (!RenderData || LodIndex >= RenderData->LODResources.Num())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid LOD index %d"), LodIndex));
	}

	const FStaticMeshLODResources& LOD = RenderData->LODResources[LodIndex];
	const FStaticMeshVertexBuffer& VertexBuffer = LOD.VertexBuffers.StaticMeshVertexBuffer;
	const int32 NumTexCoords = VertexBuffer.GetNumTexCoords();
	const int32 NumVerts = LOD.GetNumVertices();
	const int32 NumTris = LOD.GetNumTriangles();

	if (NumTris > 500000)
	{
		UE_LOG(LogMonolith, Warning, TEXT("MonolithMesh: get_mesh_uvs on mesh with %d triangles — UV analysis may be slow"), NumTris);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("uv_channels"), NumTexCoords);

	TArray<TSharedPtr<FJsonValue>> ChannelsArray;

	int32 StartCh = (TargetChannel >= 0) ? TargetChannel : 0;
	int32 EndCh = (TargetChannel >= 0) ? TargetChannel + 1 : NumTexCoords;
	EndCh = FMath::Min(EndCh, NumTexCoords);

	for (int32 Ch = StartCh; Ch < EndCh; Ch++)
	{
		auto ChObj = MakeShared<FJsonObject>();
		ChObj->SetNumberField(TEXT("index"), Ch);

		// UV bounds
		FVector2f UVMin(FLT_MAX, FLT_MAX);
		FVector2f UVMax(-FLT_MAX, -FLT_MAX);
		for (int32 V = 0; V < NumVerts; V++)
		{
			FVector2f UV = VertexBuffer.GetVertexUV(V, Ch);
			UVMin.X = FMath::Min(UVMin.X, UV.X);
			UVMin.Y = FMath::Min(UVMin.Y, UV.Y);
			UVMax.X = FMath::Max(UVMax.X, UV.X);
			UVMax.Y = FMath::Max(UVMax.Y, UV.Y);
		}

		auto BoundsObj = MakeShared<FJsonObject>();
		BoundsObj->SetArrayField(TEXT("min"), MeshInspectionHelpers::Vector2DToJsonArray(FVector2D(UVMin.X, UVMin.Y)));
		BoundsObj->SetArrayField(TEXT("max"), MeshInspectionHelpers::Vector2DToJsonArray(FVector2D(UVMax.X, UVMax.Y)));
		ChObj->SetObjectField(TEXT("bounds"), BoundsObj);

		// Island count
		int32 Islands = UVAnalysis::CountUVIslands(LOD, Ch);
		ChObj->SetNumberField(TEXT("islands"), Islands);

		// Overlap
		float Overlap = UVAnalysis::EstimateUVOverlap(LOD, Ch);
		if (Overlap < 0.0f)
		{
			ChObj->SetStringField(TEXT("overlap_pct"), TEXT("skipped"));
		}
		else
		{
			ChObj->SetNumberField(TEXT("overlap_pct"), FMath::RoundToFloat(Overlap * 10.0f) / 10.0f);
		}

		ChannelsArray.Add(MakeShared<FJsonValueObject>(ChObj));
	}

	Result->SetArrayField(TEXT("channels"), ChannelsArray);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 7. analyze_skeletal_mesh
// ============================================================================

FMonolithActionResult FMonolithMeshInspectionActions::AnalyzeSkeletalMesh(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	USkeletalMesh* SK = MonolithMeshUtils::LoadSkeletalMesh(AssetPath, Error);
	if (!SK)
	{
		return FMonolithActionResult::Error(Error);
	}

	FSkeletalMeshRenderData* RenderData = SK->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Mesh has no render data (empty or corrupt)"));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("type"), TEXT("SkeletalMesh"));

	auto Metrics = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Issues;
	int32 OverallScore = 100;

	const FSkeletalMeshLODRenderData& LOD0 = RenderData->LODRenderData[0];
	int32 NumVerts = LOD0.GetNumVertices();
	int32 NumTris = LOD0.GetTotalFaces();

	Metrics->SetNumberField(TEXT("triangles"), NumTris);
	Metrics->SetNumberField(TEXT("vertices"), NumVerts);

	// LOD analysis
	int32 NumLODs = RenderData->LODRenderData.Num();
	Metrics->SetNumberField(TEXT("lod_count"), NumLODs);

	if (NumLODs < 2 && NumTris > 5000)
	{
		auto Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("type"), TEXT("missing_lods"));
		Issue->SetStringField(TEXT("severity"), TEXT("warning"));
		Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("Mesh has %d tris but only 1 LOD"), NumTris));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
		OverallScore -= 15;
	}
	else if (NumLODs >= 2)
	{
		// LOD triangle reduction delta
		int32 LOD1Tris = RenderData->LODRenderData[1].GetTotalFaces();
		float Reduction = 1.0f - (static_cast<float>(LOD1Tris) / static_cast<float>(FMath::Max(NumTris, 1)));
		Metrics->SetNumberField(TEXT("lod1_reduction_pct"), Reduction * 100.0f);

		if (Reduction < 0.3f)
		{
			auto Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("type"), TEXT("weak_lod_reduction"));
			Issue->SetStringField(TEXT("severity"), TEXT("info"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("LOD1 only reduces by %.0f%% — consider more aggressive LOD"), Reduction * 100.0f));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
			OverallScore -= 5;
		}
	}

	// Bone count
	int32 BoneCount = 0;
	if (SK->GetSkeleton())
	{
		BoneCount = SK->GetSkeleton()->GetReferenceSkeleton().GetNum();
		Metrics->SetNumberField(TEXT("bone_count"), BoneCount);

		if (BoneCount > 200)
		{
			auto Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("type"), TEXT("high_bone_count"));
			Issue->SetStringField(TEXT("severity"), TEXT("warning"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("Skeleton has %d bones — may impact GPU skinning performance"), BoneCount));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
			OverallScore -= 10;
		}
	}

	// UV channel count
	int32 NumUVs = LOD0.StaticVertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords();
	Metrics->SetNumberField(TEXT("uv_channels"), NumUVs);

	Result->SetObjectField(TEXT("metrics"), Metrics);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetNumberField(TEXT("overall_score"), FMath::Max(OverallScore, 0));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 8. analyze_mesh_quality
// ============================================================================

FMonolithActionResult FMonolithMeshInspectionActions::AnalyzeMeshQuality(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	UStaticMesh* SM = MonolithMeshUtils::LoadStaticMesh(AssetPath, Error);
	if (!SM)
	{
		return FMonolithActionResult::Error(Error);
	}

	FStaticMeshRenderData* RenderData = SM->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Mesh has no render data (empty or corrupt)"));
	}

	const FStaticMeshLODResources& LOD0 = RenderData->LODResources[0];
	const int32 NumTris = LOD0.GetNumTriangles();
	const int32 NumVerts = LOD0.GetNumVertices();

	if (NumTris > 1000000)
	{
		UE_LOG(LogMonolith, Warning, TEXT("MonolithMesh: analyze_mesh_quality on mesh with %d triangles — analysis may be slow"), NumTris);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	auto Metrics = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Issues;
	int32 OverallScore = 100;

	Metrics->SetNumberField(TEXT("triangles"), NumTris);
	Metrics->SetNumberField(TEXT("vertices"), NumVerts);

	// -- Degenerate triangles (using render data — cross product area check) --
	int32 DegenerateTris = 0;
	{
		FIndexArrayView Indices = LOD0.IndexBuffer.GetArrayView();
		const FPositionVertexBuffer& PosBuffer = LOD0.VertexBuffers.PositionVertexBuffer;

		for (int32 T = 0; T < NumTris; T++)
		{
			uint32 I0 = Indices[T * 3 + 0];
			uint32 I1 = Indices[T * 3 + 1];
			uint32 I2 = Indices[T * 3 + 2];

			FVector3f P0 = PosBuffer.VertexPosition(I0);
			FVector3f P1 = PosBuffer.VertexPosition(I1);
			FVector3f P2 = PosBuffer.VertexPosition(I2);

			FVector3f Cross = FVector3f::CrossProduct(P1 - P0, P2 - P0);
			float Area = Cross.Size() * 0.5f;
			if (Area < 1e-6f)
			{
				DegenerateTris++;
			}
		}
	}
	Metrics->SetNumberField(TEXT("degenerate_triangles"), DegenerateTris);
	if (DegenerateTris > 0)
	{
		auto Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("type"), TEXT("degenerate_triangles"));
		Issue->SetStringField(TEXT("severity"), DegenerateTris > 10 ? TEXT("warning") : TEXT("info"));
		Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("%d degenerate triangles detected (area < epsilon)"), DegenerateTris));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
		OverallScore -= FMath::Min(DegenerateTris, 20);
	}

	// -- Duplicate vertices (spatial hash at 0.01cm tolerance) --
	int32 DuplicateVerts = 0;
	{
		const FPositionVertexBuffer& PosBuffer = LOD0.VertexBuffers.PositionVertexBuffer;
		TMap<uint64, int32> SpatialHash;
		SpatialHash.Reserve(NumVerts);

		for (int32 V = 0; V < NumVerts; V++)
		{
			FVector3f Pos = PosBuffer.VertexPosition(V);
			// Quantize to 0.01cm
			int32 QX = FMath::RoundToInt(Pos.X * 100.0f);
			int32 QY = FMath::RoundToInt(Pos.Y * 100.0f);
			int32 QZ = FMath::RoundToInt(Pos.Z * 100.0f);
			// Pack into hash — use two uint64s worth of info via hashing
			uint64 Key = HashCombine(GetTypeHash(QX), HashCombine(GetTypeHash(QY), GetTypeHash(QZ)));

			if (SpatialHash.Contains(Key))
			{
				DuplicateVerts++;
			}
			else
			{
				SpatialHash.Add(Key, V);
			}
		}
	}
	Metrics->SetNumberField(TEXT("duplicate_vertices"), DuplicateVerts);
	if (DuplicateVerts > NumVerts / 10)
	{
		auto Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("type"), TEXT("excessive_duplicate_vertices"));
		Issue->SetStringField(TEXT("severity"), TEXT("info"));
		Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("%d duplicate vertices (%.1f%% of total)"),
			DuplicateVerts, (float)DuplicateVerts / FMath::Max(NumVerts, 1) * 100.0f));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
		OverallScore -= 5;
	}

	// -- LOD analysis --
	int32 NumLODs = SM->GetNumLODs();
	Metrics->SetNumberField(TEXT("lod_count"), NumLODs);
	if (NumLODs < 2 && NumTris > 5000)
	{
		auto Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("type"), TEXT("missing_lods"));
		Issue->SetStringField(TEXT("severity"), TEXT("warning"));
		Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("Mesh has %d tris but only 1 LOD"), NumTris));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
		OverallScore -= 15;
	}

	// -- UV overlap on channel 0 --
	float Overlap = UVAnalysis::EstimateUVOverlap(LOD0, 0);
	if (Overlap >= 0.0f)
	{
		Metrics->SetNumberField(TEXT("uv0_overlap_pct"), FMath::RoundToFloat(Overlap * 10.0f) / 10.0f);
		if (Overlap > 5.0f)
		{
			auto Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("type"), TEXT("uv_overlap"));
			Issue->SetStringField(TEXT("severity"), Overlap > 20.0f ? TEXT("warning") : TEXT("info"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("UV channel 0 has %.1f%% overlap"), Overlap));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
			OverallScore -= FMath::Min(static_cast<int32>(Overlap / 2.0f), 15);
		}
	}

	// -- Material count --
	int32 MatCount = SM->GetStaticMaterials().Num();
	Metrics->SetNumberField(TEXT("material_count"), MatCount);
	if (MatCount > 4)
	{
		auto Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("type"), TEXT("high_material_count"));
		Issue->SetStringField(TEXT("severity"), TEXT("info"));
		Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("Mesh uses %d materials — consider merging for fewer draw calls"), MatCount));
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
		OverallScore -= (MatCount - 4) * 2;
	}

	// -- Topology checks via FMeshDescription (non-manifold edges, loose vertices, open borders) --
	int32 NonManifoldEdges = 0;
	int32 LooseVertices = 0;
	int32 OpenBorderEdges = 0;
	bool bTopologySkipped = false;

	static constexpr int32 TopologyTriangleLimit = 500000;
	if (NumTris > TopologyTriangleLimit)
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("MonolithMesh: analyze_mesh_quality skipping topology checks — mesh has %d triangles (limit %d)"),
			NumTris, TopologyTriangleLimit);
		bTopologySkipped = true;
		NonManifoldEdges = -1;
		LooseVertices = -1;
		OpenBorderEdges = -1;
	}
	else
	{
		FMeshDescription* MeshDesc = SM->GetMeshDescription(0);
		if (MeshDesc)
		{
			// Ensure indexers are built so edge/vertex connectivity queries work
			MeshDesc->BuildIndexers();

			// Non-manifold edges: >2 connected triangles
			// Open border edges: exactly 1 connected triangle
			for (const FEdgeID EdgeID : MeshDesc->Edges().GetElementIDs())
			{
				const int32 NumConnectedTris = MeshDesc->GetNumEdgeConnectedTriangles(EdgeID);
				if (NumConnectedTris > 2)
				{
					NonManifoldEdges++;
				}
				else if (NumConnectedTris == 1)
				{
					OpenBorderEdges++;
				}
			}

			// Loose vertices: no connected triangles (orphaned)
			for (const FVertexID VertexID : MeshDesc->Vertices().GetElementIDs())
			{
				if (MeshDesc->IsVertexOrphaned(VertexID))
				{
					LooseVertices++;
				}
			}
		}
		else
		{
			UE_LOG(LogMonolith, Warning,
				TEXT("MonolithMesh: analyze_mesh_quality — GetMeshDescription(0) returned null, topology checks unavailable"));
			bTopologySkipped = true;
			NonManifoldEdges = -1;
			LooseVertices = -1;
			OpenBorderEdges = -1;
		}
	}

	Metrics->SetNumberField(TEXT("non_manifold_edges"), NonManifoldEdges);
	Metrics->SetNumberField(TEXT("loose_vertices"), LooseVertices);
	Metrics->SetNumberField(TEXT("open_border_edges"), OpenBorderEdges);

	if (!bTopologySkipped)
	{
		if (NonManifoldEdges > 0)
		{
			auto Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("type"), TEXT("non_manifold_edges"));
			Issue->SetStringField(TEXT("severity"), TEXT("warning"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("%d non-manifold edges detected (>2 connected triangles)"), NonManifoldEdges));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
			OverallScore -= FMath::Min(NonManifoldEdges * 3, 20);
		}

		if (LooseVertices > 0)
		{
			auto Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("type"), TEXT("loose_vertices"));
			Issue->SetStringField(TEXT("severity"), LooseVertices > 50 ? TEXT("warning") : TEXT("info"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("%d loose vertices (not connected to any polygon)"), LooseVertices));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
			OverallScore -= FMath::Min(LooseVertices, 10);
		}

		if (OpenBorderEdges > 0)
		{
			auto Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("type"), TEXT("open_border_edges"));
			Issue->SetStringField(TEXT("severity"), TEXT("info"));
			Issue->SetStringField(TEXT("message"), FString::Printf(TEXT("%d open border edges (mesh has holes)"), OpenBorderEdges));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
			OverallScore -= FMath::Min(OpenBorderEdges / 2, 10);
		}
	}

	// -- Vertex color presence --
	Metrics->SetBoolField(TEXT("has_vertex_colors"), LOD0.bHasColorVertexData != 0);

	Result->SetObjectField(TEXT("metrics"), Metrics);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetNumberField(TEXT("overall_score"), FMath::Max(OverallScore, 0));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 9. compare_meshes
// ============================================================================

FMonolithActionResult FMonolithMeshInspectionActions::CompareMeshes(const TSharedPtr<FJsonObject>& Params)
{
	FString PathA = Params->GetStringField(TEXT("asset_path_a"));
	FString PathB = Params->GetStringField(TEXT("asset_path_b"));

	if (PathA.IsEmpty() || PathB.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path_a and asset_path_b are required"));
	}

	FString ErrorA, ErrorB;
	UObject* ObjA = MeshInspectionHelpers::LoadAnyMesh(PathA, ErrorA);
	if (!ObjA) return FMonolithActionResult::Error(ErrorA);
	UObject* ObjB = MeshInspectionHelpers::LoadAnyMesh(PathB, ErrorB);
	if (!ObjB) return FMonolithActionResult::Error(ErrorB);

	// Extract metrics from both
	auto ExtractMetrics = [](UObject* Obj) -> TSharedPtr<FJsonObject>
	{
		auto M = MakeShared<FJsonObject>();
		if (UStaticMesh* SM = Cast<UStaticMesh>(Obj))
		{
			FStaticMeshRenderData* RD = SM->GetRenderData();
			if (RD && RD->LODResources.Num() > 0)
			{
				M->SetNumberField(TEXT("triangles"), RD->LODResources[0].GetNumTriangles());
				M->SetNumberField(TEXT("vertices"), RD->LODResources[0].GetNumVertices());
			}
			FBoxSphereBounds Bounds = SM->GetBounds();
			FVector Extent = Bounds.BoxExtent * 2.0;
			M->SetNumberField(TEXT("bounds_volume"), Extent.X * Extent.Y * Extent.Z);
			M->SetNumberField(TEXT("material_count"), SM->GetStaticMaterials().Num());
			M->SetNumberField(TEXT("lod_count"), SM->GetNumLODs());
		}
		else if (USkeletalMesh* SK = Cast<USkeletalMesh>(Obj))
		{
			FSkeletalMeshRenderData* RD = SK->GetResourceForRendering();
			if (RD && RD->LODRenderData.Num() > 0)
			{
				M->SetNumberField(TEXT("triangles"), RD->LODRenderData[0].GetTotalFaces());
				M->SetNumberField(TEXT("vertices"), RD->LODRenderData[0].GetNumVertices());
			}
			FBoxSphereBounds Bounds = SK->GetBounds();
			FVector Extent = Bounds.BoxExtent * 2.0;
			M->SetNumberField(TEXT("bounds_volume"), Extent.X * Extent.Y * Extent.Z);
			M->SetNumberField(TEXT("material_count"), SK->GetMaterials().Num());
			M->SetNumberField(TEXT("lod_count"), SK->GetLODNum());
		}
		return M;
	};

	auto MA = ExtractMetrics(ObjA);
	auto MB = ExtractMetrics(ObjB);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("mesh_a"), PathA);
	Result->SetStringField(TEXT("mesh_b"), PathB);

	auto Delta = MakeShared<FJsonObject>();

	// Build delta for each comparable metric
	auto BuildDelta = [&](const FString& FieldName)
	{
		if (MA->HasField(FieldName) && MB->HasField(FieldName))
		{
			double ValA = MA->GetNumberField(FieldName);
			double ValB = MB->GetNumberField(FieldName);
			auto D = MakeShared<FJsonObject>();
			D->SetNumberField(TEXT("a"), ValA);
			D->SetNumberField(TEXT("b"), ValB);
			D->SetNumberField(TEXT("delta"), ValB - ValA);
			if (FMath::Abs(ValA) > 0.001)
			{
				D->SetNumberField(TEXT("delta_pct"), ((ValB - ValA) / ValA) * 100.0);
			}
			Delta->SetObjectField(FieldName, D);
		}
	};

	BuildDelta(TEXT("triangles"));
	BuildDelta(TEXT("vertices"));
	BuildDelta(TEXT("bounds_volume"));
	BuildDelta(TEXT("material_count"));
	BuildDelta(TEXT("lod_count"));

	Result->SetObjectField(TEXT("delta"), Delta);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 10. get_vertex_data
// ============================================================================

FMonolithActionResult FMonolithMeshInspectionActions::GetVertexData(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	int32 LodIndex = 0;
	if (Params->HasField(TEXT("lod_index")))
	{
		LodIndex = static_cast<int32>(Params->GetNumberField(TEXT("lod_index")));
	}

	int32 Offset = 0;
	if (Params->HasField(TEXT("offset")))
	{
		Offset = static_cast<int32>(Params->GetNumberField(TEXT("offset")));
	}

	int32 Limit = 1000;
	if (Params->HasField(TEXT("limit")))
	{
		Limit = static_cast<int32>(Params->GetNumberField(TEXT("limit")));
	}
	Limit = FMath::Clamp(Limit, 1, 5000); // Hard max 5000

	FString Error;
	UStaticMesh* SM = MonolithMeshUtils::LoadStaticMesh(AssetPath, Error);
	if (!SM)
	{
		return FMonolithActionResult::Error(Error);
	}

	FStaticMeshRenderData* RenderData = SM->GetRenderData();
	if (!RenderData || LodIndex >= RenderData->LODResources.Num())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid LOD index %d"), LodIndex));
	}

	const FStaticMeshLODResources& LOD = RenderData->LODResources[LodIndex];
	const int32 TotalVerts = LOD.GetNumVertices();

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total_vertices"), TotalVerts);
	Result->SetNumberField(TEXT("offset"), Offset);

	if (Offset >= TotalVerts)
	{
		Result->SetNumberField(TEXT("count"), 0);
		Result->SetArrayField(TEXT("vertices"), TArray<TSharedPtr<FJsonValue>>());
		return FMonolithActionResult::Success(Result);
	}

	const FPositionVertexBuffer& PosBuffer = LOD.VertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& VertBuffer = LOD.VertexBuffers.StaticMeshVertexBuffer;

	int32 End = FMath::Min(Offset + Limit, TotalVerts);
	int32 Count = End - Offset;

	TArray<TSharedPtr<FJsonValue>> VertArray;
	VertArray.Reserve(Count);

	for (int32 V = Offset; V < End; V++)
	{
		auto VObj = MakeShared<FJsonObject>();

		FVector3f Pos = PosBuffer.VertexPosition(V);
		VObj->SetArrayField(TEXT("position"), MeshInspectionHelpers::VectorToJsonArray(FVector(Pos)));

		FVector4f Normal = VertBuffer.VertexTangentZ(V);
		TArray<TSharedPtr<FJsonValue>> NArr;
		NArr.Add(MakeShared<FJsonValueNumber>(Normal.X));
		NArr.Add(MakeShared<FJsonValueNumber>(Normal.Y));
		NArr.Add(MakeShared<FJsonValueNumber>(Normal.Z));
		VObj->SetArrayField(TEXT("normal"), NArr);

		VertArray.Add(MakeShared<FJsonValueObject>(VObj));
	}

	Result->SetNumberField(TEXT("count"), Count);
	Result->SetArrayField(TEXT("vertices"), VertArray);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 11. search_meshes_by_size
// ============================================================================

FMonolithActionResult FMonolithMeshInspectionActions::SearchMeshesBySize(const TSharedPtr<FJsonObject>& Params)
{
	// Parse min/max bounds arrays
	const TArray<TSharedPtr<FJsonValue>>* MinArr = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* MaxArr = nullptr;

	if (!Params->TryGetArrayField(TEXT("min_bounds"), MinArr) || MinArr->Num() != 3)
	{
		return FMonolithActionResult::Error(TEXT("min_bounds must be an array of 3 numbers"));
	}
	if (!Params->TryGetArrayField(TEXT("max_bounds"), MaxArr) || MaxArr->Num() != 3)
	{
		return FMonolithActionResult::Error(TEXT("max_bounds must be an array of 3 numbers"));
	}

	TArray<float> MinBounds, MaxBounds;
	for (int32 i = 0; i < 3; i++)
	{
		MinBounds.Add(static_cast<float>((*MinArr)[i]->AsNumber()));
		MaxBounds.Add(static_cast<float>((*MaxArr)[i]->AsNumber()));
	}

	FString Category;
	if (Params->HasField(TEXT("category")))
	{
		Category = Params->GetStringField(TEXT("category"));
	}

	FString ExcludeSizeClass;
	if (Params->HasField(TEXT("exclude_size_class")))
	{
		ExcludeSizeClass = Params->GetStringField(TEXT("exclude_size_class"));
	}

	int32 Limit = 20;
	if (Params->HasField(TEXT("limit")))
	{
		Limit = static_cast<int32>(Params->GetNumberField(TEXT("limit")));
	}

	FSQLiteDatabase* DB = MeshInspectionHelpers::GetCatalogDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Mesh catalog not available — run monolith_reindex() first"));
	}

	return FMonolithActionResult::Success(FMonolithMeshCatalog::SearchBySize(*DB, MinBounds, MaxBounds, Category, ExcludeSizeClass, Limit));
}

// ============================================================================
// 12. get_mesh_catalog_stats
// ============================================================================

FMonolithActionResult FMonolithMeshInspectionActions::GetMeshCatalogStats(const TSharedPtr<FJsonObject>& Params)
{
	FSQLiteDatabase* DB = MeshInspectionHelpers::GetCatalogDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Mesh catalog not available — run monolith_reindex() first"));
	}

	return FMonolithActionResult::Success(FMonolithMeshCatalog::GetStats(*DB));
}
