#include "MonolithMeshQualityActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithMeshAnalysis.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "IMeshMergeUtilities.h"
#include "MeshMergeModule.h"
#include "MeshMerge/MeshMergingSettings.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Misc/PackageName.h"
#include "StaticMeshResources.h"
#include "UObject/SavePackage.h"

// ============================================================================
// Helpers
// ============================================================================

TArray<TSharedPtr<FJsonValue>> FMonolithMeshQualityActions::VectorToJsonArray(const FVector& V)
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

void FMonolithMeshQualityActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 1. validate_naming_conventions
	Registry.RegisterAction(TEXT("mesh"), TEXT("validate_naming_conventions"),
		TEXT("Scan assets by path and flag names not matching prefix conventions (SM_ for StaticMesh, SK_ for SkeletalMesh, M_ for Material, MI_ for MaterialInstance, T_ for Texture, BP_ for Blueprint). Supports custom rules."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshQualityActions::ValidateNamingConventions),
		FParamSchemaBuilder()
			.OptionalAssetPathWithDefault(TEXT("scan_path"), TEXT("Content path to scan (e.g. /Game/Environment)"), TEXT("/Game"))
			.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Maximum violations to return"), TEXT("100"))
			.Optional(TEXT("custom_rules"), TEXT("object"), TEXT("Custom prefix rules: {\"ClassName\": \"Prefix_\", ...}"))
			.Build());

	// 2. batch_rename_assets
	Registry.RegisterAction(TEXT("mesh"), TEXT("batch_rename_assets"),
		TEXT("Rename assets with find/replace, prefix add/remove, or suffix add/remove. Uses IAssetTools::RenameAssets for automatic reference fixup and redirector creation."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshQualityActions::BatchRenameAssets),
		FParamSchemaBuilder()
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Array of asset paths to rename"))
			.Optional(TEXT("find"), TEXT("string"), TEXT("String to find in asset name"))
			.Optional(TEXT("replace"), TEXT("string"), TEXT("Replacement string"))
			.Optional(TEXT("add_prefix"), TEXT("string"), TEXT("Prefix to add"))
			.Optional(TEXT("remove_prefix"), TEXT("string"), TEXT("Prefix to remove"))
			.Optional(TEXT("add_suffix"), TEXT("string"), TEXT("Suffix to add"))
			.Optional(TEXT("remove_suffix"), TEXT("string"), TEXT("Suffix to remove"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview renames without applying"), TEXT("false"))
			.Build());

	// 3. generate_proxy_mesh
	Registry.RegisterAction(TEXT("mesh"), TEXT("generate_proxy_mesh"),
		TEXT("Merge selected static mesh actors into a single simplified proxy mesh. Uses IMeshMergeUtilities for LOD-aware merging with optional material merging."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshQualityActions::GenerateProxyMesh),
		FParamSchemaBuilder()
			.Required(TEXT("actor_names"), TEXT("array"), TEXT("Array of actor names to merge"))
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path for saved mesh (e.g. /Game/Merged/MyProxy)"))
			.Optional(TEXT("screen_size"), TEXT("integer"), TEXT("Screen size for proxy (pixels)"), TEXT("300"))
			.Optional(TEXT("merge_materials"), TEXT("boolean"), TEXT("Merge materials into atlas"), TEXT("true"))
			.Optional(TEXT("texture_size"), TEXT("integer"), TEXT("Merged material texture size"), TEXT("1024"))
			.Build());

	// 4. setup_hlod
	Registry.RegisterAction(TEXT("mesh"), TEXT("setup_hlod"),
		TEXT("Create or configure a UHLODLayer asset with type and settings."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshQualityActions::SetupHlod),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path for HLOD layer (e.g. /Game/HLOD/MyLayer)"))
			.Optional(TEXT("layer_type"), TEXT("string"), TEXT("HLOD type: MeshMerge, MeshSimplify, MeshApproximate, Custom"), TEXT("MeshSimplify"))
			.Optional(TEXT("cell_size"), TEXT("integer"), TEXT("HLOD cell size in world units"), TEXT("25600"))
			.Optional(TEXT("loading_range"), TEXT("number"), TEXT("Loading range multiplier"), TEXT("2.0"))
			.Build());

	// 5. analyze_texture_budget
	Registry.RegisterAction(TEXT("mesh"), TEXT("analyze_texture_budget"),
		TEXT("Analyze texture memory usage: pool size, used, top textures, by-format breakdown. Identifies budget hogs and gives recommendations."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshQualityActions::AnalyzeTextureBudget),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("scan_path"), TEXT("Content path filter (empty = all)"))
			.Optional(TEXT("top_count"), TEXT("integer"), TEXT("Number of top textures to return"), TEXT("20"))
			.Build());

	// 6. analyze_framing
	Registry.RegisterAction(TEXT("mesh"), TEXT("analyze_framing"),
		TEXT("Camera composition scoring: rule of thirds placement, depth layering, leading lines. Projects actors to screen space from a camera viewpoint and analyzes composition."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshQualityActions::AnalyzeFraming),
		FParamSchemaBuilder()
			.Required(TEXT("camera_location"), TEXT("array"), TEXT("Camera position [x, y, z]"))
			.Required(TEXT("camera_rotation"), TEXT("array"), TEXT("Camera rotation [pitch, yaw, roll]"))
			.Optional(TEXT("focal_actor"), TEXT("string"), TEXT("Name of the focal point actor"))
			.Optional(TEXT("fov"), TEXT("number"), TEXT("Field of view in degrees"), TEXT("90"))
			.Optional(TEXT("aspect_ratio"), TEXT("number"), TEXT("Aspect ratio (width/height)"), TEXT("1.777"))
			.Build());

	// 7. evaluate_monster_reveal
	Registry.RegisterAction(TEXT("mesh"), TEXT("evaluate_monster_reveal"),
		TEXT("Score a monster reveal moment: silhouette quality (screen coverage), backlight potential, distance rating, partial visibility, player camera alignment. Uses traces and sightline analysis."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshQualityActions::EvaluateMonsterReveal),
		FParamSchemaBuilder()
			.Required(TEXT("player_location"), TEXT("array"), TEXT("Player camera position [x, y, z]"))
			.Required(TEXT("player_rotation"), TEXT("array"), TEXT("Player camera rotation [pitch, yaw, roll]"))
			.Required(TEXT("monster_actor"), TEXT("string"), TEXT("Name of the monster/creature actor"))
			.Optional(TEXT("fov"), TEXT("number"), TEXT("Field of view in degrees"), TEXT("90"))
			.Build());

	// 8. analyze_co_op_balance
	Registry.RegisterAction(TEXT("mesh"), TEXT("analyze_co_op_balance"),
		TEXT("Analyze spatial design for co-op play: coverage blind spots, separation opportunities, communication distances. Given multiple player positions, evaluate the level's co-op balance."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshQualityActions::AnalyzeCoOpBalance),
		FParamSchemaBuilder()
			.Required(TEXT("player_positions"), TEXT("array"), TEXT("Array of player positions [[x,y,z], ...]"))
			.Optional(TEXT("region_min"), TEXT("array"), TEXT("Min corner of analysis region [x, y, z]"))
			.Optional(TEXT("region_max"), TEXT("array"), TEXT("Max corner of analysis region [x, y, z]"))
			.Build());

	// 9. integration_hooks_stub
	Registry.RegisterAction(TEXT("mesh"), TEXT("integration_hooks_stub"),
		TEXT("Stub actions for future AI Director / GAS / telemetry integration. Returns descriptions of planned interfaces rather than actual implementations."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshQualityActions::IntegrationHooksStub),
		FParamSchemaBuilder()
			.Optional(TEXT("hook_name"), TEXT("string"), TEXT("Specific hook to describe: ai_director, gas_tension, telemetry, or all"), TEXT("all"))
			.Build());
}

// ============================================================================
// 1. validate_naming_conventions
// ============================================================================

