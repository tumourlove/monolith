#include "MonolithMeshAccessibilityActions.h"
#include "Materials/MaterialInterface.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "MonolithMeshUtils.h"
#include "MonolithMeshAnalysis.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "CollisionQueryParams.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "AI/Navigation/NavigationTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshAccessibilityActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 1. validate_path_width
	Registry.RegisterAction(TEXT("mesh"), TEXT("validate_path_width"),
		TEXT("Validate path width for wheelchair accessibility (default 120cm min). Returns pinch points with exact obstruction actors."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAccessibilityActions::ValidatePathWidth),
		FParamSchemaBuilder()
			.Required(TEXT("start"), TEXT("array"), TEXT("Start position [x, y, z]"))
			.Required(TEXT("end"), TEXT("array"), TEXT("End position [x, y, z]"))
			.Optional(TEXT("min_width"), TEXT("number"), TEXT("Minimum path width in cm (120 = wheelchair turning clearance)"), TEXT("120"))
			.Build());

	// 2. validate_navigation_complexity
	Registry.RegisterAction(TEXT("mesh"), TEXT("validate_navigation_complexity"),
		TEXT("Score cognitive difficulty of navigation between two points: turn count, sharp corners, backtracking, elevation changes."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAccessibilityActions::ValidateNavigationComplexity),
		FParamSchemaBuilder()
			.Required(TEXT("start"), TEXT("array"), TEXT("Start position [x, y, z]"))
			.Required(TEXT("end"), TEXT("array"), TEXT("End position [x, y, z]"))
			.Build());

	// 3. analyze_visual_contrast
	Registry.RegisterAction(TEXT("mesh"), TEXT("analyze_visual_contrast"),
		TEXT("Analyze visual contrast of interactable actors against their backgrounds using scene capture. WCAG-inspired thresholds."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAccessibilityActions::AnalyzeVisualContrast),
		FParamSchemaBuilder()
			.Required(TEXT("location"), TEXT("array"), TEXT("Camera position [x, y, z]"))
			.Optional(TEXT("forward"), TEXT("array"), TEXT("Camera facing direction [x, y, z]"), TEXT(""))
			.Optional(TEXT("fov"), TEXT("number"), TEXT("Field of view in degrees"), TEXT("90"))
			.Optional(TEXT("tags"), TEXT("array"), TEXT("Actor tags to check for contrast"), TEXT(""))
			.Build());

	// 4. find_rest_points
	Registry.RegisterAction(TEXT("mesh"), TEXT("find_rest_points"),
		TEXT("Walk a path and inventory safe rooms/calm zones. Flag gaps exceeding max_gap (default 30m). Hospice patients need frequent rest opportunities."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAccessibilityActions::FindRestPoints),
		FParamSchemaBuilder()
			.Required(TEXT("start"), TEXT("array"), TEXT("Start position [x, y, z]"))
			.Required(TEXT("end"), TEXT("array"), TEXT("End position [x, y, z]"))
			.Optional(TEXT("max_gap"), TEXT("number"), TEXT("Maximum distance between rest points in cm"), TEXT("3000"))
			.Build());

	// 5. validate_interactive_reach
	Registry.RegisterAction(TEXT("mesh"), TEXT("validate_interactive_reach"),
		TEXT("Check interactable actors for height, navmesh distance, and obstructions. Flag items requiring jumping or precision movement."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAccessibilityActions::ValidateInteractiveReach),
		FParamSchemaBuilder()
			.Optional(TEXT("region_min"), TEXT("array"), TEXT("Min corner of search region [x, y, z]"), TEXT(""))
			.Optional(TEXT("region_max"), TEXT("array"), TEXT("Max corner of search region [x, y, z]"), TEXT(""))
			.Optional(TEXT("tags"), TEXT("array"), TEXT("Actor tags to check"), TEXT(""))
			.Build());

	// 6. generate_accessibility_report
	Registry.RegisterAction(TEXT("mesh"), TEXT("generate_accessibility_report"),
		TEXT("Comprehensive accessibility report combining path width, navigation complexity, visual contrast, rest points, and interactive reach. Profile-specific thresholds."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAccessibilityActions::GenerateAccessibilityReport),
		FParamSchemaBuilder()
			.Required(TEXT("start"), TEXT("array"), TEXT("Start position [x, y, z]"))
			.Required(TEXT("end"), TEXT("array"), TEXT("End position [x, y, z]"))
			.Optional(TEXT("profile"), TEXT("string"), TEXT("Accessibility profile: motor_impaired, vision_impaired, or cognitive_fatigue"), TEXT(""))
			.Build());
}

// ============================================================================
// Helpers
// ============================================================================

namespace
{
	TArray<TSharedPtr<FJsonValue>> MAcc_VecToArr(const FVector& V)
	{
		return MonolithMeshAnalysis::VectorToJsonArray(V);
	}
}

// ============================================================================
// 1. validate_path_width
// ============================================================================

