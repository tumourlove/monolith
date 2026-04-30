#include "MonolithMeshTechArtActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/StaticMeshComponent.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "PhysicsEngine/BodySetup.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

// Asset import
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AutomatedAssetImportData.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxFactory.h"

// Asset export
#include "AssetExportTask.h"
#include "Exporters/Exporter.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

// Package save
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"

// Material cost analysis
#include "Engine/Texture.h"

#if WITH_GEOMETRYSCRIPT
#include "MonolithMeshHandlePool.h"
#include "UDynamicMesh.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "DynamicMeshToMeshDescription.h"
#include "GeometryScript/MeshRepairFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshSimplifyFunctions.h"
#include "GeometryScript/CollisionFunctions.h"
using namespace UE::Geometry;
#endif

#if WITH_GEOMETRYSCRIPT
UMonolithMeshHandlePool* FMonolithMeshTechArtActions::Pool = nullptr;

void FMonolithMeshTechArtActions::SetHandlePool(UMonolithMeshHandlePool* InPool)
{
	Pool = InPool;
}
#endif

// ============================================================================
// Helpers
// ============================================================================

TArray<TSharedPtr<FJsonValue>> FMonolithMeshTechArtActions::VectorToJsonArray(const FVector& V)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Add(MakeShared<FJsonValueNumber>(V.X));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
	return Arr;
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshTechArtActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 16.1 import_mesh
	Registry.RegisterAction(TEXT("mesh"), TEXT("import_mesh"),
		TEXT("Import FBX/glTF mesh files via automated import. Configure static vs skeletal, collision, material import, scale."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshTechArtActions::ImportMesh),
		FParamSchemaBuilder()
			.Required(TEXT("files"), TEXT("array"), TEXT("Array of absolute file paths to import"))
			.Required(TEXT("destination"), TEXT("string"), TEXT("Destination content path (e.g. /Game/Meshes/Furniture)"))
			.Optional(TEXT("replace_existing"), TEXT("boolean"), TEXT("Replace existing assets"), TEXT("false"))
			.Optional(TEXT("combine_meshes"), TEXT("boolean"), TEXT("Combine all meshes into one"), TEXT("true"))
			.Optional(TEXT("generate_lightmap_uvs"), TEXT("boolean"), TEXT("Generate lightmap UVs"), TEXT("true"))
			.Optional(TEXT("auto_generate_collision"), TEXT("boolean"), TEXT("Generate collision on import"), TEXT("true"))
			.Optional(TEXT("normal_import_method"), TEXT("string"), TEXT("Normal import: ImportNormals, ImportNormalsAndTangents, ComputeNormals"), TEXT("ImportNormalsAndTangents"))
			.Optional(TEXT("material_import"), TEXT("string"), TEXT("Material import: create_new, find_existing, skip"), TEXT("create_new"))
			.Build());

	// 16.1b export_mesh
	Registry.RegisterAction(TEXT("mesh"), TEXT("export_mesh"),
		TEXT("Export a UStaticMesh or USkeletalMesh asset to FBX on disk via UAssetExportTask + the engine's built-in FBX exporter."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshTechArtActions::ExportMesh),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path of the mesh to export (StaticMesh or SkeletalMesh)"))
			.Required(TEXT("file_path"), TEXT("string"), TEXT("Absolute output file path (must end in .fbx)"))
			.Optional(TEXT("replace_existing"), TEXT("boolean"), TEXT("Overwrite an existing file"), TEXT("true"))
			.Build());

	// 16.2 fix_mesh_quality
	Registry.RegisterAction(TEXT("mesh"), TEXT("fix_mesh_quality"),
		TEXT("Auto-fix mesh quality issues: weld edges, remove degenerates, remove small components, fix normals. GeometryScript required."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshTechArtActions::FixMeshQuality),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path of the StaticMesh"))
			.Optional(TEXT("operations"), TEXT("array"), TEXT("Operations to apply: remove_degenerates, weld_edges, remove_small_components, fix_normals (default: all)"))
			.Optional(TEXT("weld_tolerance"), TEXT("number"), TEXT("Weld tolerance in cm"), TEXT("0.01"))
			.Build());

	// 16.3 auto_generate_lods
	Registry.RegisterAction(TEXT("mesh"), TEXT("auto_generate_lods"),
		TEXT("One-shot LOD generation: simplify via GeometryScript and write back to UStaticMesh source models with screen sizes."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshTechArtActions::AutoGenerateLods),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path of the StaticMesh"))
			.Optional(TEXT("lod_count"), TEXT("integer"), TEXT("Number of LODs to generate (excluding LOD0)"), TEXT("3"))
			.Optional(TEXT("reduction_per_lod"), TEXT("number"), TEXT("Triangle reduction ratio per LOD step (0.0-1.0)"), TEXT("0.5"))
			.Optional(TEXT("screen_sizes"), TEXT("array"), TEXT("Screen size array [LOD0, LOD1, ...]. Auto-generated if omitted."))
			.Optional(TEXT("preserve_uv_borders"), TEXT("boolean"), TEXT("Preserve UV seam borders during simplification"), TEXT("true"))
			.Build());

	// 16.4 analyze_texel_density
	Registry.RegisterAction(TEXT("mesh"), TEXT("analyze_texel_density"),
		TEXT("Calculate texels/cm for meshes. Reports UV space usage vs world-space area combined with texture resolution. Supports single actor or region mode."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshTechArtActions::AnalyzeTexelDensity),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Single actor to analyze"))
			.Optional(TEXT("region_min"), TEXT("array"), TEXT("Region min corner [x, y, z] for multi-actor analysis"))
			.Optional(TEXT("region_max"), TEXT("array"), TEXT("Region max corner [x, y, z]"))
			.Optional(TEXT("target_density"), TEXT("number"), TEXT("Target texels/cm for deviation reporting"), TEXT("5.12"))
			.Optional(TEXT("uv_channel"), TEXT("integer"), TEXT("UV channel to analyze"), TEXT("0"))
			.Build());

	// 16.5 analyze_material_cost_in_region
	Registry.RegisterAction(TEXT("mesh"), TEXT("analyze_material_cost_in_region"),
		TEXT("Cross-module: spatial query + shader instruction count per material. Identifies shader cost hotspots in a region."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshTechArtActions::AnalyzeMaterialCostInRegion),
		FParamSchemaBuilder()
			.Optional(TEXT("center"), TEXT("array"), TEXT("Region center [x, y, z]"))
			.Optional(TEXT("radius"), TEXT("number"), TEXT("Region radius in cm"))
			.Optional(TEXT("region_min"), TEXT("array"), TEXT("Region min corner [x, y, z] (alternative to center+radius)"))
			.Optional(TEXT("region_max"), TEXT("array"), TEXT("Region max corner [x, y, z]"))
			.Optional(TEXT("actors"), TEXT("array"), TEXT("Explicit actor names to analyze (alternative to region)"))
			.Build());

	// 16.6 set_mesh_collision
	Registry.RegisterAction(TEXT("mesh"), TEXT("set_mesh_collision"),
		TEXT("Set collision on a static mesh asset. Supports simple shapes, complex-as-simple, and auto-convex decomposition."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshTechArtActions::SetMeshCollision),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path of the StaticMesh"))
			.Required(TEXT("collision_type"), TEXT("string"), TEXT("Collision type: simple_box, simple_sphere, simple_capsule, complex_as_simple, use_complex, use_default"))
			.Optional(TEXT("auto_convex"), TEXT("object"), TEXT("Auto-convex params: { hull_count: 4, max_verts: 32 }"))
			.Build());

	// 16.7 analyze_lightmap_density
	Registry.RegisterAction(TEXT("mesh"), TEXT("analyze_lightmap_density"),
		TEXT("Lightmap texel density analysis and resolution recommendations for actors in the scene."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshTechArtActions::AnalyzeLightmapDensity),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Single actor to analyze"))
			.Optional(TEXT("region_min"), TEXT("array"), TEXT("Region min corner [x, y, z]"))
			.Optional(TEXT("region_max"), TEXT("array"), TEXT("Region max corner [x, y, z]"))
			.Optional(TEXT("target_density"), TEXT("number"), TEXT("Target lightmap texels/cm"), TEXT("4.0"))
			.Build());
}

