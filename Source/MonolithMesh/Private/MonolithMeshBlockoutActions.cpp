#include "MonolithMeshBlockoutActions.h"
#include "MaterialDomain.h"
#include "SceneTypes.h"
#include "Materials/Material.h"
#include "MonolithMeshSceneActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithMeshCatalog.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexDatabase.h"
#include "MonolithSettings.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/BlockingVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Volume.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BrushComponent.h"
#include "CollisionQueryParams.h"
#include "WorldCollision.h"
#include "Engine/OverlapResult.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Editor.h"
#include "SQLiteDatabase.h"
#include "Math/RandomStream.h"

// ============================================================================
// Static members
// ============================================================================

TMap<FString, TWeakObjectPtr<UMaterialInstanceDynamic>> FMonolithMeshBlockoutActions::CachedBlockoutMaterials;

// ============================================================================
// FScopedMeshTransaction
// ============================================================================

FMonolithMeshBlockoutActions::FScopedMeshTransaction::FScopedMeshTransaction(const FText& Description)
	: bOwnsTransaction(!FMonolithMeshSceneActions::bBatchTransactionActive)
{
	if (bOwnsTransaction && GEditor)
	{
		GEditor->BeginTransaction(Description);
	}
}

FMonolithMeshBlockoutActions::FScopedMeshTransaction::~FScopedMeshTransaction()
{
	if (bOwnsTransaction && GEditor)
	{
		GEditor->EndTransaction();
	}
}

void FMonolithMeshBlockoutActions::FScopedMeshTransaction::Cancel()
{
	if (bOwnsTransaction && GEditor)
	{
		GEditor->CancelTransaction(0);
		bOwnsTransaction = false;
	}
}

// ============================================================================
// Helpers
// ============================================================================

TArray<TSharedPtr<FJsonValue>> FMonolithMeshBlockoutActions::VectorToJsonArray(const FVector& V)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Add(MakeShared<FJsonValueNumber>(V.X));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
	return Arr;
}

TArray<TSharedPtr<FJsonValue>> FMonolithMeshBlockoutActions::RotatorToJsonArray(const FRotator& R)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
	Arr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
	Arr.Add(MakeShared<FJsonValueNumber>(R.Roll));
	return Arr;
}

// ============================================================================
// Overlap warning helper — checks if a newly spawned actor overlaps existing
// non-blockout geometry and returns a warning string (empty if no overlap).
// ============================================================================

static FString CheckBlockoutOverlap(UWorld* World, AActor* SpawnedActor, const FString& Label)
{
	if (!World || !SpawnedActor)
	{
		return FString();
	}

	// Get the actor's bounding box for the overlap query
	FVector Origin, Extent;
	SpawnedActor->GetActorBounds(false, Origin, Extent);

	// Minimum extent to avoid degenerate queries on flat shapes
	Extent = Extent.ComponentMax(FVector(1.0f));

	FCollisionShape BoxShape = FCollisionShape::MakeBox(Extent);
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MonolithBlockoutOverlap), true);
	QueryParams.AddIgnoredActor(SpawnedActor);

	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByChannel(Overlaps, Origin, SpawnedActor->GetActorQuat(), ECC_WorldStatic, BoxShape, QueryParams);

	// Filter: only warn about non-blockout actors (skip other Monolith primitives)
	TArray<FString> OverlappingNames;
	TSet<AActor*> SeenActors;
	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Other = Overlap.GetActor();
		if (!Other || SeenActors.Contains(Other))
		{
			continue;
		}
		SeenActors.Add(Other);

		// Skip other blockout primitives — those are expected to overlap
		if (Other->Tags.Contains(FName(TEXT("Monolith.BlockoutPrimitive"))) ||
			Other->Tags.Contains(FName(TEXT("Monolith.ScatteredProp"))))
		{
			continue;
		}

		OverlappingNames.Add(Other->GetActorNameOrLabel());
	}

	if (OverlappingNames.Num() == 0)
	{
		return FString();
	}

	// Build warning with up to 3 actor names to keep it readable
	FString ActorList;
	const int32 MaxNames = 3;
	for (int32 i = 0; i < FMath::Min(OverlappingNames.Num(), MaxNames); ++i)
	{
		if (i > 0) ActorList += TEXT(", ");
		ActorList += FString::Printf(TEXT("'%s'"), *OverlappingNames[i]);
	}
	if (OverlappingNames.Num() > MaxNames)
	{
		ActorList += FString::Printf(TEXT(" (+%d more)"), OverlappingNames.Num() - MaxNames);
	}

	return FString::Printf(TEXT("Primitive '%s' overlaps existing actor %s"), *Label, *ActorList);
}

ABlockingVolume* FMonolithMeshBlockoutActions::FindBlockingVolume(const FString& VolumeName, FString& OutError)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		OutError = TEXT("No editor world available");
		return nullptr;
	}

	TArray<ABlockingVolume*> Matches;
	for (TActorIterator<ABlockingVolume> It(World); It; ++It)
	{
		if (It->GetActorNameOrLabel() == VolumeName || It->GetActorLabel() == VolumeName || It->GetFName().ToString() == VolumeName)
		{
			Matches.Add(*It);
		}
	}

	if (Matches.Num() == 0)
	{
		OutError = FString::Printf(TEXT("BlockingVolume not found: %s"), *VolumeName);
		return nullptr;
	}

	if (Matches.Num() > 1)
	{
		OutError = FString::Printf(TEXT("Ambiguous volume name '%s'. %d matches found:"), *VolumeName, Matches.Num());
		for (ABlockingVolume* V : Matches)
		{
			OutError += FString::Printf(TEXT("\n  - %s (%s)"), *V->GetPathName(), *V->GetActorLabel());
		}
		return nullptr;
	}

	return Matches[0];
}

AActor* FMonolithMeshBlockoutActions::FindBlockoutVolumeAny(const FString& VolumeName, FString& OutError)
{
	// Try tag-based ABlockingVolume first (existing path)
	ABlockingVolume* TagVolume = FindBlockingVolume(VolumeName, OutError);
	if (TagVolume)
	{
		return TagVolume;
	}

	// Try Blueprint-based MonolithBlockout actors
	OutError.Reset();
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		OutError = TEXT("No editor world available");
		return nullptr;
	}

	TArray<AActor*> BPMatches;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor->GetClass()->GetName().Contains(TEXT("MonolithBlockout")))
		{
			continue;
		}
		if (Actor->GetClass()->FindPropertyByName(TEXT("RoomType")) == nullptr)
		{
			continue;
		}
		if (Actor->GetActorNameOrLabel() == VolumeName || Actor->GetActorLabel() == VolumeName || Actor->GetFName().ToString() == VolumeName)
		{
			BPMatches.Add(Actor);
		}
	}

	if (BPMatches.Num() == 0)
	{
		OutError = FString::Printf(TEXT("Blockout volume not found: %s (checked ABlockingVolume tags and BP_MonolithBlockoutVolume actors)"), *VolumeName);
		return nullptr;
	}

	if (BPMatches.Num() > 1)
	{
		OutError = FString::Printf(TEXT("Ambiguous volume name '%s'. %d BP matches found:"), *VolumeName, BPMatches.Num());
		for (AActor* V : BPMatches)
		{
			OutError += FString::Printf(TEXT("\n  - %s (%s)"), *V->GetPathName(), *V->GetActorLabel());
		}
		return nullptr;
	}

	return BPMatches[0];
}

UMaterialInstanceDynamic* FMonolithMeshBlockoutActions::GetBlockoutMaterial(const FString& Category)
{
	// Check cache first
	if (TWeakObjectPtr<UMaterialInstanceDynamic>* Existing = CachedBlockoutMaterials.Find(Category))
	{
		if (Existing->IsValid())
		{
			return Existing->Get();
		}
	}

	// Category colors
	FLinearColor Color = FLinearColor(0.5f, 0.5f, 0.5f, 0.6f); // gray default
	FString LowerCat = Category.ToLower();
	if (LowerCat.Contains(TEXT("furniture")) || LowerCat.Contains(TEXT("blue")))
	{
		Color = FLinearColor(0.2f, 0.4f, 0.9f, 0.6f);
	}
	else if (LowerCat.Contains(TEXT("prop")) || LowerCat.Contains(TEXT("green")))
	{
		Color = FLinearColor(0.2f, 0.8f, 0.3f, 0.6f);
	}
	else if (LowerCat.Contains(TEXT("hazard")) || LowerCat.Contains(TEXT("red")))
	{
		Color = FLinearColor(0.9f, 0.2f, 0.2f, 0.6f);
	}

	// Create transient MID — no parent material needed, just use the default engine wireframe
	// Use WorldGridMaterial as base — it's always available
	UMaterialInterface* BaseMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial"));
	if (!BaseMat)
	{
		// Fallback to any available material
		BaseMat = UMaterial::GetDefaultMaterial(MD_Surface);
	}

	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMat, GetTransientPackage());
	if (MID)
	{
		MID->SetVectorParameterValue(TEXT("Color"), Color);
		CachedBlockoutMaterials.Add(Category, MID);
	}

	return MID;
}

