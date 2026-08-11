#include "MonolithMeshSpatialActions.h"
#include "StaticMeshResources.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "MonolithMeshUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/BlockingVolume.h"
#include "Engine/Light.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Volume.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/BrushComponent.h"
#include "Components/LightComponent.h"
#include "CollisionQueryParams.h"
#include "WorldCollision.h"
#include "Engine/OverlapResult.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "AI/Navigation/NavigationTypes.h"

// ============================================================================
// Helpers
// ============================================================================

ECollisionChannel FMonolithMeshSpatialActions::ParseCollisionChannel(const FString& ChannelName, bool& bSuccess)
{
	bSuccess = true;

	if (ChannelName.Equals(TEXT("Visibility"), ESearchCase::IgnoreCase))    return ECC_Visibility;
	if (ChannelName.Equals(TEXT("Camera"), ESearchCase::IgnoreCase))         return ECC_Camera;
	if (ChannelName.Equals(TEXT("WorldStatic"), ESearchCase::IgnoreCase))    return ECC_WorldStatic;
	if (ChannelName.Equals(TEXT("WorldDynamic"), ESearchCase::IgnoreCase))   return ECC_WorldDynamic;
	if (ChannelName.Equals(TEXT("Pawn"), ESearchCase::IgnoreCase))           return ECC_Pawn;
	if (ChannelName.Equals(TEXT("PhysicsBody"), ESearchCase::IgnoreCase))    return ECC_PhysicsBody;
	if (ChannelName.Equals(TEXT("Vehicle"), ESearchCase::IgnoreCase))        return ECC_Vehicle;
	if (ChannelName.Equals(TEXT("Destructible"), ESearchCase::IgnoreCase))   return ECC_Destructible;

	// Try ECC_GameTraceChannel1..18
	if (ChannelName.StartsWith(TEXT("GameTraceChannel"), ESearchCase::IgnoreCase))
	{
		FString NumStr = ChannelName.Mid(16);
		if (NumStr.IsNumeric())
		{
			int32 Num = FCString::Atoi(*NumStr);
			if (Num >= 1 && Num <= 18)
			{
				return static_cast<ECollisionChannel>(ECC_GameTraceChannel1 + (Num - 1));
			}
		}
	}

	bSuccess = false;
	return ECC_Visibility;
}

TArray<TSharedPtr<FJsonValue>> FMonolithMeshSpatialActions::VectorToJsonArray(const FVector& V)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Add(MakeShared<FJsonValueNumber>(V.X));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
	return Arr;
}

