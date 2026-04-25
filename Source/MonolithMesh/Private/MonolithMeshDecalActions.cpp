#include "MonolithMeshDecalActions.h"
#include "MonolithMeshSceneActions.h"
#include "MonolithMeshBlockoutActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithMeshStorytellingPatterns.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"

#include "Engine/DecalActor.h"
#include "Components/DecalComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "CollisionQueryParams.h"
#include "WorldCollision.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Math/RandomStream.h"

// ============================================================================
// FScopedMeshTransaction
// ============================================================================

FMonolithMeshDecalActions::FScopedMeshTransaction::FScopedMeshTransaction(const FText& Description)
	: bOwnsTransaction(!FMonolithMeshSceneActions::bBatchTransactionActive)
{
	if (bOwnsTransaction && GEditor)
	{
		GEditor->BeginTransaction(Description);
	}
}

FMonolithMeshDecalActions::FScopedMeshTransaction::~FScopedMeshTransaction()
{
	if (bOwnsTransaction && GEditor)
	{
		GEditor->EndTransaction();
	}
}

void FMonolithMeshDecalActions::FScopedMeshTransaction::Cancel()
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

TArray<TSharedPtr<FJsonValue>> FMonolithMeshDecalActions::VectorToJsonArray(const FVector& V)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Add(MakeShared<FJsonValueNumber>(V.X));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
	return Arr;
}

bool FMonolithMeshDecalActions::ValidateDecalMaterial(const FString& MaterialPath, FString& OutError)
{
	UMaterialInterface* MatInterface = FMonolithAssetUtils::LoadAssetByPath<UMaterialInterface>(MaterialPath);
	if (!MatInterface)
	{
		OutError = FString::Printf(TEXT("Material not found: %s"), *MaterialPath);
		return false;
	}

	// Walk to the base material to check the domain
	UMaterial* BaseMat = MatInterface->GetMaterial();
	if (!BaseMat)
	{
		OutError = FString::Printf(TEXT("Could not resolve base material for: %s"), *MaterialPath);
		return false;
	}

	if (BaseMat->MaterialDomain != MD_DeferredDecal)
	{
		OutError = FString::Printf(
			TEXT("Material '%s' has domain '%s', expected 'DeferredDecal'. Only decal materials can be used with place_decals."),
			*MaterialPath,
			*UEnum::GetValueAsString(BaseMat->MaterialDomain));
		return false;
	}

	return true;
}

ADecalActor* FMonolithMeshDecalActions::SpawnAlignedDecal(
	UWorld* World,
	UMaterialInterface* Material,
	const FVector& Location,
	const FVector& DecalSize,
	float RandomYaw,
	const FCollisionQueryParams& TraceParams)
{
	// Trace downward to find surface. Start just above the provided location (50cm)
	// rather than far above (500cm) to avoid hitting ceilings or upper floors in
	// multi-story environments. The caller's Z should already be near the target surface.
	FVector TraceStart = Location + FVector(0, 0, 50.0f);
	FVector TraceEnd = Location - FVector(0, 0, 500.0f);

	FHitResult Hit;
	bool bHit = World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, TraceParams);

	FVector SpawnLocation = bHit ? Hit.ImpactPoint : Location;

	// Decal orientation: project along -ImpactNormal (into the surface)
	// FRotationMatrix::MakeFromX gives a rotation where X axis = the provided direction
	FVector ProjectionDir = bHit ? -Hit.ImpactNormal : FVector(0, 0, -1);
	FRotator SpawnRotation = FRotationMatrix::MakeFromX(ProjectionDir).Rotator();

	// Add random roll (spin around the projection axis)
	SpawnRotation.Roll += RandomYaw;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ADecalActor* DecalActor = World->SpawnActor<ADecalActor>(SpawnLocation, SpawnRotation, SpawnParams);
	if (!DecalActor)
	{
		return nullptr;
	}

	UDecalComponent* DecalComp = DecalActor->GetDecal();
	if (DecalComp)
	{
		DecalComp->SetDecalMaterial(Material);
		// DecalSize = FVector(Depth, Width, Height) — projection along local X
		DecalComp->DecalSize = DecalSize;
	}

	return DecalActor;
}

FVector FMonolithMeshDecalActions::CatmullRom(const FVector& P0, const FVector& P1, const FVector& P2, const FVector& P3, float t)
{
	// Standard Catmull-Rom: q(t) = 0.5 * ((2*P1) + (-P0+P2)*t + (2*P0-5*P1+4*P2-P3)*t^2 + (-P0+3*P1-3*P2+P3)*t^3)
	float t2 = t * t;
	float t3 = t2 * t;

	return 0.5f * (
		(2.0f * P1) +
		(-P0 + P2) * t +
		(2.0f * P0 - 5.0f * P1 + 4.0f * P2 - P3) * t2 +
		(-P0 + 3.0f * P1 - 3.0f * P2 + P3) * t3
	);
}