FMonolithActionResult FMonolithMeshQualityActions::ValidateNamingConventions(const TSharedPtr<FJsonObject>& Params)
{
	FString ScanPath = TEXT("/Game");
	Params->TryGetStringField(TEXT("scan_path"), ScanPath);

	double MaxResultsD = 100.0;
	Params->TryGetNumberField(TEXT("max_results"), MaxResultsD);
	int32 MaxResults = FMath::Clamp(static_cast<int32>(MaxResultsD), 1, 1000);

	// Default prefix rules: ClassName -> expected prefix
	TMap<FString, FString> PrefixRules;
	PrefixRules.Add(TEXT("StaticMesh"), TEXT("SM_"));
	PrefixRules.Add(TEXT("SkeletalMesh"), TEXT("SK_"));
	PrefixRules.Add(TEXT("Material"), TEXT("M_"));
	PrefixRules.Add(TEXT("MaterialInstanceConstant"), TEXT("MI_"));
	PrefixRules.Add(TEXT("Texture2D"), TEXT("T_"));
	PrefixRules.Add(TEXT("Blueprint"), TEXT("BP_"));
	PrefixRules.Add(TEXT("AnimSequence"), TEXT("AS_"));
	PrefixRules.Add(TEXT("AnimMontage"), TEXT("AM_"));
	PrefixRules.Add(TEXT("AnimBlueprint"), TEXT("ABP_"));
	PrefixRules.Add(TEXT("NiagaraSystem"), TEXT("NS_"));
	PrefixRules.Add(TEXT("NiagaraEmitter"), TEXT("NE_"));
	PrefixRules.Add(TEXT("SoundCue"), TEXT("SC_"));
	PrefixRules.Add(TEXT("SoundWave"), TEXT("SW_"));
	PrefixRules.Add(TEXT("ParticleSystem"), TEXT("PS_"));
	PrefixRules.Add(TEXT("WidgetBlueprint"), TEXT("WBP_"));

	// Apply custom rules (override defaults)
	const TSharedPtr<FJsonObject>* CustomRulesObj;
	if (Params->TryGetObjectField(TEXT("custom_rules"), CustomRulesObj) && CustomRulesObj->IsValid())
	{
		for (const auto& Pair : (*CustomRulesObj)->Values)
		{
			FString Prefix;
			if (Pair.Value->TryGetString(Prefix))
			{
				PrefixRules.Add(MonolithKeyToString(Pair.Key), Prefix);
			}
		}
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*ScanPath));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> AllAssets;
	AssetRegistry.GetAssets(Filter, AllAssets);

	TArray<TSharedPtr<FJsonValue>> Violations;
	int32 TotalScanned = 0;
	int32 TotalPassed = 0;

	for (const FAssetData& Asset : AllAssets)
	{
		if (Violations.Num() >= MaxResults)
		{
			break;
		}

		FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
		const FString* ExpectedPrefix = PrefixRules.Find(ClassName);
		if (!ExpectedPrefix)
		{
			continue; // No rule for this class, skip
		}

		TotalScanned++;
		FString AssetName = Asset.AssetName.ToString();

		if (AssetName.StartsWith(*ExpectedPrefix))
		{
			TotalPassed++;
			continue;
		}

		// Violation found
		auto Violation = MakeShared<FJsonObject>();
		Violation->SetStringField(TEXT("asset_path"), Asset.GetObjectPathString());
		Violation->SetStringField(TEXT("asset_name"), AssetName);
		Violation->SetStringField(TEXT("asset_class"), ClassName);
		Violation->SetStringField(TEXT("expected_prefix"), *ExpectedPrefix);
		Violation->SetStringField(TEXT("suggested_name"), *ExpectedPrefix + AssetName);
		Violations.Add(MakeShared<FJsonValueObject>(Violation));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("scan_path"), ScanPath);
	Result->SetNumberField(TEXT("total_assets_scanned"), TotalScanned);
	Result->SetNumberField(TEXT("passed"), TotalPassed);
	Result->SetNumberField(TEXT("violations"), Violations.Num());
	Result->SetBoolField(TEXT("truncated"), Violations.Num() >= MaxResults);
	Result->SetArrayField(TEXT("violations_list"), Violations);

	// Rules used
	TArray<TSharedPtr<FJsonValue>> RulesArr;
	for (const auto& Rule : PrefixRules)
	{
		auto RuleObj = MakeShared<FJsonObject>();
		RuleObj->SetStringField(TEXT("class"), Rule.Key);
		RuleObj->SetStringField(TEXT("prefix"), Rule.Value);
		RulesArr.Add(MakeShared<FJsonValueObject>(RuleObj));
	}
	Result->SetArrayField(TEXT("rules_applied"), RulesArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 2. batch_rename_assets
// ============================================================================

FMonolithActionResult FMonolithMeshQualityActions::BatchRenameAssets(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* PathsArr;
	if (!Params->TryGetArrayField(TEXT("asset_paths"), PathsArr) || PathsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: asset_paths"));
	}

	if (PathsArr->Num() > 200)
	{
		return FMonolithActionResult::Error(TEXT("Too many assets to rename (max 200)"));
	}

	FString FindStr, ReplaceStr, AddPrefix, RemovePrefix, AddSuffix, RemoveSuffix;
	Params->TryGetStringField(TEXT("find"), FindStr);
	Params->TryGetStringField(TEXT("replace"), ReplaceStr);
	Params->TryGetStringField(TEXT("add_prefix"), AddPrefix);
	Params->TryGetStringField(TEXT("remove_prefix"), RemovePrefix);
	Params->TryGetStringField(TEXT("add_suffix"), AddSuffix);
	Params->TryGetStringField(TEXT("remove_suffix"), RemoveSuffix);

	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	// Validate at least one operation specified
	if (FindStr.IsEmpty() && AddPrefix.IsEmpty() && RemovePrefix.IsEmpty() && AddSuffix.IsEmpty() && RemoveSuffix.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Must specify at least one rename operation (find/replace, add_prefix, remove_prefix, add_suffix, remove_suffix)"));
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

	TArray<FAssetRenameData> RenameData;
	TArray<TSharedPtr<FJsonValue>> PreviewArr;

	for (const TSharedPtr<FJsonValue>& PathVal : *PathsArr)
	{
		FString AssetPath;
		if (!PathVal->TryGetString(AssetPath) || AssetPath.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Each entry in asset_paths must be a non-empty string"));
		}

		// Normalize: if path lacks object name (Package.AssetName), append it
		// e.g. "/Game/Foo/Bar" → "/Game/Foo/Bar.Bar"
		if (!AssetPath.Contains(TEXT(".")))
		{
			FString BaseName = FPaths::GetBaseFilename(AssetPath);
			AssetPath = AssetPath + TEXT(".") + BaseName;
		}

		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
		if (!AssetData.IsValid())
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
		}

		FString OldName = AssetData.AssetName.ToString();
		FString NewName = OldName;

		// Apply operations in order: remove_prefix → remove_suffix → find/replace → add_prefix → add_suffix
		if (!RemovePrefix.IsEmpty() && NewName.StartsWith(RemovePrefix))
		{
			NewName.RightChopInline(RemovePrefix.Len());
		}
		if (!RemoveSuffix.IsEmpty() && NewName.EndsWith(RemoveSuffix))
		{
			NewName.LeftChopInline(RemoveSuffix.Len());
		}
		if (!FindStr.IsEmpty())
		{
			NewName.ReplaceInline(*FindStr, *ReplaceStr);
		}
		if (!AddPrefix.IsEmpty())
		{
			NewName = AddPrefix + NewName;
		}
		if (!AddSuffix.IsEmpty())
		{
			NewName = NewName + AddSuffix;
		}

		if (NewName == OldName)
		{
			continue; // No change needed
		}

		if (NewName.IsEmpty())
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Rename would result in empty name for %s"), *AssetPath));
		}

		FString PackagePath = FPackageName::GetLongPackagePath(AssetData.PackageName.ToString());

		FAssetRenameData Data;
		Data.Asset = AssetData.GetAsset();
		Data.NewName = NewName;
		Data.NewPackagePath = PackagePath;

		if (Data.Asset.IsValid())
		{
			RenameData.Add(Data);
		}

		auto PreviewObj = MakeShared<FJsonObject>();
		PreviewObj->SetStringField(TEXT("old_path"), AssetPath);
		PreviewObj->SetStringField(TEXT("old_name"), OldName);
		PreviewObj->SetStringField(TEXT("new_name"), NewName);
		PreviewObj->SetStringField(TEXT("new_path"), PackagePath / NewName);
		PreviewArr.Add(MakeShared<FJsonValueObject>(PreviewObj));
	}

	if (RenameData.Num() == 0)
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("no_changes"));
		Result->SetNumberField(TEXT("renamed_count"), 0);
		return FMonolithActionResult::Success(Result);
	}

	auto Result = MakeShared<FJsonObject>();

	if (bDryRun)
	{
		Result->SetStringField(TEXT("status"), TEXT("dry_run"));
		Result->SetNumberField(TEXT("would_rename_count"), RenameData.Num());
		Result->SetArrayField(TEXT("renames"), PreviewArr);
		return FMonolithActionResult::Success(Result);
	}

	// Execute renames — IAssetTools handles ALL reference fixup and redirector creation
	bool bSuccess = AssetTools.RenameAssets(RenameData);

	Result->SetStringField(TEXT("status"), bSuccess ? TEXT("success") : TEXT("partial_failure"));
	Result->SetNumberField(TEXT("renamed_count"), RenameData.Num());
	Result->SetArrayField(TEXT("renames"), PreviewArr);
	Result->SetStringField(TEXT("note"), TEXT("Redirectors created for reference fixup. Run 'Fix Up Redirectors' to clean up."));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 3. generate_proxy_mesh
