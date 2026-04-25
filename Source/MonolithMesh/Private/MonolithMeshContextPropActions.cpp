#include "MonolithMeshContextPropActions.h"
#include "MonolithMeshSceneActions.h"
#include "MonolithMeshBlockoutActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "CollisionQueryParams.h"
#include "WorldCollision.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Editor.h"
#include "Math/RandomStream.h"
#include "PhysicsEngine/BodyInstance.h"

// ============================================================================
// FScopedMeshTransaction
// ============================================================================

FMonolithMeshContextPropActions::FScopedMeshTransaction::FScopedMeshTransaction(const FText& Description)
	: bOwnsTransaction(!FMonolithMeshSceneActions::bBatchTransactionActive)
{
	if (bOwnsTransaction && GEditor)
	{
		GEditor->BeginTransaction(Description);
	}
}

FMonolithMeshContextPropActions::FScopedMeshTransaction::~FScopedMeshTransaction()
{
	if (bOwnsTransaction && GEditor)
	{
		GEditor->EndTransaction();
	}
}

void FMonolithMeshContextPropActions::FScopedMeshTransaction::Cancel()
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

TArray<TSharedPtr<FJsonValue>> FMonolithMeshContextPropActions::VectorToJsonArray(const FVector& V)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Add(MakeShared<FJsonValueNumber>(V.X));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
	return Arr;
}

TArray<TSharedPtr<FJsonValue>> FMonolithMeshContextPropActions::RotatorToJsonArray(const FRotator& R)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
	Arr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
	Arr.Add(MakeShared<FJsonValueNumber>(R.Roll));
	return Arr;
}

bool FMonolithMeshContextPropActions::ParseJsonArrayToVector(const TArray<TSharedPtr<FJsonValue>>& Arr, FVector& Out)
{
	if (Arr.Num() < 3) return false;
	Out.X = Arr[0]->AsNumber();
	Out.Y = Arr[1]->AsNumber();
	Out.Z = Arr[2]->AsNumber();
	return true;
}

FString FMonolithMeshContextPropActions::GetPropKitsDirectory()
{
	FString Dir = FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Saved") / TEXT("Monolith") / TEXT("PropKits");
	IFileManager::Get().MakeDirectory(*Dir, true);
	return Dir;
}

TSharedPtr<FJsonObject> FMonolithMeshContextPropActions::LoadPropKit(const FString& KitName, FString& OutError)
{
	FString KitDir = GetPropKitsDirectory();
	FString FilePath = KitDir / KitName + TEXT(".json");

	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *FilePath))
	{
		OutError = FString::Printf(TEXT("Prop kit file not found: %s"), *FilePath);
		return nullptr;
	}

	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to parse prop kit JSON: %s"), *FilePath);
		return nullptr;
	}

	return JsonObj;
}

bool FMonolithMeshContextPropActions::SavePropKit(const FString& KitName, const TSharedPtr<FJsonObject>& KitJson, FString& OutError)
{
	FString KitDir = GetPropKitsDirectory();
	FString FilePath = KitDir / KitName + TEXT(".json");

	FString JsonStr;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonStr);
	if (!FJsonSerializer::Serialize(KitJson.ToSharedRef(), Writer))
	{
		OutError = TEXT("Failed to serialize prop kit JSON");
		return false;
	}

	if (!FFileHelper::SaveStringToFile(JsonStr, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Failed to write prop kit file: %s"), *FilePath);
		return false;
	}

	return true;
}