TArray<FVector> FMonolithMeshDecalActions::SampleSplinePath(const TArray<FVector>& PathPoints, float Spacing)
{
	TArray<FVector> Result;
	if (PathPoints.Num() < 2)
	{
		if (PathPoints.Num() == 1)
		{
			Result.Add(PathPoints[0]);
		}
		return Result;
	}

	// For Catmull-Rom we need P0,P1,P2,P3 for each segment P1->P2
	// We mirror endpoints: P0 = 2*P1 - P2 for the first, P3 = 2*PN - P(N-1) for the last
	float AccumulatedDist = 0.0f;
	float NextPlacementDist = 0.0f;

	// Add the first point
	Result.Add(PathPoints[0]);
	NextPlacementDist = Spacing;

	// Walk each segment with fine stepping
	const float StepSize = FMath::Min(Spacing * 0.1f, 5.0f); // 10% of spacing or 5cm, whichever smaller

	for (int32 Seg = 0; Seg < PathPoints.Num() - 1; ++Seg)
	{
		// Control points for this segment
		FVector P0 = (Seg > 0) ? PathPoints[Seg - 1] : (2.0f * PathPoints[0] - PathPoints[1]);
		FVector P1 = PathPoints[Seg];
		FVector P2 = PathPoints[Seg + 1];
		FVector P3 = (Seg + 2 < PathPoints.Num()) ? PathPoints[Seg + 2] : (2.0f * PathPoints.Last() - PathPoints[PathPoints.Num() - 2]);

		// Estimate segment length for t-stepping
		float SegLength = 0.0f;
		FVector PrevPt = P1;
		const int32 EstSteps = 20;
		for (int32 i = 1; i <= EstSteps; ++i)
		{
			float t = static_cast<float>(i) / static_cast<float>(EstSteps);
			FVector Pt = CatmullRom(P0, P1, P2, P3, t);
			SegLength += FVector::Dist(PrevPt, Pt);
			PrevPt = Pt;
		}

		if (SegLength < KINDA_SMALL_NUMBER) continue;

		// Walk the segment
		FVector WalkPrev = P1;
		float WalkDist = 0.0f;
		const int32 WalkSteps = FMath::Max(1, FMath::CeilToInt32(SegLength / StepSize));
		for (int32 i = 1; i <= WalkSteps; ++i)
		{
			float t = static_cast<float>(i) / static_cast<float>(WalkSteps);
			FVector WalkCur = CatmullRom(P0, P1, P2, P3, t);
			float StepDist = FVector::Dist(WalkPrev, WalkCur);
			AccumulatedDist += StepDist;

			while (AccumulatedDist >= NextPlacementDist)
			{
				// Interpolate the exact placement point
				float Overshoot = AccumulatedDist - NextPlacementDist;
				float Frac = (StepDist > KINDA_SMALL_NUMBER) ? (1.0f - Overshoot / StepDist) : 1.0f;
				FVector PlacePt = FMath::Lerp(WalkPrev, WalkCur, FMath::Clamp(Frac, 0.0f, 1.0f));
				Result.Add(PlacePt);
				NextPlacementDist += Spacing;
			}

			WalkPrev = WalkCur;
		}
	}

	return Result;
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshDecalActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("place_decals"),
		TEXT("Place decal actors aligned to surfaces. Provide explicit locations OR a region+count for Poisson-disk scattered placement. Validates material has DeferredDecal domain."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshDecalActions::PlaceDecals),
		FParamSchemaBuilder()
			.Required(TEXT("material"), TEXT("string"), TEXT("Asset path to a decal material (must have DeferredDecal domain)"))
			.Optional(TEXT("locations"), TEXT("array"), TEXT("Array of [x,y,z] world positions to place decals"))
			.Optional(TEXT("region"), TEXT("object"), TEXT("Bounding region {center: [x,y,z], extent: [x,y,z]} for Poisson-disk scatter"))
			.Optional(TEXT("count"), TEXT("number"), TEXT("Number of decals to scatter within region (requires region)"), TEXT("10"))
			.Optional(TEXT("size"), TEXT("array"), TEXT("Decal size [depth, width, height] in cm"), TEXT("[15, 80, 80]"))
			.Optional(TEXT("random_rotation"), TEXT("boolean"), TEXT("Randomize decal roll/spin"), TEXT("true"))
			.Optional(TEXT("min_spacing"), TEXT("number"), TEXT("Minimum distance between Poisson-scattered decals (cm)"), TEXT("60"))
			.Optional(TEXT("seed"), TEXT("number"), TEXT("Random seed for reproducible placement (0 = random)"), TEXT("0"))
			.Optional(TEXT("folder"), TEXT("string"), TEXT("Outliner folder for spawned decals"), TEXT("Decals"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("place_along_path"),
		TEXT("Place decals or props along a smooth path using Catmull-Rom interpolation. Built-in patterns: blood_drips (30-80cm spacing), footprints (60cm alternating L/R), drag_marks (10-20cm dense)."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshDecalActions::PlaceAlongPath),
		FParamSchemaBuilder()
			.Required(TEXT("path_points"), TEXT("array"), TEXT("Array of [x,y,z] waypoints defining the path (minimum 2)"))
			.Required(TEXT("asset_or_decal"), TEXT("string"), TEXT("Asset path — decal material (DeferredDecal domain) or static mesh"))
			.Optional(TEXT("spacing"), TEXT("number"), TEXT("Distance between placements in cm (overrides pattern default)"))
			.Optional(TEXT("pattern"), TEXT("string"), TEXT("Built-in pattern: blood_drips, footprints, drag_marks"))
			.Optional(TEXT("size"), TEXT("array"), TEXT("Decal size [depth, width, height] or prop scale [x,y,z]"))
			.Optional(TEXT("seed"), TEXT("number"), TEXT("Random seed (0 = random)"), TEXT("0"))
			.Optional(TEXT("folder"), TEXT("string"), TEXT("Outliner folder"), TEXT("PathDecals"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("analyze_prop_density"),
		TEXT("Analyze prop/actor density within a volume using a grid. Returns per-cell counts, identifies sparse/dense areas, and scores against a target density."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshDecalActions::AnalyzePropDensity),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Name of the blockout volume to analyze"))
			.Optional(TEXT("grid_size"), TEXT("number"), TEXT("Grid cell size in cm"), TEXT("200"))
			.Optional(TEXT("target_density"), TEXT("number"), TEXT("Target props per cell (for scoring)"), TEXT("3"))
			.Optional(TEXT("summary_only"), TEXT("boolean"), TEXT("If true, return only aggregate stats (reduces token count)"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("place_storytelling_scene"),
		TEXT("Place a parameterized horror storytelling scene. Patterns: violence (radial blood splatter), abandoned_in_haste (scattered items), dragged (linear trail), medical_emergency (triage scene), corruption (organic growth). Intensity 0-1 scales density and radius. Returns placed actor names for manual material assignment."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshDecalActions::PlaceStorytellingScene),
		FParamSchemaBuilder()
			.Required(TEXT("location"), TEXT("array"), TEXT("Scene center [x, y, z]"))
			.Required(TEXT("pattern"), TEXT("string"), TEXT("Scene pattern: violence, abandoned_in_haste, dragged, medical_emergency, corruption"))
			.Optional(TEXT("intensity"), TEXT("number"), TEXT("Scene intensity 0.0-1.0 (scales count, radius)"), TEXT("0.5"))
			.Optional(TEXT("direction"), TEXT("array"), TEXT("Scene direction [x, y, z] for directional patterns like dragged (default: +X)"))
			.Optional(TEXT("seed"), TEXT("number"), TEXT("Random seed (0 = random)"), TEXT("0"))
			.Optional(TEXT("folder"), TEXT("string"), TEXT("Outliner folder"), TEXT("Storytelling"))
			.Build());
}

// ============================================================================
// 1. place_decals
// ============================================================================

FMonolithActionResult FMonolithMeshDecalActions::PlaceDecals(const TSharedPtr<FJsonObject>& Params)
{
	FString MaterialPath;
	if (!Params->TryGetStringField(TEXT("material"), MaterialPath))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: material"));
	}

	// Validate decal material domain
	FString MatError;
	if (!ValidateDecalMaterial(MaterialPath, MatError))
	{
		return FMonolithActionResult::Error(MatError);
	}

	UMaterialInterface* Material = FMonolithAssetUtils::LoadAssetByPath<UMaterialInterface>(MaterialPath);
	// Already validated above, but guard anyway
	if (!Material)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load material: %s"), *MaterialPath));
	}

	// Parse decal size (Depth, Width, Height)
	FVector DecalSize(15.0f, 80.0f, 80.0f);
	const TArray<TSharedPtr<FJsonValue>>* SizeArr;
	if (Params->TryGetArrayField(TEXT("size"), SizeArr) && SizeArr->Num() >= 3)
	{
		DecalSize.X = static_cast<float>((*SizeArr)[0]->AsNumber());
		DecalSize.Y = static_cast<float>((*SizeArr)[1]->AsNumber());
		DecalSize.Z = static_cast<float>((*SizeArr)[2]->AsNumber());
	}

	bool bRandomRotation = true;
	Params->TryGetBoolField(TEXT("random_rotation"), bRandomRotation);

	int32 Seed = 0;
	double SeedD;
	if (Params->TryGetNumberField(TEXT("seed"), SeedD))
	{
		Seed = static_cast<int32>(SeedD);
	}
	if (Seed == 0) Seed = FMath::Rand();

	FString Folder = TEXT("Decals");
	Params->TryGetStringField(TEXT("folder"), Folder);

	FRandomStream RandStream(Seed);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Determine placement locations: explicit array OR region+count with Poisson disk
	TArray<FVector> Locations;

	const TArray<TSharedPtr<FJsonValue>>* LocationsArr;
	if (Params->TryGetArrayField(TEXT("locations"), LocationsArr) && LocationsArr->Num() > 0)
	{
		for (const auto& LocVal : *LocationsArr)
		{
			const TArray<TSharedPtr<FJsonValue>>* PtArr;
			if (LocVal->TryGetArray(PtArr) && PtArr->Num() >= 3)
			{
				Locations.Add(FVector(
					(*PtArr)[0]->AsNumber(),
					(*PtArr)[1]->AsNumber(),
					(*PtArr)[2]->AsNumber()));
			}
		}

		if (Locations.Num() == 0)
		{
			return FMonolithActionResult::Error(TEXT("locations array provided but no valid [x,y,z] entries found"));
		}
	}
	else
	{
		// Region-based Poisson disk scatter
		const TSharedPtr<FJsonObject>* RegionObj;
		if (!Params->TryGetObjectField(TEXT("region"), RegionObj))
		{
			return FMonolithActionResult::Error(TEXT("Must provide either 'locations' (array of [x,y,z]) or 'region' ({center, extent}) with 'count'"));
		}

		// Default-init to silence MSVC C4701 — ParseVector's || short-circuit + early return
		// makes use-after-unset unreachable, but MSVC's flow analyzer can't prove it.
		FVector Center = FVector::ZeroVector;
		FVector Extent = FVector::ZeroVector;
		if (!MonolithMeshUtils::ParseVector(*RegionObj, TEXT("center"), Center) ||
			!MonolithMeshUtils::ParseVector(*RegionObj, TEXT("extent"), Extent))
		{
			return FMonolithActionResult::Error(TEXT("region must have 'center' and 'extent' as [x,y,z] arrays"));
		}

		double CountD = 10.0;
		Params->TryGetNumberField(TEXT("count"), CountD);
		int32 Count = FMath::Clamp(static_cast<int32>(CountD), 1, 200);

		double MinSpacing = 60.0;
		Params->TryGetNumberField(TEXT("min_spacing"), MinSpacing);

		// 2D Poisson disk on XY plane (Bridson's algorithm)
		float AreaWidth = Extent.X * 2.0f;
		float AreaHeight = Extent.Y * 2.0f;
		float CellSize = static_cast<float>(MinSpacing) / FMath::Sqrt(2.0f);

		int32 GridW = FMath::Max(1, FMath::CeilToInt32(AreaWidth / CellSize));
		int32 GridH = FMath::Max(1, FMath::CeilToInt32(AreaHeight / CellSize));

		TArray<int32> Grid;
		Grid.SetNumUninitialized(GridW * GridH);
		for (int32 i = 0; i < Grid.Num(); ++i) Grid[i] = -1;

		struct FPSample { FVector2D Pos; };
		TArray<FPSample> Samples;
		TArray<int32> ActiveList;

		FVector2D FirstPos(
			RandStream.FRandRange(0.0f, AreaWidth),
			RandStream.FRandRange(0.0f, AreaHeight));

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

				if (Candidate.X < 0 || Candidate.X >= AreaWidth || Candidate.Y < 0 || Candidate.Y >= AreaHeight)
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

		if (Samples.Num() > Count)
		{
			Samples.SetNum(Count);
		}

		// Convert 2D samples to 3D world coords
		FVector RegionMin = Center - Extent;
		for (const FPSample& S : Samples)
		{
			Locations.Add(FVector(RegionMin.X + S.Pos.X, RegionMin.Y + S.Pos.Y, Center.Z));
		}
	}

	// Cap at 200 to prevent runaway
	if (Locations.Num() > 200)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Too many decal locations (%d). Maximum is 200."), Locations.Num()));
	}

	// Spawn decals
	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Place Decals")));

	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(MonolithDecalTrace), true);

	int32 Placed = 0;
	TArray<TSharedPtr<FJsonValue>> PlacedArr;

	for (const FVector& Loc : Locations)
	{
		float Yaw = bRandomRotation ? RandStream.FRandRange(0.0f, 360.0f) : 0.0f;

		ADecalActor* Decal = SpawnAlignedDecal(World, Material, Loc, DecalSize, Yaw, TraceParams);
		if (!Decal) continue;

		Decal->Tags.Add(FName(TEXT("Monolith.Decal")));
		Decal->SetFolderPath(FName(*Folder));

		auto PlacedObj = MakeShared<FJsonObject>();
		PlacedObj->SetStringField(TEXT("actor"), Decal->GetActorNameOrLabel());
		PlacedObj->SetArrayField(TEXT("location"), VectorToJsonArray(Decal->GetActorLocation()));
		PlacedArr.Add(MakeShared<FJsonValueObject>(PlacedObj));

		Placed++;
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("placed"), Placed);
	Result->SetNumberField(TEXT("requested"), Locations.Num());
	Result->SetStringField(TEXT("material"), MaterialPath);
	Result->SetNumberField(TEXT("seed"), Seed);
	Result->SetArrayField(TEXT("decals"), PlacedArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 2. place_along_path
// ============================================================================

FMonolithActionResult FMonolithMeshDecalActions::PlaceAlongPath(const TSharedPtr<FJsonObject>& Params)
{
	// Parse path points
	const TArray<TSharedPtr<FJsonValue>>* PathArr;
	if (!Params->TryGetArrayField(TEXT("path_points"), PathArr) || PathArr->Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("Missing or insufficient path_points (need at least 2 waypoints as [x,y,z] arrays)"));
	}

	TArray<FVector> PathPoints;
	for (const auto& PtVal : *PathArr)
	{
		const TArray<TSharedPtr<FJsonValue>>* PtArr;
		if (PtVal->TryGetArray(PtArr) && PtArr->Num() >= 3)
		{
			PathPoints.Add(FVector(
				(*PtArr)[0]->AsNumber(),
				(*PtArr)[1]->AsNumber(),
				(*PtArr)[2]->AsNumber()));
		}
	}

	if (PathPoints.Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("Need at least 2 valid [x,y,z] path points"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_or_decal"), AssetPath))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: asset_or_decal"));
	}

	// Determine if this is a decal material or a static mesh
	bool bIsDecal = false;
	UMaterialInterface* DecalMaterial = nullptr;
	UStaticMesh* PropMesh = nullptr;

	// Try loading as material first
	UMaterialInterface* MatTest = FMonolithAssetUtils::LoadAssetByPath<UMaterialInterface>(AssetPath);
	if (MatTest)
	{
		UMaterial* BaseMat = MatTest->GetMaterial();
		if (BaseMat && BaseMat->MaterialDomain == MD_DeferredDecal)
		{
			bIsDecal = true;
			DecalMaterial = MatTest;
		}
		else
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Material '%s' is not a deferred decal. For non-decal materials, use a static mesh path instead."), *AssetPath));
		}
	}
	else
	{
		// Try as static mesh
		FString MeshError;
		PropMesh = MonolithMeshUtils::LoadStaticMesh(AssetPath, MeshError);
		if (!PropMesh)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("asset_or_decal '%s' is neither a valid decal material nor a static mesh. %s"), *AssetPath, *MeshError));
		}
	}

	// Pattern presets
	FString Pattern;
	Params->TryGetStringField(TEXT("pattern"), Pattern);

	float Spacing = 0.0f;
	double SpacingD;
	if (Params->TryGetNumberField(TEXT("spacing"), SpacingD))
	{
		Spacing = static_cast<float>(SpacingD);
	}

	// Default size
	FVector Size = bIsDecal ? FVector(15.0f, 50.0f, 50.0f) : FVector(1.0f, 1.0f, 1.0f);
	const TArray<TSharedPtr<FJsonValue>>* SizeArr;
	if (Params->TryGetArrayField(TEXT("size"), SizeArr) && SizeArr->Num() >= 3)
	{
		Size.X = static_cast<float>((*SizeArr)[0]->AsNumber());
		Size.Y = static_cast<float>((*SizeArr)[1]->AsNumber());
		Size.Z = static_cast<float>((*SizeArr)[2]->AsNumber());
	}

	int32 Seed = 0;
	double SeedD;
	if (Params->TryGetNumberField(TEXT("seed"), SeedD))
	{
		Seed = static_cast<int32>(SeedD);
	}
	if (Seed == 0) Seed = FMath::Rand();

	FString Folder = TEXT("PathDecals");
	Params->TryGetStringField(TEXT("folder"), Folder);

	FRandomStream RandStream(Seed);

	// Apply pattern presets (spacing, size, variation)
	float SpacingVariance = 0.0f;
	float LateralOffset = 0.0f;
	bool bAlternateLeftRight = false;
	float SizeVariance = 0.1f;
	float RotationVariance = 360.0f;

	if (!Pattern.IsEmpty())
	{
		if (Pattern.Equals(TEXT("blood_drips"), ESearchCase::IgnoreCase))
		{
			if (Spacing <= 0.0f) Spacing = 55.0f; // midpoint of 30-80
			SpacingVariance = 25.0f; // +/-25 = 30-80 range
			if (bIsDecal && !Params->HasField(TEXT("size")))
			{
				Size = FVector(8.0f, 20.0f, 20.0f);
			}
			SizeVariance = 0.4f;
			RotationVariance = 360.0f;
		}
		else if (Pattern.Equals(TEXT("footprints"), ESearchCase::IgnoreCase))
		{
			if (Spacing <= 0.0f) Spacing = 60.0f;
			SpacingVariance = 5.0f; // slight variance
			bAlternateLeftRight = true;
			LateralOffset = 15.0f; // 15cm left/right alternation
			if (bIsDecal && !Params->HasField(TEXT("size")))
			{
				Size = FVector(8.0f, 18.0f, 30.0f);
			}
			SizeVariance = 0.05f; // footprints are consistent
			RotationVariance = 10.0f; // slight toe-in/toe-out
		}
		else if (Pattern.Equals(TEXT("drag_marks"), ESearchCase::IgnoreCase))
		{
			if (Spacing <= 0.0f) Spacing = 15.0f; // midpoint of 10-20
			SpacingVariance = 5.0f;
			if (bIsDecal && !Params->HasField(TEXT("size")))
			{
				Size = FVector(10.0f, 40.0f, 15.0f); // elongated along path
			}
			SizeVariance = 0.2f;
			RotationVariance = 15.0f; // roughly follow the path
		}
		else
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Unknown pattern '%s'. Valid patterns: blood_drips, footprints, drag_marks"), *Pattern));
		}
	}

	if (Spacing <= 0.0f) Spacing = 50.0f; // fallback default

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Sample the spline path
	TArray<FVector> SampledPoints = SampleSplinePath(PathPoints, Spacing);

	// Cap at 500 placements
	if (SampledPoints.Num() > 500)
	{
		SampledPoints.SetNum(500);
	}

	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Place Along Path")));

	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(MonolithPathTrace), true);

	int32 Placed = 0;
	TArray<TSharedPtr<FJsonValue>> PlacedArr;

	for (int32 i = 0; i < SampledPoints.Num(); ++i)
	{
		FVector Pt = SampledPoints[i];

		// Apply spacing variance
		if (SpacingVariance > 0.0f && i > 0)
		{
			// Jitter along path direction
			FVector Dir = (i + 1 < SampledPoints.Num())
				? (SampledPoints[i + 1] - Pt).GetSafeNormal()
				: (Pt - SampledPoints[i - 1]).GetSafeNormal();
			Pt += Dir * RandStream.FRandRange(-SpacingVariance, SpacingVariance);
		}

		// Apply lateral offset (for footprints alternation)
		if (bAlternateLeftRight && LateralOffset > 0.0f)
		{
			FVector Dir = (i + 1 < SampledPoints.Num())
				? (SampledPoints[FMath::Min(i + 1, SampledPoints.Num() - 1)] - Pt).GetSafeNormal()
				: (Pt - SampledPoints[FMath::Max(0, i - 1)]).GetSafeNormal();
			FVector Right = FVector::CrossProduct(Dir, FVector::UpVector).GetSafeNormal();
			float Side = (i % 2 == 0) ? 1.0f : -1.0f;
			Pt += Right * LateralOffset * Side;
		}

		// Size variance
		FVector PlaceSize = Size;
		if (SizeVariance > 0.0f)
		{
			float ScaleMult = 1.0f + RandStream.FRandRange(-SizeVariance, SizeVariance);
			PlaceSize *= FMath::Max(0.1f, ScaleMult);
		}

		float RotYaw = RandStream.FRandRange(-RotationVariance * 0.5f, RotationVariance * 0.5f);

		// For footprints and drag_marks, orient along path direction
		if (!Pattern.IsEmpty() && !Pattern.Equals(TEXT("blood_drips"), ESearchCase::IgnoreCase))
		{
			FVector Dir = (i + 1 < SampledPoints.Num())
				? (SampledPoints[FMath::Min(i + 1, SampledPoints.Num() - 1)] - Pt).GetSafeNormal()
				: (Pt - SampledPoints[FMath::Max(0, i - 1)]).GetSafeNormal();
			float PathYaw = FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X));
			RotYaw += PathYaw;
		}

		FString ActorName;

		if (bIsDecal)
		{
			ADecalActor* Decal = SpawnAlignedDecal(World, DecalMaterial, Pt, PlaceSize, RotYaw, TraceParams);
			if (!Decal) continue;

			Decal->Tags.Add(FName(TEXT("Monolith.PathDecal")));
			if (!Pattern.IsEmpty())
			{
				Decal->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Pattern:%s"), *Pattern)));
			}
			Decal->SetFolderPath(FName(*Folder));
			ActorName = Decal->GetActorNameOrLabel();
		}
		else
		{
			// Spawn static mesh prop — trace from near the path point, not far above
			FVector TraceStart = Pt + FVector(0, 0, 50.0f);
			FVector TraceEnd = Pt - FVector(0, 0, 500.0f);
			FHitResult Hit;
			bool bHit = World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, TraceParams);

			FVector SpawnLoc = bHit ? Hit.ImpactPoint : Pt;
			FRotator SpawnRot(0, RotYaw, 0);

			if (bHit)
			{
				// Align to surface
				SpawnRot = FRotationMatrix::MakeFromZ(Hit.ImpactNormal).Rotator();
				SpawnRot.Yaw = RotYaw;
			}

			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

			AStaticMeshActor* PropActor = World->SpawnActor<AStaticMeshActor>(SpawnLoc, SpawnRot, SpawnParams);
			if (!PropActor) continue;

			PropActor->GetStaticMeshComponent()->SetStaticMesh(PropMesh);
			PropActor->SetActorScale3D(PlaceSize);
			PropActor->Tags.Add(FName(TEXT("Monolith.PathProp")));
			if (!Pattern.IsEmpty())
			{
				PropActor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Pattern:%s"), *Pattern)));
			}
			PropActor->SetFolderPath(FName(*Folder));
			ActorName = PropActor->GetActorNameOrLabel();
		}

		auto PlacedObj = MakeShared<FJsonObject>();
		PlacedObj->SetStringField(TEXT("actor"), ActorName);
		PlacedObj->SetNumberField(TEXT("index"), i);
		PlacedObj->SetArrayField(TEXT("location"), VectorToJsonArray(Pt));
		PlacedArr.Add(MakeShared<FJsonValueObject>(PlacedObj));

		Placed++;
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("placed"), Placed);
	Result->SetNumberField(TEXT("path_points"), PathPoints.Num());
	Result->SetStringField(TEXT("asset"), AssetPath);
	if (!Pattern.IsEmpty()) Result->SetStringField(TEXT("pattern"), Pattern);
	Result->SetNumberField(TEXT("spacing"), Spacing);
	Result->SetNumberField(TEXT("seed"), Seed);
	Result->SetArrayField(TEXT("placements"), PlacedArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 3. analyze_prop_density
// ============================================================================

FMonolithActionResult FMonolithMeshDecalActions::AnalyzePropDensity(const TSharedPtr<FJsonObject>& Params)
{
	FString VolumeName;
	if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: volume_name"));
	}

	double GridSizeD = 200.0;
	Params->TryGetNumberField(TEXT("grid_size"), GridSizeD);
	float GridSize = FMath::Max(50.0f, static_cast<float>(GridSizeD));

	double TargetDensity = 3.0;
	Params->TryGetNumberField(TEXT("target_density"), TargetDensity);

	bool bSummaryOnly = false;
	Params->TryGetBoolField(TEXT("summary_only"), bSummaryOnly);

	// Find the volume using blockout's helper pattern
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Find volume actor by name
	AActor* Volume = nullptr;
	FString Error;

	// Search by name/label
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor->GetActorNameOrLabel() == VolumeName ||
			Actor->GetActorLabel() == VolumeName ||
			Actor->GetFName().ToString() == VolumeName)
		{
			Volume = Actor;
			break;
		}
	}

	if (!Volume)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Volume not found: %s"), *VolumeName));
	}

	FVector VolumeOrigin, VolumeExtent;
	Volume->GetActorBounds(false, VolumeOrigin, VolumeExtent);
	FVector VolumeMin = VolumeOrigin - VolumeExtent;
	FVector VolumeMax = VolumeOrigin + VolumeExtent;
	FVector VolumeSize = VolumeExtent * 2.0;

	// Compute grid dimensions
	int32 CellsX = FMath::Max(1, FMath::CeilToInt32(VolumeSize.X / GridSize));
	int32 CellsY = FMath::Max(1, FMath::CeilToInt32(VolumeSize.Y / GridSize));

	if (CellsX * CellsY > 400)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Grid would create %d cells (max 400). Increase grid_size or use a smaller volume."),
			CellsX * CellsY));
	}

	// Initialize grid counts
	TArray<int32> CellCounts;
	CellCounts.SetNumZeroed(CellsX * CellsY);

	// Count actors in each cell (exclude the volume itself and Monolith infrastructure)
	int32 TotalActors = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor == Volume) continue;
		if (Actor->IsHidden()) continue;

		FVector ActorLoc = Actor->GetActorLocation();

		// Must be inside volume bounds
		if (ActorLoc.X < VolumeMin.X || ActorLoc.X > VolumeMax.X ||
			ActorLoc.Y < VolumeMin.Y || ActorLoc.Y > VolumeMax.Y ||
			ActorLoc.Z < VolumeMin.Z || ActorLoc.Z > VolumeMax.Z)
		{
			continue;
		}

		// Skip non-visible actors (cameras, lights, etc.) — only count things with mesh components
		if (!Actor->FindComponentByClass<UStaticMeshComponent>() &&
			!Actor->FindComponentByClass<UDecalComponent>())
		{
			continue;
		}

		int32 CX = FMath::Clamp(FMath::FloorToInt32((ActorLoc.X - VolumeMin.X) / GridSize), 0, CellsX - 1);
		int32 CY = FMath::Clamp(FMath::FloorToInt32((ActorLoc.Y - VolumeMin.Y) / GridSize), 0, CellsY - 1);

		CellCounts[CY * CellsX + CX]++;
		TotalActors++;
	}

	// Analyze results
	int32 EmptyCells = 0;
	int32 UnderfilledCells = 0;
	int32 OverfilledCells = 0;
	int32 MatchingCells = 0;
	int32 MinCount = INT_MAX;
	int32 MaxCount = 0;
	float TotalCount = 0.0f;

	TArray<TSharedPtr<FJsonValue>> UnderfilledArr;
	TArray<TSharedPtr<FJsonValue>> OverfilledArr;
	TArray<TSharedPtr<FJsonValue>> CellsArr;

	for (int32 y = 0; y < CellsY; ++y)
	{
		for (int32 x = 0; x < CellsX; ++x)
		{
			int32 Count = CellCounts[y * CellsX + x];
			MinCount = FMath::Min(MinCount, Count);
			MaxCount = FMath::Max(MaxCount, Count);
			TotalCount += Count;

			if (Count == 0) EmptyCells++;
			else if (Count < TargetDensity * 0.5) UnderfilledCells++;
			else if (Count > TargetDensity * 1.5) OverfilledCells++;
			else MatchingCells++;

			if (!bSummaryOnly)
			{
				FVector CellCenter(
					VolumeMin.X + (x + 0.5f) * GridSize,
					VolumeMin.Y + (y + 0.5f) * GridSize,
					VolumeOrigin.Z);

				auto CellObj = MakeShared<FJsonObject>();
				CellObj->SetNumberField(TEXT("x"), x);
				CellObj->SetNumberField(TEXT("y"), y);
				CellObj->SetNumberField(TEXT("count"), Count);
				CellObj->SetArrayField(TEXT("center"), VectorToJsonArray(CellCenter));

				FString Status;
				if (Count == 0) Status = TEXT("empty");
				else if (Count < TargetDensity * 0.5) Status = TEXT("underfilled");
				else if (Count > TargetDensity * 1.5) Status = TEXT("overfilled");
				else Status = TEXT("good");
				CellObj->SetStringField(TEXT("status"), Status);

				CellsArr.Add(MakeShared<FJsonValueObject>(CellObj));

				if (Count < TargetDensity * 0.5 && Count >= 0)
				{
					auto UFObj = MakeShared<FJsonObject>();
					UFObj->SetNumberField(TEXT("x"), x);
					UFObj->SetNumberField(TEXT("y"), y);
					UFObj->SetNumberField(TEXT("count"), Count);
					UFObj->SetArrayField(TEXT("center"), VectorToJsonArray(CellCenter));
					UnderfilledArr.Add(MakeShared<FJsonValueObject>(UFObj));
				}

				if (Count > TargetDensity * 1.5)
				{
					auto OFObj = MakeShared<FJsonObject>();
					OFObj->SetNumberField(TEXT("x"), x);
					OFObj->SetNumberField(TEXT("y"), y);
					OFObj->SetNumberField(TEXT("count"), Count);
					OFObj->SetArrayField(TEXT("center"), VectorToJsonArray(CellCenter));
					OverfilledArr.Add(MakeShared<FJsonValueObject>(OFObj));
				}
			}
		}
	}

	int32 TotalCells = CellsX * CellsY;
	float AverageDensity = TotalCells > 0 ? TotalCount / TotalCells : 0.0f;

	// Score: percentage of cells within acceptable range (0.5x - 1.5x target)
	float Score = TotalCells > 0 ? static_cast<float>(MatchingCells) / static_cast<float>(TotalCells) : 0.0f;

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("volume"), VolumeName);
	Result->SetNumberField(TEXT("grid_size"), GridSize);
	Result->SetNumberField(TEXT("target_density"), TargetDensity);

	auto GridObj = MakeShared<FJsonObject>();
	GridObj->SetNumberField(TEXT("cells_x"), CellsX);
	GridObj->SetNumberField(TEXT("cells_y"), CellsY);
	GridObj->SetNumberField(TEXT("total_cells"), TotalCells);
	Result->SetObjectField(TEXT("grid"), GridObj);

	auto StatsObj = MakeShared<FJsonObject>();
	StatsObj->SetNumberField(TEXT("total_actors"), TotalActors);
	StatsObj->SetNumberField(TEXT("average_density"), FMath::RoundToFloat(AverageDensity * 100.0f) / 100.0f);
	StatsObj->SetNumberField(TEXT("min_count"), MinCount == INT_MAX ? 0 : MinCount);
	StatsObj->SetNumberField(TEXT("max_count"), MaxCount);
	StatsObj->SetNumberField(TEXT("empty_cells"), EmptyCells);
	StatsObj->SetNumberField(TEXT("underfilled_cells"), UnderfilledCells);
	StatsObj->SetNumberField(TEXT("overfilled_cells"), OverfilledCells);
	StatsObj->SetNumberField(TEXT("matching_cells"), MatchingCells);
	StatsObj->SetNumberField(TEXT("score"), FMath::RoundToFloat(Score * 1000.0f) / 1000.0f);
	Result->SetObjectField(TEXT("stats"), StatsObj);

	if (!bSummaryOnly)
	{
		Result->SetArrayField(TEXT("cells"), CellsArr);
		Result->SetArrayField(TEXT("underfilled"), UnderfilledArr);
		Result->SetArrayField(TEXT("overfilled"), OverfilledArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 4. place_storytelling_scene
// ============================================================================

FMonolithActionResult FMonolithMeshDecalActions::PlaceStorytellingScene(const TSharedPtr<FJsonObject>& Params)
{
	FVector Location;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: location (array of 3 numbers)"));
	}

	FString PatternName;
	if (!Params->TryGetStringField(TEXT("pattern"), PatternName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: pattern"));
	}

	const FStorytellingPattern* Pattern = StorytellingPatterns::GetPattern(PatternName);
	if (!Pattern)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown pattern '%s'. Valid patterns: %s"),
			*PatternName, *StorytellingPatterns::GetPatternNames()));
	}

	double IntensityD = 0.5;
	Params->TryGetNumberField(TEXT("intensity"), IntensityD);
	float Intensity = FMath::Clamp(static_cast<float>(IntensityD), 0.0f, 1.0f);

	FVector Direction(1, 0, 0);
	MonolithMeshUtils::ParseVector(Params, TEXT("direction"), Direction);
	Direction = Direction.GetSafeNormal();
	if (Direction.IsNearlyZero()) Direction = FVector(1, 0, 0);

	int32 Seed = 0;
	double SeedD;
	if (Params->TryGetNumberField(TEXT("seed"), SeedD))
	{
		Seed = static_cast<int32>(SeedD);
	}
	if (Seed == 0) Seed = FMath::Rand();

	FString Folder = TEXT("Storytelling");
	Params->TryGetStringField(TEXT("folder"), Folder);
	Folder = FString::Printf(TEXT("%s/%s"), *Folder, *PatternName);

	FRandomStream RandStream(Seed);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FScopedMeshTransaction Transaction(FText::FromString(TEXT("Monolith: Place Storytelling Scene")));

	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(MonolithStorytellingTrace), true);

	// Build a rotation matrix from the scene direction (for directional patterns like "dragged")
	FRotator DirRotation = Direction.Rotation();

	int32 TotalPlaced = 0;
	TArray<TSharedPtr<FJsonValue>> PlacedArr;

	// Process each element in the pattern
	for (const FStorytellingElement& Elem : Pattern->Elements)
	{
		// Compute count based on intensity
		int32 Count = FMath::RoundToInt32(FMath::Lerp(
			static_cast<float>(Elem.CountMin),
			static_cast<float>(Elem.CountMax),
			Intensity));
		Count = FMath::Max(1, Count);

		for (int32 i = 0; i < Count; ++i)
		{
			FVector Offset;

			if (Elem.bRadial)
			{
				// Radial placement: random angle, distance scaled by intensity
				float Angle = RandStream.FRandRange(0.0f, 2.0f * PI);
				float RadMin = Elem.RadialMin * (0.5f + 0.5f * Intensity);
				float RadMax = Elem.RadialMax * (0.5f + 0.5f * Intensity);
				float Radius = RandStream.FRandRange(RadMin, RadMax);

				Offset = FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, 0.0f);

				// Rotate offset by scene direction
				Offset = DirRotation.RotateVector(Offset);
			}
			else
			{
				// Fixed offset (with small jitter)
				Offset = Elem.RelativeOffset;
				Offset.X += RandStream.FRandRange(-20.0f, 20.0f);
				Offset.Y += RandStream.FRandRange(-20.0f, 20.0f);

				// Rotate by scene direction
				Offset = DirRotation.RotateVector(Offset);
			}

			// Add Z offset from element definition (only for radial — fixed offsets already include it)
			if (Elem.bRadial)
			{
				Offset.Z += Elem.RelativeOffset.Z;
			}

			FVector PlaceLocation = Location + Offset;

			// Size with variance
			FVector PlaceSize = Elem.Size;
			float ScaleMult = 1.0f + RandStream.FRandRange(-Elem.ScaleVariance, Elem.ScaleVariance);
			// Also scale by intensity (bigger at higher intensity)
			ScaleMult *= (0.7f + 0.6f * Intensity);
			PlaceSize *= FMath::Max(0.2f, ScaleMult);

			// Rotation
			float RotYaw = RandStream.FRandRange(-Elem.RotationVariance * 0.5f, Elem.RotationVariance * 0.5f);

			FVector SpawnLoc;
			FVector ProjectionDir;

			if (Elem.bWallElement)
			{
				// Wall elements: trace horizontally from scene center outward to find a wall
				FVector HorizDir = FVector(PlaceLocation.X - Location.X, PlaceLocation.Y - Location.Y, 0.0f);
				if (HorizDir.IsNearlyZero())
				{
					HorizDir = FVector(1, 0, 0); // fallback direction
				}
				HorizDir.Normalize();

				// Trace from center outward, at the desired wall height
				float WallTraceZ = Location.Z + Elem.RelativeOffset.Z;
				FVector WallTraceStart = FVector(Location.X, Location.Y, WallTraceZ);
				FVector WallTraceEnd = WallTraceStart + HorizDir * 1000.0f;

				FHitResult WallHit;
				bool bWallHit = World->LineTraceSingleByChannel(WallHit, WallTraceStart, WallTraceEnd, ECC_Visibility, TraceParams);

				if (bWallHit)
				{
					// Place on the wall surface, pulled back slightly to avoid z-fighting
					SpawnLoc = WallHit.ImpactPoint - HorizDir * 0.5f;
					ProjectionDir = -WallHit.ImpactNormal;
				}
				else
				{
					// No wall found — place at computed position, project into the horizontal direction
					SpawnLoc = PlaceLocation;
					ProjectionDir = HorizDir;
				}
			}
			else
			{
				// Ground elements: trace downward from near the known floor level (Location.Z)
				// Starting from Location.Z + 50 avoids hitting ceilings/upper floors that a +500 offset would hit
				FVector TraceStart = FVector(PlaceLocation.X, PlaceLocation.Y, Location.Z + 50.0f);
				FVector TraceEnd = FVector(PlaceLocation.X, PlaceLocation.Y, Location.Z - 500.0f);

				FHitResult Hit;
				bool bHit = World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, TraceParams);

				SpawnLoc = bHit ? Hit.ImpactPoint : FVector(PlaceLocation.X, PlaceLocation.Y, Location.Z);
				ProjectionDir = bHit ? -Hit.ImpactNormal : FVector(0, 0, -1);
			}

			// Orientation: project along the determined direction
			FRotator SpawnRot = FRotationMatrix::MakeFromX(ProjectionDir).Rotator();
			SpawnRot.Roll += RotYaw;

			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

			ADecalActor* DecalActor = World->SpawnActor<ADecalActor>(SpawnLoc, SpawnRot, SpawnParams);
			if (!DecalActor) continue;

			UDecalComponent* DecalComp = DecalActor->GetDecal();
			if (DecalComp)
			{
				DecalComp->DecalSize = PlaceSize;
			}

			// Tag for identification and later material assignment
			DecalActor->Tags.Add(FName(TEXT("Monolith.StorytellingDecal")));
			DecalActor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Pattern:%s"), *PatternName)));
			DecalActor->Tags.Add(FName(*FString::Printf(TEXT("Monolith.Element:%s"), *Elem.Label)));
			DecalActor->SetActorLabel(FString::Printf(TEXT("%s_%s_%d"), *PatternName, *Elem.Label, i));
			DecalActor->SetFolderPath(FName(*Folder));

			auto PlacedObj = MakeShared<FJsonObject>();
			PlacedObj->SetStringField(TEXT("actor"), DecalActor->GetActorNameOrLabel());
			PlacedObj->SetStringField(TEXT("element"), Elem.Label);
			PlacedObj->SetStringField(TEXT("type"), Elem.Type);
			PlacedObj->SetArrayField(TEXT("location"), VectorToJsonArray(SpawnLoc));
			PlacedObj->SetArrayField(TEXT("size"), VectorToJsonArray(PlaceSize));
			PlacedArr.Add(MakeShared<FJsonValueObject>(PlacedObj));

			TotalPlaced++;
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("pattern"), PatternName);
	Result->SetStringField(TEXT("description"), Pattern->Description);
	Result->SetNumberField(TEXT("intensity"), Intensity);
	Result->SetNumberField(TEXT("placed"), TotalPlaced);
	Result->SetNumberField(TEXT("seed"), Seed);
	Result->SetStringField(TEXT("note"), TEXT("Decals spawned without materials. Use mesh_query set_actor_properties or material assignment to apply decal materials to each element by label."));
	Result->SetArrayField(TEXT("elements"), PlacedArr);

	return FMonolithActionResult::Success(Result);
}