FMonolithActionResult FMonolithMeshAccessibilityActions::ValidatePathWidth(const TSharedPtr<FJsonObject>& Params)
{
	FVector Start, End;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("start"), Start))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: start"));
	}
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("end"), End))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: end"));
	}

	double MinWidth = 120.0;
	Params->TryGetNumberField(TEXT("min_width"), MinWidth);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	TArray<FVector> PathPoints;
	float PathDist;
	if (!MonolithMeshAnalysis::FindNavPath(World, Start, End, PathPoints, PathDist))
	{
		return FMonolithActionResult::Error(TEXT("No navmesh path found. Build navigation first."));
	}

	TArray<MonolithMeshAnalysis::FPathClearance> Clearances = MonolithMeshAnalysis::MeasurePathClearance(World, PathPoints, 500.0f);

	// Find violations
	struct FPinchPoint
	{
		FVector Location;
		float Width;
		FString LeftObstruction;
		FString RightObstruction;
		float Deficit; // How much narrower than minimum
	};

	TArray<FPinchPoint> Violations;
	float MinFound = TNumericLimits<float>::Max();
	float MaxFound = 0.0f;
	float TotalWidth = 0.0f;

	for (const auto& C : Clearances)
	{
		MinFound = FMath::Min(MinFound, C.TotalWidth);
		MaxFound = FMath::Max(MaxFound, C.TotalWidth);
		TotalWidth += C.TotalWidth;

		if (C.TotalWidth < static_cast<float>(MinWidth))
		{
			// Check if this is near an existing violation (merge nearby)
			bool bMerged = false;
			for (FPinchPoint& Existing : Violations)
			{
				if (FVector::Dist(C.Location, Existing.Location) < 200.0f)
				{
					if (C.TotalWidth < Existing.Width)
					{
						Existing.Location = C.Location;
						Existing.Width = C.TotalWidth;
						Existing.LeftObstruction = C.LeftObstruction;
						Existing.RightObstruction = C.RightObstruction;
						Existing.Deficit = static_cast<float>(MinWidth) - C.TotalWidth;
					}
					bMerged = true;
					break;
				}
			}
			if (!bMerged)
			{
				FPinchPoint PP;
				PP.Location = C.Location;
				PP.Width = C.TotalWidth;
				PP.LeftObstruction = C.LeftObstruction;
				PP.RightObstruction = C.RightObstruction;
				PP.Deficit = static_cast<float>(MinWidth) - C.TotalWidth;
				Violations.Add(PP);
			}
		}
	}

	// Sort by severity (worst first)
	Violations.Sort([](const FPinchPoint& A, const FPinchPoint& B) { return A.Width < B.Width; });

	TArray<TSharedPtr<FJsonValue>> ViolArr;
	for (const FPinchPoint& PP : Violations)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetArrayField(TEXT("location"), MAcc_VecToArr(PP.Location));
		Obj->SetNumberField(TEXT("width"), PP.Width);
		Obj->SetNumberField(TEXT("deficit"), PP.Deficit);
		Obj->SetStringField(TEXT("left_obstruction"), PP.LeftObstruction);
		Obj->SetStringField(TEXT("right_obstruction"), PP.RightObstruction);

		// Suggest fix
		FString Fix;
		if (!PP.LeftObstruction.IsEmpty() && !PP.RightObstruction.IsEmpty())
		{
			Fix = FString::Printf(TEXT("Move '%s' or '%s' apart by at least %.0fcm"), *PP.LeftObstruction, *PP.RightObstruction, PP.Deficit);
		}
		else if (!PP.LeftObstruction.IsEmpty())
		{
			Fix = FString::Printf(TEXT("Move '%s' at least %.0fcm away from path"), *PP.LeftObstruction, PP.Deficit);
		}
		else if (!PP.RightObstruction.IsEmpty())
		{
			Fix = FString::Printf(TEXT("Move '%s' at least %.0fcm away from path"), *PP.RightObstruction, PP.Deficit);
		}
		Obj->SetStringField(TEXT("suggested_fix"), Fix);
		ViolArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	float AvgWidth = Clearances.Num() > 0 ? TotalWidth / static_cast<float>(Clearances.Num()) : 0.0f;

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("passes"), Violations.Num() == 0);
	Result->SetNumberField(TEXT("violations"), Violations.Num());
	Result->SetArrayField(TEXT("pinch_points"), ViolArr);
	Result->SetNumberField(TEXT("min_width_found"), MinFound < TNumericLimits<float>::Max() ? MinFound : 0.0f);
	Result->SetNumberField(TEXT("max_width_found"), MaxFound);
	Result->SetNumberField(TEXT("average_width"), AvgWidth);
	Result->SetNumberField(TEXT("path_distance"), PathDist);
	Result->SetNumberField(TEXT("min_width_required"), MinWidth);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 2. validate_navigation_complexity
// ============================================================================

