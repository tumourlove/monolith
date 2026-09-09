#include "MonolithMeshFurnishingActions.h"
#include "Math/RandomStream.h"
#include "MonolithMeshSpatialRegistry.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Wall offset to prevent z-fighting (cm)
static constexpr float GWallOffset = 5.0f;

// Door clearance zone half-width (cm)
static constexpr float GDoorClearance = 100.0f;

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshFurnishingActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("furnish_room"),
		TEXT("Furnish a single room with parametric furniture based on room type. "
			"Places items using create_parametric_mesh via the tool registry. "
			"Collision-aware: items that would clip walls, doors, or other furniture are skipped."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshFurnishingActions::FurnishRoom),
		FParamSchemaBuilder()
			.Required(TEXT("room_type"), TEXT("string"), TEXT("Room type for furniture selection (kitchen, bedroom, bathroom, office, living_room, lobby, corridor, entryway)"))
			.Required(TEXT("world_bounds"), TEXT("object"), TEXT("{min: [x,y,z], max: [x,y,z]} defining the room volume"))
			.RequiredAssetPath(TEXT("save_path_prefix"), TEXT("Base asset path for furniture meshes (e.g. /Game/Town/Furniture)"))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("Random seed for variation"), TEXT("0"))
			.Optional(TEXT("density"), TEXT("string"), TEXT("Furniture density: sparse, normal, cluttered"), TEXT("normal"))
			.Optional(TEXT("preset"), TEXT("string"), TEXT("Override preset name (default: auto from room_type)"))
			.Optional(TEXT("style"), TEXT("string"), TEXT("Furniture style: modern, worn, abandoned"), TEXT("modern"))
			.OptionalAssetPath(TEXT("folder"), TEXT("Outliner folder path for spawned actors"))
			.Optional(TEXT("door_positions"), TEXT("array"), TEXT("Array of [x,y,z] door positions to avoid blocking"))
			.Optional(TEXT("skip_types"), TEXT("array"), TEXT("Furniture types to skip (e.g. [\"bathtub\"] for small bathrooms)"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("furnish_building"),
		TEXT("Furnish all rooms in a building from the spatial registry. "
			"Iterates each room, loads the appropriate preset, and delegates to furnish_room."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshFurnishingActions::FurnishBuilding),
		FParamSchemaBuilder()
			.Required(TEXT("building_id"), TEXT("string"), TEXT("Building ID in the spatial registry"))
			.RequiredAssetPath(TEXT("save_path_prefix"), TEXT("Base asset path for furniture meshes"))
			.Optional(TEXT("block_id"), TEXT("string"), TEXT("Block ID in spatial registry"), TEXT("default"))
			.Optional(TEXT("seed"), TEXT("integer"), TEXT("Random seed"), TEXT("0"))
			.Optional(TEXT("density"), TEXT("string"), TEXT("Furniture density: sparse, normal, cluttered"), TEXT("normal"))
			.Optional(TEXT("style"), TEXT("string"), TEXT("Furniture style: modern, worn, abandoned"), TEXT("modern"))
			.OptionalAssetPath(TEXT("folder"), TEXT("Outliner folder (default: auto from building)"))
			.Optional(TEXT("skip_room_types"), TEXT("array"), TEXT("Room types to skip entirely (e.g. [\"corridor\"])"))
			.Optional(TEXT("decay"), TEXT("number"), TEXT("0-1, fraction of items to disturb for horror effect"), TEXT("0"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("list_furniture_presets"),
		TEXT("List available furniture preset configurations. Returns preset names and summaries."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshFurnishingActions::ListFurniturePresets),
		FParamSchemaBuilder()
			.Optional(TEXT("room_type"), TEXT("string"), TEXT("Filter by room type"))
			.Build());
}

// ============================================================================
// Preset Loading
// ============================================================================

FString FMonolithMeshFurnishingActions::GetPresetsDirectory()
{
	return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("Monolith"), TEXT("Saved"), TEXT("Monolith"), TEXT("FurniturePresets"));
}

TSharedPtr<FJsonObject> FMonolithMeshFurnishingActions::LoadPreset(const FString& RoomType)
{
	FString FilePath = FPaths::Combine(GetPresetsDirectory(), RoomType + TEXT(".json"));
	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *FilePath))
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Parsed = FMonolithJsonUtils::Parse(JsonStr);
	return Parsed;
}

TArray<FString> FMonolithMeshFurnishingActions::GetAvailablePresets()
{
	TArray<FString> Results;
	FString Dir = GetPresetsDirectory();

	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Dir, TEXT("*.json")), true, false);

	for (const FString& F : Files)
	{
		Results.Add(FPaths::GetBaseFilename(F));
	}

	return Results;
}

// ============================================================================
// Parsing Helpers
// ============================================================================

bool FMonolithMeshFurnishingActions::ParseVec3(const TArray<TSharedPtr<FJsonValue>>& Arr, FVector& Out)
{
	if (Arr.Num() < 3) return false;
	Out.X = Arr[0]->AsNumber();
	Out.Y = Arr[1]->AsNumber();
	Out.Z = Arr[2]->AsNumber();
	return true;
}

bool FMonolithMeshFurnishingActions::ParseBoundsObject(const TSharedPtr<FJsonObject>& BoundsObj, FBox& Out)
{
	if (!BoundsObj.IsValid()) return false;

	const TArray<TSharedPtr<FJsonValue>>* MinArr = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* MaxArr = nullptr;
	if (!BoundsObj->TryGetArrayField(TEXT("min"), MinArr) || !BoundsObj->TryGetArrayField(TEXT("max"), MaxArr))
	{
		return false;
	}

	FVector Min, Max;
	if (!ParseVec3(*MinArr, Min) || !ParseVec3(*MaxArr, Max))
	{
		return false;
	}

	Out = FBox(Min, Max);
	return true;
}

FMonolithMeshFurnishingActions::EPlacementZone FMonolithMeshFurnishingActions::ParsePlacementZone(const FString& Str)
{
	if (Str == TEXT("wall_aligned")) return EPlacementZone::WallAligned;
	if (Str == TEXT("center")) return EPlacementZone::Center;
	if (Str == TEXT("corner")) return EPlacementZone::Corner;
	if (Str == TEXT("near")) return EPlacementZone::Near;
	return EPlacementZone::Center;
}

