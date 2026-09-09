#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Math/RandomStream.h"
#include "MonolithToolRegistry.h"

/**
 * Phase 18: Context-Aware Prop Placement (8 actions)
 * Intelligent prop placement that understands surfaces, disturbance states,
 * physics configuration, and prop kits. Extends the blockout system with
 * game-ready prop workflows for survival horror level design.
 *
 * Actions:
 *   scatter_on_surface   — Place props ON specific surfaces (shelf tops, table tops, etc.)
 *   set_room_disturbance — Apply progressive disorder transforms to placed props
 *   configure_physics_props — Batch-configure SimulatePhysics + sleep states
 *   settle_props          — Trace-based gravity settle without PIE
 *   create_prop_kit       — Author a prop kit JSON file
 *   place_prop_kit        — Spawn a prop kit at a world location
 *   scatter_on_walls      — Horizontal traces to find walls, place aligned props
 *   scatter_on_ceiling    — Upward traces to find ceiling, place hanging props
 */
class FMonolithMeshContextPropActions
{
public:
	/** Register all 8 context-aware prop actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// --- Surface scatter ---
	static FMonolithActionResult ScatterOnSurface(const TSharedPtr<FJsonObject>& Params);

	// --- Disturbance ---
	static FMonolithActionResult SetRoomDisturbance(const TSharedPtr<FJsonObject>& Params);

	// --- Physics ---
	static FMonolithActionResult ConfigurePhysicsProps(const TSharedPtr<FJsonObject>& Params);

	// --- Settle ---
	static FMonolithActionResult SettleProps(const TSharedPtr<FJsonObject>& Params);

	// --- Prop kits ---
	static FMonolithActionResult CreatePropKit(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult PlacePropKit(const TSharedPtr<FJsonObject>& Params);

	// --- Wall/ceiling scatter ---
	static FMonolithActionResult ScatterOnWalls(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ScatterOnCeiling(const TSharedPtr<FJsonObject>& Params);

	// --- Helpers ---

	/** Get the prop kits directory path (creates if missing) */
	static FString GetPropKitsDirectory();

	/** Load a prop kit JSON file by name, returns nullptr and sets OutError on failure */
	static TSharedPtr<FJsonObject> LoadPropKit(const FString& KitName, FString& OutError);

	/** Save a prop kit JSON file */
	static bool SavePropKit(const FString& KitName, const TSharedPtr<FJsonObject>& KitJson, FString& OutError);

	/** Build a JSON array from FVector */
	static TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V);

	/** Build a JSON array from FRotator */
	static TArray<TSharedPtr<FJsonValue>> RotatorToJsonArray(const FRotator& R);

	/** Parse a 3-element JSON array to FVector */
	static bool ParseJsonArrayToVector(const TArray<TSharedPtr<FJsonValue>>& Arr, FVector& Out);

	/** Poisson disk sampling on a 2D rectangular region. Returns sample positions in local 2D coords. */
	static TArray<FVector2D> PoissonDiskSample2D(float Width, float Height, float MinSpacing, int32 MaxCount, FRandomStream& RandStream);

	/** Find all actors with a given Monolith.Owner:{volume} tag */
	static TArray<AActor*> FindActorsWithOwnerTag(UWorld* World, const FString& VolumeName);

	/** Scoped undo transaction that respects FMonolithMeshSceneActions::bBatchTransactionActive */
	struct FScopedMeshTransaction
	{
		bool bOwnsTransaction;

		FScopedMeshTransaction(const FText& Description);
		~FScopedMeshTransaction();
		void Cancel();
	};
};