TArray<FVector2D> FMonolithMeshContextPropActions::PoissonDiskSample2D(float Width, float Height, float MinSpacing, int32 MaxCount, FRandomStream& RandStream)
{
	TArray<FVector2D> Samples;
	if (Width <= 0 || Height <= 0 || MinSpacing <= 0)
	{
		return Samples;
	}

	float CellSize = MinSpacing / FMath::Sqrt(2.0f);
	int32 GridW = FMath::Max(1, FMath::CeilToInt32(Width / CellSize));
	int32 GridH = FMath::Max(1, FMath::CeilToInt32(Height / CellSize));

	// Background grid for acceleration (-1 = empty)
	TArray<int32> Grid;
	Grid.SetNumUninitialized(GridW * GridH);
	for (int32 i = 0; i < Grid.Num(); ++i) Grid[i] = -1;

	TArray<int32> ActiveList;

	// Seed the first sample
	FVector2D FirstPos(
		RandStream.FRandRange(0.0f, Width),
		RandStream.FRandRange(0.0f, Height)
	);

	Samples.Add(FirstPos);
	ActiveList.Add(0);
	int32 GX = FMath::Clamp(FMath::FloorToInt32(FirstPos.X / CellSize), 0, GridW - 1);
	int32 GY = FMath::Clamp(FMath::FloorToInt32(FirstPos.Y / CellSize), 0, GridH - 1);
	Grid[GY * GridW + GX] = 0;

	const int32 MaxAttempts = 30;

	while (ActiveList.Num() > 0 && Samples.Num() < MaxCount)
	{
		int32 ActiveIdx = RandStream.RandRange(0, ActiveList.Num() - 1);
		int32 SampleIdx = ActiveList[ActiveIdx];
		const FVector2D& Base = Samples[SampleIdx];

		bool bFound = false;
		for (int32 Attempt = 0; Attempt < MaxAttempts; ++Attempt)
		{
			float Angle = RandStream.FRandRange(0.0f, 2.0f * PI);
			float Dist = RandStream.FRandRange(MinSpacing, MinSpacing * 2.0f);
			FVector2D Candidate = Base + FVector2D(FMath::Cos(Angle) * Dist, FMath::Sin(Angle) * Dist);

			if (Candidate.X < 0 || Candidate.X >= Width || Candidate.Y < 0 || Candidate.Y >= Height)
			{
				continue;
			}

			int32 CX = FMath::Clamp(FMath::FloorToInt32(Candidate.X / CellSize), 0, GridW - 1);
			int32 CY = FMath::Clamp(FMath::FloorToInt32(Candidate.Y / CellSize), 0, GridH - 1);

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
						float DistSq = FVector2D::DistSquared(Candidate, Samples[NeighborIdx]);
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
				Samples.Add(Candidate);
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

	if (Samples.Num() > MaxCount)
	{
		Samples.SetNum(MaxCount);
	}

	return Samples;
}

TArray<AActor*> FMonolithMeshContextPropActions::FindActorsWithOwnerTag(UWorld* World, const FString& VolumeName)
{
	TArray<AActor*> Result;
	if (!World) return Result;

	FString OwnerTag = FString::Printf(TEXT("Monolith.Owner:%s"), *VolumeName);

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		for (const FName& Tag : Actor->Tags)
		{
			if (Tag.ToString().Equals(OwnerTag, ESearchCase::IgnoreCase))
			{
				Result.Add(Actor);
				break;
			}
		}
	}

	return Result;
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshContextPropActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 1. scatter_on_surface
	Registry.RegisterAction(TEXT("mesh"), TEXT("scatter_on_surface"),
		TEXT("Place props ON a specific surface actor (shelf top, table top, cabinet interior). Detects surface top via bounds + downward trace. Poisson disk spacing."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshContextPropActions::ScatterOnSurface),
		FParamSchemaBuilder()
			.Required(TEXT("surface_actor"), TEXT("string"), TEXT("Name or label of the surface actor to place props on"))
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Array of StaticMesh asset paths to scatter"))
			.Optional(TEXT("count"), TEXT("integer"), TEXT("Number of props to place (max 100)"), TEXT("5"))
			.Optional(TEXT("surface_side"), TEXT("string"), TEXT("Which surface to use: top (default) or inside (for cabinets)"), TEXT("top"))
			.Optional(TEXT("min_spacing"), TEXT("number"), TEXT("Minimum distance between props in cm"), TEXT("15"))
			.Optional(TEXT("random_rotation"), TEXT("boolean"), TEXT("Randomize yaw rotation"), TEXT("true"))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("Random seed (0 = random)"), TEXT("0"))
			.Optional(TEXT("random_scale_range"), TEXT("array"), TEXT("Scale range [min, max]"), TEXT("[0.9, 1.1]"))
			.Optional(TEXT("collision_mode"), TEXT("string"), TEXT("How to handle prop-geometry collisions: 'none' (always place), 'warn' (place but report overlaps), 'reject' (skip overlapping placements), 'adjust' (try push-out, reject if can't)"), TEXT("warn"))
			.Build());

	// 2. set_room_disturbance
	Registry.RegisterAction(TEXT("mesh"), TEXT("set_room_disturbance"),
		TEXT("Apply disturbance level to placed props in a volume: orderly (aligned), slightly_messy (small offsets), ransacked (large offsets, tipped), abandoned (pushed to edges). Single undo transaction."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshContextPropActions::SetRoomDisturbance),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Name of the blockout volume"))
			.Required(TEXT("disturbance"), TEXT("string"), TEXT("Disturbance level: orderly, slightly_messy, ransacked, or abandoned"))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("Random seed (0 = random)"), TEXT("42"))
			.Optional(TEXT("exclude_actors"), TEXT("array"), TEXT("Actor names to exclude from disturbance"))
			.Optional(TEXT("exclude_tags"), TEXT("array"), TEXT("Actors with these tags are excluded (e.g. 'fixed')"))
			.Build());

	// 3. configure_physics_props
	Registry.RegisterAction(TEXT("mesh"), TEXT("configure_physics_props"),
		TEXT("Batch-configure SimulatePhysics and sleep state on actors. Auto-sets Mobility to Movable. Optionally set mass and collision profile."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshContextPropActions::ConfigurePhysicsProps),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_names"), TEXT("array"), TEXT("Array of actor names/labels to configure"))
			.Optional(TEXT("volume_name"), TEXT("string"), TEXT("Configure all props in this blockout volume"))
			.Optional(TEXT("simulate_physics"), TEXT("boolean"), TEXT("Enable/disable physics simulation"), TEXT("true"))
			.Optional(TEXT("start_asleep"), TEXT("boolean"), TEXT("Put physics bodies to sleep after enabling"), TEXT("true"))
			.Optional(TEXT("mass_override"), TEXT("number"), TEXT("Mass override in kg (null = use default)"))
			.Optional(TEXT("collision_profile"), TEXT("string"), TEXT("Collision profile name"), TEXT("PhysicsActor"))
			.Build());

	// 4. settle_props
	Registry.RegisterAction(TEXT("mesh"), TEXT("settle_props"),
		TEXT("Trace-based gravity settle: for each prop, traces downward and snaps to hit surface with small random tilt. No PIE required. Single undo transaction."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshContextPropActions::SettleProps),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_names"), TEXT("array"), TEXT("Array of actor names/labels to settle"))
			.Optional(TEXT("volume_name"), TEXT("string"), TEXT("Settle all props in this blockout volume"))
			.Optional(TEXT("max_tilt"), TEXT("number"), TEXT("Maximum random tilt in degrees"), TEXT("5"))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("Random seed (0 = random)"), TEXT("0"))
			.Build());

	// 5. create_prop_kit
	Registry.RegisterAction(TEXT("mesh"), TEXT("create_prop_kit"),
		TEXT("Author a prop kit JSON file: items with relative positions, rotation, scale, spawn chances. Saved to Saved/Monolith/PropKits/."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshContextPropActions::CreatePropKit),
		FParamSchemaBuilder()
			.Required(TEXT("name"), TEXT("string"), TEXT("Kit name (used as filename without .json)"))
			.Required(TEXT("items"), TEXT("array"), TEXT("Array of item objects: {label, asset_path, offset:[x,y,z], rotation:[p,y,r], scale:1.0, required:bool, spawn_chance:0-1}"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Human-readable description of the kit"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Overwrite existing kit with same name"), TEXT("false"))
			.Build());

	// 6. place_prop_kit
	Registry.RegisterAction(TEXT("mesh"), TEXT("place_prop_kit"),
		TEXT("Spawn a prop kit at a world location. Random item selection based on spawn_chance. Single undo transaction."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshContextPropActions::PlacePropKit),
		FParamSchemaBuilder()
			.Required(TEXT("kit_name"), TEXT("string"), TEXT("Name of the prop kit to place"))
			.Required(TEXT("location"), TEXT("array"), TEXT("World location [x, y, z]"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("Rotation [pitch, yaw, roll]"), TEXT("[0,0,0]"))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("Random seed (0 = random)"), TEXT("0"))
			.Optional(TEXT("folder"), TEXT("string"), TEXT("Actor folder path in the outliner"))
			.Optional(TEXT("validate_placement"), TEXT("boolean"), TEXT("Validate each item with overlap check before spawning"), TEXT("true"))
			.Build());

	// 7. scatter_on_walls
	Registry.RegisterAction(TEXT("mesh"), TEXT("scatter_on_walls"),
		TEXT("Horizontal traces outward from volume center to find walls. Places props at hit points aligned to wall normal. For paintings, clocks, signs, sconces."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshContextPropActions::ScatterOnWalls),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Name of the blockout volume"))
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Array of StaticMesh asset paths to scatter"))
			.Optional(TEXT("count"), TEXT("integer"), TEXT("Number of props to place (max 100)"), TEXT("10"))
			.Optional(TEXT("wall_offset"), TEXT("number"), TEXT("Offset from wall surface in cm"), TEXT("2"))
			.Optional(TEXT("min_spacing"), TEXT("number"), TEXT("Minimum distance between wall props in cm"), TEXT("80"))
			.Optional(TEXT("height_range"), TEXT("array"), TEXT("Height range within volume [min_fraction, max_fraction] 0-1"), TEXT("[0.3, 0.8]"))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("Random seed (0 = random)"), TEXT("0"))
			.Optional(TEXT("collision_mode"), TEXT("string"), TEXT("How to handle prop-geometry collisions: 'none' (always place), 'warn' (place but report overlaps), 'reject' (skip overlapping placements), 'adjust' (try push-out, reject if can't)"), TEXT("warn"))
			.Build());

	// 8. scatter_on_ceiling
	Registry.RegisterAction(TEXT("mesh"), TEXT("scatter_on_ceiling"),
		TEXT("Upward traces to find ceiling. Places props hanging from hit points. For chains, cables, lights, pipes."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshContextPropActions::ScatterOnCeiling),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Name of the blockout volume"))
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Array of StaticMesh asset paths to scatter"))
			.Optional(TEXT("count"), TEXT("integer"), TEXT("Number of props to place (max 100)"), TEXT("8"))
			.Optional(TEXT("ceiling_offset"), TEXT("number"), TEXT("Offset below ceiling surface in cm"), TEXT("2"))
			.Optional(TEXT("min_spacing"), TEXT("number"), TEXT("Minimum distance between ceiling props in cm"), TEXT("100"))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("Random seed (0 = random)"), TEXT("0"))
			.Optional(TEXT("collision_mode"), TEXT("string"), TEXT("How to handle prop-geometry collisions: 'none' (always place), 'warn' (place but report overlaps), 'reject' (skip overlapping placements), 'adjust' (try push-out, reject if can't)"), TEXT("warn"))
			.Build());
}

// ============================================================================
// 1. scatter_on_surface
// ============================================================================