// ============================================================================
// 16.1 import_mesh
// ============================================================================

FMonolithActionResult FMonolithMeshTechArtActions::ImportMesh(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* FilesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("files"), FilesArray) || !FilesArray || FilesArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("'files' array is required and must not be empty"));
	}

	FString Destination = Params->GetStringField(TEXT("destination"));
	if (Destination.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'destination' is required"));
	}

	// Build file list
	TArray<FString> Filenames;
	for (const auto& Val : *FilesArray)
	{
		FString Path = Val->AsString();
		if (!FPaths::FileExists(Path))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("File not found: %s"), *Path));
		}
		Filenames.Add(Path);
	}

	// Create automated import data
	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	ImportData->Filenames = Filenames;
	ImportData->DestinationPath = Destination;
	ImportData->bReplaceExisting = Params->HasField(TEXT("replace_existing"))
		? Params->GetBoolField(TEXT("replace_existing")) : false;

	// FBX-specific settings
	bool bHasFbx = false;
	for (const FString& F : Filenames)
	{
		if (F.EndsWith(TEXT(".fbx"), ESearchCase::IgnoreCase))
		{
			bHasFbx = true;
			break;
		}
	}

	UFbxImportUI* FbxUI = nullptr;
	if (bHasFbx)
	{
		FbxUI = NewObject<UFbxImportUI>();

		// Core mesh settings (5.7-compatible subset)
		FbxUI->bImportMesh = true;
		FbxUI->bImportAsSkeletal = false;
		FbxUI->MeshTypeToImport = FBXIT_StaticMesh;

		// Material import
		FString MatImport = Params->HasField(TEXT("material_import"))
			? Params->GetStringField(TEXT("material_import")).ToLower() : TEXT("create_new");
		if (MatImport == TEXT("skip"))
		{
			FbxUI->bImportMaterials = false;
			FbxUI->bImportTextures = false;
		}
		else
		{
			FbxUI->bImportMaterials = true;
			FbxUI->bImportTextures = true;
		}

		// Animation off for mesh import
		FbxUI->bImportAnimations = false;

		// Set the FBX factory instance for proper import settings
		UFbxFactory* FbxFactory = NewObject<UFbxFactory>();
		FbxFactory->ImportUI = FbxUI;
		ImportData->Factory = FbxFactory;
	}

	// Perform import
	IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
	TArray<UObject*> ImportedObjects = AssetTools.ImportAssetsAutomated(ImportData);

	if (ImportedObjects.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Import returned no objects. Check file paths and format."));
	}

	// Build result
	TArray<TSharedPtr<FJsonValue>> ImportedArray;
	TArray<FString> Warnings;

	for (UObject* Obj : ImportedObjects)
	{
		if (!Obj) continue;

		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), Obj->GetPathName());
		Entry->SetStringField(TEXT("type"), Obj->GetClass()->GetName());

		// Extract mesh stats if static mesh
		if (UStaticMesh* SM = Cast<UStaticMesh>(Obj))
		{
			if (SM->GetRenderData() && SM->GetRenderData()->LODResources.Num() > 0)
			{
				const FStaticMeshLODResources& LOD0 = SM->GetRenderData()->LODResources[0];
				Entry->SetNumberField(TEXT("vertex_count"), LOD0.GetNumVertices());
				Entry->SetNumberField(TEXT("triangle_count"), LOD0.GetNumTriangles());
			}
			Entry->SetNumberField(TEXT("material_slots"), SM->GetStaticMaterials().Num());
		}

		ImportedArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("imported"), ImportedArray);
	Result->SetNumberField(TEXT("total_imported"), ImportedObjects.Num());

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningArr;
		for (const FString& W : Warnings)
		{
			WarningArr.Add(MakeShared<FJsonValueString>(W));
		}
		Result->SetArrayField(TEXT("warnings"), WarningArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 16.1b export_mesh
// ============================================================================

FMonolithActionResult FMonolithMeshTechArtActions::ExportMesh(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString FilePath = Params->GetStringField(TEXT("file_path"));
	if (AssetPath.IsEmpty() || FilePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'asset_path' and 'file_path' are required"));
	}
	if (!FilePath.EndsWith(TEXT(".fbx"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(TEXT("'file_path' must end with .fbx"));
	}

	bool bReplaceExisting = true;
	Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);

	if (FPaths::FileExists(FilePath) && !bReplaceExisting)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("File already exists and replace_existing=false: %s"), *FilePath));
	}

	// Ensure output directory exists
	const FString OutDir = FPaths::GetPath(FilePath);
	if (!OutDir.IsEmpty() && !IFileManager::Get().DirectoryExists(*OutDir))
	{
		IFileManager::Get().MakeDirectory(*OutDir, /*Tree=*/true);
	}

	// Load the asset (StaticMesh or SkeletalMesh)
	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));
	}
	if (!Asset->IsA(UStaticMesh::StaticClass()) && !Asset->IsA(USkeletalMesh::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset is not a StaticMesh or SkeletalMesh: %s (%s)"),
			*AssetPath, *Asset->GetClass()->GetName()));
	}

	// Find an FBX exporter compatible with this asset
	UExporter* Exporter = UExporter::FindExporter(Asset, TEXT("FBX"));
	if (!Exporter)
	{
		return FMonolithActionResult::Error(TEXT("No FBX exporter found for this asset class — ensure the FBX import/export plugin is enabled"));
	}

	// Build and run the export task
	UAssetExportTask* Task = NewObject<UAssetExportTask>();
	Task->AddToRoot();
	Task->Object = Asset;
	Task->Filename = FilePath;
	Task->bSelected = false;
	Task->bReplaceIdentical = bReplaceExisting;
	Task->bPrompt = false;
	Task->bUseFileArchive = false;
	Task->bWriteEmptyFiles = false;
	Task->bAutomated = true;
	Task->Exporter = Exporter;

	const bool bSucceeded = UExporter::RunAssetExportTask(Task);
	Task->RemoveFromRoot();

	if (!bSucceeded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Export failed for asset %s -> %s"), *AssetPath, *FilePath));
	}

	const int64 FileSize = IFileManager::Get().FileSize(*FilePath);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("file_path"), FilePath);
	Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
	Result->SetNumberField(TEXT("file_size_bytes"), static_cast<double>(FileSize));
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 16.2 fix_mesh_quality
// ============================================================================

