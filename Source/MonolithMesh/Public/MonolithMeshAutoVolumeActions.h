#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MonolithToolRegistry.h"

/**
 * SP7: Auto-Volume Generation — Automatic NavMesh, Audio, Trigger, and NavLink
 * volume spawning for procedurally generated buildings and city blocks.
 *
 * Uses the SP6 Spatial Registry to read building/room/door data, then delegates
 * to the existing spawn_volume action (via the tool registry) and spawns
 * ANavLinkProxy actors for door connections.
 *
 * 3 actions:
 *   auto_volumes_for_building  — Volumes for one building (navmesh, audio, triggers, nav links)
 *   auto_volumes_for_block     — Volumes for all buildings in a block + block-level navmesh
 *   spawn_nav_link             — Spawn a NavLinkProxy between two points
 */
class FMonolithMeshAutoVolumeActions
{
public:
	/** Register all 3 auto-volume actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// ---- Action Handlers ----
	static FMonolithActionResult AutoVolumesForBuilding(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AutoVolumesForBlock(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SpawnNavLink(const TSharedPtr<FJsonObject>& Params);

	// ---- Helpers ----

	/** Determine reverb preset name from room type and area (m²) */
	static FString DetermineReverbPreset(
		const FString& RoomType, float AreaSquareMeters,
		const FString& SmallPreset, const FString& MediumPreset,
		const FString& LargePreset, const FString& CorridorPreset);

	/** Compute the union AABB of all rooms in a building */
	static FBox ComputeBuildingBounds(const struct FSpatialBlock& Block, const struct FSpatialBuilding& Building);

	/** Get the wall normal direction from a wall string ("north", "south", "east", "west") */
	static FVector WallToDirection(const FString& Wall);

	/** Spawn a NavLinkProxy via dynamic class loading (avoids AIModule compile dependency) */
	static AActor* SpawnNavLinkActor(UWorld* World, const FVector& Location, const FVector& LeftPoint,
		const FVector& RightPoint, bool bBidirectional, const FString& Label, const FString& Folder);
};