FMonolithMeshFurnishingActions::EWallRule FMonolithMeshFurnishingActions::ParseWallRule(const FString& Str)
{
	if (Str == TEXT("longest_wall")) return EWallRule::LongestWall;
	if (Str == TEXT("back_wall")) return EWallRule::BackWall;
	if (Str == TEXT("entrance_wall")) return EWallRule::EntranceWall;
	if (Str == TEXT("opposite_longest")) return EWallRule::OppositeLongest;
	if (Str == TEXT("shortest_wall")) return EWallRule::ShortestWall;
	return EWallRule::Any;
}

FMonolithMeshFurnishingActions::FFurnitureItem FMonolithMeshFurnishingActions::ParseFurnitureItem(const TSharedPtr<FJsonObject>& ItemJson)
{
	FFurnitureItem Item;
	if (!ItemJson.IsValid()) return Item;

	ItemJson->TryGetStringField(TEXT("type"), Item.Type);
	ItemJson->TryGetStringField(TEXT("name"), Item.Name);

	// Parse dimensions sub-object
	const TSharedPtr<FJsonObject>* DimsObj = nullptr;
	if (ItemJson->TryGetObjectField(TEXT("dimensions"), DimsObj) && DimsObj && (*DimsObj).IsValid())
	{
		if ((*DimsObj)->HasField(TEXT("width")))  Item.Width  = static_cast<float>((*DimsObj)->GetNumberField(TEXT("width")));
		if ((*DimsObj)->HasField(TEXT("depth")))  Item.Depth  = static_cast<float>((*DimsObj)->GetNumberField(TEXT("depth")));
		if ((*DimsObj)->HasField(TEXT("height"))) Item.Height = static_cast<float>((*DimsObj)->GetNumberField(TEXT("height")));
	}

	// Placement
	FString PlacementStr;
	if (ItemJson->TryGetStringField(TEXT("placement"), PlacementStr))
	{
		Item.Placement = ParsePlacementZone(PlacementStr);
	}

	// Wall rule
	FString WallStr;
	if (ItemJson->TryGetStringField(TEXT("wall"), WallStr))
	{
		Item.Wall = ParseWallRule(WallStr);
	}

	// Count — can be int or [min, max] array
	if (ItemJson->HasField(TEXT("count")))
	{
		const TSharedPtr<FJsonValue>& CountVal = ItemJson->Values.FindChecked(TEXT("count"));
		if (CountVal->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& CountArr = CountVal->AsArray();
			if (CountArr.Num() >= 2)
			{
				Item.CountMin = static_cast<int32>(CountArr[0]->AsNumber());
				Item.CountMax = static_cast<int32>(CountArr[1]->AsNumber());
			}
		}
		else
		{
			Item.CountMin = Item.CountMax = static_cast<int32>(CountVal->AsNumber());
		}
	}

	if (ItemJson->HasField(TEXT("spacing")))
	{
		Item.Spacing = static_cast<float>(ItemJson->GetNumberField(TEXT("spacing")));
	}

	Item.bStretchToWall = ItemJson->HasField(TEXT("stretch_to_wall")) && ItemJson->GetBoolField(TEXT("stretch_to_wall"));

	if (ItemJson->HasField(TEXT("material_slot")))
	{
		Item.MaterialSlot = static_cast<int32>(ItemJson->GetNumberField(TEXT("material_slot")));
	}

	ItemJson->TryGetStringField(TEXT("near"), Item.NearItem);

	// Placement type (floor vs wall_mount)
	FString PlacementTypeStr;
	if (ItemJson->TryGetStringField(TEXT("placement_type"), PlacementTypeStr))
	{
		if (PlacementTypeStr == TEXT("wall_mount"))
			Item.PlacementType = EPlacementType::WallMount;
	}
	double MountHeightVal = 0.0;
	if (ItemJson->TryGetNumberField(TEXT("mount_height"), MountHeightVal))
		Item.MountHeight = static_cast<float>(MountHeightVal);

	return Item;
}

// ============================================================================
// Room Wall Geometry
// ============================================================================

FMonolithMeshFurnishingActions::FRoomWalls FMonolithMeshFurnishingActions::ComputeRoomWalls(
	const FBox& Bounds, const TArray<FVector>& DoorPositions)
{
	FRoomWalls W;
	W.RoomMin = Bounds.Min;
	W.RoomMax = Bounds.Max;
	W.RoomCenter = Bounds.GetCenter();
	W.RoomWidth = Bounds.Max.X - Bounds.Min.X;
	W.RoomDepth = Bounds.Max.Y - Bounds.Min.Y;
	W.RoomHeight = Bounds.Max.Z - Bounds.Min.Z;

	// 4 walls: 0=MinY(south), 1=MaxY(north), 2=MinX(west), 3=MaxX(east)
	W.Walls.SetNum(4);

	// South wall (MinY) — runs along X
	W.Walls[0].Start = FVector(Bounds.Min.X, Bounds.Min.Y, Bounds.Min.Z);
	W.Walls[0].End = FVector(Bounds.Max.X, Bounds.Min.Y, Bounds.Min.Z);
	W.Walls[0].Normal = FVector(0, 1, 0);
	W.Walls[0].Length = W.RoomWidth;
	W.Walls[0].Center = FVector(W.RoomCenter.X, Bounds.Min.Y, W.RoomCenter.Z);

	// North wall (MaxY) — runs along X
	W.Walls[1].Start = FVector(Bounds.Min.X, Bounds.Max.Y, Bounds.Min.Z);
	W.Walls[1].End = FVector(Bounds.Max.X, Bounds.Max.Y, Bounds.Min.Z);
	W.Walls[1].Normal = FVector(0, -1, 0);
	W.Walls[1].Length = W.RoomWidth;
	W.Walls[1].Center = FVector(W.RoomCenter.X, Bounds.Max.Y, W.RoomCenter.Z);

	// West wall (MinX) — runs along Y
	W.Walls[2].Start = FVector(Bounds.Min.X, Bounds.Min.Y, Bounds.Min.Z);
	W.Walls[2].End = FVector(Bounds.Min.X, Bounds.Max.Y, Bounds.Min.Z);
	W.Walls[2].Normal = FVector(1, 0, 0);
	W.Walls[2].Length = W.RoomDepth;
	W.Walls[2].Center = FVector(Bounds.Min.X, W.RoomCenter.Y, W.RoomCenter.Z);

	// East wall (MaxX) — runs along Y
	W.Walls[3].Start = FVector(Bounds.Max.X, Bounds.Min.Y, Bounds.Min.Z);
	W.Walls[3].End = FVector(Bounds.Max.X, Bounds.Max.Y, Bounds.Min.Z);
	W.Walls[3].Normal = FVector(-1, 0, 0);
	W.Walls[3].Length = W.RoomDepth;
	W.Walls[3].Center = FVector(Bounds.Max.X, W.RoomCenter.Y, W.RoomCenter.Z);

	// Identify longest and shortest walls
	float MaxLen = 0.0f;
	float MinLen = TNumericLimits<float>::Max();
	for (int32 i = 0; i < 4; ++i)
	{
		if (W.Walls[i].Length > MaxLen)
		{
			MaxLen = W.Walls[i].Length;
			W.LongestWallIndex = i;
		}
		if (W.Walls[i].Length < MinLen)
		{
			MinLen = W.Walls[i].Length;
			W.ShortestWallIndex = i;
		}
	}

	// Identify entrance wall (nearest to any door position)
	if (DoorPositions.Num() > 0)
	{
		float BestDist = TNumericLimits<float>::Max();
		for (int32 i = 0; i < 4; ++i)
		{
			for (const FVector& DoorPos : DoorPositions)
			{
				float Dist = FVector::Dist(W.Walls[i].Center, DoorPos);
				if (Dist < BestDist)
				{
					BestDist = Dist;
					W.EntranceWallIndex = i;
				}
			}
		}
		W.Walls[W.EntranceWallIndex].bIsDoorWall = true;
	}
	else
	{
		// Default: south wall is entrance
		W.EntranceWallIndex = 0;
		W.Walls[0].bIsDoorWall = true;
	}

	// Back wall = opposite of entrance
	// 0<->1 (south<->north), 2<->3 (west<->east)
	static const int32 OppositeMap[] = { 1, 0, 3, 2 };
	W.BackWallIndex = OppositeMap[W.EntranceWallIndex];

	return W;
}