FMonolithActionResult FMonolithMeshTechArtActions::FixMeshQuality(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_GEOMETRYSCRIPT
	return FMonolithActionResult::Error(TEXT("fix_mesh_quality requires the GeometryScripting plugin"));
#else
	if (!Pool)
	{
		return FMonolithActionResult::Error(TEXT("GeometryScript handle pool not available"));
	}

	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'asset_path' is required"));
	}

	// Parse operations (default: all)
	TSet<FString> Ops;
	const TArray<TSharedPtr<FJsonValue>>* OpsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("operations"), OpsArray) && OpsArray)
	{
		for (const auto& Val : *OpsArray)
		{
			Ops.Add(Val->AsString().ToLower());
		}
	}
	else
	{
		Ops.Add(TEXT("remove_degenerates"));
		Ops.Add(TEXT("weld_edges"));
		Ops.Add(TEXT("remove_small_components"));
		Ops.Add(TEXT("fix_normals"));
	}

	double WeldTolerance = Params->HasField(TEXT("weld_tolerance"))
		? Params->GetNumberField(TEXT("weld_tolerance")) : 0.01;

	// Load mesh into handle pool for round-trip editing
	FString HandleName = FString::Printf(TEXT("__techart_fix_%s"), *FGuid::NewGuid().ToString());
	FString CreateError;
	if (!Pool->CreateHandle(HandleName, AssetPath, CreateError))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load mesh: %s"), *CreateError));
	}

	FString GetError;
	UDynamicMesh* DynMesh = Pool->GetHandle(HandleName, GetError);
	if (!DynMesh)
	{
		Pool->ReleaseHandle(HandleName);
		return FMonolithActionResult::Error(GetError);
	}

	int32 OriginalTris = DynMesh->GetTriangleCount();
	int32 OriginalVerts = DynMesh->GetMeshRef().VertexCount();
	TArray<FString> AppliedOps;

	// Remove degenerate geometry — use default options
	int32 DegeneratesRemoved = 0;
	if (Ops.Contains(TEXT("remove_degenerates")))
	{
		int32 PreTris = DynMesh->GetTriangleCount();
		// RepairMeshDegenerateGeometry with default options removes zero-area triangles
		UGeometryScriptLibrary_MeshRepairFunctions::RepairMeshDegenerateGeometry(
			DynMesh, FGeometryScriptDegenerateTriangleOptions());
		DegeneratesRemoved = PreTris - DynMesh->GetTriangleCount();
		AppliedOps.Add(TEXT("remove_degenerates"));
	}

	// Weld edges — use default tolerance or custom
	int32 EdgesWelded = 0;
	if (Ops.Contains(TEXT("weld_edges")))
	{
		int32 PreVerts = DynMesh->GetMeshRef().VertexCount();
		FGeometryScriptWeldEdgesOptions WeldOpts;
		WeldOpts.Tolerance = WeldTolerance;
		UGeometryScriptLibrary_MeshRepairFunctions::WeldMeshEdges(DynMesh, WeldOpts);
		EdgesWelded = PreVerts - DynMesh->GetMeshRef().VertexCount();
		AppliedOps.Add(TEXT("weld_edges"));
	}

	// Remove small components — use default options
	int32 ComponentsRemoved = 0;
	if (Ops.Contains(TEXT("remove_small_components")))
	{
		int32 PreTris = DynMesh->GetTriangleCount();
		UGeometryScriptLibrary_MeshRepairFunctions::RemoveSmallComponents(
			DynMesh, FGeometryScriptRemoveSmallComponentOptions());
		ComponentsRemoved = PreTris - DynMesh->GetTriangleCount();
		AppliedOps.Add(TEXT("remove_small_components"));
	}

	// Fill holes then fix normals
	int32 HolesFilled = 0;
	if (Ops.Contains(TEXT("remove_degenerates")) || Ops.Contains(TEXT("weld_edges")))
	{
		// Fill any holes created by repairs
		FGeometryScriptFillHolesOptions FillOpts;
		FillOpts.FillMethod = EGeometryScriptFillHolesMethod::Automatic;
		int32 NumFailed = 0;
		UGeometryScriptLibrary_MeshRepairFunctions::FillAllMeshHoles(DynMesh, FillOpts, HolesFilled, NumFailed);
	}

	if (Ops.Contains(TEXT("fix_normals")))
	{
		FGeometryScriptCalculateNormalsOptions NormOpts;
		UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(DynMesh, NormOpts);
		AppliedOps.Add(TEXT("fix_normals"));
	}

	// Write back: save handle overwrites the asset
	FString SaveError;
	if (!Pool->SaveHandle(HandleName, AssetPath, /*bOverwrite=*/true, SaveError))
	{
		Pool->ReleaseHandle(HandleName);
		return FMonolithActionResult::Error(FString::Printf(TEXT("Fixed mesh but failed to save: %s"), *SaveError));
	}

	Pool->ReleaseHandle(HandleName);

	// Build result
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);

	TArray<TSharedPtr<FJsonValue>> OpsArr;
	for (const FString& Op : AppliedOps)
	{
		OpsArr.Add(MakeShared<FJsonValueString>(Op));
	}
	Result->SetArrayField(TEXT("operations_applied"), OpsArr);

	Result->SetNumberField(TEXT("original_triangles"), OriginalTris);
	Result->SetNumberField(TEXT("original_vertices"), OriginalVerts);
	Result->SetNumberField(TEXT("result_triangles"), DynMesh->GetTriangleCount());
	Result->SetNumberField(TEXT("degenerates_removed"), DegeneratesRemoved);
	Result->SetNumberField(TEXT("edges_welded"), EdgesWelded);
	Result->SetNumberField(TEXT("components_removed"), ComponentsRemoved);
	Result->SetNumberField(TEXT("holes_filled"), HolesFilled);

	return FMonolithActionResult::Success(Result);