TSharedPtr<FJsonObject> FMonolithMeshSpatialActions::HitResultToJson(const FHitResult& Hit)
{
	auto Obj = MakeShared<FJsonObject>();

	AActor* HitActor = Hit.GetActor();
	Obj->SetStringField(TEXT("actor"), HitActor ? HitActor->GetActorNameOrLabel() : TEXT("None"));

	UPrimitiveComponent* HitComp = Hit.GetComponent();
	Obj->SetStringField(TEXT("component"), HitComp ? HitComp->GetFName().ToString() : TEXT("None"));

	Obj->SetArrayField(TEXT("location"), VectorToJsonArray(Hit.Location));
	Obj->SetArrayField(TEXT("normal"), VectorToJsonArray(Hit.Normal));
	Obj->SetNumberField(TEXT("distance"), Hit.Distance);

	if (Hit.PhysMaterial.IsValid())
	{
		Obj->SetStringField(TEXT("phys_material"), Hit.PhysMaterial->GetName());
	}
	else
	{
		Obj->SetStringField(TEXT("phys_material"), TEXT("None"));
	}

	return Obj;
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshSpatialActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 1. query_raycast
	Registry.RegisterAction(TEXT("mesh"), TEXT("query_raycast"),
		TEXT("Fire a single raycast in the editor world. Returns hit data including actor, location, normal, distance, and physical material."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialActions::QueryRaycast),
		FParamSchemaBuilder()
			.Required(TEXT("start"), TEXT("array"), TEXT("Start position [x, y, z]"))
			.Required(TEXT("end"), TEXT("array"), TEXT("End position [x, y, z]"))
			.Optional(TEXT("channel"), TEXT("string"), TEXT("Collision channel name (Visibility, Camera, WorldStatic, WorldDynamic, Pawn, PhysicsBody, Vehicle, Destructible, GameTraceChannel1-18)"), TEXT("Visibility"))
			.Optional(TEXT("ignore_actors"), TEXT("array"), TEXT("Array of actor names to ignore"))
			.Build());

	// 2. query_multi_raycast
	Registry.RegisterAction(TEXT("mesh"), TEXT("query_multi_raycast"),
		TEXT("Fire a multi-hit raycast. Returns all hits sorted by distance."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialActions::QueryMultiRaycast),
		FParamSchemaBuilder()
			.Required(TEXT("start"), TEXT("array"), TEXT("Start position [x, y, z]"))
			.Required(TEXT("end"), TEXT("array"), TEXT("End position [x, y, z]"))
			.Optional(TEXT("channel"), TEXT("string"), TEXT("Collision channel name"), TEXT("Visibility"))
			.Optional(TEXT("max_hits"), TEXT("integer"), TEXT("Maximum number of hits to return"), TEXT("10"))
			.Build());

	// 3. query_radial_sweep
	Registry.RegisterAction(TEXT("mesh"), TEXT("query_radial_sweep"),
		TEXT("Fire rays in a radial pattern from origin. Returns summary by compass direction. Hard cap: ray_count * vertical_angles <= 512."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialActions::QueryRadialSweep),
		FParamSchemaBuilder()
			.Required(TEXT("origin"), TEXT("array"), TEXT("Origin position [x, y, z]"))
			.Optional(TEXT("radius"), TEXT("number"), TEXT("Ray length"), TEXT("1000"))
			.Optional(TEXT("ray_count"), TEXT("integer"), TEXT("Number of azimuthal rays (max 72)"), TEXT("36"))
			.Optional(TEXT("vertical_angles"), TEXT("integer"), TEXT("Number of vertical elevation layers (max 8)"), TEXT("3"))
			.Optional(TEXT("channel"), TEXT("string"), TEXT("Collision channel name"), TEXT("Visibility"))
			.Build());

	// 4. query_overlap
	Registry.RegisterAction(TEXT("mesh"), TEXT("query_overlap"),
		TEXT("Perform a shape overlap test (box, sphere, or capsule) at a location. Returns overlapping actors."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialActions::QueryOverlap),
		FParamSchemaBuilder()
			.Required(TEXT("location"), TEXT("array"), TEXT("Center position [x, y, z]"))
			.Required(TEXT("shape"), TEXT("string"), TEXT("Shape type: box, sphere, or capsule"))
			.Required(TEXT("extent"), TEXT("any"), TEXT("Shape extent: [x,y,z] for box, float for sphere radius, [radius, half_height] for capsule"))
			.Optional(TEXT("channel"), TEXT("string"), TEXT("Collision channel name"), TEXT("Visibility"))
			.Build());

	// 5. query_nearest
	Registry.RegisterAction(TEXT("mesh"), TEXT("query_nearest"),
		TEXT("Find nearest actors using physics broadphase (OverlapMultiByObjectType). Filter by class and/or tag, sorted by distance."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialActions::QueryNearest),
		FParamSchemaBuilder()
			.Required(TEXT("location"), TEXT("array"), TEXT("Search center [x, y, z]"))
			.Optional(TEXT("class_filter"), TEXT("string"), TEXT("Filter by class name (e.g. StaticMeshActor)"))
			.Optional(TEXT("tag_filter"), TEXT("string"), TEXT("Filter by actor tag"))
			.Optional(TEXT("radius"), TEXT("number"), TEXT("Search radius in cm"), TEXT("5000"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max actors to return"), TEXT("20"))
			.Build());

	// 6. query_line_of_sight
	Registry.RegisterAction(TEXT("mesh"), TEXT("query_line_of_sight"),
		TEXT("Check line of sight between two points. Returns visibility status and blocking info."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialActions::QueryLineOfSight),
		FParamSchemaBuilder()
			.Required(TEXT("from"), TEXT("array"), TEXT("Start position [x, y, z]"))
			.Required(TEXT("to"), TEXT("array"), TEXT("End position [x, y, z]"))
			.Optional(TEXT("ignore_actors"), TEXT("array"), TEXT("Array of actor names to ignore"))
			.Build());

	// 7. get_actors_in_volume
	Registry.RegisterAction(TEXT("mesh"), TEXT("get_actors_in_volume"),
		TEXT("Get all actors inside a named BlockingVolume. Checks Monolith.Owner tags and spatial containment."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialActions::GetActorsInVolume),
		FParamSchemaBuilder()
			.Required(TEXT("volume_name"), TEXT("string"), TEXT("Name or label of the BlockingVolume"))
			.Build());

	// 8. get_scene_bounds
	Registry.RegisterAction(TEXT("mesh"), TEXT("get_scene_bounds"),
		TEXT("Compute the enclosing axis-aligned bounding box for all actors (or filtered by class)."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialActions::GetSceneBounds),
		FParamSchemaBuilder()
			.Optional(TEXT("class_filter"), TEXT("string"), TEXT("Filter by class name (e.g. StaticMeshActor)"))
			.Build());

	// 9. get_scene_statistics
	Registry.RegisterAction(TEXT("mesh"), TEXT("get_scene_statistics"),
		TEXT("Get scene statistics: actor counts by class, total triangles, light count, volume count, navmesh status. Optional region filter."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialActions::GetSceneStatistics),
		FParamSchemaBuilder()
			.Optional(TEXT("region_min"), TEXT("array"), TEXT("Min corner of region filter [x, y, z]"))
			.Optional(TEXT("region_max"), TEXT("array"), TEXT("Max corner of region filter [x, y, z]"))
			.Build());

	// 10. get_spatial_relationships
	Registry.RegisterAction(TEXT("mesh"), TEXT("get_spatial_relationships"),
		TEXT("Analyze spatial relationships between an actor and its neighbors (on_top_of, inside, adjacent, above, below, etc). Thresholds scale with actor bounds."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialActions::GetSpatialRelationships),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Actor name or label"))
			.Optional(TEXT("radius"), TEXT("number"), TEXT("Search radius in cm"), TEXT("500"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max neighbors to analyze"), TEXT("20"))
			.Build());

	// 11. query_navmesh
	Registry.RegisterAction(TEXT("mesh"), TEXT("query_navmesh"),
		TEXT("Find a navigation path between two points. Returns path points and total distance. Errors if navmesh not built."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSpatialActions::QueryNavmesh),
		FParamSchemaBuilder()
			.Required(TEXT("start"), TEXT("array"), TEXT("Start position [x, y, z]"))
			.Required(TEXT("end"), TEXT("array"), TEXT("End position [x, y, z]"))
			.Optional(TEXT("agent_radius"), TEXT("number"), TEXT("Navigation agent radius"), TEXT("42"))
			.Build());
}

// ============================================================================
// 1. query_raycast
// ============================================================================

FMonolithActionResult FMonolithMeshSpatialActions::QueryRaycast(const TSharedPtr<FJsonObject>& Params)
{
	FVector Start, End;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("start"), Start))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: start (array of 3 numbers)"));
	}
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("end"), End))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: end (array of 3 numbers)"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Parse collision channel
	FString ChannelName = TEXT("Visibility");
	Params->TryGetStringField(TEXT("channel"), ChannelName);

	bool bChannelValid = false;
	ECollisionChannel Channel = ParseCollisionChannel(ChannelName, bChannelValid);
	if (!bChannelValid)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown collision channel: '%s'. Valid: Visibility, Camera, WorldStatic, WorldDynamic, Pawn, PhysicsBody, Vehicle, Destructible, GameTraceChannel1-18"),
			*ChannelName));
	}

	// Build query params
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MonolithRaycast), true);
	QueryParams.bReturnPhysicalMaterial = true;

	// Ignore actors
	const TArray<TSharedPtr<FJsonValue>>* IgnoreArr;
	if (Params->TryGetArrayField(TEXT("ignore_actors"), IgnoreArr))
	{
		for (const auto& Val : *IgnoreArr)
		{
			FString Error;
			AActor* IgnoreActor = MonolithMeshUtils::FindActorByName(Val->AsString(), Error);
			if (IgnoreActor)
			{
				QueryParams.AddIgnoredActor(IgnoreActor);
			}
		}
	}

	FHitResult HitResult;
	bool bHit = World->LineTraceSingleByChannel(HitResult, Start, End, Channel, QueryParams);

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("hit"), bHit);

	if (bHit)
	{
		AActor* HitActor = HitResult.GetActor();
		Result->SetStringField(TEXT("actor"), HitActor ? HitActor->GetActorNameOrLabel() : TEXT("None"));

		UPrimitiveComponent* HitComp = HitResult.GetComponent();
		Result->SetStringField(TEXT("component"), HitComp ? HitComp->GetFName().ToString() : TEXT("None"));

		Result->SetArrayField(TEXT("location"), VectorToJsonArray(HitResult.Location));
		Result->SetArrayField(TEXT("normal"), VectorToJsonArray(HitResult.Normal));
		Result->SetNumberField(TEXT("distance"), HitResult.Distance);

		if (HitResult.PhysMaterial.IsValid())
		{
			Result->SetStringField(TEXT("phys_material"), HitResult.PhysMaterial->GetName());
		}
		else
		{
			Result->SetStringField(TEXT("phys_material"), TEXT("None"));
		}
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 2. query_multi_raycast
// ============================================================================

FMonolithActionResult FMonolithMeshSpatialActions::QueryMultiRaycast(const TSharedPtr<FJsonObject>& Params)
{
	FVector Start, End;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("start"), Start))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: start (array of 3 numbers)"));
	}
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("end"), End))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: end (array of 3 numbers)"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString ChannelName = TEXT("Visibility");
	Params->TryGetStringField(TEXT("channel"), ChannelName);

	bool bChannelValid = false;
	ECollisionChannel Channel = ParseCollisionChannel(ChannelName, bChannelValid);
	if (!bChannelValid)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown collision channel: '%s'"), *ChannelName));
	}

	int32 MaxHits = 10;
	double MaxHitsD;
	if (Params->TryGetNumberField(TEXT("max_hits"), MaxHitsD))
	{
		MaxHits = FMath::Clamp(static_cast<int32>(MaxHitsD), 1, 100);
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MonolithMultiRaycast), true);
	QueryParams.bReturnPhysicalMaterial = true;

	TArray<FHitResult> Hits;
	World->LineTraceMultiByChannel(Hits, Start, End, Channel, QueryParams);

	// Sort by distance (should already be sorted, but ensure it)
	Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Distance < B.Distance; });

	// Clamp to max_hits
	if (Hits.Num() > MaxHits)
	{
		Hits.SetNum(MaxHits);
	}

	TArray<TSharedPtr<FJsonValue>> HitsArr;
	for (const FHitResult& Hit : Hits)
	{
		HitsArr.Add(MakeShared<FJsonValueObject>(HitResultToJson(Hit)));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("hit_count"), HitsArr.Num());
	Result->SetArrayField(TEXT("hits"), HitsArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 3. query_radial_sweep
// ============================================================================

FMonolithActionResult FMonolithMeshSpatialActions::QueryRadialSweep(const TSharedPtr<FJsonObject>& Params)
{
	FVector Origin;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("origin"), Origin))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: origin (array of 3 numbers)"));
	}

	double Radius = 1000.0;
	Params->TryGetNumberField(TEXT("radius"), Radius);

	int32 RequestedRayCount = 36;
	double RayCountD;
	if (Params->TryGetNumberField(TEXT("ray_count"), RayCountD))
	{
		RequestedRayCount = static_cast<int32>(RayCountD);
	}
	int32 RayCount = FMath::Clamp(RequestedRayCount, 1, 72);

	int32 RequestedVerticalAngles = 3;
	double VerticalAnglesD;
	if (Params->TryGetNumberField(TEXT("vertical_angles"), VerticalAnglesD))
	{
		RequestedVerticalAngles = static_cast<int32>(VerticalAnglesD);
	}
	int32 VerticalAngles = FMath::Clamp(RequestedVerticalAngles, 1, 8);

	// HARD CAP: 512 total rays
	const int32 TotalRays = RayCount * VerticalAngles;
	if (TotalRays > 512)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Requested %d*%d=%d, clamped to %d*%d=%d, exceeds hard cap of 512. Reduce ray_count or vertical_angles."),
			RequestedRayCount, RequestedVerticalAngles, RequestedRayCount * RequestedVerticalAngles,
			RayCount, VerticalAngles, TotalRays));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString ChannelName = TEXT("Visibility");
	Params->TryGetStringField(TEXT("channel"), ChannelName);

	bool bChannelValid = false;
	ECollisionChannel Channel = ParseCollisionChannel(ChannelName, bChannelValid);
	if (!bChannelValid)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown collision channel: '%s'"), *ChannelName));
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MonolithRadialSweep), true);
	QueryParams.bReturnPhysicalMaterial = true;

	// Compute elevation angles: distribute evenly from -60 to +60 degrees
	TArray<float> Elevations;
	if (VerticalAngles == 1)
	{
		Elevations.Add(0.0f);
	}
	else
	{
		for (int32 v = 0; v < VerticalAngles; ++v)
		{
			float Alpha = static_cast<float>(v) / static_cast<float>(VerticalAngles - 1);
			Elevations.Add(FMath::Lerp(-60.0f, 60.0f, Alpha));
		}
	}

	// Compass direction names and their angle ranges (in degrees, 0 = +X = East in UE)
	// UE: X = forward, Y = right. We map compass to: N = +X, E = +Y, S = -X, W = -Y
	struct FCompassDir
	{
		const TCHAR* Name;
		float MinAngle; // inclusive
		float MaxAngle; // exclusive
	};

	// Use 8 compass sectors of 45 degrees each, centered on cardinal/ordinal directions
	// N = 0 degrees (forward/+X), angles measured clockwise from +X
	static const FCompassDir CompassDirs[] =
	{
		{ TEXT("N"),  -22.5f,  22.5f  },
		{ TEXT("NE"),  22.5f,  67.5f  },
		{ TEXT("E"),   67.5f, 112.5f  },
		{ TEXT("SE"), 112.5f, 157.5f  },
		{ TEXT("S"),  157.5f, 202.5f  },
		{ TEXT("SW"), 202.5f, 247.5f  },
		{ TEXT("W"),  247.5f, 292.5f  },
		{ TEXT("NW"), 292.5f, 337.5f  },
	};

	// Track per-direction data
	struct FDirectionData
	{
		TArray<float> Distances;
		TSet<FString> ActorsHit;
	};

	TMap<FString, FDirectionData> DirectionMap;
	// Also track up/down
	DirectionMap.Add(TEXT("up"));
	DirectionMap.Add(TEXT("down"));
	for (const auto& Dir : CompassDirs)
	{
		DirectionMap.Add(Dir.Name);
	}

	TSet<FString> AllActorsHit;
	int32 TotalHits = 0;

	for (int32 a = 0; a < RayCount; ++a)
	{
		float AzimuthDeg = (360.0f / static_cast<float>(RayCount)) * static_cast<float>(a);

		for (int32 v = 0; v < VerticalAngles; ++v)
		{
			float ElevDeg = Elevations[v];
			float ElevRad = FMath::DegreesToRadians(ElevDeg);
			float AzimuthRad = FMath::DegreesToRadians(AzimuthDeg);

			FVector Dir;
			Dir.X = FMath::Cos(ElevRad) * FMath::Cos(AzimuthRad);
			Dir.Y = FMath::Cos(ElevRad) * FMath::Sin(AzimuthRad);
			Dir.Z = FMath::Sin(ElevRad);
			Dir.Normalize();

			FVector End = Origin + Dir * Radius;

			FHitResult Hit;
			bool bHit = World->LineTraceSingleByChannel(Hit, Origin, End, Channel, QueryParams);

			// Determine which compass direction this ray belongs to
			FString DirName;
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
				// Normalize azimuth to 0-360
				float NormAzimuth = FMath::Fmod(AzimuthDeg + 360.0f, 360.0f);
				DirName = TEXT("N"); // default
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
						// Wraps around 360 (N sector: 337.5 - 22.5)
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
	}

	// Build summary JSON
	auto SummaryObj = MakeShared<FJsonObject>();
	for (auto& Pair : DirectionMap)
	{
		auto DirObj = MakeShared<FJsonObject>();

		DirObj->SetNumberField(TEXT("hit_count"), Pair.Value.Distances.Num());

		if (Pair.Value.Distances.Num() > 0)
		{
			float Sum = 0.0f;
			float Min = TNumericLimits<float>::Max();
			for (float D : Pair.Value.Distances)
			{
				Sum += D;
				Min = FMath::Min(Min, D);
			}
			DirObj->SetNumberField(TEXT("avg_distance"), Sum / Pair.Value.Distances.Num());
			DirObj->SetNumberField(TEXT("min_distance"), Min);
		}
		else
		{
			DirObj->SetNumberField(TEXT("avg_distance"), -1.0);
			DirObj->SetNumberField(TEXT("min_distance"), -1.0);
		}

		TArray<TSharedPtr<FJsonValue>> ActorArr;
		for (const FString& ActorName : Pair.Value.ActorsHit)
		{
			ActorArr.Add(MakeShared<FJsonValueString>(ActorName));
		}
		DirObj->SetArrayField(TEXT("actors_hit"), ActorArr);

		SummaryObj->SetObjectField(Pair.Key, DirObj);
	}

	TArray<TSharedPtr<FJsonValue>> UniqueActorsArr;
	for (const FString& ActorName : AllActorsHit)
	{
		UniqueActorsArr.Add(MakeShared<FJsonValueString>(ActorName));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("origin"), VectorToJsonArray(Origin));
	Result->SetNumberField(TEXT("radius"), Radius);
	Result->SetNumberField(TEXT("total_rays"), TotalRays);
	Result->SetNumberField(TEXT("hits"), TotalHits);
	Result->SetObjectField(TEXT("summary_by_direction"), SummaryObj);
	Result->SetArrayField(TEXT("unique_actors_hit"), UniqueActorsArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 4. query_overlap
// ============================================================================

FMonolithActionResult FMonolithMeshSpatialActions::QueryOverlap(const TSharedPtr<FJsonObject>& Params)
{
	FVector Location;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: location (array of 3 numbers)"));
	}

	FString ShapeStr;
	if (!Params->TryGetStringField(TEXT("shape"), ShapeStr))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: shape (box, sphere, or capsule)"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString ChannelName = TEXT("Visibility");
	Params->TryGetStringField(TEXT("channel"), ChannelName);

	bool bChannelValid = false;
	ECollisionChannel Channel = ParseCollisionChannel(ChannelName, bChannelValid);
	if (!bChannelValid)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown collision channel: '%s'"), *ChannelName));
	}

	// Parse shape
	FCollisionShape CollisionShape;

	if (ShapeStr.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
	{
		double RadiusVal = 100.0;
		if (!Params->TryGetNumberField(TEXT("extent"), RadiusVal))
		{
			return FMonolithActionResult::Error(TEXT("For sphere shape, extent must be a number (radius)"));
		}
		CollisionShape = FCollisionShape::MakeSphere(static_cast<float>(RadiusVal));
	}
	else if (ShapeStr.Equals(TEXT("box"), ESearchCase::IgnoreCase))
	{
		FVector BoxExtent;
		if (!MonolithMeshUtils::ParseVector(Params, TEXT("extent"), BoxExtent))
		{
			return FMonolithActionResult::Error(TEXT("For box shape, extent must be an array [x, y, z] (half-extents)"));
		}
		CollisionShape = FCollisionShape::MakeBox(BoxExtent);
	}
	else if (ShapeStr.Equals(TEXT("capsule"), ESearchCase::IgnoreCase))
	{
		const TArray<TSharedPtr<FJsonValue>>* ExtentArr;
		if (!Params->TryGetArrayField(TEXT("extent"), ExtentArr) || ExtentArr->Num() < 2)
		{
			return FMonolithActionResult::Error(TEXT("For capsule shape, extent must be an array [radius, half_height]"));
		}
		float CapsuleRadius = static_cast<float>((*ExtentArr)[0]->AsNumber());
		float CapsuleHalfHeight = static_cast<float>((*ExtentArr)[1]->AsNumber());
		CollisionShape = FCollisionShape::MakeCapsule(CapsuleRadius, CapsuleHalfHeight);
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid shape: '%s'. Must be box, sphere, or capsule."), *ShapeStr));
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MonolithOverlap), true);

	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByChannel(Overlaps, Location, FQuat::Identity, Channel, CollisionShape, QueryParams);

	// Deduplicate by actor
	TSet<AActor*> SeenActors;
	TArray<TSharedPtr<FJsonValue>> ActorsArr;

	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Actor = Overlap.GetActor();
		if (!Actor || SeenActors.Contains(Actor))
		{
			continue;
		}
		SeenActors.Add(Actor);

		auto ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("name"), Actor->GetActorNameOrLabel());
		ActorObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

		UPrimitiveComponent* Comp = Overlap.GetComponent();
		ActorObj->SetStringField(TEXT("component"), Comp ? Comp->GetFName().ToString() : TEXT("None"));

		ActorsArr.Add(MakeShared<FJsonValueObject>(ActorObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("overlap_count"), ActorsArr.Num());
	Result->SetArrayField(TEXT("actors"), ActorsArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 5. query_nearest
// ============================================================================

FMonolithActionResult FMonolithMeshSpatialActions::QueryNearest(const TSharedPtr<FJsonObject>& Params)
{
	FVector Location;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: location (array of 3 numbers)"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString ClassFilter;
	Params->TryGetStringField(TEXT("class_filter"), ClassFilter);

	FString TagFilter;
	Params->TryGetStringField(TEXT("tag_filter"), TagFilter);

	double RadiusD = 5000.0;
	Params->TryGetNumberField(TEXT("radius"), RadiusD);
	float Radius = static_cast<float>(RadiusD);

	int32 Limit = 20;
	double LimitD;
	if (Params->TryGetNumberField(TEXT("limit"), LimitD))
	{
		Limit = FMath::Clamp(static_cast<int32>(LimitD), 1, 500);
	}

	// Use OverlapMultiByObjectType with sphere (physics broadphase)
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MonolithNearest), true);
	FCollisionObjectQueryParams ObjectQueryParams(FCollisionObjectQueryParams::AllObjects);
	FCollisionShape SphereShape = FCollisionShape::MakeSphere(Radius);

	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByObjectType(Overlaps, Location, FQuat::Identity, ObjectQueryParams, SphereShape, QueryParams);

	// Deduplicate by actor and collect info
	struct FActorEntry
	{
		AActor* Actor;
		float Distance;
	};

	TSet<AActor*> SeenActors;
	TArray<FActorEntry> Entries;

	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Actor = Overlap.GetActor();
		if (!Actor || SeenActors.Contains(Actor))
		{
			continue;
		}

		// Class filter
		if (!ClassFilter.IsEmpty())
		{
			FString ActorClassName = Actor->GetClass()->GetName();
			if (!ActorClassName.Equals(ClassFilter, ESearchCase::IgnoreCase))
			{
				// Also try without 'A' prefix
				FString AltName = ClassFilter.StartsWith(TEXT("A")) ? ClassFilter.Mid(1) : (TEXT("A") + ClassFilter);
				if (!ActorClassName.Equals(AltName, ESearchCase::IgnoreCase))
				{
					continue;
				}
			}
		}

		// Tag filter
		if (!TagFilter.IsEmpty())
		{
			bool bHasTag = false;
			for (const FName& Tag : Actor->Tags)
			{
				if (Tag.ToString().Equals(TagFilter, ESearchCase::IgnoreCase))
				{
					bHasTag = true;
					break;
				}
			}
			if (!bHasTag)
			{
				continue;
			}
		}

		SeenActors.Add(Actor);
		float Dist = FVector::Dist(Location, Actor->GetActorLocation());
		Entries.Add({ Actor, Dist });
	}

	// Sort by distance
	Entries.Sort([](const FActorEntry& A, const FActorEntry& B) { return A.Distance < B.Distance; });

	int32 TotalInRadius = Entries.Num();

	// Clamp to limit
	if (Entries.Num() > Limit)
	{
		Entries.SetNum(Limit);
	}

	TArray<TSharedPtr<FJsonValue>> ActorsArr;
	for (const FActorEntry& Entry : Entries)
	{
		auto ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("name"), Entry.Actor->GetActorNameOrLabel());
		ActorObj->SetStringField(TEXT("class"), Entry.Actor->GetClass()->GetName());
		ActorObj->SetNumberField(TEXT("distance"), Entry.Distance);
		ActorObj->SetArrayField(TEXT("location"), VectorToJsonArray(Entry.Actor->GetActorLocation()));
		ActorsArr.Add(MakeShared<FJsonValueObject>(ActorObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total_in_radius"), TotalInRadius);
	Result->SetNumberField(TEXT("returned"), ActorsArr.Num());
	Result->SetArrayField(TEXT("actors"), ActorsArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 6. query_line_of_sight
// ============================================================================

FMonolithActionResult FMonolithMeshSpatialActions::QueryLineOfSight(const TSharedPtr<FJsonObject>& Params)
{
	FVector From, To;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("from"), From))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: from (array of 3 numbers)"));
	}
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("to"), To))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: to (array of 3 numbers)"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MonolithLOS), true);
	QueryParams.bReturnPhysicalMaterial = true;

	// Ignore actors
	const TArray<TSharedPtr<FJsonValue>>* IgnoreArr;
	if (Params->TryGetArrayField(TEXT("ignore_actors"), IgnoreArr))
	{
		for (const auto& Val : *IgnoreArr)
		{
			FString Error;
			AActor* IgnoreActor = MonolithMeshUtils::FindActorByName(Val->AsString(), Error);
			if (IgnoreActor)
			{
				QueryParams.AddIgnoredActor(IgnoreActor);
			}
		}
	}

	FHitResult Hit;
	bool bHit = World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, QueryParams);

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("visible"), !bHit);

	if (bHit)
	{
		AActor* BlockingActor = Hit.GetActor();
		Result->SetStringField(TEXT("blocking_actor"), BlockingActor ? BlockingActor->GetActorNameOrLabel() : TEXT("None"));
		Result->SetNumberField(TEXT("blocking_distance"), Hit.Distance);
	}
	else
	{
		Result->SetField(TEXT("blocking_actor"), MakeShared<FJsonValueNull>());
		Result->SetNumberField(TEXT("blocking_distance"), FVector::Dist(From, To));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 7. get_actors_in_volume
// ============================================================================

FMonolithActionResult FMonolithMeshSpatialActions::GetActorsInVolume(const TSharedPtr<FJsonObject>& Params)
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

	// Find the blocking volume
	ABlockingVolume* Volume = nullptr;
	for (TActorIterator<ABlockingVolume> It(World); It; ++It)
	{
		if (It->GetActorNameOrLabel() == VolumeName || It->GetActorLabel() == VolumeName || It->GetFName().ToString() == VolumeName)
		{
			Volume = *It;
			break;
		}
	}

	if (!Volume)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("BlockingVolume not found: %s"), *VolumeName));
	}

	// Get volume bounds
	FVector VolumeOrigin, VolumeExtent;
	Volume->GetActorBounds(false, VolumeOrigin, VolumeExtent);
	FBox VolumeBounds(VolumeOrigin - VolumeExtent, VolumeOrigin + VolumeExtent);

	// Collect actors: those with Monolith.Owner tag matching this volume, or spatially inside
	FString OwnerTag = FString::Printf(TEXT("Monolith.Owner:%s"), *VolumeName);

	TArray<TSharedPtr<FJsonValue>> ActorsArr;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor == Volume)
		{
			continue;
		}

		bool bInclude = false;

		// Check for Monolith.Owner tag
		for (const FName& Tag : Actor->Tags)
		{
			if (Tag.ToString().Equals(OwnerTag, ESearchCase::IgnoreCase))
			{
				bInclude = true;
				break;
			}
		}

		// Spatial containment check
		if (!bInclude)
		{
			FVector ActorOrigin, ActorExtent;
			Actor->GetActorBounds(false, ActorOrigin, ActorExtent);
			FBox ActorBounds(ActorOrigin - ActorExtent, ActorOrigin + ActorExtent);

			if (VolumeBounds.Intersect(ActorBounds))
			{
				bInclude = true;
			}
		}

		if (bInclude)
		{
			auto ActorObj = MakeShared<FJsonObject>();
			ActorObj->SetStringField(TEXT("name"), Actor->GetActorNameOrLabel());
			ActorObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
			ActorObj->SetNumberField(TEXT("distance"), FVector::Dist(VolumeOrigin, Actor->GetActorLocation()));
			ActorObj->SetArrayField(TEXT("location"), VectorToJsonArray(Actor->GetActorLocation()));
			ActorsArr.Add(MakeShared<FJsonValueObject>(ActorObj));
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("volume"), VolumeName);
	Result->SetNumberField(TEXT("actor_count"), ActorsArr.Num());
	Result->SetArrayField(TEXT("actors"), ActorsArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 8. get_scene_bounds
// ============================================================================

FMonolithActionResult FMonolithMeshSpatialActions::GetSceneBounds(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString ClassFilter;
	Params->TryGetStringField(TEXT("class_filter"), ClassFilter);

	FBox SceneBounds(ForceInit);
	int32 ActorCount = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;

		// Class filter
		if (!ClassFilter.IsEmpty())
		{
			FString ActorClassName = Actor->GetClass()->GetName();
			if (!ActorClassName.Equals(ClassFilter, ESearchCase::IgnoreCase))
			{
				FString AltName = ClassFilter.StartsWith(TEXT("A")) ? ClassFilter.Mid(1) : (TEXT("A") + ClassFilter);
				if (!ActorClassName.Equals(AltName, ESearchCase::IgnoreCase))
				{
					continue;
				}
			}
		}

		FVector Origin, Extent;
		Actor->GetActorBounds(false, Origin, Extent);

		// Skip actors with zero extent (lights, cameras without mesh, etc.)
		if (Extent.IsNearlyZero())
		{
			continue;
		}

		FBox ActorBounds(Origin - Extent, Origin + Extent);

		if (ActorCount == 0)
		{
			SceneBounds = ActorBounds;
		}
		else
		{
			SceneBounds += ActorBounds;
		}
		ActorCount++;
	}

	if (ActorCount == 0)
	{
		return FMonolithActionResult::Error(ClassFilter.IsEmpty()
			? TEXT("No actors found in the scene")
			: FString::Printf(TEXT("No actors of class '%s' found"), *ClassFilter));
	}

	FVector Center = SceneBounds.GetCenter();
	FVector Extent = SceneBounds.GetExtent();

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("center"), VectorToJsonArray(Center));
	Result->SetArrayField(TEXT("extent"), VectorToJsonArray(Extent));
	Result->SetArrayField(TEXT("min"), VectorToJsonArray(SceneBounds.Min));
	Result->SetArrayField(TEXT("max"), VectorToJsonArray(SceneBounds.Max));
	Result->SetNumberField(TEXT("actor_count"), ActorCount);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 9. get_scene_statistics