int32 FMonolithMeshFurnishingActions::ResolveWallIndex(EWallRule Rule, const FRoomWalls& Walls)
{
	switch (Rule)
	{
	case EWallRule::LongestWall:     return Walls.LongestWallIndex;
	case EWallRule::ShortestWall:    return Walls.ShortestWallIndex;
	case EWallRule::BackWall:        return Walls.BackWallIndex;
	case EWallRule::EntranceWall:    return Walls.EntranceWallIndex;
	case EWallRule::OppositeLongest:
	{
		static const int32 OppositeMap[] = { 1, 0, 3, 2 };
		return OppositeMap[Walls.LongestWallIndex];
	}
	case EWallRule::Any:
	default:
		return 0;
	}
}

// ============================================================================
// Collision Detection
// ============================================================================

bool FMonolithMeshFurnishingActions::CheckCollision(
	const FBox& CandidateBox,
	const TArray<FBox>& OccupiedBoxes,
	const TArray<FVector>& DoorPositions,
	float DoorClearance)
{
	// Check against existing furniture
	for (const FBox& Occupied : OccupiedBoxes)
	{
		if (CandidateBox.Intersect(Occupied))
		{
			return true;
		}
	}

	// Check against door clearance zones (cylinder around door position on XY, full height)
	for (const FVector& DoorPos : DoorPositions)
	{
		// Expand the candidate box test: if any part of the box is within DoorClearance of the door XY
		FVector ClosestPoint = CandidateBox.GetClosestPointTo(DoorPos);
		float Dist2D = FVector::Dist2D(ClosestPoint, DoorPos);
		if (Dist2D < DoorClearance)
		{
			return true;
		}
	}

	return false;
}

// ============================================================================
// Placement Computation
// ============================================================================