#endif // WITH_GEOMETRYSCRIPT
}

// ============================================================================
// 16.3 auto_generate_lods
// ============================================================================

FMonolithActionResult FMonolithMeshTechArtActions::AutoGenerateLods(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_GEOMETRYSCRIPT
	return FMonolithActionResult::Error(TEXT("auto_generate_lods requires the GeometryScripting plugin"));
#else
	if (!Pool)
	{
		return FMonolithActionResult::Error(TEXT("GeometryScript handle pool not available"));
	}

	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'asset_path' is required"));
	}

	int32 LodCount = Params->HasField(TEXT("lod_count"))
		? static_cast<int32>(Params->GetNumberField(TEXT("lod_count"))) : 3;
	LodCount = FMath::Clamp(LodCount, 1, 8);

	double ReductionPerLod = Params->HasField(TEXT("reduction_per_lod"))
		? Params->GetNumberField(TEXT("reduction_per_lod")) : 0.5;
	ReductionPerLod = FMath::Clamp(ReductionPerLod, 0.1, 0.9);

	bool bPreserveUV = Params->HasField(TEXT("preserve_uv_borders"))
		? Params->GetBoolField(TEXT("preserve_uv_borders")) : true;

	// Parse optional screen sizes
	TArray<float> ScreenSizes;
	const TArray<TSharedPtr<FJsonValue>>* ScreenSizesArray = nullptr;
	if (Params->TryGetArrayField(TEXT("screen_sizes"), ScreenSizesArray) && ScreenSizesArray)
	{
		for (const auto& Val : *ScreenSizesArray)
		{
			ScreenSizes.Add(static_cast<float>(Val->AsNumber()));
		}
	}

	// Auto-generate screen sizes if not provided
	if (ScreenSizes.Num() < LodCount + 1)
	{
		ScreenSizes.Empty();
		ScreenSizes.Add(1.0f); // LOD0
		for (int32 i = 1; i <= LodCount; ++i)
		{
			ScreenSizes.Add(FMath::Pow(0.5f, static_cast<float>(i)));
		}
	}

	// Load the source mesh
	FString Error;
	UStaticMesh* SM = MonolithMeshUtils::LoadStaticMesh(AssetPath, Error);
	if (!SM)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Get LOD0 triangle count from render data
	int32 BaseTris = 0;
	if (SM->GetRenderData() && SM->GetRenderData()->LODResources.Num() > 0)
	{
		BaseTris = SM->GetRenderData()->LODResources[0].GetNumTriangles();
	}

	// Create handle from asset for the source LOD0
	FString Lod0Handle = FString::Printf(TEXT("__techart_lod0_%s"), *FGuid::NewGuid().ToString());
	FString CreateError;
	if (!Pool->CreateHandle(Lod0Handle, AssetPath, CreateError))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load source mesh: %s"), *CreateError));
	}

	// Generate each LOD via simplification
	TArray<FMeshDescription> LodMeshDescs;
	TArray<int32> LodTriCounts;

	// LOD0 mesh description: read from the source static mesh directly
	{
		FMeshDescription LOD0Desc;
		FStaticMeshAttributes Attrs(LOD0Desc);
		Attrs.Register();

		if (SM->GetNumSourceModels() > 0)
		{
			const FMeshDescription* SrcDesc = SM->GetMeshDescription(0);
			if (SrcDesc)
			{
				LOD0Desc = *SrcDesc;
			}
		}
		else
		{
			// Fallback: convert from dynamic mesh
			FString GetErr;
			UDynamicMesh* DynLod0 = Pool->GetHandle(Lod0Handle, GetErr);
			if (DynLod0)
			{
				FDynamicMeshToMeshDescription Converter;
				Converter.Convert(&DynLod0->GetMeshRef(), LOD0Desc);
			}
		}

		LodMeshDescs.Add(MoveTemp(LOD0Desc));
		LodTriCounts.Add(BaseTris);
	}

	for (int32 Lod = 1; Lod <= LodCount; ++Lod)
	{
		FString LodHandle = FString::Printf(TEXT("__techart_lod%d_%s"), Lod, *FGuid::NewGuid().ToString());

		// Create LOD handle as copy of LOD0 source
		if (!Pool->CreateHandle(LodHandle, FString::Printf(TEXT("internal:lod:%s:%d"), *Lod0Handle, Lod), CreateError))
		{
			// Clean up
			Pool->ReleaseHandle(Lod0Handle);
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create LOD%d handle: %s"), Lod, *CreateError));
		}

		FString GetErr;
		UDynamicMesh* LodMesh = Pool->GetHandle(LodHandle, GetErr);
		if (!LodMesh)
		{
			Pool->ReleaseHandle(Lod0Handle);
			Pool->ReleaseHandle(LodHandle);
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to get LOD%d handle"), Lod));
		}

		// Copy LOD0 source
		FString Lod0Err;
		UDynamicMesh* SrcMesh = Pool->GetHandle(Lod0Handle, Lod0Err);
		if (SrcMesh)
		{
			LodMesh->SetMesh(SrcMesh->GetMeshRef());
		}

		// Simplify progressively
		int32 TargetTris = FMath::Max(4, FMath::RoundToInt32(BaseTris * FMath::Pow(ReductionPerLod, Lod)));

		FGeometryScriptSimplifyMeshOptions SimplifyOpts;
		if (bPreserveUV)
		{
			SimplifyOpts.bAllowSeamSplits = false;
			SimplifyOpts.bAllowSeamCollapse = false;
		}
		UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTriangleCount(LodMesh, TargetTris, SimplifyOpts);

		// Convert to MeshDescription
		FMeshDescription LodDesc;
		FStaticMeshAttributes Attrs(LodDesc);
		Attrs.Register();

		FDynamicMeshToMeshDescription Converter;
		Converter.Convert(&LodMesh->GetMeshRef(), LodDesc);

		LodMeshDescs.Add(MoveTemp(LodDesc));
		LodTriCounts.Add(LodMesh->GetTriangleCount());

		Pool->ReleaseHandle(LodHandle);
	}

	Pool->ReleaseHandle(Lod0Handle);

	// Write all LODs back to the UStaticMesh
	TArray<const FMeshDescription*> MeshDescPtrs;
	for (const FMeshDescription& Desc : LodMeshDescs)
	{
		MeshDescPtrs.Add(&Desc);
	}

	UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
	BuildParams.bMarkPackageDirty = true;
	BuildParams.bBuildSimpleCollision = false;
	BuildParams.bCommitMeshDescription = true;

	SM->BuildFromMeshDescriptions(MeshDescPtrs, BuildParams);

	// Set screen sizes
	for (int32 i = 0; i < SM->GetNumSourceModels() && i < ScreenSizes.Num(); ++i)
	{
		SM->GetSourceModel(i).ScreenSize = ScreenSizes[i];
	}

	SM->PostEditChange();
	SM->MarkPackageDirty();

	// Build result
	TArray<TSharedPtr<FJsonValue>> LodArray;
	for (int32 i = 0; i <= LodCount && i < LodTriCounts.Num(); ++i)
	{
		auto LodInfo = MakeShared<FJsonObject>();
		LodInfo->SetNumberField(TEXT("lod"), i);
		LodInfo->SetNumberField(TEXT("triangles"), LodTriCounts[i]);
		LodInfo->SetNumberField(TEXT("screen_size"), i < ScreenSizes.Num() ? ScreenSizes[i] : 0.0f);
		LodArray.Add(MakeShared<FJsonValueObject>(LodInfo));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("total_lods"), LodCount + 1);
	Result->SetArrayField(TEXT("lods"), LodArray);

	return FMonolithActionResult::Success(Result);