bool FMonolithMeshBlockoutActions::HasMonolithTag(const AActor* Actor, const FString& TagPrefix)
{
	if (!Actor) return false;
	for (const FName& Tag : Actor->Tags)
	{
		if (Tag.ToString().StartsWith(TagPrefix, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

FString FMonolithMeshBlockoutActions::GetMonolithTagValue(const AActor* Actor, const FString& TagPrefix)
{
	if (!Actor) return FString();
	for (const FName& Tag : Actor->Tags)
	{
		FString TagStr = Tag.ToString();
		if (TagStr.StartsWith(TagPrefix, ESearchCase::IgnoreCase))
		{
			int32 ColonIdx;
			if (TagStr.FindChar(TEXT(':'), ColonIdx))
			{
				return TagStr.Mid(ColonIdx + 1);
			}
			return FString();
		}
	}
	return FString();
}

FString FMonolithMeshBlockoutActions::GetBasicShapePath(const FString& ShapeName, bool& bValid)
{
	bValid = true;
	FString Lower = ShapeName.ToLower();
	if (Lower == TEXT("box") || Lower == TEXT("cube"))   return TEXT("/Engine/BasicShapes/Cube.Cube");
	if (Lower == TEXT("cylinder"))                        return TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
	if (Lower == TEXT("sphere"))                          return TEXT("/Engine/BasicShapes/Sphere.Sphere");
	if (Lower == TEXT("cone"))                            return TEXT("/Engine/BasicShapes/Cone.Cone");
	if (Lower == TEXT("wedge"))                           return TEXT("/Engine/BasicShapes/Cube.Cube"); // scaled cube
	bValid = false;
	return FString();
}

FVector FMonolithMeshBlockoutActions::GetBlockoutActorSize(const AActor* Actor)
{
	if (!Actor) return FVector::ZeroVector;

	// BasicShapes are 100x100x100 unit cubes, so actual size = scale * 100
	FVector Scale = Actor->GetActorScale3D();

	UStaticMeshComponent* SMC = Actor->FindComponentByClass<UStaticMeshComponent>();
	if (SMC && SMC->GetStaticMesh())
	{
		FBoxSphereBounds MeshBounds = SMC->GetStaticMesh()->GetBounds();
		return MeshBounds.BoxExtent * 2.0 * Scale;
	}

	// Fallback: basic shape default is 100cm extent per axis (50 half-extent * 2)
	return Scale * 100.0;
}

namespace BlockoutHelpers
{
	/** Get catalog database, or nullptr */
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

	/** Sort 3 floats smallest to largest */
	void SortAxes(float& A, float& B, float& C)
	{
		if (A > B) Swap(A, B);
		if (B > C) Swap(B, C);
		if (A > B) Swap(A, B);
	}
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshBlockoutActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 1. get_blockout_volumes
	Registry.RegisterAction(TEXT("mesh"), TEXT("get_blockout_volumes"),
		TEXT("Find all actors with Monolith.Blockout tag. Warns about misconfigured actors with partial Monolith tags."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::GetBlockoutVolumes),
		FParamSchemaBuilder()
			.Build());

	// 2. get_blockout_volume_info
	Registry.RegisterAction(TEXT("mesh"), TEXT("get_blockout_volume_info"),
		TEXT("Get detailed info for a blockout volume: parsed tags, blockout primitives, and other actors."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::GetBlockoutVolumeInfo),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Name or label of the blockout volume"))
			.Build());

	// 3. setup_blockout_volume
	Registry.RegisterAction(TEXT("mesh"), TEXT("setup_blockout_volume"),
		TEXT("Apply Monolith blockout tags to a BlockingVolume. Clears existing Monolith tags first."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::SetupBlockoutVolume),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Name or label of the ABlockingVolume"))
			.Required(TEXT("room_type"), TEXT("string"), TEXT("Room type (e.g. Kitchen, Bathroom, Hallway)"))
			.Optional(TEXT("tags"), TEXT("array"), TEXT("Array of content tag strings (e.g. ['Furniture.Kitchen', 'Props.Food'])"))
			.Optional(TEXT("density"), TEXT("string"), TEXT("Density hint: Sparse, Normal, Dense, Cluttered"), TEXT("Normal"))
			.Optional(TEXT("allow_physics"), TEXT("boolean"), TEXT("Whether spawned props should simulate physics"), TEXT("false"))
			.Optional(TEXT("floor_height"), TEXT("number"), TEXT("Floor Z offset within volume"), TEXT("0"))
			.Build());

	// 4. create_blockout_primitive
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_blockout_primitive"),
		TEXT("Spawn a scaled BasicShape as a blockout primitive with category-colored material and owner tags."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::CreateBlockoutPrimitive),
		FParamSchemaBuilder()
			.Required(TEXT("shape"), TEXT("string"), TEXT("Shape type: box, cylinder, sphere, cone, wedge"))
			.Required(TEXT("location"), TEXT("array"), TEXT("World location [x, y, z]"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("Rotation [pitch, yaw, roll]"), TEXT("[0,0,0]"))
			.Required(TEXT("scale"), TEXT("array"), TEXT("Scale [x, y, z]"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("Descriptive label for this primitive"))
			.Optional(TEXT("volume_name"), TEXT("string"), TEXT("Owner volume name (sets Monolith.Owner tag)"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Category for material color (furniture=blue, prop=green, hazard=red)"), TEXT("default"))
			.Build());

	// 5. create_blockout_primitives_batch
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_blockout_primitives_batch"),
		TEXT("Create multiple blockout primitives in a single undo transaction. Max 200 primitives. Warns if outside volume bounds."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::CreateBlockoutPrimitivesBatch),
		FParamSchemaBuilder()
			.Required(TEXT("primitives"), TEXT("array"), TEXT("Array of {shape, location, rotation, scale, label, category} objects. Max 200."))
			.Optional(TEXT("volume_name"), TEXT("string"), TEXT("Owner volume name"))
			.Build());

	// 6. create_blockout_grid
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_blockout_grid"),
		TEXT("Create a floor grid of box primitives filling a blockout volume."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::CreateBlockoutGrid),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Volume to fill with grid"))
			.Required(TEXT("cell_size"), TEXT("number"), TEXT("Grid cell size in cm"))
			.Optional(TEXT("wall_thickness"), TEXT("number"), TEXT("Wall outline thickness in cm"), TEXT("10"))
			.Build());

	// 7. match_asset_to_blockout
	Registry.RegisterAction(TEXT("mesh"), TEXT("match_asset_to_blockout"),
		TEXT("Find mesh catalog assets that match a blockout primitive's size. Axis-sorted matching with weighted scoring."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::MatchAssetToBlockout),
		FParamSchemaBuilder()
			.Required(TEXT("blockout_actor"), TEXT("string"), TEXT("Name of the blockout primitive actor"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Filter by asset category"))
			.Optional(TEXT("tolerance_pct"), TEXT("number"), TEXT("Size match tolerance percentage"), TEXT("20"))
			.Optional(TEXT("top_n"), TEXT("integer"), TEXT("Number of top matches to return"), TEXT("3"))
			.Build());

	// 8. match_all_in_volume
	Registry.RegisterAction(TEXT("mesh"), TEXT("match_all_in_volume"),
		TEXT("Batch match all blockout primitives in a volume against mesh catalog. Returns replacement plan."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::MatchAllInVolume),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Volume containing blockout primitives"))
			.Optional(TEXT("tolerance_pct"), TEXT("number"), TEXT("Size match tolerance percentage"), TEXT("20"))
			.Optional(TEXT("top_n"), TEXT("integer"), TEXT("Top matches per primitive"), TEXT("3"))
			.Build());

	// 9. apply_replacement
	Registry.RegisterAction(TEXT("mesh"), TEXT("apply_replacement"),
		TEXT("ATOMIC: Replace blockout primitives with real mesh assets. Validates ALL assets first, single undo, pivot adjustment in local space."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::ApplyReplacement),
		FParamSchemaBuilder()
			.Required(TEXT("replacements"), TEXT("array"), TEXT("Array of {blockout_actor, replacement_asset} objects"))
			.Optional(TEXT("volume_name"), TEXT("string"), TEXT("Volume name for post-replacement tag cleanup"))
			.Build());

	// 10. set_actor_tags
	Registry.RegisterAction(TEXT("mesh"), TEXT("set_actor_tags"),
		TEXT("Batch apply tags to multiple actors in a single undo transaction."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::SetActorTags),
		FParamSchemaBuilder()
			.Required(TEXT("actor_tags"), TEXT("array"), TEXT("Array of {actor, tags} objects"))
			.Build());

	// 11. clear_blockout
	Registry.RegisterAction(TEXT("mesh"), TEXT("clear_blockout"),
		TEXT("Delete blockout primitives by Monolith.Owner tag. Respects keep_tagged to preserve replaced actors."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::ClearBlockout),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Volume whose primitives to clear"))
			.Optional(TEXT("keep_tagged"), TEXT("boolean"), TEXT("If true, keep actors that are NOT Monolith.BlockoutPrimitive (already replaced)"), TEXT("false"))
			.Build());

	// 12. export_blockout_layout
	Registry.RegisterAction(TEXT("mesh"), TEXT("export_blockout_layout"),
		TEXT("Export blockout layout as JSON. Positions normalized 0-1 relative to volume, sizes absolute."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::ExportBlockoutLayout),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Volume to export"))
			.Build());

	// 13. import_blockout_layout
	Registry.RegisterAction(TEXT("mesh"), TEXT("import_blockout_layout"),
		TEXT("Import a blockout layout into a volume. Scales POSITIONS to fit, keeps SIZES unchanged. Flags overflow."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::ImportBlockoutLayout),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Target volume"))
			.Required(TEXT("layout_json"), TEXT("any"), TEXT("Layout JSON object or string (from export_blockout_layout)"))
			.Build());

	// 14. scan_volume
	Registry.RegisterAction(TEXT("mesh"), TEXT("scan_volume"),
		TEXT("Multi-origin radial sweep of a volume. Detects walls, floor, ceiling, openings. Semantic JSON output."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::ScanVolume),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Volume to scan"))
			.Optional(TEXT("ray_density"), TEXT("string"), TEXT("low, medium, or high"), TEXT("medium"))
			.Optional(TEXT("vertical_layers"), TEXT("integer"), TEXT("Number of vertical elevation layers"), TEXT("3"))
			.Build());

	// 15. scatter_props
	Registry.RegisterAction(TEXT("mesh"), TEXT("scatter_props"),
		TEXT("Poisson disk scatter props within a volume. Floor trace, overlap check, random rotation, reproducible seed."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::ScatterProps),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Volume to scatter into"))
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Array of StaticMesh asset paths to scatter"))
			.Required(TEXT("count"), TEXT("integer"), TEXT("Target number of props to place"))
			.Optional(TEXT("min_spacing"), TEXT("number"), TEXT("Minimum distance between props in cm"), TEXT("50"))
			.Optional(TEXT("random_rotation"), TEXT("boolean"), TEXT("Apply random yaw rotation"), TEXT("true"))
			.Optional(TEXT("random_scale_range"), TEXT("array"), TEXT("Scale range [min, max]"), TEXT("[0.9, 1.1]"))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("Random seed for reproducibility (0 = random)"), TEXT("0"))
			.Optional(TEXT("surface_align"), TEXT("boolean"), TEXT("Align to surface normal via floor trace"), TEXT("false"))
			.Optional(TEXT("collision_mode"), TEXT("string"), TEXT("Collision handling: none (skip validation), warn (validate+place anyway), reject (skip overlapping), adjust (push-out then reject)"), TEXT("warn"))
			.Build());

	// 16. create_blockout_blueprint
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_blockout_blueprint"),
		TEXT("Create the BP_MonolithBlockoutVolume Blueprint asset in the project. One-time setup — creates /Game/Monolith/Blockout/BP_MonolithBlockoutVolume with editable RoomType, BlockoutTags, Density, physics, wall/ceiling properties. Drag into levels for blockout volumes with proper Details panel UX."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshBlockoutActions::CreateBlockoutBlueprint),
		FParamSchemaBuilder()
			.OptionalAssetPathWithDefault(TEXT("save_path"), TEXT("Asset path to save the Blueprint"), TEXT("/Game/Monolith/Blockout/BP_MonolithBlockoutVolume"))
			.Optional(TEXT("force"), TEXT("boolean"), TEXT("Recreate even if already exists"), TEXT("false"))
			.Build());
}

// ============================================================================
// 1. get_blockout_volumes
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::GetBlockoutVolumes(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	TArray<TSharedPtr<FJsonValue>> VolumesArr;
	TArray<TSharedPtr<FJsonValue>> WarningsArr;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		bool bHasBlockoutSentinel = HasMonolithTag(Actor, TEXT("Monolith.Blockout"));
		bool bHasAnyMonolithTag = false;

		// Check for Blueprint-based blockout volume (has RoomType property + MonolithBlockout class name)
		bool bIsBPBlockout = false;
		if (!bHasBlockoutSentinel
			&& Actor->GetClass()->GetName().Contains(TEXT("MonolithBlockout"))
			&& Actor->GetClass()->FindPropertyByName(TEXT("RoomType")) != nullptr)
		{
			bIsBPBlockout = true;
		}

		for (const FName& Tag : Actor->Tags)
		{
			if (Tag.ToString().StartsWith(TEXT("Monolith."), ESearchCase::IgnoreCase))
			{
				bHasAnyMonolithTag = true;
				break;
			}
		}

		if (bHasBlockoutSentinel || bIsBPBlockout)
		{
			MonolithMeshUtils::FBlockoutTags Parsed = MonolithMeshUtils::ParseBlockoutTags(Actor);

			auto VolObj = MakeShared<FJsonObject>();
			VolObj->SetStringField(TEXT("name"), Actor->GetActorNameOrLabel());
			VolObj->SetStringField(TEXT("room_type"), Parsed.RoomType);

			TArray<TSharedPtr<FJsonValue>> TagsJsonArr;
			for (const FString& T : Parsed.Tags)
			{
				TagsJsonArr.Add(MakeShared<FJsonValueString>(T));
			}
			VolObj->SetArrayField(TEXT("tags"), TagsJsonArr);
			VolObj->SetStringField(TEXT("density"), Parsed.Density.IsEmpty() ? TEXT("Normal") : Parsed.Density);
			VolObj->SetStringField(TEXT("source"), bIsBPBlockout ? TEXT("blueprint") : TEXT("tag"));

			// Bounds
			FVector Origin, Extent;
			Actor->GetActorBounds(false, Origin, Extent);
			VolObj->SetArrayField(TEXT("extent"), VectorToJsonArray(Extent * 2.0));
			VolObj->SetArrayField(TEXT("center"), VectorToJsonArray(Origin));

			// Count primitives owned by this volume
			FString OwnerTag = FString::Printf(TEXT("Monolith.Owner:%s"), *Actor->GetActorNameOrLabel());
			int32 PrimitiveCount = 0;
			for (TActorIterator<AActor> PrimIt(World); PrimIt; ++PrimIt)
			{
				if (HasMonolithTag(*PrimIt, OwnerTag))
				{
					PrimitiveCount++;
				}
			}
			VolObj->SetNumberField(TEXT("primitive_count"), PrimitiveCount);
			VolObj->SetBoolField(TEXT("allow_physics"), Parsed.bAllowPhysics);

			VolumesArr.Add(MakeShared<FJsonValueObject>(VolObj));
		}
		else if (bHasAnyMonolithTag && !bHasBlockoutSentinel)
		{
			// Warn about actors with Monolith tags but no sentinel
			WarningsArr.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Actor '%s' has Monolith.* tags but missing Monolith.Blockout sentinel"),
					*Actor->GetActorNameOrLabel())));
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("volumes"), VolumesArr);
	Result->SetArrayField(TEXT("warnings"), WarningsArr);
	Result->SetNumberField(TEXT("count"), VolumesArr.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 2. get_blockout_volume_info
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::GetBlockoutVolumeInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString VolumeName;
	if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: volume_name"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Find the volume — can be any actor with Monolith.Blockout tag
	FString Error;
	AActor* VolumeActor = MonolithMeshUtils::FindActorByName(VolumeName, Error);
	if (!VolumeActor)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Check if it's a blockout volume — either tagged or Blueprint-based
	bool bIsBlockout = HasMonolithTag(VolumeActor, TEXT("Monolith.Blockout"));
	bool bIsBPBlockout = (!bIsBlockout &&
		VolumeActor->GetClass()->FindPropertyByName(TEXT("RoomType")) != nullptr &&
		VolumeActor->GetClass()->GetName().Contains(TEXT("MonolithBlockout")));
	if (!bIsBlockout && !bIsBPBlockout)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Actor '%s' is not a blockout volume (missing Monolith.Blockout tag or BP_MonolithBlockoutVolume class)"), *VolumeName));
	}

	MonolithMeshUtils::FBlockoutTags Parsed = MonolithMeshUtils::ParseBlockoutTags(VolumeActor);

	// Find all actors owned by this volume
	FString OwnerTag = FString::Printf(TEXT("Monolith.Owner:%s"), *VolumeName);

	TArray<TSharedPtr<FJsonValue>> BlockoutPrimitives;
	TArray<TSharedPtr<FJsonValue>> OtherActors;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor == VolumeActor) continue;

		if (!HasMonolithTag(Actor, OwnerTag)) continue;

		auto ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("name"), Actor->GetActorNameOrLabel());
		ActorObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		ActorObj->SetArrayField(TEXT("location"), VectorToJsonArray(Actor->GetActorLocation()));
		ActorObj->SetArrayField(TEXT("scale"), VectorToJsonArray(Actor->GetActorScale3D()));

		FString Label = GetMonolithTagValue(Actor, TEXT("Monolith.Label"));
		if (!Label.IsEmpty())
		{
			ActorObj->SetStringField(TEXT("label"), Label);
		}

		if (HasMonolithTag(Actor, TEXT("Monolith.BlockoutPrimitive")))
		{
			FVector Size = GetBlockoutActorSize(Actor);
			ActorObj->SetArrayField(TEXT("size"), VectorToJsonArray(Size));
			BlockoutPrimitives.Add(MakeShared<FJsonValueObject>(ActorObj));
		}
		else
		{
			OtherActors.Add(MakeShared<FJsonValueObject>(ActorObj));
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("volume"), VolumeName);
	Result->SetStringField(TEXT("room_type"), Parsed.RoomType);

	TArray<TSharedPtr<FJsonValue>> TagsJsonArr;
	for (const FString& T : Parsed.Tags)
	{
		TagsJsonArr.Add(MakeShared<FJsonValueString>(T));
	}
	Result->SetArrayField(TEXT("tags"), TagsJsonArr);
	Result->SetStringField(TEXT("density"), Parsed.Density.IsEmpty() ? TEXT("Normal") : Parsed.Density);
	Result->SetBoolField(TEXT("allow_physics"), Parsed.bAllowPhysics);
	Result->SetArrayField(TEXT("blockout_primitives"), BlockoutPrimitives);
	Result->SetArrayField(TEXT("other_actors"), OtherActors);
	Result->SetNumberField(TEXT("primitive_count"), BlockoutPrimitives.Num());
	Result->SetNumberField(TEXT("other_count"), OtherActors.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 3. setup_blockout_volume
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::SetupBlockoutVolume(const TSharedPtr<FJsonObject>& Params)
{
	FString VolumeName;
	if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: volume_name"));
	}

	FString RoomType;
	if (!Params->TryGetStringField(TEXT("room_type"), RoomType) || RoomType.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: room_type"));
	}

	FString Error;
	ABlockingVolume* Volume = FindBlockingVolume(VolumeName, Error);
	if (!Volume)
	{
		return FMonolithActionResult::Error(Error);
	}

	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Setup Blockout Volume")));

	// Clear existing Monolith.* tags
	Volume->Tags.RemoveAll([](const FName& Tag)
	{
		return Tag.ToString().StartsWith(TEXT("Monolith."), ESearchCase::IgnoreCase);
	});

	// Apply sentinel
	Volume->Tags.Add(FName(TEXT("Monolith.Blockout")));

	// Room type
	Volume->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Room:%s"), *RoomType)));

	// Content tags
	const TArray<TSharedPtr<FJsonValue>>* TagsArr;
	if (Params->TryGetArrayField(TEXT("tags"), TagsArr))
	{
		for (const auto& Val : *TagsArr)
		{
			Volume->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Tag:%s"), *Val->AsString())));
		}
	}

	// Density
	FString Density = TEXT("Normal");
	Params->TryGetStringField(TEXT("density"), Density);
	Volume->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Density:%s"), *Density)));

	// Allow physics
	bool bAllowPhysics = false;
	Params->TryGetBoolField(TEXT("allow_physics"), bAllowPhysics);
	if (bAllowPhysics)
	{
		Volume->Tags.Add(FName(TEXT("Monolith.AllowPhysics")));
	}

	// Floor height
	double FloorHeight = 0.0;
	if (Params->TryGetNumberField(TEXT("floor_height"), FloorHeight) && FloorHeight != 0.0)
	{
		Volume->Tags.Add(FName(*FString::Printf(TEXT("Monolith.FloorHeight:%.1f"), FloorHeight)));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("volume"), Volume->GetActorNameOrLabel());
	Result->SetStringField(TEXT("room_type"), RoomType);
	Result->SetNumberField(TEXT("tags_applied"), Volume->Tags.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 4. create_blockout_primitive
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::CreateBlockoutPrimitive(const TSharedPtr<FJsonObject>& Params)
{
	FString ShapeName;
	if (!Params->TryGetStringField(TEXT("shape"), ShapeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: shape"));
	}

	bool bValidShape = false;
	FString MeshPath = GetBasicShapePath(ShapeName, bValidShape);
	if (!bValidShape)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid shape: '%s'. Valid: box, cylinder, sphere, cone, wedge"), *ShapeName));
	}

	FVector Location;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: location (array of 3 numbers)"));
	}

	FVector Scale;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("scale"), Scale))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: scale (array of 3 numbers)"));
	}

	FRotator Rotation(0, 0, 0);
	MonolithMeshUtils::ParseRotator(Params, TEXT("rotation"), Rotation);

	FString Label;
	Params->TryGetStringField(TEXT("label"), Label);

	FString VolumeName;
	Params->TryGetStringField(TEXT("volume_name"), VolumeName);

	FString Category = TEXT("default");
	Params->TryGetStringField(TEXT("category"), Category);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	UStaticMesh* ShapeMesh = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(MeshPath);
	if (!ShapeMesh)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load BasicShape mesh: %s"), *MeshPath));
	}

	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Create Blockout Primitive")));

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, Rotation, SpawnParams);
	if (!Actor)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(TEXT("Failed to spawn StaticMeshActor"));
	}

	Actor->GetStaticMeshComponent()->SetStaticMesh(ShapeMesh);
	Actor->SetActorScale3D(Scale);

	// Apply blockout material
	UMaterialInstanceDynamic* BlockoutMat = GetBlockoutMaterial(Category);
	if (BlockoutMat)
	{
		Actor->GetStaticMeshComponent()->SetMaterial(0, BlockoutMat);
	}

	// Apply tags — one FName per tag entry, NOT comma-separated
	Actor->Tags.Add(FName(TEXT("Monolith.BlockoutPrimitive")));

	if (!VolumeName.IsEmpty())
	{
		Actor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Owner:%s"), *VolumeName)));
	}

	if (!Label.IsEmpty())
	{
		Actor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Label:%s"), *Label)));
		Actor->SetActorLabel(Label);
	}

	Actor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Shape:%s"), *ShapeName.ToLower())));
	Actor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Category:%s"), *Category)));

	// Set folder for organization
	if (!VolumeName.IsEmpty())
	{
		Actor->SetFolderPath(FName(*FString::Printf(TEXT("Blockout/%s"), *VolumeName)));
	}
	else
	{
		Actor->SetFolderPath(FName(TEXT("Blockout")));
	}

	// Check for overlaps with existing non-blockout geometry
	TArray<TSharedPtr<FJsonValue>> Warnings;
	FString OverlapLabel = Label.IsEmpty() ? Actor->GetActorNameOrLabel() : Label;
	FString OverlapWarning = CheckBlockoutOverlap(World, Actor, OverlapLabel);
	if (!OverlapWarning.IsEmpty())
	{
		Warnings.Add(MakeShared<FJsonValueString>(OverlapWarning));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	Result->SetStringField(TEXT("shape"), ShapeName);
	Result->SetArrayField(TEXT("location"), VectorToJsonArray(Actor->GetActorLocation()));
	Result->SetArrayField(TEXT("scale"), VectorToJsonArray(Scale));
	Result->SetStringField(TEXT("category"), Category);
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), Warnings);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 5. create_blockout_primitives_batch
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::CreateBlockoutPrimitivesBatch(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* PrimitivesArr;
	if (!Params->TryGetArrayField(TEXT("primitives"), PrimitivesArr) || PrimitivesArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: primitives"));
	}

	if (PrimitivesArr->Num() > 200)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Batch limited to 200 primitives, got %d"), PrimitivesArr->Num()));
	}

	FString VolumeName;
	Params->TryGetStringField(TEXT("volume_name"), VolumeName);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Get volume bounds for overflow checking
	FVector VolumeOrigin = FVector::ZeroVector, VolumeExtent = FVector::ZeroVector;
	bool bHasVolume = false;
	if (!VolumeName.IsEmpty())
	{
		FString VolumeError;
		ABlockingVolume* Volume = FindBlockingVolume(VolumeName, VolumeError);
		if (Volume)
		{
			Volume->GetActorBounds(false, VolumeOrigin, VolumeExtent);
			bHasVolume = true;
		}
	}

	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Create Blockout Primitives Batch")));

	int32 Created = 0;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	for (int32 i = 0; i < PrimitivesArr->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>* PrimObj;
		if (!(*PrimitivesArr)[i]->TryGetObject(PrimObj))
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid primitive at index %d — expected object"), i));
		}

		// Build sub-params
		auto SubParams = MakeShared<FJsonObject>();
		SubParams->Values = (*PrimObj)->Values; // copy all fields
		if (!VolumeName.IsEmpty() && !SubParams->HasField(TEXT("volume_name")))
		{
			SubParams->SetStringField(TEXT("volume_name"), VolumeName);
		}

		// Validate shape before spawn
		FString ShapeName;
		if (!SubParams->TryGetStringField(TEXT("shape"), ShapeName))
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Primitive at index %d missing 'shape'"), i));
		}

		bool bValidShape = false;
		FString MeshPath = GetBasicShapePath(ShapeName, bValidShape);
		if (!bValidShape)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Primitive at index %d: invalid shape '%s'"), i, *ShapeName));
		}

		FVector Location;
		if (!MonolithMeshUtils::ParseVector(SubParams, TEXT("location"), Location))
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Primitive at index %d missing valid 'location'"), i));
		}

		FVector Scale;
		if (!MonolithMeshUtils::ParseVector(SubParams, TEXT("scale"), Scale))
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Primitive at index %d missing valid 'scale'"), i));
		}

		// Check if outside volume bounds
		if (bHasVolume)
		{
			FVector Diff = (Location - VolumeOrigin).GetAbs();
			float OverflowDist = 0.0f;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				float Overflow = Diff[Axis] - VolumeExtent[Axis];
				if (Overflow > 0.0f)
				{
					OverflowDist = FMath::Max(OverflowDist, Overflow);
				}
			}
			if (OverflowDist > 0.0f)
			{
				FString PrimLabel;
				SubParams->TryGetStringField(TEXT("label"), PrimLabel);
				if (PrimLabel.IsEmpty()) PrimLabel = FString::Printf(TEXT("index_%d"), i);
				Warnings.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Primitive '%s' is %.0fcm outside volume bounds"), *PrimLabel, OverflowDist)));
			}
		}

		// Spawn
		UStaticMesh* ShapeMesh = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(MeshPath);
		if (!ShapeMesh)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load mesh: %s"), *MeshPath));
		}

		FRotator Rotation(0, 0, 0);
		MonolithMeshUtils::ParseRotator(SubParams, TEXT("rotation"), Rotation);

		FString Label;
		SubParams->TryGetStringField(TEXT("label"), Label);

		FString Category = TEXT("default");
		SubParams->TryGetStringField(TEXT("category"), Category);

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, Rotation, SpawnParams);
		if (!Actor)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to spawn primitive at index %d"), i));
		}

		Actor->GetStaticMeshComponent()->SetStaticMesh(ShapeMesh);
		Actor->SetActorScale3D(Scale);

		UMaterialInstanceDynamic* BlockoutMat = GetBlockoutMaterial(Category);
		if (BlockoutMat)
		{
			Actor->GetStaticMeshComponent()->SetMaterial(0, BlockoutMat);
		}

		Actor->Tags.Add(FName(TEXT("Monolith.BlockoutPrimitive")));
		if (!VolumeName.IsEmpty())
		{
			Actor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Owner:%s"), *VolumeName)));
		}
		if (!Label.IsEmpty())
		{
			Actor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Label:%s"), *Label)));
			Actor->SetActorLabel(Label);
		}
		Actor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Shape:%s"), *ShapeName.ToLower())));
		Actor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Category:%s"), *Category)));

		if (!VolumeName.IsEmpty())
		{
			Actor->SetFolderPath(FName(*FString::Printf(TEXT("Blockout/%s"), *VolumeName)));
		}

		// Check for overlaps with existing non-blockout geometry
		FString OverlapLabel = Label.IsEmpty() ? FString::Printf(TEXT("index_%d"), i) : Label;
		FString OverlapWarning = CheckBlockoutOverlap(World, Actor, OverlapLabel);
		if (!OverlapWarning.IsEmpty())
		{
			Warnings.Add(MakeShared<FJsonValueString>(OverlapWarning));
		}

		Created++;
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("created"), Created);
	Result->SetArrayField(TEXT("warnings"), Warnings);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 6. create_blockout_grid
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::CreateBlockoutGrid(const TSharedPtr<FJsonObject>& Params)
{
	FString VolumeName;
	if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: volume_name"));
	}

	double CellSize = 0.0;
	if (!Params->TryGetNumberField(TEXT("cell_size"), CellSize) || CellSize <= 0.0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: cell_size (must be > 0)"));
	}

	double WallThickness = 10.0;
	Params->TryGetNumberField(TEXT("wall_thickness"), WallThickness);

	FString Error;
	AActor* Volume = FindBlockoutVolumeAny(VolumeName, Error);
	if (!Volume)
	{
		return FMonolithActionResult::Error(Error);
	}

	FVector VolumeOrigin, VolumeExtent;
	Volume->GetActorBounds(false, VolumeOrigin, VolumeExtent);
	FVector VolumeMin = VolumeOrigin - VolumeExtent;
	FVector VolumeSize = VolumeExtent * 2.0;

	// Compute grid dimensions
	int32 CellsX = FMath::Max(1, FMath::FloorToInt32(VolumeSize.X / CellSize));
	int32 CellsY = FMath::Max(1, FMath::FloorToInt32(VolumeSize.Y / CellSize));

	// Cap at reasonable count
	if (CellsX * CellsY > 200)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Grid would create %d cells (max 200). Increase cell_size or use a smaller volume."),
			CellsX * CellsY));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	UStaticMesh* CubeMesh = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!CubeMesh)
	{
		return FMonolithActionResult::Error(TEXT("Failed to load Cube BasicShape"));
	}

	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Create Blockout Grid")));

	UMaterialInstanceDynamic* GridMat = GetBlockoutMaterial(TEXT("default"));
	int32 Created = 0;
	float FloorZ = VolumeMin.Z;

	// BasicShape Cube is 100x100x100, centered at origin
	// Scale to match cell_size x cell_size x wall_thickness
	FVector CellScale(CellSize / 100.0, CellSize / 100.0, WallThickness / 100.0);

	for (int32 xi = 0; xi < CellsX; ++xi)
	{
		for (int32 yi = 0; yi < CellsY; ++yi)
		{
			float CenterX = VolumeMin.X + (xi + 0.5f) * CellSize;
			float CenterY = VolumeMin.Y + (yi + 0.5f) * CellSize;
			FVector CellCenter(CenterX, CenterY, FloorZ + WallThickness * 0.5);

			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

			AStaticMeshActor* Cell = World->SpawnActor<AStaticMeshActor>(CellCenter, FRotator::ZeroRotator, SpawnParams);
			if (!Cell) continue;

			Cell->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
			Cell->SetActorScale3D(CellScale);

			if (GridMat)
			{
				Cell->GetStaticMeshComponent()->SetMaterial(0, GridMat);
			}

			Cell->Tags.Add(FName(TEXT("Monolith.BlockoutPrimitive")));
			Cell->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Owner:%s"), *VolumeName)));
			Cell->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Label:Grid_%d_%d"), xi, yi)));
			Cell->Tags.Add(FName(TEXT("Monolith.Shape:box")));
			Cell->Tags.Add(FName(TEXT("Monolith.Category:grid")));

			Cell->SetActorLabel(FString::Printf(TEXT("Grid_%d_%d"), xi, yi));
			Cell->SetFolderPath(FName(*FString::Printf(TEXT("Blockout/%s/Grid"), *VolumeName)));

			Created++;
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("created"), Created);
	Result->SetNumberField(TEXT("cells_x"), CellsX);
	Result->SetNumberField(TEXT("cells_y"), CellsY);
	Result->SetNumberField(TEXT("cell_size"), CellSize);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 7. match_asset_to_blockout
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::MatchAssetToBlockout(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("blockout_actor"), ActorName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: blockout_actor"));
	}

	FString Error;
	AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	FSQLiteDatabase* DB = BlockoutHelpers::GetCatalogDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Mesh catalog not available. Run monolith_reindex() first."));
	}

	// Get blockout size
	FVector BlockoutSize = GetBlockoutActorSize(Actor);
	if (BlockoutSize.IsNearlyZero())
	{
		return FMonolithActionResult::Error(TEXT("Could not determine blockout actor size"));
	}

	// Parse tolerance
	double TolerancePct = GetDefault<UMonolithSettings>()->DefaultSizeMatchTolerance;
	Params->TryGetNumberField(TEXT("tolerance_pct"), TolerancePct);
	float Tolerance = static_cast<float>(TolerancePct) / 100.0f;

	int32 TopN = 3;
	double TopND;
	if (Params->TryGetNumberField(TEXT("top_n"), TopND))
	{
		TopN = FMath::Clamp(static_cast<int32>(TopND), 1, 50);
	}

	FString Category;
	Params->TryGetStringField(TEXT("category"), Category);

	// Sort blockout axes smallest-to-largest for orientation-independent matching
	float SortedSize[3] = { static_cast<float>(BlockoutSize.X), static_cast<float>(BlockoutSize.Y), static_cast<float>(BlockoutSize.Z) };
	BlockoutHelpers::SortAxes(SortedSize[0], SortedSize[1], SortedSize[2]);

	// Build search bounds with tolerance
	TArray<float> MinBounds, MaxBounds;
	MinBounds.SetNum(3);
	MaxBounds.SetNum(3);
	for (int32 i = 0; i < 3; ++i)
	{
		MinBounds[i] = SortedSize[i] * (1.0f - Tolerance);
		MaxBounds[i] = SortedSize[i] * (1.0f + Tolerance);
	}

	// Query catalog — get more than TopN to allow scoring
	TSharedPtr<FJsonObject> CatalogResults = FMonolithMeshCatalog::SearchBySize(
		*DB, MinBounds, MaxBounds, Category, FString(), TopN * 5);

	const TArray<TSharedPtr<FJsonValue>>* ResultsArr;
	if (!CatalogResults || !CatalogResults->TryGetArrayField(TEXT("results"), ResultsArr))
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("blockout_actor"), ActorName);
		Result->SetArrayField(TEXT("blockout_size"), VectorToJsonArray(BlockoutSize));
		Result->SetArrayField(TEXT("candidates"), TArray<TSharedPtr<FJsonValue>>());
		return FMonolithActionResult::Success(Result);
	}

	// Get blockout actor's category tag for category matching
	FString BlockoutCategory = GetMonolithTagValue(Actor, TEXT("Monolith.Category"));

	// Score candidates
	struct FCandidate
	{
		FString AssetPath;
		FVector Size;
		FString AssetCategory;
		int32 TriCount;
		float Score;
		float SizeDeltaPct;
	};

	TArray<FCandidate> Candidates;
	for (const auto& Val : *ResultsArr)
	{
		const TSharedPtr<FJsonObject>* EntryObj;
		if (!Val->TryGetObject(EntryObj)) continue;

		FCandidate C;
		(*EntryObj)->TryGetStringField(TEXT("asset_path"), C.AssetPath);
		(*EntryObj)->TryGetStringField(TEXT("category"), C.AssetCategory);

		double TriCountD = 0;
		(*EntryObj)->TryGetNumberField(TEXT("tri_count"), TriCountD);
		C.TriCount = static_cast<int32>(TriCountD);

		const TArray<TSharedPtr<FJsonValue>>* BoundsArr;
		if ((*EntryObj)->TryGetArrayField(TEXT("bounds"), BoundsArr) && BoundsArr->Num() >= 3)
		{
			C.Size.X = (*BoundsArr)[0]->AsNumber();
			C.Size.Y = (*BoundsArr)[1]->AsNumber();
			C.Size.Z = (*BoundsArr)[2]->AsNumber();
		}

		// Sort candidate axes
		float CandSorted[3] = { static_cast<float>(C.Size.X), static_cast<float>(C.Size.Y), static_cast<float>(C.Size.Z) };
		BlockoutHelpers::SortAxes(CandSorted[0], CandSorted[1], CandSorted[2]);

		// Size similarity (60% weight) — per-axis division, avoid divide-by-zero
		float SizeSim = 0.0f;
		for (int32 i = 0; i < 3; ++i)
		{
			float Denom = FMath::Max(1.0f, SortedSize[i]);
			float Delta = FMath::Abs(CandSorted[i] - SortedSize[i]) / Denom;
			SizeSim += (1.0f - FMath::Min(Delta, 1.0f));
		}
		SizeSim = (SizeSim / 3.0f) * 60.0f;

		// Category match (25% weight)
		float CatScore = 0.0f;
		if (!BlockoutCategory.IsEmpty() && !C.AssetCategory.IsEmpty())
		{
			if (C.AssetCategory.Contains(BlockoutCategory, ESearchCase::IgnoreCase) ||
				BlockoutCategory.Contains(C.AssetCategory, ESearchCase::IgnoreCase))
			{
				CatScore = 25.0f;
			}
		}

		// Gameplay tag overlap (15% weight) — simplified: give partial credit for path overlap
		float TagScore = 0.0f;
		// Use folder path similarity as proxy
		FString BlockoutLabel = GetMonolithTagValue(Actor, TEXT("Monolith.Label"));
		if (!BlockoutLabel.IsEmpty() && C.AssetPath.Contains(BlockoutLabel, ESearchCase::IgnoreCase))
		{
			TagScore = 15.0f;
		}

		C.Score = SizeSim + CatScore + TagScore;

		// Size delta percentage (average across axes)
		float TotalDeltaPct = 0.0f;
		for (int32 i = 0; i < 3; ++i)
		{
			float Denom = FMath::Max(1.0f, SortedSize[i]);
			TotalDeltaPct += (FMath::Abs(CandSorted[i] - SortedSize[i]) / Denom) * 100.0f;
		}
		C.SizeDeltaPct = TotalDeltaPct / 3.0f;

		Candidates.Add(C);
	}

	// Sort by score descending
	Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.Score > B.Score; });

	// Return top N
	TArray<TSharedPtr<FJsonValue>> CandidatesJson;
	int32 Count = FMath::Min(TopN, Candidates.Num());
	for (int32 i = 0; i < Count; ++i)
	{
		auto CandObj = MakeShared<FJsonObject>();
		CandObj->SetStringField(TEXT("asset"), Candidates[i].AssetPath);
		CandObj->SetNumberField(TEXT("score"), FMath::RoundToInt32(Candidates[i].Score));
		CandObj->SetArrayField(TEXT("size"), VectorToJsonArray(Candidates[i].Size));
		CandObj->SetNumberField(TEXT("size_delta_pct"), FMath::RoundToFloat(Candidates[i].SizeDeltaPct * 10.0f) / 10.0f);
		CandObj->SetNumberField(TEXT("tri_count"), Candidates[i].TriCount);
		CandidatesJson.Add(MakeShared<FJsonValueObject>(CandObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blockout_actor"), ActorName);
	Result->SetArrayField(TEXT("blockout_size"), VectorToJsonArray(BlockoutSize));
	Result->SetArrayField(TEXT("candidates"), CandidatesJson);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 8. match_all_in_volume
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::MatchAllInVolume(const TSharedPtr<FJsonObject>& Params)
{
	FString VolumeName;
	if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: volume_name"));
	}

	FSQLiteDatabase* DB = BlockoutHelpers::GetCatalogDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("No meshes indexed. Run monolith_reindex() first."));
	}

	// Check catalog isn't empty
	TSharedPtr<FJsonObject> Stats = FMonolithMeshCatalog::GetStats(*DB);
	double TotalMeshes = 0;
	if (Stats) Stats->TryGetNumberField(TEXT("total_meshes"), TotalMeshes);
	if (TotalMeshes == 0)
	{
		return FMonolithActionResult::Error(TEXT("No meshes indexed. Run monolith_reindex() first."));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Find all blockout primitives owned by this volume
	FString OwnerTag = FString::Printf(TEXT("Monolith.Owner:%s"), *VolumeName);
	TArray<AActor*> Primitives;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (HasMonolithTag(*It, OwnerTag) && HasMonolithTag(*It, TEXT("Monolith.BlockoutPrimitive")))
		{
			Primitives.Add(*It);
		}
	}

	if (Primitives.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No blockout primitives found with Monolith.Owner:%s tag"), *VolumeName));
	}

	// Match each primitive
	TArray<TSharedPtr<FJsonValue>> Matches;
	for (AActor* Prim : Primitives)
	{
		auto SubParams = MakeShared<FJsonObject>();
		SubParams->SetStringField(TEXT("blockout_actor"), Prim->GetActorNameOrLabel());

		// Forward tolerance and top_n
		double Tol;
		if (Params->TryGetNumberField(TEXT("tolerance_pct"), Tol))
		{
			SubParams->SetNumberField(TEXT("tolerance_pct"), Tol);
		}
		double TopN;
		if (Params->TryGetNumberField(TEXT("top_n"), TopN))
		{
			SubParams->SetNumberField(TEXT("top_n"), TopN);
		}

		FMonolithActionResult MatchResult = MatchAssetToBlockout(SubParams);
		if (MatchResult.bSuccess && MatchResult.Result)
		{
			Matches.Add(MakeShared<FJsonValueObject>(MatchResult.Result));
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("volume"), VolumeName);
	Result->SetNumberField(TEXT("primitives_matched"), Matches.Num());
	Result->SetArrayField(TEXT("matches"), Matches);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 9. apply_replacement
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::ApplyReplacement(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* ReplacementsArr;
	if (!Params->TryGetArrayField(TEXT("replacements"), ReplacementsArr) || ReplacementsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: replacements"));
	}

	FString VolumeName;
	Params->TryGetStringField(TEXT("volume_name"), VolumeName);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Phase 1: Validate ALL assets and actors exist before starting
	struct FReplacementEntry
	{
		AActor* BlockoutActor;
		UStaticMesh* ReplacementMesh;
		FString BlockoutName;
		FString AssetPath;
	};

	TArray<FReplacementEntry> Entries;
	Entries.Reserve(ReplacementsArr->Num());
	TSet<AActor*> SeenActors; // Guard against duplicate blockout_actor entries

	TArray<TSharedPtr<FJsonValue>> ValidationErrors;

	for (int32 i = 0; i < ReplacementsArr->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>* EntryObj;
		if (!(*ReplacementsArr)[i]->TryGetObject(EntryObj))
		{
			ValidationErrors.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Index %d: invalid entry, expected object"), i)));
			continue;
		}

		FString BlockoutName, AssetPath;
		(*EntryObj)->TryGetStringField(TEXT("blockout_actor"), BlockoutName);
		(*EntryObj)->TryGetStringField(TEXT("replacement_asset"), AssetPath);

		if (BlockoutName.IsEmpty() || AssetPath.IsEmpty())
		{
			ValidationErrors.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Index %d: missing blockout_actor or replacement_asset"), i)));
			continue;
		}

		FString ActorError;
		AActor* BlockoutActor = MonolithMeshUtils::FindActorByName(BlockoutName, ActorError);
		if (!BlockoutActor)
		{
			ValidationErrors.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Actor '%s': %s"), *BlockoutName, *ActorError)));
			continue;
		}

		if (SeenActors.Contains(BlockoutActor))
		{
			ValidationErrors.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Duplicate blockout_actor '%s' at index %d"), *BlockoutName, i)));
			continue;
		}
		SeenActors.Add(BlockoutActor);

		FString MeshError;
		UStaticMesh* Mesh = MonolithMeshUtils::LoadStaticMesh(AssetPath, MeshError);
		if (!Mesh)
		{
			ValidationErrors.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Asset '%s': %s"), *AssetPath, *MeshError)));
			continue;
		}

		FReplacementEntry Entry;
		Entry.BlockoutActor = BlockoutActor;
		Entry.ReplacementMesh = Mesh;
		Entry.BlockoutName = BlockoutName;
		Entry.AssetPath = AssetPath;
		Entries.Add(Entry);
	}

	if (ValidationErrors.Num() > 0)
	{
		// Return as success with clear error indicators so the caller gets the detailed
		// per-entry validation_errors array (Error() discards the Result payload).
		auto Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("validation_errors"), ValidationErrors);
		Result->SetBoolField(TEXT("rolled_back"), true);
		Result->SetBoolField(TEXT("error"), true);
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Validation failed with %d errors. No replacements made."),
				ValidationErrors.Num()));
		return FMonolithActionResult::Success(Result);
	}

	// Phase 2: Execute replacements in single undo transaction
	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Apply Replacement")));

	TArray<TSharedPtr<FJsonValue>> Succeeded;
	TArray<TSharedPtr<FJsonValue>> Failed;
	TArray<AActor*> BlockoutsToDelete; // Batch deletion after all spawns succeed

	for (const FReplacementEntry& Entry : Entries)
	{
		// Get blockout actor info before spawning replacement
		FVector BlockoutLocation = Entry.BlockoutActor->GetActorLocation();
		FRotator BlockoutRotation = Entry.BlockoutActor->GetActorRotation();
		FVector BlockoutScale = Entry.BlockoutActor->GetActorScale3D();
		FVector BlockoutSize = GetBlockoutActorSize(Entry.BlockoutActor);
		float BlockoutHalfHeight = BlockoutSize.Z * 0.5f;

		// Copy tags (excluding BlockoutPrimitive)
		TArray<FName> CopiedTags;
		for (const FName& Tag : Entry.BlockoutActor->Tags)
		{
			FString TagStr = Tag.ToString();
			if (!TagStr.Equals(TEXT("Monolith.BlockoutPrimitive"), ESearchCase::IgnoreCase))
			{
				CopiedTags.Add(Tag);
			}
		}

		// Align replacement mesh bottom to blockout bottom.
		// Blockout primitive (BasicShape) has pivot at center, so bottom = -BlockoutHalfHeight in local Z.
		// Replacement mesh bottom relative to its pivot = BoundsOrigin.Z - BoundsExtent.Z.
		FBoxSphereBounds ReplacementBounds = Entry.ReplacementMesh->GetBounds();
		float ReplacementBottomLocal = ReplacementBounds.Origin.Z - ReplacementBounds.BoxExtent.Z;

		// We want: SpawnZ + ReplacementBottomLocal == BlockoutZ - BlockoutHalfHeight
		// So local offset = -BlockoutHalfHeight - ReplacementBottomLocal
		FVector PivotAdjust(0, 0, -BlockoutHalfHeight - ReplacementBottomLocal);
		FVector WorldAdjust = BlockoutRotation.RotateVector(PivotAdjust);

		FVector SpawnLocation = BlockoutLocation + WorldAdjust;

		// Spawn replacement
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AStaticMeshActor* Replacement = World->SpawnActor<AStaticMeshActor>(SpawnLocation, BlockoutRotation, SpawnParams);
		if (!Replacement)
		{
			// Mid-operation failure — cancel entire transaction
			Transaction.Cancel();

			auto Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("succeeded"), Succeeded);
			auto FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("blockout"), Entry.BlockoutName);
			FailObj->SetStringField(TEXT("error"), TEXT("Failed to spawn replacement actor"));
			Failed.Add(MakeShared<FJsonValueObject>(FailObj));
			Result->SetArrayField(TEXT("failed"), Failed);
			Result->SetBoolField(TEXT("rolled_back"), true);
			return FMonolithActionResult::Error(TEXT("Failed to spawn replacement actor. All changes rolled back."));
		}

		Replacement->GetStaticMeshComponent()->SetStaticMesh(Entry.ReplacementMesh);
		Replacement->SetActorScale3D(BlockoutScale);

		// Copy tags
		Replacement->Tags = CopiedTags;

		// Copy folder path
		FName FolderPath = Entry.BlockoutActor->GetFolderPath();
		if (!FolderPath.IsNone())
		{
			Replacement->SetFolderPath(FolderPath);
		}

		// Copy label
		FString OldLabel = Entry.BlockoutActor->GetActorLabel();
		if (!OldLabel.IsEmpty())
		{
			Replacement->SetActorLabel(OldLabel);
		}

		// Queue blockout for batch deletion (safer than per-entry delete)
		BlockoutsToDelete.Add(Entry.BlockoutActor);

		auto SuccessObj = MakeShared<FJsonObject>();
		SuccessObj->SetStringField(TEXT("blockout"), Entry.BlockoutName);
		SuccessObj->SetStringField(TEXT("replacement"), Entry.AssetPath);
		Succeeded.Add(MakeShared<FJsonValueObject>(SuccessObj));
	}

	// Batch delete all blockout primitives in one selection pass
	GEditor->SelectNone(false, true, false);
	for (AActor* Actor : BlockoutsToDelete)
	{
		GEditor->SelectActor(Actor, true, false, true);
	}
	GEditor->edactDeleteSelected(World, false, false, false);

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("succeeded"), Succeeded);
	Result->SetArrayField(TEXT("failed"), Failed);
	Result->SetBoolField(TEXT("rolled_back"), false);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 10. set_actor_tags
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::SetActorTags(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* ActorTagsArr;
	if (!Params->TryGetArrayField(TEXT("actor_tags"), ActorTagsArr) || ActorTagsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: actor_tags"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Validate all actors first
	struct FTagEntry
	{
		AActor* Actor;
		TArray<FName> Tags;
	};

	TArray<FTagEntry> Entries;
	for (const auto& Val : *ActorTagsArr)
	{
		const TSharedPtr<FJsonObject>* EntryObj;
		if (!Val->TryGetObject(EntryObj))
		{
			return FMonolithActionResult::Error(TEXT("Each entry must be an object with 'actor' and 'tags' fields"));
		}

		FString ActorName;
		(*EntryObj)->TryGetStringField(TEXT("actor"), ActorName);
		if (ActorName.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Entry missing 'actor' field"));
		}

		FString Error;
		AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, Error);
		if (!Actor)
		{
			return FMonolithActionResult::Error(Error);
		}

		FTagEntry Entry;
		Entry.Actor = Actor;

		const TArray<TSharedPtr<FJsonValue>>* TagsArr;
		if ((*EntryObj)->TryGetArrayField(TEXT("tags"), TagsArr))
		{
			for (const auto& TagVal : *TagsArr)
			{
				Entry.Tags.Add(FName(*TagVal->AsString()));
			}
		}

		Entries.Add(Entry);
	}

	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Set Actor Tags")));

	for (const FTagEntry& Entry : Entries)
	{
		Entry.Actor->Tags = Entry.Tags;
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("actors_updated"), Entries.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 11. clear_blockout
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::ClearBlockout(const TSharedPtr<FJsonObject>& Params)
{
	FString VolumeName;
	if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: volume_name"));
	}

	bool bKeepTagged = false;
	Params->TryGetBoolField(TEXT("keep_tagged"), bKeepTagged);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString OwnerTag = FString::Printf(TEXT("Monolith.Owner:%s"), *VolumeName);

	// Find actors to delete by tag (NOT spatial)
	TArray<AActor*> ToDelete;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!HasMonolithTag(Actor, OwnerTag)) continue;

		if (bKeepTagged)
		{
			// Only delete blockout primitives, keep replaced actors
			if (!HasMonolithTag(Actor, TEXT("Monolith.BlockoutPrimitive")))
			{
				continue;
			}
		}

		ToDelete.Add(Actor);
	}

	if (ToDelete.Num() == 0)
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("deleted"), 0);
		Result->SetStringField(TEXT("message"), TEXT("No actors found to delete"));
		return FMonolithActionResult::Success(Result);
	}

	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Clear Blockout")));

	// Collect names before deletion
	TArray<TSharedPtr<FJsonValue>> DeletedNames;
	for (AActor* Actor : ToDelete)
	{
		DeletedNames.Add(MakeShared<FJsonValueString>(Actor->GetActorNameOrLabel()));
	}

	GEditor->SelectNone(false, true, false);
	for (AActor* Actor : ToDelete)
	{
		GEditor->SelectActor(Actor, true, false, true);
	}
	GEditor->edactDeleteSelected(World, false, false, false);

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("deleted"), DeletedNames.Num());
	Result->SetArrayField(TEXT("actors"), DeletedNames);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 12. export_blockout_layout
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::ExportBlockoutLayout(const TSharedPtr<FJsonObject>& Params)
{
	FString VolumeName;
	if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: volume_name"));
	}

	FString Error;
	AActor* Volume = FindBlockoutVolumeAny(VolumeName, Error);
	if (!Volume)
	{
		return FMonolithActionResult::Error(Error);
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FVector VolumeOrigin, VolumeExtent;
	Volume->GetActorBounds(false, VolumeOrigin, VolumeExtent);
	FVector VolumeSize = VolumeExtent * 2.0;

	// Find all blockout primitives
	FString OwnerTag = FString::Printf(TEXT("Monolith.Owner:%s"), *VolumeName);

	TArray<TSharedPtr<FJsonValue>> PrimitivesJson;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!HasMonolithTag(Actor, OwnerTag)) continue;
		if (!HasMonolithTag(Actor, TEXT("Monolith.BlockoutPrimitive"))) continue;

		FVector ActorLoc = Actor->GetActorLocation();
		FVector RelativePos = ActorLoc - (VolumeOrigin - VolumeExtent); // relative to volume min corner

		// Normalize to 0-1 per axis
		FVector NormalizedPos;
		NormalizedPos.X = VolumeSize.X > 0 ? RelativePos.X / VolumeSize.X : 0.0;
		NormalizedPos.Y = VolumeSize.Y > 0 ? RelativePos.Y / VolumeSize.Y : 0.0;
		NormalizedPos.Z = VolumeSize.Z > 0 ? RelativePos.Z / VolumeSize.Z : 0.0;

		// Size is ABSOLUTE
		FVector Size = GetBlockoutActorSize(Actor);

		auto PrimObj = MakeShared<FJsonObject>();

		FString Shape = GetMonolithTagValue(Actor, TEXT("Monolith.Shape"));
		PrimObj->SetStringField(TEXT("shape"), Shape.IsEmpty() ? TEXT("box") : Shape);
		PrimObj->SetArrayField(TEXT("relative_position"), VectorToJsonArray(NormalizedPos));
		PrimObj->SetArrayField(TEXT("size"), VectorToJsonArray(Size));
		PrimObj->SetArrayField(TEXT("rotation"), RotatorToJsonArray(Actor->GetActorRotation()));

		FString Label = GetMonolithTagValue(Actor, TEXT("Monolith.Label"));
		if (!Label.IsEmpty())
		{
			PrimObj->SetStringField(TEXT("label"), Label);
		}

		FString Category = GetMonolithTagValue(Actor, TEXT("Monolith.Category"));
		if (!Category.IsEmpty())
		{
			PrimObj->SetStringField(TEXT("category"), Category);
		}

		PrimitivesJson.Add(MakeShared<FJsonValueObject>(PrimObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("volume_extent"), VectorToJsonArray(VolumeSize));
	Result->SetArrayField(TEXT("primitives"), PrimitivesJson);
	Result->SetNumberField(TEXT("count"), PrimitivesJson.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 13. import_blockout_layout
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::ImportBlockoutLayout(const TSharedPtr<FJsonObject>& Params)
{
	FString VolumeName;
	if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: volume_name"));
	}

	FString Error;
	AActor* Volume = FindBlockoutVolumeAny(VolumeName, Error);
	if (!Volume)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Parse layout JSON — accept object or string
	TSharedPtr<FJsonObject> LayoutObj;
	const TSharedPtr<FJsonObject>* LayoutObjPtr;
	if (Params->TryGetObjectField(TEXT("layout_json"), LayoutObjPtr))
	{
		LayoutObj = *LayoutObjPtr;
	}
	else
	{
		FString LayoutStr;
		if (Params->TryGetStringField(TEXT("layout_json"), LayoutStr))
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(LayoutStr);
			if (!FJsonSerializer::Deserialize(Reader, LayoutObj) || !LayoutObj.IsValid())
			{
				return FMonolithActionResult::Error(TEXT("Failed to parse layout_json string as JSON"));
			}
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("Missing required param: layout_json (object or JSON string)"));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* PrimitivesArr;
	if (!LayoutObj->TryGetArrayField(TEXT("primitives"), PrimitivesArr) || PrimitivesArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Layout JSON missing or empty 'primitives' array"));
	}

	if (PrimitivesArr->Num() > 200)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Layout has %d primitives (max 200)"), PrimitivesArr->Num()));
	}

	// Get target volume bounds
	FVector VolumeOrigin, VolumeExtent;
	Volume->GetActorBounds(false, VolumeOrigin, VolumeExtent);
	FVector VolumeMin = VolumeOrigin - VolumeExtent;
	FVector VolumeSize = VolumeExtent * 2.0;

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Import Blockout Layout")));

	int32 Created = 0;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	for (const auto& Val : *PrimitivesArr)
	{
		const TSharedPtr<FJsonObject>* PrimObj;
		if (!Val->TryGetObject(PrimObj)) continue;

		FString Shape = TEXT("box");
		(*PrimObj)->TryGetStringField(TEXT("shape"), Shape);

		bool bValidShape = false;
		FString MeshPath = GetBasicShapePath(Shape, bValidShape);
		if (!bValidShape) continue;

		// Scale POSITIONS to target volume, keep SIZES unchanged
		FVector RelativePos;
		if (!MonolithMeshUtils::ParseVector(*PrimObj, TEXT("relative_position"), RelativePos))
		{
			continue;
		}

		// Convert normalized position to world position in target volume
		FVector WorldPos = VolumeMin + FVector(
			RelativePos.X * VolumeSize.X,
			RelativePos.Y * VolumeSize.Y,
			RelativePos.Z * VolumeSize.Z
		);

		// Size is ABSOLUTE — compute scale from size
		FVector Size;
		if (!MonolithMeshUtils::ParseVector(*PrimObj, TEXT("size"), Size))
		{
			continue;
		}

		// BasicShapes are 100cm, so scale = size / 100
		FVector PrimScale = Size / 100.0;

		FRotator PrimRotation(0, 0, 0);
		MonolithMeshUtils::ParseRotator(*PrimObj, TEXT("rotation"), PrimRotation);

		FString Label;
		(*PrimObj)->TryGetStringField(TEXT("label"), Label);

		FString Category = TEXT("default");
		(*PrimObj)->TryGetStringField(TEXT("category"), Category);

		// Check overflow
		FVector Diff = (WorldPos - VolumeOrigin).GetAbs();
		bool bOverflow = false;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Diff[Axis] > VolumeExtent[Axis] + Size[Axis] * 0.5)
			{
				bOverflow = true;
				break;
			}
		}
		if (bOverflow)
		{
			FString WarnLabel = Label.IsEmpty() ? FString::Printf(TEXT("index_%d"), Created) : Label;
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Primitive '%s' overflows target volume bounds"), *WarnLabel)));
		}

		// Spawn
		UStaticMesh* ShapeMesh = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(MeshPath);
		if (!ShapeMesh) continue;

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(WorldPos, PrimRotation, SpawnParams);
		if (!Actor) continue;

		Actor->GetStaticMeshComponent()->SetStaticMesh(ShapeMesh);
		Actor->SetActorScale3D(PrimScale);

		UMaterialInstanceDynamic* BlockoutMat = GetBlockoutMaterial(Category);
		if (BlockoutMat)
		{
			Actor->GetStaticMeshComponent()->SetMaterial(0, BlockoutMat);
		}

		Actor->Tags.Add(FName(TEXT("Monolith.BlockoutPrimitive")));
		Actor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Owner:%s"), *VolumeName)));
		if (!Label.IsEmpty())
		{
			Actor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Label:%s"), *Label)));
			Actor->SetActorLabel(Label);
		}
		Actor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Shape:%s"), *Shape.ToLower())));
		Actor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Category:%s"), *Category)));
		Actor->SetFolderPath(FName(*FString::Printf(TEXT("Blockout/%s"), *VolumeName)));

		// Check for overlaps with existing non-blockout geometry
		FString OverlapLabel = Label.IsEmpty() ? FString::Printf(TEXT("index_%d"), Created) : Label;
		FString OverlapWarning = CheckBlockoutOverlap(World, Actor, OverlapLabel);
		if (!OverlapWarning.IsEmpty())
		{
			Warnings.Add(MakeShared<FJsonValueString>(OverlapWarning));
		}

		Created++;
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("created"), Created);
	Result->SetArrayField(TEXT("warnings"), Warnings);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 14. scan_volume
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::ScanVolume(const TSharedPtr<FJsonObject>& Params)
{
	FString VolumeName;
	if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: volume_name"));
	}

	FString Error;
	AActor* Volume = FindBlockoutVolumeAny(VolumeName, Error);
	if (!Volume)
	{
		return FMonolithActionResult::Error(Error);
	}

	FString RayDensity = TEXT("medium");
	Params->TryGetStringField(TEXT("ray_density"), RayDensity);

	int32 VerticalLayers = 3;
	double VerticalLayersD;
	if (Params->TryGetNumberField(TEXT("vertical_layers"), VerticalLayersD))
	{
		VerticalLayers = FMath::Clamp(static_cast<int32>(VerticalLayersD), 1, 8);
	}

	// Determine ray count per origin based on density
	int32 RaysPerOrigin = 12;  // medium
	if (RayDensity.Equals(TEXT("low"), ESearchCase::IgnoreCase))
	{
		RaysPerOrigin = 8;
	}
	else if (RayDensity.Equals(TEXT("high"), ESearchCase::IgnoreCase))
	{
		RaysPerOrigin = 24;
	}

	FVector VolumeOrigin, VolumeExtent;
	Volume->GetActorBounds(false, VolumeOrigin, VolumeExtent);
	FVector VolumeMin = VolumeOrigin - VolumeExtent;
	FVector VolumeMax = VolumeOrigin + VolumeExtent;
	float MaxRadius = VolumeExtent.GetMax() * 2.0f;

	// Build ray origins: center + 8 corners + midpoints of edges (up to ~13 for medium)
	TArray<FVector> Origins;
	Origins.Add(VolumeOrigin); // center

	// 8 corners
	for (int32 xi = 0; xi <= 1; ++xi)
	{
		for (int32 yi = 0; yi <= 1; ++yi)
		{
			for (int32 zi = 0; zi <= 1; ++zi)
			{
				FVector Corner(
					xi == 0 ? VolumeMin.X : VolumeMax.X,
					yi == 0 ? VolumeMin.Y : VolumeMax.Y,
					zi == 0 ? VolumeMin.Z : VolumeMax.Z
				);
				// Inset corners slightly to ensure they're inside the volume
				FVector InsetDir = (VolumeOrigin - Corner).GetSafeNormal();
				Origins.Add(Corner + InsetDir * 10.0f);
			}
		}
	}

	// 4 wall midpoints (at volume center height)
	Origins.Add(FVector(VolumeMin.X + 10.0f, VolumeOrigin.Y, VolumeOrigin.Z));
	Origins.Add(FVector(VolumeMax.X - 10.0f, VolumeOrigin.Y, VolumeOrigin.Z));
	Origins.Add(FVector(VolumeOrigin.X, VolumeMin.Y + 10.0f, VolumeOrigin.Z));
	Origins.Add(FVector(VolumeOrigin.X, VolumeMax.Y - 10.0f, VolumeOrigin.Z));

	// Hard cap: 512 total rays
	int32 TotalRaysBudget = 512;
	int32 TotalOriginsRays = Origins.Num() * RaysPerOrigin * VerticalLayers;
	if (TotalOriginsRays > TotalRaysBudget)
	{
		// Reduce origins to fit budget
		int32 MaxOrigins = TotalRaysBudget / (RaysPerOrigin * VerticalLayers);
		MaxOrigins = FMath::Max(1, MaxOrigins);
		Origins.SetNum(FMath::Min(Origins.Num(), MaxOrigins));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MonolithScanVolume), true);
	QueryParams.bReturnPhysicalMaterial = true;
	QueryParams.AddIgnoredActor(Volume);

	// Track hits per compass direction
	struct FDirectionData
	{
		TArray<float> Distances;
		TSet<FString> ActorsHit;
	};

	// Compass sectors
	struct FCompassDir
	{
		const TCHAR* Name;
		float MinAngle;
		float MaxAngle;
	};

	static const FCompassDir CompassDirs[] =
	{
		{ TEXT("N"),  337.5f,  22.5f },
		{ TEXT("NE"),  22.5f,  67.5f },
		{ TEXT("E"),   67.5f, 112.5f },
		{ TEXT("SE"), 112.5f, 157.5f },
		{ TEXT("S"),  157.5f, 202.5f },
		{ TEXT("SW"), 202.5f, 247.5f },
		{ TEXT("W"),  247.5f, 292.5f },
		{ TEXT("NW"), 292.5f, 337.5f },
	};

	TMap<FString, FDirectionData> DirectionMap;
	DirectionMap.Add(TEXT("up"));
	DirectionMap.Add(TEXT("down"));
	for (const auto& Dir : CompassDirs)
	{
		DirectionMap.Add(Dir.Name);
	}

	TSet<FString> AllActorsHit;
	int32 TotalRaysFired = 0;
	int32 TotalHits = 0;

	// Compute elevation angles
	TArray<float> Elevations;
	if (VerticalLayers == 1)
	{
		Elevations.Add(0.0f);
	}
	else
	{
		for (int32 v = 0; v < VerticalLayers; ++v)
		{
			float Alpha = static_cast<float>(v) / static_cast<float>(VerticalLayers - 1);
			Elevations.Add(FMath::Lerp(-60.0f, 60.0f, Alpha));
		}
	}

	for (const FVector& Origin : Origins)
	{
		for (int32 a = 0; a < RaysPerOrigin; ++a)
		{
			float AzimuthDeg = (360.0f / static_cast<float>(RaysPerOrigin)) * static_cast<float>(a);

			for (int32 v = 0; v < VerticalLayers; ++v)
			{
				if (TotalRaysFired >= 512) break;

				float ElevDeg = Elevations[v];
				float ElevRad = FMath::DegreesToRadians(ElevDeg);
				float AzimuthRad = FMath::DegreesToRadians(AzimuthDeg);

				FVector Dir;
				Dir.X = FMath::Cos(ElevRad) * FMath::Cos(AzimuthRad);
				Dir.Y = FMath::Cos(ElevRad) * FMath::Sin(AzimuthRad);
				Dir.Z = FMath::Sin(ElevRad);
				Dir.Normalize();

				FVector End = Origin + Dir * MaxRadius;

				FHitResult Hit;
				bool bHit = World->LineTraceSingleByChannel(Hit, Origin, End, ECC_Visibility, QueryParams);
				TotalRaysFired++;

				// Determine direction
				FString DirName = TEXT("N");
				if (ElevDeg > 45.0f)
				{
					DirName = TEXT("up");
				}
				else if (ElevDeg < -45.0f)
				{
					DirName = TEXT("down");
				}
				else
				{
					float NormAzimuth = FMath::Fmod(AzimuthDeg + 360.0f, 360.0f);
					for (const auto& CDir : CompassDirs)
					{
						float Min = FMath::Fmod(CDir.MinAngle + 360.0f, 360.0f);
						float Max = FMath::Fmod(CDir.MaxAngle + 360.0f, 360.0f);
						if (Min < Max)
						{
							if (NormAzimuth >= Min && NormAzimuth < Max)
							{
								DirName = CDir.Name;
								break;
							}
						}
						else
						{
							if (NormAzimuth >= Min || NormAzimuth < Max)
							{
								DirName = CDir.Name;
								break;
							}
						}
					}
				}

				if (bHit)
				{
					TotalHits++;
					FDirectionData& Data = DirectionMap.FindOrAdd(DirName);
					Data.Distances.Add(Hit.Distance);

					AActor* HitActor = Hit.GetActor();
					if (HitActor)
					{
						FString ActorName = HitActor->GetActorNameOrLabel();
						Data.ActorsHit.Add(ActorName);
						AllActorsHit.Add(ActorName);
					}
				}
			}
			if (TotalRaysFired >= 512) break;
		}
		if (TotalRaysFired >= 512) break;
	}

	// Build semantic output
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("volume"), VolumeName);
	Result->SetNumberField(TEXT("rays_fired"), TotalRaysFired);
	Result->SetNumberField(TEXT("total_hits"), TotalHits);
	Result->SetBoolField(TEXT("enclosed"), TotalRaysFired > 0 ? (static_cast<float>(TotalHits) / TotalRaysFired > 0.5f) : false);

	// Walls — N, E, S, W (consistent hits at similar distances)
	auto WallsObj = MakeShared<FJsonObject>();
	for (const auto& CDir : CompassDirs)
	{
		FString DirName = CDir.Name;
		// Only report cardinal + ordinal for wall detection
		if (FDirectionData* Data = DirectionMap.Find(DirName))
		{
			if (Data->Distances.Num() > 0)
			{
				auto WallInfo = MakeShared<FJsonObject>();

				// Average distance
				float AvgDist = 0.0f;
				for (float D : Data->Distances) AvgDist += D;
				AvgDist /= Data->Distances.Num();
				WallInfo->SetNumberField(TEXT("avg_distance"), AvgDist);
				WallInfo->SetNumberField(TEXT("hit_count"), Data->Distances.Num());

				// Variance — low = consistent wall, high = opening
				float Variance = 0.0f;
				for (float D : Data->Distances) Variance += FMath::Square(D - AvgDist);
				Variance /= Data->Distances.Num();
				WallInfo->SetNumberField(TEXT("variance"), Variance);
				WallInfo->SetBoolField(TEXT("likely_wall"), Variance < AvgDist * 0.3f); // low relative variance

				TArray<TSharedPtr<FJsonValue>> ActorsArr;
				for (const FString& A : Data->ActorsHit) ActorsArr.Add(MakeShared<FJsonValueString>(A));
				WallInfo->SetArrayField(TEXT("actors"), ActorsArr);

				WallsObj->SetObjectField(DirName, WallInfo);
			}
		}
	}
	Result->SetObjectField(TEXT("walls"), WallsObj);

	// Floor
	if (FDirectionData* FloorData = DirectionMap.Find(TEXT("down")))
	{
		auto FloorObj = MakeShared<FJsonObject>();
		if (FloorData->Distances.Num() > 0)
		{
			float AvgDist = 0.0f;
			for (float D : FloorData->Distances) AvgDist += D;
			AvgDist /= FloorData->Distances.Num();
			FloorObj->SetNumberField(TEXT("avg_distance"), AvgDist);
			FloorObj->SetBoolField(TEXT("detected"), true);
		}
		else
		{
			FloorObj->SetBoolField(TEXT("detected"), false);
		}
		Result->SetObjectField(TEXT("floor"), FloorObj);
	}

	// Ceiling
	if (FDirectionData* CeilData = DirectionMap.Find(TEXT("up")))
	{
		auto CeilObj = MakeShared<FJsonObject>();
		if (CeilData->Distances.Num() > 0)
		{
			float AvgDist = 0.0f;
			for (float D : CeilData->Distances) AvgDist += D;
			AvgDist /= CeilData->Distances.Num();
			CeilObj->SetNumberField(TEXT("avg_distance"), AvgDist);
			CeilObj->SetBoolField(TEXT("detected"), true);
		}
		else
		{
			CeilObj->SetBoolField(TEXT("detected"), false);
		}
		Result->SetObjectField(TEXT("ceiling"), CeilObj);
	}

	// Openings — directions with high variance or low hit count
	TArray<TSharedPtr<FJsonValue>> OpeningsArr;
	for (const auto& CDir : CompassDirs)
	{
		FString DirName = CDir.Name;
		if (FDirectionData* Data = DirectionMap.Find(DirName))
		{
			if (Data->Distances.Num() == 0)
			{
				OpeningsArr.Add(MakeShared<FJsonValueString>(DirName));
			}
			else
			{
				float AvgDist = 0.0f;
				for (float D : Data->Distances) AvgDist += D;
				AvgDist /= Data->Distances.Num();

				float Variance = 0.0f;
				for (float D : Data->Distances) Variance += FMath::Square(D - AvgDist);
				Variance /= Data->Distances.Num();

				if (Variance > AvgDist * 0.5f)
				{
					OpeningsArr.Add(MakeShared<FJsonValueString>(DirName));
				}
			}
		}
	}
	Result->SetArrayField(TEXT("openings"), OpeningsArr);

	// Existing actors
	TArray<TSharedPtr<FJsonValue>> ActorsJsonArr;
	for (const FString& A : AllActorsHit)
	{
		ActorsJsonArr.Add(MakeShared<FJsonValueString>(A));
	}
	Result->SetArrayField(TEXT("existing_actors"), ActorsJsonArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 15. scatter_props
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::ScatterProps(const TSharedPtr<FJsonObject>& Params)
{
	FString VolumeName;
	if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: volume_name"));
	}

	const TArray<TSharedPtr<FJsonValue>>* AssetPathsArr;
	if (!Params->TryGetArrayField(TEXT("asset_paths"), AssetPathsArr) || AssetPathsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: asset_paths"));
	}

	double CountD = 0;
	if (!Params->TryGetNumberField(TEXT("count"), CountD) || CountD <= 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: count (must be > 0)"));
	}
	int32 Count = FMath::Clamp(static_cast<int32>(CountD), 1, 200);

	double MinSpacing = 50.0;
	Params->TryGetNumberField(TEXT("min_spacing"), MinSpacing);

	bool bRandomRotation = true;
	Params->TryGetBoolField(TEXT("random_rotation"), bRandomRotation);

	float ScaleMin = 0.9f, ScaleMax = 1.1f;
	const TArray<TSharedPtr<FJsonValue>>* ScaleRangeArr;
	if (Params->TryGetArrayField(TEXT("random_scale_range"), ScaleRangeArr) && ScaleRangeArr->Num() >= 2)
	{
		ScaleMin = static_cast<float>((*ScaleRangeArr)[0]->AsNumber());
		ScaleMax = static_cast<float>((*ScaleRangeArr)[1]->AsNumber());
	}

	int32 Seed = 0;
	double SeedD;
	if (Params->TryGetNumberField(TEXT("seed"), SeedD))
	{
		Seed = static_cast<int32>(SeedD);
	}
	if (Seed == 0)
	{
		Seed = FMath::Rand();
	}

	bool bSurfaceAlign = false;
	Params->TryGetBoolField(TEXT("surface_align"), bSurfaceAlign);

	// Collision mode: none, warn, reject, adjust
	FString CollisionMode = TEXT("warn");
	Params->TryGetStringField(TEXT("collision_mode"), CollisionMode);
	CollisionMode = CollisionMode.ToLower();
	if (CollisionMode != TEXT("none") && CollisionMode != TEXT("warn") &&
		CollisionMode != TEXT("reject") && CollisionMode != TEXT("adjust"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid collision_mode '%s'. Must be: none, warn, reject, adjust"), *CollisionMode));
	}
	const bool bDoCollision = (CollisionMode != TEXT("none"));
	const bool bRejectOnOverlap = (CollisionMode == TEXT("reject") || CollisionMode == TEXT("adjust"));
	const bool bAllowPushOut = (CollisionMode == TEXT("adjust"));

	// Validate volume
	FString Error;
	AActor* Volume = FindBlockoutVolumeAny(VolumeName, Error);
	if (!Volume)
	{
		return FMonolithActionResult::Error(Error);
	}

	// Validate all meshes
	TArray<UStaticMesh*> Meshes;
	for (const auto& Val : *AssetPathsArr)
	{
		FString MeshError;
		UStaticMesh* Mesh = MonolithMeshUtils::LoadStaticMesh(Val->AsString(), MeshError);
		if (!Mesh)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s': %s"), *Val->AsString(), *MeshError));
		}
		Meshes.Add(Mesh);
	}

	FVector VolumeOrigin, VolumeExtent;
	Volume->GetActorBounds(false, VolumeOrigin, VolumeExtent);
	FVector VolumeMin = VolumeOrigin - VolumeExtent;
	FVector VolumeMax = VolumeOrigin + VolumeExtent;

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Poisson disk sampling (Bridson's algorithm) with FRandomStream for reproducibility
	FRandomStream RandStream(Seed);

	// 2D Poisson disk on the XY plane, then project Z via floor trace
	float AreaWidth = VolumeExtent.X * 2.0f;
	float AreaHeight = VolumeExtent.Y * 2.0f;
	float CellSize = static_cast<float>(MinSpacing) / FMath::Sqrt(2.0f);

	int32 GridW = FMath::Max(1, FMath::CeilToInt32(AreaWidth / CellSize));
	int32 GridH = FMath::Max(1, FMath::CeilToInt32(AreaHeight / CellSize));

	// Background grid for acceleration (-1 = empty)
	TArray<int32> Grid;
	Grid.SetNumUninitialized(GridW * GridH);
	for (int32 i = 0; i < Grid.Num(); ++i) Grid[i] = -1;

	struct FSample { FVector2D Pos; };
	TArray<FSample> Samples;
	TArray<int32> ActiveList;

	// Seed the first sample
	FVector2D FirstPos(
		RandStream.FRandRange(0.0f, AreaWidth),
		RandStream.FRandRange(0.0f, AreaHeight)
	);

	Samples.Add({ FirstPos });
	ActiveList.Add(0);
	int32 GX = FMath::Clamp(FMath::FloorToInt32(FirstPos.X / CellSize), 0, GridW - 1);
	int32 GY = FMath::Clamp(FMath::FloorToInt32(FirstPos.Y / CellSize), 0, GridH - 1);
	Grid[GY * GridW + GX] = 0;

	const int32 MaxAttempts = 30;

	while (ActiveList.Num() > 0 && Samples.Num() < Count)
	{
		int32 ActiveIdx = RandStream.RandRange(0, ActiveList.Num() - 1);
		int32 SampleIdx = ActiveList[ActiveIdx];
		const FVector2D& Base = Samples[SampleIdx].Pos;

		bool bFound = false;
		for (int32 Attempt = 0; Attempt < MaxAttempts; ++Attempt)
		{
			float Angle = RandStream.FRandRange(0.0f, 2.0f * PI);
			float Dist = RandStream.FRandRange(static_cast<float>(MinSpacing), static_cast<float>(MinSpacing) * 2.0f);
			FVector2D Candidate = Base + FVector2D(FMath::Cos(Angle) * Dist, FMath::Sin(Angle) * Dist);

			// Bounds check
			if (Candidate.X < 0 || Candidate.X >= AreaWidth || Candidate.Y < 0 || Candidate.Y >= AreaHeight)
			{
				continue;
			}

			int32 CX = FMath::Clamp(FMath::FloorToInt32(Candidate.X / CellSize), 0, GridW - 1);
			int32 CY = FMath::Clamp(FMath::FloorToInt32(Candidate.Y / CellSize), 0, GridH - 1);

			// Check neighboring cells for conflicts
			bool bTooClose = false;
			for (int32 dy = -2; dy <= 2 && !bTooClose; ++dy)
			{
				for (int32 dx = -2; dx <= 2 && !bTooClose; ++dx)
				{
					int32 NX = CX + dx;
					int32 NY = CY + dy;
					if (NX < 0 || NX >= GridW || NY < 0 || NY >= GridH) continue;

					int32 NeighborIdx = Grid[NY * GridW + NX];
					if (NeighborIdx >= 0)
					{
						float DistSq = FVector2D::DistSquared(Candidate, Samples[NeighborIdx].Pos);
						if (DistSq < MinSpacing * MinSpacing)
						{
							bTooClose = true;
						}
					}
				}
			}

			if (!bTooClose)
			{
				int32 NewIdx = Samples.Num();
				Samples.Add({ Candidate });
				ActiveList.Add(NewIdx);
				Grid[CY * GridW + CX] = NewIdx;
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			ActiveList.RemoveAtSwap(ActiveIdx);
		}
	}

	// Limit to requested count
	if (Samples.Num() > Count)
	{
		Samples.SetNum(Count);
	}

	// Spawn actors with collision-aware placement
	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Scatter Props")));

	int32 Placed = 0;
	int32 RejectedCount = 0;
	int32 AdjustedCount = 0;
	TArray<TSharedPtr<FJsonValue>> PlacedArr;
	TArray<TSharedPtr<FJsonValue>> ScatterWarnings;

	FCollisionQueryParams FloorTraceParams(SCENE_QUERY_STAT(MonolithFloorTrace), true);
	FloorTraceParams.AddIgnoredActor(Volume);

	// Track spawned actors so we can ignore them in collision checks for subsequent props
	TArray<AActor*> SpawnedActors;
	SpawnedActors.Add(Volume);

	for (const FSample& Sample : Samples)
	{
		// Convert 2D sample to world XY
		float WorldX = VolumeMin.X + Sample.Pos.X;
		float WorldY = VolumeMin.Y + Sample.Pos.Y;

		// Random scale (compute early so we can use it for collision shape)
		float Scale = RandStream.FRandRange(ScaleMin, ScaleMax);

		// Pick random mesh from the pool
		int32 MeshIdx = RandStream.RandRange(0, Meshes.Num() - 1);
		UStaticMesh* ChosenMesh = Meshes[MeshIdx];

		// Compute prop half-extent for sweep/overlap queries (0.9x shrink to avoid AABB false positives)
		FVector PropHalfExtent = ChosenMesh->GetBounds().BoxExtent * Scale * 0.9f;
		PropHalfExtent = PropHalfExtent.ComponentMax(FVector(1.0f));

		// Floor finding: use SweepSingle with prop footprint when collision is enabled,
		// fall back to LineTrace when collision_mode is "none" (preserves exact legacy behavior)
		FVector TraceStart(WorldX, WorldY, VolumeMax.Z);
		FVector TraceEnd(WorldX, WorldY, VolumeMin.Z - 100.0f);

		FHitResult FloorHit;
		bool bHitFloor = false;
		FVector SpawnLocation;
		FRotator SpawnRotation = FRotator::ZeroRotator;

		if (bDoCollision)
		{
			// Pre-compute random yaw so we can pass it to the OBB sweep query
			if (bRandomRotation)
			{
				SpawnRotation.Yaw = RandStream.FRandRange(0.0f, 360.0f);
			}

			// Sweep a thin box (prop's XY footprint, 1cm tall) to find a floor position
			// that accounts for the prop's full footprint, not just a single point
			FCollisionShape FootprintShape = FCollisionShape::MakeBox(
				FVector(PropHalfExtent.X, PropHalfExtent.Y, 1.0f));

			bHitFloor = World->SweepSingleByChannel(
				FloorHit, TraceStart, TraceEnd,
				SpawnRotation.Quaternion(),
				ECC_WorldStatic, FootprintShape, FloorTraceParams);

			if (bHitFloor && FloorHit.bStartPenetrating)
			{
				// Footprint started inside geometry — candidate is invalid
				RejectedCount++;
				ScatterWarnings.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Sample at (%.0f, %.0f) rejected: footprint starts inside geometry"),
						WorldX, WorldY)));
				continue;
			}

			if (bHitFloor)
			{
				// Sweep Location is center of the swept shape; offset up by prop half-height
				SpawnLocation = FloorHit.Location + FVector(0, 0, PropHalfExtent.Z);

				if (bSurfaceAlign)
				{
					SpawnRotation = FloorHit.Normal.Rotation();
					SpawnRotation.Pitch -= 90.0f;
					if (bRandomRotation)
					{
						SpawnRotation.Yaw += RandStream.FRandRange(0.0f, 360.0f);
					}
				}
			}
			else
			{
				SpawnLocation = FVector(WorldX, WorldY, VolumeMin.Z);
			}
		}
		else
		{
			// Legacy line trace for "none" mode — preserves exact original behavior + RNG sequence
			bHitFloor = World->LineTraceSingleByChannel(
				FloorHit, TraceStart, TraceEnd, ECC_Visibility, FloorTraceParams);

			if (bHitFloor)
			{
				SpawnLocation = FloorHit.Location;
				if (bSurfaceAlign)
				{
					SpawnRotation = FloorHit.Normal.Rotation();
					SpawnRotation.Pitch -= 90.0f;
				}
			}
			else
			{
				SpawnLocation = FVector(WorldX, WorldY, VolumeMin.Z);
			}

			if (bRandomRotation)
			{
				SpawnRotation.Yaw += RandStream.FRandRange(0.0f, 360.0f);
			}
		}

		// Pre-spawn collision validation
		if (bDoCollision)
		{
			MonolithMeshUtils::FPropPlacementResult PlacementResult =
				MonolithMeshUtils::ValidatePropPlacement(
					World,
					SpawnLocation,
					SpawnRotation.Quaternion(),
					PropHalfExtent,
					SpawnedActors,
					bAllowPushOut,
					/*MaxPushOutDistance=*/50.0f);

			if (!PlacementResult.bValid)
			{
				if (bRejectOnOverlap)
				{
					// Skip this candidate entirely
					RejectedCount++;
					ScatterWarnings.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("Prop rejected at (%.0f, %.0f, %.0f): %s"),
							SpawnLocation.X, SpawnLocation.Y, SpawnLocation.Z,
							*PlacementResult.RejectReason)));
					continue;
				}
				else
				{
					// "warn" mode: place anyway but record the issue
					ScatterWarnings.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("Overlap at (%.0f, %.0f, %.0f): %s"),
							SpawnLocation.X, SpawnLocation.Y, SpawnLocation.Z,
							*PlacementResult.RejectReason)));
				}
			}
			else
			{
				// Placement is valid — may have been adjusted
				if (!FVector::PointsAreNear(SpawnLocation, PlacementResult.FinalLocation, UE_KINDA_SMALL_NUMBER))
				{
					SpawnLocation = PlacementResult.FinalLocation;
					AdjustedCount++;
				}

				// Propagate any push-out warnings
				for (const FString& Warning : PlacementResult.Warnings)
				{
					ScatterWarnings.Add(MakeShared<FJsonValueString>(Warning));
				}
			}
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AStaticMeshActor* PropActor = World->SpawnActor<AStaticMeshActor>(SpawnLocation, SpawnRotation, SpawnParams);
		if (!PropActor) continue;

		PropActor->GetStaticMeshComponent()->SetStaticMesh(ChosenMesh);
		PropActor->SetActorScale3D(FVector(Scale));

		PropActor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Owner:%s"), *VolumeName)));
		PropActor->Tags.Add(FName(TEXT("Monolith.ScatteredProp")));
		PropActor->SetFolderPath(FName(*FString::Printf(TEXT("Blockout/%s/Props"), *VolumeName)));

		// Track for subsequent collision checks
		SpawnedActors.Add(PropActor);

		auto PlacedObj = MakeShared<FJsonObject>();
		PlacedObj->SetStringField(TEXT("actor"), PropActor->GetActorNameOrLabel());
		PlacedObj->SetStringField(TEXT("mesh"), ChosenMesh->GetPathName());
		PlacedObj->SetArrayField(TEXT("location"), VectorToJsonArray(SpawnLocation));
		PlacedObj->SetNumberField(TEXT("scale"), Scale);

		// Legacy overlap warning for "none" mode (post-spawn check like before)
		if (!bDoCollision)
		{
			FString OverlapWarning = CheckBlockoutOverlap(World, PropActor, PropActor->GetActorNameOrLabel());
			if (!OverlapWarning.IsEmpty())
			{
				PlacedObj->SetStringField(TEXT("warning"), OverlapWarning);
			}
		}

		PlacedArr.Add(MakeShared<FJsonValueObject>(PlacedObj));
		Placed++;
	}

	// For non-"none" modes, also collect per-prop warnings from PlacedArr
	if (!bDoCollision)
	{
		for (const auto& P : PlacedArr)
		{
			const TSharedPtr<FJsonObject>& Obj = P->AsObject();
			FString Warn;
			if (Obj.IsValid() && Obj->TryGetStringField(TEXT("warning"), Warn))
			{
				ScatterWarnings.Add(MakeShared<FJsonValueString>(Warn));
			}
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("placed"), Placed);
	Result->SetNumberField(TEXT("requested"), Count);
	Result->SetNumberField(TEXT("seed"), Seed);
	Result->SetStringField(TEXT("collision_mode"), CollisionMode);
	Result->SetArrayField(TEXT("props"), PlacedArr);
	if (RejectedCount > 0)
	{
		Result->SetNumberField(TEXT("rejected_count"), RejectedCount);
	}
	if (AdjustedCount > 0)
	{
		Result->SetNumberField(TEXT("adjusted_count"), AdjustedCount);
	}
	if (ScatterWarnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), ScatterWarnings);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 16. create_blockout_blueprint