// ============================================================================

FMonolithActionResult FMonolithMeshQualityActions::GenerateProxyMesh(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* ActorNamesArr;
	if (!Params->TryGetArrayField(TEXT("actor_names"), ActorNamesArr) || ActorNamesArr->Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("Required: actor_names (array of at least 2 actor names to merge)"));
	}

	if (ActorNamesArr->Num() > 100)
	{
		return FMonolithActionResult::Error(TEXT("Too many actors to merge (max 100)"));
	}

	FString SavePath;
	if (!Params->TryGetStringField(TEXT("save_path"), SavePath) || SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Required: save_path (asset path for saved proxy mesh)"));
	}

	double ScreenSizeD = 300.0;
	Params->TryGetNumberField(TEXT("screen_size"), ScreenSizeD);
	int32 ScreenSize = FMath::Clamp(static_cast<int32>(ScreenSizeD), 50, 4096);

	bool bMergeMaterials = true;
	Params->TryGetBoolField(TEXT("merge_materials"), bMergeMaterials);

	double TextureSizeD = 1024.0;
	Params->TryGetNumberField(TEXT("texture_size"), TextureSizeD);
	int32 TextureSize = FMath::Clamp(static_cast<int32>(TextureSizeD), 64, 4096);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Resolve actor names to components (API takes UPrimitiveComponent*)
	TArray<UPrimitiveComponent*> Components;
	TArray<FString> ResolvedNames;

	for (const TSharedPtr<FJsonValue>& NameVal : *ActorNamesArr)
	{
		FString ActorName;
		if (!NameVal->TryGetString(ActorName) || ActorName.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Each entry in actor_names must be a non-empty string"));
		}

		FString FindError;
		AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, FindError);
		if (!Actor)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Actor not found: %s — %s"), *ActorName, *FindError));
		}

		TArray<UStaticMeshComponent*> SMCs;
		Actor->GetComponents(SMCs);
		if (SMCs.Num() == 0)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Actor '%s' has no StaticMeshComponents"), *ActorName));
		}

		for (UStaticMeshComponent* SMC : SMCs)
		{
			if (SMC && SMC->GetStaticMesh())
			{
				Components.Add(SMC);
			}
		}

		ResolvedNames.Add(ActorName);
	}

	if (Components.Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("Need at least 2 static mesh components to merge"));
	}

	// Get the MeshMergeUtilities module
	const IMeshMergeUtilities& MergeUtilities = FModuleManager::Get().LoadModuleChecked<IMeshMergeModule>("MeshMergeUtilities").GetUtilities();

	// Configure merge settings
	FMeshMergingSettings MergeSettings;
	MergeSettings.bMergeMeshSockets = true;
	MergeSettings.bMergePhysicsData = true;
	MergeSettings.bBakeVertexDataToMesh = false;
#if !(ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
	// FMeshMergingSettings::bPivotPointAtZero deprecated/renamed in UE 5.8; false was the default.
	MergeSettings.bPivotPointAtZero = false;
#endif

	if (bMergeMaterials)
	{
		MergeSettings.bMergeMaterials = true;
		MergeSettings.MaterialSettings.TextureSize = FIntPoint(TextureSize, TextureSize);
	}

	// Create the output package
	FString PackageName = SavePath;
	FString AssetName = FPackageName::GetShortName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
	}

	// Perform the merge
	FVector MergedActorPivot = FVector::ZeroVector;
	TArray<UObject*> CreatedAssets;

	MergeUtilities.MergeComponentsToStaticMesh(
		Components,
		World,
		MergeSettings,
		nullptr, // InBaseMaterial
		Package,  // InOuter
		PackageName,
		CreatedAssets,
		MergedActorPivot,
		static_cast<float>(ScreenSize),
		true // bSilent
	);

	if (CreatedAssets.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Merge produced no output assets. Check that actors have valid static meshes."));
	}

	// Find the merged static mesh in created assets
	UStaticMesh* MergedMesh = nullptr;
	for (UObject* Obj : CreatedAssets)
	{
		MergedMesh = Cast<UStaticMesh>(Obj);
		if (MergedMesh) break;
	}

	auto Result = MakeShared<FJsonObject>();

	if (MergedMesh)
	{
		// Save the package
		FMonolithAssetUtils::SaveAssetPackage(Package);

		Result->SetStringField(TEXT("status"), TEXT("success"));
		Result->SetStringField(TEXT("merged_mesh_path"), SavePath);
		Result->SetNumberField(TEXT("source_components"), Components.Num());
		Result->SetNumberField(TEXT("source_actors"), ResolvedNames.Num());

		// Report merged mesh stats
		if (MergedMesh->GetRenderData() && MergedMesh->GetRenderData()->LODResources.Num() > 0)
		{
			const FStaticMeshLODResources& LOD0 = MergedMesh->GetRenderData()->LODResources[0];
			Result->SetNumberField(TEXT("merged_triangles"), LOD0.GetNumTriangles());
			Result->SetNumberField(TEXT("merged_vertices"), LOD0.GetNumVertices());
			Result->SetNumberField(TEXT("merged_sections"), LOD0.Sections.Num());
		}

		Result->SetNumberField(TEXT("created_assets"), CreatedAssets.Num());
		Result->SetArrayField(TEXT("pivot"), VectorToJsonArray(MergedActorPivot));
	}
	else
	{
		Result->SetStringField(TEXT("status"), TEXT("completed_no_mesh"));
		Result->SetNumberField(TEXT("created_assets"), CreatedAssets.Num());
		Result->SetStringField(TEXT("note"), TEXT("Merge completed but no StaticMesh found in output. Check created assets."));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 4. setup_hlod
// ============================================================================

FMonolithActionResult FMonolithMeshQualityActions::SetupHlod(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath;
	if (!Params->TryGetStringField(TEXT("save_path"), SavePath) || SavePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Required: save_path (asset path for HLOD layer)"));
	}

	FString LayerTypeStr = TEXT("MeshSimplify");
	Params->TryGetStringField(TEXT("layer_type"), LayerTypeStr);

	double CellSizeD = 25600.0;
	Params->TryGetNumberField(TEXT("cell_size"), CellSizeD);
	int32 CellSize = FMath::Clamp(static_cast<int32>(CellSizeD), 1600, 409600);

	double LoadingRange = 2.0;
	Params->TryGetNumberField(TEXT("loading_range"), LoadingRange);
	LoadingRange = FMath::Clamp(LoadingRange, 0.5, 10.0);

	// Map string to enum
	EHLODLayerType LayerType;
	if (LayerTypeStr.Equals(TEXT("MeshMerge"), ESearchCase::IgnoreCase))
	{
		LayerType = EHLODLayerType::MeshMerge;
	}
	else if (LayerTypeStr.Equals(TEXT("MeshSimplify"), ESearchCase::IgnoreCase))
	{
		LayerType = EHLODLayerType::MeshSimplify;
	}
	else if (LayerTypeStr.Equals(TEXT("MeshApproximate"), ESearchCase::IgnoreCase))
	{
		LayerType = EHLODLayerType::MeshApproximate;
	}
	else if (LayerTypeStr.Equals(TEXT("Custom"), ESearchCase::IgnoreCase))
	{
		LayerType = EHLODLayerType::Custom;
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid layer_type: %s. Use MeshMerge, MeshSimplify, MeshApproximate, or Custom"), *LayerTypeStr));
	}

	// Create the HLOD layer asset
	FString PackageName = SavePath;
	FString AssetName = FPackageName::GetShortName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
	}

	UHLODLayer* HLODLayer = NewObject<UHLODLayer>(Package, FName(*AssetName), RF_Public | RF_Standalone);
	if (!HLODLayer)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UHLODLayer"));
	}

	HLODLayer->SetLayerType(LayerType);

	// CellSize and LoadingRange are private UPROPERTY — set via property system
	if (FIntProperty* CellSizeProp = CastField<FIntProperty>(UHLODLayer::StaticClass()->FindPropertyByName(TEXT("CellSize"))))
	{
		CellSizeProp->SetPropertyValue_InContainer(HLODLayer, CellSize);
	}
	if (FDoubleProperty* LoadingRangeProp = CastField<FDoubleProperty>(UHLODLayer::StaticClass()->FindPropertyByName(TEXT("LoadingRange"))))
	{
		LoadingRangeProp->SetPropertyValue_InContainer(HLODLayer, LoadingRange);
	}

	// Mark dirty and save
	HLODLayer->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(HLODLayer);

	FMonolithAssetUtils::SaveAssetPackage(Package);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("success"));
	Result->SetStringField(TEXT("hlod_layer_path"), SavePath);
	Result->SetStringField(TEXT("layer_type"), LayerTypeStr);
	Result->SetNumberField(TEXT("cell_size"), CellSize);
	Result->SetNumberField(TEXT("loading_range"), LoadingRange);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 5. analyze_texture_budget