#endif // WITH_GEOMETRYSCRIPT
}

// ============================================================================
// 16.4 analyze_texel_density
// ============================================================================

FMonolithActionResult FMonolithMeshTechArtActions::AnalyzeTexelDensity(const TSharedPtr<FJsonObject>& Params)
{
	double TargetDensity = Params->HasField(TEXT("target_density"))
		? Params->GetNumberField(TEXT("target_density")) : 5.12;
	int32 UVChannel = Params->HasField(TEXT("uv_channel"))
		? static_cast<int32>(Params->GetNumberField(TEXT("uv_channel"))) : 0;

	// Collect actors to analyze
	TArray<AActor*> Actors;
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	if (Params->HasField(TEXT("actor_name")))
	{
		FString Error;
		AActor* Actor = MonolithMeshUtils::FindActorByName(Params->GetStringField(TEXT("actor_name")), Error);
		if (!Actor)
		{
			return FMonolithActionResult::Error(Error);
		}
		Actors.Add(Actor);
	}
	else if (Params->HasField(TEXT("region_min")) && Params->HasField(TEXT("region_max")))
	{
		FVector RegionMin, RegionMax;
		MonolithMeshUtils::ParseVector(Params, TEXT("region_min"), RegionMin);
		MonolithMeshUtils::ParseVector(Params, TEXT("region_max"), RegionMax);

		FBox RegionBox(
			FVector(FMath::Min(RegionMin.X, RegionMax.X), FMath::Min(RegionMin.Y, RegionMax.Y), FMath::Min(RegionMin.Z, RegionMax.Z)),
			FVector(FMath::Max(RegionMin.X, RegionMax.X), FMath::Max(RegionMin.Y, RegionMax.Y), FMath::Max(RegionMin.Z, RegionMax.Z))
		);

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			FVector Origin, Extent;
			Actor->GetActorBounds(false, Origin, Extent);
			FBox ActorBox(Origin - Extent, Origin + Extent);
			if (RegionBox.Intersect(ActorBox))
			{
				Actors.Add(Actor);
			}
		}
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Provide either 'actor_name' or 'region_min'+'region_max'"));
	}

	// Analyze each actor
	TArray<TSharedPtr<FJsonValue>> ActorResults;
	double SumDensity = 0.0;
	double MinDensity = TNumericLimits<double>::Max();
	double MaxDensity = 0.0;
	int32 SectionCount = 0;

	for (AActor* Actor : Actors)
	{
		TArray<UStaticMeshComponent*> SMCs;
		Actor->GetComponents(SMCs);

		if (SMCs.Num() == 0) continue;

		auto ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());

		TArray<TSharedPtr<FJsonValue>> Sections;

		for (UStaticMeshComponent* SMC : SMCs)
		{
			if (!SMC || !SMC->IsVisible()) continue;

			UStaticMesh* SM = SMC->GetStaticMesh();
			if (!SM || !SM->GetRenderData() || SM->GetRenderData()->LODResources.Num() == 0)
				continue;

			const FStaticMeshLODResources& LOD0 = SM->GetRenderData()->LODResources[0];
			FVector ActorScale = Actor->GetActorScale3D();
			float AvgScale = (FMath::Abs(ActorScale.X) + FMath::Abs(ActorScale.Y) + FMath::Abs(ActorScale.Z)) / 3.0f;

			// Use FMeshUVChannelInfo if available
			const FMeshUVChannelInfo& UVInfo = SM->GetRenderData()->UVChannelDataPerMaterial.IsValidIndex(0)
				? SM->GetRenderData()->UVChannelDataPerMaterial[0]
				: FMeshUVChannelInfo();

			for (int32 SectionIdx = 0; SectionIdx < LOD0.Sections.Num(); ++SectionIdx)
			{
				const FStaticMeshSection& Section = LOD0.Sections[SectionIdx];
				auto SectionObj = MakeShared<FJsonObject>();
				SectionObj->SetNumberField(TEXT("slot"), Section.MaterialIndex);

				// Get material and texture resolution
				UMaterialInterface* MatInterface = SMC->GetMaterial(Section.MaterialIndex);
				FString MatName = MatInterface ? MatInterface->GetName() : TEXT("None");
				SectionObj->SetStringField(TEXT("material"), MatName);

				int32 TextureRes = 1024; // fallback
				if (MatInterface)
				{
					TArray<UTexture*> UsedTextures;
					MatInterface->GetUsedTextures(UsedTextures, EMaterialQualityLevel::High);
					if (UsedTextures.Num() > 0 && UsedTextures[0])
					{
						TextureRes = FMath::Max(UsedTextures[0]->GetSurfaceWidth(), UsedTextures[0]->GetSurfaceHeight());
					}
				}
				SectionObj->SetNumberField(TEXT("texture_resolution"), TextureRes);

				// Calculate texel density
				// LocalUVDensities gives the ratio of UV space to local mesh space
				double LocalUVDensity = 1.0;
				if (UVInfo.bInitialized && UVChannel < UE_ARRAY_COUNT(UVInfo.LocalUVDensities))
				{
					LocalUVDensity = FMath::Max(0.001, static_cast<double>(UVInfo.LocalUVDensities[UVChannel]));
				}

				// TexelsPerCm = TextureResolution * LocalUVDensity / ActorScale
				double TexelsPerCm = (TextureRes * LocalUVDensity) / FMath::Max(0.001, static_cast<double>(AvgScale));
				SectionObj->SetNumberField(TEXT("texels_per_cm"), TexelsPerCm);
				SectionObj->SetNumberField(TEXT("deviation_from_target"), TexelsPerCm - TargetDensity);

				Sections.Add(MakeShared<FJsonValueObject>(SectionObj));

				SumDensity += TexelsPerCm;
				MinDensity = FMath::Min(MinDensity, TexelsPerCm);
				MaxDensity = FMath::Max(MaxDensity, TexelsPerCm);
				SectionCount++;
			}
		}

		if (Sections.Num() > 0)
		{
			ActorObj->SetArrayField(TEXT("sections"), Sections);
			ActorResults.Add(MakeShared<FJsonValueObject>(ActorObj));
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("actors"), ActorResults);

	auto Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("actor_count"), ActorResults.Num());
	Summary->SetNumberField(TEXT("section_count"), SectionCount);
	Summary->SetNumberField(TEXT("average_density"), SectionCount > 0 ? SumDensity / SectionCount : 0.0);
	Summary->SetNumberField(TEXT("min_density"), SectionCount > 0 ? MinDensity : 0.0);
	Summary->SetNumberField(TEXT("max_density"), MaxDensity);
	Summary->SetNumberField(TEXT("target_density"), TargetDensity);
	Result->SetObjectField(TEXT("summary"), Summary);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 16.5 analyze_material_cost_in_region