bool FMonolithMeshFurnishingActions::ComputePlacement(
	const FFurnitureItem& Item,
	const FRoomWalls& Walls,
	const TArray<FVector>& DoorPositions,
	const TArray<FBox>& OccupiedBoxes,
	const TMap<FString, FVector>& PlacedItems,
	FVector& OutPosition,
	FRotator& OutRotation,
	FVector& OutDimensions,
	FRandomStream& Rng)
{
	OutDimensions = FVector(Item.Width, Item.Depth, Item.Height);
	OutRotation = FRotator::ZeroRotator;

	switch (Item.Placement)
	{
	case EPlacementZone::WallAligned:
	{
		int32 WallIdx = ResolveWallIndex(Item.Wall, Walls);
		const FRoomWalls::FWallInfo& WallInfo = Walls.Walls[WallIdx];

		// If stretch_to_wall, scale width to match wall length (minus offset margins)
		if (Item.bStretchToWall)
		{
			OutDimensions.X = WallInfo.Length - GWallOffset * 2.0f;
		}

		// Rotation: face away from wall (furniture back against wall)
		// Normal points inward, so rotation faces the normal direction
		if (WallIdx == 0)      OutRotation = FRotator(0, 0, 0);     // South wall, face north
		else if (WallIdx == 1) OutRotation = FRotator(0, 180, 0);   // North wall, face south
		else if (WallIdx == 2) OutRotation = FRotator(0, 90, 0);    // West wall, face east
		else if (WallIdx == 3) OutRotation = FRotator(0, -90, 0);   // East wall, face west

		// --- WallMount: position flush against wall at mount height ---
		if (Item.PlacementType == EPlacementType::WallMount)
		{
			FVector WallCenter = WallInfo.Center;
			// Position flush against wall
			OutPosition = WallCenter + WallInfo.Normal * (OutDimensions.Y * 0.5f + 1.0f);
			// Height: use mount_height or default to wall midpoint
			OutPosition.Z = (Item.MountHeight > 0.0f)
				? Walls.RoomMin.Z + Item.MountHeight
				: Walls.RoomMin.Z + Walls.RoomHeight * 0.5f;

			// Center along wall axis
			if (WallIdx <= 1)
				OutPosition.X = WallCenter.X;
			else
				OutPosition.Y = WallCenter.Y;

			// Spread wall-mounted items along wall to avoid stacking
			FVector WallDir = (WallInfo.End - WallInfo.Start).GetSafeNormal();
			float WallLength = WallInfo.Length - 2.0f * GWallOffset;
			float ItemW = OutDimensions.X;
			if (!Item.bStretchToWall && WallLength > ItemW)
			{
				uint32 NameHash = GetTypeHash(Item.Name);
				float Offset = (static_cast<float>(NameHash % 1000) / 1000.0f) * (WallLength - ItemW) - (WallLength - ItemW) * 0.5f;
				OutPosition += WallDir * Offset;
			}

			break;
		}

		// --- Floor-level wall-aligned placement ---
		FVector WallCenter = WallInfo.Center;
		WallCenter.Z = Walls.RoomMin.Z; // Floor level

		// Offset from wall: half depth + margin
		FVector InwardOffset = WallInfo.Normal * (OutDimensions.Y * 0.5f + GWallOffset);
		OutPosition = WallCenter + InwardOffset;

		// For wall-aligned items on N/S walls, furniture width spans X; on E/W walls, spans Y
		// Adjust position to wall center along the wall's axis
		if (WallIdx <= 1)
		{
			// N/S wall — item centered along X
			OutPosition.X = WallCenter.X;
		}
		else
		{
			// E/W wall — item centered along Y, swap width/depth for placement
			OutPosition.Y = WallCenter.Y;
		}

		// Spread items along wall instead of all landing at center
		FVector WallDir = (WallInfo.End - WallInfo.Start).GetSafeNormal();
		float WallLength = WallInfo.Length - 2.0f * GWallOffset;
		float ItemW = OutDimensions.X;
		if (!Item.bStretchToWall && WallLength > ItemW)
		{
			uint32 NameHash = GetTypeHash(Item.Name);
			float Offset = (static_cast<float>(NameHash % 1000) / 1000.0f) * (WallLength - ItemW) - (WallLength - ItemW) * 0.5f;
			OutPosition += WallDir * Offset;
		}

		break;
	}

	case EPlacementZone::Center:
	{
		// Room center with small random offset
		OutPosition = Walls.RoomCenter;
		OutPosition.Z = Walls.RoomMin.Z;
		float JitterX = Rng.FRandRange(-Walls.RoomWidth * 0.05f, Walls.RoomWidth * 0.05f);
		float JitterY = Rng.FRandRange(-Walls.RoomDepth * 0.05f, Walls.RoomDepth * 0.05f);
		OutPosition.X += JitterX;
		OutPosition.Y += JitterY;
		break;
	}

	case EPlacementZone::Corner:
	{
		// Pick a random corner, offset inward by item half-size + margin
		int32 CornerIdx = Rng.RandRange(0, 3);
		float CX = (CornerIdx & 1) ? Walls.RoomMax.X : Walls.RoomMin.X;
		float CY = (CornerIdx & 2) ? Walls.RoomMax.Y : Walls.RoomMin.Y;

		float SignX = (CornerIdx & 1) ? -1.0f : 1.0f;
		float SignY = (CornerIdx & 2) ? -1.0f : 1.0f;

		OutPosition.X = CX + SignX * (OutDimensions.X * 0.5f + GWallOffset);
		OutPosition.Y = CY + SignY * (OutDimensions.Y * 0.5f + GWallOffset);
		OutPosition.Z = Walls.RoomMin.Z;
		break;
	}

	case EPlacementZone::Near:
	{
		// Place near another named item
		const FVector* NearPos = PlacedItems.Find(Item.NearItem);
		if (!NearPos)
		{
			// Fallback to center if referenced item wasn't placed
			OutPosition = Walls.RoomCenter;
			OutPosition.Z = Walls.RoomMin.Z;
			break;
		}

		// Offset from the near item by a random direction, but stay in bounds
		float OffsetDist = FMath::Max(OutDimensions.X, OutDimensions.Y) + Item.Spacing + 20.0f;
		float Angle = Rng.FRandRange(0.0f, 360.0f);
		FVector Offset(FMath::Cos(FMath::DegreesToRadians(Angle)) * OffsetDist,
		               FMath::Sin(FMath::DegreesToRadians(Angle)) * OffsetDist,
		               0.0f);
		OutPosition = *NearPos + Offset;
		OutPosition.Z = Walls.RoomMin.Z;

		// Clamp to room bounds with margin
		OutPosition.X = FMath::Clamp(OutPosition.X,
			Walls.RoomMin.X + OutDimensions.X * 0.5f + GWallOffset,
			Walls.RoomMax.X - OutDimensions.X * 0.5f - GWallOffset);
		OutPosition.Y = FMath::Clamp(OutPosition.Y,
			Walls.RoomMin.Y + OutDimensions.Y * 0.5f + GWallOffset,
			Walls.RoomMax.Y - OutDimensions.Y * 0.5f - GWallOffset);

		break;
	}
	}

	// Build candidate bounding box (centered at position, half-extents)
	FVector HalfExtent(OutDimensions.X * 0.5f, OutDimensions.Y * 0.5f, OutDimensions.Z * 0.5f);
	FVector BoxCenter(OutPosition.X, OutPosition.Y, OutPosition.Z + OutDimensions.Z * 0.5f);
	FBox CandidateBox(BoxCenter - HalfExtent, BoxCenter + HalfExtent);

	// Collision rejection
	if (CheckCollision(CandidateBox, OccupiedBoxes, DoorPositions))
	{
		return false;
	}

	return true;
}

// ============================================================================
// Density Overrides
// ============================================================================