// ============================================================================

FMonolithActionResult FMonolithMeshQualityActions::AnalyzeTextureBudget(const TSharedPtr<FJsonObject>& Params)
{
	FString ScanPath;
	Params->TryGetStringField(TEXT("scan_path"), ScanPath);

	double TopCountD = 20.0;
	Params->TryGetNumberField(TEXT("top_count"), TopCountD);
	int32 TopCount = FMath::Clamp(static_cast<int32>(TopCountD), 1, 100);

	// Collect all texture info
	struct FTextureInfo
	{
		FString Path;
		FString Format;
		int32 Width = 0;
		int32 Height = 0;
		int64 ResourceSize = 0;
		int32 MipCount = 0;
		bool bIsStreamable = false;
	};

	TArray<FTextureInfo> Textures;
	int64 TotalResourceSize = 0;
	TMap<FString, int64> ByFormat;
	TMap<FString, int32> ByFormatCount;
	int32 TexturesOver2K = 0;
	int32 TexturesOver4K = 0;
	int32 NonPowerOf2 = 0;

	for (TObjectIterator<UTexture2D> It; It; ++It)
	{
		UTexture2D* Tex = *It;
		if (!Tex || Tex->HasAnyFlags(RF_Transient) || Tex->GetPathName().StartsWith(TEXT("/Engine/")))
		{
			continue;
		}

		// Filter by scan path if specified
		FString TexPath = Tex->GetPathName();
		if (!ScanPath.IsEmpty() && !TexPath.StartsWith(ScanPath))
		{
			continue;
		}

		FTextureInfo Info;
		Info.Path = TexPath;
		Info.Width = Tex->GetSizeX();
		Info.Height = Tex->GetSizeY();
		Info.ResourceSize = Tex->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal);
		Info.MipCount = Tex->GetNumMips();
		Info.bIsStreamable = Tex->IsStreamable();

		// Format string
		if (Tex->GetPlatformData() && Tex->GetPlatformData()->Mips.Num() > 0)
		{
			Info.Format = GPixelFormats[Tex->GetPlatformData()->PixelFormat].Name;
		}
		else
		{
			Info.Format = TEXT("Unknown");
		}

		TotalResourceSize += Info.ResourceSize;

		// Accumulate by format
		int64& FormatTotal = ByFormat.FindOrAdd(Info.Format, 0);
		FormatTotal += Info.ResourceSize;
		int32& FormatCount = ByFormatCount.FindOrAdd(Info.Format, 0);
		FormatCount++;

		// Stats
		if (Info.Width > 2048 || Info.Height > 2048)
		{
			TexturesOver2K++;
		}
		if (Info.Width > 4096 || Info.Height > 4096)
		{
			TexturesOver4K++;
		}
		if (!FMath::IsPowerOfTwo(Info.Width) || !FMath::IsPowerOfTwo(Info.Height))
		{
			NonPowerOf2++;
		}

		Textures.Add(MoveTemp(Info));
	}

	// Sort by resource size descending
	Textures.Sort([](const FTextureInfo& A, const FTextureInfo& B)
	{
		return A.ResourceSize > B.ResourceSize;
	});

	// Pool size from CVar
	float PoolSizeMB = 0.0f;
	IConsoleVariable* PoolCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Streaming.PoolSize"));
	if (PoolCVar)
	{
		PoolSizeMB = static_cast<float>(PoolCVar->GetInt());
	}

	// Top textures
	TArray<TSharedPtr<FJsonValue>> TopArr;
	for (int32 i = 0; i < FMath::Min(TopCount, Textures.Num()); ++i)
	{
		const FTextureInfo& Info = Textures[i];
		auto TexObj = MakeShared<FJsonObject>();
		TexObj->SetStringField(TEXT("path"), Info.Path);
		TexObj->SetStringField(TEXT("format"), Info.Format);
		TexObj->SetNumberField(TEXT("width"), Info.Width);
		TexObj->SetNumberField(TEXT("height"), Info.Height);
		TexObj->SetNumberField(TEXT("size_mb"), static_cast<double>(Info.ResourceSize) / (1024.0 * 1024.0));
		TexObj->SetNumberField(TEXT("mips"), Info.MipCount);
		TexObj->SetBoolField(TEXT("streamable"), Info.bIsStreamable);
		TopArr.Add(MakeShared<FJsonValueObject>(TexObj));
	}

	// By-format breakdown
	auto FormatObj = MakeShared<FJsonObject>();
	for (const auto& Pair : ByFormat)
	{
		auto FmtEntry = MakeShared<FJsonObject>();
		FmtEntry->SetNumberField(TEXT("size_mb"), static_cast<double>(Pair.Value) / (1024.0 * 1024.0));
		FmtEntry->SetNumberField(TEXT("count"), ByFormatCount.FindRef(Pair.Key));
		FormatObj->SetObjectField(Pair.Key, FmtEntry);
	}

	// Recommendations
	TArray<TSharedPtr<FJsonValue>> Recommendations;
	double UsedMB = static_cast<double>(TotalResourceSize) / (1024.0 * 1024.0);

	if (PoolSizeMB > 0 && UsedMB > PoolSizeMB)
	{
		Recommendations.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("OVER BUDGET: Using %.1fMB of %.0fMB pool. Reduce largest textures or increase r.Streaming.PoolSize."), UsedMB, PoolSizeMB)));
	}
	if (TexturesOver4K > 0)
	{
		Recommendations.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("%d textures exceed 4K resolution. Consider downscaling to 2K for non-hero assets."), TexturesOver4K)));
	}
	if (NonPowerOf2 > 10)
	{
		Recommendations.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("%d non-power-of-2 textures detected. These can't mip properly and waste VRAM."), NonPowerOf2)));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("pool_size_mb"), PoolSizeMB);
	Result->SetNumberField(TEXT("used_mb"), UsedMB);
	Result->SetNumberField(TEXT("texture_count"), Textures.Num());
	Result->SetNumberField(TEXT("textures_over_2k"), TexturesOver2K);
	Result->SetNumberField(TEXT("textures_over_4k"), TexturesOver4K);
	Result->SetNumberField(TEXT("non_power_of_2"), NonPowerOf2);
	Result->SetArrayField(TEXT("top_textures"), TopArr);
	Result->SetObjectField(TEXT("by_format"), FormatObj);
	Result->SetArrayField(TEXT("recommendations"), Recommendations);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 6. analyze_framing
// ============================================================================