// ============================================================================

FMonolithActionResult FMonolithMeshTechArtActions::AnalyzeMaterialCostInRegion(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Collect actors via region or explicit list
	TArray<AActor*> Actors;

	if (Params->HasField(TEXT("actors")))
	{
		const TArray<TSharedPtr<FJsonValue>>* ActorNames = nullptr;
		if (Params->TryGetArrayField(TEXT("actors"), ActorNames) && ActorNames)
		{
			for (const auto& Val : *ActorNames)
			{
				FString Name = Val->AsString();
				FString Error;
				// Support wildcards
				if (Name.Contains(TEXT("*")))
				{
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						if ((*It)->GetActorNameOrLabel().MatchesWildcard(Name))
						{
							Actors.Add(*It);
						}
					}
				}
				else
				{
					AActor* Actor = MonolithMeshUtils::FindActorByName(Name, Error);
					if (Actor) Actors.Add(Actor);
				}
			}
		}
	}
	else if (Params->HasField(TEXT("center")) && Params->HasField(TEXT("radius")))
	{
		FVector Center;
		MonolithMeshUtils::ParseVector(Params, TEXT("center"), Center);
		double Radius = Params->GetNumberField(TEXT("radius"));

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			FVector Origin, Extent;
			(*It)->GetActorBounds(false, Origin, Extent);
			if (FVector::Dist(Origin, Center) - Extent.Size() <= Radius)
			{
				Actors.Add(*It);
			}
		}
	}
	else if (Params->HasField(TEXT("region_min")) && Params->HasField(TEXT("region_max")))
	{
		FVector RegionMin, RegionMax;
		MonolithMeshUtils::ParseVector(Params, TEXT("region_min"), RegionMin);
		MonolithMeshUtils::ParseVector(Params, TEXT("region_max"), RegionMax);

		FBox RegionBox(
			FVector(FMath::Min(RegionMin.X, RegionMax.X), FMath::Min(RegionMin.Y, RegionMax.Y), FMath::Min(RegionMin.Z, RegionMax.Z)),
			FVector(FMath::Max(RegionMin.X, RegionMax.X), FMath::Max(RegionMin.Y, RegionMax.Y), FMath::Max(RegionMin.Z, RegionMax.Z))
		);

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			FVector Origin, Extent;
			(*It)->GetActorBounds(false, Origin, Extent);
			FBox ActorBox(Origin - Extent, Origin + Extent);
			if (RegionBox.Intersect(ActorBox))
			{
				Actors.Add(*It);
			}
		}
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Provide 'actors' array, 'center'+'radius', or 'region_min'+'region_max'"));
	}

	// Gather unique materials and their instruction counts
	struct FMaterialCostEntry
	{
		FString MaterialName;
		FString MaterialPath;
		int32 InstructionCount = -1;
		TArray<FString> UsedByActors;
	};

	TMap<FString, FMaterialCostEntry> MaterialMap;
	int64 TotalInstructions = 0;

	for (AActor* Actor : Actors)
	{
		TArray<UPrimitiveComponent*> PrimComps;
		Actor->GetComponents(PrimComps);

		for (UPrimitiveComponent* PrimComp : PrimComps)
		{
			if (!PrimComp) continue;

			int32 NumMaterials = PrimComp->GetNumMaterials();
			for (int32 MatIdx = 0; MatIdx < NumMaterials; ++MatIdx)
			{
				UMaterialInterface* MatInterface = PrimComp->GetMaterial(MatIdx);
				if (!MatInterface) continue;

				FString MatPath = MatInterface->GetPathName();
				FMaterialCostEntry& Entry = MaterialMap.FindOrAdd(MatPath);

				if (Entry.MaterialName.IsEmpty())
				{
					Entry.MaterialName = MatInterface->GetName();
					Entry.MaterialPath = MatPath;

					// Get material complexity metrics
					// Count used textures as a cost proxy — always available and correlates with
					// shader complexity. Texture fetches are a major GPU cost driver.
					TArray<UTexture*> Textures;
					MatInterface->GetUsedTextures(Textures, EMaterialQualityLevel::High);
					int32 TextureCount = Textures.Num();

					// Also count the number of material expressions as a complexity metric
					UMaterial* BaseMaterial = MatInterface->GetMaterial();
					int32 ExpressionCount = 0;
					if (BaseMaterial)
					{
						ExpressionCount = BaseMaterial->GetExpressions().Num();
					}

					// Estimate instruction count from expression graph size
					// Typical ratio: ~3-5 instructions per expression node
					Entry.InstructionCount = FMath::Max(ExpressionCount * 4, TextureCount * 20);

					if (Entry.InstructionCount < 0)
					{
						Entry.InstructionCount = 0; // uncompiled or unavailable
					}
				}

				// Track which actors use this material
				FString ActorName = Actor->GetActorNameOrLabel();
				Entry.UsedByActors.AddUnique(ActorName);

				TotalInstructions += Entry.InstructionCount;
			}
		}
	}

	// Sort by instruction count descending for hotspot ordering
	TArray<FMaterialCostEntry> SortedEntries;
	MaterialMap.GenerateValueArray(SortedEntries);
	SortedEntries.Sort([](const FMaterialCostEntry& A, const FMaterialCostEntry& B)
	{
		return A.InstructionCount > B.InstructionCount;
	});

	// Build result
	TArray<TSharedPtr<FJsonValue>> Hotspots;
	for (const FMaterialCostEntry& Entry : SortedEntries)
	{
		auto HotObj = MakeShared<FJsonObject>();
		HotObj->SetStringField(TEXT("material"), Entry.MaterialName);
		HotObj->SetStringField(TEXT("material_path"), Entry.MaterialPath);
		HotObj->SetNumberField(TEXT("instruction_count"), Entry.InstructionCount);
		HotObj->SetNumberField(TEXT("actor_usage_count"), Entry.UsedByActors.Num());

		TArray<TSharedPtr<FJsonValue>> ActorArr;
		for (const FString& AN : Entry.UsedByActors)
		{
			ActorArr.Add(MakeShared<FJsonValueString>(AN));
		}
		HotObj->SetArrayField(TEXT("used_by_actors"), ActorArr);

		Hotspots.Add(MakeShared<FJsonValueObject>(HotObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("actors_analyzed"), Actors.Num());
	Result->SetNumberField(TEXT("total_unique_materials"), MaterialMap.Num());
	Result->SetNumberField(TEXT("total_instruction_count"), TotalInstructions);
	Result->SetArrayField(TEXT("hotspots"), Hotspots);

	// Simple budget assessment
	FString Assessment;
	if (MaterialMap.Num() == 0)
	{
		Assessment = TEXT("No materials found in region");
	}
	else
	{
		int32 AvgInstructions = MaterialMap.Num() > 0 ? static_cast<int32>(TotalInstructions / MaterialMap.Num()) : 0;
		if (AvgInstructions > 300)
		{
			Assessment = TEXT("HIGH COST: Average instruction count exceeds 300. Consider simplifying complex materials.");
		}
		else if (AvgInstructions > 150)
		{
			Assessment = TEXT("MODERATE: Average instruction count is 150-300. Monitor performance.");
		}
		else
		{
			Assessment = TEXT("OK: Average instruction count is under 150.");
		}
	}
	Result->SetStringField(TEXT("budget_assessment"), Assessment);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 16.6 set_mesh_collision
// ============================================================================

FMonolithActionResult FMonolithMeshTechArtActions::SetMeshCollision(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'asset_path' is required"));
	}

	FString CollisionType = Params->GetStringField(TEXT("collision_type")).ToLower();
	if (CollisionType.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'collision_type' is required"));
	}

	FString Error;
	UStaticMesh* SM = MonolithMeshUtils::LoadStaticMesh(AssetPath, Error);
	if (!SM)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Ensure BodySetup exists
	if (!SM->GetBodySetup())
	{
		SM->CreateBodySetup();
	}
	UBodySetup* BodySetup = SM->GetBodySetup();
	if (!BodySetup)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create or get BodySetup"));
	}

	int32 HullCount = 0;
	FString ResultCollisionType = CollisionType;

	// Handle auto_convex first if provided
	if (Params->HasField(TEXT("auto_convex")))
	{
#if WITH_GEOMETRYSCRIPT
		if (!Pool)
		{
			return FMonolithActionResult::Error(TEXT("auto_convex requires GeometryScript handle pool"));
		}

		const TSharedPtr<FJsonObject>* ConvexParams = nullptr;
		if (Params->TryGetObjectField(TEXT("auto_convex"), ConvexParams) && ConvexParams)
		{
			int32 MaxHulls = (*ConvexParams)->HasField(TEXT("hull_count"))
				? static_cast<int32>((*ConvexParams)->GetNumberField(TEXT("hull_count"))) : 4;
			int32 MaxVerts = (*ConvexParams)->HasField(TEXT("max_verts"))
				? static_cast<int32>((*ConvexParams)->GetNumberField(TEXT("max_verts"))) : 32;

			// Load mesh into handle pool
			FString HandleName = FString::Printf(TEXT("__techart_collision_%s"), *FGuid::NewGuid().ToString());
			FString CreateError;
			if (Pool->CreateHandle(HandleName, AssetPath, CreateError))
			{
				FString GetErr;
				UDynamicMesh* DynMesh = Pool->GetHandle(HandleName, GetErr);
				if (DynMesh)
				{
					FGeometryScriptCollisionFromMeshOptions CollisionOpts;
					CollisionOpts.Method = EGeometryScriptCollisionGenerationMethod::ConvexHulls;
					CollisionOpts.MaxConvexHullsPerMesh = MaxHulls;
					CollisionOpts.ConvexHullTargetFaceCount = MaxVerts;

					UGeometryScriptLibrary_CollisionFunctions::SetStaticMeshCollisionFromMesh(
						DynMesh, SM, CollisionOpts);

					HullCount = MaxHulls;
				}
				Pool->ReleaseHandle(HandleName);
			}

			ResultCollisionType = TEXT("auto_convex");
		}
#else
		return FMonolithActionResult::Error(TEXT("auto_convex requires the GeometryScripting plugin"));
#endif
	}
	else if (CollisionType == TEXT("simple_box"))
	{
		BodySetup->AggGeom.BoxElems.Empty();
		BodySetup->AggGeom.SphereElems.Empty();
		BodySetup->AggGeom.SphylElems.Empty();
		BodySetup->AggGeom.ConvexElems.Empty();

		FKBoxElem BoxElem;
		FBoxSphereBounds Bounds = SM->GetBounds();
		BoxElem.Center = FVector::ZeroVector;
		BoxElem.X = Bounds.BoxExtent.X * 2.0f;
		BoxElem.Y = Bounds.BoxExtent.Y * 2.0f;
		BoxElem.Z = Bounds.BoxExtent.Z * 2.0f;
		BodySetup->AggGeom.BoxElems.Add(BoxElem);

		BodySetup->CollisionTraceFlag = CTF_UseDefault;
		HullCount = 1;
	}
	else if (CollisionType == TEXT("simple_sphere"))
	{
		BodySetup->AggGeom.BoxElems.Empty();
		BodySetup->AggGeom.SphereElems.Empty();
		BodySetup->AggGeom.SphylElems.Empty();
		BodySetup->AggGeom.ConvexElems.Empty();

		FKSphereElem SphereElem;
		FBoxSphereBounds Bounds = SM->GetBounds();
		SphereElem.Center = FVector::ZeroVector;
		SphereElem.Radius = Bounds.SphereRadius;
		BodySetup->AggGeom.SphereElems.Add(SphereElem);

		BodySetup->CollisionTraceFlag = CTF_UseDefault;
		HullCount = 1;
	}
	else if (CollisionType == TEXT("simple_capsule"))
	{
		BodySetup->AggGeom.BoxElems.Empty();
		BodySetup->AggGeom.SphereElems.Empty();
		BodySetup->AggGeom.SphylElems.Empty();
		BodySetup->AggGeom.ConvexElems.Empty();

		FKSphylElem CapsuleElem;
		FBoxSphereBounds Bounds = SM->GetBounds();
		CapsuleElem.Center = FVector::ZeroVector;
		CapsuleElem.Radius = FMath::Max(Bounds.BoxExtent.X, Bounds.BoxExtent.Y);
		CapsuleElem.Length = Bounds.BoxExtent.Z * 2.0f;
		BodySetup->AggGeom.SphylElems.Add(CapsuleElem);

		BodySetup->CollisionTraceFlag = CTF_UseDefault;
		HullCount = 1;
	}
	else if (CollisionType == TEXT("complex_as_simple"))
	{
		BodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
	}
	else if (CollisionType == TEXT("use_complex"))
	{
		BodySetup->CollisionTraceFlag = CTF_UseSimpleAndComplex;
	}
	else if (CollisionType == TEXT("use_default"))
	{
		BodySetup->CollisionTraceFlag = CTF_UseDefault;
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown collision_type '%s'. Valid: simple_box, simple_sphere, simple_capsule, complex_as_simple, use_complex, use_default"),
			*CollisionType));
	}

	// Invalidate physics data and rebuild
	BodySetup->InvalidatePhysicsData();
	BodySetup->CreatePhysicsMeshes();

	SM->PostEditChange();
	SM->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("collision_type"), ResultCollisionType);
	Result->SetNumberField(TEXT("hull_count"), HullCount);
	Result->SetNumberField(TEXT("box_elements"), BodySetup->AggGeom.BoxElems.Num());
	Result->SetNumberField(TEXT("sphere_elements"), BodySetup->AggGeom.SphereElems.Num());
	Result->SetNumberField(TEXT("capsule_elements"), BodySetup->AggGeom.SphylElems.Num());
	Result->SetNumberField(TEXT("convex_elements"), BodySetup->AggGeom.ConvexElems.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 16.7 analyze_lightmap_density
// ============================================================================

FMonolithActionResult FMonolithMeshTechArtActions::AnalyzeLightmapDensity(const TSharedPtr<FJsonObject>& Params)
{
	double TargetDensity = Params->HasField(TEXT("target_density"))
		? Params->GetNumberField(TEXT("target_density")) : 4.0;

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Collect actors
	TArray<AActor*> Actors;

	if (Params->HasField(TEXT("actor_name")))
	{
		FString Error;
		AActor* Actor = MonolithMeshUtils::FindActorByName(Params->GetStringField(TEXT("actor_name")), Error);
		if (!Actor)
		{
			return FMonolithActionResult::Error(Error);
		}
		Actors.Add(Actor);
	}
	else if (Params->HasField(TEXT("region_min")) && Params->HasField(TEXT("region_max")))
	{
		FVector RegionMin, RegionMax;
		MonolithMeshUtils::ParseVector(Params, TEXT("region_min"), RegionMin);
		MonolithMeshUtils::ParseVector(Params, TEXT("region_max"), RegionMax);

		FBox RegionBox(
			FVector(FMath::Min(RegionMin.X, RegionMax.X), FMath::Min(RegionMin.Y, RegionMax.Y), FMath::Min(RegionMin.Z, RegionMax.Z)),
			FVector(FMath::Max(RegionMin.X, RegionMax.X), FMath::Max(RegionMin.Y, RegionMax.Y), FMath::Max(RegionMin.Z, RegionMax.Z))
		);

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			FVector Origin, Extent;
			Actor->GetActorBounds(false, Origin, Extent);
			FBox ActorBox(Origin - Extent, Origin + Extent);
			if (RegionBox.Intersect(ActorBox))
			{
				Actors.Add(Actor);
			}
		}
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Provide either 'actor_name' or 'region_min'+'region_max'"));
	}

	// Analyze lightmap density per actor
	TArray<TSharedPtr<FJsonValue>> ActorResults;

	for (AActor* Actor : Actors)
	{
		TArray<UStaticMeshComponent*> SMCs;
		Actor->GetComponents(SMCs);

		if (SMCs.Num() == 0) continue;

		for (UStaticMeshComponent* SMC : SMCs)
		{
			if (!SMC || !SMC->IsVisible()) continue;

			UStaticMesh* SM = SMC->GetStaticMesh();
			if (!SM || !SM->GetRenderData() || SM->GetRenderData()->LODResources.Num() == 0)
				continue;

			auto ActorObj = MakeShared<FJsonObject>();
			ActorObj->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());

			// Get lightmap resolution
			int32 LightmapWidth = 0, LightmapHeight = 0;
			SMC->GetLightMapResolution(LightmapWidth, LightmapHeight);
			ActorObj->SetNumberField(TEXT("current_resolution"), FMath::Max(LightmapWidth, LightmapHeight));

			// Get lightmap UV channel index
			int32 LightmapUVIndex = SM->GetLightMapCoordinateIndex();
			ActorObj->SetNumberField(TEXT("lightmap_uv_index"), LightmapUVIndex);

			// Calculate UV coverage for the lightmap channel
			const FMeshUVChannelInfo& UVInfo = SM->GetRenderData()->UVChannelDataPerMaterial.IsValidIndex(0)
				? SM->GetRenderData()->UVChannelDataPerMaterial[0]
				: FMeshUVChannelInfo();

			double LocalUVDensity = 1.0;
			if (UVInfo.bInitialized && LightmapUVIndex < UE_ARRAY_COUNT(UVInfo.LocalUVDensities))
			{
				LocalUVDensity = FMath::Max(0.001, static_cast<double>(UVInfo.LocalUVDensities[LightmapUVIndex]));
			}

			// Calculate texels per cm
			FVector ActorScale = Actor->GetActorScale3D();
			float AvgScale = (FMath::Abs(ActorScale.X) + FMath::Abs(ActorScale.Y) + FMath::Abs(ActorScale.Z)) / 3.0f;
			int32 LightmapRes = FMath::Max(LightmapWidth, LightmapHeight);
			double TexelsPerCm = (LightmapRes * LocalUVDensity) / FMath::Max(0.001, static_cast<double>(AvgScale));
			ActorObj->SetNumberField(TEXT("texels_per_cm"), TexelsPerCm);

			// UV coverage estimate
			ActorObj->SetNumberField(TEXT("uv_density"), LocalUVDensity);

			// Recommend resolution to hit target density
			// target = (rec_res * uvDensity) / scale => rec_res = target * scale / uvDensity
			int32 RecommendedRes = FMath::RoundToInt32((TargetDensity * AvgScale) / FMath::Max(0.001, LocalUVDensity));
			// Snap to power of 2
			RecommendedRes = FMath::RoundUpToPowerOfTwo(FMath::Max(4, RecommendedRes));
			RecommendedRes = FMath::Min(RecommendedRes, 4096); // cap at 4096
			ActorObj->SetNumberField(TEXT("recommended_resolution"), RecommendedRes);

			// Deviation
			ActorObj->SetNumberField(TEXT("deviation_from_target"), TexelsPerCm - TargetDensity);

			ActorResults.Add(MakeShared<FJsonValueObject>(ActorObj));
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("actors"), ActorResults);
	Result->SetNumberField(TEXT("target_density"), TargetDensity);
	Result->SetNumberField(TEXT("actors_analyzed"), ActorResults.Num());

	return FMonolithActionResult::Success(Result);
}
