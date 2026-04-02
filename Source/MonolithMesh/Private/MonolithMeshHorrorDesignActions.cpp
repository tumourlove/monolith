#include "MonolithMeshHorrorDesignActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithMeshAnalysis.h"
#include "MonolithMeshLightingCapture.h"
#include "MonolithMeshAcoustics.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "CollisionQueryParams.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "AI/Navigation/NavigationTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

// ============================================================================
// Helpers
// ============================================================================

namespace
{
	TArray<TSharedPtr<FJsonValue>> HDA_VecToArr(const FVector& V)
	{
		return MonolithMeshAnalysis::VectorToJsonArray(V);
	}

	/** Parse an array of [x,y,z] arrays from a JSON field */
	bool HDA_ParseVectorArray(const TSharedPtr<FJsonObject>& Params, const FString& Key, TArray<FVector>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!Params->TryGetArrayField(Key, Arr) || Arr->Num() == 0)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			const TArray<TSharedPtr<FJsonValue>>* Inner;
			if (Val->TryGetArray(Inner) && Inner->Num() >= 3)
			{
				FVector V;
				V.X = (*Inner)[0]->AsNumber();
				V.Y = (*Inner)[1]->AsNumber();
				V.Z = (*Inner)[2]->AsNumber();
				Out.Add(V);
			}
		}

		return Out.Num() > 0;
	}

	/** Parse a string array from a JSON field */
	bool ParseStringArray(const TSharedPtr<FJsonObject>& Params, const FString& Key, TArray<FString>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!Params->TryGetArrayField(Key, Arr))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			Out.Add(Val->AsString());
		}
		return Out.Num() > 0;
	}

	/** Resample a polyline at regular intervals. Returns at least start and end. */
	TArray<FVector> ResamplePath(const TArray<FVector>& PathPoints, float Interval)
	{
		TArray<FVector> Result;
		if (PathPoints.Num() < 2)
		{
			Result = PathPoints;
			return Result;
		}

		Result.Add(PathPoints[0]);
		float AccumDist = 0.0f;
		float NextSample = Interval;

		for (int32 i = 1; i < PathPoints.Num(); ++i)
		{
			float SegLen = FVector::Dist(PathPoints[i - 1], PathPoints[i]);
			FVector SegDir = (PathPoints[i] - PathPoints[i - 1]).GetSafeNormal();
			float SegProgress = 0.0f;

			while (true)
			{
				float Remaining = NextSample - (AccumDist + SegProgress);
				float SegRemaining = SegLen - SegProgress;
				if (Remaining <= SegRemaining)
				{
					SegProgress += Remaining;
					Result.Add(PathPoints[i - 1] + SegDir * SegProgress);
					NextSample += Interval;
				}
				else
				{
					break;
				}
			}
			AccumDist += SegLen;
		}

		// Always include the last point
		if (Result.Num() > 0 && FVector::Dist(Result.Last(), PathPoints.Last()) > 10.0f)
		{
			Result.Add(PathPoints.Last());
		}

		return Result;
	}

	/** Compute total polyline distance */
	float PathLength(const TArray<FVector>& Points)
	{
		float Dist = 0.0f;
		for (int32 i = 1; i < Points.Num(); ++i)
		{
			Dist += FVector::Dist(Points[i - 1], Points[i]);
		}
		return Dist;
	}

	/** Quick 8-direction average sightline distance at a point (eye height offset) */
	float QuickAvgSightlineDistance(UWorld* World, const FVector& Location)
	{
		FCollisionQueryParams QP(SCENE_QUERY_STAT(MonolithHorrorDesign), true);
		FVector Origin = Location + FVector(0, 0, 170.0f);
		float TotalDist = 0.0f;
		const int32 Dirs = 8;
		for (int32 d = 0; d < Dirs; ++d)
		{
			float Angle = (2.0f * PI / static_cast<float>(Dirs)) * static_cast<float>(d);
			FVector Dir(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
			FHitResult Hit;
			bool bHit = World->LineTraceSingleByChannel(Hit, Origin, Origin + Dir * 5000.0f, ECC_Visibility, QP);
			TotalDist += bHit ? Hit.Distance : 5000.0f;
		}
		return TotalDist / static_cast<float>(Dirs);
	}

	/** Quick tension score at a point (lightweight — sightlines + ceiling only, skip volume/exits for speed) */
	float QuickTensionScore(UWorld* World, const FVector& Location)
	{
		MonolithMeshAnalysis::FTensionInputs Inputs;
		Inputs.AverageSightlineDistance = QuickAvgSightlineDistance(World, Location);
		Inputs.CeilingHeight = MonolithMeshAnalysis::MeasureCeilingHeight(World, Location);
		Inputs.RoomVolume = 0.0f; // Neutral
		Inputs.ExitCount = 2;     // Neutral
		return MonolithMeshAnalysis::ComputeTensionScore(Inputs);
	}

	/** Quick perpendicular clearance at a point along a direction */
	float QuickPerpendicularClearance(UWorld* World, const FVector& Location, const FVector& Forward)
	{
		FVector Right = FVector::CrossProduct(Forward, FVector::UpVector).GetSafeNormal();
		if (Right.IsNearlyZero()) Right = FVector::RightVector;

		FCollisionQueryParams QP(SCENE_QUERY_STAT(MonolithHorrorDesignClearance), true);
		FVector TestPt = Location + FVector(0, 0, 50.0f);
		const float MaxDist = 500.0f;

		float RightDist = MaxDist;
		float LeftDist = MaxDist;

		{
			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, TestPt, TestPt + Right * MaxDist, ECC_Visibility, QP))
			{
				RightDist = Hit.Distance;
			}
		}
		{
			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, TestPt, TestPt - Right * MaxDist, ECC_Visibility, QP))
			{
				LeftDist = Hit.Distance;
			}
		}

		return LeftDist + RightDist;
	}

	/** Minimum distance from a point to the nearest wall (any horizontal direction) */
	float MinWallDistance(UWorld* World, const FVector& Location)
	{
		FCollisionQueryParams QP(SCENE_QUERY_STAT(MonolithHorrorDesignWall), true);
		FVector TestPt = Location + FVector(0, 0, 50.0f);
		const float MaxDist = 500.0f;
		float MinDist = MaxDist;

		for (int32 d = 0; d < 8; ++d)
		{
			float Angle = (2.0f * PI / 8.0f) * static_cast<float>(d);
			FVector Dir(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, TestPt, TestPt + Dir * MaxDist, ECC_Visibility, QP))
			{
				MinDist = FMath::Min(MinDist, Hit.Distance);
			}
		}
		return MinDist;
	}

	/** Compute a safety score for a grid cell. 0 = dangerous, 1 = safe.
	 *  Factors: sightline distance (high = safe), clearance (wide = safe), lighting (bright = safe) */
	struct FGridCell
	{
		FVector Location;
		float SafetyScore = 0.0f;
		float CuriosityScore = 0.0f;
		float CautionScore = 0.0f;
	};
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshHorrorDesignActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 1. predict_player_paths
	Registry.RegisterAction(TEXT("mesh"), TEXT("predict_player_paths"),
		TEXT("Generate weighted navmesh paths between two points using multiple strategy heuristics: shortest, safest, curious, cautious. Returns path points, distance, estimated time, and per-strategy scores."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshHorrorDesignActions::PredictPlayerPaths),
		FParamSchemaBuilder()
			.Required(TEXT("start"), TEXT("array"), TEXT("Start position [x, y, z]"))
			.Required(TEXT("end"), TEXT("array"), TEXT("End position [x, y, z]"))
			.Optional(TEXT("strategies"), TEXT("array"), TEXT("Array of strategy names: shortest, safest, curious, cautious"), TEXT(""))
			.Optional(TEXT("agent_radius"), TEXT("number"), TEXT("Navigation agent radius in cm"), TEXT("45"))
			.Optional(TEXT("agent_height"), TEXT("number"), TEXT("Navigation agent height in cm"), TEXT("180"))
			.Optional(TEXT("waypoints"), TEXT("array"), TEXT("Optional intermediate waypoints [[x,y,z], ...]"), TEXT(""))
			.Optional(TEXT("sample_density"), TEXT("number"), TEXT("Grid spacing for safety/curiosity scoring in cm"), TEXT("200"))
			.Optional(TEXT("max_samples"), TEXT("integer"), TEXT("Hard cap on grid samples (max 500)"), TEXT("500"))
			.Optional(TEXT("max_paths_per_strategy"), TEXT("integer"), TEXT("Max alternate paths per strategy"), TEXT("3"))
			.Optional(TEXT("walk_speed_cms"), TEXT("number"), TEXT("Walk speed in cm/s for time estimation"), TEXT("400"))
			.Build());

	// 2. evaluate_spawn_point
	Registry.RegisterAction(TEXT("mesh"), TEXT("evaluate_spawn_point"),
		TEXT("Composite score for an enemy spawn location. Evaluates visibility delay, lighting, audio cover, escape proximity, and path commitment from player paths."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshHorrorDesignActions::EvaluateSpawnPoint),
		FParamSchemaBuilder()
			.Required(TEXT("location"), TEXT("array"), TEXT("Spawn point position [x, y, z]"))
			.Optional(TEXT("player_paths"), TEXT("array"), TEXT("Array of player path positions [[x,y,z], ...] to evaluate against"), TEXT(""))
			.Optional(TEXT("player_location"), TEXT("array"), TEXT("Single player location if no paths provided"), TEXT(""))
			.Optional(TEXT("weights"), TEXT("object"), TEXT("Score weights: { visibility_delay, lighting, audio_cover, escape_proximity, path_commitment }"), TEXT(""))
			.Build());

	// 3. suggest_scare_positions
	Registry.RegisterAction(TEXT("mesh"), TEXT("suggest_scare_positions"),
		TEXT("Find optimal positions for scripted scare events along a player path. Scores anticipation buildup, player visibility, timing, and player agency. Supports hospice mode."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshHorrorDesignActions::SuggestScarePositions),
		FParamSchemaBuilder()
			.Required(TEXT("path_points"), TEXT("array"), TEXT("Player path positions [[x,y,z], ...]"))
			.Optional(TEXT("scare_type"), TEXT("string"), TEXT("Type: audio, visual, entity_spawn, environmental"), TEXT("visual"))
			.Optional(TEXT("count"), TEXT("integer"), TEXT("Number of positions to suggest"), TEXT("5"))
			.Optional(TEXT("min_spacing_cm"), TEXT("number"), TEXT("Minimum distance between scare positions in cm"), TEXT("1000"))
			.Optional(TEXT("intensity_curve"), TEXT("string"), TEXT("Curve: escalating, wave, random"), TEXT("escalating"))
			.Optional(TEXT("hospice_mode"), TEXT("boolean"), TEXT("Cap intensity and ensure adequate spacing for hospice patients"), TEXT("false"))
			.Build());

	// 4. evaluate_encounter_pacing
	Registry.RegisterAction(TEXT("mesh"), TEXT("evaluate_encounter_pacing"),
		TEXT("Analyze spacing and intensity of multiple encounter positions along a level path. Flags back-to-back encounters, insufficient rest periods, and intensity curve issues."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshHorrorDesignActions::EvaluateEncounterPacing),
		FParamSchemaBuilder()
			.Required(TEXT("path_points"), TEXT("array"), TEXT("Level path positions [[x,y,z], ...]"))
			.Required(TEXT("encounters"), TEXT("array"), TEXT("Array of encounter objects: [{ location: [x,y,z], type: string, intensity: 0-1, duration_s: float }]"))
			.Optional(TEXT("target_pacing"), TEXT("string"), TEXT("Pacing profile: horror_standard, hospice_gentle, action"), TEXT("horror_standard"))
			.Optional(TEXT("walk_speed_cms"), TEXT("number"), TEXT("Walk speed in cm/s"), TEXT("400"))
			.Build());
}