namespace
{
	/** Project a world point to normalized screen coords (0-1) given camera params */
	bool ProjectToScreen(const FVector& WorldPoint, const FVector& CamLoc, const FRotator& CamRot,
		float FOVDeg, float AspectRatio, FVector2D& OutScreenPos)
	{
		FVector ToPoint = WorldPoint - CamLoc;
		FVector Forward = CamRot.Vector();
		FVector Right = FRotationMatrix(CamRot).GetScaledAxis(EAxis::Y);
		FVector Up = FRotationMatrix(CamRot).GetScaledAxis(EAxis::Z);

		float Depth = FVector::DotProduct(ToPoint, Forward);
		if (Depth <= 0.0f)
		{
			return false; // Behind camera
		}

		float HalfFovRad = FMath::DegreesToRadians(FOVDeg * 0.5f);
		float HalfWidth = Depth * FMath::Tan(HalfFovRad);
		float HalfHeight = HalfWidth / AspectRatio;

		float ScreenX = FVector::DotProduct(ToPoint, Right);
		float ScreenY = FVector::DotProduct(ToPoint, Up);

		OutScreenPos.X = (ScreenX / HalfWidth) * 0.5f + 0.5f;
		OutScreenPos.Y = (-ScreenY / HalfHeight) * 0.5f + 0.5f; // Flip Y

		return OutScreenPos.X >= 0.0f && OutScreenPos.X <= 1.0f &&
		       OutScreenPos.Y >= 0.0f && OutScreenPos.Y <= 1.0f;
	}

	/** Score how close a normalized screen point is to rule-of-thirds intersections */
	float ScoreRuleOfThirds(const FVector2D& ScreenPos)
	{
		// Four rule-of-thirds intersection points
		static const FVector2D ThirdsPoints[] = {
			{1.0f/3.0f, 1.0f/3.0f}, {2.0f/3.0f, 1.0f/3.0f},
			{1.0f/3.0f, 2.0f/3.0f}, {2.0f/3.0f, 2.0f/3.0f}
		};

		float BestDist = TNumericLimits<float>::Max();
		for (const FVector2D& Pt : ThirdsPoints)
		{
			float Dist = FVector2D::Distance(ScreenPos, Pt);
			BestDist = FMath::Min(BestDist, Dist);
		}

		// Score: 1.0 if exactly on a thirds point, 0.0 if far away (>0.3 normalized)
		return FMath::Clamp(1.0f - (BestDist / 0.3f), 0.0f, 1.0f);
	}
}

