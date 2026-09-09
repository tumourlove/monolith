#pragma once

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "MonolithToolRegistry.h"

class ADecalActor;

/**
 * Phase 10: Decal & Detail Placement (4 actions)
 * Environmental storytelling via decals, path patterns, and horror scene presets.
 * Surface-aligned decal spawning, Catmull-Rom path interpolation,
 * prop density analysis, and parameterized horror scene generation.
 */
class FMonolithMeshDecalActions
{
public:
	/** Register all 4 decal/detail actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// --- Decal placement ---
	static FMonolithActionResult PlaceDecals(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult PlaceAlongPath(const TSharedPtr<FJsonObject>& Params);

	// --- Analysis ---
	static FMonolithActionResult AnalyzePropDensity(const TSharedPtr<FJsonObject>& Params);

	// --- Storytelling ---
	static FMonolithActionResult PlaceStorytellingScene(const TSharedPtr<FJsonObject>& Params);

	// --- Helpers ---

	/** Validate that a material has MD_DeferredDecal domain */
	static bool ValidateDecalMaterial(const FString& MaterialPath, FString& OutError);

	/** Spawn a single decal actor aligned to a surface via line trace */
	static ADecalActor* SpawnAlignedDecal(
		UWorld* World,
		class UMaterialInterface* Material,
		const FVector& Location,
		const FVector& DecalSize,
		float RandomYaw,
		const FCollisionQueryParams& TraceParams);

	/** Catmull-Rom interpolation between 4 control points at parameter t */
	static FVector CatmullRom(const FVector& P0, const FVector& P1, const FVector& P2, const FVector& P3, float t);

	/** Sample evenly-spaced points along a Catmull-Rom spline defined by path_points */
	static TArray<FVector> SampleSplinePath(const TArray<FVector>& PathPoints, float Spacing);

	/** Build a JSON array from FVector */
	static TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V);

	/** Scoped undo transaction that respects FMonolithMeshSceneActions::bBatchTransactionActive */
	struct FScopedMeshTransaction
	{
		bool bOwnsTransaction;

		FScopedMeshTransaction(const FText& Description);
		~FScopedMeshTransaction();
		void Cancel();
	};
};