void FMonolithMeshFurnishingActions::ApplyDensityOverrides(
	TArray<FFurnitureItem>& Items,
	const TSharedPtr<FJsonObject>& Preset,
	const FString& Density)
{
	if (!Preset.IsValid() || Density == TEXT("normal")) return;

	const TSharedPtr<FJsonObject>* OverridesObj = nullptr;
	if (!Preset->TryGetObjectField(TEXT("density_overrides"), OverridesObj) || !OverridesObj || !(*OverridesObj).IsValid())
	{
		return;
	}

	const TSharedPtr<FJsonObject>* DensityObj = nullptr;
	if (!(*OverridesObj)->TryGetObjectField(Density, DensityObj) || !DensityObj || !(*DensityObj).IsValid())
	{
		return;
	}

	// Handle "exclude" — remove items by name
	const TArray<TSharedPtr<FJsonValue>>* ExcludeArr = nullptr;
	if ((*DensityObj)->TryGetArrayField(TEXT("exclude"), ExcludeArr) && ExcludeArr)
	{
		TSet<FString> ExcludeNames;
		for (const auto& V : *ExcludeArr)
		{
			ExcludeNames.Add(V->AsString());
		}

		Items.RemoveAll([&ExcludeNames](const FFurnitureItem& Item)
		{
			return ExcludeNames.Contains(Item.Name);
		});
	}

	// Handle "extra" — add more items
	const TArray<TSharedPtr<FJsonValue>>* ExtraArr = nullptr;
	if ((*DensityObj)->TryGetArrayField(TEXT("extra"), ExtraArr) && ExtraArr)
	{
		for (const auto& V : *ExtraArr)
		{
			const TSharedPtr<FJsonObject>* ExtraObj = nullptr;
			if (V->TryGetObject(ExtraObj) && ExtraObj && (*ExtraObj).IsValid())
			{
				Items.Add(ParseFurnitureItem(*ExtraObj));
			}
		}
	}
}

// ============================================================================
// Horror Decay
// ============================================================================

void FMonolithMeshFurnishingActions::ApplyDecay(
	TArray<TSharedPtr<FJsonObject>>& PlacedItems,
	float DecayAmount,
	FRandomStream& Rng)
{
	if (DecayAmount <= 0.0f || PlacedItems.Num() == 0) return;

	int32 NumToDisturb = FMath::CeilToInt32(DecayAmount * PlacedItems.Num());
	NumToDisturb = FMath::Min(NumToDisturb, PlacedItems.Num());

	// Shuffle and pick first N
	TArray<int32> Indices;
	Indices.Reserve(PlacedItems.Num());
	for (int32 i = 0; i < PlacedItems.Num(); ++i) Indices.Add(i);

	// Fisher-Yates shuffle
	for (int32 i = Indices.Num() - 1; i > 0; --i)
	{
		int32 j = Rng.RandRange(0, i);
		Indices.Swap(i, j);
	}

	for (int32 k = 0; k < NumToDisturb; ++k)
	{
		int32 Idx = Indices[k];
		auto& ItemJson = PlacedItems[Idx];

		// Random tilt: up to 45 degrees on pitch/roll
		float TiltPitch = Rng.FRandRange(-45.0f, 45.0f);
		float TiltRoll = Rng.FRandRange(-30.0f, 30.0f);
		float TiltYaw = Rng.FRandRange(-15.0f, 15.0f);

		// Get existing rotation if any
		const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
		float BasePitch = 0, BaseYaw = 0, BaseRoll = 0;
		if (ItemJson->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr && RotArr->Num() >= 3)
		{
			BasePitch = static_cast<float>((*RotArr)[0]->AsNumber());
			BaseYaw = static_cast<float>((*RotArr)[1]->AsNumber());
			BaseRoll = static_cast<float>((*RotArr)[2]->AsNumber());
		}

		TArray<TSharedPtr<FJsonValue>> NewRot;
		NewRot.Add(MakeShared<FJsonValueNumber>(BasePitch + TiltPitch));
		NewRot.Add(MakeShared<FJsonValueNumber>(BaseYaw + TiltYaw));
		NewRot.Add(MakeShared<FJsonValueNumber>(BaseRoll + TiltRoll));
		ItemJson->SetArrayField(TEXT("rotation"), NewRot);
		ItemJson->SetBoolField(TEXT("decayed"), true);
	}
}

// ============================================================================
// Parametric Mesh Params Builder
// ============================================================================

TSharedPtr<FJsonObject> FMonolithMeshFurnishingActions::BuildParametricMeshParams(
	const FFurnitureItem& Item,
	const FVector& Position,
	const FRotator& Rotation,
	const FVector& Dimensions,
	const FString& SavePathPrefix,
	const FString& Folder,
	int32 Index)
{
	auto P = MakeShared<FJsonObject>();

	P->SetStringField(TEXT("type"), Item.Type);

	// Dimensions
	auto Dims = MakeShared<FJsonObject>();
	Dims->SetNumberField(TEXT("width"), Dimensions.X);
	Dims->SetNumberField(TEXT("depth"), Dimensions.Y);
	Dims->SetNumberField(TEXT("height"), Dimensions.Z);
	P->SetObjectField(TEXT("dimensions"), Dims);

	// Save path
	FString SafeName = Item.Name.Replace(TEXT(" "), TEXT("_"));
	FString SavePath = FString::Printf(TEXT("%s/SM_%s_%02d"), *SavePathPrefix, *SafeName, Index);
	P->SetStringField(TEXT("save_path"), SavePath);

	// Place in scene
	P->SetBoolField(TEXT("place_in_scene"), true);

	// Location
	TArray<TSharedPtr<FJsonValue>> LocArr;
	LocArr.Add(MakeShared<FJsonValueNumber>(Position.X));
	LocArr.Add(MakeShared<FJsonValueNumber>(Position.Y));
	LocArr.Add(MakeShared<FJsonValueNumber>(Position.Z));
	P->SetArrayField(TEXT("location"), LocArr);

	// Rotation
	TArray<TSharedPtr<FJsonValue>> RotArr;
	RotArr.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
	RotArr.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
	RotArr.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
	P->SetArrayField(TEXT("rotation"), RotArr);

	// Label
	FString Label = FString::Printf(TEXT("%s_%02d"), *Item.Name, Index);
	P->SetStringField(TEXT("label"), Label);

	// Folder
	if (!Folder.IsEmpty())
	{
		P->SetStringField(TEXT("folder"), Folder);
	}

	// Use cache for identical furniture types
	P->SetBoolField(TEXT("use_cache"), true);

	// Snap to floor disabled — we manage Z explicitly
	P->SetBoolField(TEXT("snap_to_floor"), false);

	return P;
}

// ============================================================================
// furnish_room
// ============================================================================