FMonolithActionResult FMonolithMeshQualityActions::AnalyzeFraming(const TSharedPtr<FJsonObject>& Params)
{
	FVector CamLocation;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("camera_location"), CamLocation))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: camera_location"));
	}

	FRotator CamRotation;
	if (!MonolithMeshUtils::ParseRotator(Params, TEXT("camera_rotation"), CamRotation))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: camera_rotation"));
	}

	double FOV = 90.0;
	Params->TryGetNumberField(TEXT("fov"), FOV);
	FOV = FMath::Clamp(FOV, 30.0, 170.0);

	double AspectRatio = 1.777;
	Params->TryGetNumberField(TEXT("aspect_ratio"), AspectRatio);
	AspectRatio = FMath::Clamp(AspectRatio, 0.5, 4.0);

	FString FocalActorName;
	Params->TryGetStringField(TEXT("focal_actor"), FocalActorName);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FVector Forward = CamRotation.Vector();

	// Collect visible actors and project to screen
	struct FScreenActor
	{
		FString Name;
		FVector2D ScreenCenter;
		FVector2D ScreenMin;
		FVector2D ScreenMax;
		float Distance;
		float ScreenArea;
	};

	TArray<FScreenActor> ScreenActors;
	float HalfFovRad = FMath::DegreesToRadians(static_cast<float>(FOV) * 0.5f);

	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		FVector Origin, Extent;
		Actor->GetActorBounds(false, Origin, Extent);

		FVector2D ScreenCenter;
		if (!ProjectToScreen(Origin, CamLocation, CamRotation, static_cast<float>(FOV), static_cast<float>(AspectRatio), ScreenCenter))
		{
			continue;
		}

		// Project bounds corners for screen extent
		FVector2D ScreenMin(1.0f, 1.0f), ScreenMax(0.0f, 0.0f);
		FVector Corners[8];
		FBox Box(Origin - Extent, Origin + Extent);
		for (int32 i = 0; i < 8; ++i)
		{
			Corners[i] = FVector(
				(i & 1) ? Box.Max.X : Box.Min.X,
				(i & 2) ? Box.Max.Y : Box.Min.Y,
				(i & 4) ? Box.Max.Z : Box.Min.Z
			);
			FVector2D CornerScreen;
			if (ProjectToScreen(Corners[i], CamLocation, CamRotation, static_cast<float>(FOV), static_cast<float>(AspectRatio), CornerScreen))
			{
				ScreenMin.X = FMath::Min(ScreenMin.X, CornerScreen.X);
				ScreenMin.Y = FMath::Min(ScreenMin.Y, CornerScreen.Y);
				ScreenMax.X = FMath::Max(ScreenMax.X, CornerScreen.X);
				ScreenMax.Y = FMath::Max(ScreenMax.Y, CornerScreen.Y);
			}
		}

		FScreenActor SA;
		SA.Name = Actor->GetActorLabel();
		SA.ScreenCenter = ScreenCenter;
		SA.ScreenMin = ScreenMin;
		SA.ScreenMax = ScreenMax;
		SA.Distance = FVector::Dist(CamLocation, Origin);
		SA.ScreenArea = (ScreenMax.X - ScreenMin.X) * (ScreenMax.Y - ScreenMin.Y);
		ScreenActors.Add(SA);
	}

	// Analyze depth layers
	ScreenActors.Sort([](const FScreenActor& A, const FScreenActor& B) { return A.Distance < B.Distance; });

	int32 ForegroundCount = 0, MidgroundCount = 0, BackgroundCount = 0;
	float MaxDist = ScreenActors.Num() > 0 ? ScreenActors.Last().Distance : 1.0f;

	for (const FScreenActor& SA : ScreenActors)
	{
		float NormDist = SA.Distance / FMath::Max(MaxDist, 1.0f);
		if (NormDist < 0.2f) ForegroundCount++;
		else if (NormDist < 0.6f) MidgroundCount++;
		else BackgroundCount++;
	}

	// Focal actor analysis
	float FocalRuleOfThirdsScore = 0.0f;
	float FocalScreenCoverage = 0.0f;
	FString FocalStatus = TEXT("not_specified");

	if (!FocalActorName.IsEmpty())
	{
		bool bFound = false;
		for (const FScreenActor& SA : ScreenActors)
		{
			if (SA.Name == FocalActorName)
			{
				FocalRuleOfThirdsScore = ScoreRuleOfThirds(SA.ScreenCenter);
				FocalScreenCoverage = SA.ScreenArea;
				FocalStatus = TEXT("visible");
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			FocalStatus = TEXT("not_visible");
		}
	}

	// Leading lines: trace from screen edges toward focal point
	int32 LeadingLineCount = 0;
	if (!FocalActorName.IsEmpty() && FocalStatus == TEXT("visible"))
	{
		FString FindErr;
		AActor* FocalActor = MonolithMeshUtils::FindActorByName(FocalActorName, FindErr);
		if (FocalActor)
		{
			FVector FocalLoc = FocalActor->GetActorLocation();
			// Sample rays from camera edges toward focal — count hits that create depth lines
			FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(FramingTrace), true);

			const FVector Right = FRotationMatrix(CamRotation).GetScaledAxis(EAxis::Y);
			const FVector Up = FRotationMatrix(CamRotation).GetScaledAxis(EAxis::Z);

			for (int32 i = 0; i < 8; ++i)
			{
				float Angle = (static_cast<float>(i) / 8.0f) * UE_TWO_PI;
				FVector EdgeDir = FMath::Cos(Angle) * Right + FMath::Sin(Angle) * Up;
				FVector RayStart = CamLocation + EdgeDir * 200.0f;
				FVector ToFocal = (FocalLoc - RayStart).GetSafeNormal();

				FHitResult Hit;
				if (World->LineTraceSingleByChannel(Hit, RayStart, RayStart + ToFocal * 5000.0f, ECC_Visibility, TraceParams))
				{
					// Edge hits near the focal direction suggest leading geometry
					float DotToFocal = FVector::DotProduct((Hit.ImpactPoint - CamLocation).GetSafeNormal(), (FocalLoc - CamLocation).GetSafeNormal());
					if (DotToFocal > 0.7f)
					{
						LeadingLineCount++;
					}
				}
			}
		}
	}

	// Overall composition score
	float DepthLayerScore = 0.0f;
	if (ForegroundCount > 0 && MidgroundCount > 0 && BackgroundCount > 0)
	{
		DepthLayerScore = 1.0f;
	}
	else if ((ForegroundCount > 0 && BackgroundCount > 0) || (ForegroundCount > 0 && MidgroundCount > 0) || (MidgroundCount > 0 && BackgroundCount > 0))
	{
		DepthLayerScore = 0.6f;
	}
	else
	{
		DepthLayerScore = 0.2f;
	}

	float LeadingLineScore = FMath::Clamp(static_cast<float>(LeadingLineCount) / 3.0f, 0.0f, 1.0f);
	float OverallScore = (FocalRuleOfThirdsScore * 0.4f + DepthLayerScore * 0.35f + LeadingLineScore * 0.25f) * 100.0f;

	// Build result
	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("camera_location"), VectorToJsonArray(CamLocation));
	Result->SetNumberField(TEXT("fov"), FOV);
	Result->SetNumberField(TEXT("visible_actors"), ScreenActors.Num());
	Result->SetNumberField(TEXT("overall_composition_score"), FMath::RoundToFloat(OverallScore));

	auto DepthObj = MakeShared<FJsonObject>();
	DepthObj->SetNumberField(TEXT("foreground"), ForegroundCount);
	DepthObj->SetNumberField(TEXT("midground"), MidgroundCount);
	DepthObj->SetNumberField(TEXT("background"), BackgroundCount);
	DepthObj->SetNumberField(TEXT("depth_layer_score"), DepthLayerScore);
	Result->SetObjectField(TEXT("depth_layers"), DepthObj);

	auto FocalObj = MakeShared<FJsonObject>();
	FocalObj->SetStringField(TEXT("status"), FocalStatus);
	FocalObj->SetNumberField(TEXT("rule_of_thirds_score"), FocalRuleOfThirdsScore);
	FocalObj->SetNumberField(TEXT("screen_coverage"), FocalScreenCoverage);
	FocalObj->SetNumberField(TEXT("leading_lines_detected"), LeadingLineCount);
	FocalObj->SetNumberField(TEXT("leading_line_score"), LeadingLineScore);
	Result->SetObjectField(TEXT("focal_analysis"), FocalObj);

	// Top 5 largest screen actors
	TArray<FScreenActor> BySize = ScreenActors;
	BySize.Sort([](const FScreenActor& A, const FScreenActor& B) { return A.ScreenArea > B.ScreenArea; });

	TArray<TSharedPtr<FJsonValue>> TopActorsArr;
	for (int32 i = 0; i < FMath::Min(5, BySize.Num()); ++i)
	{
		auto ActObj = MakeShared<FJsonObject>();
		ActObj->SetStringField(TEXT("name"), BySize[i].Name);
		ActObj->SetNumberField(TEXT("screen_area"), BySize[i].ScreenArea);
		ActObj->SetNumberField(TEXT("distance"), BySize[i].Distance);
		ActObj->SetNumberField(TEXT("rule_of_thirds_score"), ScoreRuleOfThirds(BySize[i].ScreenCenter));
		TopActorsArr.Add(MakeShared<FJsonValueObject>(ActObj));
	}
	Result->SetArrayField(TEXT("dominant_actors"), TopActorsArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 7. evaluate_monster_reveal
// ============================================================================

FMonolithActionResult FMonolithMeshQualityActions::EvaluateMonsterReveal(const TSharedPtr<FJsonObject>& Params)
{
	FVector PlayerLoc;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("player_location"), PlayerLoc))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: player_location"));
	}

	FRotator PlayerRot;
	if (!MonolithMeshUtils::ParseRotator(Params, TEXT("player_rotation"), PlayerRot))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: player_rotation"));
	}

	FString MonsterName;
	if (!Params->TryGetStringField(TEXT("monster_actor"), MonsterName) || MonsterName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Required: monster_actor (name of the creature actor)"));
	}

	double FOV = 90.0;
	Params->TryGetNumberField(TEXT("fov"), FOV);
	FOV = FMath::Clamp(FOV, 30.0, 170.0);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString FindErr;
	AActor* MonsterActor = MonolithMeshUtils::FindActorByName(MonsterName, FindErr);
	if (!MonsterActor)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Monster actor not found: %s — %s"), *MonsterName, *FindErr));
	}

	FVector MonsterOrigin, MonsterExtent;
	MonsterActor->GetActorBounds(false, MonsterOrigin, MonsterExtent);
	float Distance = FVector::Dist(PlayerLoc, MonsterOrigin);

	FVector Forward = PlayerRot.Vector();
	FVector ToMonster = (MonsterOrigin - PlayerLoc).GetSafeNormal();

	// --- Camera alignment ---
	float DotForward = FVector::DotProduct(Forward, ToMonster);
	float AlignmentScore = FMath::Clamp(DotForward, 0.0f, 1.0f);
	bool bInFOV = DotForward > FMath::Cos(FMath::DegreesToRadians(static_cast<float>(FOV) * 0.5f));

	// --- Silhouette (screen coverage) ---
	FVector2D ScreenCenter;
	float SilhouetteScore = 0.0f;
	float ScreenCoverage = 0.0f;
	if (bInFOV)
	{
		ProjectToScreen(MonsterOrigin, PlayerLoc, PlayerRot, static_cast<float>(FOV), 1.777f, ScreenCenter);

		// Approximate screen coverage from bounds
		float AngularSize = FMath::Atan2(MonsterExtent.Size(), FMath::Max(Distance, 1.0f));
		ScreenCoverage = FMath::Clamp(AngularSize / FMath::DegreesToRadians(static_cast<float>(FOV) * 0.5f), 0.0f, 1.0f);

		// Best silhouette: 10-30% of screen coverage
		if (ScreenCoverage >= 0.1f && ScreenCoverage <= 0.3f)
		{
			SilhouetteScore = 1.0f;
		}
		else if (ScreenCoverage > 0.3f)
		{
			SilhouetteScore = FMath::Clamp(1.0f - (ScreenCoverage - 0.3f) / 0.4f, 0.0f, 1.0f);
		}
		else
		{
			SilhouetteScore = FMath::Clamp(ScreenCoverage / 0.1f, 0.0f, 1.0f);
		}
	}

	// --- Distance rating ---
	// Ideal reveal: 8-20 meters
	float DistanceScore;
	if (Distance >= 800.0f && Distance <= 2000.0f)
	{
		DistanceScore = 1.0f;
	}
	else if (Distance < 800.0f)
	{
		DistanceScore = FMath::Clamp(Distance / 800.0f, 0.0f, 1.0f);
	}
	else
	{
		DistanceScore = FMath::Clamp(1.0f - (Distance - 2000.0f) / 3000.0f, 0.0f, 1.0f);
	}

	// --- Partial visibility (concealment from player POV) ---
	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(RevealTrace), true);
	TraceParams.AddIgnoredActor(MonsterActor);

	// Trace to multiple points on the monster to check partial visibility
	int32 VisiblePoints = 0;
	int32 TotalPoints = 0;
	for (int32 z = -1; z <= 1; ++z)
	{
		for (int32 x = -1; x <= 1; ++x)
		{
			FVector TestPoint = MonsterOrigin + FVector(
				MonsterExtent.X * static_cast<float>(x) * 0.7f,
				0.0f,
				MonsterExtent.Z * static_cast<float>(z) * 0.7f
			);
			TotalPoints++;

			FHitResult Hit;
			if (!World->LineTraceSingleByChannel(Hit, PlayerLoc, TestPoint, ECC_Visibility, TraceParams))
			{
				VisiblePoints++;
			}
		}
	}

	float VisibilityRatio = TotalPoints > 0 ? static_cast<float>(VisiblePoints) / static_cast<float>(TotalPoints) : 0.0f;

	// Partial visibility is best: 30-70% visible creates mystery
	float PartialScore;
	if (VisibilityRatio >= 0.3f && VisibilityRatio <= 0.7f)
	{
		PartialScore = 1.0f;
	}
	else if (VisibilityRatio < 0.3f)
	{
		PartialScore = FMath::Clamp(VisibilityRatio / 0.3f, 0.0f, 1.0f);
	}
	else
	{
		PartialScore = FMath::Clamp(1.0f - (VisibilityRatio - 0.7f) / 0.3f, 0.2f, 1.0f);
	}

	// --- Backlight potential ---
	// Trace from monster in directions away from player to find lights
	float BacklightScore = 0.0f;
	int32 BacklightsFound = 0;

	FVector MonsterToPlayer = (PlayerLoc - MonsterOrigin).GetSafeNormal();
	FVector BackDirection = -MonsterToPlayer;

	for (TActorIterator<AActor> LightIt(World); LightIt; ++LightIt)
	{
		ULightComponent* LightComp = (*LightIt)->FindComponentByClass<ULightComponent>();
		if (!LightComp || !LightComp->IsVisible())
		{
			continue;
		}

		FVector LightLoc = LightComp->GetComponentLocation();
		FVector MonsterToLight = (LightLoc - MonsterOrigin).GetSafeNormal();

		// Light is behind monster relative to player if dot with back direction > 0.3
		float DotBack = FVector::DotProduct(MonsterToLight, BackDirection);
		if (DotBack > 0.3f)
		{
			// Check line of sight to light
			FHitResult LightHit;
			if (!World->LineTraceSingleByChannel(LightHit, MonsterOrigin, LightLoc, ECC_Visibility, TraceParams))
			{
				BacklightsFound++;
				BacklightScore = FMath::Max(BacklightScore, DotBack);
			}
		}
	}

	// --- Overall reveal score ---
	float OverallScore = (
		SilhouetteScore * 0.25f +
		DistanceScore * 0.2f +
		PartialScore * 0.25f +
		AlignmentScore * 0.15f +
		BacklightScore * 0.15f
	) * 100.0f;

	// Quality tier
	FString Tier;
	if (OverallScore >= 80.0f) Tier = TEXT("Excellent");
	else if (OverallScore >= 60.0f) Tier = TEXT("Good");
	else if (OverallScore >= 40.0f) Tier = TEXT("Mediocre");
	else if (OverallScore >= 20.0f) Tier = TEXT("Poor");
	else Tier = TEXT("Bad");

	// Recommendations
	TArray<TSharedPtr<FJsonValue>> Tips;
	if (!bInFOV)
	{
		Tips.Add(MakeShared<FJsonValueString>(TEXT("Monster is outside player FOV — no reveal happens. Reposition or adjust approach angle.")));
	}
	if (SilhouetteScore < 0.5f && ScreenCoverage < 0.1f)
	{
		Tips.Add(MakeShared<FJsonValueString>(TEXT("Monster is too small on screen. Move closer or use a narrower corridor.")));
	}
	if (VisibilityRatio > 0.9f)
	{
		Tips.Add(MakeShared<FJsonValueString>(TEXT("Monster is fully visible — add partial occlusion (doorframe, fog, corner) for mystery.")));
	}
	if (BacklightsFound == 0)
	{
		Tips.Add(MakeShared<FJsonValueString>(TEXT("No backlight detected. Place a light behind the monster for silhouette definition.")));
	}
	if (Distance < 300.0f)
	{
		Tips.Add(MakeShared<FJsonValueString>(TEXT("Too close for a reveal — this is a jumpscare distance. Pull back to 8-20m for dread.")));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("monster_actor"), MonsterName);
	Result->SetNumberField(TEXT("distance_cm"), Distance);
	Result->SetBoolField(TEXT("in_fov"), bInFOV);
	Result->SetNumberField(TEXT("visibility_ratio"), VisibilityRatio);
	Result->SetNumberField(TEXT("screen_coverage"), ScreenCoverage);
	Result->SetNumberField(TEXT("backlights_found"), BacklightsFound);

	auto ScoresObj = MakeShared<FJsonObject>();
	ScoresObj->SetNumberField(TEXT("silhouette"), FMath::RoundToFloat(SilhouetteScore * 100.0f));
	ScoresObj->SetNumberField(TEXT("distance"), FMath::RoundToFloat(DistanceScore * 100.0f));
	ScoresObj->SetNumberField(TEXT("partial_visibility"), FMath::RoundToFloat(PartialScore * 100.0f));
	ScoresObj->SetNumberField(TEXT("camera_alignment"), FMath::RoundToFloat(AlignmentScore * 100.0f));
	ScoresObj->SetNumberField(TEXT("backlight"), FMath::RoundToFloat(BacklightScore * 100.0f));
	ScoresObj->SetNumberField(TEXT("overall"), FMath::RoundToFloat(OverallScore));
	Result->SetObjectField(TEXT("scores"), ScoresObj);

	Result->SetStringField(TEXT("tier"), Tier);
	Result->SetArrayField(TEXT("recommendations"), Tips);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 8. analyze_co_op_balance (P3 placeholder)