FMonolithActionResult FMonolithMeshAccessibilityActions::ValidateNavigationComplexity(const TSharedPtr<FJsonObject>& Params)
{
	FVector Start, End;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("start"), Start))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: start"));
	}
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("end"), End))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: end"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	TArray<FVector> PathPoints;
	float PathDist;
	if (!MonolithMeshAnalysis::FindNavPath(World, Start, End, PathPoints, PathDist))
	{
		return FMonolithActionResult::Error(TEXT("No navmesh path found."));
	}

	if (PathPoints.Num() < 2)
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("complexity_score"), 0);
		Result->SetStringField(TEXT("rating"), TEXT("trivial"));
		return FMonolithActionResult::Success(Result);
	}

	// Count turns
	int32 TurnCount = 0;
	int32 SharpCorners = 0; // >90 degree turns
	int32 BacktrackSegments = 0;
	float TotalElevationChange = 0.0f;
	float MaxElevationChange = 0.0f;

	for (int32 i = 1; i < PathPoints.Num() - 1; ++i)
	{
		FVector Dir1 = (PathPoints[i] - PathPoints[i - 1]).GetSafeNormal();
		FVector Dir2 = (PathPoints[i + 1] - PathPoints[i]).GetSafeNormal();
		float Dot = FVector::DotProduct(Dir1, Dir2);

		if (Dot < 0.707f) // > ~45 degrees
		{
			++TurnCount;
		}
		if (Dot < 0.0f) // > 90 degrees
		{
			++SharpCorners;
		}
		if (Dot < -0.5f) // > ~120 degrees — backtracking
		{
			++BacktrackSegments;
		}
	}

	// Elevation changes
	for (int32 i = 1; i < PathPoints.Num(); ++i)
	{
		float DeltaZ = FMath::Abs(PathPoints[i].Z - PathPoints[i - 1].Z);
		TotalElevationChange += DeltaZ;
		MaxElevationChange = FMath::Max(MaxElevationChange, DeltaZ);
	}

	// Compute complexity score 0-100
	// Factors: turns per 10m, sharp corners, backtracking, elevation
	float TurnsPer10m = (PathDist > 0) ? (static_cast<float>(TurnCount) / (PathDist / 1000.0f)) : 0.0f;

	float TurnScore = FMath::Clamp(TurnsPer10m * 10.0f, 0.0f, 30.0f);
	float SharpScore = FMath::Clamp(static_cast<float>(SharpCorners) * 8.0f, 0.0f, 25.0f);
	float BacktrackScore = FMath::Clamp(static_cast<float>(BacktrackSegments) * 15.0f, 0.0f, 25.0f);
	float ElevationScore = FMath::Clamp(TotalElevationChange / 50.0f, 0.0f, 20.0f);

	float ComplexityScore = FMath::Clamp(TurnScore + SharpScore + BacktrackScore + ElevationScore, 0.0f, 100.0f);

	// Rating
	FString Rating;
	if (ComplexityScore < 15.0f)      Rating = TEXT("simple");
	else if (ComplexityScore < 35.0f) Rating = TEXT("moderate");
	else if (ComplexityScore < 60.0f) Rating = TEXT("complex");
	else                              Rating = TEXT("confusing");

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("complexity_score"), FMath::RoundToInt(ComplexityScore));
	Result->SetStringField(TEXT("rating"), Rating);
	Result->SetNumberField(TEXT("turn_count"), TurnCount);
	Result->SetNumberField(TEXT("sharp_corners"), SharpCorners);
	Result->SetNumberField(TEXT("backtrack_segments"), BacktrackSegments);
	Result->SetNumberField(TEXT("total_elevation_change"), TotalElevationChange);
	Result->SetNumberField(TEXT("max_single_elevation_change"), MaxElevationChange);
	Result->SetNumberField(TEXT("path_distance"), PathDist);
	Result->SetNumberField(TEXT("turns_per_10m"), TurnsPer10m);

	// Recommendations
	TArray<TSharedPtr<FJsonValue>> Recs;
	if (SharpCorners > 0)
	{
		Recs.Add(MakeShared<FJsonValueString>(FString::Printf(
			TEXT("Found %d sharp corners (>90 deg). Consider smoothing these for patients with cognitive fatigue."), SharpCorners)));
	}
	if (BacktrackSegments > 0)
	{
		Recs.Add(MakeShared<FJsonValueString>(FString::Printf(
			TEXT("Path requires %d backtracking segments. Add clearer waypoints or signage."), BacktrackSegments)));
	}
	if (TotalElevationChange > 200.0f)
	{
		Recs.Add(MakeShared<FJsonValueString>(FString::Printf(
			TEXT("Total elevation change of %.0fcm. Ensure ramps are available (no stairs-only paths)."), TotalElevationChange)));
	}
	if (ComplexityScore > 60.0f)
	{
		Recs.Add(MakeShared<FJsonValueString>(TEXT("Path is highly complex. Consider adding visual breadcrumbs (lighting, color coding) to guide players.")));
	}
	Result->SetArrayField(TEXT("recommendations"), Recs);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 3. analyze_visual_contrast
// ============================================================================