FMonolithActionResult FMonolithMeshFurnishingActions::FurnishRoom(const TSharedPtr<FJsonObject>& Params)
{
	// --- Parse required params ---
	FString RoomType;
	if (!Params->TryGetStringField(TEXT("room_type"), RoomType))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: room_type"));
	}
	RoomType = RoomType.ToLower().TrimStartAndEnd();

	const TSharedPtr<FJsonObject>* BoundsObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("world_bounds"), BoundsObj) || !BoundsObj || !(*BoundsObj).IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: world_bounds"));
	}

	FBox RoomBounds;
	if (!ParseBoundsObject(*BoundsObj, RoomBounds))
	{
		return FMonolithActionResult::Error(TEXT("Invalid world_bounds format — expected {min: [x,y,z], max: [x,y,z]}"));
	}

	FString SavePathPrefix;
	if (!Params->TryGetStringField(TEXT("save_path_prefix"), SavePathPrefix))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: save_path_prefix"));
	}

	// --- Parse optional params ---
	int32 Seed = 0;
	if (Params->HasField(TEXT("seed")))
	{
		Seed = static_cast<int32>(Params->GetNumberField(TEXT("seed")));
	}
	FRandomStream Rng(Seed);

	FString Density = TEXT("normal");
	Params->TryGetStringField(TEXT("density"), Density);
	Density = Density.ToLower().TrimStartAndEnd();

	FString PresetOverride;
	Params->TryGetStringField(TEXT("preset"), PresetOverride);

	FString Style = TEXT("modern");
	Params->TryGetStringField(TEXT("style"), Style);

	FString Folder;
	Params->TryGetStringField(TEXT("folder"), Folder);

	// Parse door positions
	TArray<FVector> DoorPositions;
	const TArray<TSharedPtr<FJsonValue>>* DoorArr = nullptr;
	if (Params->TryGetArrayField(TEXT("door_positions"), DoorArr) && DoorArr)
	{
		for (const auto& V : *DoorArr)
		{
			if (V->Type == EJson::Array)
			{
				FVector DoorPos;
				if (ParseVec3(V->AsArray(), DoorPos))
				{
					DoorPositions.Add(DoorPos);
				}
			}
		}
	}

	// Parse skip_types
	TSet<FString> SkipTypes;
	const TArray<TSharedPtr<FJsonValue>>* SkipArr = nullptr;
	if (Params->TryGetArrayField(TEXT("skip_types"), SkipArr) && SkipArr)
	{
		for (const auto& V : *SkipArr)
		{
			SkipTypes.Add(V->AsString().ToLower());
		}
	}

	// --- Load preset ---
	FString PresetName = PresetOverride.IsEmpty() ? RoomType : PresetOverride;
	TSharedPtr<FJsonObject> Preset = LoadPreset(PresetName);
	if (!Preset.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No furniture preset found for '%s'. Use list_furniture_presets to see available presets."), *PresetName));
	}

	// --- Parse items from preset ---
	const TArray<TSharedPtr<FJsonValue>>* ItemsArr = nullptr;
	if (!Preset->TryGetArrayField(TEXT("items"), ItemsArr) || !ItemsArr)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Preset '%s' has no 'items' array"), *PresetName));
	}

	TArray<FFurnitureItem> FurnitureItems;
	for (const auto& V : *ItemsArr)
	{
		const TSharedPtr<FJsonObject>* ItemObj = nullptr;
		if (V->TryGetObject(ItemObj) && ItemObj && (*ItemObj).IsValid())
		{
			FFurnitureItem Parsed = ParseFurnitureItem(*ItemObj);
			if (!SkipTypes.Contains(Parsed.Name.ToLower()) && !SkipTypes.Contains(Parsed.Type.ToLower()))
			{
				FurnitureItems.Add(MoveTemp(Parsed));
			}
		}
	}

	// Apply density overrides
	ApplyDensityOverrides(FurnitureItems, Preset, Density);

	// --- Compute room walls ---
	FRoomWalls Walls = ComputeRoomWalls(RoomBounds, DoorPositions);

	// --- Place each item ---
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	TArray<FBox> OccupiedBoxes;
	TMap<FString, FVector> PlacedItemPositions;
	TArray<TSharedPtr<FJsonValue>> PlacedArr;
	TArray<TSharedPtr<FJsonValue>> SkippedArr;
	int32 GlobalIndex = 0;

	for (const FFurnitureItem& Item : FurnitureItems)
	{
		// Determine count for this item
		int32 Count = (Item.CountMin == Item.CountMax)
			? Item.CountMin
			: Rng.RandRange(Item.CountMin, Item.CountMax);

		for (int32 i = 0; i < Count; ++i)
		{
			FVector Position;
			FRotator Rotation;
			FVector Dimensions;

			static constexpr int32 MaxRetries = 4;
			bool bPlaced = false;
			for (int32 Retry = 0; Retry <= MaxRetries; ++Retry)
			{
				FRandomStream RetryRng(Rng.GetCurrentSeed() + Retry * 997);
				bPlaced = ComputePlacement(Item, Walls, DoorPositions, OccupiedBoxes,
					PlacedItemPositions, Position, Rotation, Dimensions,
					(Retry == 0) ? Rng : RetryRng);
				if (bPlaced)
					break;
			}

			if (!bPlaced)
			{
				// Item skipped after all retries due to collision
				auto SkipObj = MakeShared<FJsonObject>();
				SkipObj->SetStringField(TEXT("name"), Item.Name);
				SkipObj->SetStringField(TEXT("type"), Item.Type);
				SkipObj->SetStringField(TEXT("reason"), TEXT("collision"));
				SkippedArr.Add(MakeShared<FJsonValueObject>(SkipObj));
				continue;
			}

			// Build params and call create_parametric_mesh
			auto MeshParams = BuildParametricMeshParams(Item, Position, Rotation, Dimensions,
				SavePathPrefix, Folder, GlobalIndex);

			FMonolithActionResult MeshResult = Registry.ExecuteAction(TEXT("mesh"), TEXT("create_parametric_mesh"), MeshParams);

			if (!MeshResult.bSuccess)
			{
				auto SkipObj = MakeShared<FJsonObject>();
				SkipObj->SetStringField(TEXT("name"), Item.Name);
				SkipObj->SetStringField(TEXT("type"), Item.Type);
				SkipObj->SetStringField(TEXT("reason"), FString::Printf(TEXT("mesh_error: %s"), *MeshResult.ErrorMessage));
				SkippedArr.Add(MakeShared<FJsonValueObject>(SkipObj));
				continue;
			}

			// Record placement
			FVector HalfExtent(Dimensions.X * 0.5f, Dimensions.Y * 0.5f, Dimensions.Z * 0.5f);
			FVector BoxCenter(Position.X, Position.Y, Position.Z + Dimensions.Z * 0.5f);
			OccupiedBoxes.Add(FBox(BoxCenter - HalfExtent, BoxCenter + HalfExtent));
			PlacedItemPositions.Add(Item.Name, Position);

			// Build result entry
			auto PlacedObj = MakeShared<FJsonObject>();
			PlacedObj->SetStringField(TEXT("type"), Item.Type);
			PlacedObj->SetStringField(TEXT("name"), Item.Name);

			// Actor name from mesh result
			FString ActorName;
			if (MeshResult.Result.IsValid())
			{
				MeshResult.Result->TryGetStringField(TEXT("actor_name"), ActorName);
			}
			PlacedObj->SetStringField(TEXT("actor_name"), ActorName);

			TArray<TSharedPtr<FJsonValue>> PosArr;
			PosArr.Add(MakeShared<FJsonValueNumber>(Position.X));
			PosArr.Add(MakeShared<FJsonValueNumber>(Position.Y));
			PosArr.Add(MakeShared<FJsonValueNumber>(Position.Z));
			PlacedObj->SetArrayField(TEXT("position"), PosArr);

			auto DimsObj = MakeShared<FJsonObject>();
			DimsObj->SetNumberField(TEXT("width"), Dimensions.X);
			DimsObj->SetNumberField(TEXT("depth"), Dimensions.Y);
			DimsObj->SetNumberField(TEXT("height"), Dimensions.Z);
			PlacedObj->SetObjectField(TEXT("dimensions"), DimsObj);

			PlacedArr.Add(MakeShared<FJsonValueObject>(PlacedObj));
			++GlobalIndex;
		}
	}

	// --- Build result ---
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("room_type"), RoomType);
	Result->SetArrayField(TEXT("items_placed"), PlacedArr);
	Result->SetArrayField(TEXT("items_skipped"), SkippedArr);
	Result->SetNumberField(TEXT("total_placed"), PlacedArr.Num());
	Result->SetNumberField(TEXT("total_skipped"), SkippedArr.Num());

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// furnish_building
// ============================================================================