FMonolithActionResult FMonolithMeshContextPropActions::ScatterOnSurface(const TSharedPtr<FJsonObject>& Params)
{
	FString SurfaceActorName;
	if (!Params->TryGetStringField(TEXT("surface_actor"), SurfaceActorName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: surface_actor"));
	}

	const TArray<TSharedPtr<FJsonValue>>* AssetPathsArr;
	if (!Params->TryGetArrayField(TEXT("asset_paths"), AssetPathsArr) || AssetPathsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: asset_paths"));
	}

	double CountD = 5;
	Params->TryGetNumberField(TEXT("count"), CountD);
	int32 Count = FMath::Clamp(static_cast<int32>(CountD), 1, 100);

	FString SurfaceSide = TEXT("top");
	Params->TryGetStringField(TEXT("surface_side"), SurfaceSide);

	double MinSpacing = 15.0;
	Params->TryGetNumberField(TEXT("min_spacing"), MinSpacing);

	bool bRandomRotation = true;
	Params->TryGetBoolField(TEXT("random_rotation"), bRandomRotation);

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

	float ScaleMin = 0.9f, ScaleMax = 1.1f;
	const TArray<TSharedPtr<FJsonValue>>* ScaleRangeArr;
	if (Params->TryGetArrayField(TEXT("random_scale_range"), ScaleRangeArr) && ScaleRangeArr->Num() >= 2)
	{
		ScaleMin = static_cast<float>((*ScaleRangeArr)[0]->AsNumber());
		ScaleMax = static_cast<float>((*ScaleRangeArr)[1]->AsNumber());
	}

	FString CollisionMode = TEXT("warn");
	Params->TryGetStringField(TEXT("collision_mode"), CollisionMode);

	// Find the surface actor
	FString FindError;
	AActor* SurfaceActor = MonolithMeshUtils::FindActorByName(SurfaceActorName, FindError);
	if (!SurfaceActor)
	{
		return FMonolithActionResult::Error(FindError);
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

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Get surface actor bounds
	FVector SurfaceOrigin, SurfaceExtent;
	SurfaceActor->GetActorBounds(false, SurfaceOrigin, SurfaceExtent);

	// Compute the scatter region based on surface_side
	FVector ScatterMin, ScatterMax;
	float TraceStartZ, TraceEndZ;

	if (SurfaceSide.Equals(TEXT("inside"), ESearchCase::IgnoreCase))
	{
		// Inside mode for cabinets: scatter within the bounds interior
		ScatterMin = SurfaceOrigin - SurfaceExtent;
		ScatterMax = SurfaceOrigin + SurfaceExtent;
		TraceStartZ = SurfaceOrigin.Z + SurfaceExtent.Z;
		TraceEndZ = SurfaceOrigin.Z - SurfaceExtent.Z;
	}
	else
	{
		// Top mode: scatter on the top surface within XY footprint
		ScatterMin = FVector(SurfaceOrigin.X - SurfaceExtent.X, SurfaceOrigin.Y - SurfaceExtent.Y, SurfaceOrigin.Z + SurfaceExtent.Z);
		ScatterMax = FVector(SurfaceOrigin.X + SurfaceExtent.X, SurfaceOrigin.Y + SurfaceExtent.Y, SurfaceOrigin.Z + SurfaceExtent.Z);
		// Trace from above the top down to the top surface
		TraceStartZ = SurfaceOrigin.Z + SurfaceExtent.Z + 50.0f;
		TraceEndZ = SurfaceOrigin.Z;
	}

	float AreaWidth = ScatterMax.X - ScatterMin.X;
	float AreaHeight = ScatterMax.Y - ScatterMin.Y;

	if (AreaWidth < 1.0f || AreaHeight < 1.0f)
	{
		return FMonolithActionResult::Error(TEXT("Surface actor is too small for prop scattering (< 1cm in X or Y)"));
	}

	// Poisson disk sampling
	FRandomStream RandStream(Seed);
	TArray<FVector2D> Samples = PoissonDiskSample2D(AreaWidth, AreaHeight, static_cast<float>(MinSpacing), Count, RandStream);

	// Spawn actors
	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Scatter On Surface")));

	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(MonolithSurfaceTrace), true);

	int32 Placed = 0;
	int32 Rejected = 0;
	TArray<TSharedPtr<FJsonValue>> PlacedArr;
	TArray<FString> CollisionWarnings;

	// Collect spawned actors for ignore list
	TArray<AActor*> SpawnedActors;

	for (const FVector2D& Sample : Samples)
	{
		float WorldX = ScatterMin.X + Sample.X;
		float WorldY = ScatterMin.Y + Sample.Y;

		FVector TraceStart(WorldX, WorldY, TraceStartZ);
		FVector TraceEnd(WorldX, WorldY, TraceEndZ);

		FHitResult Hit;
		bool bHit = World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, TraceParams);

		// Verify the trace hit our target surface actor
		FVector SpawnLocation;
		if (bHit && Hit.GetActor() == SurfaceActor)
		{
			SpawnLocation = Hit.ImpactPoint;
		}
		else if (bHit)
		{
			// Hit something else — for "top" mode, still use the surface top Z
			SpawnLocation = FVector(WorldX, WorldY, ScatterMax.Z);
		}
		else
		{
			// No hit — use the computed top surface position
			SpawnLocation = FVector(WorldX, WorldY, ScatterMax.Z);
		}

		FRotator SpawnRotation = FRotator::ZeroRotator;
		if (bRandomRotation)
		{
			SpawnRotation.Yaw = RandStream.FRandRange(0.0f, 360.0f);
		}

		float Scale = RandStream.FRandRange(ScaleMin, ScaleMax);

		// Pick random mesh
		int32 MeshIdx = RandStream.RandRange(0, Meshes.Num() - 1);
		UStaticMesh* ChosenMesh = Meshes[MeshIdx];

		// Check prop bounds won't overhang the surface
		FBoxSphereBounds MeshBounds = ChosenMesh->GetBounds();
		float PropHalfX = MeshBounds.BoxExtent.X * Scale;
		float PropHalfY = MeshBounds.BoxExtent.Y * Scale;

		bool bOverhangs = (WorldX - PropHalfX < ScatterMin.X) ||
		                  (WorldX + PropHalfX > ScatterMax.X) ||
		                  (WorldY - PropHalfY < ScatterMin.Y) ||
		                  (WorldY + PropHalfY > ScatterMax.Y);

		if (bOverhangs)
		{
			continue; // Skip this placement to avoid overhang
		}

		// Collision validation (unless mode is "none")
		if (!CollisionMode.Equals(TEXT("none"), ESearchCase::IgnoreCase))
		{
			FVector PropHalfExtent = MeshBounds.BoxExtent * Scale * 0.9f;
			PropHalfExtent = PropHalfExtent.ComponentMax(FVector(1.0f));

			TArray<AActor*> IgnoreActors;
			IgnoreActors.Add(SurfaceActor); // Don't count the surface itself as an overlap
			IgnoreActors.Append(SpawnedActors);

			bool bAllowPushOut = CollisionMode.Equals(TEXT("adjust"), ESearchCase::IgnoreCase);
			MonolithMeshUtils::FPropPlacementResult PlacementResult = MonolithMeshUtils::ValidatePropPlacement(
				World, SpawnLocation, SpawnRotation.Quaternion(), PropHalfExtent, IgnoreActors, bAllowPushOut);

			if (!PlacementResult.bValid)
			{
				if (CollisionMode.Equals(TEXT("reject"), ESearchCase::IgnoreCase) || CollisionMode.Equals(TEXT("adjust"), ESearchCase::IgnoreCase))
				{
					Rejected++;
					continue;
				}
				// "warn" mode: place anyway but record warning
				CollisionWarnings.Add(FString::Printf(TEXT("Surface prop at (%.0f, %.0f, %.0f): %s"),
					SpawnLocation.X, SpawnLocation.Y, SpawnLocation.Z, *PlacementResult.RejectReason));
			}
			else
			{
				SpawnLocation = PlacementResult.FinalLocation;
				for (const FString& W : PlacementResult.Warnings)
				{
					CollisionWarnings.Add(W);
				}
			}
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AStaticMeshActor* PropActor = World->SpawnActor<AStaticMeshActor>(SpawnLocation, SpawnRotation, SpawnParams);
		if (!PropActor) continue;

		SpawnedActors.Add(PropActor);
		PropActor->GetStaticMeshComponent()->SetStaticMesh(ChosenMesh);
		PropActor->SetActorScale3D(FVector(Scale));

		PropActor->Tags.Add(FName(TEXT("Monolith.SurfaceProp")));
		PropActor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.SurfaceOwner:%s"), *SurfaceActorName)));

		PropActor->SetFolderPath(FName(*FString::Printf(TEXT("Props/Surface/%s"), *SurfaceActorName)));

		auto PlacedObj = MakeShared<FJsonObject>();
		PlacedObj->SetStringField(TEXT("actor"), PropActor->GetActorNameOrLabel());
		PlacedObj->SetStringField(TEXT("mesh"), ChosenMesh->GetPathName());
		PlacedObj->SetArrayField(TEXT("location"), VectorToJsonArray(SpawnLocation));
		PlacedObj->SetNumberField(TEXT("scale"), Scale);
		PlacedArr.Add(MakeShared<FJsonValueObject>(PlacedObj));

		Placed++;
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("placed"), Placed);
	Result->SetNumberField(TEXT("requested"), Count);
	Result->SetNumberField(TEXT("rejected"), Rejected);
	Result->SetStringField(TEXT("surface_actor"), SurfaceActorName);
	Result->SetStringField(TEXT("surface_side"), SurfaceSide);
	Result->SetStringField(TEXT("collision_mode"), CollisionMode);
	Result->SetNumberField(TEXT("seed"), Seed);
	Result->SetArrayField(TEXT("props"), PlacedArr);
	if (CollisionWarnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArr;
		for (const FString& W : CollisionWarnings)
		{
			WarningsArr.Add(MakeShared<FJsonValueString>(W));
		}
		Result->SetArrayField(TEXT("collision_warnings"), WarningsArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 2. set_room_disturbance
// ============================================================================

FMonolithActionResult FMonolithMeshContextPropActions::SetRoomDisturbance(const TSharedPtr<FJsonObject>& Params)
{
	FString VolumeName;
	if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: volume_name"));
	}

	FString Disturbance;
	if (!Params->TryGetStringField(TEXT("disturbance"), Disturbance))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: disturbance"));
	}

	// Validate disturbance level
	struct FDisturbanceConfig
	{
		float OffsetMin, OffsetMax;      // cm
		float RotationMin, RotationMax;  // degrees
		float ScaleVariance;
		float TippedPercent;             // 0-1
		float OnFloorPercent;            // 0-1
	};

	FDisturbanceConfig Config;
	if (Disturbance.Equals(TEXT("orderly"), ESearchCase::IgnoreCase))
	{
		Config = { 0.0f, 2.0f, 0.0f, 3.0f, 0.0f, 0.0f, 0.0f };
	}
	else if (Disturbance.Equals(TEXT("slightly_messy"), ESearchCase::IgnoreCase))
	{
		Config = { 5.0f, 15.0f, 5.0f, 15.0f, 0.05f, 0.05f, 0.0f };
	}
	else if (Disturbance.Equals(TEXT("ransacked"), ESearchCase::IgnoreCase))
	{
		Config = { 20.0f, 80.0f, 15.0f, 90.0f, 0.1f, 0.40f, 0.25f };
	}
	else if (Disturbance.Equals(TEXT("abandoned"), ESearchCase::IgnoreCase))
	{
		Config = { 30.0f, 100.0f, 15.0f, 90.0f, 0.15f, 0.50f, 0.40f };
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid disturbance level: '%s'. Use orderly, slightly_messy, ransacked, or abandoned."), *Disturbance));
	}

	int32 Seed = 42;
	double SeedD;
	if (Params->TryGetNumberField(TEXT("seed"), SeedD))
	{
		Seed = static_cast<int32>(SeedD);
	}
	if (Seed == 0)
	{
		Seed = FMath::Rand();
	}

	// Parse exclusions
	TSet<FString> ExcludeActorNames;
	const TArray<TSharedPtr<FJsonValue>>* ExcludeActorsArr;
	if (Params->TryGetArrayField(TEXT("exclude_actors"), ExcludeActorsArr))
	{
		for (const auto& Val : *ExcludeActorsArr)
		{
			ExcludeActorNames.Add(Val->AsString());
		}
	}

	TSet<FString> ExcludeTags;
	const TArray<TSharedPtr<FJsonValue>>* ExcludeTagsArr;
	if (Params->TryGetArrayField(TEXT("exclude_tags"), ExcludeTagsArr))
	{
		for (const auto& Val : *ExcludeTagsArr)
		{
			ExcludeTags.Add(Val->AsString());
		}
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Find all owned actors
	TArray<AActor*> OwnedActors = FindActorsWithOwnerTag(World, VolumeName);
	if (OwnedActors.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No actors found with tag Monolith.Owner:%s"), *VolumeName));
	}

	// Get volume bounds for "abandoned" edge-pushing and floor traces
	FVector VolumeOrigin = FVector::ZeroVector, VolumeExtent = FVector::ZeroVector;
	// Try to find the volume for bounds context
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor->GetActorNameOrLabel() == VolumeName || Actor->GetActorLabel() == VolumeName)
		{
			Actor->GetActorBounds(false, VolumeOrigin, VolumeExtent);
			break;
		}
	}
	FVector VolumeMin = VolumeOrigin - VolumeExtent;
	FVector VolumeMax = VolumeOrigin + VolumeExtent;

	FRandomStream RandStream(Seed);
	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Set Room Disturbance")));

	int32 Modified = 0;
	int32 TippedOver = 0;
	int32 MovedToFloor = 0;
	int32 Unchanged = 0;

	FCollisionQueryParams FloorTraceParams(SCENE_QUERY_STAT(MonolithDisturbanceFloorTrace), true);

	for (AActor* Actor : OwnedActors)
	{
		// Check exclusions
		FString ActorName = Actor->GetActorNameOrLabel();
		if (ExcludeActorNames.Contains(ActorName))
		{
			Unchanged++;
			continue;
		}

		bool bExcluded = false;
		for (const FString& ExTag : ExcludeTags)
		{
			for (const FName& Tag : Actor->Tags)
			{
				if (Tag.ToString().Contains(ExTag))
				{
					bExcluded = true;
					break;
				}
			}
			if (bExcluded) break;
		}
		if (bExcluded)
		{
			Unchanged++;
			continue;
		}

		// Large objects resist disturbance — check bounds
		FVector ActorOrigin, ActorExtent;
		Actor->GetActorBounds(false, ActorOrigin, ActorExtent);
		float ActorSize = ActorExtent.GetMax();

		// Objects larger than 150cm half-extent are "large" and get reduced disturbance
		float SizeMultiplier = (ActorSize > 150.0f) ? 0.3f : 1.0f;

		Actor->Modify();

		FVector CurrentLoc = Actor->GetActorLocation();
		FRotator CurrentRot = Actor->GetActorRotation();
		FVector CurrentScale = Actor->GetActorScale3D();

		// Apply offset
		float OffsetX = RandStream.FRandRange(Config.OffsetMin, Config.OffsetMax) * SizeMultiplier;
		float OffsetY = RandStream.FRandRange(Config.OffsetMin, Config.OffsetMax) * SizeMultiplier;
		// Randomize direction
		if (RandStream.FRand() > 0.5f) OffsetX = -OffsetX;
		if (RandStream.FRand() > 0.5f) OffsetY = -OffsetY;

		FVector NewLoc = CurrentLoc + FVector(OffsetX, OffsetY, 0.0f);

		// For "abandoned" mode, push toward nearest wall (volume edges)
		if (Disturbance.Equals(TEXT("abandoned"), ESearchCase::IgnoreCase) && VolumeExtent.GetMax() > 0)
		{
			// Find nearest wall axis and push toward it
			float DistToMinX = FMath::Abs(CurrentLoc.X - VolumeMin.X);
			float DistToMaxX = FMath::Abs(CurrentLoc.X - VolumeMax.X);
			float DistToMinY = FMath::Abs(CurrentLoc.Y - VolumeMin.Y);
			float DistToMaxY = FMath::Abs(CurrentLoc.Y - VolumeMax.Y);

			float MinDist = FMath::Min(FMath::Min(DistToMinX, DistToMaxX), FMath::Min(DistToMinY, DistToMaxY));
			float PushStrength = RandStream.FRandRange(0.3f, 0.7f) * SizeMultiplier;

			if (MinDist == DistToMinX)
				NewLoc.X = FMath::Lerp(CurrentLoc.X, VolumeMin.X + ActorExtent.X, PushStrength);
			else if (MinDist == DistToMaxX)
				NewLoc.X = FMath::Lerp(CurrentLoc.X, VolumeMax.X - ActorExtent.X, PushStrength);
			else if (MinDist == DistToMinY)
				NewLoc.Y = FMath::Lerp(CurrentLoc.Y, VolumeMin.Y + ActorExtent.Y, PushStrength);
			else
				NewLoc.Y = FMath::Lerp(CurrentLoc.Y, VolumeMax.Y - ActorExtent.Y, PushStrength);
		}

		// Apply rotation disturbance
		FRotator NewRot = CurrentRot;
		float RotDist = RandStream.FRandRange(Config.RotationMin, Config.RotationMax) * SizeMultiplier;
		NewRot.Yaw += RandStream.FRandRange(-RotDist, RotDist);

		// Check if this actor gets tipped over
		bool bTipped = false;
		if (RandStream.FRand() < Config.TippedPercent * SizeMultiplier)
		{
			float TipAngle = RandStream.FRandRange(60.0f, 90.0f);
			// Tip along a random axis (pitch or roll)
			if (RandStream.FRand() > 0.5f)
				NewRot.Pitch += TipAngle;
			else
				NewRot.Roll += TipAngle;
			bTipped = true;
			TippedOver++;
		}

		// Check if this actor gets moved to floor
		bool bOnFloor = false;
		if (RandStream.FRand() < Config.OnFloorPercent * SizeMultiplier)
		{
			// Trace to find the floor
			FloorTraceParams.ClearIgnoredSourceObjects();
			FloorTraceParams.AddIgnoredActor(Actor);

			FHitResult FloorHit;
			FVector FloorTraceStart = FVector(NewLoc.X, NewLoc.Y, VolumeMax.Z);
			FVector FloorTraceEnd = FVector(NewLoc.X, NewLoc.Y, VolumeMin.Z - 100.0f);

			if (World->LineTraceSingleByChannel(FloorHit, FloorTraceStart, FloorTraceEnd, ECC_Visibility, FloorTraceParams))
			{
				NewLoc.Z = FloorHit.ImpactPoint.Z + ActorExtent.Z;
			}
			else
			{
				NewLoc.Z = VolumeMin.Z;
			}

			// Also tip it when on floor
			if (!bTipped)
			{
				float TipAngle = RandStream.FRandRange(60.0f, 90.0f);
				if (RandStream.FRand() > 0.5f)
					NewRot.Pitch += TipAngle;
				else
					NewRot.Roll += TipAngle;
				TippedOver++;
			}

			bOnFloor = true;
			MovedToFloor++;
		}

		// Apply scale variance
		FVector NewScale = CurrentScale;
		if (Config.ScaleVariance > 0.0f)
		{
			float ScaleMod = 1.0f + RandStream.FRandRange(-Config.ScaleVariance, Config.ScaleVariance);
			NewScale = CurrentScale * ScaleMod;
		}

		// For "orderly" mode, snap to grid and align rotations
		if (Disturbance.Equals(TEXT("orderly"), ESearchCase::IgnoreCase))
		{
			// Snap yaw to nearest 90 degrees
			NewRot.Yaw = FMath::RoundToFloat(CurrentRot.Yaw / 90.0f) * 90.0f;
			NewRot.Pitch = 0.0f;
			NewRot.Roll = 0.0f;
			// Keep position mostly unchanged, just tiny alignment nudge
		}

		Actor->SetActorLocation(NewLoc);
		Actor->SetActorRotation(NewRot);
		Actor->SetActorScale3D(NewScale);

		Modified++;
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("modified_count"), Modified);
	Result->SetNumberField(TEXT("tipped_over"), TippedOver);
	Result->SetNumberField(TEXT("moved_to_floor"), MovedToFloor);
	Result->SetNumberField(TEXT("unchanged"), Unchanged);
	Result->SetStringField(TEXT("disturbance"), Disturbance);
	Result->SetNumberField(TEXT("seed"), Seed);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 3. configure_physics_props
// ============================================================================

FMonolithActionResult FMonolithMeshContextPropActions::ConfigurePhysicsProps(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Collect target actors from actor_names or volume_name
	TArray<AActor*> Targets;

	const TArray<TSharedPtr<FJsonValue>>* ActorNamesArr;
	FString VolumeName;
	if (Params->TryGetArrayField(TEXT("actor_names"), ActorNamesArr) && ActorNamesArr->Num() > 0)
	{
		for (const auto& Val : *ActorNamesArr)
		{
			FString FindError;
			AActor* Actor = MonolithMeshUtils::FindActorByName(Val->AsString(), FindError);
			if (!Actor)
			{
				return FMonolithActionResult::Error(FindError);
			}
			Targets.Add(Actor);
		}
	}
	else if (Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		Targets = FindActorsWithOwnerTag(World, VolumeName);
		if (Targets.Num() == 0)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("No actors found with tag Monolith.Owner:%s"), *VolumeName));
		}
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Provide either actor_names or volume_name"));
	}

	bool bSimulatePhysics = true;
	Params->TryGetBoolField(TEXT("simulate_physics"), bSimulatePhysics);

	bool bStartAsleep = true;
	Params->TryGetBoolField(TEXT("start_asleep"), bStartAsleep);

	double MassOverride = -1.0;
	bool bHasMassOverride = Params->TryGetNumberField(TEXT("mass_override"), MassOverride);

	FString CollisionProfile = TEXT("PhysicsActor");
	Params->TryGetStringField(TEXT("collision_profile"), CollisionProfile);

	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Configure Physics Props")));

	int32 Configured = 0;
	TArray<TSharedPtr<FJsonValue>> ConfiguredArr;
	TArray<FString> Warnings;

	for (AActor* Actor : Targets)
	{
		UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
		if (!PrimComp)
		{
			Warnings.Add(FString::Printf(TEXT("'%s': no PrimitiveComponent root, skipped"), *Actor->GetActorNameOrLabel()));
			continue;
		}

		Actor->Modify();
		PrimComp->Modify();

		// Must set Movable before enabling physics
		if (PrimComp->Mobility != EComponentMobility::Movable)
		{
			PrimComp->SetMobility(EComponentMobility::Movable);
			Warnings.Add(FString::Printf(TEXT("'%s': auto-set Mobility to Movable"), *Actor->GetActorNameOrLabel()));
		}

		// Set collision profile
		PrimComp->SetCollisionProfileName(FName(*CollisionProfile));

		// Enable/disable physics
		PrimComp->SetSimulatePhysics(bSimulatePhysics);

		// Mass override
		if (bHasMassOverride && MassOverride > 0)
		{
			FBodyInstance* Body = PrimComp->GetBodyInstance();
			if (Body)
			{
				Body->SetMassOverride(static_cast<float>(MassOverride));
			}
		}

		// Put to sleep (must happen after physics is enabled and body is in scene)
		if (bSimulatePhysics && bStartAsleep)
		{
			FBodyInstance* Body = PrimComp->GetBodyInstance();
			if (Body)
			{
				Body->PutInstanceToSleep();
			}
		}

		auto ConfigObj = MakeShared<FJsonObject>();
		ConfigObj->SetStringField(TEXT("actor"), Actor->GetActorNameOrLabel());
		ConfigObj->SetBoolField(TEXT("simulate_physics"), bSimulatePhysics);
		ConfigObj->SetBoolField(TEXT("asleep"), bSimulatePhysics && bStartAsleep);
		ConfiguredArr.Add(MakeShared<FJsonValueObject>(ConfigObj));

		Configured++;
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("configured"), Configured);
	Result->SetArrayField(TEXT("actors"), ConfiguredArr);
	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArr;
		for (const FString& W : Warnings)
		{
			WarningsArr.Add(MakeShared<FJsonValueString>(W));
		}
		Result->SetArrayField(TEXT("warnings"), WarningsArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 4. settle_props
// ============================================================================

FMonolithActionResult FMonolithMeshContextPropActions::SettleProps(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Collect target actors
	TArray<AActor*> Targets;

	const TArray<TSharedPtr<FJsonValue>>* ActorNamesArr;
	FString VolumeName;
	if (Params->TryGetArrayField(TEXT("actor_names"), ActorNamesArr) && ActorNamesArr->Num() > 0)
	{
		for (const auto& Val : *ActorNamesArr)
		{
			FString FindError;
			AActor* Actor = MonolithMeshUtils::FindActorByName(Val->AsString(), FindError);
			if (!Actor)
			{
				return FMonolithActionResult::Error(FindError);
			}
			Targets.Add(Actor);
		}
	}
	else if (Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		Targets = FindActorsWithOwnerTag(World, VolumeName);
		if (Targets.Num() == 0)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("No actors found with tag Monolith.Owner:%s"), *VolumeName));
		}
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Provide either actor_names or volume_name"));
	}

	double MaxTilt = 5.0;
	Params->TryGetNumberField(TEXT("max_tilt"), MaxTilt);

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

	FRandomStream RandStream(Seed);
	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Settle Props")));

	int32 Settled = 0;
	int32 FellThrough = 0;
	TArray<TSharedPtr<FJsonValue>> SettledArr;

	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(MonolithSettleTrace), true);

	for (AActor* Actor : Targets)
	{
		TraceParams.ClearIgnoredSourceObjects();
		TraceParams.AddIgnoredActor(Actor);

		FVector ActorOrigin, ActorExtent;
		Actor->GetActorBounds(false, ActorOrigin, ActorExtent);

		FVector OriginalLoc = Actor->GetActorLocation();
		FRotator OriginalRot = Actor->GetActorRotation();

		// Use SweepSingle with prop bounding box instead of LineTrace
		// This prevents props from settling through thin geometry (shelves, platforms)
		FCollisionShape PropShape = FCollisionShape::MakeBox(
			FVector(ActorExtent.X * 0.9f, ActorExtent.Y * 0.9f, 1.0f).ComponentMax(FVector(1.0f)));

		FVector SweepStart = ActorOrigin;
		FVector SweepEnd = ActorOrigin - FVector(0, 0, 10000.0f);

		FHitResult Hit;
		bool bHit = World->SweepSingleByChannel(
			Hit, SweepStart, SweepEnd, FQuat::Identity,
			ECC_WorldStatic, PropShape, TraceParams);

		if (!bHit)
		{
			// Fallback: try a simple line trace in case sweep misses (e.g., brush geometry)
			bHit = World->LineTraceSingleByChannel(Hit, SweepStart, SweepEnd, ECC_Visibility, TraceParams);
			if (!bHit)
			{
				FellThrough++;
				continue; // Nothing to settle onto
			}
		}

		if (Hit.bStartPenetrating)
		{
			// Already embedded in geometry -- skip to avoid making it worse
			FellThrough++;
			continue;
		}

		Actor->Modify();

		// Compute settled location: hit point + half extent in Z (so bottom rests on surface)
		FVector SettledLoc = FVector(OriginalLoc.X, OriginalLoc.Y, Hit.ImpactPoint.Z + ActorExtent.Z);

		// Add small random tilt for "someone dropped this" look
		FRotator SettledRot = OriginalRot;
		float TiltAmount = static_cast<float>(MaxTilt);
		SettledRot.Pitch += RandStream.FRandRange(-TiltAmount, TiltAmount);
		SettledRot.Roll += RandStream.FRandRange(-TiltAmount, TiltAmount);

		Actor->SetActorLocation(SettledLoc);
		Actor->SetActorRotation(SettledRot);

		float FellDistance = OriginalLoc.Z - SettledLoc.Z;

		auto SettledObj = MakeShared<FJsonObject>();
		SettledObj->SetStringField(TEXT("name"), Actor->GetActorNameOrLabel());
		SettledObj->SetArrayField(TEXT("original_location"), VectorToJsonArray(OriginalLoc));
		SettledObj->SetArrayField(TEXT("settled_location"), VectorToJsonArray(SettledLoc));
		SettledObj->SetNumberField(TEXT("fell_distance"), FellDistance);
		SettledArr.Add(MakeShared<FJsonValueObject>(SettledObj));

		Settled++;
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("settled_count"), Settled);
	if (FellThrough > 0)
	{
		Result->SetNumberField(TEXT("skipped_no_surface"), FellThrough);
	}
	Result->SetNumberField(TEXT("seed"), Seed);
	Result->SetArrayField(TEXT("actors"), SettledArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 5. create_prop_kit
// ============================================================================

FMonolithActionResult FMonolithMeshContextPropActions::CreatePropKit(const TSharedPtr<FJsonObject>& Params)
{
	FString KitName;
	if (!Params->TryGetStringField(TEXT("name"), KitName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: name"));
	}

	const TArray<TSharedPtr<FJsonValue>>* ItemsArr;
	if (!Params->TryGetArrayField(TEXT("items"), ItemsArr) || ItemsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: items"));
	}

	bool bOverwrite = false;
	Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

	// Check if kit already exists
	if (!bOverwrite)
	{
		FString ExistingError;
		TSharedPtr<FJsonObject> Existing = LoadPropKit(KitName, ExistingError);
		if (Existing.IsValid())
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Prop kit '%s' already exists. Use overwrite: true to replace."), *KitName));
		}
	}

	// Validate items and build kit JSON
	TArray<TSharedPtr<FJsonValue>> ValidatedItems;
	for (int32 i = 0; i < ItemsArr->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>* ItemObjPtr;
		if (!(*ItemsArr)[i]->TryGetObject(ItemObjPtr) || !ItemObjPtr || !(*ItemObjPtr).IsValid())
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Item %d is not a valid JSON object"), i));
		}
		const TSharedPtr<FJsonObject>& ItemObj = *ItemObjPtr;

		// Require at least label and asset_path
		FString Label, AssetPath;
		if (!ItemObj->TryGetStringField(TEXT("label"), Label))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Item %d missing 'label'"), i));
		}
		if (!ItemObj->TryGetStringField(TEXT("asset_path"), AssetPath))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Item %d ('%s') missing 'asset_path'"), i, *Label));
		}

		// Validate the mesh exists
		FString MeshError;
		UStaticMesh* Mesh = MonolithMeshUtils::LoadStaticMesh(AssetPath, MeshError);
		if (!Mesh)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Item %d ('%s'): %s"), i, *Label, *MeshError));
		}

		// Build the item with defaults
		auto ValidItem = MakeShared<FJsonObject>();
		ValidItem->SetStringField(TEXT("label"), Label);
		ValidItem->SetStringField(TEXT("asset_path"), AssetPath);

		// Copy offset, rotation, scale, required, spawn_chance with defaults
		const TArray<TSharedPtr<FJsonValue>>* OffsetArr;
		if (ItemObj->TryGetArrayField(TEXT("offset"), OffsetArr))
		{
			ValidItem->SetArrayField(TEXT("offset"), *OffsetArr);
		}
		else
		{
			ValidItem->SetArrayField(TEXT("offset"), VectorToJsonArray(FVector::ZeroVector));
		}

		const TArray<TSharedPtr<FJsonValue>>* RotArr;
		if (ItemObj->TryGetArrayField(TEXT("rotation"), RotArr))
		{
			ValidItem->SetArrayField(TEXT("rotation"), *RotArr);
		}
		else
		{
			ValidItem->SetArrayField(TEXT("rotation"), RotatorToJsonArray(FRotator::ZeroRotator));
		}

		double Scale = 1.0;
		ItemObj->TryGetNumberField(TEXT("scale"), Scale);
		ValidItem->SetNumberField(TEXT("scale"), Scale);

		bool bRequired = false;
		ItemObj->TryGetBoolField(TEXT("required"), bRequired);
		ValidItem->SetBoolField(TEXT("required"), bRequired);

		double SpawnChance = 1.0;
		ItemObj->TryGetNumberField(TEXT("spawn_chance"), SpawnChance);
		ValidItem->SetNumberField(TEXT("spawn_chance"), FMath::Clamp(SpawnChance, 0.0, 1.0));

		// Optional rotation_range for random yaw
		const TArray<TSharedPtr<FJsonValue>>* RotRangeArr;
		if (ItemObj->TryGetArrayField(TEXT("rotation_range"), RotRangeArr) && RotRangeArr->Num() >= 2)
		{
			ValidItem->SetArrayField(TEXT("rotation_range"), *RotRangeArr);
		}

		ValidatedItems.Add(MakeShared<FJsonValueObject>(ValidItem));
	}

	// Build the full kit object
	auto KitObj = MakeShared<FJsonObject>();
	KitObj->SetStringField(TEXT("name"), KitName);

	FString Description;
	if (Params->TryGetStringField(TEXT("description"), Description))
	{
		KitObj->SetStringField(TEXT("description"), Description);
	}

	KitObj->SetArrayField(TEXT("items"), ValidatedItems);

	// Save
	FString SaveError;
	if (!SavePropKit(KitName, KitObj, SaveError))
	{
		return FMonolithActionResult::Error(SaveError);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), KitName);
	Result->SetNumberField(TEXT("item_count"), ValidatedItems.Num());
	Result->SetStringField(TEXT("file_path"), GetPropKitsDirectory() / KitName + TEXT(".json"));
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Prop kit '%s' saved with %d items"), *KitName, ValidatedItems.Num()));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 6. place_prop_kit
// ============================================================================