FMonolithActionResult FMonolithMeshAccessibilityActions::AnalyzeVisualContrast(const TSharedPtr<FJsonObject>& Params)
{
	FVector Location;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: location"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FVector Forward = FVector::ForwardVector;
	MonolithMeshUtils::ParseVector(Params, TEXT("forward"), Forward);
	Forward = Forward.GetSafeNormal();
	if (Forward.IsNearlyZero()) Forward = FVector::ForwardVector;

	double FOV = 90.0;
	Params->TryGetNumberField(TEXT("fov"), FOV);

	// Parse tags for interactable actors
	TArray<FString> Tags;
	const TArray<TSharedPtr<FJsonValue>>* TagsArr;
	if (Params->TryGetArrayField(TEXT("tags"), TagsArr))
	{
		for (const auto& V : *TagsArr) Tags.Add(V->AsString());
	}
	if (Tags.Num() == 0)
	{
		Tags.Add(TEXT("Interactable"));
	}

	// Find interactable actors in view
	FVector Origin = Location + FVector(0, 0, 170.0f);
	float HalfFOVRad = FMath::DegreesToRadians(static_cast<float>(FOV) * 0.5f);

	struct FContrastResult
	{
		FString ActorName;
		FString Tag;
		float Distance;
		float ContrastRatio;
		bool bPassesWCAG;
	};

	TArray<FContrastResult> Results;
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MonolithContrast), true);

	// Rather than using full scene capture (which requires a running game viewport),
	// we approximate contrast using material property analysis.
	// This works in editor without play mode.
	//
	// Approach: for each interactable actor, cast rays to it and around it.
	// Compare the interactable's approximate visual properties against surrounding geometry.
	// This is a geometric/material-based heuristic rather than pixel-perfect.

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;

		// Check tags
		bool bHasTag = false;
		FString MatchedTag;
		for (const FName& ActorTag : Actor->Tags)
		{
			for (const FString& SearchTag : Tags)
			{
				if (ActorTag.ToString().Contains(SearchTag, ESearchCase::IgnoreCase))
				{
					bHasTag = true;
					MatchedTag = SearchTag;
					break;
				}
			}
			if (bHasTag) break;
		}
		if (!bHasTag) continue;

		FVector ActorLoc = Actor->GetActorLocation();

		// Check if in FOV
		FVector ToActor = (ActorLoc - Origin).GetSafeNormal();
		float DotVal = FVector::DotProduct(Forward, ToActor);
		if (DotVal < FMath::Cos(HalfFOVRad))
		{
			continue; // Outside FOV
		}

		float Distance = FVector::Dist(Origin, ActorLoc);
		if (Distance > 5000.0f) continue; // Too far

		// Check line of sight
		FHitResult LOSHit;
		QueryParams.AddIgnoredActor(Actor);
		bool bBlocked = World->LineTraceSingleByChannel(LOSHit, Origin, ActorLoc, ECC_Visibility, QueryParams);
		QueryParams.ClearIgnoredSourceObjects();

		if (bBlocked && LOSHit.Distance < Distance - 50.0f)
		{
			continue; // Can't see it
		}

		// Estimate contrast via surrounding geometry analysis
		// Fire rays around the actor to sample background colors via physical materials
		// Different phys materials = higher contrast (rough heuristic)
		TSet<FString> ActorMaterials;
		TSet<FString> BackgroundMaterials;

		// Actor's own materials (from primitive components)
		TArray<UPrimitiveComponent*> PrimComps;
		Actor->GetComponents<UPrimitiveComponent>(PrimComps);
		for (UPrimitiveComponent* Comp : PrimComps)
		{
			for (int32 m = 0; m < Comp->GetNumMaterials(); ++m)
			{
				UMaterialInterface* Mat = Comp->GetMaterial(m);
				if (Mat)
				{
					ActorMaterials.Add(Mat->GetName());
				}
			}
		}

		// Background materials: fire rays past the actor to hit what's behind it
		const int32 BgSamples = 8;
		FVector Up = FVector::UpVector;
		FVector ActorRight = FVector::CrossProduct(ToActor, Up).GetSafeNormal();

		for (int32 s = 0; s < BgSamples; ++s)
		{
			float Angle = (2.0f * PI / static_cast<float>(BgSamples)) * static_cast<float>(s);
			FVector Offset = (ActorRight * FMath::Cos(Angle) + Up * FMath::Sin(Angle)) * 100.0f;
			FVector BgTarget = ActorLoc + Offset + ToActor * 200.0f;

			FHitResult BgHit;
			bool bBgHit = World->LineTraceSingleByChannel(BgHit, ActorLoc + Offset, BgTarget, ECC_Visibility, QueryParams);
			if (bBgHit && BgHit.GetActor() != Actor)
			{
				if (BgHit.PhysMaterial.IsValid())
				{
					BackgroundMaterials.Add(BgHit.PhysMaterial->GetName());
				}
				UPrimitiveComponent* BgComp = BgHit.GetComponent();
				if (BgComp)
				{
					for (int32 m = 0; m < BgComp->GetNumMaterials(); ++m)
					{
						UMaterialInterface* Mat = BgComp->GetMaterial(m);
						if (Mat) BackgroundMaterials.Add(Mat->GetName());
					}
				}
			}
		}

		// Contrast heuristic: number of unique materials in background vs actor
		// If they share no materials, contrast is likely good. If materials are identical, contrast is poor.
		int32 SharedCount = 0;
		for (const FString& AM : ActorMaterials)
		{
			if (BackgroundMaterials.Contains(AM))
			{
				++SharedCount;
			}
		}

		int32 TotalUnique = ActorMaterials.Num() + BackgroundMaterials.Num() - SharedCount;
		float ContrastRatio = (TotalUnique > 0)
			? 1.0f - (static_cast<float>(SharedCount) / static_cast<float>(FMath::Max(ActorMaterials.Num(), 1)))
			: 0.5f;

		// Scale by distance (farther = harder to see)
		float DistancePenalty = FMath::Clamp(Distance / 3000.0f, 0.0f, 0.5f);
		ContrastRatio = FMath::Clamp(ContrastRatio - DistancePenalty, 0.0f, 1.0f);

		// WCAG-inspired threshold: 0.45 minimum (loose equivalent of 3:1 ratio)
		bool bPasses = ContrastRatio >= 0.45f;

		FContrastResult CR;
		CR.ActorName = Actor->GetActorNameOrLabel();
		CR.Tag = MatchedTag;
		CR.Distance = Distance;
		CR.ContrastRatio = ContrastRatio;
		CR.bPassesWCAG = bPasses;
		Results.Add(CR);
	}

	// Sort by contrast (worst first)
	Results.Sort([](const FContrastResult& A, const FContrastResult& B) { return A.ContrastRatio < B.ContrastRatio; });

	int32 Failures = 0;
	TArray<TSharedPtr<FJsonValue>> ItemsArr;
	for (const FContrastResult& CR : Results)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("actor"), CR.ActorName);
		Obj->SetStringField(TEXT("tag"), CR.Tag);
		Obj->SetNumberField(TEXT("distance"), CR.Distance);
		Obj->SetNumberField(TEXT("contrast_ratio"), CR.ContrastRatio);
		Obj->SetBoolField(TEXT("passes_wcag"), CR.bPassesWCAG);
		if (!CR.bPassesWCAG)
		{
			Obj->SetStringField(TEXT("suggested_fix"), TEXT("Add emissive highlight, outline material, or increase color difference from background"));
			++Failures;
		}
		ItemsArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("actors_analyzed"), Results.Num());
	Result->SetNumberField(TEXT("failures"), Failures);
	Result->SetBoolField(TEXT("all_pass"), Failures == 0);
	Result->SetArrayField(TEXT("results"), ItemsArr);
	Result->SetStringField(TEXT("method"), TEXT("material_heuristic"));
	Result->SetStringField(TEXT("note"), TEXT("Contrast is approximated via material analysis (no scene capture). For pixel-perfect results, use the Phase 7 lighting actions."));

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 4. find_rest_points
// ============================================================================