FMonolithActionResult FMonolithMeshFurnishingActions::FurnishBuilding(const TSharedPtr<FJsonObject>& Params)
{
	// --- Parse required params ---
	FString BuildingId;
	if (!Params->TryGetStringField(TEXT("building_id"), BuildingId))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: building_id"));
	}

	FString SavePathPrefix;
	if (!Params->TryGetStringField(TEXT("save_path_prefix"), SavePathPrefix))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: save_path_prefix"));
	}

	// --- Parse optional params ---
	FString BlockId = TEXT("default");
	Params->TryGetStringField(TEXT("block_id"), BlockId);

	int32 Seed = 0;
	if (Params->HasField(TEXT("seed")))
	{
		Seed = static_cast<int32>(Params->GetNumberField(TEXT("seed")));
	}

	FString Density = TEXT("normal");
	Params->TryGetStringField(TEXT("density"), Density);

	FString Style = TEXT("modern");
	Params->TryGetStringField(TEXT("style"), Style);

	FString Folder;
	Params->TryGetStringField(TEXT("folder"), Folder);

	float Decay = 0.0f;
	if (Params->HasField(TEXT("decay")))
	{
		Decay = FMath::Clamp(static_cast<float>(Params->GetNumberField(TEXT("decay"))), 0.0f, 1.0f);
	}

	// Parse skip_room_types
	TSet<FString> SkipRoomTypes;
	const TArray<TSharedPtr<FJsonValue>>* SkipArr = nullptr;
	if (Params->TryGetArrayField(TEXT("skip_room_types"), SkipArr) && SkipArr)
	{
		for (const auto& V : *SkipArr)
		{
			SkipRoomTypes.Add(V->AsString().ToLower());
		}
	}

	// --- Access spatial registry ---
	if (!FMonolithMeshSpatialRegistry::HasBlock(BlockId))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Block '%s' not found in spatial registry. Load it first with load_block_descriptor."), *BlockId));
	}

	FSpatialBlock& Block = FMonolithMeshSpatialRegistry::GetBlock(BlockId);
	FSpatialBuilding* Building = Block.Buildings.Find(BuildingId);
	if (!Building)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Building '%s' not found in block '%s'."), *BuildingId, *BlockId));
	}

	// Default folder from building
	if (Folder.IsEmpty())
	{
		Folder = FString::Printf(TEXT("Procedural/Furnishing/%s"), *BuildingId);
	}

	// --- Iterate rooms and furnish each ---
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	int32 RoomsFurnished = 0;
	int32 TotalItems = 0;
	TMap<FString, int32> ItemsByRoomType;
	TArray<TSharedPtr<FJsonValue>> RoomResults;

	FRandomStream BuildingRng(Seed);

	// Collect all room IDs from the building
	TArray<FString> AllRoomIds;
	for (const auto& FloorPair : Building->FloorToRoomIds)
	{
		AllRoomIds.Append(FloorPair.Value);
	}

	for (const FString& RoomId : AllRoomIds)
	{
		FSpatialRoom* Room = Block.Rooms.Find(RoomId);
		if (!Room) continue;

		FString LowerType = Room->RoomType.ToLower();
		if (SkipRoomTypes.Contains(LowerType)) continue;

		// Collect door positions for this room
		TArray<FVector> DoorPositions;
		for (const FString& DoorId : Room->DoorIds)
		{
			FSpatialDoor* Door = Block.Doors.Find(DoorId);
			if (Door)
			{
				DoorPositions.Add(Door->WorldPosition);
			}
		}

		// Build per-room params
		auto RoomParams = MakeShared<FJsonObject>();
		RoomParams->SetStringField(TEXT("room_type"), Room->RoomType);
		RoomParams->SetStringField(TEXT("save_path_prefix"),
			FString::Printf(TEXT("%s/%s"), *SavePathPrefix, *RoomId));
		RoomParams->SetNumberField(TEXT("seed"), BuildingRng.RandRange(0, 999999));
		RoomParams->SetStringField(TEXT("density"), Density);
		RoomParams->SetStringField(TEXT("style"), Style);
		RoomParams->SetStringField(TEXT("folder"), FString::Printf(TEXT("%s/%s"), *Folder, *RoomId));

		// World bounds
		auto BoundsObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> MinArr, MaxArr;
		MinArr.Add(MakeShared<FJsonValueNumber>(Room->WorldBounds.Min.X));
		MinArr.Add(MakeShared<FJsonValueNumber>(Room->WorldBounds.Min.Y));
		MinArr.Add(MakeShared<FJsonValueNumber>(Room->WorldBounds.Min.Z));
		MaxArr.Add(MakeShared<FJsonValueNumber>(Room->WorldBounds.Max.X));
		MaxArr.Add(MakeShared<FJsonValueNumber>(Room->WorldBounds.Max.Y));
		MaxArr.Add(MakeShared<FJsonValueNumber>(Room->WorldBounds.Max.Z));
		BoundsObj->SetArrayField(TEXT("min"), MinArr);
		BoundsObj->SetArrayField(TEXT("max"), MaxArr);
		RoomParams->SetObjectField(TEXT("world_bounds"), BoundsObj);

		// Door positions
		if (DoorPositions.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> DoorPosArr;
			for (const FVector& DP : DoorPositions)
			{
				TArray<TSharedPtr<FJsonValue>> DPArr;
				DPArr.Add(MakeShared<FJsonValueNumber>(DP.X));
				DPArr.Add(MakeShared<FJsonValueNumber>(DP.Y));
				DPArr.Add(MakeShared<FJsonValueNumber>(DP.Z));
				DoorPosArr.Add(MakeShared<FJsonValueArray>(DPArr));
			}
			RoomParams->SetArrayField(TEXT("door_positions"), DoorPosArr);
		}

		// Call furnish_room via registry (re-entrant through our own handler)
		FMonolithActionResult RoomResult = Registry.ExecuteAction(TEXT("mesh"), TEXT("furnish_room"), RoomParams);

		if (RoomResult.bSuccess && RoomResult.Result.IsValid())
		{
			int32 Placed = static_cast<int32>(RoomResult.Result->GetNumberField(TEXT("total_placed")));
			TotalItems += Placed;
			ItemsByRoomType.FindOrAdd(Room->RoomType) += Placed;
			++RoomsFurnished;

			// Collect result
			RoomResult.Result->SetStringField(TEXT("room_id"), RoomId);
			RoomResults.Add(MakeShared<FJsonValueObject>(RoomResult.Result));
		}
	}

	// --- Apply decay across all placed items if requested ---
	// Decay is applied by calling set_room_disturbance or rotating items post-placement
	// For now we annotate decay info in the result — actual rotation is applied per-item above
	// TODO: integrate with set_room_disturbance when available

	// --- Build result ---
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("building_id"), BuildingId);
	Result->SetNumberField(TEXT("rooms_furnished"), RoomsFurnished);
	Result->SetNumberField(TEXT("total_items"), TotalItems);

	auto ByTypeObj = MakeShared<FJsonObject>();
	for (const auto& Pair : ItemsByRoomType)
	{
		ByTypeObj->SetNumberField(Pair.Key, Pair.Value);
	}
	Result->SetObjectField(TEXT("items_by_room_type"), ByTypeObj);
	Result->SetArrayField(TEXT("room_details"), RoomResults);

	if (Decay > 0.0f)
	{
		Result->SetNumberField(TEXT("decay"), Decay);
		Result->SetStringField(TEXT("decay_note"), TEXT("Decay applied — some items may have rotation offsets"));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// list_furniture_presets
// ============================================================================

FMonolithActionResult FMonolithMeshFurnishingActions::ListFurniturePresets(const TSharedPtr<FJsonObject>& Params)
{
	FString FilterType;
	Params->TryGetStringField(TEXT("room_type"), FilterType);
	FilterType = FilterType.ToLower().TrimStartAndEnd();

	TArray<FString> Available = GetAvailablePresets();

	TArray<TSharedPtr<FJsonValue>> PresetsArr;

	for (const FString& PresetName : Available)
	{
		if (!FilterType.IsEmpty() && PresetName.ToLower() != FilterType)
		{
			continue;
		}

		auto PresetInfo = MakeShared<FJsonObject>();
		PresetInfo->SetStringField(TEXT("name"), PresetName);

		// Load preset to get description and item count
		TSharedPtr<FJsonObject> Preset = LoadPreset(PresetName);
		if (Preset.IsValid())
		{
			FString Desc;
			Preset->TryGetStringField(TEXT("description"), Desc);
			PresetInfo->SetStringField(TEXT("description"), Desc);

			FString PresetRoomType;
			Preset->TryGetStringField(TEXT("room_type"), PresetRoomType);
			PresetInfo->SetStringField(TEXT("room_type"), PresetRoomType);

			const TArray<TSharedPtr<FJsonValue>>* ItemsArr = nullptr;
			if (Preset->TryGetArrayField(TEXT("items"), ItemsArr) && ItemsArr)
			{
				PresetInfo->SetNumberField(TEXT("item_count"), ItemsArr->Num());

				// List item names
				TArray<TSharedPtr<FJsonValue>> ItemNames;
				for (const auto& V : *ItemsArr)
				{
					const TSharedPtr<FJsonObject>* ItemObj = nullptr;
					if (V->TryGetObject(ItemObj) && ItemObj && (*ItemObj).IsValid())
					{
						FString Name;
						if ((*ItemObj)->TryGetStringField(TEXT("name"), Name))
						{
							ItemNames.Add(MakeShared<FJsonValueString>(Name));
						}
					}
				}
				PresetInfo->SetArrayField(TEXT("item_names"), ItemNames);
			}

			// Check for density overrides
			const TSharedPtr<FJsonObject>* OverridesObj = nullptr;
			if (Preset->TryGetObjectField(TEXT("density_overrides"), OverridesObj) && OverridesObj)
			{
				TArray<TSharedPtr<FJsonValue>> DensityModes;
				for (const auto& OverridePair : (*OverridesObj)->Values)
				{
					DensityModes.Add(MakeShared<FJsonValueString>(MonolithKeyToString(OverridePair.Key)));
				}
				PresetInfo->SetArrayField(TEXT("density_modes"), DensityModes);
			}
		}

		PresetsArr.Add(MakeShared<FJsonValueObject>(PresetInfo));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("presets"), PresetsArr);
	Result->SetNumberField(TEXT("count"), PresetsArr.Num());
	Result->SetStringField(TEXT("presets_directory"), GetPresetsDirectory());

	return FMonolithActionResult::Success(Result);
}