// ============================================================================

FMonolithActionResult FMonolithMeshQualityActions::AnalyzeCoOpBalance(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* PositionsArr;
	if (!Params->TryGetArrayField(TEXT("player_positions"), PositionsArr) || PositionsArr->Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("Required: player_positions (array of at least 2 player positions [[x,y,z], ...])"));
	}

	if (PositionsArr->Num() > 8)
	{
		return FMonolithActionResult::Error(TEXT("Maximum 8 player positions supported"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Parse player positions
	TArray<FVector> Positions;
	for (const TSharedPtr<FJsonValue>& PosVal : *PositionsArr)
	{
		const TArray<TSharedPtr<FJsonValue>>* PosArr;
		if (!PosVal->TryGetArray(PosArr) || PosArr->Num() < 3)
		{
			return FMonolithActionResult::Error(TEXT("Each player position must be [x, y, z]"));
		}

		FVector Pos;
		Pos.X = (*PosArr)[0]->AsNumber();
		Pos.Y = (*PosArr)[1]->AsNumber();
		Pos.Z = (*PosArr)[2]->AsNumber();
		Positions.Add(Pos);
	}

	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(CoopTrace), true);

	// --- Pairwise analysis ---
	TArray<TSharedPtr<FJsonValue>> PairAnalysis;
	float TotalSeparation = 0.0f;
	int32 LOSCount = 0;
	int32 PairCount = 0;

	for (int32 i = 0; i < Positions.Num(); ++i)
	{
		for (int32 j = i + 1; j < Positions.Num(); ++j)
		{
			PairCount++;
			float Dist = FVector::Dist(Positions[i], Positions[j]);
			TotalSeparation += Dist;

			// Check line of sight
			FHitResult Hit;
			bool bHasLOS = !World->LineTraceSingleByChannel(Hit, Positions[i], Positions[j], ECC_Visibility, TraceParams);
			if (bHasLOS) LOSCount++;

			auto PairObj = MakeShared<FJsonObject>();
			PairObj->SetStringField(TEXT("pair"), FString::Printf(TEXT("Player%d-Player%d"), i, j));
			PairObj->SetNumberField(TEXT("distance_cm"), Dist);
			PairObj->SetBoolField(TEXT("line_of_sight"), bHasLOS);

			// Communication distance rating
			FString CommRating;
			if (Dist < 500.0f) CommRating = TEXT("close");
			else if (Dist < 1500.0f) CommRating = TEXT("comfortable");
			else if (Dist < 3000.0f) CommRating = TEXT("strained");
			else CommRating = TEXT("separated");
			PairObj->SetStringField(TEXT("communication_rating"), CommRating);

			PairAnalysis.Add(MakeShared<FJsonValueObject>(PairObj));
		}
	}

	// --- Coverage analysis: radial sweep from each player for blind spots ---
	int32 TotalDirections = 16;
	TArray<bool> CoveredDirections;
	CoveredDirections.SetNumZeroed(TotalDirections);

	FVector Centroid = FVector::ZeroVector;
	for (const FVector& Pos : Positions)
	{
		Centroid += Pos;
	}
	Centroid /= static_cast<float>(Positions.Num());

	// For each direction from centroid, check if any player covers it
	for (int32 d = 0; d < TotalDirections; ++d)
	{
		float Angle = (static_cast<float>(d) / static_cast<float>(TotalDirections)) * UE_TWO_PI;
		FVector Dir(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
		FVector TestPoint = Centroid + Dir * 2000.0f;

		for (const FVector& Pos : Positions)
		{
			FVector PlayerToTest = (TestPoint - Pos).GetSafeNormal();
			FVector PlayerForward = (Centroid - Pos).GetSafeNormal(); // Approximate forward as toward group center

			// Player "covers" this direction if it's within 90 degrees of their facing
			if (FVector::DotProduct(PlayerToTest, (TestPoint - Pos).GetSafeNormal()) > 0.0f)
			{
				FHitResult CoverHit;
				if (!World->LineTraceSingleByChannel(CoverHit, Pos, TestPoint, ECC_Visibility, TraceParams))
				{
					CoveredDirections[d] = true;
					break;
				}
			}
		}
	}

	int32 CoveredCount = 0;
	TArray<TSharedPtr<FJsonValue>> BlindSpots;
	for (int32 d = 0; d < TotalDirections; ++d)
	{
		if (CoveredDirections[d])
		{
			CoveredCount++;
		}
		else
		{
			float Angle = (static_cast<float>(d) / static_cast<float>(TotalDirections)) * 360.0f;
			BlindSpots.Add(MakeShared<FJsonValueNumber>(Angle));
		}
	}

	float CoveragePercent = static_cast<float>(CoveredCount) / static_cast<float>(TotalDirections) * 100.0f;

	// --- Separation opportunities ---
	// Check for walls/geometry between players that could force separation
	int32 SeparationOpportunities = PairCount - LOSCount;

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("player_count"), Positions.Num());
	Result->SetNumberField(TEXT("average_separation_cm"), PairCount > 0 ? TotalSeparation / PairCount : 0.0);
	Result->SetNumberField(TEXT("line_of_sight_pairs"), LOSCount);
	Result->SetNumberField(TEXT("total_pairs"), PairCount);
	Result->SetNumberField(TEXT("separation_opportunities"), SeparationOpportunities);
	Result->SetNumberField(TEXT("coverage_percent"), CoveragePercent);
	Result->SetArrayField(TEXT("blind_spot_angles"), BlindSpots);
	Result->SetArrayField(TEXT("pair_analysis"), PairAnalysis);
	Result->SetArrayField(TEXT("centroid"), VectorToJsonArray(Centroid));

	// Co-op suitability score
	float LOSRatio = PairCount > 0 ? static_cast<float>(LOSCount) / static_cast<float>(PairCount) : 0.0f;
	float AvgSep = PairCount > 0 ? TotalSeparation / PairCount : 0.0f;

	// Ideal: some but not all LOS, moderate separation, good coverage
	float CoopScore = 0.0f;
	CoopScore += (LOSRatio >= 0.3f && LOSRatio <= 0.7f) ? 30.0f : (LOSRatio > 0.7f ? 20.0f : 10.0f);
	CoopScore += (AvgSep >= 500.0f && AvgSep <= 2000.0f) ? 30.0f : 15.0f;
	CoopScore += CoveragePercent * 0.4f;

	Result->SetNumberField(TEXT("co_op_balance_score"), FMath::RoundToFloat(CoopScore));
	Result->SetStringField(TEXT("note"), TEXT("P3 placeholder: basic spatial analysis. Future versions will integrate navmesh pathing, encounter zones, and dynamic difficulty."));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 9. integration_hooks_stub
// ============================================================================

FMonolithActionResult FMonolithMeshQualityActions::IntegrationHooksStub(const TSharedPtr<FJsonObject>& Params)
{
	FString HookName = TEXT("all");
	Params->TryGetStringField(TEXT("hook_name"), HookName);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("not_yet_implemented"));
	Result->SetStringField(TEXT("note"), TEXT("These are interface descriptions for planned integrations. Actual implementations will come in future phases."));

	auto AddHook = [&](const FString& Name, const TSharedPtr<FJsonObject>& HookObj)
	{
		Result->SetObjectField(Name, HookObj);
	};

	if (HookName == TEXT("all") || HookName == TEXT("ai_director"))
	{
		auto Hook = MakeShared<FJsonObject>();
		Hook->SetStringField(TEXT("name"), TEXT("ai_director"));
		Hook->SetStringField(TEXT("description"), TEXT("Real-time spatial data feed for the AI Director. Provides tension maps, player position analysis, and encounter zone scoring."));
		Hook->SetStringField(TEXT("planned_interface"), TEXT("feed_ai_director({ region, player_positions, tension_override? }) -> { tension_map, suggested_spawn_zones, pacing_state }"));

		TArray<TSharedPtr<FJsonValue>> Inputs;
		Inputs.Add(MakeShared<FJsonValueString>(TEXT("region: FBox — world region to analyze")));
		Inputs.Add(MakeShared<FJsonValueString>(TEXT("player_positions: TArray<FVector> — current player locations")));
		Inputs.Add(MakeShared<FJsonValueString>(TEXT("tension_override: float (optional) — force a tension level")));
		Hook->SetArrayField(TEXT("inputs"), Inputs);

		TArray<TSharedPtr<FJsonValue>> Outputs;
		Outputs.Add(MakeShared<FJsonValueString>(TEXT("tension_map: grid of tension values per cell")));
		Outputs.Add(MakeShared<FJsonValueString>(TEXT("suggested_spawn_zones: ranked list of spawn locations")));
		Outputs.Add(MakeShared<FJsonValueString>(TEXT("pacing_state: current phase (buildup/encounter/cooldown)")));
		Hook->SetArrayField(TEXT("outputs"), Outputs);

		Hook->SetStringField(TEXT("dependency"), TEXT("Requires AI Director subsystem (not yet implemented)"));
		AddHook(TEXT("ai_director"), Hook);
	}

	if (HookName == TEXT("all") || HookName == TEXT("gas_tension"))
	{
		auto Hook = MakeShared<FJsonObject>();
		Hook->SetStringField(TEXT("name"), TEXT("gas_tension"));
		Hook->SetStringField(TEXT("description"), TEXT("GAS (Gameplay Ability System) integration for spatial tension effects. Applies gameplay effects based on environmental tension scoring."));
		Hook->SetStringField(TEXT("planned_interface"), TEXT("apply_tension_effects({ player_actor, region? }) -> { applied_effects[], tension_score, modifiers[] }"));

		TArray<TSharedPtr<FJsonValue>> Inputs;
		Inputs.Add(MakeShared<FJsonValueString>(TEXT("player_actor: AActor* — the player to apply effects to")));
		Inputs.Add(MakeShared<FJsonValueString>(TEXT("region: FBox (optional) — override analysis region")));
		Hook->SetArrayField(TEXT("inputs"), Inputs);

		TArray<TSharedPtr<FJsonValue>> Outputs;
		Outputs.Add(MakeShared<FJsonValueString>(TEXT("applied_effects: list of GE tags applied (e.g. Effect.Tension.Dread)")));
		Outputs.Add(MakeShared<FJsonValueString>(TEXT("tension_score: 0-100 from spatial analysis")));
		Outputs.Add(MakeShared<FJsonValueString>(TEXT("modifiers: attribute modifiers applied (move speed, FOV, etc.)")));
		Hook->SetArrayField(TEXT("outputs"), Outputs);

		Hook->SetStringField(TEXT("dependency"), TEXT("Requires GAS foundation (attribute set, effect definitions) — see GAS audit"));
		AddHook(TEXT("gas_tension"), Hook);
	}

	if (HookName == TEXT("all") || HookName == TEXT("telemetry"))
	{
		auto Hook = MakeShared<FJsonObject>();
		Hook->SetStringField(TEXT("name"), TEXT("telemetry"));
		Hook->SetStringField(TEXT("description"), TEXT("Telemetry feedback loop: record spatial events (scare effectiveness, player movement, death locations) and feed back into horror analysis for data-driven tuning."));
		Hook->SetStringField(TEXT("planned_interface"), TEXT("record_spatial_event({ event_type, location, player_state, metadata }) + query_heatmap({ event_type, region }) -> { heatmap_data, hotspots[] }"));

		TArray<TSharedPtr<FJsonValue>> EventTypes;
		EventTypes.Add(MakeShared<FJsonValueString>(TEXT("scare_reaction: player response to a scare event")));
		EventTypes.Add(MakeShared<FJsonValueString>(TEXT("death_location: where the player died")));
		EventTypes.Add(MakeShared<FJsonValueString>(TEXT("movement_stall: where players hesitate or turn back")));
		EventTypes.Add(MakeShared<FJsonValueString>(TEXT("resource_pickup: item collection locations")));
		EventTypes.Add(MakeShared<FJsonValueString>(TEXT("monster_encounter: sighting/engagement locations")));
		Hook->SetArrayField(TEXT("event_types"), EventTypes);

		Hook->SetStringField(TEXT("dependency"), TEXT("Requires telemetry subsystem + data storage (local SQLite or cloud). Hospice mode: anonymized, no PII."));
		AddHook(TEXT("telemetry"), Hook);
	}

	if (!Result->HasField(TEXT("ai_director")) && !Result->HasField(TEXT("gas_tension")) && !Result->HasField(TEXT("telemetry")))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown hook_name: %s. Use ai_director, gas_tension, telemetry, or all"), *HookName));
	}

	return FMonolithActionResult::Success(Result);
}