FMonolithActionResult FMonolithMeshContextPropActions::PlacePropKit(const TSharedPtr<FJsonObject>& Params)
{
	FString KitName;
	if (!Params->TryGetStringField(TEXT("kit_name"), KitName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: kit_name"));
	}

	FVector Location;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: location"));
	}

	FRotator Rotation = FRotator::ZeroRotator;
	MonolithMeshUtils::ParseRotator(Params, TEXT("rotation"), Rotation);

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

	FString FolderPath;
	Params->TryGetStringField(TEXT("folder"), FolderPath);

	bool bValidatePlacement = true;
	Params->TryGetBoolField(TEXT("validate_placement"), bValidatePlacement);

	// Load the kit
	FString LoadError;
	TSharedPtr<FJsonObject> KitObj = LoadPropKit(KitName, LoadError);
	if (!KitObj)
	{
		return FMonolithActionResult::Error(LoadError);
	}

	const TArray<TSharedPtr<FJsonValue>>* ItemsArr;
	if (!KitObj->TryGetArrayField(TEXT("items"), ItemsArr) || ItemsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Prop kit has no items"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FRandomStream RandStream(Seed);
	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Place Prop Kit")));

	// Build rotation matrix for transforming offsets
	FQuat KitQuat = Rotation.Quaternion();

	int32 Placed = 0;
	int32 Rejected = 0;
	TArray<TSharedPtr<FJsonValue>> PlacedArr;
	TArray<FString> SkippedMissingAssets;
	TArray<FString> CollisionWarnings;

	// Collect spawned actors for ignore list during validation
	TArray<AActor*> SpawnedActors;

	for (const auto& ItemVal : *ItemsArr)
	{
		const TSharedPtr<FJsonObject>* ItemObjPtr;
		if (!ItemVal->TryGetObject(ItemObjPtr) || !ItemObjPtr || !(*ItemObjPtr).IsValid())
		{
			continue;
		}
		const TSharedPtr<FJsonObject>& ItemObj = *ItemObjPtr;

		// Check spawn_chance
		double SpawnChance = 1.0;
		ItemObj->TryGetNumberField(TEXT("spawn_chance"), SpawnChance);

		bool bRequired = false;
		ItemObj->TryGetBoolField(TEXT("required"), bRequired);

		if (!bRequired && RandStream.FRand() > SpawnChance)
		{
			continue; // Skipped by RNG
		}

		FString AssetPath;
		if (!ItemObj->TryGetStringField(TEXT("asset_path"), AssetPath))
		{
			continue;
		}

		FString MeshError;
		UStaticMesh* Mesh = MonolithMeshUtils::LoadStaticMesh(AssetPath, MeshError);
		if (!Mesh)
		{
			SkippedMissingAssets.Add(AssetPath);
			continue;
		}

		// Parse offset
		FVector Offset = FVector::ZeroVector;
		const TArray<TSharedPtr<FJsonValue>>* OffsetArr;
		if (ItemObj->TryGetArrayField(TEXT("offset"), OffsetArr))
		{
			ParseJsonArrayToVector(*OffsetArr, Offset);
		}

		// Parse rotation
		FRotator ItemRotation = FRotator::ZeroRotator;
		const TArray<TSharedPtr<FJsonValue>>* RotArr;
		if (ItemObj->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr->Num() >= 3)
		{
			ItemRotation.Pitch = static_cast<float>((*RotArr)[0]->AsNumber());
			ItemRotation.Yaw = static_cast<float>((*RotArr)[1]->AsNumber());
			ItemRotation.Roll = static_cast<float>((*RotArr)[2]->AsNumber());
		}

		// Check for rotation_range (random yaw within range)
		const TArray<TSharedPtr<FJsonValue>>* RotRangeArr;
		if (ItemObj->TryGetArrayField(TEXT("rotation_range"), RotRangeArr) && RotRangeArr->Num() >= 2)
		{
			float RotMin = static_cast<float>((*RotRangeArr)[0]->AsNumber());
			float RotMax = static_cast<float>((*RotRangeArr)[1]->AsNumber());
			ItemRotation.Yaw += RandStream.FRandRange(RotMin, RotMax);
		}

		double ItemScale = 1.0;
		ItemObj->TryGetNumberField(TEXT("scale"), ItemScale);

		// Transform offset by kit rotation and add to kit location
		FVector WorldOffset = KitQuat.RotateVector(Offset);
		FVector SpawnLocation = Location + WorldOffset;

		// Combine rotations
		FRotator SpawnRotation = (KitQuat * ItemRotation.Quaternion()).Rotator();

		// Collision validation (if enabled)
		if (bValidatePlacement)
		{
			FVector PropHalfExtent = Mesh->GetBounds().BoxExtent * FVector(static_cast<float>(ItemScale)) * 0.9f;
			PropHalfExtent = PropHalfExtent.ComponentMax(FVector(1.0f));

			TArray<AActor*> IgnoreActors;
			IgnoreActors.Append(SpawnedActors);

			MonolithMeshUtils::FPropPlacementResult PlacementResult = MonolithMeshUtils::ValidatePropPlacement(
				World, SpawnLocation, SpawnRotation.Quaternion(), PropHalfExtent, IgnoreActors, false);

			if (!PlacementResult.bValid)
			{
				FString Label;
				ItemObj->TryGetStringField(TEXT("label"), Label);
				CollisionWarnings.Add(FString::Printf(TEXT("Kit item '%s' at (%.0f, %.0f, %.0f): %s"),
					*Label, SpawnLocation.X, SpawnLocation.Y, SpawnLocation.Z, *PlacementResult.RejectReason));
				Rejected++;
				continue;
			}
			else
			{
				SpawnLocation = PlacementResult.FinalLocation;
				for (const FString& W : PlacementResult.Warnings)
				{
					CollisionWarnings.Add(W);
				}
			}
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AStaticMeshActor* PropActor = World->SpawnActor<AStaticMeshActor>(SpawnLocation, SpawnRotation, SpawnParams);
		if (!PropActor) continue;

		SpawnedActors.Add(PropActor);
		PropActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		PropActor->SetActorScale3D(FVector(static_cast<float>(ItemScale)));

		FString Label;
		ItemObj->TryGetStringField(TEXT("label"), Label);
		if (!Label.IsEmpty())
		{
			PropActor->SetActorLabel(Label);
		}

		PropActor->Tags.Add(FName(TEXT("Monolith.PropKit")));
		PropActor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Kit:%s"), *KitName)));

		if (!FolderPath.IsEmpty())
		{
			PropActor->SetFolderPath(FName(*FolderPath));
		}
		else
		{
			PropActor->SetFolderPath(FName(*FString::Printf(TEXT("Props/Kits/%s"), *KitName)));
		}

		auto PlacedObj = MakeShared<FJsonObject>();
		PlacedObj->SetStringField(TEXT("actor"), PropActor->GetActorNameOrLabel());
		PlacedObj->SetStringField(TEXT("mesh"), Mesh->GetPathName());
		PlacedObj->SetArrayField(TEXT("location"), VectorToJsonArray(SpawnLocation));
		if (!Label.IsEmpty())
		{
			PlacedObj->SetStringField(TEXT("label"), Label);
		}
		PlacedArr.Add(MakeShared<FJsonValueObject>(PlacedObj));

		Placed++;
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("placed_count"), Placed);
	Result->SetNumberField(TEXT("rejected"), Rejected);
	Result->SetStringField(TEXT("kit_name"), KitName);
	Result->SetBoolField(TEXT("validate_placement"), bValidatePlacement);
	Result->SetNumberField(TEXT("seed"), Seed);
	Result->SetArrayField(TEXT("actors"), PlacedArr);
	if (SkippedMissingAssets.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> SkippedArr;
		for (const FString& S : SkippedMissingAssets)
		{
			SkippedArr.Add(MakeShared<FJsonValueString>(S));
		}
		Result->SetArrayField(TEXT("skipped_missing_assets"), SkippedArr);
	}
	if (CollisionWarnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArr;
		for (const FString& W : CollisionWarnings)
		{
			WarningsArr.Add(MakeShared<FJsonValueString>(W));
		}
		Result->SetArrayField(TEXT("collision_warnings"), WarningsArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 7. scatter_on_walls
// ============================================================================

FMonolithActionResult FMonolithMeshContextPropActions::ScatterOnWalls(const TSharedPtr<FJsonObject>& Params)
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

	double CountD = 10;
	Params->TryGetNumberField(TEXT("count"), CountD);
	int32 Count = FMath::Clamp(static_cast<int32>(CountD), 1, 100);

	double WallOffset = 2.0;
	Params->TryGetNumberField(TEXT("wall_offset"), WallOffset);

	double MinSpacing = 80.0;
	Params->TryGetNumberField(TEXT("min_spacing"), MinSpacing);

	float HeightMin = 0.3f, HeightMax = 0.8f;
	const TArray<TSharedPtr<FJsonValue>>* HeightRangeArr;
	if (Params->TryGetArrayField(TEXT("height_range"), HeightRangeArr) && HeightRangeArr->Num() >= 2)
	{
		HeightMin = static_cast<float>((*HeightRangeArr)[0]->AsNumber());
		HeightMax = static_cast<float>((*HeightRangeArr)[1]->AsNumber());
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

	FString CollisionMode = TEXT("warn");
	Params->TryGetStringField(TEXT("collision_mode"), CollisionMode);

	// Validate meshes
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

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Find volume — use FindActorByName for generic volume lookup
	FString FindError;
	AActor* Volume = MonolithMeshUtils::FindActorByName(VolumeName, FindError);
	if (!Volume)
	{
		return FMonolithActionResult::Error(FindError);
	}

	FVector VolumeOrigin, VolumeExtent;
	Volume->GetActorBounds(false, VolumeOrigin, VolumeExtent);
	FVector VolumeMin = VolumeOrigin - VolumeExtent;
	FVector VolumeMax = VolumeOrigin + VolumeExtent;

	FRandomStream RandStream(Seed);

	// Generate candidate wall positions by tracing horizontally from interior points
	// Use multiple sample heights within the height range
	struct FWallHit
	{
		FVector Location;
		FVector Normal;
	};
	TArray<FWallHit> WallHits;

	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(MonolithWallTrace), true);
	TraceParams.AddIgnoredActor(Volume);

	int32 MaxAttempts = Count * 10; // More attempts because wall hits have more misses
	float VolumeHeight = VolumeExtent.Z * 2.0f;

	for (int32 Attempt = 0; Attempt < MaxAttempts && WallHits.Num() < Count * 3; ++Attempt)
	{
		// Random interior point
		float SampleX = RandStream.FRandRange(VolumeMin.X + VolumeExtent.X * 0.2f, VolumeMax.X - VolumeExtent.X * 0.2f);
		float SampleY = RandStream.FRandRange(VolumeMin.Y + VolumeExtent.Y * 0.2f, VolumeMax.Y - VolumeExtent.Y * 0.2f);
		float SampleZ = VolumeMin.Z + VolumeHeight * RandStream.FRandRange(HeightMin, HeightMax);

		// Random horizontal direction
		float Angle = RandStream.FRandRange(0.0f, 2.0f * PI);
		FVector Direction(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);

		FVector TraceStart(SampleX, SampleY, SampleZ);
		FVector TraceEnd = TraceStart + Direction * VolumeExtent.GetMax() * 2.0f;

		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, TraceParams))
		{
			// Verify the hit is roughly vertical (wall-like surface)
			float NormalDotUp = FMath::Abs(FVector::DotProduct(Hit.ImpactNormal, FVector::UpVector));
			if (NormalDotUp < 0.3f) // Normal is mostly horizontal = wall
			{
				WallHits.Add({ Hit.ImpactPoint, Hit.ImpactNormal });
			}
		}
	}

	if (WallHits.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No wall surfaces found via traces. Ensure the volume contains geometry with walls."));
	}

	// Filter by min_spacing using a simple greedy approach
	TArray<FWallHit> FilteredHits;
	for (const FWallHit& Candidate : WallHits)
	{
		bool bTooClose = false;
		for (const FWallHit& Existing : FilteredHits)
		{
			if (FVector::Dist(Candidate.Location, Existing.Location) < MinSpacing)
			{
				bTooClose = true;
				break;
			}
		}
		if (!bTooClose)
		{
			FilteredHits.Add(Candidate);
			if (FilteredHits.Num() >= Count)
			{
				break;
			}
		}
	}

	// Spawn
	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Scatter On Walls")));

	int32 Placed = 0;
	int32 Rejected = 0;
	TArray<TSharedPtr<FJsonValue>> PlacedArr;
	TArray<FString> CollisionWarnings;

	// Collect spawned actors for ignore list
	TArray<AActor*> SpawnedActors;

	for (const FWallHit& WH : FilteredHits)
	{
		int32 MeshIdx = RandStream.RandRange(0, Meshes.Num() - 1);
		UStaticMesh* ChosenMesh = Meshes[MeshIdx];

		// Position: offset from wall along normal
		FVector SpawnLocation = WH.Location + WH.Normal * static_cast<float>(WallOffset);

		// Rotation: face the wall (align -X to wall normal, so the prop faces away from wall)
		// FRotationMatrix::MakeFromX with -Normal gives us "looking at the wall"
		FRotator SpawnRotation = FRotationMatrix::MakeFromX(-WH.Normal).Rotator();

		// Collision validation (unless mode is "none")
		if (!CollisionMode.Equals(TEXT("none"), ESearchCase::IgnoreCase))
		{
			FVector PropHalfExtent = ChosenMesh->GetBounds().BoxExtent * 0.9f;
			PropHalfExtent = PropHalfExtent.ComponentMax(FVector(1.0f));

			TArray<AActor*> IgnoreActors;
			IgnoreActors.Add(Volume);
			IgnoreActors.Append(SpawnedActors);

			bool bAllowPushOut = CollisionMode.Equals(TEXT("adjust"), ESearchCase::IgnoreCase);
			MonolithMeshUtils::FPropPlacementResult PlacementResult = MonolithMeshUtils::ValidatePropPlacement(
				World, SpawnLocation, SpawnRotation.Quaternion(), PropHalfExtent, IgnoreActors, bAllowPushOut);

			if (!PlacementResult.bValid)
			{
				if (CollisionMode.Equals(TEXT("reject"), ESearchCase::IgnoreCase) || CollisionMode.Equals(TEXT("adjust"), ESearchCase::IgnoreCase))
				{
					Rejected++;
					continue;
				}
				// "warn" mode: place anyway but record warning
				CollisionWarnings.Add(FString::Printf(TEXT("Wall prop at (%.0f, %.0f, %.0f): %s"),
					SpawnLocation.X, SpawnLocation.Y, SpawnLocation.Z, *PlacementResult.RejectReason));
			}
			else
			{
				SpawnLocation = PlacementResult.FinalLocation;
				for (const FString& W : PlacementResult.Warnings)
				{
					CollisionWarnings.Add(W);
				}
			}
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AStaticMeshActor* PropActor = World->SpawnActor<AStaticMeshActor>(SpawnLocation, SpawnRotation, SpawnParams);
		if (!PropActor) continue;

		SpawnedActors.Add(PropActor);
		PropActor->GetStaticMeshComponent()->SetStaticMesh(ChosenMesh);

		PropActor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Owner:%s"), *VolumeName)));
		PropActor->Tags.Add(FName(TEXT("Monolith.WallProp")));

		PropActor->SetFolderPath(FName(*FString::Printf(TEXT("Props/Walls/%s"), *VolumeName)));

		auto PlacedObj = MakeShared<FJsonObject>();
		PlacedObj->SetStringField(TEXT("actor"), PropActor->GetActorNameOrLabel());
		PlacedObj->SetStringField(TEXT("mesh"), ChosenMesh->GetPathName());
		PlacedObj->SetArrayField(TEXT("location"), VectorToJsonArray(SpawnLocation));
		PlacedObj->SetArrayField(TEXT("wall_normal"), VectorToJsonArray(WH.Normal));
		PlacedArr.Add(MakeShared<FJsonValueObject>(PlacedObj));

		Placed++;
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("placed"), Placed);
	Result->SetNumberField(TEXT("requested"), Count);
	Result->SetNumberField(TEXT("rejected"), Rejected);
	Result->SetNumberField(TEXT("wall_hits_found"), WallHits.Num());
	Result->SetStringField(TEXT("collision_mode"), CollisionMode);
	Result->SetNumberField(TEXT("seed"), Seed);
	Result->SetArrayField(TEXT("props"), PlacedArr);
	if (CollisionWarnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArr;
		for (const FString& W : CollisionWarnings)
		{
			WarningsArr.Add(MakeShared<FJsonValueString>(W));
		}
		Result->SetArrayField(TEXT("collision_warnings"), WarningsArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 8. scatter_on_ceiling
// ============================================================================

FMonolithActionResult FMonolithMeshContextPropActions::ScatterOnCeiling(const TSharedPtr<FJsonObject>& Params)
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

	double CountD = 8;
	Params->TryGetNumberField(TEXT("count"), CountD);
	int32 Count = FMath::Clamp(static_cast<int32>(CountD), 1, 100);

	double CeilingOffset = 2.0;
	Params->TryGetNumberField(TEXT("ceiling_offset"), CeilingOffset);

	double MinSpacing = 100.0;
	Params->TryGetNumberField(TEXT("min_spacing"), MinSpacing);

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

	FString CollisionMode = TEXT("warn");
	Params->TryGetStringField(TEXT("collision_mode"), CollisionMode);

	// Validate meshes
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

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Find volume
	FString FindError;
	AActor* Volume = MonolithMeshUtils::FindActorByName(VolumeName, FindError);
	if (!Volume)
	{
		return FMonolithActionResult::Error(FindError);
	}

	FVector VolumeOrigin, VolumeExtent;
	Volume->GetActorBounds(false, VolumeOrigin, VolumeExtent);
	FVector VolumeMin = VolumeOrigin - VolumeExtent;
	FVector VolumeMax = VolumeOrigin + VolumeExtent;

	FRandomStream RandStream(Seed);

	// Poisson disk sampling on XY plane
	float AreaWidth = VolumeExtent.X * 2.0f;
	float AreaHeight = VolumeExtent.Y * 2.0f;

	TArray<FVector2D> Samples = PoissonDiskSample2D(AreaWidth, AreaHeight, static_cast<float>(MinSpacing), Count, RandStream);

	// For each sample, trace upward to find ceiling
	struct FCeilingHit
	{
		FVector Location;
		FVector Normal;
	};
	TArray<FCeilingHit> CeilingHits;

	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(MonolithCeilingTrace), true);
	TraceParams.AddIgnoredActor(Volume);

	for (const FVector2D& Sample : Samples)
	{
		float WorldX = VolumeMin.X + Sample.X;
		float WorldY = VolumeMin.Y + Sample.Y;

		// Trace upward from mid-height
		FVector TraceStart(WorldX, WorldY, VolumeOrigin.Z);
		FVector TraceEnd(WorldX, WorldY, VolumeMax.Z + 100.0f);

		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, TraceParams))
		{
			// Verify the hit normal points roughly downward (ceiling surface)
			if (FVector::DotProduct(Hit.ImpactNormal, FVector::DownVector) > 0.5f)
			{
				CeilingHits.Add({ Hit.ImpactPoint, Hit.ImpactNormal });
			}
		}
	}

	if (CeilingHits.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No ceiling surfaces found via upward traces. Ensure the volume has geometry above."));
	}

	// Spawn
	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Scatter On Ceiling")));

	int32 Placed = 0;
	int32 Rejected = 0;
	TArray<TSharedPtr<FJsonValue>> PlacedArr;
	TArray<FString> CollisionWarnings;

	// Collect spawned actors for ignore list
	TArray<AActor*> SpawnedActors;

	for (const FCeilingHit& CH : CeilingHits)
	{
		if (Placed >= Count) break;

		int32 MeshIdx = RandStream.RandRange(0, Meshes.Num() - 1);
		UStaticMesh* ChosenMesh = Meshes[MeshIdx];

		// Position: offset below ceiling along its normal (which points downward)
		FVector SpawnLocation = CH.Location + CH.Normal * static_cast<float>(CeilingOffset);

		// Rotation: align prop to hang from ceiling
		// Props hanging from ceiling: flip 180 degrees around X (pitch) so they point downward
		FRotator SpawnRotation = FRotationMatrix::MakeFromZ(-FVector::UpVector).Rotator();

		// Add some random yaw
		SpawnRotation.Yaw = RandStream.FRandRange(0.0f, 360.0f);

		// Collision validation (unless mode is "none")
		if (!CollisionMode.Equals(TEXT("none"), ESearchCase::IgnoreCase))
		{
			FVector PropHalfExtent = ChosenMesh->GetBounds().BoxExtent * 0.9f;
			PropHalfExtent = PropHalfExtent.ComponentMax(FVector(1.0f));

			TArray<AActor*> IgnoreActors;
			IgnoreActors.Add(Volume);
			IgnoreActors.Append(SpawnedActors);

			bool bAllowPushOut = CollisionMode.Equals(TEXT("adjust"), ESearchCase::IgnoreCase);
			MonolithMeshUtils::FPropPlacementResult PlacementResult = MonolithMeshUtils::ValidatePropPlacement(
				World, SpawnLocation, SpawnRotation.Quaternion(), PropHalfExtent, IgnoreActors, bAllowPushOut);

			if (!PlacementResult.bValid)
			{
				if (CollisionMode.Equals(TEXT("reject"), ESearchCase::IgnoreCase) || CollisionMode.Equals(TEXT("adjust"), ESearchCase::IgnoreCase))
				{
					Rejected++;
					continue;
				}
				// "warn" mode: place anyway but record warning
				CollisionWarnings.Add(FString::Printf(TEXT("Ceiling prop at (%.0f, %.0f, %.0f): %s"),
					SpawnLocation.X, SpawnLocation.Y, SpawnLocation.Z, *PlacementResult.RejectReason));
			}
			else
			{
				SpawnLocation = PlacementResult.FinalLocation;
				for (const FString& W : PlacementResult.Warnings)
				{
					CollisionWarnings.Add(W);
				}
			}
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AStaticMeshActor* PropActor = World->SpawnActor<AStaticMeshActor>(SpawnLocation, SpawnRotation, SpawnParams);
		if (!PropActor) continue;

		SpawnedActors.Add(PropActor);
		PropActor->GetStaticMeshComponent()->SetStaticMesh(ChosenMesh);

		PropActor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Owner:%s"), *VolumeName)));
		PropActor->Tags.Add(FName(TEXT("Monolith.CeilingProp")));

		PropActor->SetFolderPath(FName(*FString::Printf(TEXT("Props/Ceiling/%s"), *VolumeName)));

		auto PlacedObj = MakeShared<FJsonObject>();
		PlacedObj->SetStringField(TEXT("actor"), PropActor->GetActorNameOrLabel());
		PlacedObj->SetStringField(TEXT("mesh"), ChosenMesh->GetPathName());
		PlacedObj->SetArrayField(TEXT("location"), VectorToJsonArray(SpawnLocation));
		PlacedArr.Add(MakeShared<FJsonValueObject>(PlacedObj));

		Placed++;
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("placed"), Placed);
	Result->SetNumberField(TEXT("requested"), Count);
	Result->SetNumberField(TEXT("rejected"), Rejected);
	Result->SetNumberField(TEXT("ceiling_hits_found"), CeilingHits.Num());
	Result->SetStringField(TEXT("collision_mode"), CollisionMode);
	Result->SetNumberField(TEXT("seed"), Seed);
	Result->SetArrayField(TEXT("props"), PlacedArr);
	if (CollisionWarnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArr;
		for (const FString& W : CollisionWarnings)
		{
			WarningsArr.Add(MakeShared<FJsonValueString>(W));
		}
		Result->SetArrayField(TEXT("collision_warnings"), WarningsArr);
	}

	return FMonolithActionResult::Success(Result);
}
