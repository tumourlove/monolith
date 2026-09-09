#pragma once

#include "CoreMinimal.h"
#include "Engine/HitResult.h"
#include "Engine/EngineTypes.h"
#include "MonolithToolRegistry.h"

/**
 * Phase 3: Scene Spatial Queries (11 actions)
 * Physics-based world queries - raycasts, overlaps, spatial relationships.
 * Enables AI spatial reasoning about the scene.
 * All queries work in editor without a play session.
 */
class FMonolithMeshSpatialActions
{
public:
	/** Register all 11 spatial query actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// --- Raycasts ---
	static FMonolithActionResult QueryRaycast(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult QueryMultiRaycast(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult QueryRadialSweep(const TSharedPtr<FJsonObject>& Params);

	// --- Overlaps ---
	static FMonolithActionResult QueryOverlap(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult QueryNearest(const TSharedPtr<FJsonObject>& Params);

	// --- Line of sight ---
	static FMonolithActionResult QueryLineOfSight(const TSharedPtr<FJsonObject>& Params);

	// --- Volume / bounds ---
	static FMonolithActionResult GetActorsInVolume(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetSceneBounds(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetSceneStatistics(const TSharedPtr<FJsonObject>& Params);

	// --- Spatial analysis ---
	static FMonolithActionResult GetSpatialRelationships(const TSharedPtr<FJsonObject>& Params);

	// --- Navigation ---
	static FMonolithActionResult QueryNavmesh(const TSharedPtr<FJsonObject>& Params);

	// --- Helpers ---
	static ECollisionChannel ParseCollisionChannel(const FString& ChannelName, bool& bSuccess);
	static TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V);
	static TSharedPtr<FJsonObject> HitResultToJson(const FHitResult& Hit);
};
