#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "MonolithToolRegistry.h"

/**
 * SP10: Room Furnishing Pipeline
 *
 * 3 actions:
 *   furnish_room         - Furnish a single room by type + bounds using parametric mesh generation
 *   furnish_building     - Furnish all rooms in a spatial registry building
 *   list_furniture_presets - List available furniture preset configurations
 *
 * Delegates mesh creation to existing create_parametric_mesh via FMonolithToolRegistry::ExecuteAction.
 * Reads room data from SP6 spatial registry (FMonolithMeshSpatialRegistry).
 * Furniture presets loaded from JSON files in Saved/Monolith/FurniturePresets/.
 */
class FMonolithMeshFurnishingActions
{
public:
	/** Register all 3 furnishing actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// ---- Action Handlers ----

	static FMonolithActionResult FurnishRoom(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult FurnishBuilding(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListFurniturePresets(const TSharedPtr<FJsonObject>& Params);

	// ---- Preset Loading ----

	/** Get the presets directory path */
	static FString GetPresetsDirectory();

	/** Load a furniture preset JSON by room type. Returns nullptr if not found. */
	static TSharedPtr<FJsonObject> LoadPreset(const FString& RoomType);

	/** Get all available preset names (from filesystem) */
	static TArray<FString> GetAvailablePresets();

	// ---- Placement Types ----

	enum class EPlacementZone : uint8
	{
		WallAligned,
		Center,
		Corner,
		Near
	};

	enum class EWallRule : uint8
	{
		LongestWall,
		BackWall,
		EntranceWall,
		OppositeLongest,
		ShortestWall,
		Any
	};

	enum class EPlacementType : uint8
	{
		Floor,       // Default — placed on floor
		WallMount,   // Mounted on wall at mount_height
	};

	/** Parsed furniture item from preset JSON */
	struct FFurnitureItem
	{
		FString Type;          // Parametric mesh type (box, chair, table, etc.)
		FString Name;          // Human-readable name (counter, fridge, etc.)
		float Width = 100.0f;
		float Depth = 100.0f;
		float Height = 100.0f;
		EPlacementZone Placement = EPlacementZone::Center;
		EWallRule Wall = EWallRule::Any;
		int32 CountMin = 1;
		int32 CountMax = 1;
		float Spacing = 0.0f;
		bool bStretchToWall = false;
		int32 MaterialSlot = -1;
		FString NearItem;      // For EPlacementZone::Near — name of item to be near
		EPlacementType PlacementType = EPlacementType::Floor;
		float MountHeight = 0.0f;  // For WallMount: center Z offset from floor (cm)
	};

	/** Room wall geometry computed from bounds */
	struct FRoomWalls
	{
		FVector RoomMin;
		FVector RoomMax;
		FVector RoomCenter;
		float RoomWidth;   // X extent
		float RoomDepth;   // Y extent
		float RoomHeight;  // Z extent

		// Wall midpoints and normals (inward-facing)
		// Index: 0=MinY (south), 1=MaxY (north), 2=MinX (west), 3=MaxX (east)
		struct FWallInfo
		{
			FVector Center;
			FVector Normal;    // Inward-facing
			float Length;
			FVector Start;     // Wall start corner
			FVector End;       // Wall end corner
			bool bIsDoorWall = false;
		};
		TArray<FWallInfo> Walls; // 4 walls

		/** Identify which wall index is longest, shortest, entrance (nearest door), back (opposite entrance) */
		int32 LongestWallIndex = 0;
		int32 ShortestWallIndex = 0;
		int32 EntranceWallIndex = 0;
		int32 BackWallIndex = 0;
	};

	// ---- Placement Engine ----

	/** Compute wall geometry from room bounds and door positions */
	static FRoomWalls ComputeRoomWalls(const FBox& Bounds, const TArray<FVector>& DoorPositions);

	/** Resolve a wall rule to a wall index */
	static int32 ResolveWallIndex(EWallRule Rule, const FRoomWalls& Walls);

	/** Parse placement zone from string */
	static EPlacementZone ParsePlacementZone(const FString& Str);

	/** Parse wall rule from string */
	static EWallRule ParseWallRule(const FString& Str);

	/** Parse a furniture item from preset JSON */
	static FFurnitureItem ParseFurnitureItem(const TSharedPtr<FJsonObject>& ItemJson);

	/** Compute world position + rotation for a furniture item placement */
	static bool ComputePlacement(
		const FFurnitureItem& Item,
		const FRoomWalls& Walls,
		const TArray<FVector>& DoorPositions,
		const TArray<FBox>& OccupiedBoxes,
		const TMap<FString, FVector>& PlacedItems,
		FVector& OutPosition,
		FRotator& OutRotation,
		FVector& OutDimensions,
		FRandomStream& Rng
	);

	/** Check if a box collides with any occupied boxes or door zones */
	static bool CheckCollision(
		const FBox& CandidateBox,
		const TArray<FBox>& OccupiedBoxes,
		const TArray<FVector>& DoorPositions,
		float DoorClearance = 100.0f
	);

	/** Apply density overrides to the item list */
	static void ApplyDensityOverrides(
		TArray<FFurnitureItem>& Items,
		const TSharedPtr<FJsonObject>& Preset,
		const FString& Density
	);

	/** Apply decay (horror disturbance) to placed items */
	static void ApplyDecay(
		TArray<TSharedPtr<FJsonObject>>& PlacedItems,
		float DecayAmount,
		FRandomStream& Rng
	);

	/** Parse a [x,y,z] JSON array to FVector */
	static bool ParseVec3(const TArray<TSharedPtr<FJsonValue>>& Arr, FVector& Out);

	/** Parse world_bounds object {min:[x,y,z], max:[x,y,z]} to FBox */
	static bool ParseBoundsObject(const TSharedPtr<FJsonObject>& BoundsObj, FBox& Out);

	/** Create the parametric mesh params JSON for a furniture item */
	static TSharedPtr<FJsonObject> BuildParametricMeshParams(
		const FFurnitureItem& Item,
		const FVector& Position,
		const FRotator& Rotation,
		const FVector& Dimensions,
		const FString& SavePathPrefix,
		const FString& Folder,
		int32 Index
	);
};