// ============================================================================
// 1. predict_player_paths
// ============================================================================

FMonolithActionResult FMonolithMeshHorrorDesignActions::PredictPlayerPaths(const TSharedPtr<FJsonObject>& Params)
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

	// Parse nav system
	UNavigationSystemV1* NavSys = nullptr;
	ANavigationData* NavData = nullptr;
	FString NavError;
	if (!MonolithMeshAnalysis::GetNavSystem(World, NavSys, NavData, NavError))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Navigation not available: %s. Build navmesh first (Build > Build Paths or use build_navmesh action)."), *NavError));
	}

	// Parse strategies
	TArray<FString> Strategies;
	if (!ParseStringArray(Params, TEXT("strategies"), Strategies) || Strategies.Num() == 0)
	{
		Strategies = { TEXT("shortest"), TEXT("safest"), TEXT("curious"), TEXT("cautious") };
	}

	double AgentRadius = 45.0;
	Params->TryGetNumberField(TEXT("agent_radius"), AgentRadius);
	double AgentHeight = 180.0;
	Params->TryGetNumberField(TEXT("agent_height"), AgentHeight);

	double SampleDensity = 200.0;
	Params->TryGetNumberField(TEXT("sample_density"), SampleDensity);
	SampleDensity = FMath::Clamp(SampleDensity, 50.0, 1000.0);

	int32 MaxSamples = 500;
	{
		double MaxSamplesD;
		if (Params->TryGetNumberField(TEXT("max_samples"), MaxSamplesD))
		{
			MaxSamples = FMath::Clamp(static_cast<int32>(MaxSamplesD), 10, 500); // HARD CAP at 500
		}
	}

	int32 MaxPathsPerStrategy = 3;
	{
		double V;
		if (Params->TryGetNumberField(TEXT("max_paths_per_strategy"), V))
		{
			MaxPathsPerStrategy = FMath::Clamp(static_cast<int32>(V), 1, 5);
		}
	}

	double WalkSpeed = 400.0;
	Params->TryGetNumberField(TEXT("walk_speed_cms"), WalkSpeed);
	WalkSpeed = FMath::Clamp(WalkSpeed, 50.0, 2000.0);

	// Parse optional waypoints
	TArray<FVector> Waypoints;
	HDA_ParseVectorArray(Params, TEXT("waypoints"), Waypoints);

	FNavAgentProperties AgentProps;
	AgentProps.AgentRadius = static_cast<float>(AgentRadius);
	AgentProps.AgentHeight = static_cast<float>(AgentHeight);

	// ---- Build a scored grid in the corridor between start and end ----
	// Bounding box of start-end + padding
	FVector CorridorMin = Start.ComponentMin(End);
	FVector CorridorMax = Start.ComponentMax(End);
	// Pad by 30% of the diagonal or at least 500cm
	float Pad = FMath::Max(500.0f, FVector::Dist(Start, End) * 0.3f);
	CorridorMin -= FVector(Pad, Pad, 0.0f);
	CorridorMax += FVector(Pad, Pad, 0.0f);

	FVector GridExtent = CorridorMax - CorridorMin;
	int32 XCount = FMath::Max(1, FMath::CeilToInt(GridExtent.X / SampleDensity));
	int32 YCount = FMath::Max(1, FMath::CeilToInt(GridExtent.Y / SampleDensity));

	// Enforce hard cap
	if (XCount * YCount > MaxSamples)
	{
		float Scale = FMath::Sqrt(static_cast<float>(MaxSamples) / static_cast<float>(XCount * YCount));
		XCount = FMath::Max(1, FMath::FloorToInt(XCount * Scale));
		YCount = FMath::Max(1, FMath::FloorToInt(YCount * Scale));
	}

	// Sample grid cells on navmesh and score them
	TArray<FGridCell> GridCells;
	TMap<FIntVector, int32> GridMap;

	for (int32 X = 0; X < XCount; ++X)
	{
		for (int32 Y = 0; Y < YCount; ++Y)
		{
			FVector TestPt = CorridorMin + FVector(
				(static_cast<float>(X) + 0.5f) * static_cast<float>(SampleDensity),
				(static_cast<float>(Y) + 0.5f) * static_cast<float>(SampleDensity),
				GridExtent.Z * 0.5f);

			FNavLocation NavLoc;
			if (!NavSys->ProjectPointToNavigation(TestPt, NavLoc,
				FVector(static_cast<float>(SampleDensity) * 0.5f, static_cast<float>(SampleDensity) * 0.5f, 500.0f)))
			{
				continue;
			}

			FGridCell Cell;
			Cell.Location = NavLoc.Location;

			// Safety score: sightline distance (higher = safer) + clearance (wider = safer)
			float AvgSightline = QuickAvgSightlineDistance(World, Cell.Location);
			float NormSightline = FMath::Clamp(AvgSightline / 5000.0f, 0.0f, 1.0f);

			FVector DirToEnd = (End - Cell.Location).GetSafeNormal();
			float Clearance = QuickPerpendicularClearance(World, Cell.Location, DirToEnd);
			float NormClearance = FMath::Clamp(Clearance / 500.0f, 0.0f, 1.0f);

			Cell.SafetyScore = NormSightline * 0.6f + NormClearance * 0.4f;

			// Curiosity score: points near dead-end-like areas, low sightlines (hidden), near tagged actors
			// Inverse of safety — hidden, enclosed areas are "interesting"
			Cell.CuriosityScore = FMath::Clamp(1.0f - NormSightline, 0.0f, 1.0f) * 0.7f
				+ FMath::Clamp(1.0f - NormClearance, 0.0f, 1.0f) * 0.3f;

			// Caution score: prefer positions near walls (min wall distance small) + wide clearance ahead
			float WallDist = MinWallDistance(World, Cell.Location);
			float NormWallProximity = FMath::Clamp(1.0f - (WallDist / 200.0f), 0.0f, 1.0f); // Close to wall = high
			Cell.CautionScore = NormWallProximity * 0.5f + NormSightline * 0.3f + NormClearance * 0.2f;

			int32 Idx = GridCells.Num();
			GridCells.Add(Cell);
			GridMap.Add(FIntVector(X, Y, 0), Idx);
		}
	}

	// ---- Generate paths per strategy ----
	struct FStrategyPath
	{
		FString Strategy;
		TArray<FVector> Points;
		float Distance = 0.0f;
		float EstimatedTimeSeconds = 0.0f;
		float SafetyScore = 0.0f;
		float VisibilityScore = 0.0f;
	};

	TArray<FStrategyPath> AllPaths;

	auto BuildWaypointPath = [&](const TArray<FVector>& Via) -> bool
	{
		// Builds a path from Start -> Via[0] -> Via[1] -> ... -> End
		TArray<FVector> FullPath;
		FVector Prev = Start;
		float TotalDist = 0.0f;

		for (int32 i = 0; i <= Via.Num(); ++i)
		{
			FVector Next = (i < Via.Num()) ? Via[i] : End;
			TArray<FVector> Seg;
			float SegDist;
			if (!MonolithMeshAnalysis::FindNavPath(World, Prev, Next, Seg, SegDist, static_cast<float>(AgentRadius)))
			{
				return false;
			}
			// Avoid duplicating the junction point
			int32 StartIdx = (FullPath.Num() > 0 && Seg.Num() > 0) ? 1 : 0;
			for (int32 j = StartIdx; j < Seg.Num(); ++j)
			{
				FullPath.Add(Seg[j]);
			}
			TotalDist += SegDist;
			Prev = Next;
		}

		if (FullPath.Num() < 2)
		{
			return false;
		}

		FStrategyPath SP;
		SP.Points = MoveTemp(FullPath);
		SP.Distance = TotalDist;
		SP.EstimatedTimeSeconds = TotalDist / static_cast<float>(WalkSpeed);

		// Compute scores along the generated path
		float SafetySum = 0.0f;
		float VisibilitySum = 0.0f;
		int32 SampleCount = FMath::Min(SP.Points.Num(), 20); // cap per-path scoring
		int32 Step = FMath::Max(1, SP.Points.Num() / SampleCount);

		for (int32 i = 0; i < SP.Points.Num(); i += Step)
		{
			float Sightline = QuickAvgSightlineDistance(World, SP.Points[i]);
			SafetySum += FMath::Clamp(Sightline / 5000.0f, 0.0f, 1.0f);
			// Visibility = how exposed the point is (inverse of concealment from center)
			VisibilitySum += FMath::Clamp(Sightline / 3000.0f, 0.0f, 1.0f);
		}

		int32 ActualSamples = (SP.Points.Num() + Step - 1) / Step;
		SP.SafetyScore = ActualSamples > 0 ? SafetySum / static_cast<float>(ActualSamples) : 0.0f;
		SP.VisibilityScore = ActualSamples > 0 ? VisibilitySum / static_cast<float>(ActualSamples) : 0.0f;

		AllPaths.Add(MoveTemp(SP));
		return true;
	};

	for (const FString& Strategy : Strategies)
	{
		if (Strategy.Equals(TEXT("shortest"), ESearchCase::IgnoreCase))
		{
			// Direct navmesh path, optionally through waypoints
			TArray<FVector> Via = Waypoints;
			FStrategyPath SP;
			TArray<FVector> DirectPath;
			float DirectDist;

			if (Via.Num() > 0)
			{
				BuildWaypointPath(Via);
			}
			else if (MonolithMeshAnalysis::FindNavPath(World, Start, End, DirectPath, DirectDist, static_cast<float>(AgentRadius)))
			{
				SP.Points = MoveTemp(DirectPath);
				SP.Distance = DirectDist;
				SP.EstimatedTimeSeconds = DirectDist / static_cast<float>(WalkSpeed);

				// Score
				float SafetySum = 0.0f;
				float VisSum = 0.0f;
				int32 Step = FMath::Max(1, SP.Points.Num() / 20);
				int32 Counted = 0;
				for (int32 i = 0; i < SP.Points.Num(); i += Step)
				{
					float S = QuickAvgSightlineDistance(World, SP.Points[i]);
					SafetySum += FMath::Clamp(S / 5000.0f, 0.0f, 1.0f);
					VisSum += FMath::Clamp(S / 3000.0f, 0.0f, 1.0f);
					++Counted;
				}
				SP.SafetyScore = Counted > 0 ? SafetySum / static_cast<float>(Counted) : 0.0f;
				SP.VisibilityScore = Counted > 0 ? VisSum / static_cast<float>(Counted) : 0.0f;

				SP.Strategy = TEXT("shortest");
				AllPaths.Add(MoveTemp(SP));
			}
			// Tag the strategy name
			if (AllPaths.Num() > 0 && AllPaths.Last().Strategy.IsEmpty())
			{
				AllPaths.Last().Strategy = TEXT("shortest");
			}
		}
		else if (Strategy.Equals(TEXT("safest"), ESearchCase::IgnoreCase))
		{
			// Route through the highest-safety grid cells as waypoints
			if (GridCells.Num() > 0)
			{
				// Sort cells by safety descending, pick top N as waypoint candidates
				TArray<int32> Indices;
				Indices.SetNum(GridCells.Num());
				for (int32 i = 0; i < GridCells.Num(); ++i) Indices[i] = i;
				Indices.Sort([&](int32 A, int32 B) { return GridCells[A].SafetyScore > GridCells[B].SafetyScore; });

				// Pick 1-3 waypoints that are roughly between start and end
				FVector PathDir = (End - Start).GetSafeNormal();
				float PathLen = FVector::Dist(Start, End);

				TArray<FVector> SafeWaypoints = Waypoints;
				int32 WaypointBudget = FMath::Max(1, MaxPathsPerStrategy);

				for (int32 i = 0; i < Indices.Num() && SafeWaypoints.Num() < WaypointBudget + Waypoints.Num(); ++i)
				{
					const FGridCell& Cell = GridCells[Indices[i]];
					// Must be between start and end (project onto path axis)
					float Proj = FVector::DotProduct(Cell.Location - Start, PathDir);
					if (Proj < PathLen * 0.1f || Proj > PathLen * 0.9f) continue;

					// Must not be too far from the direct path axis
					FVector Closest = Start + PathDir * Proj;
					float LateralDist = FVector::Dist(Cell.Location, Closest);
					if (LateralDist > Pad * 0.6f) continue;

					// Check spacing from existing waypoints
					bool bTooClose = false;
					for (const FVector& Existing : SafeWaypoints)
					{
						if (FVector::Dist(Cell.Location, Existing) < static_cast<float>(SampleDensity) * 2.0f)
						{
							bTooClose = true;
							break;
						}
					}
					if (bTooClose) continue;

					SafeWaypoints.Add(Cell.Location);
				}

				// Sort waypoints by projection along path direction
				SafeWaypoints.Sort([&](const FVector& A, const FVector& B)
				{
					return FVector::DotProduct(A - Start, PathDir) < FVector::DotProduct(B - Start, PathDir);
				});

				BuildWaypointPath(SafeWaypoints);
				if (AllPaths.Num() > 0 && AllPaths.Last().Strategy.IsEmpty())
				{
					AllPaths.Last().Strategy = TEXT("safest");
				}
			}
		}
		else if (Strategy.Equals(TEXT("curious"), ESearchCase::IgnoreCase))
		{
			// Route through the highest-curiosity grid cells (hidden, enclosed areas)
			if (GridCells.Num() > 0)
			{
				TArray<int32> Indices;
				Indices.SetNum(GridCells.Num());
				for (int32 i = 0; i < GridCells.Num(); ++i) Indices[i] = i;
				Indices.Sort([&](int32 A, int32 B) { return GridCells[A].CuriosityScore > GridCells[B].CuriosityScore; });

				FVector PathDir = (End - Start).GetSafeNormal();
				float PathLen = FVector::Dist(Start, End);

				TArray<FVector> CuriousWaypoints = Waypoints;
				int32 WaypointBudget = FMath::Max(1, MaxPathsPerStrategy);

				for (int32 i = 0; i < Indices.Num() && CuriousWaypoints.Num() < WaypointBudget + Waypoints.Num(); ++i)
				{
					const FGridCell& Cell = GridCells[Indices[i]];
					float Proj = FVector::DotProduct(Cell.Location - Start, PathDir);
					if (Proj < PathLen * 0.05f || Proj > PathLen * 0.95f) continue;

					// Curious paths are allowed to deviate further
					FVector Closest = Start + PathDir * Proj;
					float LateralDist = FVector::Dist(Cell.Location, Closest);
					if (LateralDist > Pad * 0.8f) continue;

					bool bTooClose = false;
					for (const FVector& Existing : CuriousWaypoints)
					{
						if (FVector::Dist(Cell.Location, Existing) < static_cast<float>(SampleDensity) * 2.0f)
						{
							bTooClose = true;
							break;
						}
					}
					if (bTooClose) continue;

					CuriousWaypoints.Add(Cell.Location);
				}

				CuriousWaypoints.Sort([&](const FVector& A, const FVector& B)
				{
					return FVector::DotProduct(A - Start, (End - Start).GetSafeNormal())
						< FVector::DotProduct(B - Start, (End - Start).GetSafeNormal());
				});

				BuildWaypointPath(CuriousWaypoints);
				if (AllPaths.Num() > 0 && AllPaths.Last().Strategy.IsEmpty())
				{
					AllPaths.Last().Strategy = TEXT("curious");
				}
			}
		}
		else if (Strategy.Equals(TEXT("cautious"), ESearchCase::IgnoreCase))
		{
			// Route through cells that hug walls but still have good sightlines ahead
			if (GridCells.Num() > 0)
			{
				TArray<int32> Indices;
				Indices.SetNum(GridCells.Num());
				for (int32 i = 0; i < GridCells.Num(); ++i) Indices[i] = i;
				Indices.Sort([&](int32 A, int32 B) { return GridCells[A].CautionScore > GridCells[B].CautionScore; });

				FVector PathDir = (End - Start).GetSafeNormal();
				float PathLen = FVector::Dist(Start, End);

				TArray<FVector> CautiousWaypoints = Waypoints;
				int32 WaypointBudget = FMath::Max(1, MaxPathsPerStrategy);

				for (int32 i = 0; i < Indices.Num() && CautiousWaypoints.Num() < WaypointBudget + Waypoints.Num(); ++i)
				{
					const FGridCell& Cell = GridCells[Indices[i]];
					float Proj = FVector::DotProduct(Cell.Location - Start, PathDir);
					if (Proj < PathLen * 0.1f || Proj > PathLen * 0.9f) continue;

					FVector Closest = Start + PathDir * Proj;
					float LateralDist = FVector::Dist(Cell.Location, Closest);
					if (LateralDist > Pad * 0.7f) continue;

					bool bTooClose = false;
					for (const FVector& Existing : CautiousWaypoints)
					{
						if (FVector::Dist(Cell.Location, Existing) < static_cast<float>(SampleDensity) * 2.0f)
						{
							bTooClose = true;
							break;
						}
					}
					if (bTooClose) continue;

					CautiousWaypoints.Add(Cell.Location);
				}

				CautiousWaypoints.Sort([&](const FVector& A, const FVector& B)
				{
					return FVector::DotProduct(A - Start, (End - Start).GetSafeNormal())
						< FVector::DotProduct(B - Start, (End - Start).GetSafeNormal());
				});

				BuildWaypointPath(CautiousWaypoints);
				if (AllPaths.Num() > 0 && AllPaths.Last().Strategy.IsEmpty())
				{
					AllPaths.Last().Strategy = TEXT("cautious");
				}
			}
		}
	}

	// Build JSON result
	TArray<TSharedPtr<FJsonValue>> PathsArr;
	for (const FStrategyPath& SP : AllPaths)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("strategy"), SP.Strategy);

		TArray<TSharedPtr<FJsonValue>> PtsArr;
		for (const FVector& Pt : SP.Points)
		{
			PtsArr.Add(MakeShared<FJsonValueArray>(HDA_VecToArr(Pt)));
		}
		Obj->SetArrayField(TEXT("points"), PtsArr);
		Obj->SetNumberField(TEXT("total_distance"), SP.Distance);
		Obj->SetNumberField(TEXT("estimated_time_seconds"), SP.EstimatedTimeSeconds);
		Obj->SetNumberField(TEXT("safety_score"), SP.SafetyScore);
		Obj->SetNumberField(TEXT("visibility_score"), SP.VisibilityScore);
		Obj->SetNumberField(TEXT("point_count"), SP.Points.Num());

		PathsArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("navmesh_available"), true);
	Result->SetArrayField(TEXT("paths"), PathsArr);
	Result->SetNumberField(TEXT("strategies_requested"), Strategies.Num());
	Result->SetNumberField(TEXT("paths_generated"), AllPaths.Num());
	Result->SetNumberField(TEXT("grid_cells_scored"), GridCells.Num());
	Result->SetNumberField(TEXT("max_samples_cap"), MaxSamples);

	if (AllPaths.Num() == 0)
	{
		Result->SetStringField(TEXT("warning"), TEXT("No paths could be generated. Verify navmesh connectivity between start and end."));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 2. evaluate_spawn_point
// ============================================================================

FMonolithActionResult FMonolithMeshHorrorDesignActions::EvaluateSpawnPoint(const TSharedPtr<FJsonObject>& Params)
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

	// Parse player paths or single player location
	TArray<FVector> PlayerPath;
	if (!HDA_ParseVectorArray(Params, TEXT("player_paths"), PlayerPath) || PlayerPath.Num() == 0)
	{
		FVector PlayerLoc;
		if (MonolithMeshUtils::ParseVector(Params, TEXT("player_location"), PlayerLoc))
		{
			PlayerPath.Add(PlayerLoc);
		}
	}

	// Parse weights
	float WVisibilityDelay = 1.0f;
	float WLighting = 1.0f;
	float WAudioCover = 0.5f;
	float WEscapeProximity = 1.5f;
	float WPathCommitment = 1.0f;

	const TSharedPtr<FJsonObject>* WeightsObj;
	if (Params->TryGetObjectField(TEXT("weights"), WeightsObj))
	{
		double V;
		if ((*WeightsObj)->TryGetNumberField(TEXT("visibility_delay"), V)) WVisibilityDelay = static_cast<float>(V);
		if ((*WeightsObj)->TryGetNumberField(TEXT("lighting"), V)) WLighting = static_cast<float>(V);
		if ((*WeightsObj)->TryGetNumberField(TEXT("audio_cover"), V)) WAudioCover = static_cast<float>(V);
		if ((*WeightsObj)->TryGetNumberField(TEXT("escape_proximity"), V)) WEscapeProximity = static_cast<float>(V);
		if ((*WeightsObj)->TryGetNumberField(TEXT("path_commitment"), V)) WPathCommitment = static_cast<float>(V);
	}

	float TotalWeight = WVisibilityDelay + WLighting + WAudioCover + WEscapeProximity + WPathCommitment;
	if (TotalWeight <= 0.0f) TotalWeight = 1.0f;

	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(MonolithSpawnEval), true);

	// ---- Visibility Delay Score (0-1): how long before the player can see the spawn point ----
	// Measured as concealment from player path points
	float VisibilityDelayScore = 0.0f;
	if (PlayerPath.Num() > 0)
	{
		float Concealment = MonolithMeshAnalysis::ComputeConcealment(World, Location, PlayerPath);
		VisibilityDelayScore = Concealment; // 1 = fully hidden until close, 0 = immediately visible
	}
	else
	{
		// No player path — use sightline analysis from spawn point
		float AvgSightline = QuickAvgSightlineDistance(World, Location);
		VisibilityDelayScore = FMath::Clamp(1.0f - (AvgSightline / 3000.0f), 0.0f, 1.0f);
	}

	// ---- Lighting Score (0-1): darker = better for spawn ----
	float LightingScore = 0.0f;
	{
		// Use analytic luminance (no scene capture dependency)
		TArray<MonolithLightingCapture::FLightInfo> Lights = MonolithLightingCapture::GatherLights(World);
		int32 DominantIdx = -1;
		float Luminance = MonolithLightingCapture::ComputeAnalyticLuminance(World, Location, Lights, DominantIdx);
		// Lower luminance = better spawn. Normalize: 0 lux -> 1.0, 5+ lux -> 0.0
		LightingScore = FMath::Clamp(1.0f - (Luminance / 5.0f), 0.0f, 1.0f);
	}

	// ---- Audio Cover Score (0-1): how much ambient occlusion dampens player hearing ----
	float AudioCoverScore = 0.0f;
	if (PlayerPath.Num() > 0)
	{
		// Check audio occlusion from the spawn point to the nearest player path point
		FVector NearestPlayerPt = PlayerPath[0];
		float MinDist = FVector::Dist(Location, PlayerPath[0]);
		for (int32 i = 1; i < PlayerPath.Num(); ++i)
		{
			float D = FVector::Dist(Location, PlayerPath[i]);
			if (D < MinDist)
			{
				MinDist = D;
				NearestPlayerPt = PlayerPath[i];
			}
		}

		int32 WallCount = 0;
		float TotalLossdB = 0.0f;
		float OcclusionFactor = MonolithMeshAcoustics::TraceOcclusion(World, Location, NearestPlayerPt, WallCount, TotalLossdB);
		// More occlusion = better audio cover for spawn
		AudioCoverScore = FMath::Clamp(1.0f - OcclusionFactor, 0.0f, 1.0f);
	}

	// ---- Escape Proximity Score (0-1): how close is the player to an escape route ----
	// Lower escape options near spawn = better for horror (but flags accessibility concern)
	float EscapeProximityScore = 0.0f;
	int32 NearbyExitCount = 0;
	{
		// Count exits from the spawn area (from player perspective — fewer exits near encounter = scarier)
		NearbyExitCount = MonolithMeshAnalysis::CountExits(World, Location, 2000.0f);
		// 0 exits = 1.0 (scariest), 5+ exits = 0.0 (safest)
		EscapeProximityScore = FMath::Clamp(1.0f - (static_cast<float>(NearbyExitCount) / 5.0f), 0.0f, 1.0f);
	}

	// ---- Path Commitment Score (0-1): how committed is the player when they reach the spawn ----
	// Measured by corridor narrowness and turn count
	float PathCommitmentScore = 0.0f;
	if (PlayerPath.Num() >= 2)
	{
		// Measure clearance at points nearest to spawn
		TArray<MonolithMeshAnalysis::FPathClearance> Clearances = MonolithMeshAnalysis::MeasurePathClearance(World, PlayerPath, 500.0f);

		// Find clearance at the point nearest to spawn
		float MinPathWidth = 1000.0f;
		for (const auto& C : Clearances)
		{
			if (FVector::Dist(C.Location, Location) < 800.0f)
			{
				MinPathWidth = FMath::Min(MinPathWidth, C.TotalWidth);
			}
		}
		// Narrow path near spawn = high commitment. 100cm or less = 1.0, 500cm+ = 0.0
		PathCommitmentScore = FMath::Clamp(1.0f - ((MinPathWidth - 100.0f) / 400.0f), 0.0f, 1.0f);
	}

	// ---- Composite ----
	float WeightedScore = (
		VisibilityDelayScore * WVisibilityDelay +
		LightingScore * WLighting +
		AudioCoverScore * WAudioCover +
		EscapeProximityScore * WEscapeProximity +
		PathCommitmentScore * WPathCommitment
	) / TotalWeight;

	// Grade
	FString Grade;
	if (WeightedScore >= 0.8f) Grade = TEXT("S");
	else if (WeightedScore >= 0.65f) Grade = TEXT("A");
	else if (WeightedScore >= 0.5f) Grade = TEXT("B");
	else if (WeightedScore >= 0.35f) Grade = TEXT("C");
	else if (WeightedScore >= 0.2f) Grade = TEXT("D");
	else Grade = TEXT("F");

	// Suggestions
	TArray<TSharedPtr<FJsonValue>> Suggestions;
	if (VisibilityDelayScore < 0.3f)
	{
		Suggestions.Add(MakeShared<FJsonValueString>(TEXT("Spawn is too exposed — add visual occluders (pillars, walls, furniture) between spawn and player approach.")));
	}
	if (LightingScore < 0.3f)
	{
		Suggestions.Add(MakeShared<FJsonValueString>(TEXT("Spawn area is too bright — reduce nearby light intensity or add shadow-casting geometry.")));
	}
	if (AudioCoverScore < 0.3f)
	{
		Suggestions.Add(MakeShared<FJsonValueString>(TEXT("Poor audio cover — ambient sound or wall occlusion between spawn and player path would mask movement sounds.")));
	}
	if (NearbyExitCount >= 4)
	{
		Suggestions.Add(MakeShared<FJsonValueString>(TEXT("Many escape routes near spawn reduce scare impact — consider narrowing approach paths.")));
	}
	if (NearbyExitCount == 0)
	{
		Suggestions.Add(MakeShared<FJsonValueString>(TEXT("ACCESSIBILITY WARNING: No exits detected near spawn point. Hospice patients need a viable escape route.")));
	}
	if (PathCommitmentScore < 0.2f)
	{
		Suggestions.Add(MakeShared<FJsonValueString>(TEXT("Player is not committed when near this spawn — wide open area allows easy retreat. Consider using tighter corridors.")));
	}

	// Build result
	auto Breakdown = MakeShared<FJsonObject>();
	Breakdown->SetNumberField(TEXT("visibility_delay"), VisibilityDelayScore);
	Breakdown->SetNumberField(TEXT("lighting"), LightingScore);
	Breakdown->SetNumberField(TEXT("audio_cover"), AudioCoverScore);
	Breakdown->SetNumberField(TEXT("escape_proximity"), EscapeProximityScore);
	Breakdown->SetNumberField(TEXT("path_commitment"), PathCommitmentScore);
	Breakdown->SetNumberField(TEXT("nearby_exit_count"), NearbyExitCount);

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("score"), WeightedScore);
	Result->SetStringField(TEXT("grade"), Grade);
	Result->SetObjectField(TEXT("breakdown"), Breakdown);
	Result->SetArrayField(TEXT("suggestions"), Suggestions);
	{
		UNavigationSystemV1* NavSys = nullptr;
		ANavigationData* NavData = nullptr;
		FString NavErr;
		Result->SetBoolField(TEXT("navmesh_available"), MonolithMeshAnalysis::GetNavSystem(World, NavSys, NavData, NavErr));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 3. suggest_scare_positions
// ============================================================================

FMonolithActionResult FMonolithMeshHorrorDesignActions::SuggestScarePositions(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FVector> PathPoints;
	if (!HDA_ParseVectorArray(Params, TEXT("path_points"), PathPoints) || PathPoints.Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: path_points (array of at least 2 [x,y,z])"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString ScareType = TEXT("visual");
	Params->TryGetStringField(TEXT("scare_type"), ScareType);

	int32 Count = 5;
	{
		double V;
		if (Params->TryGetNumberField(TEXT("count"), V))
		{
			Count = FMath::Clamp(static_cast<int32>(V), 1, 30);
		}
	}

	double MinSpacing = 1000.0;
	Params->TryGetNumberField(TEXT("min_spacing_cm"), MinSpacing);
	MinSpacing = FMath::Clamp(MinSpacing, 100.0, 50000.0);

	FString IntensityCurve = TEXT("escalating");
	Params->TryGetStringField(TEXT("intensity_curve"), IntensityCurve);

	bool bHospiceMode = false;
	Params->TryGetBoolField(TEXT("hospice_mode"), bHospiceMode);

	if (bHospiceMode)
	{
		// Enforce minimum spacing and cap count for hospice patients
		MinSpacing = FMath::Max(MinSpacing, 2000.0);
		Count = FMath::Min(Count, 3);
	}

	// Resample path at a reasonable interval for evaluation
	float EvalInterval = static_cast<float>(MinSpacing) * 0.25f;
	EvalInterval = FMath::Clamp(EvalInterval, 100.0f, 500.0f);
	TArray<FVector> SamplePoints = ResamplePath(PathPoints, EvalInterval);

	// Cap sample count
	if (SamplePoints.Num() > 300)
	{
		int32 Step = (SamplePoints.Num() + 299) / 300;
		TArray<FVector> Thinned;
		for (int32 i = 0; i < SamplePoints.Num(); i += Step)
		{
			Thinned.Add(SamplePoints[i]);
		}
		SamplePoints = MoveTemp(Thinned);
	}

	// Score each sample point as a scare candidate
	struct FScareCandidate
	{
		FVector Location;
		float Score = 0.0f;
		float DistanceAlongPath = 0.0f;
		float TensionContext = 0.0f;
		float VisibilityFromPath = 0.0f;
		int32 EscapeOptions = 0;
		FVector SuggestedDirection = FVector::ZeroVector; // Direction scare should come from
	};

	TArray<FScareCandidate> Candidates;
	float CumulativeDist = 0.0f;
	float TotalPathLen = PathLength(SamplePoints);

	// Gather lights once for reuse
	TArray<MonolithLightingCapture::FLightInfo> Lights = MonolithLightingCapture::GatherLights(World);

	for (int32 i = 0; i < SamplePoints.Num(); ++i)
	{
		if (i > 0)
		{
			CumulativeDist += FVector::Dist(SamplePoints[i - 1], SamplePoints[i]);
		}

		FScareCandidate Cand;
		Cand.Location = SamplePoints[i];
		Cand.DistanceAlongPath = CumulativeDist;

		// Tension context (0-100)
		Cand.TensionContext = QuickTensionScore(World, SamplePoints[i]);

		// Visibility from nearby path — lower visibility = better scare potential
		FVector Forward = FVector::ForwardVector;
		if (i < SamplePoints.Num() - 1)
		{
			Forward = (SamplePoints[i + 1] - SamplePoints[i]).GetSafeNormal();
		}
		else if (i > 0)
		{
			Forward = (SamplePoints[i] - SamplePoints[i - 1]).GetSafeNormal();
		}

		float Clearance = QuickPerpendicularClearance(World, SamplePoints[i], Forward);
		Cand.VisibilityFromPath = Clearance;

		// Escape options
		Cand.EscapeOptions = MonolithMeshAnalysis::CountExits(World, SamplePoints[i], 1500.0f, 6);

		// Suggested direction: perpendicular to path, preferring the side with more concealment
		FVector Right = FVector::CrossProduct(Forward, FVector::UpVector).GetSafeNormal();
		if (Right.IsNearlyZero()) Right = FVector::RightVector;

		float RightConc = MonolithMeshAnalysis::ComputeConcealment(World,
			SamplePoints[i] + Right * 200.0f, { SamplePoints[i] });
		float LeftConc = MonolithMeshAnalysis::ComputeConcealment(World,
			SamplePoints[i] - Right * 200.0f, { SamplePoints[i] });
		Cand.SuggestedDirection = (RightConc > LeftConc) ? Right : -Right;

		// Composite score:
		// - Tension context contributes positively (scarier area = better)
		// - Path position bonus for anticipation buildup (further from start with history of calm = better)
		// - Low visibility nearby = good (enclosed)
		// - At least 1 escape option = critical for hospice
		float TensionFactor = FMath::Clamp(Cand.TensionContext / 100.0f, 0.0f, 1.0f);
		float EnclosedFactor = FMath::Clamp(1.0f - (Clearance / 600.0f), 0.0f, 1.0f);
		float EscapeFactor = (Cand.EscapeOptions >= 1) ? 1.0f : 0.3f; // Penalize no-escape heavily in score
		float LightFactor = 0.5f; // Default neutral
		{
			int32 DummyIdx;
			float Lum = MonolithLightingCapture::ComputeAnalyticLuminance(World, SamplePoints[i], Lights, DummyIdx);
			LightFactor = FMath::Clamp(1.0f - (Lum / 5.0f), 0.0f, 1.0f); // Darker = better
		}

		Cand.Score = TensionFactor * 0.3f + EnclosedFactor * 0.2f + LightFactor * 0.2f + EscapeFactor * 0.15f;

		// Position along path factor depends on intensity curve
		float PathProgress = (TotalPathLen > 0.0f) ? (CumulativeDist / TotalPathLen) : 0.0f;
		if (IntensityCurve.Equals(TEXT("escalating"), ESearchCase::IgnoreCase))
		{
			Cand.Score += PathProgress * 0.15f;
		}
		else if (IntensityCurve.Equals(TEXT("wave"), ESearchCase::IgnoreCase))
		{
			// Sin wave — peaks at ~0.3 and ~0.8 of path
			float WaveVal = FMath::Abs(FMath::Sin(PathProgress * PI * 2.0f));
			Cand.Score += WaveVal * 0.15f;
		}
		else // random or default
		{
			Cand.Score += 0.075f; // Flat
		}

		if (bHospiceMode)
		{
			// Cap maximum tension for hospice patients
			if (Cand.TensionContext > 70.0f)
			{
				Cand.Score *= 0.5f; // Significantly reduce very tense positions
			}
			if (Cand.EscapeOptions < 2)
			{
				Cand.Score *= 0.3f; // Strongly penalize limited escape
			}
		}

		Candidates.Add(MoveTemp(Cand));
	}

	// Sort all candidates by score descending
	Candidates.Sort([](const FScareCandidate& A, const FScareCandidate& B)
	{
		return A.Score > B.Score;
	});

	// Greedy selection with spacing enforcement
	TArray<FScareCandidate> Selected;
	for (const FScareCandidate& Cand : Candidates)
	{
		if (Selected.Num() >= Count) break;

		bool bTooClose = false;
		for (const FScareCandidate& Sel : Selected)
		{
			float PathDist = FMath::Abs(Cand.DistanceAlongPath - Sel.DistanceAlongPath);
			if (PathDist < static_cast<float>(MinSpacing))
			{
				bTooClose = true;
				break;
			}
		}
		if (bTooClose) continue;

		Selected.Add(Cand);
	}

	// Sort selected by path distance for natural ordering
	Selected.Sort([](const FScareCandidate& A, const FScareCandidate& B)
	{
		return A.DistanceAlongPath < B.DistanceAlongPath;
	});

	// Build result
	TArray<TSharedPtr<FJsonValue>> PosArr;
	for (const FScareCandidate& S : Selected)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetArrayField(TEXT("location"), HDA_VecToArr(S.Location));
		Obj->SetNumberField(TEXT("score"), S.Score);
		Obj->SetNumberField(TEXT("path_distance_cm"), S.DistanceAlongPath);
		Obj->SetNumberField(TEXT("visibility_from_path_cm"), S.VisibilityFromPath);
		Obj->SetNumberField(TEXT("tension_context"), FMath::RoundToInt(S.TensionContext));
		Obj->SetStringField(TEXT("tension_level"),
			MonolithMeshAnalysis::TensionLevelToString(MonolithMeshAnalysis::ClassifyTension(S.TensionContext)));
		Obj->SetNumberField(TEXT("escape_options"), S.EscapeOptions);
		Obj->SetArrayField(TEXT("suggested_direction"), HDA_VecToArr(S.SuggestedDirection));
		PosArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("positions_found"), Selected.Num());
	Result->SetArrayField(TEXT("positions"), PosArr);
	Result->SetStringField(TEXT("scare_type"), ScareType);
	Result->SetStringField(TEXT("intensity_curve"), IntensityCurve);
	Result->SetNumberField(TEXT("min_spacing_cm"), MinSpacing);
	Result->SetBoolField(TEXT("hospice_mode"), bHospiceMode);
	Result->SetNumberField(TEXT("candidates_evaluated"), Candidates.Num());
	Result->SetNumberField(TEXT("total_path_length"), TotalPathLen);

	if (Selected.Num() < Count)
	{
		Result->SetStringField(TEXT("warning"), FString::Printf(
			TEXT("Only %d of %d requested positions could be placed with min_spacing_cm=%.0f along a %.0f cm path."),
			Selected.Num(), Count, MinSpacing, TotalPathLen));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 4. evaluate_encounter_pacing
// ============================================================================

FMonolithActionResult FMonolithMeshHorrorDesignActions::EvaluateEncounterPacing(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FVector> PathPoints;
	if (!HDA_ParseVectorArray(Params, TEXT("path_points"), PathPoints) || PathPoints.Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: path_points (array of at least 2 [x,y,z])"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Parse encounters
	struct FEncounter
	{
		FVector Location;
		FString Type;
		float Intensity = 0.5f; // 0-1
		float DurationS = 5.0f;
		float DistanceAlongPath = 0.0f; // Computed
	};

	TArray<FEncounter> Encounters;
	{
		const TArray<TSharedPtr<FJsonValue>>* EncArr;
		if (!Params->TryGetArrayField(TEXT("encounters"), EncArr) || EncArr->Num() == 0)
		{
			return FMonolithActionResult::Error(TEXT("Missing or invalid required param: encounters (array of encounter objects)"));
		}

		for (const TSharedPtr<FJsonValue>& Val : *EncArr)
		{
			const TSharedPtr<FJsonObject>* EncObj;
			if (!Val->TryGetObject(EncObj)) continue;

			FEncounter Enc;
			const TArray<TSharedPtr<FJsonValue>>* LocArr;
			if ((*EncObj)->TryGetArrayField(TEXT("location"), LocArr) && LocArr->Num() >= 3)
			{
				Enc.Location.X = (*LocArr)[0]->AsNumber();
				Enc.Location.Y = (*LocArr)[1]->AsNumber();
				Enc.Location.Z = (*LocArr)[2]->AsNumber();
			}
			else
			{
				continue; // Skip encounters without location
			}

			(*EncObj)->TryGetStringField(TEXT("type"), Enc.Type);

			double V;
			if ((*EncObj)->TryGetNumberField(TEXT("intensity"), V))
			{
				Enc.Intensity = FMath::Clamp(static_cast<float>(V), 0.0f, 1.0f);
			}
			if ((*EncObj)->TryGetNumberField(TEXT("duration_s"), V))
			{
				Enc.DurationS = FMath::Clamp(static_cast<float>(V), 0.0f, 300.0f);
			}

			Encounters.Add(MoveTemp(Enc));
		}
	}

	if (Encounters.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid encounters parsed from the encounters array."));
	}

	FString TargetPacing = TEXT("horror_standard");
	Params->TryGetStringField(TEXT("target_pacing"), TargetPacing);

	double WalkSpeed = 400.0;
	Params->TryGetNumberField(TEXT("walk_speed_cms"), WalkSpeed);
	WalkSpeed = FMath::Clamp(WalkSpeed, 50.0, 2000.0);

	// Pacing profile thresholds
	float MinGapSeconds = 15.0f;    // Minimum gap between encounters
	float IdealGapSeconds = 30.0f;  // Ideal gap
	float MaxIntensity = 1.0f;      // Max allowed intensity
	float MinRestAfterHigh = 20.0f; // Rest required after high-intensity encounter

	if (TargetPacing.Equals(TEXT("hospice_gentle"), ESearchCase::IgnoreCase))
	{
		MinGapSeconds = 30.0f;
		IdealGapSeconds = 60.0f;
		MaxIntensity = 0.5f;
		MinRestAfterHigh = 45.0f;
	}
	else if (TargetPacing.Equals(TEXT("action"), ESearchCase::IgnoreCase))
	{
		MinGapSeconds = 5.0f;
		IdealGapSeconds = 15.0f;
		MaxIntensity = 1.0f;
		MinRestAfterHigh = 10.0f;
	}

	// Compute distance along path for each encounter (project to nearest path point)
	float TotalPathLen = PathLength(PathPoints);

	for (FEncounter& Enc : Encounters)
	{
		float BestDist = TNumericLimits<float>::Max();
		float AccumDist = 0.0f;
		float BestAccum = 0.0f;

		for (int32 i = 0; i < PathPoints.Num(); ++i)
		{
			if (i > 0) AccumDist += FVector::Dist(PathPoints[i - 1], PathPoints[i]);

			float D = FVector::Dist(Enc.Location, PathPoints[i]);
			if (D < BestDist)
			{
				BestDist = D;
				BestAccum = AccumDist;
			}
		}
		Enc.DistanceAlongPath = BestAccum;
	}

	// Sort encounters by path distance
	Encounters.Sort([](const FEncounter& A, const FEncounter& B)
	{
		return A.DistanceAlongPath < B.DistanceAlongPath;
	});

	// Analyze pacing issues
	struct FPacingIssue
	{
		FString Type;
		int32 EncounterA = -1;
		int32 EncounterB = -1;
		float GapSeconds = 0.0f;
		float RecommendedMin = 0.0f;
		FString Description;
	};

	TArray<FPacingIssue> Issues;

	for (int32 i = 1; i < Encounters.Num(); ++i)
	{
		float GapDist = Encounters[i].DistanceAlongPath - Encounters[i - 1].DistanceAlongPath;
		float GapSeconds = GapDist / static_cast<float>(WalkSpeed);

		// Account for previous encounter duration
		float EffectiveGap = GapSeconds - Encounters[i - 1].DurationS;

		if (EffectiveGap < MinGapSeconds)
		{
			FPacingIssue Issue;
			Issue.Type = TEXT("insufficient_gap");
			Issue.EncounterA = i - 1;
			Issue.EncounterB = i;
			Issue.GapSeconds = EffectiveGap;
			Issue.RecommendedMin = MinGapSeconds;
			Issue.Description = FString::Printf(
				TEXT("Only %.1fs between encounter %d (%s) and %d (%s). Minimum recommended: %.1fs."),
				EffectiveGap, i - 1, *Encounters[i - 1].Type, i, *Encounters[i].Type, MinGapSeconds);
			Issues.Add(MoveTemp(Issue));
		}

		// Check for high-intensity followed by insufficient rest
		if (Encounters[i - 1].Intensity > 0.7f && EffectiveGap < MinRestAfterHigh)
		{
			FPacingIssue Issue;
			Issue.Type = TEXT("no_breather_after_peak");
			Issue.EncounterA = i - 1;
			Issue.EncounterB = i;
			Issue.GapSeconds = EffectiveGap;
			Issue.RecommendedMin = MinRestAfterHigh;
			Issue.Description = FString::Printf(
				TEXT("High-intensity encounter %d (%.0f%%) needs %.1fs rest before next. Only %.1fs available."),
				i - 1, Encounters[i - 1].Intensity * 100.0f, MinRestAfterHigh, EffectiveGap);
			Issues.Add(MoveTemp(Issue));
		}
	}

	// Check intensity cap for pacing profile
	for (int32 i = 0; i < Encounters.Num(); ++i)
	{
		if (Encounters[i].Intensity > MaxIntensity)
		{
			FPacingIssue Issue;
			Issue.Type = TEXT("intensity_exceeds_profile");
			Issue.EncounterA = i;
			Issue.GapSeconds = 0.0f;
			Issue.RecommendedMin = 0.0f;
			Issue.Description = FString::Printf(
				TEXT("Encounter %d intensity %.0f%% exceeds %s profile max of %.0f%%."),
				i, Encounters[i].Intensity * 100.0f, *TargetPacing, MaxIntensity * 100.0f);
			Issues.Add(MoveTemp(Issue));
		}
	}

	// Check for monotonous intensity (3+ consecutive at similar intensity)
	for (int32 i = 2; i < Encounters.Num(); ++i)
	{
		float I0 = Encounters[i - 2].Intensity;
		float I1 = Encounters[i - 1].Intensity;
		float I2 = Encounters[i].Intensity;
		if (FMath::Abs(I0 - I1) < 0.15f && FMath::Abs(I1 - I2) < 0.15f)
		{
			FPacingIssue Issue;
			Issue.Type = TEXT("monotonous_intensity");
			Issue.EncounterA = i - 2;
			Issue.EncounterB = i;
			Issue.Description = FString::Printf(
				TEXT("Encounters %d-%d have similar intensity (%.0f%%, %.0f%%, %.0f%%). Vary intensity for better pacing."),
				i - 2, i, I0 * 100.0f, I1 * 100.0f, I2 * 100.0f);
			Issues.Add(MoveTemp(Issue));
		}
	}

	// Build tension curve (sample environmental tension between encounters)
	TArray<TSharedPtr<FJsonValue>> TensionCurveArr;
	{
		TArray<FVector> ResampledPath = ResamplePath(PathPoints, 300.0f);
		if (ResampledPath.Num() > 100)
		{
			int32 Step = (ResampledPath.Num() + 99) / 100;
			TArray<FVector> Thinned;
			for (int32 i = 0; i < ResampledPath.Num(); i += Step) Thinned.Add(ResampledPath[i]);
			ResampledPath = MoveTemp(Thinned);
		}

		float Accum = 0.0f;
		for (int32 i = 0; i < ResampledPath.Num(); ++i)
		{
			if (i > 0) Accum += FVector::Dist(ResampledPath[i - 1], ResampledPath[i]);

			float EnvTension = QuickTensionScore(World, ResampledPath[i]);

			// Add encounter-sourced tension (proximity boost)
			float EncounterBoost = 0.0f;
			for (const FEncounter& Enc : Encounters)
			{
				float D = FMath::Abs(Accum - Enc.DistanceAlongPath);
				float DurationDist = Enc.DurationS * static_cast<float>(WalkSpeed);
				if (D < DurationDist)
				{
					EncounterBoost = FMath::Max(EncounterBoost, Enc.Intensity * 50.0f * (1.0f - D / DurationDist));
				}
			}

			float TotalTension = FMath::Clamp(EnvTension + EncounterBoost, 0.0f, 100.0f);

			auto Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("distance_along_path"), Accum);
			Obj->SetNumberField(TEXT("environmental_tension"), FMath::RoundToInt(EnvTension));
			Obj->SetNumberField(TEXT("total_tension"), FMath::RoundToInt(TotalTension));
			TensionCurveArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}

	// Overall score: 0-100, higher = better pacing
	float OverallScore = 100.0f;
	for (const FPacingIssue& I : Issues)
	{
		if (I.Type == TEXT("insufficient_gap")) OverallScore -= 15.0f;
		else if (I.Type == TEXT("no_breather_after_peak")) OverallScore -= 20.0f;
		else if (I.Type == TEXT("intensity_exceeds_profile")) OverallScore -= 10.0f;
		else if (I.Type == TEXT("monotonous_intensity")) OverallScore -= 5.0f;
	}
	OverallScore = FMath::Clamp(OverallScore, 0.0f, 100.0f);

	// Hospice compliance
	bool bHospiceCompliant = true;
	if (TargetPacing.Equals(TEXT("hospice_gentle"), ESearchCase::IgnoreCase))
	{
		for (const FPacingIssue& I : Issues)
		{
			if (I.Type != TEXT("monotonous_intensity"))
			{
				bHospiceCompliant = false;
				break;
			}
		}
	}

	// Build encounters output with computed distances
	TArray<TSharedPtr<FJsonValue>> EncArr;
	for (int32 i = 0; i < Encounters.Num(); ++i)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("index"), i);
		Obj->SetArrayField(TEXT("location"), HDA_VecToArr(Encounters[i].Location));
		Obj->SetStringField(TEXT("type"), Encounters[i].Type);
		Obj->SetNumberField(TEXT("intensity"), Encounters[i].Intensity);
		Obj->SetNumberField(TEXT("duration_s"), Encounters[i].DurationS);
		Obj->SetNumberField(TEXT("distance_along_path"), Encounters[i].DistanceAlongPath);
		Obj->SetNumberField(TEXT("time_along_path_s"), Encounters[i].DistanceAlongPath / static_cast<float>(WalkSpeed));
		EncArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	// Build issues output
	TArray<TSharedPtr<FJsonValue>> IssuesArr;
	for (const FPacingIssue& I : Issues)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("type"), I.Type);
		Obj->SetStringField(TEXT("description"), I.Description);
		if (I.EncounterA >= 0) Obj->SetNumberField(TEXT("encounter_a"), I.EncounterA);
		if (I.EncounterB >= 0) Obj->SetNumberField(TEXT("encounter_b"), I.EncounterB);
		if (I.GapSeconds > 0.0f) Obj->SetNumberField(TEXT("gap_seconds"), I.GapSeconds);
		if (I.RecommendedMin > 0.0f) Obj->SetNumberField(TEXT("recommended_min_seconds"), I.RecommendedMin);
		IssuesArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("overall_score"), FMath::RoundToInt(OverallScore));
	Result->SetStringField(TEXT("target_pacing"), TargetPacing);
	Result->SetNumberField(TEXT("encounter_count"), Encounters.Num());
	Result->SetArrayField(TEXT("encounters"), EncArr);
	Result->SetArrayField(TEXT("issues"), IssuesArr);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetArrayField(TEXT("tension_curve"), TensionCurveArr);
	Result->SetNumberField(TEXT("total_path_length"), TotalPathLen);
	Result->SetNumberField(TEXT("total_path_time_s"), TotalPathLen / static_cast<float>(WalkSpeed));

	if (TargetPacing.Equals(TEXT("hospice_gentle"), ESearchCase::IgnoreCase))
	{
		Result->SetBoolField(TEXT("hospice_compliant"), bHospiceCompliant);
		if (!bHospiceCompliant)
		{
			Result->SetStringField(TEXT("hospice_warning"),
				TEXT("Encounter pacing does not meet hospice guidelines. Review issues for required adjustments."));
		}
	}

	return FMonolithActionResult::Success(Result);
}