// ============================================================================

FMonolithActionResult FMonolithMeshBlockoutActions::CreateBlockoutBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = TEXT("/Game/Monolith/Blockout/BP_MonolithBlockoutVolume");
	if (Params->HasField(TEXT("save_path")))
	{
		SavePath = Params->GetStringField(TEXT("save_path"));
	}

	bool bForce = false;
	if (Params->HasField(TEXT("force")))
	{
		bForce = Params->GetBoolField(TEXT("force"));
	}

	// Check if already exists
	if (!bForce && FPackageName::DoesPackageExist(SavePath))
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), SavePath);
		Result->SetBoolField(TEXT("already_exists"), true);
		Result->SetStringField(TEXT("message"), TEXT("Blueprint already exists. Use force: true to recreate."));
		return FMonolithActionResult::Success(Result);
	}

	auto& Registry = FMonolithToolRegistry::Get();

	// Step 1: Create the Blueprint
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("save_path"), SavePath);
		P->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		auto R = Registry.ExecuteAction(TEXT("blueprint"), TEXT("create_blueprint"), P);
		if (!R.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create Blueprint: %s"), *R.ErrorMessage));
		}
	}

	// Step 2: Add BoxComponent root
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), SavePath);
		P->SetStringField(TEXT("component_class"), TEXT("BoxComponent"));
		P->SetStringField(TEXT("component_name"), TEXT("VolumeExtent"));
		Registry.ExecuteAction(TEXT("blueprint"), TEXT("add_component"), P);
	}

	// Step 3: Configure box component
	auto SetComponentProp = [&](const FString& PropName, const FString& Value)
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), SavePath);
		P->SetStringField(TEXT("component_name"), TEXT("VolumeExtent"));
		P->SetStringField(TEXT("property_name"), PropName);
		P->SetStringField(TEXT("value"), Value);
		Registry.ExecuteAction(TEXT("blueprint"), TEXT("set_component_property"), P);
	};

	SetComponentProp(TEXT("BoxExtent"), TEXT("(X=200.0,Y=200.0,Z=150.0)"));
	SetComponentProp(TEXT("ShapeColor"), TEXT("(R=0,G=255,B=128,A=255)"));
	SetComponentProp(TEXT("bHiddenInGame"), TEXT("true"));
	SetComponentProp(TEXT("LineThickness"), TEXT("2.0"));

	// Step 4: Add blockout variables
	struct FVarDef { const TCHAR* Name; const TCHAR* Type; const TCHAR* Default; };
	const FVarDef Vars[] = {
		{ TEXT("RoomType"),     TEXT("string"),       TEXT("") },
		{ TEXT("BlockoutTags"), TEXT("array:string"), TEXT("") },
		{ TEXT("Density"),      TEXT("string"),       TEXT("Normal") },
		{ TEXT("bAllowPhysics"),TEXT("bool"),          TEXT("true") },
		{ TEXT("FloorHeight"),  TEXT("float"),         TEXT("0.0") },
		{ TEXT("bHasWalls"),    TEXT("bool"),          TEXT("true") },
		{ TEXT("bHasCeiling"),  TEXT("bool"),          TEXT("true") },
	};

	for (const auto& Var : Vars)
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), SavePath);
		P->SetStringField(TEXT("name"), Var.Name);
		P->SetStringField(TEXT("type"), Var.Type);
		if (FCString::Strlen(Var.Default) > 0)
		{
			P->SetStringField(TEXT("default_value"), Var.Default);
		}
		P->SetStringField(TEXT("category"), TEXT("Blockout"));
		P->SetBoolField(TEXT("instance_editable"), true);
		Registry.ExecuteAction(TEXT("blueprint"), TEXT("add_variable"), P);
	}

	// Step 5: Compile and save
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), SavePath);
		Registry.ExecuteAction(TEXT("blueprint"), TEXT("compile_blueprint"), P);
		Registry.ExecuteAction(TEXT("blueprint"), TEXT("save_asset"), P);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), SavePath);
	Result->SetBoolField(TEXT("created"), true);
	Result->SetNumberField(TEXT("variables"), 7);
	Result->SetStringField(TEXT("message"), TEXT("BP_MonolithBlockoutVolume created. Drag from Content Browser into levels. Configure RoomType, BlockoutTags, Density in Details panel."));
	return FMonolithActionResult::Success(Result);
}