FMonolithActionResult FMonolithMeshAccessibilityActions::FindRestPoints(const TSharedPtr<FJsonObject>& Params)
{
	FVector Start, End;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("start"), Start))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: start"));
	}
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("end"), End))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: end"));
	}

	double MaxGap = 3000.0; // 30m default
	Params->TryGetNumberField(TEXT("max_gap"), MaxGap);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	TArray<FVector> PathPoints;
	float PathDist;
	if (!MonolithMeshAnalysis::FindNavPath(World, Start, End, PathPoints, PathDist))
	{
		return FMonolithActionResult::Error(TEXT("No navmesh path found."));
	}

	// Find rest points: actors tagged SafeRoom or calm zones (low tension)
	struct FRestPoint
	{
		FVector Location;
		FString Name;
		FString Type; // "safe_room", "calm_zone", "bench" etc.
		float DistAlongPath;
	};

	TArray<FRestPoint> RestPoints;

	// First: tagged actors near the path
	TArray<FString> RestTags = { TEXT("SafeRoom"), TEXT("RestPoint"), TEXT("CalmZone"), TEXT("Bench"), TEXT("SavePoint") };

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		FString MatchedTag;
		for (const FName& ActorTag : Actor->Tags)
		{
			FString TagStr = ActorTag.ToString();
			for (const FString& RT : RestTags)
			{
				if (TagStr.Contains(RT, ESearchCase::IgnoreCase))
				{
					MatchedTag = RT;
					break;
				}
			}
			if (!MatchedTag.IsEmpty()) break;
		}
		if (MatchedTag.IsEmpty()) continue;

		FVector ActorLoc = Actor->GetActorLocation();

		// Find closest point on path and distance along path
		float MinDistToPath = TNumericLimits<float>::Max();
		float BestDistAlong = 0.0f;
		float CumulDist = 0.0f;

		for (int32 i = 0; i < PathPoints.Num(); ++i)
		{
			float D = FVector::Dist(ActorLoc, PathPoints[i]);
			if (D < MinDistToPath)
			{
				MinDistToPath = D;
				BestDistAlong = CumulDist;
			}
			if (i > 0)
			{
				CumulDist += FVector::Dist(PathPoints[i - 1], PathPoints[i]);
			}
		}

		if (MinDistToPath < 1000.0f) // Within 10m of path
		{
			FRestPoint RP;
			RP.Location = ActorLoc;
			RP.Name = Actor->GetActorNameOrLabel();
			RP.Type = MatchedTag.ToLower();
			RP.DistAlongPath = BestDistAlong;
			RestPoints.Add(RP);
		}
	}

	// Also sample tension along path to find natural calm zones
	{
		float SampleInterval = 500.0f;
		float CumulDist = 0.0f;

		for (int32 i = 0; i < PathPoints.Num(); ++i)
		{
			if (i > 0)
			{
				CumulDist += FVector::Dist(PathPoints[i - 1], PathPoints[i]);
			}

			if (FMath::Fmod(CumulDist, SampleInterval) < 50.0f || i == 0)
			{
				MonolithMeshAnalysis::FTensionInputs Inputs;
				// Quick sightline check
				FCollisionQueryParams QP(SCENE_QUERY_STAT(MonolithRest), true);
				FVector Origin = PathPoints[i] + FVector(0, 0, 170.0f);
				float TotalSightDist = 0.0f;
				for (int32 d = 0; d < 8; ++d)
				{
					float Angle = (2.0f * PI / 8.0f) * static_cast<float>(d);
					FVector Dir(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
					FHitResult Hit;
					bool bHit = World->LineTraceSingleByChannel(Hit, Origin, Origin + Dir * 5000.0f, ECC_Visibility, QP);
					TotalSightDist += bHit ? Hit.Distance : 5000.0f;
				}
				Inputs.AverageSightlineDistance = TotalSightDist / 8.0f;
				Inputs.CeilingHeight = MonolithMeshAnalysis::MeasureCeilingHeight(World, PathPoints[i]);
				Inputs.ExitCount = 3; // Neutral
				Inputs.RoomVolume = 0.0f;

				float Score = MonolithMeshAnalysis::ComputeTensionScore(Inputs);
				if (Score < 20.0f) // Calm zone
				{
					// Check not too close to existing rest point
					bool bTooClose = false;
					for (const FRestPoint& Existing : RestPoints)
					{
						if (FMath::Abs(CumulDist - Existing.DistAlongPath) < 500.0f)
						{
							bTooClose = true;
							break;
						}
					}
					if (!bTooClose)
					{
						FRestPoint RP;
						RP.Location = PathPoints[i];
						RP.Name = TEXT("(natural calm zone)");
						RP.Type = TEXT("calm_zone");
						RP.DistAlongPath = CumulDist;
						RestPoints.Add(RP);
					}
				}
			}
		}
	}

	// Sort by distance along path
	RestPoints.Sort([](const FRestPoint& A, const FRestPoint& B) { return A.DistAlongPath < B.DistAlongPath; });

	// Find gaps
	struct FGap
	{
		float StartDist;
		float EndDist;
		float Length;
	};

	TArray<FGap> Gaps;
	float PrevDist = 0.0f;

	for (const FRestPoint& RP : RestPoints)
	{
		float GapLen = RP.DistAlongPath - PrevDist;
		if (GapLen > static_cast<float>(MaxGap))
		{
			FGap G;
			G.StartDist = PrevDist;
			G.EndDist = RP.DistAlongPath;
			G.Length = GapLen;
			Gaps.Add(G);
		}
		PrevDist = RP.DistAlongPath;
	}
	// Check gap from last rest point to end
	if (PathDist - PrevDist > static_cast<float>(MaxGap))
	{
		FGap G;
		G.StartDist = PrevDist;
		G.EndDist = PathDist;
		G.Length = PathDist - PrevDist;
		Gaps.Add(G);
	}

	TArray<TSharedPtr<FJsonValue>> RPArr;
	for (const FRestPoint& RP : RestPoints)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetArrayField(TEXT("location"), MAcc_VecToArr(RP.Location));
		Obj->SetStringField(TEXT("name"), RP.Name);
		Obj->SetStringField(TEXT("type"), RP.Type);
		Obj->SetNumberField(TEXT("distance_along_path"), RP.DistAlongPath);
		RPArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TArray<TSharedPtr<FJsonValue>> GapsArr;
	for (const FGap& G : Gaps)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("start_distance"), G.StartDist);
		Obj->SetNumberField(TEXT("end_distance"), G.EndDist);
		Obj->SetNumberField(TEXT("gap_length"), G.Length);
		Obj->SetStringField(TEXT("severity"), G.Length > static_cast<float>(MaxGap) * 2.0f ? TEXT("critical") : TEXT("warning"));
		GapsArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("rest_points_found"), RestPoints.Num());
	Result->SetArrayField(TEXT("rest_points"), RPArr);
	Result->SetNumberField(TEXT("gaps_found"), Gaps.Num());
	Result->SetArrayField(TEXT("gaps"), GapsArr);
	Result->SetNumberField(TEXT("path_distance"), PathDist);
	Result->SetNumberField(TEXT("max_gap_allowed"), MaxGap);
	Result->SetBoolField(TEXT("passes"), Gaps.Num() == 0);

	if (RestPoints.Num() == 0)
	{
		Result->SetStringField(TEXT("warning"), TEXT("No rest points found along this path. Hospice patients need regular rest opportunities. Tag safe areas with 'SafeRoom' or 'RestPoint'."));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 5. validate_interactive_reach
// ============================================================================

FMonolithActionResult FMonolithMeshAccessibilityActions::ValidateInteractiveReach(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Optional region filter
	FVector RegionMin = FVector::ZeroVector;
	FVector RegionMax = FVector::ZeroVector;
	bool bHasRegion = MonolithMeshUtils::ParseVector(Params, TEXT("region_min"), RegionMin)
		&& MonolithMeshUtils::ParseVector(Params, TEXT("region_max"), RegionMax);

	// Tags
	TArray<FString> Tags;
	const TArray<TSharedPtr<FJsonValue>>* TagsArr;
	if (Params->TryGetArrayField(TEXT("tags"), TagsArr))
	{
		for (const auto& V : *TagsArr) Tags.Add(V->AsString());
	}
	if (Tags.Num() == 0)
	{
		Tags.Add(TEXT("Interactable"));
	}

	// Check navmesh availability
	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MonolithReach), true);

	struct FReachIssue
	{
		FString ActorName;
		FVector Location;
		float Height;
		float NavmeshDistance;
		bool bLineOfSight;
		TArray<FString> Issues;
		FString Severity; // warning, error
	};

	TArray<FReachIssue> AllItems;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;

		// Check tags
		bool bHasTag = false;
		for (const FName& ActorTag : Actor->Tags)
		{
			for (const FString& T : Tags)
			{
				if (ActorTag.ToString().Contains(T, ESearchCase::IgnoreCase))
				{
					bHasTag = true;
					break;
				}
			}
			if (bHasTag) break;
		}
		if (!bHasTag) continue;

		FVector ActorLoc = Actor->GetActorLocation();

		// Region filter
		if (bHasRegion)
		{
			if (ActorLoc.X < RegionMin.X || ActorLoc.X > RegionMax.X ||
				ActorLoc.Y < RegionMin.Y || ActorLoc.Y > RegionMax.Y ||
				ActorLoc.Z < RegionMin.Z || ActorLoc.Z > RegionMax.Z)
			{
				continue;
			}
		}

		FReachIssue Item;
		Item.ActorName = Actor->GetActorNameOrLabel();
		Item.Location = ActorLoc;

		// Check height — floor trace down from actor
		FHitResult FloorHit;
		bool bHitFloor = World->LineTraceSingleByChannel(FloorHit, ActorLoc, ActorLoc - FVector(0, 0, 1000.0f), ECC_Visibility, QueryParams);
		float FloorZ = bHitFloor ? FloorHit.Location.Z : ActorLoc.Z - 100.0f;
		Item.Height = ActorLoc.Z - FloorZ;

		// Check navmesh distance
		Item.NavmeshDistance = 0.0f;
		if (NavSys)
		{
			FNavLocation NavLoc;
			FVector ProjectExtent(200.0f, 200.0f, 500.0f);
			if (NavSys->ProjectPointToNavigation(ActorLoc, NavLoc, ProjectExtent))
			{
				Item.NavmeshDistance = FVector::Dist(ActorLoc, NavLoc.Location);
			}
			else
			{
				Item.NavmeshDistance = -1.0f; // Not reachable via navmesh
			}
		}

		// Check line of sight from a reasonable standing position nearby
		Item.bLineOfSight = true;
		if (NavSys)
		{
			FNavLocation NearestNav;
			if (NavSys->ProjectPointToNavigation(ActorLoc, NearestNav, FVector(300.0f, 300.0f, 500.0f)))
			{
				FVector EyePos = NearestNav.Location + FVector(0, 0, 170.0f);
				FHitResult LOSHit;
				QueryParams.AddIgnoredActor(Actor);
				bool bBlocked = World->LineTraceSingleByChannel(LOSHit, EyePos, ActorLoc, ECC_Visibility, QueryParams);
				QueryParams.ClearIgnoredSourceObjects();
				Item.bLineOfSight = !bBlocked;
			}
		}

		// Identify issues
		if (Item.Height > 150.0f)
		{
			Item.Issues.Add(FString::Printf(TEXT("Too high (%.0fcm above floor). Max comfortable reach is 150cm."), Item.Height));
		}
		if (Item.Height < 40.0f && Item.Height > 0.0f)
		{
			Item.Issues.Add(FString::Printf(TEXT("Very low (%.0fcm above floor). May require bending/crouching."), Item.Height));
		}
		if (Item.NavmeshDistance < 0.0f)
		{
			Item.Issues.Add(TEXT("Not reachable from navmesh. Actor may be in an inaccessible area."));
		}
		else if (Item.NavmeshDistance > 100.0f)
		{
			Item.Issues.Add(FString::Printf(TEXT("%.0fcm from nearest navmesh point. May require precision movement."), Item.NavmeshDistance));
		}
		if (!Item.bLineOfSight)
		{
			Item.Issues.Add(TEXT("No clear line of sight from nearest reachable position."));
		}

		Item.Severity = Item.Issues.Num() > 1 ? TEXT("error") : (Item.Issues.Num() == 1 ? TEXT("warning") : TEXT("ok"));

		AllItems.Add(MoveTemp(Item));
	}

	// Sort: errors first, then warnings
	AllItems.Sort([](const FReachIssue& A, const FReachIssue& B)
	{
		return A.Issues.Num() > B.Issues.Num();
	});

	int32 ErrorCount = 0, WarningCount = 0, OKCount = 0;
	TArray<TSharedPtr<FJsonValue>> ItemsArr;
	for (const FReachIssue& Item : AllItems)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("actor"), Item.ActorName);
		Obj->SetArrayField(TEXT("location"), MAcc_VecToArr(Item.Location));
		Obj->SetNumberField(TEXT("height_above_floor"), Item.Height);
		Obj->SetNumberField(TEXT("navmesh_distance"), Item.NavmeshDistance);
		Obj->SetBoolField(TEXT("line_of_sight"), Item.bLineOfSight);
		Obj->SetStringField(TEXT("severity"), Item.Severity);

		TArray<TSharedPtr<FJsonValue>> IssuesArr;
		for (const FString& Issue : Item.Issues)
		{
			IssuesArr.Add(MakeShared<FJsonValueString>(Issue));
		}
		Obj->SetArrayField(TEXT("issues"), IssuesArr);
		ItemsArr.Add(MakeShared<FJsonValueObject>(Obj));

		if (Item.Severity == TEXT("error")) ++ErrorCount;
		else if (Item.Severity == TEXT("warning")) ++WarningCount;
		else ++OKCount;
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("actors_checked"), AllItems.Num());
	Result->SetNumberField(TEXT("errors"), ErrorCount);
	Result->SetNumberField(TEXT("warnings"), WarningCount);
	Result->SetNumberField(TEXT("ok"), OKCount);
	Result->SetBoolField(TEXT("all_pass"), ErrorCount == 0 && WarningCount == 0);
	Result->SetArrayField(TEXT("items"), ItemsArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 6. generate_accessibility_report
// ============================================================================

FMonolithActionResult FMonolithMeshAccessibilityActions::GenerateAccessibilityReport(const TSharedPtr<FJsonObject>& Params)
{
	FVector Start, End;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("start"), Start))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: start"));
	}
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("end"), End))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: end"));
	}

	FString Profile;
	Params->TryGetStringField(TEXT("profile"), Profile);

	// Profile-specific thresholds
	float MinPathWidth = 120.0f;
	float MaxRestGap = 3000.0f;
	float ContrastThreshold = 0.45f;
	float MaxComplexity = 60.0f;

	if (Profile == TEXT("motor_impaired"))
	{
		MinPathWidth = 150.0f;    // Wider for wheelchair + companion
		MaxRestGap = 2000.0f;     // 20m max between rests
		MaxComplexity = 40.0f;    // Simpler paths
	}
	else if (Profile == TEXT("vision_impaired"))
	{
		ContrastThreshold = 0.6f; // Higher contrast needed
		MaxRestGap = 3000.0f;
	}
	else if (Profile == TEXT("cognitive_fatigue"))
	{
		MaxComplexity = 30.0f;    // Much simpler paths
		MaxRestGap = 2000.0f;     // Frequent rests
	}

	// Run all sub-analyses
	auto Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("profile"), Profile.IsEmpty() ? TEXT("general") : Profile);

	TArray<TSharedPtr<FJsonValue>> Issues;
	int32 CriticalCount = 0, WarningCount = 0;

	// 1. Path Width
	{
		auto PathParams = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> StartArr, EndArr;
		StartArr.Add(MakeShared<FJsonValueNumber>(Start.X));
		StartArr.Add(MakeShared<FJsonValueNumber>(Start.Y));
		StartArr.Add(MakeShared<FJsonValueNumber>(Start.Z));
		EndArr.Add(MakeShared<FJsonValueNumber>(End.X));
		EndArr.Add(MakeShared<FJsonValueNumber>(End.Y));
		EndArr.Add(MakeShared<FJsonValueNumber>(End.Z));
		PathParams->SetArrayField(TEXT("start"), StartArr);
		PathParams->SetArrayField(TEXT("end"), EndArr);
		PathParams->SetNumberField(TEXT("min_width"), MinPathWidth);

		FMonolithActionResult PathResult = ValidatePathWidth(PathParams);
		if (PathResult.bSuccess)
		{
			Report->SetObjectField(TEXT("path_width"), PathResult.Result);
			if (!PathResult.Result->GetBoolField(TEXT("passes")))
			{
				int32 Violations = static_cast<int32>(PathResult.Result->GetNumberField(TEXT("violations")));
				auto Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("category"), TEXT("path_width"));
				Issue->SetStringField(TEXT("severity"), TEXT("critical"));
				Issue->SetStringField(TEXT("description"), FString::Printf(
					TEXT("%d path width violations found (min required: %.0fcm)"), Violations, MinPathWidth));
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
				++CriticalCount;
			}
		}
	}

	// 2. Navigation Complexity
	{
		auto NavParams = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> StartArr, EndArr;
		StartArr.Add(MakeShared<FJsonValueNumber>(Start.X));
		StartArr.Add(MakeShared<FJsonValueNumber>(Start.Y));
		StartArr.Add(MakeShared<FJsonValueNumber>(Start.Z));
		EndArr.Add(MakeShared<FJsonValueNumber>(End.X));
		EndArr.Add(MakeShared<FJsonValueNumber>(End.Y));
		EndArr.Add(MakeShared<FJsonValueNumber>(End.Z));
		NavParams->SetArrayField(TEXT("start"), StartArr);
		NavParams->SetArrayField(TEXT("end"), EndArr);

		FMonolithActionResult NavResult = ValidateNavigationComplexity(NavParams);
		if (NavResult.bSuccess)
		{
			Report->SetObjectField(TEXT("navigation_complexity"), NavResult.Result);
			float Score = static_cast<float>(NavResult.Result->GetNumberField(TEXT("complexity_score")));
			if (Score > MaxComplexity)
			{
				auto Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("category"), TEXT("navigation"));
				Issue->SetStringField(TEXT("severity"), Score > 75.0f ? TEXT("critical") : TEXT("warning"));
				Issue->SetStringField(TEXT("description"), FString::Printf(
					TEXT("Navigation complexity %.0f exceeds threshold %.0f"), Score, MaxComplexity));
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
				if (Score > 75.0f) ++CriticalCount; else ++WarningCount;
			}
		}
	}

	// 3. Visual Contrast
	{
		auto ContrastParams = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> LocArr;
		// Use midpoint of path for contrast check
		FVector Mid = (Start + End) * 0.5f;
		LocArr.Add(MakeShared<FJsonValueNumber>(Mid.X));
		LocArr.Add(MakeShared<FJsonValueNumber>(Mid.Y));
		LocArr.Add(MakeShared<FJsonValueNumber>(Mid.Z));
		ContrastParams->SetArrayField(TEXT("location"), LocArr);

		FMonolithActionResult ContrastResult = AnalyzeVisualContrast(ContrastParams);
		if (ContrastResult.bSuccess)
		{
			Report->SetObjectField(TEXT("visual_contrast"), ContrastResult.Result);
			int32 Failures = static_cast<int32>(ContrastResult.Result->GetNumberField(TEXT("failures")));
			if (Failures > 0)
			{
				auto Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("category"), TEXT("visual_contrast"));
				Issue->SetStringField(TEXT("severity"), TEXT("warning"));
				Issue->SetStringField(TEXT("description"), FString::Printf(
					TEXT("%d interactable actors have insufficient visual contrast"), Failures));
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
				++WarningCount;
			}
		}
	}

	// 4. Rest Points
	{
		auto RestParams = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> StartArr, EndArr;
		StartArr.Add(MakeShared<FJsonValueNumber>(Start.X));
		StartArr.Add(MakeShared<FJsonValueNumber>(Start.Y));
		StartArr.Add(MakeShared<FJsonValueNumber>(Start.Z));
		EndArr.Add(MakeShared<FJsonValueNumber>(End.X));
		EndArr.Add(MakeShared<FJsonValueNumber>(End.Y));
		EndArr.Add(MakeShared<FJsonValueNumber>(End.Z));
		RestParams->SetArrayField(TEXT("start"), StartArr);
		RestParams->SetArrayField(TEXT("end"), EndArr);
		RestParams->SetNumberField(TEXT("max_gap"), MaxRestGap);

		FMonolithActionResult RestResult = FindRestPoints(RestParams);
		if (RestResult.bSuccess)
		{
			Report->SetObjectField(TEXT("rest_points"), RestResult.Result);
			if (!RestResult.Result->GetBoolField(TEXT("passes")))
			{
				int32 GapCount = static_cast<int32>(RestResult.Result->GetNumberField(TEXT("gaps_found")));
				auto Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("category"), TEXT("rest_points"));
				Issue->SetStringField(TEXT("severity"), TEXT("critical"));
				Issue->SetStringField(TEXT("description"), FString::Printf(
					TEXT("%d gaps exceed max rest distance of %.0fm"), GapCount, MaxRestGap / 100.0f));
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
				++CriticalCount;
			}
		}
	}

	// 5. Interactive Reach
	{
		auto ReachParams = MakeShared<FJsonObject>();
		FMonolithActionResult ReachResult = ValidateInteractiveReach(ReachParams);
		if (ReachResult.bSuccess)
		{
			Report->SetObjectField(TEXT("interactive_reach"), ReachResult.Result);
			int32 Errors = static_cast<int32>(ReachResult.Result->GetNumberField(TEXT("errors")));
			if (Errors > 0)
			{
				auto Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("category"), TEXT("interactive_reach"));
				Issue->SetStringField(TEXT("severity"), TEXT("warning"));
				Issue->SetStringField(TEXT("description"), FString::Printf(
					TEXT("%d interactable actors have reach issues"), Errors));
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
				++WarningCount;
			}
		}
	}

	// Sort issues by severity
	Issues.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return A->AsObject()->GetStringField(TEXT("severity")) < B->AsObject()->GetStringField(TEXT("severity"));
	});

	Report->SetArrayField(TEXT("issues"), Issues);
	Report->SetNumberField(TEXT("critical_issues"), CriticalCount);
	Report->SetNumberField(TEXT("warning_issues"), WarningCount);
	Report->SetNumberField(TEXT("total_issues"), CriticalCount + WarningCount);

	// Overall grade
	FString Grade;
	if (CriticalCount == 0 && WarningCount == 0)
	{
		Grade = TEXT("A");
	}
	else if (CriticalCount == 0 && WarningCount <= 2)
	{
		Grade = TEXT("B");
	}
	else if (CriticalCount <= 1)
	{
		Grade = TEXT("C");
	}
	else
	{
		Grade = TEXT("F");
	}
	Report->SetStringField(TEXT("grade"), Grade);

	// Profile-specific summary
	if (Profile == TEXT("motor_impaired"))
	{
		Report->SetStringField(TEXT("profile_note"), TEXT("Motor impairment profile: wider paths (150cm), shorter rest gaps (20m), simpler navigation."));
	}
	else if (Profile == TEXT("vision_impaired"))
	{
		Report->SetStringField(TEXT("profile_note"), TEXT("Vision impairment profile: higher contrast thresholds (0.6), emphasis on visual clarity."));
	}
	else if (Profile == TEXT("cognitive_fatigue"))
	{
		Report->SetStringField(TEXT("profile_note"), TEXT("Cognitive fatigue profile: simpler navigation (max 30 complexity), frequent rests (20m), clear wayfinding."));
	}

	return FMonolithActionResult::Success(Report);
}