// ============================================================================

FMonolithActionResult FMonolithMeshSpatialActions::GetSceneStatistics(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Optional region filter
	FVector RegionMin, RegionMax;
	bool bHasRegion = false;
	FBox RegionBox(ForceInit);
	if (MonolithMeshUtils::ParseVector(Params, TEXT("region_min"), RegionMin) &&
		MonolithMeshUtils::ParseVector(Params, TEXT("region_max"), RegionMax))
	{
		bHasRegion = true;
		RegionBox = FBox(RegionMin, RegionMax);
	}

	TMap<FString, int32> ClassCounts;
	int64 TotalTriangles = 0;
	int32 LightCount = 0;
	int32 VolumeCount = 0;
	int32 DrawCallEstimate = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;

		// Region filter
		if (bHasRegion)
		{
			FVector Origin, Extent;
			Actor->GetActorBounds(false, Origin, Extent);
			if (!RegionBox.Intersect(FBox(Origin - Extent, Origin + Extent)))
			{
				continue;
			}
		}

		FString ClassName = Actor->GetClass()->GetName();
		ClassCounts.FindOrAdd(ClassName)++;

		// Count triangles and draw calls from StaticMesh components
		TArray<UStaticMeshComponent*> SMCs;
		Actor->GetComponents(SMCs);
		for (UStaticMeshComponent* SMC : SMCs)
		{
			UStaticMesh* Mesh = SMC->GetStaticMesh();
			if (Mesh && Mesh->GetRenderData() && Mesh->GetRenderData()->LODResources.Num() > 0)
			{
				TotalTriangles += Mesh->GetRenderData()->LODResources[0].GetNumTriangles();
			}
			DrawCallEstimate += FMath::Max(1, SMC->GetNumMaterials());
		}

		// Count lights
		if (Actor->FindComponentByClass<ULightComponent>())
		{
			LightCount++;
		}

		// Count volumes
		if (Actor->IsA(AVolume::StaticClass()))
		{
			VolumeCount++;
		}
	}

	// Build class count JSON
	auto ClassCountObj = MakeShared<FJsonObject>();
	for (const auto& Pair : ClassCounts)
	{
		ClassCountObj->SetNumberField(Pair.Key, Pair.Value);
	}

	// Navmesh status
	FString NavmeshStatus = TEXT("not_built");
	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (NavSys)
	{
		ANavigationData* NavData = NavSys->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
		if (NavData)
		{
			NavmeshStatus = TEXT("built");
		}
		else
		{
			NavmeshStatus = TEXT("no_navdata");
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("actor_count"), ClassCountObj);
	Result->SetNumberField(TEXT("total_triangles"), static_cast<double>(TotalTriangles));
	Result->SetNumberField(TEXT("draw_calls_estimate"), DrawCallEstimate);
	Result->SetNumberField(TEXT("light_count"), LightCount);
	Result->SetNumberField(TEXT("volume_count"), VolumeCount);
	Result->SetStringField(TEXT("navmesh_status"), NavmeshStatus);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 10. get_spatial_relationships
// ============================================================================

FMonolithActionResult FMonolithMeshSpatialActions::GetSpatialRelationships(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: actor_name"));
	}

	FString Error;
	AActor* TargetActor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!TargetActor)
	{
		return FMonolithActionResult::Error(Error);
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	double RadiusD = 500.0;
	Params->TryGetNumberField(TEXT("radius"), RadiusD);
	float Radius = static_cast<float>(RadiusD);

	int32 Limit = 20;
	double LimitD;
	if (Params->TryGetNumberField(TEXT("limit"), LimitD))
	{
		Limit = FMath::Clamp(static_cast<int32>(LimitD), 1, 200);
	}

	// Get target actor bounds
	FVector TargetOrigin, TargetExtent;
	TargetActor->GetActorBounds(false, TargetOrigin, TargetExtent);

	// Compute threshold scale from actor bounds
	float SmallestAxis = FMath::Min3(TargetExtent.X, TargetExtent.Y, TargetExtent.Z) * 2.0f; // full extent
	if (SmallestAxis < 1.0f)
	{
		SmallestAxis = 1.0f;
	}

	float OnTopOfThreshold = FMath::Max(10.0f, SmallestAxis * 0.1f);
	float AdjacentThreshold = FMath::Max(20.0f, SmallestAxis * 0.15f);

	// Use broadphase to find neighbors
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MonolithSpatialRel), true);
	QueryParams.AddIgnoredActor(TargetActor);
	FCollisionObjectQueryParams ObjectQueryParams(FCollisionObjectQueryParams::AllObjects);
	FCollisionShape SphereShape = FCollisionShape::MakeSphere(Radius);

	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByObjectType(Overlaps, TargetOrigin, FQuat::Identity, ObjectQueryParams, SphereShape, QueryParams);

	// Deduplicate and sort by distance
	struct FNeighborEntry
	{
		AActor* Actor;
		float Distance;
	};

	TSet<AActor*> SeenActors;
	TArray<FNeighborEntry> Neighbors;

	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Actor = Overlap.GetActor();
		if (!Actor || Actor == TargetActor || SeenActors.Contains(Actor))
		{
			continue;
		}
		SeenActors.Add(Actor);

		float Dist = FVector::Dist(TargetOrigin, Actor->GetActorLocation());
		Neighbors.Add({ Actor, Dist });
	}

	Neighbors.Sort([](const FNeighborEntry& A, const FNeighborEntry& B) { return A.Distance < B.Distance; });

	int32 TotalInRadius = Neighbors.Num();
	if (Neighbors.Num() > Limit)
	{
		Neighbors.SetNum(Limit);
	}

	// Get target bounds as FBox
	FBox TargetBox(TargetOrigin - TargetExtent, TargetOrigin + TargetExtent);

	// Forward vector of target actor for in_front_of / behind
	FVector TargetForward = TargetActor->GetActorForwardVector();
	FVector TargetRight = TargetActor->GetActorRightVector();

	TArray<TSharedPtr<FJsonValue>> NeighborsArr;

	for (const FNeighborEntry& Entry : Neighbors)
	{
		FVector NeighborOrigin, NeighborExtent;
		Entry.Actor->GetActorBounds(false, NeighborOrigin, NeighborExtent);
		FBox NeighborBox(NeighborOrigin - NeighborExtent, NeighborOrigin + NeighborExtent);

		TArray<TSharedPtr<FJsonValue>> Relationships;

		// Direction from target to neighbor
		FVector Delta = NeighborOrigin - TargetOrigin;

		// inside: neighbor box is fully contained within target box
		if (TargetBox.Min.X <= NeighborBox.Min.X && TargetBox.Min.Y <= NeighborBox.Min.Y && TargetBox.Min.Z <= NeighborBox.Min.Z &&
			TargetBox.Max.X >= NeighborBox.Max.X && TargetBox.Max.Y >= NeighborBox.Max.Y && TargetBox.Max.Z >= NeighborBox.Max.Z)
		{
			Relationships.Add(MakeShared<FJsonValueString>(TEXT("inside")));
		}

		// on_top_of: neighbor's bottom is near target's top
		float NeighborBottom = NeighborBox.Min.Z;
		float TargetTop = TargetBox.Max.Z;
		float VerticalGap = NeighborBottom - TargetTop;
		if (VerticalGap >= -OnTopOfThreshold && VerticalGap <= OnTopOfThreshold)
		{
			// Also check horizontal overlap
			bool bHorizontalOverlap =
				NeighborBox.Max.X > TargetBox.Min.X && NeighborBox.Min.X < TargetBox.Max.X &&
				NeighborBox.Max.Y > TargetBox.Min.Y && NeighborBox.Min.Y < TargetBox.Max.Y;
			if (bHorizontalOverlap)
			{
				Relationships.Add(MakeShared<FJsonValueString>(TEXT("on_top_of")));
			}
		}

		// above: neighbor center is significantly above target center
		if (Delta.Z > TargetExtent.Z + NeighborExtent.Z)
		{
			Relationships.Add(MakeShared<FJsonValueString>(TEXT("above")));
		}

		// below: neighbor center is significantly below target center
		if (Delta.Z < -(TargetExtent.Z + NeighborExtent.Z))
		{
			Relationships.Add(MakeShared<FJsonValueString>(TEXT("below")));
		}

		// adjacent: bounding boxes are close but not overlapping
		float MinSepX = FMath::Max(0.0f, FMath::Max(NeighborBox.Min.X - TargetBox.Max.X, TargetBox.Min.X - NeighborBox.Max.X));
		float MinSepY = FMath::Max(0.0f, FMath::Max(NeighborBox.Min.Y - TargetBox.Max.Y, TargetBox.Min.Y - NeighborBox.Max.Y));
		float MinSepZ = FMath::Max(0.0f, FMath::Max(NeighborBox.Min.Z - TargetBox.Max.Z, TargetBox.Min.Z - NeighborBox.Max.Z));
		float MinSeparation = FMath::Sqrt(MinSepX * MinSepX + MinSepY * MinSepY + MinSepZ * MinSepZ);

		if (MinSeparation <= AdjacentThreshold && MinSeparation > 0.0f)
		{
			Relationships.Add(MakeShared<FJsonValueString>(TEXT("adjacent")));
		}

		// Directional relationships (using actor's local frame)
		FVector DeltaHoriz(Delta.X, Delta.Y, 0.0f);
		if (DeltaHoriz.SizeSquared() > 1.0f)
		{
			DeltaHoriz.Normalize();

			float ForwardDot = FVector::DotProduct(DeltaHoriz, TargetForward);
			float RightDot = FVector::DotProduct(DeltaHoriz, TargetRight);

			if (ForwardDot > 0.5f)
			{
				Relationships.Add(MakeShared<FJsonValueString>(TEXT("in_front_of")));
			}
			if (ForwardDot < -0.5f)
			{
				Relationships.Add(MakeShared<FJsonValueString>(TEXT("behind")));
			}
			if (RightDot > 0.5f)
			{
				Relationships.Add(MakeShared<FJsonValueString>(TEXT("right_of")));
			}
			if (RightDot < -0.5f)
			{
				Relationships.Add(MakeShared<FJsonValueString>(TEXT("left_of")));
			}
		}

		// near: catch-all if within radius but no specific relationship
		if (Relationships.Num() == 0)
		{
			Relationships.Add(MakeShared<FJsonValueString>(TEXT("near")));
		}

		auto NeighborObj = MakeShared<FJsonObject>();
		NeighborObj->SetStringField(TEXT("name"), Entry.Actor->GetActorNameOrLabel());
		NeighborObj->SetNumberField(TEXT("distance"), Entry.Distance);
		NeighborObj->SetArrayField(TEXT("relationships"), Relationships);
		NeighborsArr.Add(MakeShared<FJsonValueObject>(NeighborObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor"), ActorName);
	Result->SetNumberField(TEXT("total_in_radius"), TotalInRadius);
	Result->SetNumberField(TEXT("returned"), NeighborsArr.Num());
	Result->SetArrayField(TEXT("neighbors"), NeighborsArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 11. query_navmesh
// ============================================================================

FMonolithActionResult FMonolithMeshSpatialActions::QueryNavmesh(const TSharedPtr<FJsonObject>& Params)
{
	FVector Start, End;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("start"), Start))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: start (array of 3 numbers)"));
	}
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("end"), End))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: end (array of 3 numbers)"));
	}

	double AgentRadiusD = 42.0;
	Params->TryGetNumberField(TEXT("agent_radius"), AgentRadiusD);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return FMonolithActionResult::Error(TEXT("Navmesh not built. Build navigation in the editor first (Build > Build Paths)."));
	}

	ANavigationData* NavData = NavSys->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
	if (!NavData)
	{
		return FMonolithActionResult::Error(TEXT("Navmesh not built. Build navigation in the editor first (Build > Build Paths)."));
	}

	// Set up agent properties
	FNavAgentProperties AgentProps;
	AgentProps.AgentRadius = static_cast<float>(AgentRadiusD);
	AgentProps.AgentHeight = 192.0f; // UE default capsule half-height * 2

	FPathFindingQuery Query(nullptr, *NavData, Start, End);
	Query.SetAllowPartialPaths(true);

	FPathFindingResult PathResult = NavSys->FindPathSync(AgentProps, Query);

	auto Result = MakeShared<FJsonObject>();

	if (!PathResult.IsSuccessful() && !PathResult.Path.IsValid())
	{
		Result->SetBoolField(TEXT("reachable"), false);
		Result->SetBoolField(TEXT("path_complete"), false);
		Result->SetArrayField(TEXT("path_points"), TArray<TSharedPtr<FJsonValue>>());
		Result->SetNumberField(TEXT("distance"), 0.0);
		Result->SetNumberField(TEXT("segment_count"), 0);
		return FMonolithActionResult::Success(Result);
	}

	const TArray<FNavPathPoint>& PathPoints = PathResult.Path->GetPathPoints();

	bool bIsPartial = PathResult.Path->IsPartial();
	Result->SetBoolField(TEXT("reachable"), PathResult.IsSuccessful());
	Result->SetBoolField(TEXT("path_complete"), !bIsPartial);

	// Convert path points
	TArray<TSharedPtr<FJsonValue>> PointsArr;
	double TotalDistance = 0.0;

	for (int32 i = 0; i < PathPoints.Num(); ++i)
	{
		PointsArr.Add(MakeShared<FJsonValueArray>(VectorToJsonArray(PathPoints[i].Location)));

		if (i > 0)
		{
			TotalDistance += FVector::Dist(PathPoints[i - 1].Location, PathPoints[i].Location);
		}
	}

	Result->SetArrayField(TEXT("path_points"), PointsArr);
	Result->SetNumberField(TEXT("distance"), TotalDistance);
	Result->SetNumberField(TEXT("segment_count"), FMath::Max(0, PathPoints.Num() - 1));

	if (bIsPartial)
	{
		Result->SetStringField(TEXT("warning"), TEXT("Path is partial - destination may not be fully reachable via navmesh"));
	}

	return FMonolithActionResult::Success(Result);
}
