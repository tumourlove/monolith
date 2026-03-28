#include "MonolithMeshFloorPlanGenerator.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMonolithFloorPlan, Log, All);
DEFINE_LOG_CATEGORY(LogMonolithFloorPlan);

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshFloorPlanGenerator::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("generate_floor_plan"),
		TEXT("Generate a complete floor plan from a building archetype. Returns grid, rooms, and doors "
			"in the exact format consumed by create_building_from_grid. "
			"Algorithm: archetype loading -> room resolution -> squarified treemap layout -> "
			"adjacency validation -> privacy gradient -> corridor insertion -> door placement -> "
			"horror post-processing -> Space Syntax scoring. "
			"WP-2: Supports adjacency_matrix (MUST/MUST_NOT), privacy gradient, wet wall clustering, "
			"and circulation patterns (double_loaded, hub_spoke, racetrack, enfilade). "
			"WP-6: Horror subversion (door locking, dead-end control, loop breaking, wrong-room injection), "
			"Space Syntax metrics (integration, connectivity, depth), tension curve metadata per room, "
			"and hospice safety caps."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshFloorPlanGenerator::GenerateFloorPlan),
		FParamSchemaBuilder()
			.Required(TEXT("archetype"), TEXT("string"), TEXT("Archetype name (e.g. 'residential_house') or full path to archetype JSON file"))
			.Required(TEXT("footprint_width"), TEXT("number"), TEXT("Building footprint width in cm (e.g. 800 = 8m)"))
			.Required(TEXT("footprint_height"), TEXT("number"), TEXT("Building footprint height/depth in cm (e.g. 600 = 6m)"))
			.Optional(TEXT("cell_size"), TEXT("number"), TEXT("Grid cell size in cm. Smaller = finer resolution, larger grids"), TEXT("50"))
			.Optional(TEXT("seed"), TEXT("number"), TEXT("Random seed for deterministic generation. -1 = random"), TEXT("-1"))
			.Optional(TEXT("hospice_mode"), TEXT("boolean"), TEXT("Enforce wheelchair accessibility: min 100cm doors, 180cm corridors, rest alcoves"), TEXT("false"))
			.Optional(TEXT("min_room_aspect"), TEXT("number"), TEXT("Minimum acceptable room aspect ratio (width/height). Rooms worse than this get rebalanced"), TEXT("3.0"))
			.Optional(TEXT("floor_index"), TEXT("number"), TEXT("Floor index for per-floor room filtering: 0=ground, 1+=upper, -1=all floors (default)"), TEXT("-1"))
			.Optional(TEXT("horror_level"), TEXT("number"), TEXT("Horror intensity 0.0-1.0. Controls door locking, dead-end ratio, loop breaking, wrong-room injection. 0=normal, 1=maximum horror. Hospice mode caps at 0.3."), TEXT("0.0"))
			.Optional(TEXT("template"), TEXT("string"), TEXT("Specific template name to load (e.g. 'small_ranch_01'). Must exist in the template_category directory."))
			.Optional(TEXT("template_category"), TEXT("string"), TEXT("Template category to select from (e.g. 'residential', 'commercial', 'horror'). A matching template is chosen randomly."))
			.Optional(TEXT("use_templates"), TEXT("boolean"), TEXT("Enable template-based floor plans. When false, always uses algorithmic treemap. Default true."), TEXT("true"))
			.Optional(TEXT("genre"), TEXT("string"), TEXT("Game genre hint. When 'horror', overrides template_category to 'horror'."))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("list_building_archetypes"),
		TEXT("List all available building archetype JSON files in the archetypes directory."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshFloorPlanGenerator::ListBuildingArchetypes),
		FParamSchemaBuilder()
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("get_building_archetype"),
		TEXT("Return the full JSON definition of a specific building archetype."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshFloorPlanGenerator::GetBuildingArchetype),
		FParamSchemaBuilder()
			.Required(TEXT("archetype"), TEXT("string"), TEXT("Archetype name (e.g. 'residential_house') without .json extension"))
			.Build());
}

// ============================================================================
// WP-2: Adjacency Matrix
// ============================================================================

FMonolithMeshFloorPlanGenerator::EAdjacencyRelation
FMonolithMeshFloorPlanGenerator::FAdjacencyMatrix::GetRule(const FString& TypeA, const FString& TypeB) const
{
	// Check A->B
	if (const auto* Inner = Rules.Find(TypeA))
	{
		if (const auto* Rule = Inner->Find(TypeB))
		{
			return *Rule;
		}
	}
	// Check B->A (symmetric)
	if (const auto* Inner = Rules.Find(TypeB))
	{
		if (const auto* Rule = Inner->Find(TypeA))
		{
			return *Rule;
		}
	}
	return EAdjacencyRelation::MAY;
}

bool FMonolithMeshFloorPlanGenerator::FAdjacencyMatrix::ViolatesMustNot(const FString& TypeA, const FString& TypeB) const
{
	return GetRule(TypeA, TypeB) == EAdjacencyRelation::MUST_NOT;
}

// ============================================================================
// WP-2: Privacy Zone Mapping
// ============================================================================

FMonolithMeshFloorPlanGenerator::EPrivacyZone
FMonolithMeshFloorPlanGenerator::GetPrivacyZone(const FString& RoomType)
{
	// PUBLIC
	static const TSet<FString> PublicTypes = {
		TEXT("entry"), TEXT("entryway"), TEXT("foyer"), TEXT("lobby"),
		TEXT("living_room"), TEXT("reception"), TEXT("vestibule"),
		TEXT("waiting_room"), TEXT("waiting_area"), TEXT("nave"),
		TEXT("front_desk")
	};

	// SEMI_PUBLIC
	static const TSet<FString> SemiPublicTypes = {
		TEXT("dining_room"), TEXT("family_room"), TEXT("dining_area"),
		TEXT("bar"), TEXT("fellowship_hall"), TEXT("cafeteria"),
		TEXT("gymnasium"), TEXT("library"), TEXT("classroom"),
		TEXT("open_office"), TEXT("bullpen"), TEXT("mailbox_area")
	};

	// SEMI_PRIVATE
	static const TSet<FString> SemiPrivateTypes = {
		TEXT("hallway"), TEXT("corridor"), TEXT("kitchen"),
		TEXT("break_room"), TEXT("nurse_station"), TEXT("copy_room"),
		TEXT("teacher_lounge"), TEXT("admin_office"), TEXT("nurse_office"),
		TEXT("conference_room"), TEXT("stairwell"), TEXT("elevator")
	};

	// PRIVATE
	static const TSet<FString> PrivateTypes = {
		TEXT("bedroom"), TEXT("bathroom"), TEXT("office"),
		TEXT("master_bedroom"), TEXT("interrogation"), TEXT("exam_room"),
		TEXT("doctor_office"), TEXT("private_office"), TEXT("chief_office"),
		TEXT("detective_office"), TEXT("observation"), TEXT("half_bath"),
		TEXT("bathroom_ada"), TEXT("restroom_male"), TEXT("restroom_female"),
		TEXT("restroom_block"), TEXT("holding_cell"), TEXT("sacristy"),
		TEXT("altar")
	};

	// SERVICE
	static const TSet<FString> ServiceTypes = {
		TEXT("laundry"), TEXT("laundry_room"), TEXT("utility"),
		TEXT("utility_closet"), TEXT("storage"), TEXT("storage_cage"),
		TEXT("closet"), TEXT("garage"), TEXT("janitor_closet"),
		TEXT("server_room"), TEXT("mechanical"), TEXT("armory"),
		TEXT("evidence_room"), TEXT("locker_room"), TEXT("booking"),
		TEXT("dispatch"), TEXT("security_desk"), TEXT("supply_room"),
		TEXT("mail_room"), TEXT("loading_dock"), TEXT("warehouse_floor"),
		TEXT("rest_alcove")
	};

	if (PublicTypes.Contains(RoomType)) return EPrivacyZone::PUBLIC;
	if (SemiPublicTypes.Contains(RoomType)) return EPrivacyZone::SEMI_PUBLIC;
	if (SemiPrivateTypes.Contains(RoomType)) return EPrivacyZone::SEMI_PRIVATE;
	if (PrivateTypes.Contains(RoomType)) return EPrivacyZone::PRIVATE;
	if (ServiceTypes.Contains(RoomType)) return EPrivacyZone::SERVICE;

	// Default: SEMI_PRIVATE for unknown types
	return EPrivacyZone::SEMI_PRIVATE;
}

// ============================================================================
// WP-2: Wet Wall Clustering
// ============================================================================

bool FMonolithMeshFloorPlanGenerator::IsWetRoom(const FString& RoomType)
{
	static const TSet<FString> WetTypes = {
		TEXT("bathroom"), TEXT("half_bath"), TEXT("bathroom_ada"),
		TEXT("restroom_male"), TEXT("restroom_female"), TEXT("restroom_block"),
		TEXT("kitchen"), TEXT("laundry"), TEXT("laundry_room")
	};
	return WetTypes.Contains(RoomType);
}

TSet<FIntPoint> FMonolithMeshFloorPlanGenerator::BuildPlumbingChaseSet(
	const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	const TArray<FRoomDef>& Rooms)
{
	TSet<FIntPoint> PlumbingCells;
	static const FIntPoint Dirs[] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

	for (int32 i = 0; i < Rooms.Num(); ++i)
	{
		if (!IsWetRoom(Rooms[i].RoomType))
			continue;

		// Mark all cells of wet rooms and their adjacent cells as plumbing chase
		for (const FIntPoint& Cell : Rooms[i].GridCells)
		{
			PlumbingCells.Add(Cell);
			for (const FIntPoint& D : Dirs)
			{
				FIntPoint N = Cell + D;
				if (N.X >= 0 && N.Y >= 0 && N.X < GridW && N.Y < GridH)
				{
					PlumbingCells.Add(N);
				}
			}
		}
	}

	return PlumbingCells;
}

// ============================================================================
// WP-2: Adjacency Validation & Fix
// ============================================================================

TArray<TPair<int32, int32>> FMonolithMeshFloorPlanGenerator::FindMustNotViolations(
	const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	const TArray<FRoomDef>& Rooms, const FAdjacencyMatrix& Matrix)
{
	TArray<TPair<int32, int32>> Violations;
	TSet<uint64> Checked;

	auto PairKey = [](int32 A, int32 B) -> uint64 {
		int32 Lo = FMath::Min(A, B);
		int32 Hi = FMath::Max(A, B);
		return (static_cast<uint64>(Lo) << 32) | static_cast<uint64>(Hi);
	};

	static const FIntPoint Dirs[] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

	for (int32 Y = 0; Y < GridH; ++Y)
	{
		for (int32 X = 0; X < GridW; ++X)
		{
			int32 IdxA = Grid[Y][X];
			if (IdxA < 0 || IdxA >= Rooms.Num()) continue;

			for (const FIntPoint& D : Dirs)
			{
				int32 NX = X + D.X, NY = Y + D.Y;
				if (NX < 0 || NY < 0 || NX >= GridW || NY >= GridH) continue;

				int32 IdxB = Grid[NY][NX];
				if (IdxB < 0 || IdxB >= Rooms.Num() || IdxB == IdxA) continue;

				uint64 Key = PairKey(IdxA, IdxB);
				if (Checked.Contains(Key)) continue;
				Checked.Add(Key);

				if (Matrix.ViolatesMustNot(Rooms[IdxA].RoomType, Rooms[IdxB].RoomType))
				{
					Violations.Add(TPair<int32, int32>(IdxA, IdxB));
				}
			}
		}
	}

	return Violations;
}

TArray<TPair<int32, int32>> FMonolithMeshFloorPlanGenerator::FindUnmetMustRequirements(
	const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	const TArray<FRoomDef>& Rooms, const FAdjacencyMatrix& Matrix)
{
	TArray<TPair<int32, int32>> Unmet;

	// Build adjacency set from grid
	TSet<uint64> AdjacentPairs;
	auto PairKey = [](int32 A, int32 B) -> uint64 {
		int32 Lo = FMath::Min(A, B);
		int32 Hi = FMath::Max(A, B);
		return (static_cast<uint64>(Lo) << 32) | static_cast<uint64>(Hi);
	};

	static const FIntPoint Dirs[] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
	for (int32 Y = 0; Y < GridH; ++Y)
	{
		for (int32 X = 0; X < GridW; ++X)
		{
			int32 IdxA = Grid[Y][X];
			if (IdxA < 0) continue;
			for (const FIntPoint& D : Dirs)
			{
				int32 NX = X + D.X, NY = Y + D.Y;
				if (NX < 0 || NY < 0 || NX >= GridW || NY >= GridH) continue;
				int32 IdxB = Grid[NY][NX];
				if (IdxB >= 0 && IdxB != IdxA)
					AdjacentPairs.Add(PairKey(IdxA, IdxB));
			}
		}
	}

	// Check all MUST rules in the matrix
	for (const auto& OuterPair : Matrix.Rules)
	{
		const FString& TypeA = OuterPair.Key;
		for (const auto& InnerPair : OuterPair.Value)
		{
			if (InnerPair.Value != EAdjacencyRelation::MUST) continue;

			const FString& TypeB = InnerPair.Key;

			// Find all room instances of TypeA and TypeB
			TArray<int32> IndicesA, IndicesB;
			for (int32 i = 0; i < Rooms.Num(); ++i)
			{
				if (Rooms[i].RoomType == TypeA) IndicesA.Add(i);
				if (Rooms[i].RoomType == TypeB) IndicesB.Add(i);
			}

			// Each instance of TypeA must be adjacent to at least one instance of TypeB
			for (int32 IA : IndicesA)
			{
				bool bFound = false;
				for (int32 IB : IndicesB)
				{
					if (AdjacentPairs.Contains(PairKey(IA, IB)))
					{
						bFound = true;
						break;
					}
				}
				if (!bFound && IndicesB.Num() > 0)
				{
					Unmet.Add(TPair<int32, int32>(IA, IndicesB[0]));
				}
			}
		}
	}

	return Unmet;
}

bool FMonolithMeshFloorPlanGenerator::TrySwapRooms(
	TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	TArray<FRoomDef>& Rooms, int32 RoomIdxA, int32 RoomIdxB)
{
	if (RoomIdxA < 0 || RoomIdxB < 0 || RoomIdxA >= Rooms.Num() || RoomIdxB >= Rooms.Num())
		return false;

	if (RoomIdxA == RoomIdxB)
		return false;

	// Don't swap corridor rooms
	if (Rooms[RoomIdxA].RoomType == TEXT("corridor") || Rooms[RoomIdxB].RoomType == TEXT("corridor"))
		return false;

	// Swap grid cell assignments
	for (const FIntPoint& Cell : Rooms[RoomIdxA].GridCells)
	{
		if (Cell.Y >= 0 && Cell.Y < GridH && Cell.X >= 0 && Cell.X < GridW)
			Grid[Cell.Y][Cell.X] = RoomIdxB;
	}
	for (const FIntPoint& Cell : Rooms[RoomIdxB].GridCells)
	{
		if (Cell.Y >= 0 && Cell.Y < GridH && Cell.X >= 0 && Cell.X < GridW)
			Grid[Cell.Y][Cell.X] = RoomIdxA;
	}

	// Swap grid cell lists
	TArray<FIntPoint> TempCells = MoveTemp(Rooms[RoomIdxA].GridCells);
	Rooms[RoomIdxA].GridCells = MoveTemp(Rooms[RoomIdxB].GridCells);
	Rooms[RoomIdxB].GridCells = MoveTemp(TempCells);

	return true;
}

int32 FMonolithMeshFloorPlanGenerator::ValidateAndFixAdjacency(
	TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	TArray<FRoomDef>& Rooms, const FAdjacencyMatrix& Matrix, int32 MaxRetries)
{
	for (int32 Retry = 0; Retry < MaxRetries; ++Retry)
	{
		TArray<TPair<int32, int32>> Violations = FindMustNotViolations(Grid, GridW, GridH, Rooms, Matrix);

		if (Violations.Num() == 0)
		{
			UE_LOG(LogMonolithFloorPlan, Log, TEXT("Adjacency validation passed (retry %d): 0 MUST_NOT violations"), Retry);
			return 0;
		}

		UE_LOG(LogMonolithFloorPlan, Log, TEXT("Adjacency validation retry %d: %d MUST_NOT violation(s), attempting swaps"),
			Retry, Violations.Num());

		bool bFixedAny = false;

		for (const auto& Violation : Violations)
		{
			int32 RoomA = Violation.Key;
			int32 RoomB = Violation.Value;

			// Try to swap RoomA with any non-violating neighbor of RoomB
			bool bSwapped = false;
			for (int32 Candidate = 0; Candidate < Rooms.Num(); ++Candidate)
			{
				if (Candidate == RoomA || Candidate == RoomB) continue;
				if (Rooms[Candidate].RoomType == TEXT("corridor")) continue;
				if (Rooms[Candidate].RoomType == TEXT("rest_alcove")) continue;

				// Would swapping RoomA <-> Candidate fix this violation without creating a new one?
				// Check: if Candidate ends up next to RoomB, does that violate?
				if (Matrix.ViolatesMustNot(Rooms[Candidate].RoomType, Rooms[RoomB].RoomType))
					continue;

				// Check: if RoomA ends up in Candidate's position, does it violate with Candidate's old neighbors?
				// Simplified: just try the swap and validate
				TrySwapRooms(Grid, GridW, GridH, Rooms, RoomA, Candidate);

				// Check if we didn't make things worse
				TArray<TPair<int32, int32>> NewViolations = FindMustNotViolations(Grid, GridW, GridH, Rooms, Matrix);
				if (NewViolations.Num() < Violations.Num())
				{
					UE_LOG(LogMonolithFloorPlan, Log, TEXT("  Swapped '%s' <-> '%s' to fix violation (violations: %d -> %d)"),
						*Rooms[RoomA].RoomId, *Rooms[Candidate].RoomId, Violations.Num(), NewViolations.Num());
					bSwapped = true;
					bFixedAny = true;
					break;
				}
				else
				{
					// Undo swap
					TrySwapRooms(Grid, GridW, GridH, Rooms, RoomA, Candidate);
				}
			}

			if (!bSwapped)
			{
				// Also try swapping RoomB with someone
				for (int32 Candidate = 0; Candidate < Rooms.Num(); ++Candidate)
				{
					if (Candidate == RoomA || Candidate == RoomB) continue;
					if (Rooms[Candidate].RoomType == TEXT("corridor")) continue;
					if (Rooms[Candidate].RoomType == TEXT("rest_alcove")) continue;

					if (Matrix.ViolatesMustNot(Rooms[Candidate].RoomType, Rooms[RoomA].RoomType))
						continue;

					TrySwapRooms(Grid, GridW, GridH, Rooms, RoomB, Candidate);

					TArray<TPair<int32, int32>> NewViolations = FindMustNotViolations(Grid, GridW, GridH, Rooms, Matrix);
					if (NewViolations.Num() < Violations.Num())
					{
						UE_LOG(LogMonolithFloorPlan, Log, TEXT("  Swapped '%s' <-> '%s' to fix violation (violations: %d -> %d)"),
							*Rooms[RoomB].RoomId, *Rooms[Candidate].RoomId, Violations.Num(), NewViolations.Num());
						bFixedAny = true;
						break;
					}
					else
					{
						TrySwapRooms(Grid, GridW, GridH, Rooms, RoomB, Candidate);
					}
				}
			}
		}

		if (!bFixedAny)
		{
			// No progress -- stop retrying
			TArray<TPair<int32, int32>> Remaining = FindMustNotViolations(Grid, GridW, GridH, Rooms, Matrix);
			for (const auto& V : Remaining)
			{
				UE_LOG(LogMonolithFloorPlan, Warning, TEXT("Unresolved MUST_NOT violation: '%s' (%s) adjacent to '%s' (%s)"),
					*Rooms[V.Key].RoomId, *Rooms[V.Key].RoomType, *Rooms[V.Value].RoomId, *Rooms[V.Value].RoomType);
			}
			return Remaining.Num();
		}
	}

	TArray<TPair<int32, int32>> Final = FindMustNotViolations(Grid, GridW, GridH, Rooms, Matrix);
	return Final.Num();
}

// ============================================================================
// WP-2: Privacy Gradient
// ============================================================================

TMap<int32, int32> FMonolithMeshFloorPlanGenerator::ComputeGraphDepthFromEntry(
	const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	const TArray<FRoomDef>& Rooms)
{
	TMap<int32, int32> Depth;

	// Find entry room (first room with PUBLIC privacy zone on the perimeter)
	int32 EntryIdx = -1;

	// Priority 1: explicitly named entryway/foyer/lobby/vestibule
	static const TArray<FString> EntryTypes = {
		TEXT("entryway"), TEXT("foyer"), TEXT("lobby"), TEXT("vestibule")
	};

	for (int32 i = 0; i < Rooms.Num(); ++i)
	{
		if (EntryTypes.Contains(Rooms[i].RoomType))
		{
			EntryIdx = i;
			break;
		}
	}

	// Priority 2: any PUBLIC room on the perimeter
	if (EntryIdx < 0)
	{
		for (int32 i = 0; i < Rooms.Num(); ++i)
		{
			if (GetPrivacyZone(Rooms[i].RoomType) == EPrivacyZone::PUBLIC)
			{
				for (const FIntPoint& Cell : Rooms[i].GridCells)
				{
					if (Cell.X == 0 || Cell.X == GridW - 1 || Cell.Y == 0 || Cell.Y == GridH - 1)
					{
						EntryIdx = i;
						break;
					}
				}
				if (EntryIdx >= 0) break;
			}
		}
	}

	// Priority 3: first room
	if (EntryIdx < 0 && Rooms.Num() > 0)
		EntryIdx = 0;

	if (EntryIdx < 0) return Depth;

	// Build room adjacency graph from grid
	TMap<int32, TSet<int32>> AdjGraph;
	static const FIntPoint Dirs[] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

	for (int32 Y = 0; Y < GridH; ++Y)
	{
		for (int32 X = 0; X < GridW; ++X)
		{
			int32 IdxA = Grid[Y][X];
			if (IdxA < 0) continue;
			for (const FIntPoint& D : Dirs)
			{
				int32 NX = X + D.X, NY = Y + D.Y;
				if (NX < 0 || NY < 0 || NX >= GridW || NY >= GridH) continue;
				int32 IdxB = Grid[NY][NX];
				if (IdxB >= 0 && IdxB != IdxA)
				{
					AdjGraph.FindOrAdd(IdxA).Add(IdxB);
					AdjGraph.FindOrAdd(IdxB).Add(IdxA);
				}
			}
		}
	}

	// BFS from entry
	TQueue<int32> Queue;
	Queue.Enqueue(EntryIdx);
	Depth.Add(EntryIdx, 0);

	while (!Queue.IsEmpty())
	{
		int32 Current;
		Queue.Dequeue(Current);
		int32 CurrentDepth = Depth[Current];

		if (const TSet<int32>* Neighbors = AdjGraph.Find(Current))
		{
			for (int32 Neighbor : *Neighbors)
			{
				if (!Depth.Contains(Neighbor))
				{
					Depth.Add(Neighbor, CurrentDepth + 1);
					Queue.Enqueue(Neighbor);
				}
			}
		}
	}

	return Depth;
}

int32 FMonolithMeshFloorPlanGenerator::ValidatePrivacyGradient(
	const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	const TArray<FRoomDef>& Rooms, TArray<FString>& OutViolations)
{
	TMap<int32, int32> Depths = ComputeGraphDepthFromEntry(Grid, GridW, GridH, Rooms);
	int32 ViolationCount = 0;

	for (int32 i = 0; i < Rooms.Num(); ++i)
	{
		const int32* DepthPtr = Depths.Find(i);
		if (!DepthPtr) continue;

		int32 RoomDepth = *DepthPtr;
		EPrivacyZone Zone = GetPrivacyZone(Rooms[i].RoomType);

		// PRIVATE rooms must be >= 2 steps from entry
		if (Zone == EPrivacyZone::PRIVATE && RoomDepth < 2)
		{
			FString Msg = FString::Printf(TEXT("Privacy violation: '%s' (%s, PRIVATE) is only %d step(s) from entry (need >= 2)"),
				*Rooms[i].RoomId, *Rooms[i].RoomType, RoomDepth);
			OutViolations.Add(Msg);
			UE_LOG(LogMonolithFloorPlan, Warning, TEXT("%s"), *Msg);
			++ViolationCount;
		}

		// Rooms 1 step from entry should be PUBLIC or SEMI_PUBLIC (warning only)
		if (RoomDepth == 1 && Zone != EPrivacyZone::PUBLIC && Zone != EPrivacyZone::SEMI_PUBLIC
			&& Zone != EPrivacyZone::SEMI_PRIVATE)
		{
			// Service and private rooms directly adjacent to entry is a warning
			FString Msg = FString::Printf(TEXT("Privacy warning: '%s' (%s) is 1 step from entry but is not PUBLIC/SEMI_PUBLIC"),
				*Rooms[i].RoomId, *Rooms[i].RoomType);
			OutViolations.Add(Msg);
			UE_LOG(LogMonolithFloorPlan, Warning, TEXT("%s"), *Msg);
			++ViolationCount;
		}
	}

	return ViolationCount;
}

// ============================================================================
// WP-2: Circulation Patterns
// ============================================================================

FMonolithMeshFloorPlanGenerator::ECirculationType
FMonolithMeshFloorPlanGenerator::ParseCirculationType(const FString& TypeStr)
{
	if (TypeStr == TEXT("hub_spoke") || TypeStr == TEXT("hub_and_spoke"))
		return ECirculationType::HubSpoke;
	if (TypeStr == TEXT("racetrack") || TypeStr == TEXT("loop"))
		return ECirculationType::Racetrack;
	if (TypeStr == TEXT("enfilade"))
		return ECirculationType::Enfilade;
	return ECirculationType::DoubleLoaded;
}

void FMonolithMeshFloorPlanGenerator::InsertCorridorsForCirculation(
	ECirculationType Circulation,
	TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	TArray<FRoomDef>& Rooms, const TArray<FAdjacencyRule>& Adjacency,
	bool bHospiceMode, FRandomStream& Rng)
{
	switch (Circulation)
	{
	case ECirculationType::HubSpoke:
		InsertCorridorsHubSpoke(Grid, GridW, GridH, Rooms, Adjacency, bHospiceMode, Rng);
		break;
	case ECirculationType::Racetrack:
		InsertCorridorsRacetrack(Grid, GridW, GridH, Rooms, Adjacency, bHospiceMode, Rng);
		break;
	case ECirculationType::Enfilade:
		InsertCorridorsEnfilade(Grid, GridW, GridH, Rooms, Adjacency, bHospiceMode, Rng);
		break;
	case ECirculationType::DoubleLoaded:
	default:
		InsertCorridors(Grid, GridW, GridH, Rooms, Adjacency, bHospiceMode, Rng);
		break;
	}
}

void FMonolithMeshFloorPlanGenerator::InsertCorridorsHubSpoke(
	TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	TArray<FRoomDef>& Rooms, const TArray<FAdjacencyRule>& Adjacency,
	bool bHospiceMode, FRandomStream& Rng)
{
	// Hub-and-spoke: find the entry/foyer/lobby room (the hub).
	// If all rooms are directly adjacent to the hub, no corridor needed.
	// Only generate corridor for rooms that can't reach the hub.

	int32 HubIdx = -1;
	static const TArray<FString> HubTypes = {
		TEXT("entryway"), TEXT("foyer"), TEXT("lobby"), TEXT("vestibule"),
		TEXT("living_room")
	};

	for (int32 i = 0; i < Rooms.Num(); ++i)
	{
		if (HubTypes.Contains(Rooms[i].RoomType))
		{
			HubIdx = i;
			break;
		}
	}

	if (HubIdx < 0)
	{
		// No hub found -- fall back to double-loaded
		InsertCorridors(Grid, GridW, GridH, Rooms, Adjacency, bHospiceMode, Rng);
		return;
	}

	// Check which rooms are NOT adjacent to the hub
	TArray<int32> Unreachable;
	for (int32 i = 0; i < Rooms.Num(); ++i)
	{
		if (i == HubIdx) continue;
		if (!RoomsShareEdge(Rooms[i], Rooms[HubIdx]))
		{
			Unreachable.Add(i);
		}
	}

	if (Unreachable.Num() == 0)
	{
		UE_LOG(LogMonolithFloorPlan, Log, TEXT("Hub-spoke: all %d rooms adjacent to hub '%s', no corridor needed"),
			Rooms.Num() - 1, *Rooms[HubIdx].RoomId);
		return;
	}

	// For unreachable rooms, build corridor paths from them to the hub
	int32 CorridorRoomIndex = Rooms.Num();
	FRoomDef CorridorRoom;
	CorridorRoom.RoomId = TEXT("corridor");
	CorridorRoom.RoomType = TEXT("corridor");

	int32 CorridorWidth = bHospiceMode ? 4 : 3;

	for (int32 Idx : Unreachable)
	{
		TArray<FIntPoint> Path = FindCorridorPath(Grid, GridW, GridH, Rooms[Idx], Rooms[HubIdx], CorridorRoomIndex, CorridorWidth);
		for (const FIntPoint& P : Path)
		{
			if (P.X >= 0 && P.Y >= 0 && P.X < GridW && P.Y < GridH)
			{
				int32 Current = Grid[P.Y][P.X];
				if (Current == -1 || Current == CorridorRoomIndex)
				{
					Grid[P.Y][P.X] = CorridorRoomIndex;
					CorridorRoom.GridCells.AddUnique(P);
				}
			}
		}
	}

	if (CorridorRoom.GridCells.Num() > 0)
	{
		Rooms.Add(MoveTemp(CorridorRoom));
		UE_LOG(LogMonolithFloorPlan, Log, TEXT("Hub-spoke: corridor with %d cells connects %d unreachable room(s) to hub"),
			Rooms.Last().GridCells.Num(), Unreachable.Num());
	}
}

void FMonolithMeshFloorPlanGenerator::InsertCorridorsRacetrack(
	TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	TArray<FRoomDef>& Rooms, const TArray<FAdjacencyRule>& Adjacency,
	bool bHospiceMode, FRandomStream& Rng)
{
	// Racetrack: create a loop corridor around a central core.
	// Core = center cells reserved for stairwell/elevator/restrooms.
	// Corridor forms a rectangular loop between core and perimeter rooms.

	int32 CorridorWidth = bHospiceMode ? 4 : 3;

	// Calculate core region (center ~30% of grid)
	int32 CoreMarginX = FMath::Max(CorridorWidth + 2, GridW / 4);
	int32 CoreMarginY = FMath::Max(CorridorWidth + 2, GridH / 4);
	int32 CoreX1 = CoreMarginX;
	int32 CoreY1 = CoreMarginY;
	int32 CoreX2 = GridW - CoreMarginX - 1;
	int32 CoreY2 = GridH - CoreMarginY - 1;

	// Ensure core is at least 2x2
	if (CoreX2 <= CoreX1) { CoreX1 = GridW / 3; CoreX2 = 2 * GridW / 3; }
	if (CoreY2 <= CoreY1) { CoreY1 = GridH / 3; CoreY2 = 2 * GridH / 3; }

	int32 CorridorRoomIndex = Rooms.Num();
	FRoomDef CorridorRoom;
	CorridorRoom.RoomId = TEXT("corridor");
	CorridorRoom.RoomType = TEXT("corridor");

	// Carve the loop corridor just outside the core
	// Top edge of loop
	for (int32 X = CoreX1 - CorridorWidth; X <= CoreX2 + CorridorWidth; ++X)
	{
		for (int32 W = 0; W < CorridorWidth; ++W)
		{
			int32 Y = CoreY1 - CorridorWidth + W;
			if (X >= 0 && X < GridW && Y >= 0 && Y < GridH)
			{
				if (Grid[Y][X] == -1 || Grid[Y][X] == CorridorRoomIndex)
				{
					Grid[Y][X] = CorridorRoomIndex;
					CorridorRoom.GridCells.AddUnique(FIntPoint(X, Y));
				}
			}
		}
	}

	// Bottom edge of loop
	for (int32 X = CoreX1 - CorridorWidth; X <= CoreX2 + CorridorWidth; ++X)
	{
		for (int32 W = 0; W < CorridorWidth; ++W)
		{
			int32 Y = CoreY2 + 1 + W;
			if (X >= 0 && X < GridW && Y >= 0 && Y < GridH)
			{
				if (Grid[Y][X] == -1 || Grid[Y][X] == CorridorRoomIndex)
				{
					Grid[Y][X] = CorridorRoomIndex;
					CorridorRoom.GridCells.AddUnique(FIntPoint(X, Y));
				}
			}
		}
	}

	// Left edge of loop (between top and bottom)
	for (int32 Y = CoreY1 - CorridorWidth; Y <= CoreY2 + CorridorWidth; ++Y)
	{
		for (int32 W = 0; W < CorridorWidth; ++W)
		{
			int32 X = CoreX1 - CorridorWidth + W;
			if (X >= 0 && X < GridW && Y >= 0 && Y < GridH)
			{
				if (Grid[Y][X] == -1 || Grid[Y][X] == CorridorRoomIndex)
				{
					Grid[Y][X] = CorridorRoomIndex;
					CorridorRoom.GridCells.AddUnique(FIntPoint(X, Y));
				}
			}
		}
	}

	// Right edge of loop
	for (int32 Y = CoreY1 - CorridorWidth; Y <= CoreY2 + CorridorWidth; ++Y)
	{
		for (int32 W = 0; W < CorridorWidth; ++W)
		{
			int32 X = CoreX2 + 1 + W;
			if (X >= 0 && X < GridW && Y >= 0 && Y < GridH)
			{
				if (Grid[Y][X] == -1 || Grid[Y][X] == CorridorRoomIndex)
				{
					Grid[Y][X] = CorridorRoomIndex;
					CorridorRoom.GridCells.AddUnique(FIntPoint(X, Y));
				}
			}
		}
	}

	if (CorridorRoom.GridCells.Num() > 0)
	{
		Rooms.Add(MoveTemp(CorridorRoom));

		// Ensure all rooms can reach the corridor
		for (int32 i = 0; i < Rooms.Num() - 1; ++i)
		{
			if (!RoomsShareEdge(Rooms[i], Rooms.Last()))
			{
				TArray<FIntPoint> Path = FindCorridorPath(Grid, GridW, GridH, Rooms[i], Rooms.Last(), CorridorRoomIndex, 1);
				for (const FIntPoint& P : Path)
				{
					if (P.X >= 0 && P.Y >= 0 && P.X < GridW && P.Y < GridH)
					{
						if (Grid[P.Y][P.X] == -1 || Grid[P.Y][P.X] == CorridorRoomIndex)
						{
							Grid[P.Y][P.X] = CorridorRoomIndex;
							Rooms.Last().GridCells.AddUnique(P);
						}
					}
				}
			}
		}

		UE_LOG(LogMonolithFloorPlan, Log, TEXT("Racetrack: loop corridor with %d cells, core=(%d,%d)-(%d,%d)"),
			Rooms.Last().GridCells.Num(), CoreX1, CoreY1, CoreX2, CoreY2);
	}
	else
	{
		// Fallback to double-loaded if racetrack couldn't carve cells
		InsertCorridors(Grid, GridW, GridH, Rooms, Adjacency, bHospiceMode, Rng);
	}
}

void FMonolithMeshFloorPlanGenerator::InsertCorridorsEnfilade(
	TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	TArray<FRoomDef>& Rooms, const TArray<FAdjacencyRule>& Adjacency,
	bool bHospiceMode, FRandomStream& Rng)
{
	// Enfilade: rooms connect directly through aligned doors.
	// No corridor generated. Rooms chain sequentially.
	// Just verify all rooms share at least one edge with a neighbor.

	// Build a graph of which rooms are adjacent
	TSet<int32> Connected;
	TSet<int32> All;
	for (int32 i = 0; i < Rooms.Num(); ++i)
		All.Add(i);

	if (Rooms.Num() == 0) return;

	// BFS from first room to find connected component
	TQueue<int32> Queue;
	Queue.Enqueue(0);
	Connected.Add(0);

	while (!Queue.IsEmpty())
	{
		int32 Current;
		Queue.Dequeue(Current);

		for (int32 Other = 0; Other < Rooms.Num(); ++Other)
		{
			if (Connected.Contains(Other)) continue;
			if (RoomsShareEdge(Rooms[Current], Rooms[Other]))
			{
				Connected.Add(Other);
				Queue.Enqueue(Other);
			}
		}
	}

	// If some rooms are disconnected, add minimal corridor to connect them
	TSet<int32> Disconnected = All.Difference(Connected);
	if (Disconnected.Num() > 0)
	{
		int32 CorridorRoomIndex = Rooms.Num();
		FRoomDef CorridorRoom;
		CorridorRoom.RoomId = TEXT("corridor");
		CorridorRoom.RoomType = TEXT("corridor");

		int32 CorridorWidth = bHospiceMode ? 4 : 2;  // Narrower for enfilade

		for (int32 DisIdx : Disconnected)
		{
			// Find closest connected room and path to it
			int32 ClosestConnected = *Connected.begin();
			TArray<FIntPoint> BestPath;

			for (int32 ConIdx : Connected)
			{
				TArray<FIntPoint> Path = FindCorridorPath(Grid, GridW, GridH, Rooms[DisIdx], Rooms[ConIdx], CorridorRoomIndex, CorridorWidth);
				if (Path.Num() > 0 && (BestPath.Num() == 0 || Path.Num() < BestPath.Num()))
				{
					BestPath = Path;
				}
			}

			for (const FIntPoint& P : BestPath)
			{
				if (P.X >= 0 && P.Y >= 0 && P.X < GridW && P.Y < GridH)
				{
					if (Grid[P.Y][P.X] == -1 || Grid[P.Y][P.X] == CorridorRoomIndex)
					{
						Grid[P.Y][P.X] = CorridorRoomIndex;
						CorridorRoom.GridCells.AddUnique(P);
					}
				}
			}
		}

		if (CorridorRoom.GridCells.Num() > 0)
		{
			Rooms.Add(MoveTemp(CorridorRoom));
			UE_LOG(LogMonolithFloorPlan, Log, TEXT("Enfilade: minimal corridor with %d cells to connect %d disconnected room(s)"),
				Rooms.Last().GridCells.Num(), Disconnected.Num());
		}
	}
	else
	{
		UE_LOG(LogMonolithFloorPlan, Log, TEXT("Enfilade: all %d rooms connected directly, no corridor needed"), Rooms.Num());
	}
}

// ============================================================================
// File I/O
// ============================================================================

FString FMonolithMeshFloorPlanGenerator::GetArchetypeRoofType(const FString& ArchetypeName)
{
	FBuildingArchetype Arch;
	FString Err;
	if (LoadArchetype(ArchetypeName, Arch, Err))
	{
		return Arch.RoofType;
	}
	return TEXT("gable");
}

FString FMonolithMeshFloorPlanGenerator::GetArchetypeDirectory()
{
	return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("Monolith"), TEXT("Saved"), TEXT("Monolith"), TEXT("BuildingArchetypes"));
}

bool FMonolithMeshFloorPlanGenerator::LoadArchetype(const FString& ArchetypeName, FBuildingArchetype& OutArchetype, FString& OutError)
{
	FString FilePath;
	if (ArchetypeName.EndsWith(TEXT(".json")))
	{
		FilePath = ArchetypeName;
	}
	else
	{
		FilePath = FPaths::Combine(GetArchetypeDirectory(), ArchetypeName + TEXT(".json"));
	}

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		OutError = FString::Printf(TEXT("Could not load archetype file: %s"), *FilePath);
		return false;
	}

	TSharedPtr<FJsonObject> Json = FMonolithJsonUtils::Parse(JsonString);
	if (!Json.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to parse archetype JSON: %s"), *FilePath);
		return false;
	}

	return ParseArchetypeJson(Json, OutArchetype, OutError);
}

bool FMonolithMeshFloorPlanGenerator::ParseArchetypeJson(const TSharedPtr<FJsonObject>& Json, FBuildingArchetype& OutArchetype, FString& OutError)
{
	Json->TryGetStringField(TEXT("name"), OutArchetype.Name);
	Json->TryGetStringField(TEXT("description"), OutArchetype.Description);
	Json->TryGetStringField(TEXT("roof_type"), OutArchetype.RoofType);

	// Parse floor_height
	if (Json->HasField(TEXT("floor_height")))
		OutArchetype.FloorHeight = static_cast<float>(Json->GetNumberField(TEXT("floor_height")));

	// Parse material_hints
	const TSharedPtr<FJsonObject>* MatHintsObj = nullptr;
	if (Json->TryGetObjectField(TEXT("material_hints"), MatHintsObj) && MatHintsObj && (*MatHintsObj).IsValid())
	{
		(*MatHintsObj)->TryGetStringField(TEXT("exterior"), OutArchetype.MaterialHints.Exterior);
		(*MatHintsObj)->TryGetStringField(TEXT("interior"), OutArchetype.MaterialHints.Interior);
		(*MatHintsObj)->TryGetStringField(TEXT("floor"), OutArchetype.MaterialHints.FloorMaterial);
	}

	// Parse floors
	const TSharedPtr<FJsonObject>* FloorsObj = nullptr;
	if (Json->TryGetObjectField(TEXT("floors"), FloorsObj) && FloorsObj && (*FloorsObj).IsValid())
	{
		if ((*FloorsObj)->HasField(TEXT("min")))
			OutArchetype.FloorsMin = static_cast<int32>((*FloorsObj)->GetNumberField(TEXT("min")));
		if ((*FloorsObj)->HasField(TEXT("max")))
			OutArchetype.FloorsMax = static_cast<int32>((*FloorsObj)->GetNumberField(TEXT("max")));
	}

	// WP-2: Parse circulation type
	FString CircStr;
	if (Json->TryGetStringField(TEXT("circulation"), CircStr))
	{
		OutArchetype.Circulation = ParseCirculationType(CircStr);
	}

	// Parse rooms
	const TArray<TSharedPtr<FJsonValue>>* RoomsArr = nullptr;
	if (!Json->TryGetArrayField(TEXT("rooms"), RoomsArr) || !RoomsArr)
	{
		OutError = TEXT("Archetype missing 'rooms' array");
		return false;
	}

	for (int32 i = 0; i < RoomsArr->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>* RObj = nullptr;
		if (!(*RoomsArr)[i]->TryGetObject(RObj) || !RObj || !(*RObj).IsValid())
			continue;

		FArchetypeRoom Room;
		(*RObj)->TryGetStringField(TEXT("type"), Room.Type);

		if ((*RObj)->HasField(TEXT("min_area")))
			Room.MinArea = static_cast<float>((*RObj)->GetNumberField(TEXT("min_area")));
		if ((*RObj)->HasField(TEXT("max_area")))
			Room.MaxArea = static_cast<float>((*RObj)->GetNumberField(TEXT("max_area")));

		// Count can be a number or an array [min, max]
		if ((*RObj)->HasField(TEXT("count")))
		{
			const TArray<TSharedPtr<FJsonValue>>* CountArr = nullptr;
			if ((*RObj)->TryGetArrayField(TEXT("count"), CountArr) && CountArr && CountArr->Num() >= 2)
			{
				Room.CountMin = static_cast<int32>((*CountArr)[0]->AsNumber());
				Room.CountMax = static_cast<int32>((*CountArr)[1]->AsNumber());
			}
			else
			{
				int32 CountVal = static_cast<int32>((*RObj)->GetNumberField(TEXT("count")));
				Room.CountMin = CountVal;
				Room.CountMax = CountVal;
			}
		}

		if ((*RObj)->HasField(TEXT("required")))
			Room.bRequired = (*RObj)->GetBoolField(TEXT("required"));
		if ((*RObj)->HasField(TEXT("priority")))
			Room.Priority = static_cast<int32>((*RObj)->GetNumberField(TEXT("priority")));
		if ((*RObj)->HasField(TEXT("auto_generate")))
			Room.bAutoGenerate = (*RObj)->GetBoolField(TEXT("auto_generate"));
		if ((*RObj)->HasField(TEXT("exterior_wall")))
			Room.bExteriorWall = (*RObj)->GetBoolField(TEXT("exterior_wall"));

		// Per-floor assignment
		if ((*RObj)->HasField(TEXT("floor")))
			(*RObj)->TryGetStringField(TEXT("floor"), Room.Floor);

		// Aspect ratio constraints
		if ((*RObj)->HasField(TEXT("min_aspect")))
			Room.MinAspect = static_cast<float>((*RObj)->GetNumberField(TEXT("min_aspect")));
		if ((*RObj)->HasField(TEXT("max_aspect")))
			Room.MaxAspect = static_cast<float>((*RObj)->GetNumberField(TEXT("max_aspect")));

		OutArchetype.Rooms.Add(MoveTemp(Room));
	}

	// Parse adjacency (legacy array format)
	const TArray<TSharedPtr<FJsonValue>>* AdjArr = nullptr;
	if (Json->TryGetArrayField(TEXT("adjacency"), AdjArr) && AdjArr)
	{
		for (const auto& AdjVal : *AdjArr)
		{
			const TSharedPtr<FJsonObject>* AObj = nullptr;
			if (!AdjVal->TryGetObject(AObj) || !AObj || !(*AObj).IsValid())
				continue;

			FAdjacencyRule Rule;
			(*AObj)->TryGetStringField(TEXT("from"), Rule.From);
			(*AObj)->TryGetStringField(TEXT("to"), Rule.To);
			(*AObj)->TryGetStringField(TEXT("strength"), Rule.Strength);
			OutArchetype.Adjacency.Add(MoveTemp(Rule));
		}
	}

	// WP-2: Parse adjacency_matrix (new format, complements legacy adjacency array)
	const TSharedPtr<FJsonObject>* AdjMatObj = nullptr;
	if (Json->TryGetObjectField(TEXT("adjacency_matrix"), AdjMatObj) && AdjMatObj && (*AdjMatObj).IsValid())
	{
		for (const auto& OuterPair : (*AdjMatObj)->Values)
		{
			const FString& RoomTypeA = OuterPair.Key;
			const TSharedPtr<FJsonObject>* InnerObj = nullptr;
			if (!OuterPair.Value->TryGetObject(InnerObj) || !InnerObj || !(*InnerObj).IsValid())
				continue;

			TMap<FString, EAdjacencyRelation>& InnerMap = OutArchetype.AdjacencyMatrix.Rules.FindOrAdd(RoomTypeA);

			for (const auto& InnerPair : (*InnerObj)->Values)
			{
				const FString& RoomTypeB = InnerPair.Key;
				FString RelStr;
				if (InnerPair.Value->TryGetString(RelStr))
				{
					EAdjacencyRelation Rel = EAdjacencyRelation::MAY;
					if (RelStr == TEXT("MUST")) Rel = EAdjacencyRelation::MUST;
					else if (RelStr == TEXT("SHOULD")) Rel = EAdjacencyRelation::SHOULD;
					else if (RelStr == TEXT("MAY")) Rel = EAdjacencyRelation::MAY;
					else if (RelStr == TEXT("MAY_NOT")) Rel = EAdjacencyRelation::MAY_NOT;
					else if (RelStr == TEXT("MUST_NOT")) Rel = EAdjacencyRelation::MUST_NOT;
					else if (RelStr == TEXT("PREFERRED")) Rel = EAdjacencyRelation::SHOULD;

					InnerMap.Add(RoomTypeB, Rel);
				}
			}
		}
	}

	if (OutArchetype.Name.IsEmpty())
	{
		OutError = TEXT("Archetype missing 'name' field");
		return false;
	}

	return true;
}

// ============================================================================
// Room Resolution
// ============================================================================

TArray<FMonolithMeshFloorPlanGenerator::FRoomInstance> FMonolithMeshFloorPlanGenerator::ResolveRoomInstances(
	const FBuildingArchetype& Archetype, int32 GridW, int32 GridH, FRandomStream& Rng, int32 FloorIndex)
{
	TArray<FRoomInstance> Instances;
	const int32 TotalCells = GridW * GridH;

	for (const FArchetypeRoom& AR : Archetype.Rooms)
	{
		if (AR.bAutoGenerate)
			continue;  // Corridors are generated later

		// Per-floor filtering: skip rooms that don't belong on this floor
		if (FloorIndex >= 0)
		{
			const FString& RoomFloor = AR.Floor;
			if (RoomFloor == TEXT("ground") && FloorIndex != 0)
				continue;
			if (RoomFloor == TEXT("upper") && FloorIndex == 0)
				continue;
			// "every" and "any" (default) pass through on all floors
		}

		// Roll how many of this room type
		int32 Count = Rng.RandRange(AR.CountMin, AR.CountMax);

		for (int32 i = 0; i < Count; ++i)
		{
			FRoomInstance Inst;
			Inst.RoomType = AR.Type;
			Inst.RoomId = (Count > 1) ? FString::Printf(TEXT("%s_%d"), *AR.Type, i + 1) : AR.Type;
			Inst.Priority = AR.Priority;
			Inst.bExteriorWall = AR.bExteriorWall;
			Inst.Floor = AR.Floor;
			Inst.MinAspect = AR.MinAspect;
			Inst.MaxAspect = AR.MaxAspect;

			// Target area: random within range, clamped to available grid space
			float Area = Rng.FRandRange(AR.MinArea, AR.MaxArea);
			// Clamp to a reasonable fraction of the total
			Area = FMath::Clamp(Area, 1.0f, static_cast<float>(TotalCells) * 0.6f);
			Inst.TargetArea = Area;

			Instances.Add(MoveTemp(Inst));
		}
	}

	// Sort by priority (lower number = higher priority = gets laid out first = better aspect ratio)
	Instances.Sort([](const FRoomInstance& A, const FRoomInstance& B) { return A.Priority < B.Priority; });

	// Normalize areas so they sum to the total grid area
	float AreaSum = 0.0f;
	for (const FRoomInstance& Inst : Instances)
		AreaSum += Inst.TargetArea;

	if (AreaSum > 0.0f)
	{
		// Reserve ~15% for corridors
		const float UsableArea = static_cast<float>(TotalCells) * 0.85f;
		const float Scale = UsableArea / AreaSum;
		for (FRoomInstance& Inst : Instances)
		{
			Inst.TargetArea = FMath::Max(1.0f, Inst.TargetArea * Scale);
		}
	}

	return Instances;
}

// ============================================================================
// Validation
// ============================================================================

FString FMonolithMeshFloorPlanGenerator::ValidateFootprintCapacity(
	const FBuildingArchetype& Archetype, int32 GridW, int32 GridH, int32 FloorIndex)
{
	float TotalRequiredMin = 0.0f;
	for (const FArchetypeRoom& AR : Archetype.Rooms)
	{
		if (AR.bAutoGenerate)
			continue;
		if (!AR.bRequired)
			continue;

		// Per-floor filtering
		if (FloorIndex >= 0)
		{
			if (AR.Floor == TEXT("ground") && FloorIndex != 0)
				continue;
			if (AR.Floor == TEXT("upper") && FloorIndex == 0)
				continue;
		}

		TotalRequiredMin += AR.MinArea * static_cast<float>(AR.CountMin);
	}

	const float AvailableArea = static_cast<float>(GridW * GridH) * 0.85f;  // 85% after corridors
	if (TotalRequiredMin > AvailableArea)
	{
		// Convert to meters for a helpful error message
		float NeededM2 = TotalRequiredMin * 0.25f / 0.85f;  // Account for corridor reservation
		float NeededSide = FMath::Sqrt(NeededM2);
		float NeededCm = NeededSide * 100.0f;
		return FString::Printf(
			TEXT("Footprint too small for archetype '%s'. Required rooms need at least %.0f grid cells (%.0f m²) "
				"but footprint provides %.0f usable cells (%.0f m²). Try at least %.0fx%.0f cm."),
			*Archetype.Name,
			TotalRequiredMin, TotalRequiredMin * 0.25f,
			AvailableArea, AvailableArea * 0.25f,
			NeededCm, NeededCm);
	}

	return FString();
}

FString FMonolithMeshFloorPlanGenerator::ValidateStairwellRequirement(const FBuildingArchetype& Archetype)
{
	if (Archetype.FloorsMax <= 1)
		return FString();  // Single-floor archetypes don't need stairwells

	bool bHasStairwell = false;
	for (const FArchetypeRoom& AR : Archetype.Rooms)
	{
		if (AR.Type == TEXT("stairwell") || AR.Type == TEXT("stairs"))
		{
			bHasStairwell = true;
			if (AR.MinArea < 24.0f)
			{
				return FString::Printf(
					TEXT("Multi-floor archetype '%s' has stairwell with min_area %.0f but minimum is 24 cells "
						"(4x6 = 200x300cm). Increase stairwell min_area."),
					*Archetype.Name, AR.MinArea);
			}
			break;
		}
	}

	if (!bHasStairwell)
	{
		return FString::Printf(
			TEXT("Multi-floor archetype '%s' (%d-%d floors) has no stairwell room type. "
				"Add a room with type 'stairwell', floor 'every', and min_area >= 24."),
			*Archetype.Name, Archetype.FloorsMin, Archetype.FloorsMax);
	}

	return FString();
}

// ============================================================================
// Aspect Ratio Correction
// ============================================================================

void FMonolithMeshFloorPlanGenerator::CorrectAspectRatios(
	TArray<FGridRect>& Rects, const TArray<FRoomInstance>& Rooms, int32 GridW, int32 GridH)
{
	for (int32 i = 0; i < Rects.Num() && i < Rooms.Num(); ++i)
	{
		FGridRect& R = Rects[i];
		if (R.W <= 0 || R.H <= 0)
			continue;

		float MaxAspect = Rooms[i].MaxAspect;
		float CurrentAspect = R.AspectRatio();

		if (CurrentAspect <= MaxAspect)
			continue;

		// Room exceeds max aspect ratio. Try to reshape it.
		// Strategy: reduce the longer dimension and increase the shorter one,
		// keeping area roughly the same (within grid constraints).
		int32 Area = R.Area();
		bool bWideRoom = R.W > R.H;

		// Calculate ideal dimensions for the target aspect ratio
		// Area = W * H, W/H = MaxAspect => W = sqrt(Area * MaxAspect), H = sqrt(Area / MaxAspect)
		int32 IdealW, IdealH;
		if (bWideRoom)
		{
			IdealW = FMath::RoundToInt32(FMath::Sqrt(static_cast<float>(Area) * MaxAspect));
			IdealH = FMath::Max(1, FMath::RoundToInt32(static_cast<float>(Area) / static_cast<float>(IdealW)));
		}
		else
		{
			IdealH = FMath::RoundToInt32(FMath::Sqrt(static_cast<float>(Area) * MaxAspect));
			IdealW = FMath::Max(1, FMath::RoundToInt32(static_cast<float>(Area) / static_cast<float>(IdealH)));
		}

		// Clamp to grid bounds
		IdealW = FMath::Clamp(IdealW, 1, GridW - R.X);
		IdealH = FMath::Clamp(IdealH, 1, GridH - R.Y);

		// Only apply if the new aspect ratio is actually better
		float NewAspect = (IdealW > 0 && IdealH > 0)
			? FMath::Max(static_cast<float>(IdealW) / IdealH, static_cast<float>(IdealH) / IdealW)
			: CurrentAspect;

		if (NewAspect < CurrentAspect)
		{
			UE_LOG(LogMonolithFloorPlan, Log, TEXT("Correcting aspect ratio for '%s': %dx%d (%.1f) -> %dx%d (%.1f)"),
				*Rooms[i].RoomId, R.W, R.H, CurrentAspect, IdealW, IdealH, NewAspect);
			R.W = IdealW;
			R.H = IdealH;
		}
	}
}

// ============================================================================
// Squarified Treemap
// ============================================================================

float FMonolithMeshFloorPlanGenerator::WorstAspectRatio(const TArray<float>& RowAreas, float SideLength)
{
	if (RowAreas.Num() == 0 || SideLength <= 0.0f)
		return TNumericLimits<float>::Max();

	float RowTotal = 0.0f;
	for (float A : RowAreas)
		RowTotal += A;

	if (RowTotal <= 0.0f)
		return TNumericLimits<float>::Max();

	float RowWidth = RowTotal / SideLength;
	float Worst = 0.0f;

	for (float A : RowAreas)
	{
		if (A <= 0.0f) continue;
		float H = A / RowWidth;
		float Ratio = FMath::Max(RowWidth / H, H / RowWidth);
		Worst = FMath::Max(Worst, Ratio);
	}

	return Worst;
}

void FMonolithMeshFloorPlanGenerator::LayoutRow(const TArray<int32>& RowIndices, const TArray<float>& Areas,
	FGridRect& Rect, bool bHorizontal, TArray<FGridRect>& OutRects)
{
	if (RowIndices.Num() == 0) return;

	float RowTotal = 0.0f;
	for (int32 Idx : RowIndices)
		RowTotal += Areas[Idx];

	if (bHorizontal)
	{
		// Row is laid out along the X axis, consuming some height
		int32 RowHeight = FMath::Max(1, FMath::RoundToInt32(RowTotal / static_cast<float>(Rect.W)));
		RowHeight = FMath::Min(RowHeight, Rect.H);

		int32 CurX = Rect.X;
		for (int32 i = 0; i < RowIndices.Num(); ++i)
		{
			int32 Idx = RowIndices[i];
			int32 CellW;
			if (i == RowIndices.Num() - 1)
			{
				// Last item gets remaining width to avoid rounding gaps
				CellW = (Rect.X + Rect.W) - CurX;
			}
			else
			{
				CellW = FMath::Max(1, FMath::RoundToInt32(Areas[Idx] / static_cast<float>(RowHeight)));
				CellW = FMath::Min(CellW, (Rect.X + Rect.W) - CurX);
			}

			FGridRect& R = OutRects[Idx];
			R.X = CurX;
			R.Y = Rect.Y;
			R.W = CellW;
			R.H = RowHeight;
			CurX += CellW;
		}

		// Shrink remaining rect
		Rect.Y += RowHeight;
		Rect.H -= RowHeight;
	}
	else
	{
		// Row is laid out along the Y axis, consuming some width
		int32 RowWidth = FMath::Max(1, FMath::RoundToInt32(RowTotal / static_cast<float>(Rect.H)));
		RowWidth = FMath::Min(RowWidth, Rect.W);

		int32 CurY = Rect.Y;
		for (int32 i = 0; i < RowIndices.Num(); ++i)
		{
			int32 Idx = RowIndices[i];
			int32 CellH;
			if (i == RowIndices.Num() - 1)
			{
				CellH = (Rect.Y + Rect.H) - CurY;
			}
			else
			{
				CellH = FMath::Max(1, FMath::RoundToInt32(Areas[Idx] / static_cast<float>(RowWidth)));
				CellH = FMath::Min(CellH, (Rect.Y + Rect.H) - CurY);
			}

			FGridRect& R = OutRects[Idx];
			R.X = Rect.X;
			R.Y = CurY;
			R.W = RowWidth;
			R.H = CellH;
			CurY += CellH;
		}

		// Shrink remaining rect
		Rect.X += RowWidth;
		Rect.W -= RowWidth;
	}
}

TArray<FMonolithMeshFloorPlanGenerator::FGridRect> FMonolithMeshFloorPlanGenerator::SquarifiedTreemapLayout(
	TArray<FRoomInstance>& Rooms, int32 GridW, int32 GridH)
{
	TArray<FGridRect> Rects;
	Rects.SetNum(Rooms.Num());

	if (Rooms.Num() == 0)
		return Rects;

	// Build area array (already sorted by priority from ResolveRoomInstances)
	// Re-sort by area descending for treemap algorithm
	TArray<int32> SortedIndices;
	SortedIndices.Reserve(Rooms.Num());
	for (int32 i = 0; i < Rooms.Num(); ++i)
		SortedIndices.Add(i);

	SortedIndices.Sort([&Rooms](int32 A, int32 B) { return Rooms[A].TargetArea > Rooms[B].TargetArea; });

	TArray<float> Areas;
	Areas.SetNum(Rooms.Num());
	for (int32 i = 0; i < Rooms.Num(); ++i)
		Areas[i] = Rooms[i].TargetArea;

	FGridRect Remaining;
	Remaining.X = 0;
	Remaining.Y = 0;
	Remaining.W = GridW;
	Remaining.H = GridH;

	TArray<int32> CurrentRow;
	TArray<float> CurrentRowAreas;
	int32 Cursor = 0;

	while (Cursor < SortedIndices.Num())
	{
		if (Remaining.W <= 0 || Remaining.H <= 0)
		{
			// No space left -- force remaining rooms into last pixel
			for (int32 i = Cursor; i < SortedIndices.Num(); ++i)
			{
				FGridRect& R = Rects[SortedIndices[i]];
				R.X = FMath::Max(0, Remaining.X);
				R.Y = FMath::Max(0, Remaining.Y);
				R.W = FMath::Max(1, Remaining.W);
				R.H = FMath::Max(1, Remaining.H);
			}
			break;
		}

		bool bHorizontal = Remaining.W >= Remaining.H;
		float SideLength = bHorizontal ? static_cast<float>(Remaining.W) : static_cast<float>(Remaining.H);

		int32 Idx = SortedIndices[Cursor];

		// Try adding this room to the current row
		TArray<float> CandidateAreas = CurrentRowAreas;
		CandidateAreas.Add(Areas[Idx]);

		float CurrentWorst = (CurrentRowAreas.Num() > 0)
			? WorstAspectRatio(CurrentRowAreas, SideLength)
			: TNumericLimits<float>::Max();
		float CandidateWorst = WorstAspectRatio(CandidateAreas, SideLength);

		if (CandidateWorst <= CurrentWorst || CurrentRow.Num() == 0)
		{
			// Adding improves (or doesn't worsen) aspect ratios -- keep going
			CurrentRow.Add(Idx);
			CurrentRowAreas.Add(Areas[Idx]);
			++Cursor;
		}
		else
		{
			// Finalize current row
			LayoutRow(CurrentRow, Areas, Remaining, bHorizontal, Rects);
			CurrentRow.Reset();
			CurrentRowAreas.Reset();
			// Don't advance cursor -- re-evaluate this room against the new remaining rect
		}
	}

	// Lay out any remaining row
	if (CurrentRow.Num() > 0 && Remaining.W > 0 && Remaining.H > 0)
	{
		bool bHorizontal = Remaining.W >= Remaining.H;
		LayoutRow(CurrentRow, Areas, Remaining, bHorizontal, Rects);
	}

	return Rects;
}

// ============================================================================
// Grid construction
// ============================================================================

TArray<TArray<int32>> FMonolithMeshFloorPlanGenerator::BuildGridFromRects(
	const TArray<FGridRect>& Rects, int32 GridW, int32 GridH)
{
	// Initialize with -1 (empty)
	TArray<TArray<int32>> Grid;
	Grid.SetNum(GridH);
	for (int32 Y = 0; Y < GridH; ++Y)
	{
		Grid[Y].SetNum(GridW);
		for (int32 X = 0; X < GridW; ++X)
			Grid[Y][X] = -1;
	}

	// Paint rooms. Later rooms overwrite earlier ones at overlaps (shouldn't happen with treemap).
	for (int32 RoomIdx = 0; RoomIdx < Rects.Num(); ++RoomIdx)
	{
		const FGridRect& R = Rects[RoomIdx];
		for (int32 Y = R.Y; Y < R.Y + R.H && Y < GridH; ++Y)
		{
			for (int32 X = R.X; X < R.X + R.W && X < GridW; ++X)
			{
				Grid[Y][X] = RoomIdx;
			}
		}
	}

	return Grid;
}

TArray<FRoomDef> FMonolithMeshFloorPlanGenerator::BuildRoomDefs(
	const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	const TArray<FRoomInstance>& Instances)
{
	TArray<FRoomDef> Rooms;
	Rooms.SetNum(Instances.Num());

	for (int32 i = 0; i < Instances.Num(); ++i)
	{
		Rooms[i].RoomId = Instances[i].RoomId;
		Rooms[i].RoomType = Instances[i].RoomType;
	}

	// Collect grid cells for each room
	for (int32 Y = 0; Y < GridH; ++Y)
	{
		for (int32 X = 0; X < GridW; ++X)
		{
			int32 Idx = Grid[Y][X];
			if (Idx >= 0 && Idx < Rooms.Num())
			{
				Rooms[Idx].GridCells.Add(FIntPoint(X, Y));
			}
		}
	}

	return Rooms;
}

// ============================================================================
// Corridor insertion (double-loaded, default)
// ============================================================================

bool FMonolithMeshFloorPlanGenerator::RoomsShareEdge(const FRoomDef& A, const FRoomDef& B)
{
	// Two rooms share an edge if any cell in A is cardinally adjacent to any cell in B
	TSet<FIntPoint> SetB;
	for (const FIntPoint& P : B.GridCells)
		SetB.Add(P);

	static const FIntPoint Dirs[] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
	for (const FIntPoint& P : A.GridCells)
	{
		for (const FIntPoint& D : Dirs)
		{
			if (SetB.Contains(P + D))
				return true;
		}
	}
	return false;
}

TArray<FIntPoint> FMonolithMeshFloorPlanGenerator::FindCorridorPath(
	const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	const FRoomDef& From, const FRoomDef& To, int32 CorridorRoomIndex, int32 CorridorWidth)
{
	TArray<FIntPoint> Path;

	// BFS from any cell adjacent to From toward any cell adjacent to To
	// through empty (-1) or existing corridor cells

	TSet<FIntPoint> FromEdge, ToEdge;
	static const FIntPoint Dirs[] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

	auto IsPassable = [&](int32 X, int32 Y) -> bool
	{
		if (X < 0 || Y < 0 || X >= GridW || Y >= GridH) return false;
		int32 Val = Grid[Y][X];
		return Val == -1 || Val == CorridorRoomIndex;
	};

	// Find cells adjacent to room From that are passable
	for (const FIntPoint& P : From.GridCells)
	{
		for (const FIntPoint& D : Dirs)
		{
			FIntPoint N = P + D;
			if (N.X >= 0 && N.Y >= 0 && N.X < GridW && N.Y < GridH && IsPassable(N.X, N.Y))
				FromEdge.Add(N);
		}
	}

	// Find cells that are part of To or adjacent to To
	TSet<FIntPoint> ToSet;
	for (const FIntPoint& P : To.GridCells)
		ToSet.Add(P);

	for (const FIntPoint& P : To.GridCells)
	{
		for (const FIntPoint& D : Dirs)
		{
			FIntPoint N = P + D;
			if (IsPassable(N.X, N.Y))
				ToEdge.Add(N);
		}
	}

	if (FromEdge.Num() == 0 || ToEdge.Num() == 0)
		return Path;

	// BFS
	TMap<FIntPoint, FIntPoint> CameFrom;  // child -> parent
	TQueue<FIntPoint> Queue;

	for (const FIntPoint& Start : FromEdge)
	{
		Queue.Enqueue(Start);
		CameFrom.Add(Start, FIntPoint(-1, -1));
	}

	FIntPoint Goal(-1, -1);
	while (!Queue.IsEmpty())
	{
		FIntPoint Current;
		Queue.Dequeue(Current);

		// Check if we reached a cell adjacent to the target room
		if (ToEdge.Contains(Current) || ToSet.Contains(Current))
		{
			Goal = Current;
			break;
		}

		for (const FIntPoint& D : Dirs)
		{
			FIntPoint N = Current + D;
			if (!IsPassable(N.X, N.Y) && !ToSet.Contains(N))
				continue;
			if (CameFrom.Contains(N))
				continue;

			CameFrom.Add(N, Current);
			Queue.Enqueue(N);
		}
	}

	if (Goal.X < 0)
		return Path;  // No path found

	// Reconstruct path
	FIntPoint Cur = Goal;
	while (Cur.X >= 0 && Cur.Y >= 0)
	{
		// Don't include cells that belong to the target room
		if (!ToSet.Contains(Cur))
			Path.Add(Cur);

		FIntPoint* Parent = CameFrom.Find(Cur);
		if (!Parent || (Parent->X < 0 && Parent->Y < 0))
			break;
		Cur = *Parent;
	}

	// Widen corridor if requested (for hospice mode)
	if (CorridorWidth > 1)
	{
		TSet<FIntPoint> Widened;
		for (const FIntPoint& P : Path)
			Widened.Add(P);

		for (const FIntPoint& P : Path)
		{
			// Add cells in the perpendicular direction
			for (int32 Offset = 1; Offset < CorridorWidth; ++Offset)
			{
				// Try both perpendicular directions
				FIntPoint W1(P.X + Offset, P.Y);
				FIntPoint W2(P.X, P.Y + Offset);

				if (W1.X < GridW && (Grid[W1.Y][W1.X] == -1 || Grid[W1.Y][W1.X] == CorridorRoomIndex))
					Widened.Add(W1);
				if (W2.Y < GridH && (Grid[W2.Y][W2.X] == -1 || Grid[W2.Y][W2.X] == CorridorRoomIndex))
					Widened.Add(W2);
			}
		}

		Path = Widened.Array();
	}

	return Path;
}

void FMonolithMeshFloorPlanGenerator::InsertCorridors(
	TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	TArray<FRoomDef>& Rooms, const TArray<FAdjacencyRule>& Adjacency,
	bool bHospiceMode, FRandomStream& Rng)
{
	// Build a map from room type -> room indices (handles multiple instances like bedroom_1, bedroom_2)
	TMap<FString, TArray<int32>> TypeToIndices;
	for (int32 i = 0; i < Rooms.Num(); ++i)
	{
		TypeToIndices.FindOrAdd(Rooms[i].RoomType).Add(i);
	}

	// Determine corridor room index (always the next available)
	int32 CorridorRoomIndex = Rooms.Num();

	// Collect all room pairs that need connectivity but don't share an edge
	TArray<TPair<int32, int32>> NeedsCorridor;

	for (const FAdjacencyRule& Rule : Adjacency)
	{
		if (Rule.Strength == TEXT("weak"))
			continue;  // Weak adjacencies don't need corridors

		const TArray<int32>* FromIndices = TypeToIndices.Find(Rule.From);
		const TArray<int32>* ToIndices = TypeToIndices.Find(Rule.To);

		if (!FromIndices || !ToIndices)
			continue;

		for (int32 Fi : *FromIndices)
		{
			for (int32 Ti : *ToIndices)
			{
				if (Fi == Ti) continue;
				if (!RoomsShareEdge(Rooms[Fi], Rooms[Ti]))
				{
					NeedsCorridor.Add(TPair<int32, int32>(Fi, Ti));
				}
			}
		}
	}

	if (NeedsCorridor.Num() == 0)
	{
		// All required adjacencies are satisfied -- still add a minimal corridor for circulation
		// Find the largest room and carve a corridor strip along its longest interior edge
		return;
	}

	// Create the corridor room definition
	FRoomDef CorridorRoom;
	CorridorRoom.RoomId = TEXT("corridor");
	CorridorRoom.RoomType = TEXT("corridor");

	int32 CorridorWidth = bHospiceMode ? 4 : 3;  // 4 cells * 50cm = 200cm hospice; 3 cells * 50cm = 150cm normal (84cm capsule + 33cm clearance/side)

	// Carve corridors for each pair
	for (const auto& Pair : NeedsCorridor)
	{
		TArray<FIntPoint> Path = FindCorridorPath(Grid, GridW, GridH, Rooms[Pair.Key], Rooms[Pair.Value], CorridorRoomIndex, CorridorWidth);

		for (const FIntPoint& P : Path)
		{
			if (P.X >= 0 && P.Y >= 0 && P.X < GridW && P.Y < GridH)
			{
				int32 Current = Grid[P.Y][P.X];
				if (Current == -1 || Current == CorridorRoomIndex)
				{
					Grid[P.Y][P.X] = CorridorRoomIndex;
					CorridorRoom.GridCells.AddUnique(P);
				}
			}
		}
	}

	// Also ensure all rooms are reachable from the corridor.
	// For any room not adjacent to the corridor, carve a path.
	if (CorridorRoom.GridCells.Num() > 0)
	{
		for (int32 i = 0; i < Rooms.Num(); ++i)
		{
			if (!RoomsShareEdge(Rooms[i], CorridorRoom))
			{
				TArray<FIntPoint> Path = FindCorridorPath(Grid, GridW, GridH, Rooms[i], CorridorRoom, CorridorRoomIndex, CorridorWidth);
				for (const FIntPoint& P : Path)
				{
					if (P.X >= 0 && P.Y >= 0 && P.X < GridW && P.Y < GridH)
					{
						int32 Current = Grid[P.Y][P.X];
						if (Current == -1 || Current == CorridorRoomIndex)
						{
							Grid[P.Y][P.X] = CorridorRoomIndex;
							CorridorRoom.GridCells.AddUnique(P);
						}
					}
				}
			}
		}
	}

	// Only add the corridor room if it has any cells
	if (CorridorRoom.GridCells.Num() > 0)
	{
		Rooms.Add(MoveTemp(CorridorRoom));
	}

	// Also claim any remaining empty cells adjacent to corridors (prevents isolated pockets)
	bool bChanged = true;
	int32 Passes = 0;
	while (bChanged && Passes < 3)
	{
		bChanged = false;
		++Passes;
		static const FIntPoint Dirs[] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
		for (int32 Y = 0; Y < GridH; ++Y)
		{
			for (int32 X = 0; X < GridW; ++X)
			{
				if (Grid[Y][X] != -1) continue;

				// Count adjacent corridor cells
				int32 AdjCorridor = 0;
				for (const FIntPoint& D : Dirs)
				{
					int32 NX = X + D.X, NY = Y + D.Y;
					if (NX >= 0 && NY >= 0 && NX < GridW && NY < GridH && Grid[NY][NX] == CorridorRoomIndex)
						++AdjCorridor;
				}

				if (AdjCorridor >= 2)
				{
					Grid[Y][X] = CorridorRoomIndex;
					if (Rooms.Num() > CorridorRoomIndex)
						Rooms[CorridorRoomIndex].GridCells.Add(FIntPoint(X, Y));
					bChanged = true;
				}
			}
		}
	}
}

// ============================================================================
// Hospice rest alcoves
// ============================================================================

void FMonolithMeshFloorPlanGenerator::InsertRestAlcoves(
	TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	TArray<FRoomDef>& Rooms, int32 RoomsBetweenAlcoves, FRandomStream& Rng)
{
	// Find corridor room index
	int32 CorridorIdx = -1;
	for (int32 i = 0; i < Rooms.Num(); ++i)
	{
		if (Rooms[i].RoomType == TEXT("corridor"))
		{
			CorridorIdx = i;
			break;
		}
	}

	if (CorridorIdx < 0)
		return;  // No corridor to attach alcoves to

	// Count non-corridor rooms
	int32 RealRoomCount = 0;
	for (int32 i = 0; i < Rooms.Num(); ++i)
	{
		if (Rooms[i].RoomType != TEXT("corridor") && Rooms[i].RoomType != TEXT("rest_alcove"))
			++RealRoomCount;
	}

	int32 AlcoveCount = FMath::Max(1, RealRoomCount / RoomsBetweenAlcoves);
	static const FIntPoint Dirs[] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

	for (int32 A = 0; A < AlcoveCount; ++A)
	{
		// Pick a random corridor cell and try to carve an alcove next to it
		if (Rooms[CorridorIdx].GridCells.Num() == 0)
			break;

		for (int32 Attempt = 0; Attempt < 20; ++Attempt)
		{
			int32 CellIdx = Rng.RandRange(0, Rooms[CorridorIdx].GridCells.Num() - 1);
			FIntPoint Base = Rooms[CorridorIdx].GridCells[CellIdx];

			// Try each direction for a 2x2 alcove
			for (const FIntPoint& D : Dirs)
			{
				FIntPoint P1 = Base + D;
				FIntPoint P2 = P1 + FIntPoint(D.Y != 0 ? 1 : 0, D.X != 0 ? 1 : 0);  // Perpendicular

				if (P1.X < 0 || P1.Y < 0 || P1.X >= GridW || P1.Y >= GridH) continue;
				if (P2.X < 0 || P2.Y < 0 || P2.X >= GridW || P2.Y >= GridH) continue;
				if (Grid[P1.Y][P1.X] != -1 || Grid[P2.Y][P2.X] != -1) continue;

				// Found space for an alcove
				int32 AlcoveIdx = Rooms.Num();
				FRoomDef Alcove;
				Alcove.RoomId = FString::Printf(TEXT("rest_alcove_%d"), A + 1);
				Alcove.RoomType = TEXT("rest_alcove");
				Alcove.GridCells.Add(P1);
				Alcove.GridCells.Add(P2);

				Grid[P1.Y][P1.X] = AlcoveIdx;
				Grid[P2.Y][P2.X] = AlcoveIdx;
				Rooms.Add(MoveTemp(Alcove));
				goto NextAlcove;
			}
		}
		NextAlcove:;
	}
}

// ============================================================================
// Door placement
// ============================================================================

TArray<FIntPoint> FMonolithMeshFloorPlanGenerator::FindSharedEdge(
	const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	int32 RoomIndexA, int32 RoomIndexB)
{
	TArray<FIntPoint> SharedCells;
	static const FIntPoint Dirs[] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

	for (int32 Y = 0; Y < GridH; ++Y)
	{
		for (int32 X = 0; X < GridW; ++X)
		{
			if (Grid[Y][X] != RoomIndexA) continue;

			for (const FIntPoint& D : Dirs)
			{
				int32 NX = X + D.X, NY = Y + D.Y;
				if (NX >= 0 && NY >= 0 && NX < GridW && NY < GridH && Grid[NY][NX] == RoomIndexB)
				{
					SharedCells.Add(FIntPoint(X, Y));
					break;  // Only add each cell once
				}
			}
		}
	}

	return SharedCells;
}

TArray<FDoorDef> FMonolithMeshFloorPlanGenerator::PlaceDoors(
	const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	const TArray<FRoomDef>& Rooms, const TArray<FAdjacencyRule>& Adjacency,
	bool bHospiceMode, FRandomStream& Rng)
{
	TArray<FDoorDef> Doors;

	// Build type -> indices map
	TMap<FString, TArray<int32>> TypeToIndices;
	for (int32 i = 0; i < Rooms.Num(); ++i)
		TypeToIndices.FindOrAdd(Rooms[i].RoomType).Add(i);

	// Track which room pairs already have doors to avoid duplicates
	TSet<uint64> DoorPairs;
	auto PairKey = [](int32 A, int32 B) -> uint64
	{
		int32 Lo = FMath::Min(A, B);
		int32 Hi = FMath::Max(A, B);
		return (static_cast<uint64>(Lo) << 32) | static_cast<uint64>(Hi);
	};

	int32 DoorCounter = 0;
	float DoorWidth = bHospiceMode ? 120.0f : 110.0f;  // 110cm normal (84cm capsule + 13cm/side); 120cm hospice (18cm/side for wheelchair)

	// Place doors for each adjacency rule
	for (const FAdjacencyRule& Rule : Adjacency)
	{
		const TArray<int32>* FromIndices = TypeToIndices.Find(Rule.From);
		const TArray<int32>* ToIndices = TypeToIndices.Find(Rule.To);
		if (!FromIndices || !ToIndices) continue;

		for (int32 Fi : *FromIndices)
		{
			for (int32 Ti : *ToIndices)
			{
				if (Fi == Ti) continue;
				uint64 Key = PairKey(Fi, Ti);
				if (DoorPairs.Contains(Key)) continue;

				TArray<FIntPoint> SharedEdge = FindSharedEdge(Grid, GridW, GridH, Fi, Ti);
				if (SharedEdge.Num() == 0) continue;

				DoorPairs.Add(Key);

				// Pick a cell roughly in the middle of the shared edge for door placement
				int32 DoorCellIdx = SharedEdge.Num() / 2;
				FIntPoint DoorCell = SharedEdge[DoorCellIdx];

				// Determine which direction the door faces
				FIntPoint NeighborCell(INDEX_NONE, INDEX_NONE);
				static const FIntPoint Dirs[] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
				FString WallDir;
				for (const FIntPoint& D : Dirs)
				{
					int32 NX = DoorCell.X + D.X, NY = DoorCell.Y + D.Y;
					if (NX >= 0 && NY >= 0 && NX < GridW && NY < GridH && Grid[NY][NX] == Ti)
					{
						NeighborCell = FIntPoint(NX, NY);
						if (D.X > 0) WallDir = TEXT("east");
						else if (D.X < 0) WallDir = TEXT("west");
						else if (D.Y > 0) WallDir = TEXT("south");
						else WallDir = TEXT("north");
						break;
					}
				}

				FDoorDef Door;
				Door.DoorId = FString::Printf(TEXT("door_%02d"), ++DoorCounter);
				Door.RoomA = Rooms[Fi].RoomId;
				Door.RoomB = Rooms[Ti].RoomId;

				// WP-B: Convert room cells to wall boundary coordinates (R1-C2)
				// create_building_from_grid treats grid coordinates as left/top edges —
				// the wall boundary IS at the max coordinate between the two adjacent cells.
				if (NeighborCell.X != INDEX_NONE)
				{
					if (DoorCell.X != NeighborCell.X)
					{
						// Vertical wall: boundary at max X
						int32 WallX = FMath::Max(DoorCell.X, NeighborCell.X);
						Door.EdgeStart = FIntPoint(WallX, DoorCell.Y);
						Door.EdgeEnd = FIntPoint(WallX, DoorCell.Y);
					}
					else
					{
						// Horizontal wall: boundary at max Y
						int32 WallY = FMath::Max(DoorCell.Y, NeighborCell.Y);
						Door.EdgeStart = FIntPoint(DoorCell.X, WallY);
						Door.EdgeEnd = FIntPoint(DoorCell.X, WallY);
					}
				}
				else
				{
					Door.EdgeStart = DoorCell;
					Door.EdgeEnd = DoorCell;
				}

				Door.Wall = WallDir;
				Door.Width = DoorWidth;
				Door.Height = 220.0f;

				Doors.Add(MoveTemp(Door));
			}
		}
	}

	// Ensure corridor connects to every room that has a shared edge with it
	int32 CorridorIdx = -1;
	for (int32 i = 0; i < Rooms.Num(); ++i)
	{
		if (Rooms[i].RoomType == TEXT("corridor"))
		{
			CorridorIdx = i;
			break;
		}
	}

	if (CorridorIdx >= 0)
	{
		for (int32 i = 0; i < Rooms.Num(); ++i)
		{
			if (i == CorridorIdx) continue;
			if (Rooms[i].RoomType == TEXT("corridor")) continue;

			uint64 Key = PairKey(i, CorridorIdx);
			if (DoorPairs.Contains(Key)) continue;

			TArray<FIntPoint> SharedEdge = FindSharedEdge(Grid, GridW, GridH, i, CorridorIdx);
			if (SharedEdge.Num() == 0) continue;

			DoorPairs.Add(Key);

			int32 DoorCellIdx = SharedEdge.Num() / 2;
			FIntPoint DoorCell = SharedEdge[DoorCellIdx];

			FIntPoint NeighborCell(INDEX_NONE, INDEX_NONE);
			static const FIntPoint Dirs[] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
			FString WallDir;
			for (const FIntPoint& D : Dirs)
			{
				int32 NX = DoorCell.X + D.X, NY = DoorCell.Y + D.Y;
				if (NX >= 0 && NY >= 0 && NX < GridW && NY < GridH && Grid[NY][NX] == CorridorIdx)
				{
					NeighborCell = FIntPoint(NX, NY);
					if (D.X > 0) WallDir = TEXT("east");
					else if (D.X < 0) WallDir = TEXT("west");
					else if (D.Y > 0) WallDir = TEXT("south");
					else WallDir = TEXT("north");
					break;
				}
			}

			FDoorDef Door;
			Door.DoorId = FString::Printf(TEXT("door_%02d"), ++DoorCounter);
			Door.RoomA = Rooms[i].RoomId;
			Door.RoomB = TEXT("corridor");

			// WP-B: Convert room cells to wall boundary coordinates
			if (NeighborCell.X != INDEX_NONE)
			{
				if (DoorCell.X != NeighborCell.X)
				{
					int32 WallX = FMath::Max(DoorCell.X, NeighborCell.X);
					Door.EdgeStart = FIntPoint(WallX, DoorCell.Y);
					Door.EdgeEnd = FIntPoint(WallX, DoorCell.Y);
				}
				else
				{
					int32 WallY = FMath::Max(DoorCell.Y, NeighborCell.Y);
					Door.EdgeStart = FIntPoint(DoorCell.X, WallY);
					Door.EdgeEnd = FIntPoint(DoorCell.X, WallY);
				}
			}
			else
			{
				Door.EdgeStart = DoorCell;
				Door.EdgeEnd = DoorCell;
			}

			Door.Wall = WallDir;
			Door.Width = DoorWidth;
			Door.Height = 220.0f;

			Doors.Add(MoveTemp(Door));
		}
	}

	// For rooms that still have no door at all, add one to the closest neighbor
	TSet<int32> RoomsWithDoors;
	for (const FDoorDef& D : Doors)
	{
		for (int32 i = 0; i < Rooms.Num(); ++i)
		{
			if (Rooms[i].RoomId == D.RoomA || Rooms[i].RoomId == D.RoomB)
				RoomsWithDoors.Add(i);
		}
	}

	for (int32 i = 0; i < Rooms.Num(); ++i)
	{
		if (RoomsWithDoors.Contains(i)) continue;

		// Find any adjacent room and add a door
		static const FIntPoint Dirs[] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
		for (const FIntPoint& Cell : Rooms[i].GridCells)
		{
			bool bFoundDoor = false;
			for (const FIntPoint& D : Dirs)
			{
				int32 NX = Cell.X + D.X, NY = Cell.Y + D.Y;
				if (NX < 0 || NY < 0 || NX >= GridW || NY >= GridH) continue;
				int32 Neighbor = Grid[NY][NX];
				if (Neighbor >= 0 && Neighbor != i)
				{
					uint64 Key = PairKey(i, Neighbor);
					if (DoorPairs.Contains(Key)) { bFoundDoor = true; break; }

					DoorPairs.Add(Key);

					FString WallDir;
					if (D.X > 0) WallDir = TEXT("east");
					else if (D.X < 0) WallDir = TEXT("west");
					else if (D.Y > 0) WallDir = TEXT("south");
					else WallDir = TEXT("north");

					FDoorDef Door;
					Door.DoorId = FString::Printf(TEXT("door_%02d"), ++DoorCounter);
					Door.RoomA = Rooms[i].RoomId;
					Door.RoomB = Rooms[Neighbor].RoomId;

					// WP-B: Convert room cells to wall boundary coordinates
					if (Cell.X != NX)
					{
						int32 WallX = FMath::Max(Cell.X, NX);
						Door.EdgeStart = FIntPoint(WallX, Cell.Y);
						Door.EdgeEnd = FIntPoint(WallX, Cell.Y);
					}
					else
					{
						int32 WallY = FMath::Max(Cell.Y, NY);
						Door.EdgeStart = FIntPoint(Cell.X, WallY);
						Door.EdgeEnd = FIntPoint(Cell.X, WallY);
					}

					Door.Wall = WallDir;
					Door.Width = DoorWidth;
					Door.Height = 220.0f;
					Doors.Add(MoveTemp(Door));
					bFoundDoor = true;
					break;
				}
			}
			if (bFoundDoor) break;
		}
	}

	return Doors;
}

// ============================================================================
// Exterior Entrance Guarantee
// ============================================================================

static void EnsureExteriorEntrance(const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	const TArray<FRoomDef>& Rooms, TArray<FDoorDef>& Doors, bool bHospiceMode, FRandomStream& Rng)
{
	// 1. Check if any existing door already connects to exterior
	for (const FDoorDef& D : Doors)
	{
		if (D.RoomB == TEXT("exterior") || D.RoomA == TEXT("exterior"))
		{
			return;  // Already have an exterior entrance
		}
	}

	// 2. Find rooms that touch the building perimeter
	TArray<int32> PerimeterRoomIndices;
	for (int32 RoomIdx = 0; RoomIdx < Rooms.Num(); ++RoomIdx)
	{
		if (Rooms[RoomIdx].RoomType == TEXT("corridor")) continue;  // Prefer non-corridor rooms

		for (const FIntPoint& Cell : Rooms[RoomIdx].GridCells)
		{
			if (Cell.X == 0 || Cell.X == GridW - 1 || Cell.Y == 0 || Cell.Y == GridH - 1)
			{
				PerimeterRoomIndices.AddUnique(RoomIdx);
				break;
			}
		}
	}

	if (PerimeterRoomIndices.IsEmpty())
	{
		// Fallback: try corridors too
		for (int32 RoomIdx = 0; RoomIdx < Rooms.Num(); ++RoomIdx)
		{
			for (const FIntPoint& Cell : Rooms[RoomIdx].GridCells)
			{
				if (Cell.X == 0 || Cell.X == GridW - 1 || Cell.Y == 0 || Cell.Y == GridH - 1)
				{
					PerimeterRoomIndices.AddUnique(RoomIdx);
					break;
				}
			}
		}
	}

	if (PerimeterRoomIndices.IsEmpty())
	{
		UE_LOG(LogMonolithFloorPlan, Warning, TEXT("EnsureExteriorEntrance: no rooms touch building perimeter -- cannot place entrance"));
		return;
	}

	// 3. Prefer entryway, lobby, foyer, reception types
	int32 BestRoomIdx = -1;
	static const TArray<FString> PreferredTypes = { TEXT("entryway"), TEXT("lobby"), TEXT("foyer"), TEXT("reception") };
	for (int32 Idx : PerimeterRoomIndices)
	{
		if (PreferredTypes.Contains(Rooms[Idx].RoomType))
		{
			BestRoomIdx = Idx;
			break;
		}
	}

	// 4. Fallback: largest perimeter room
	if (BestRoomIdx < 0)
	{
		int32 MaxCells = 0;
		for (int32 Idx : PerimeterRoomIndices)
		{
			if (Rooms[Idx].GridCells.Num() > MaxCells)
			{
				MaxCells = Rooms[Idx].GridCells.Num();
				BestRoomIdx = Idx;
			}
		}
	}

	if (BestRoomIdx < 0) return;

	// 5. Find best perimeter cell -- prefer SOUTH edge (building fronts face south per project convention)
	//    Grid convention: Y==0 is south edge, Y==GridH-1 is north edge
	FIntPoint BestCell(-1, -1);
	FString BestWall;
	int32 BestPriority = 0;  // Higher = better. South=4, East=3, West=2, North=1

	for (const FIntPoint& Cell : Rooms[BestRoomIdx].GridCells)
	{
		// South edge (Y == 0) — highest priority
		if (Cell.Y == 0 && BestPriority < 4)
		{
			BestCell = Cell;
			BestWall = TEXT("south");
			BestPriority = 4;
		}
		// East edge (X == GridW - 1)
		if (Cell.X == GridW - 1 && BestPriority < 3)
		{
			BestCell = Cell;
			BestWall = TEXT("east");
			BestPriority = 3;
		}
		// West edge (X == 0)
		if (Cell.X == 0 && BestPriority < 2)
		{
			BestCell = Cell;
			BestWall = TEXT("west");
			BestPriority = 2;
		}
		// North edge (Y == GridH - 1) — lowest priority
		if (Cell.Y == GridH - 1 && BestPriority < 1)
		{
			BestCell = Cell;
			BestWall = TEXT("north");
			BestPriority = 1;
		}
	}

	if (BestCell.X < 0)
	{
		UE_LOG(LogMonolithFloorPlan, Warning, TEXT("EnsureExteriorEntrance: could not find perimeter cell on room '%s'"), *Rooms[BestRoomIdx].RoomId);
		return;
	}

	// 6. Try to find a run of perimeter cells on the same wall for a wider entrance placement
	//    Pick the midpoint of the run that includes BestCell
	TArray<FIntPoint> SameWallCells;
	for (const FIntPoint& Cell : Rooms[BestRoomIdx].GridCells)
	{
		bool bOnSameWall = false;
		if (BestWall == TEXT("south") && Cell.Y == 0) bOnSameWall = true;
		else if (BestWall == TEXT("north") && Cell.Y == GridH - 1) bOnSameWall = true;
		else if (BestWall == TEXT("east") && Cell.X == GridW - 1) bOnSameWall = true;
		else if (BestWall == TEXT("west") && Cell.X == 0) bOnSameWall = true;

		if (bOnSameWall) SameWallCells.Add(Cell);
	}

	// Sort by the axis parallel to the wall and pick the midpoint
	if (SameWallCells.Num() > 1)
	{
		if (BestWall == TEXT("south") || BestWall == TEXT("north"))
		{
			SameWallCells.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.X < B.X; });
		}
		else
		{
			SameWallCells.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.Y < B.Y; });
		}
		// Pick midpoint, but avoid cells where an interior wall crosses perpendicular
		// (a room boundary at this cell would block the entrance)
		int32 MidIdx = SameWallCells.Num() / 2;
		BestCell = SameWallCells[MidIdx];

		// Check for interior wall: does a different room ID exist on the perpendicular neighbor?
		auto HasPerpendicularWall = [&](const FIntPoint& Cell) -> bool
		{
			if (BestWall == TEXT("south") || BestWall == TEXT("north"))
			{
				// Wall runs along X. Check if cell above/below (inward) has a different room on left/right
				int32 InY = (BestWall == TEXT("south")) ? Cell.Y + 1 : Cell.Y - 1;
				if (InY >= 0 && InY < GridH && Cell.X > 0 && Cell.X < GridW)
				{
					int32 Left = Grid[InY][Cell.X - 1];
					int32 Right = Grid[InY][Cell.X];
					if (Left != Right && Left >= 0 && Right >= 0) return true;
				}
				if (InY >= 0 && InY < GridH && Cell.X + 1 < GridW)
				{
					int32 Left = Grid[InY][Cell.X];
					int32 Right = Grid[InY][Cell.X + 1];
					if (Left != Right && Left >= 0 && Right >= 0) return true;
				}
			}
			else
			{
				// Wall runs along Y. Check if cell left/right (inward) has different room above/below
				int32 InX = (BestWall == TEXT("west")) ? Cell.X + 1 : Cell.X - 1;
				if (InX >= 0 && InX < GridW && Cell.Y > 0 && Cell.Y < GridH)
				{
					int32 Top = Grid[Cell.Y - 1][InX];
					int32 Bot = Grid[Cell.Y][InX];
					if (Top != Bot && Top >= 0 && Bot >= 0) return true;
				}
				if (InX >= 0 && InX < GridW && Cell.Y + 1 < GridH)
				{
					int32 Top = Grid[Cell.Y][InX];
					int32 Bot = Grid[Cell.Y + 1][InX];
					if (Top != Bot && Top >= 0 && Bot >= 0) return true;
				}
			}
			return false;
		};

		// If midpoint has a perpendicular wall, search outward for a clear cell
		if (HasPerpendicularWall(BestCell))
		{
			bool bFound = false;
			for (int32 Offset = 1; Offset < SameWallCells.Num(); ++Offset)
			{
				int32 LeftIdx = MidIdx - Offset;
				int32 RightIdx = MidIdx + Offset;
				if (LeftIdx >= 0 && !HasPerpendicularWall(SameWallCells[LeftIdx]))
				{
					BestCell = SameWallCells[LeftIdx];
					bFound = true;
					break;
				}
				if (RightIdx < SameWallCells.Num() && !HasPerpendicularWall(SameWallCells[RightIdx]))
				{
					BestCell = SameWallCells[RightIdx];
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				UE_LOG(LogMonolithFloorPlan, Warning, TEXT("EnsureExteriorEntrance: all perimeter cells have perpendicular walls -- using midpoint anyway"));
			}
		}
	}

	// 7. Create exterior entrance door
	// WP-B: Compute wall boundary coordinates for exterior entrance.
	// For exterior walls, the boundary is at the grid edge:
	//   South (Y==0):       boundary at Y=0 (wall is at min Y edge)
	//   North (Y==GridH-1): boundary at Y=GridH (wall is at max Y edge, one past grid)
	//   West  (X==0):       boundary at X=0
	//   East  (X==GridW-1): boundary at X=GridW
	FIntPoint EdgePt = BestCell;
	if (BestWall == TEXT("south"))
	{
		EdgePt = FIntPoint(BestCell.X, 0);
	}
	else if (BestWall == TEXT("north"))
	{
		EdgePt = FIntPoint(BestCell.X, GridH);
	}
	else if (BestWall == TEXT("west"))
	{
		EdgePt = FIntPoint(0, BestCell.Y);
	}
	else if (BestWall == TEXT("east"))
	{
		EdgePt = FIntPoint(GridW, BestCell.Y);
	}

	FDoorDef EntDoor;
	EntDoor.DoorId = TEXT("entrance_01");
	EntDoor.RoomA = Rooms[BestRoomIdx].RoomId;
	EntDoor.RoomB = TEXT("exterior");
	EntDoor.EdgeStart = EdgePt;
	EntDoor.EdgeEnd = EdgePt;
	EntDoor.Wall = BestWall;
	EntDoor.Width = bHospiceMode ? 120.0f : 110.0f;   // Exterior entrance matches interior width minimum
	EntDoor.Height = 240.0f;   // Taller exterior entrance
	EntDoor.bTraversable = true;

	Doors.Add(MoveTemp(EntDoor));

	UE_LOG(LogMonolithFloorPlan, Log, TEXT("Placed exterior entrance: room='%s', wall=%s, cell=(%d,%d)"),
		*Rooms[BestRoomIdx].RoomId, *BestWall, BestCell.X, BestCell.Y);
}

// ============================================================================
// Door Clearance Validation
// ============================================================================

static void ValidateDoorClearances(const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	const TArray<FRoomDef>& Rooms, TArray<FDoorDef>& Doors, float CellSize)
{
	// Player capsule: 42cm radius, 84cm diameter (Leviathan / Game Animation Sample character)
	constexpr float CapsuleDiameter = 84.0f;
	constexpr float MinClearanceBuffer = 26.0f;  // 13cm per side for comfortable diagonal approach
	const float MinDoorWidth = CapsuleDiameter + MinClearanceBuffer;  // 110cm
	const float MinDepthCells = 2.0f;  // At least 2 cells (100cm) depth adjacent to door for passage

	int32 WarningCount = 0;

	for (FDoorDef& Door : Doors)
	{
		// 1. Width check -- auto-widen if below minimum
		if (Door.Width < MinDoorWidth && Door.bTraversable)
		{
			UE_LOG(LogMonolithFloorPlan, Warning,
				TEXT("Door '%s' width %.0fcm < minimum %.0fcm for player capsule (84cm diameter + 26cm buffer). Widening."),
				*Door.DoorId, Door.Width, MinDoorWidth);
			Door.Width = MinDoorWidth;
			++WarningCount;
		}

		// 2. Check rooms on both sides have enough depth adjacent to the door
		//    "Depth" = cells in the direction perpendicular to the door wall
		if (Door.RoomB == TEXT("exterior")) continue;  // Skip exterior doors for depth check

		auto CheckRoomDepthAtDoor = [&](const FString& RoomId, const FIntPoint& DoorEdge) -> bool
		{
			// Find the room
			const FRoomDef* Room = nullptr;
			for (const FRoomDef& R : Rooms)
			{
				if (R.RoomId == RoomId)
				{
					Room = &R;
					break;
				}
			}
			if (!Room) return true;  // Can't validate, assume OK

			// Count cells adjacent to door in perpendicular direction
			int32 DepthCount = 0;
			if (Door.Wall == TEXT("north") || Door.Wall == TEXT("south"))
			{
				// Door on N/S wall -- check depth in Y direction from door cell
				int32 DY = (Door.Wall == TEXT("south")) ? 1 : -1;
				for (int32 Step = 0; Step < GridH; ++Step)
				{
					int32 CheckY = DoorEdge.Y + DY * Step;
					if (CheckY < 0 || CheckY >= GridH) break;
					if (Grid[CheckY][DoorEdge.X] < 0) break;

					// Check this cell belongs to the room
					bool bInRoom = false;
					for (const FIntPoint& C : Room->GridCells)
					{
						if (C.X == DoorEdge.X && C.Y == CheckY) { bInRoom = true; break; }
					}
					if (!bInRoom) break;
					++DepthCount;
				}
			}
			else
			{
				// Door on E/W wall -- check depth in X direction
				int32 DX = (Door.Wall == TEXT("west")) ? 1 : -1;
				for (int32 Step = 0; Step < GridW; ++Step)
				{
					int32 CheckX = DoorEdge.X + DX * Step;
					if (CheckX < 0 || CheckX >= GridW) break;
					if (Grid[DoorEdge.Y][CheckX] < 0) break;

					bool bInRoom = false;
					for (const FIntPoint& C : Room->GridCells)
					{
						if (C.X == CheckX && C.Y == DoorEdge.Y) { bInRoom = true; break; }
					}
					if (!bInRoom) break;
					++DepthCount;
				}
			}

			return DepthCount >= static_cast<int32>(MinDepthCells);
		};

		if (!CheckRoomDepthAtDoor(Door.RoomA, Door.EdgeStart))
		{
			UE_LOG(LogMonolithFloorPlan, Warning,
				TEXT("Door '%s': room '%s' has insufficient depth (< %.0fcm) adjacent to door for player passage"),
				*Door.DoorId, *Door.RoomA, MinDepthCells * CellSize);
			++WarningCount;
		}

		if (!CheckRoomDepthAtDoor(Door.RoomB, Door.EdgeEnd))
		{
			UE_LOG(LogMonolithFloorPlan, Warning,
				TEXT("Door '%s': room '%s' has insufficient depth (< %.0fcm) adjacent to door for player passage"),
				*Door.DoorId, *Door.RoomB, MinDepthCells * CellSize);
			++WarningCount;
		}
	}

	if (WarningCount > 0)
	{
		UE_LOG(LogMonolithFloorPlan, Warning, TEXT("Door clearance validation: %d warning(s) across %d doors"), WarningCount, Doors.Num());
	}
	else
	{
		UE_LOG(LogMonolithFloorPlan, Log, TEXT("Door clearance validation: all %d doors passed"), Doors.Num());
	}
}

// ============================================================================
// Output JSON
// ============================================================================

TSharedPtr<FJsonObject> FMonolithMeshFloorPlanGenerator::BuildOutputJson(
	const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
	const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
	const FString& ArchetypeName, float FootprintWidth, float FootprintHeight,
	bool bHospiceMode, float CellSize)
{
	auto Result = MakeShared<FJsonObject>();

	// Grid as 2D array
	TArray<TSharedPtr<FJsonValue>> GridArr;
	for (int32 Y = 0; Y < GridH; ++Y)
	{
		TArray<TSharedPtr<FJsonValue>> RowArr;
		for (int32 X = 0; X < GridW; ++X)
		{
			RowArr.Add(MakeShared<FJsonValueNumber>(Grid[Y][X]));
		}
		GridArr.Add(MakeShared<FJsonValueArray>(RowArr));
	}
	Result->SetArrayField(TEXT("grid"), GridArr);

	// Rooms
	TArray<TSharedPtr<FJsonValue>> RoomArr;
	for (const FRoomDef& R : Rooms)
	{
		RoomArr.Add(MakeShared<FJsonValueObject>(R.ToJson()));
	}
	Result->SetArrayField(TEXT("rooms"), RoomArr);

	// Doors
	TArray<TSharedPtr<FJsonValue>> DoorArr;
	for (const FDoorDef& D : Doors)
	{
		auto DJ = MakeShared<FJsonObject>();
		DJ->SetStringField(TEXT("door_id"), D.DoorId);
		DJ->SetStringField(TEXT("room_a"), D.RoomA);
		DJ->SetStringField(TEXT("room_b"), D.RoomB);

		TArray<TSharedPtr<FJsonValue>> EdgeStart, EdgeEnd;
		EdgeStart.Add(MakeShared<FJsonValueNumber>(D.EdgeStart.X));
		EdgeStart.Add(MakeShared<FJsonValueNumber>(D.EdgeStart.Y));
		EdgeEnd.Add(MakeShared<FJsonValueNumber>(D.EdgeEnd.X));
		EdgeEnd.Add(MakeShared<FJsonValueNumber>(D.EdgeEnd.Y));
		DJ->SetArrayField(TEXT("edge_start"), EdgeStart);
		DJ->SetArrayField(TEXT("edge_end"), EdgeEnd);

		DJ->SetStringField(TEXT("wall"), D.Wall);
		DJ->SetNumberField(TEXT("width"), D.Width);
		DJ->SetNumberField(TEXT("height"), D.Height);
		DoorArr.Add(MakeShared<FJsonValueObject>(DJ));
	}
	Result->SetArrayField(TEXT("doors"), DoorArr);

	// Metadata
	Result->SetStringField(TEXT("archetype"), ArchetypeName);

	auto Footprint = MakeShared<FJsonObject>();
	Footprint->SetNumberField(TEXT("width"), FootprintWidth);
	Footprint->SetNumberField(TEXT("height"), FootprintHeight);
	Result->SetObjectField(TEXT("footprint"), Footprint);

	Result->SetBoolField(TEXT("hospice_mode"), bHospiceMode);
	Result->SetNumberField(TEXT("cell_size"), CellSize);

	// Stats
	int32 CorridorCells = 0;
	for (const FRoomDef& R : Rooms)
	{
		if (R.RoomType == TEXT("corridor"))
			CorridorCells += R.GridCells.Num();
	}

	auto Stats = MakeShared<FJsonObject>();
	Stats->SetNumberField(TEXT("room_count"), Rooms.Num());
	Stats->SetNumberField(TEXT("door_count"), Doors.Num());
	Stats->SetNumberField(TEXT("corridor_cells"), CorridorCells);
	Stats->SetNumberField(TEXT("grid_width"), GridW);
	Stats->SetNumberField(TEXT("grid_height"), GridH);
	Stats->SetNumberField(TEXT("total_cells"), GridW * GridH);

	int32 UsedCells = 0;
	for (int32 Y = 0; Y < GridH; ++Y)
		for (int32 X = 0; X < GridW; ++X)
			if (Grid[Y][X] >= 0) ++UsedCells;
	Stats->SetNumberField(TEXT("used_cells"), UsedCells);
	Stats->SetNumberField(TEXT("fill_ratio"), (GridW * GridH > 0) ? static_cast<float>(UsedCells) / (GridW * GridH) : 0.0f);

	Result->SetObjectField(TEXT("stats"), Stats);

	return Result;
}

// ============================================================================
// Template System (WP-A)
// ============================================================================

FString FMonolithMeshFloorPlanGenerator::GetTemplateDirectory()
{
	return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("Monolith"), TEXT("Saved"), TEXT("Monolith"), TEXT("FloorPlanTemplates"));
}

FString FMonolithMeshFloorPlanGenerator::ArchetypeToTemplateCategory(const FString& ArchetypeName)
{
	// Map archetype names to template categories
	FString Lower = ArchetypeName.ToLower();

	if (Lower.Contains(TEXT("residential")) || Lower.Contains(TEXT("house")) || Lower.Contains(TEXT("home"))
		|| Lower.Contains(TEXT("cabin")) || Lower.Contains(TEXT("bungalow")) || Lower.Contains(TEXT("ranch"))
		|| Lower.Contains(TEXT("colonial")) || Lower.Contains(TEXT("farmhouse")) || Lower.Contains(TEXT("duplex"))
		|| Lower.Contains(TEXT("townhouse")) || Lower.Contains(TEXT("apartment")) || Lower.Contains(TEXT("cottage")))
	{
		return TEXT("residential");
	}

	if (Lower.Contains(TEXT("horror")) || Lower.Contains(TEXT("abandoned")) || Lower.Contains(TEXT("haunted")))
	{
		return TEXT("horror");
	}

	// Everything else: office, warehouse, clinic, police, church, restaurant, school, shop, garage, store
	return TEXT("commercial");
}

bool FMonolithMeshFloorPlanGenerator::LoadFloorPlanTemplate(
	const FString& TemplateName, const FString& Category,
	TArray<TArray<int32>>& OutGrid, int32& OutGridW, int32& OutGridH,
	TArray<FRoomDef>& OutRooms, TArray<FDoorDef>& OutDoors,
	TArray<FStairwellDef>& OutStairwells, FString& OutError)
{
	// Build file path: {TemplateDir}/{category}/{name}.json
	FString FilePath = FPaths::Combine(GetTemplateDirectory(), Category, TemplateName + TEXT(".json"));

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		OutError = FString::Printf(TEXT("Could not load template file: %s"), *FilePath);
		return false;
	}

	TSharedPtr<FJsonObject> Json = FMonolithJsonUtils::Parse(JsonString);
	if (!Json.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to parse template JSON: %s"), *FilePath);
		return false;
	}

	// Parse floors array — use first floor for now
	const TArray<TSharedPtr<FJsonValue>>* FloorsArr = nullptr;
	if (!Json->TryGetArrayField(TEXT("floors"), FloorsArr) || FloorsArr->Num() == 0)
	{
		OutError = FString::Printf(TEXT("Template '%s' has no floors array"), *TemplateName);
		return false;
	}

	// Get template-level grid dimensions
	OutGridW = static_cast<int32>(Json->GetNumberField(TEXT("grid_width")));
	OutGridH = static_cast<int32>(Json->GetNumberField(TEXT("grid_height")));

	if (OutGridW <= 0 || OutGridH <= 0)
	{
		OutError = FString::Printf(TEXT("Template '%s' has invalid grid dimensions %dx%d"), *TemplateName, OutGridW, OutGridH);
		return false;
	}

	// Parse floor 0
	const TSharedPtr<FJsonObject>& FloorObj = (*FloorsArr)[0]->AsObject();
	if (!FloorObj.IsValid())
	{
		OutError = FString::Printf(TEXT("Template '%s' floor 0 is not a valid JSON object"), *TemplateName);
		return false;
	}

	// Parse grid
	const TArray<TSharedPtr<FJsonValue>>* GridArr = nullptr;
	if (!FloorObj->TryGetArrayField(TEXT("grid"), GridArr) || GridArr->Num() == 0)
	{
		OutError = FString::Printf(TEXT("Template '%s' floor 0 has no grid array"), *TemplateName);
		return false;
	}

	OutGrid.SetNum(OutGridH);
	for (int32 Y = 0; Y < OutGridH && Y < GridArr->Num(); ++Y)
	{
		const TArray<TSharedPtr<FJsonValue>>& RowArr = (*GridArr)[Y]->AsArray();
		OutGrid[Y].SetNum(OutGridW);
		for (int32 X = 0; X < OutGridW && X < RowArr.Num(); ++X)
		{
			OutGrid[Y][X] = static_cast<int32>(RowArr[X]->AsNumber());
		}
	}

	// Parse rooms — get room_id and room_type from JSON, but reconstruct GridCells from grid (R1-C1)
	const TArray<TSharedPtr<FJsonValue>>* RoomsArr = nullptr;
	if (FloorObj->TryGetArrayField(TEXT("rooms"), RoomsArr))
	{
		// First pass: load room metadata from JSON
		TMap<int32, int32> RoomIndexToArrayIndex;  // room_index_in_grid -> index in OutRooms
		for (int32 i = 0; i < RoomsArr->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>& RoomObj = (*RoomsArr)[i]->AsObject();
			if (!RoomObj.IsValid()) continue;

			FRoomDef Room;
			RoomObj->TryGetStringField(TEXT("room_id"), Room.RoomId);
			RoomObj->TryGetStringField(TEXT("room_type"), Room.RoomType);
			// GridCells will be reconstructed from grid scan below

			OutRooms.Add(MoveTemp(Room));
			RoomIndexToArrayIndex.Add(i, i);
		}

		// Second pass: reconstruct GridCells by scanning the grid (R1-C1: grid is the source of truth)
		for (int32 Y = 0; Y < OutGridH; ++Y)
		{
			for (int32 X = 0; X < OutGridW; ++X)
			{
				int32 CellVal = OutGrid[Y][X];
				if (CellVal >= 0 && CellVal < OutRooms.Num())
				{
					OutRooms[CellVal].GridCells.Add(FIntPoint(X, Y));
				}
			}
		}
	}

	// Parse doors
	const TArray<TSharedPtr<FJsonValue>>* DoorsArr = nullptr;
	if (FloorObj->TryGetArrayField(TEXT("doors"), DoorsArr))
	{
		for (int32 i = 0; i < DoorsArr->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>& DoorObj = (*DoorsArr)[i]->AsObject();
			if (!DoorObj.IsValid()) continue;

			FDoorDef Door;
			DoorObj->TryGetStringField(TEXT("door_id"), Door.DoorId);
			DoorObj->TryGetStringField(TEXT("room_a"), Door.RoomA);
			DoorObj->TryGetStringField(TEXT("room_b"), Door.RoomB);

			// Normalize EXTERIOR -> exterior for consistency with EnsureExteriorEntrance
			if (Door.RoomA.ToUpper() == TEXT("EXTERIOR")) Door.RoomA = TEXT("exterior");
			if (Door.RoomB.ToUpper() == TEXT("EXTERIOR")) Door.RoomB = TEXT("exterior");

			// Parse edge_start
			const TArray<TSharedPtr<FJsonValue>>* EdgeStartArr = nullptr;
			if (DoorObj->TryGetArrayField(TEXT("edge_start"), EdgeStartArr) && EdgeStartArr->Num() >= 2)
			{
				Door.EdgeStart.X = static_cast<int32>((*EdgeStartArr)[0]->AsNumber());
				Door.EdgeStart.Y = static_cast<int32>((*EdgeStartArr)[1]->AsNumber());
			}

			// Parse edge_end
			const TArray<TSharedPtr<FJsonValue>>* EdgeEndArr = nullptr;
			if (DoorObj->TryGetArrayField(TEXT("edge_end"), EdgeEndArr) && EdgeEndArr->Num() >= 2)
			{
				Door.EdgeEnd.X = static_cast<int32>((*EdgeEndArr)[0]->AsNumber());
				Door.EdgeEnd.Y = static_cast<int32>((*EdgeEndArr)[1]->AsNumber());
			}

			DoorObj->TryGetStringField(TEXT("wall"), Door.Wall);

			double WidthDbl = 110.0;
			if (DoorObj->TryGetNumberField(TEXT("width"), WidthDbl))
				Door.Width = static_cast<float>(WidthDbl);
			double HeightDbl = 220.0;
			if (DoorObj->TryGetNumberField(TEXT("height"), HeightDbl))
				Door.Height = static_cast<float>(HeightDbl);

			// Validate door edge coordinates (R1-C1): must be axis-aligned
			if (Door.EdgeStart.X != Door.EdgeEnd.X && Door.EdgeStart.Y != Door.EdgeEnd.Y)
			{
				UE_LOG(LogMonolithFloorPlan, Warning,
					TEXT("Template '%s' door '%s' has non-axis-aligned edge: (%d,%d)->(%d,%d) — correcting to EdgeStart"),
					*TemplateName, *Door.DoorId,
					Door.EdgeStart.X, Door.EdgeStart.Y, Door.EdgeEnd.X, Door.EdgeEnd.Y);
				Door.EdgeEnd = Door.EdgeStart;
			}

			OutDoors.Add(MoveTemp(Door));
		}
	}

	// Parse stairwells
	const TArray<TSharedPtr<FJsonValue>>* StairArr = nullptr;
	if (FloorObj->TryGetArrayField(TEXT("stairwells"), StairArr))
	{
		for (int32 i = 0; i < StairArr->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>& StairObj = (*StairArr)[i]->AsObject();
			if (!StairObj.IsValid()) continue;

			FStairwellDef Stair;
			StairObj->TryGetStringField(TEXT("stairwell_id"), Stair.StairwellId);

			const TArray<TSharedPtr<FJsonValue>>* CellsArr = nullptr;
			if (StairObj->TryGetArrayField(TEXT("grid_cells"), CellsArr))
			{
				for (const auto& CellVal : *CellsArr)
				{
					const TArray<TSharedPtr<FJsonValue>>& Pair = CellVal->AsArray();
					if (Pair.Num() >= 2)
					{
						Stair.GridCells.Add(FIntPoint(
							static_cast<int32>(Pair[0]->AsNumber()),
							static_cast<int32>(Pair[1]->AsNumber())));
					}
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* FloorsConnArr = nullptr;
			if (StairObj->TryGetArrayField(TEXT("connects_floors"), FloorsConnArr) && FloorsConnArr->Num() >= 2)
			{
				Stair.ConnectsFloorA = static_cast<int32>((*FloorsConnArr)[0]->AsNumber());
				Stair.ConnectsFloorB = static_cast<int32>((*FloorsConnArr)[1]->AsNumber());
			}

			FString AccessStr;
			if (StairObj->TryGetStringField(TEXT("vertical_access"), AccessStr))
			{
				if (AccessStr == TEXT("elevator")) Stair.VerticalAccess = EVerticalAccessType::Elevator;
				else if (AccessStr == TEXT("both")) Stair.VerticalAccess = EVerticalAccessType::Both;
			}

			OutStairwells.Add(MoveTemp(Stair));
		}
	}

	UE_LOG(LogMonolithFloorPlan, Log, TEXT("Loaded template '%s/%s': grid=%dx%d, %d rooms, %d doors, %d stairwells"),
		*Category, *TemplateName, OutGridW, OutGridH, OutRooms.Num(), OutDoors.Num(), OutStairwells.Num());

	return true;
}

FString FMonolithMeshFloorPlanGenerator::SelectTemplate(
	const FString& Category, float FootprintW, float FootprintH,
	FRandomStream& Rng, FString& OutError)
{
	FString TemplateDir = FPaths::Combine(GetTemplateDirectory(), Category);

	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(TemplateDir, TEXT("*.json")), true, false);

	if (Files.Num() == 0)
	{
		OutError = FString::Printf(TEXT("No templates found in category '%s' (dir: %s)"), *Category, *TemplateDir);
		return FString();
	}

	// Score each template by footprint compatibility
	struct FTemplateCandidate
	{
		FString Name;
		float Score;  // Lower = better fit
	};

	TArray<FTemplateCandidate> Candidates;

	for (const FString& File : Files)
	{
		FString Name = FPaths::GetBaseFilename(File);
		FString FullPath = FPaths::Combine(TemplateDir, File);

		// Load just the metadata (don't parse full grid)
		FString JsonStr;
		if (!FFileHelper::LoadFileToString(JsonStr, *FullPath)) continue;

		TSharedPtr<FJsonObject> Json = FMonolithJsonUtils::Parse(JsonStr);
		if (!Json.IsValid()) continue;

		double TemplateW = 0, TemplateH = 0;
		Json->TryGetNumberField(TEXT("footprint_width"), TemplateW);
		Json->TryGetNumberField(TEXT("footprint_height"), TemplateH);

		if (TemplateW <= 0 || TemplateH <= 0) continue;

		// Check 50% scale tolerance: template footprint must be within [0.5x, 2.0x] of requested
		float ScaleX = FootprintW / static_cast<float>(TemplateW);
		float ScaleY = FootprintH / static_cast<float>(TemplateH);

		if (ScaleX < 0.5f || ScaleX > 2.0f || ScaleY < 0.5f || ScaleY > 2.0f)
			continue;

		// Score: how close is the aspect ratio match? (1.0 = perfect)
		float TemplateRatio = static_cast<float>(TemplateW / TemplateH);
		float RequestedRatio = FootprintW / FootprintH;
		float RatioScore = FMath::Abs(TemplateRatio - RequestedRatio) / FMath::Max(TemplateRatio, RequestedRatio);

		// Also factor in scale proximity (prefer templates that need less scaling)
		float ScaleScore = FMath::Abs(ScaleX - 1.0f) + FMath::Abs(ScaleY - 1.0f);

		float TotalScore = RatioScore * 2.0f + ScaleScore;

		Candidates.Add({ Name, TotalScore });
	}

	if (Candidates.Num() == 0)
	{
		OutError = FString::Printf(TEXT("No templates in category '%s' fit footprint %.0fx%.0f (within 50%% scale tolerance)"),
			*Category, FootprintW, FootprintH);
		return FString();
	}

	// Sort by score (best first)
	Candidates.Sort([](const FTemplateCandidate& A, const FTemplateCandidate& B) { return A.Score < B.Score; });

	// Weighted random from top 3 (or fewer)
	int32 PoolSize = FMath::Min(3, Candidates.Num());

	// Weight: inverse of score (better = higher weight). Use softmax-ish weighting.
	TArray<float> Weights;
	float TotalWeight = 0.0f;
	for (int32 i = 0; i < PoolSize; ++i)
	{
		float W = 1.0f / (1.0f + Candidates[i].Score * 10.0f);
		Weights.Add(W);
		TotalWeight += W;
	}

	float Roll = Rng.FRand() * TotalWeight;
	float Accum = 0.0f;
	for (int32 i = 0; i < PoolSize; ++i)
	{
		Accum += Weights[i];
		if (Roll <= Accum)
		{
			UE_LOG(LogMonolithFloorPlan, Log, TEXT("Selected template '%s' from category '%s' (score=%.3f, pool=%d)"),
				*Candidates[i].Name, *Category, Candidates[i].Score, Candidates.Num());
			return Candidates[i].Name;
		}
	}

	// Fallback to best
	return Candidates[0].Name;
}

bool FMonolithMeshFloorPlanGenerator::ScaleTemplateGrid(
	TArray<TArray<int32>>& InOutGrid, int32& InOutGridW, int32& InOutGridH,
	TArray<FRoomDef>& InOutRooms, TArray<FDoorDef>& InOutDoors,
	TArray<FStairwellDef>& InOutStairwells,
	int32 TargetGridW, int32 TargetGridH,
	FString& OutError)
{
	int32 OldW = InOutGridW;
	int32 OldH = InOutGridH;

	if (TargetGridW <= 0 || TargetGridH <= 0)
	{
		OutError = TEXT("Invalid target grid dimensions for scaling");
		return false;
	}

	// If no scaling needed, just return
	if (TargetGridW == OldW && TargetGridH == OldH)
		return true;

	float ScaleX = static_cast<float>(TargetGridW) / static_cast<float>(OldW);
	float ScaleY = static_cast<float>(TargetGridH) / static_cast<float>(OldH);

	// Build new grid using nearest-neighbor sampling
	TArray<TArray<int32>> NewGrid;
	NewGrid.SetNum(TargetGridH);
	for (int32 Y = 0; Y < TargetGridH; ++Y)
	{
		NewGrid[Y].SetNum(TargetGridW);
		int32 SrcY = FMath::Clamp(FMath::RoundToInt32(static_cast<float>(Y) / ScaleY), 0, OldH - 1);
		for (int32 X = 0; X < TargetGridW; ++X)
		{
			int32 SrcX = FMath::Clamp(FMath::RoundToInt32(static_cast<float>(X) / ScaleX), 0, OldW - 1);
			NewGrid[Y][X] = InOutGrid[SrcY][SrcX];
		}
	}

	// Reconstruct room GridCells from the new grid
	for (FRoomDef& Room : InOutRooms)
	{
		Room.GridCells.Empty();
	}
	for (int32 Y = 0; Y < TargetGridH; ++Y)
	{
		for (int32 X = 0; X < TargetGridW; ++X)
		{
			int32 CellVal = NewGrid[Y][X];
			if (CellVal >= 0 && CellVal < InOutRooms.Num())
			{
				InOutRooms[CellVal].GridCells.Add(FIntPoint(X, Y));
			}
		}
	}

	// Scale door positions
	for (FDoorDef& Door : InOutDoors)
	{
		Door.EdgeStart.X = FMath::Clamp(FMath::RoundToInt32(Door.EdgeStart.X * ScaleX), 0, TargetGridW - 1);
		Door.EdgeStart.Y = FMath::Clamp(FMath::RoundToInt32(Door.EdgeStart.Y * ScaleY), 0, TargetGridH - 1);
		Door.EdgeEnd.X = FMath::Clamp(FMath::RoundToInt32(Door.EdgeEnd.X * ScaleX), 0, TargetGridW - 1);
		Door.EdgeEnd.Y = FMath::Clamp(FMath::RoundToInt32(Door.EdgeEnd.Y * ScaleY), 0, TargetGridH - 1);

		// Re-enforce axis alignment after scaling
		if (Door.EdgeStart.X != Door.EdgeEnd.X && Door.EdgeStart.Y != Door.EdgeEnd.Y)
		{
			// Snap to the dominant axis
			if (FMath::Abs(Door.EdgeEnd.X - Door.EdgeStart.X) >= FMath::Abs(Door.EdgeEnd.Y - Door.EdgeStart.Y))
				Door.EdgeEnd.Y = Door.EdgeStart.Y;  // Horizontal door
			else
				Door.EdgeEnd.X = Door.EdgeStart.X;  // Vertical door
		}
	}

	// Scale stairwell positions
	for (FStairwellDef& Stair : InOutStairwells)
	{
		TArray<FIntPoint> NewCells;
		for (const FIntPoint& Cell : Stair.GridCells)
		{
			int32 NX = FMath::Clamp(FMath::RoundToInt32(Cell.X * ScaleX), 0, TargetGridW - 1);
			int32 NY = FMath::Clamp(FMath::RoundToInt32(Cell.Y * ScaleY), 0, TargetGridH - 1);
			NewCells.AddUnique(FIntPoint(NX, NY));
		}
		Stair.GridCells = MoveTemp(NewCells);
	}

	// Apply scaled grid
	InOutGrid = MoveTemp(NewGrid);
	InOutGridW = TargetGridW;
	InOutGridH = TargetGridH;

	// ---- Post-scaling validation (R2-I1, R2-I2, R2-I3) ----

	// R2-I1: Corridors still >= 3 cells wide
	for (int32 i = 0; i < InOutRooms.Num(); ++i)
	{
		if (InOutRooms[i].RoomType != TEXT("corridor") && InOutRooms[i].RoomType != TEXT("hallway"))
			continue;

		if (InOutRooms[i].GridCells.Num() == 0) continue;

		// Compute bounding box width/height
		int32 MinX = INT_MAX, MaxX = INT_MIN, MinY = INT_MAX, MaxY = INT_MIN;
		for (const FIntPoint& C : InOutRooms[i].GridCells)
		{
			MinX = FMath::Min(MinX, C.X); MaxX = FMath::Max(MaxX, C.X);
			MinY = FMath::Min(MinY, C.Y); MaxY = FMath::Max(MaxY, C.Y);
		}
		int32 BoundsW = MaxX - MinX + 1;
		int32 BoundsH = MaxY - MinY + 1;
		int32 MinDim = FMath::Min(BoundsW, BoundsH);

		if (MinDim < 3)
		{
			UE_LOG(LogMonolithFloorPlan, Warning, TEXT("Post-scale: corridor '%s' narrowed to %d cells (min 3). Template may produce narrow passages."),
				*InOutRooms[i].RoomId, MinDim);
		}
	}

	// R2-I2: Stairwells >= 4x6
	for (const FStairwellDef& Stair : InOutStairwells)
	{
		if (Stair.GridCells.Num() == 0) continue;

		int32 MinX = INT_MAX, MaxX = INT_MIN, MinY = INT_MAX, MaxY = INT_MIN;
		for (const FIntPoint& C : Stair.GridCells)
		{
			MinX = FMath::Min(MinX, C.X); MaxX = FMath::Max(MaxX, C.X);
			MinY = FMath::Min(MinY, C.Y); MaxY = FMath::Max(MaxY, C.Y);
		}
		int32 BoundsW = MaxX - MinX + 1;
		int32 BoundsH = MaxY - MinY + 1;
		int32 MinDim = FMath::Min(BoundsW, BoundsH);
		int32 MaxDim = FMath::Max(BoundsW, BoundsH);

		if (MinDim < 4 || MaxDim < 6)
		{
			OutError = FString::Printf(TEXT("Post-scale: stairwell '%s' shrank to %dx%d (min 4x6). Cannot use scaled template; use original size."),
				*Stair.StairwellId, BoundsW, BoundsH);
			return false;
		}
	}

	// R2-I3: Entrance cell still on exterior edge
	// We check if any room has cells on the perimeter — if the template had an entrance it should still be reachable
	bool bHasPerimeterRoom = false;
	for (const FRoomDef& Room : InOutRooms)
	{
		for (const FIntPoint& C : Room.GridCells)
		{
			if (C.X == 0 || C.X == TargetGridW - 1 || C.Y == 0 || C.Y == TargetGridH - 1)
			{
				bHasPerimeterRoom = true;
				break;
			}
		}
		if (bHasPerimeterRoom) break;
	}
	if (!bHasPerimeterRoom)
	{
		UE_LOG(LogMonolithFloorPlan, Warning, TEXT("Post-scale: no rooms touch exterior perimeter. Entrance may need re-placement."));
	}

	UE_LOG(LogMonolithFloorPlan, Log, TEXT("Scaled template grid from %dx%d to %dx%d (scale %.2fx%.2f)"),
		OldW, OldH, TargetGridW, TargetGridH, ScaleX, ScaleY);

	return true;
}

// ============================================================================
// Action handlers
// ============================================================================

FMonolithActionResult FMonolithMeshFloorPlanGenerator::GenerateFloorPlan(const TSharedPtr<FJsonObject>& Params)
{
	// ---- Parse params ----
	FString ArchetypeName;
	if (!Params->TryGetStringField(TEXT("archetype"), ArchetypeName) || ArchetypeName.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required 'archetype' parameter"));

	double FootprintW = 0, FootprintH = 0;
	if (!Params->TryGetNumberField(TEXT("footprint_width"), FootprintW) || FootprintW <= 0)
		return FMonolithActionResult::Error(TEXT("Missing or invalid 'footprint_width' (must be > 0)"));
	if (!Params->TryGetNumberField(TEXT("footprint_height"), FootprintH) || FootprintH <= 0)
		return FMonolithActionResult::Error(TEXT("Missing or invalid 'footprint_height' (must be > 0)"));

	double CellSizeDbl = 50.0;
	Params->TryGetNumberField(TEXT("cell_size"), CellSizeDbl);
	float CellSize = static_cast<float>(FMath::Max(10.0, CellSizeDbl));

	double SeedDbl = -1.0;
	Params->TryGetNumberField(TEXT("seed"), SeedDbl);
	int32 Seed = static_cast<int32>(SeedDbl);
	if (Seed < 0)
		Seed = FMath::Rand();

	bool bHospiceMode = false;
	Params->TryGetBoolField(TEXT("hospice_mode"), bHospiceMode);

	double MinAspectDbl = 3.0;
	Params->TryGetNumberField(TEXT("min_room_aspect"), MinAspectDbl);

	// ---- WP-6: Parse horror_level ----
	double HorrorLevelDbl = 0.0;
	Params->TryGetNumberField(TEXT("horror_level"), HorrorLevelDbl);
	float HorrorLevel = FMath::Clamp(static_cast<float>(HorrorLevelDbl), 0.0f, 1.0f);

	// Hospice safety: cap horror_level
	if (bHospiceMode && HorrorLevel > 0.3f)
	{
		UE_LOG(LogMonolithFloorPlan, Log, TEXT("Hospice mode: capping horror_level from %.2f to 0.3"), HorrorLevel);
		HorrorLevel = 0.3f;
	}

	FRandomStream Rng(Seed);

	// ---- Parse optional floor_index for per-floor generation ----
	double FloorIndexDbl = -1.0;
	Params->TryGetNumberField(TEXT("floor_index"), FloorIndexDbl);
	int32 FloorIndex = static_cast<int32>(FloorIndexDbl);

	// ---- WP-A: Parse template params ----
	FString TemplateName;
	Params->TryGetStringField(TEXT("template"), TemplateName);
	FString TemplateCategory;
	Params->TryGetStringField(TEXT("template_category"), TemplateCategory);
	bool bUseTemplates = true;
	Params->TryGetBoolField(TEXT("use_templates"), bUseTemplates);
	FString Genre;
	Params->TryGetStringField(TEXT("genre"), Genre);

	// Genre override: horror genre forces horror template category
	if (Genre.ToLower() == TEXT("horror") && TemplateCategory.IsEmpty())
	{
		TemplateCategory = TEXT("horror");
	}

	// ---- WP-A: Template-based floor plan path ----
	if (bUseTemplates)
	{
		FString TemplateError;
		FString ResolvedTemplate = TemplateName;
		FString ResolvedCategory = TemplateCategory;

		// Resolve category from archetype if not specified
		if (ResolvedCategory.IsEmpty() && ResolvedTemplate.IsEmpty())
		{
			ResolvedCategory = ArchetypeToTemplateCategory(ArchetypeName);
		}

		// If no specific template, select one
		if (ResolvedTemplate.IsEmpty() && !ResolvedCategory.IsEmpty())
		{
			ResolvedTemplate = SelectTemplate(ResolvedCategory, static_cast<float>(FootprintW), static_cast<float>(FootprintH), Rng, TemplateError);
		}

		// If we still don't have a category but have a template name, try common categories
		if (!ResolvedTemplate.IsEmpty() && ResolvedCategory.IsEmpty())
		{
			static const TArray<FString> Categories = { TEXT("residential"), TEXT("commercial"), TEXT("horror") };
			for (const FString& Cat : Categories)
			{
				FString TestPath = FPaths::Combine(GetTemplateDirectory(), Cat, ResolvedTemplate + TEXT(".json"));
				if (IFileManager::Get().FileExists(*TestPath))
				{
					ResolvedCategory = Cat;
					break;
				}
			}
		}

		if (!ResolvedTemplate.IsEmpty() && !ResolvedCategory.IsEmpty())
		{
			TArray<TArray<int32>> TemplateGrid;
			int32 TemplateGridW = 0, TemplateGridH = 0;
			TArray<FRoomDef> TemplateRooms;
			TArray<FDoorDef> TemplateDoors;
			TArray<FStairwellDef> TemplateStairwells;

			if (LoadFloorPlanTemplate(ResolvedTemplate, ResolvedCategory,
				TemplateGrid, TemplateGridW, TemplateGridH,
				TemplateRooms, TemplateDoors, TemplateStairwells, TemplateError))
			{
				// Compute target grid dimensions from requested footprint
				int32 TargetGridW = FMath::Max(2, FMath::RoundToInt32(static_cast<float>(FootprintW) / CellSize));
				int32 TargetGridH = FMath::Max(2, FMath::RoundToInt32(static_cast<float>(FootprintH) / CellSize));

				// Scale if needed
				if (TargetGridW != TemplateGridW || TargetGridH != TemplateGridH)
				{
					FString ScaleError;
					if (!ScaleTemplateGrid(TemplateGrid, TemplateGridW, TemplateGridH,
						TemplateRooms, TemplateDoors, TemplateStairwells,
						TargetGridW, TargetGridH, ScaleError))
					{
						UE_LOG(LogMonolithFloorPlan, Warning, TEXT("Template scaling failed (%s), using original template size"), *ScaleError);
						// Reload at original size — scaling modified in place
						TemplateGrid.Empty(); TemplateRooms.Empty(); TemplateDoors.Empty(); TemplateStairwells.Empty();
						LoadFloorPlanTemplate(ResolvedTemplate, ResolvedCategory,
							TemplateGrid, TemplateGridW, TemplateGridH,
							TemplateRooms, TemplateDoors, TemplateStairwells, TemplateError);
					}
				}

				// R2-I4: Hospice door width clamp — all doors >= 120cm
				if (bHospiceMode)
				{
					for (FDoorDef& Door : TemplateDoors)
					{
						if (Door.Width < 120.0f)
						{
							Door.Width = 120.0f;
						}
					}
				}

				// Ensure exterior entrance exists on template
				EnsureExteriorEntrance(TemplateGrid, TemplateGridW, TemplateGridH,
					TemplateRooms, TemplateDoors, bHospiceMode, Rng);

				// Validate door clearances
				ValidateDoorClearances(TemplateGrid, TemplateGridW, TemplateGridH,
					TemplateRooms, TemplateDoors, CellSize);

				// ---- Horror post-processing on template ----
				TArray<FRoomSpaceSyntax> PerRoomSS;
				FSpaceSyntaxScores SSScores;
				TSet<int32> LockedDoors;
				int32 WrongRoomCount = 0;

				if (HorrorLevel > 0.0f)
				{
					LockedDoors = ApplyHorrorModifiers(TemplateRooms, TemplateDoors, HorrorLevel, bHospiceMode, Rng, PerRoomSS, SSScores);
					if (HorrorLevel > 0.7f && !bHospiceMode)
					{
						WrongRoomCount = ApplyWrongRoomInjection(TemplateRooms, Rng);
					}
				}
				else
				{
					TSet<int32> NoLocked;
					ComputeSpaceSyntax(TemplateRooms, TemplateDoors, NoLocked, PerRoomSS, SSScores);
				}

				// ---- Build output JSON (same format as treemap path) ----
				TSharedPtr<FJsonObject> ResultJson = BuildOutputJson(
					TemplateGrid, TemplateGridW, TemplateGridH, TemplateRooms, TemplateDoors,
					ArchetypeName, static_cast<float>(FootprintW), static_cast<float>(FootprintH),
					bHospiceMode, CellSize);

				ResultJson->SetNumberField(TEXT("seed"), Seed);
				ResultJson->SetNumberField(TEXT("floor_index"), FloorIndex);
				ResultJson->SetStringField(TEXT("template_name"), ResolvedTemplate);
				ResultJson->SetStringField(TEXT("template_category"), ResolvedCategory);
				ResultJson->SetBoolField(TEXT("from_template"), true);

				// Entrance info
				{
					const FDoorDef* EntranceDoor = nullptr;
					for (const FDoorDef& D : TemplateDoors)
					{
						if (D.RoomB == TEXT("exterior") || D.RoomB == TEXT("EXTERIOR")
							|| D.RoomA == TEXT("exterior") || D.RoomA == TEXT("EXTERIOR"))
						{
							EntranceDoor = &D;
							break;
						}
					}
					ResultJson->SetBoolField(TEXT("has_exterior_entrance"), EntranceDoor != nullptr);
					if (EntranceDoor)
					{
						ResultJson->SetStringField(TEXT("entrance_door_id"), EntranceDoor->DoorId);
						ResultJson->SetStringField(TEXT("entrance_wall"), EntranceDoor->Wall);
						FString EntranceRoom = (EntranceDoor->RoomA == TEXT("exterior") || EntranceDoor->RoomA == TEXT("EXTERIOR"))
							? EntranceDoor->RoomB : EntranceDoor->RoomA;
						ResultJson->SetStringField(TEXT("entrance_room"), EntranceRoom);
					}
				}

				// Horror + Space Syntax data
				EmitHorrorAndSpaceSyntaxJson(ResultJson, TemplateRooms, TemplateDoors, LockedDoors, PerRoomSS, SSScores, HorrorLevel, WrongRoomCount);

				UE_LOG(LogMonolithFloorPlan, Log,
					TEXT("Floor plan from template '%s/%s': %d rooms, %d doors, grid=%dx%d, seed=%d, horror=%.2f"),
					*ResolvedCategory, *ResolvedTemplate,
					TemplateRooms.Num(), TemplateDoors.Num(), TemplateGridW, TemplateGridH, Seed, HorrorLevel);

				return FMonolithActionResult::Success(ResultJson);
			}
			else
			{
				UE_LOG(LogMonolithFloorPlan, Warning, TEXT("Template load failed: %s — falling back to treemap"), *TemplateError);
			}
		}
		else if (!TemplateError.IsEmpty())
		{
			UE_LOG(LogMonolithFloorPlan, Log, TEXT("Template selection: %s — falling back to treemap"), *TemplateError);
		}
	}

	// ==== TREEMAP FALLBACK (existing code) ====

	// ---- Load archetype ----
	FBuildingArchetype Archetype;
	FString LoadError;
	if (!LoadArchetype(ArchetypeName, Archetype, LoadError))
		return FMonolithActionResult::Error(LoadError);

	// ---- Validate stairwell requirement for multi-floor archetypes ----
	FString StairwellError = ValidateStairwellRequirement(Archetype);
	if (!StairwellError.IsEmpty())
		return FMonolithActionResult::Error(StairwellError);

	// ---- Compute grid dimensions ----
	int32 GridW = FMath::Max(2, FMath::RoundToInt32(static_cast<float>(FootprintW) / CellSize));
	int32 GridH = FMath::Max(2, FMath::RoundToInt32(static_cast<float>(FootprintH) / CellSize));

	// ---- Validate footprint capacity (warn, don't fail — do our best with available space) ----
	FString CapacityError = ValidateFootprintCapacity(Archetype, GridW, GridH, FloorIndex);
	if (!CapacityError.IsEmpty())
	{
		UE_LOG(LogMonolithFloorPlan, Warning, TEXT("%s — will generate with reduced room set"), *CapacityError);
		// Don't return error — let ResolveRoomInstances drop optional rooms to fit
	}

	UE_LOG(LogMonolithFloorPlan, Log, TEXT("Generating floor plan: archetype=%s, grid=%dx%d, cell=%.0f, seed=%d, hospice=%d, floor=%d, circulation=%d"),
		*Archetype.Name, GridW, GridH, CellSize, Seed, bHospiceMode ? 1 : 0, FloorIndex, static_cast<int32>(Archetype.Circulation));

	// ---- Resolve room instances (with per-floor filtering) ----
	TArray<FRoomInstance> Instances = ResolveRoomInstances(Archetype, GridW, GridH, Rng, FloorIndex);

	if (Instances.Num() == 0)
		return FMonolithActionResult::Error(TEXT("Archetype produced zero room instances for this floor. Check per-floor room assignments."));

	// ---- Squarified treemap layout ----
	TArray<FGridRect> Rects = SquarifiedTreemapLayout(Instances, GridW, GridH);

	// ---- Correct aspect ratios (per-room max_aspect from archetype) ----
	CorrectAspectRatios(Rects, Instances, GridW, GridH);

	// ---- Validate room quality (warn on remaining extreme aspect ratios) ----
	float MaxAspect = static_cast<float>(MinAspectDbl);
	for (int32 i = 0; i < Rects.Num(); ++i)
	{
		float RoomMaxAspect = (i < Instances.Num()) ? Instances[i].MaxAspect : MaxAspect;
		float EffectiveMax = FMath::Max(MaxAspect, RoomMaxAspect);
		if (Rects[i].AspectRatio() > EffectiveMax && Rects[i].Area() > 2)
		{
			UE_LOG(LogMonolithFloorPlan, Warning, TEXT("Room '%s' has extreme aspect ratio %.1f (%dx%d, max=%.1f) -- may produce awkward geometry"),
				*Instances[i].RoomId, Rects[i].AspectRatio(), Rects[i].W, Rects[i].H, EffectiveMax);
		}
	}

	// ---- Build grid ----
	TArray<TArray<int32>> Grid = BuildGridFromRects(Rects, GridW, GridH);

	// ---- Build initial room defs ----
	TArray<FRoomDef> Rooms = BuildRoomDefs(Grid, GridW, GridH, Instances);

	// ---- WP-2: Adjacency MUST_NOT validation + swap pass ----
	int32 AdjacencyViolations = 0;
	if (Archetype.AdjacencyMatrix.Rules.Num() > 0)
	{
		AdjacencyViolations = ValidateAndFixAdjacency(Grid, GridW, GridH, Rooms, Archetype.AdjacencyMatrix, 3);

		// Also check MUST requirements
		TArray<TPair<int32, int32>> UnmetMust = FindUnmetMustRequirements(Grid, GridW, GridH, Rooms, Archetype.AdjacencyMatrix);
		for (const auto& Pair : UnmetMust)
		{
			UE_LOG(LogMonolithFloorPlan, Warning, TEXT("Unmet MUST adjacency: '%s' (%s) should be adjacent to '%s' (%s)"),
				*Rooms[Pair.Key].RoomId, *Rooms[Pair.Key].RoomType,
				*Rooms[Pair.Value].RoomId, *Rooms[Pair.Value].RoomType);
		}
		AdjacencyViolations += UnmetMust.Num();
	}

	// ---- WP-2: Insert corridors based on circulation pattern ----
	InsertCorridorsForCirculation(Archetype.Circulation, Grid, GridW, GridH, Rooms, Archetype.Adjacency, bHospiceMode, Rng);

	// ---- Hospice: rest alcoves ----
	if (bHospiceMode)
	{
		InsertRestAlcoves(Grid, GridW, GridH, Rooms, 4, Rng);
	}

	// ---- WP-2: Privacy gradient validation ----
	TArray<FString> PrivacyViolations;
	int32 PrivacyViolationCount = ValidatePrivacyGradient(Grid, GridW, GridH, Rooms, PrivacyViolations);

	// ---- Place doors ----
	TArray<FDoorDef> Doors = PlaceDoors(Grid, GridW, GridH, Rooms, Archetype.Adjacency, bHospiceMode, Rng);

	// ---- Ensure exterior entrance exists ----
	EnsureExteriorEntrance(Grid, GridW, GridH, Rooms, Doors, bHospiceMode, Rng);

	// ---- Validate door clearances ----
	ValidateDoorClearances(Grid, GridW, GridH, Rooms, Doors, CellSize);

	// ---- WP-6: Horror post-processing + Space Syntax ----
	TArray<FRoomSpaceSyntax> PerRoomSS;
	FSpaceSyntaxScores SSScores;
	TSet<int32> LockedDoors;
	int32 WrongRoomCount = 0;

	// Always compute Space Syntax (even at horror_level 0 -- scores are informational)
	if (HorrorLevel > 0.0f)
	{
		LockedDoors = ApplyHorrorModifiers(Rooms, Doors, HorrorLevel, bHospiceMode, Rng, PerRoomSS, SSScores);
		// Wrong room injection only at high horror
		if (HorrorLevel > 0.7f && !bHospiceMode)
		{
			WrongRoomCount = ApplyWrongRoomInjection(Rooms, Rng);
		}
	}
	else
	{
		// No horror, but still compute Space Syntax for informational output
		TSet<int32> NoLocked;
		ComputeSpaceSyntax(Rooms, Doors, NoLocked, PerRoomSS, SSScores);
	}

	// ---- WP-2: Wet wall clustering stats ----
	TSet<FIntPoint> PlumbingCells = BuildPlumbingChaseSet(Grid, GridW, GridH, Rooms);
	int32 WetRoomCount = 0;
	int32 ClusteredWetRooms = 0;
	for (int32 i = 0; i < Rooms.Num(); ++i)
	{
		if (!IsWetRoom(Rooms[i].RoomType)) continue;
		++WetRoomCount;

		// Check if this wet room shares a wall with another wet room
		for (int32 j = 0; j < Rooms.Num(); ++j)
		{
			if (i == j || !IsWetRoom(Rooms[j].RoomType)) continue;
			if (RoomsShareEdge(Rooms[i], Rooms[j]))
			{
				++ClusteredWetRooms;
				break;
			}
		}
	}

	// ---- Build output ----
	TSharedPtr<FJsonObject> ResultJson = BuildOutputJson(
		Grid, GridW, GridH, Rooms, Doors,
		Archetype.Name, static_cast<float>(FootprintW), static_cast<float>(FootprintH),
		bHospiceMode, CellSize);

	ResultJson->SetNumberField(TEXT("seed"), Seed);
	ResultJson->SetNumberField(TEXT("floor_index"), FloorIndex);
	ResultJson->SetNumberField(TEXT("floor_height"), Archetype.FloorHeight);
	ResultJson->SetBoolField(TEXT("from_template"), false);

	// ---- WP-2: Add intelligence stats to output ----
	auto StatsObj = ResultJson->GetObjectField(TEXT("stats"));
	if (StatsObj.IsValid())
	{
		StatsObj->SetNumberField(TEXT("adjacency_violations"), AdjacencyViolations);
		StatsObj->SetNumberField(TEXT("privacy_violations"), PrivacyViolationCount);
		StatsObj->SetNumberField(TEXT("wet_rooms"), WetRoomCount);
		StatsObj->SetNumberField(TEXT("wet_rooms_clustered"), ClusteredWetRooms);

		// Circulation type
		FString CircStr;
		switch (Archetype.Circulation)
		{
		case ECirculationType::HubSpoke: CircStr = TEXT("hub_spoke"); break;
		case ECirculationType::Racetrack: CircStr = TEXT("racetrack"); break;
		case ECirculationType::Enfilade: CircStr = TEXT("enfilade"); break;
		default: CircStr = TEXT("double_loaded"); break;
		}
		StatsObj->SetStringField(TEXT("circulation"), CircStr);
	}

	// ---- WP-2: Privacy violations detail ----
	if (PrivacyViolations.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ViolArr;
		for (const FString& V : PrivacyViolations)
		{
			ViolArr.Add(MakeShared<FJsonValueString>(V));
		}
		ResultJson->SetArrayField(TEXT("privacy_violations"), ViolArr);
	}

	// ---- Entrance info ----
	{
		const FDoorDef* EntranceDoor = nullptr;
		for (const FDoorDef& D : Doors)
		{
			if (D.RoomB == TEXT("exterior") || D.RoomA == TEXT("exterior"))
			{
				EntranceDoor = &D;
				break;
			}
		}

		ResultJson->SetBoolField(TEXT("has_exterior_entrance"), EntranceDoor != nullptr);
		if (EntranceDoor)
		{
			ResultJson->SetStringField(TEXT("entrance_door_id"), EntranceDoor->DoorId);
			ResultJson->SetStringField(TEXT("entrance_wall"), EntranceDoor->Wall);
			// entrance_room = whichever side is not "exterior"
			FString EntranceRoom = (EntranceDoor->RoomA == TEXT("exterior")) ? EntranceDoor->RoomB : EntranceDoor->RoomA;
			ResultJson->SetStringField(TEXT("entrance_room"), EntranceRoom);
		}
	}

	// Include material hints if present
	if (!Archetype.MaterialHints.Exterior.IsEmpty() || !Archetype.MaterialHints.Interior.IsEmpty() || !Archetype.MaterialHints.FloorMaterial.IsEmpty())
	{
		auto MatHints = MakeShared<FJsonObject>();
		if (!Archetype.MaterialHints.Exterior.IsEmpty())
			MatHints->SetStringField(TEXT("exterior"), Archetype.MaterialHints.Exterior);
		if (!Archetype.MaterialHints.Interior.IsEmpty())
			MatHints->SetStringField(TEXT("interior"), Archetype.MaterialHints.Interior);
		if (!Archetype.MaterialHints.FloorMaterial.IsEmpty())
			MatHints->SetStringField(TEXT("floor"), Archetype.MaterialHints.FloorMaterial);
		ResultJson->SetObjectField(TEXT("material_hints"), MatHints);
	}

	// ---- WP-6: Emit horror + Space Syntax data ----
	EmitHorrorAndSpaceSyntaxJson(ResultJson, Rooms, Doors, LockedDoors, PerRoomSS, SSScores, HorrorLevel, WrongRoomCount);

	UE_LOG(LogMonolithFloorPlan, Log, TEXT("Floor plan generated: %d rooms, %d doors, seed=%d, floor=%d, adj_violations=%d, privacy_violations=%d, wet_clustered=%d/%d, horror=%.2f, locked=%d, horror_score=%.3f"),
		Rooms.Num(), Doors.Num(), Seed, FloorIndex, AdjacencyViolations, PrivacyViolationCount, ClusteredWetRooms, WetRoomCount,
		HorrorLevel, LockedDoors.Num(), SSScores.HorrorScore);

	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithMeshFloorPlanGenerator::ListBuildingArchetypes(const TSharedPtr<FJsonObject>& Params)
{
	FString Dir = GetArchetypeDirectory();
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Dir, TEXT("*.json")), true, false);

	auto Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ArchetypeArr;

	for (const FString& File : Files)
	{
		FString Name = FPaths::GetBaseFilename(File);

		// Try to load and get the description
		FBuildingArchetype Arch;
		FString Err;
		FString FullPath = FPaths::Combine(Dir, File);

		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetStringField(TEXT("file"), FullPath);

		if (LoadArchetype(Name, Arch, Err))
		{
			Entry->SetStringField(TEXT("description"), Arch.Description);
			Entry->SetNumberField(TEXT("room_types"), Arch.Rooms.Num());
			Entry->SetNumberField(TEXT("adjacency_rules"), Arch.Adjacency.Num());
			Entry->SetStringField(TEXT("roof_type"), Arch.RoofType);

			// WP-2: include circulation type
			FString CircStr;
			switch (Arch.Circulation)
			{
			case ECirculationType::HubSpoke: CircStr = TEXT("hub_spoke"); break;
			case ECirculationType::Racetrack: CircStr = TEXT("racetrack"); break;
			case ECirculationType::Enfilade: CircStr = TEXT("enfilade"); break;
			default: CircStr = TEXT("double_loaded"); break;
			}
			Entry->SetStringField(TEXT("circulation"), CircStr);
			Entry->SetBoolField(TEXT("has_adjacency_matrix"), Arch.AdjacencyMatrix.Rules.Num() > 0);

			auto Floors = MakeShared<FJsonObject>();
			Floors->SetNumberField(TEXT("min"), Arch.FloorsMin);
			Floors->SetNumberField(TEXT("max"), Arch.FloorsMax);
			Entry->SetObjectField(TEXT("floors"), Floors);
		}

		ArchetypeArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Result->SetArrayField(TEXT("archetypes"), ArchetypeArr);
	Result->SetNumberField(TEXT("count"), Files.Num());
	Result->SetStringField(TEXT("directory"), Dir);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshFloorPlanGenerator::GetBuildingArchetype(const TSharedPtr<FJsonObject>& Params)
{
	FString ArchetypeName;
	if (!Params->TryGetStringField(TEXT("archetype"), ArchetypeName) || ArchetypeName.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required 'archetype' parameter"));

	// Load the raw JSON file
	FString FilePath;
	if (ArchetypeName.EndsWith(TEXT(".json")))
		FilePath = ArchetypeName;
	else
		FilePath = FPaths::Combine(GetArchetypeDirectory(), ArchetypeName + TEXT(".json"));

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Archetype not found: %s"), *FilePath));

	TSharedPtr<FJsonObject> Json = FMonolithJsonUtils::Parse(JsonString);
	if (!Json.IsValid())
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to parse archetype JSON: %s"), *FilePath));

	// Return the raw JSON as the result, wrapped in a result object
	auto Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("archetype"), Json);
	Result->SetStringField(TEXT("file"), FilePath);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// WP-6: Room Adjacency Graph Helpers
// ============================================================================

int32 FMonolithMeshFloorPlanGenerator::FindRoomIndexById(const TArray<FRoomDef>& Rooms, const FString& RoomId)
{
	for (int32 i = 0; i < Rooms.Num(); ++i)
	{
		if (Rooms[i].RoomId == RoomId)
			return i;
	}
	return INDEX_NONE;
}

TArray<TSet<int32>> FMonolithMeshFloorPlanGenerator::BuildRoomAdjacencyFromDoors(
	const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
	const TSet<int32>* LockedDoors)
{
	TArray<TSet<int32>> Adj;
	Adj.SetNum(Rooms.Num());

	for (int32 DoorIdx = 0; DoorIdx < Doors.Num(); ++DoorIdx)
	{
		// Skip locked doors if specified
		if (LockedDoors && LockedDoors->Contains(DoorIdx))
			continue;

		const FDoorDef& D = Doors[DoorIdx];

		// Skip exterior doors
		if (D.RoomA == TEXT("exterior") || D.RoomB == TEXT("exterior"))
			continue;

		int32 IdxA = FindRoomIndexById(Rooms, D.RoomA);
		int32 IdxB = FindRoomIndexById(Rooms, D.RoomB);

		// Handle "corridor" as a room ID -- find the corridor room
		if (IdxA == INDEX_NONE && D.RoomA == TEXT("corridor"))
		{
			for (int32 i = 0; i < Rooms.Num(); ++i)
			{
				if (Rooms[i].RoomType == TEXT("corridor"))
				{
					// Check if this corridor shares the door edge
					IdxA = i;
					break;
				}
			}
		}
		if (IdxB == INDEX_NONE && D.RoomB == TEXT("corridor"))
		{
			for (int32 i = 0; i < Rooms.Num(); ++i)
			{
				if (Rooms[i].RoomType == TEXT("corridor"))
				{
					IdxB = i;
					break;
				}
			}
		}

		if (IdxA != INDEX_NONE && IdxB != INDEX_NONE && IdxA != IdxB)
		{
			Adj[IdxA].Add(IdxB);
			Adj[IdxB].Add(IdxA);
		}
	}

	return Adj;
}

int32 FMonolithMeshFloorPlanGenerator::BFSShortestPath(const TArray<TSet<int32>>& Adj, int32 From, int32 To)
{
	if (From == To) return 0;
	if (From < 0 || To < 0 || From >= Adj.Num() || To >= Adj.Num()) return -1;

	TArray<int32> Dist;
	Dist.SetNumZeroed(Adj.Num());
	for (int32& D : Dist) D = -1;

	TQueue<int32> Queue;
	Queue.Enqueue(From);
	Dist[From] = 0;

	while (!Queue.IsEmpty())
	{
		int32 Curr;
		Queue.Dequeue(Curr);

		for (int32 Neighbor : Adj[Curr])
		{
			if (Dist[Neighbor] == -1)
			{
				Dist[Neighbor] = Dist[Curr] + 1;
				if (Neighbor == To) return Dist[Neighbor];
				Queue.Enqueue(Neighbor);
			}
		}
	}

	return -1; // Unreachable
}

bool FMonolithMeshFloorPlanGenerator::IsGraphConnected(const TArray<TSet<int32>>& Adj, int32 NumRooms)
{
	if (NumRooms <= 1) return true;

	TSet<int32> Visited;
	TQueue<int32> Queue;

	// Start BFS from room 0
	Queue.Enqueue(0);
	Visited.Add(0);

	while (!Queue.IsEmpty())
	{
		int32 Curr;
		Queue.Dequeue(Curr);

		for (int32 Neighbor : Adj[Curr])
		{
			if (!Visited.Contains(Neighbor))
			{
				Visited.Add(Neighbor);
				Queue.Enqueue(Neighbor);
			}
		}
	}

	return Visited.Num() == NumRooms;
}

// ============================================================================
// WP-6: Tarjan's Bridge-Finding Algorithm
// ============================================================================

TSet<int32> FMonolithMeshFloorPlanGenerator::FindBridgeDoors(
	const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
	const TSet<int32>& AlreadyLocked)
{
	TSet<int32> BridgeDoors;

	// Build the unlocked adjacency graph
	TArray<TSet<int32>> Adj = BuildRoomAdjacencyFromDoors(Rooms, Doors, &AlreadyLocked);
	const int32 N = Rooms.Num();

	// Tarjan's bridge algorithm using DFS
	TArray<int32> Disc, Low, Parent;
	Disc.SetNumZeroed(N);
	Low.SetNumZeroed(N);
	Parent.SetNumZeroed(N);
	for (int32 i = 0; i < N; ++i)
	{
		Disc[i] = -1;
		Low[i] = -1;
		Parent[i] = -1;
	}

	// Bridge edges as (RoomA, RoomB) pairs
	TSet<uint64> BridgeEdges;

	int32 Timer = 0;

	// Iterative DFS to avoid stack overflow on large graphs
	struct FStackFrame
	{
		int32 Node;
		int32 NeighborIdx;
	};

	for (int32 Start = 0; Start < N; ++Start)
	{
		if (Disc[Start] != -1) continue;

		TArray<FStackFrame> Stack;
		Stack.Push({Start, 0});
		Disc[Start] = Low[Start] = Timer++;

		while (Stack.Num() > 0)
		{
			FStackFrame& Frame = Stack.Last();
			int32 U = Frame.Node;

			TArray<int32> Neighbors = Adj[U].Array();

			if (Frame.NeighborIdx < Neighbors.Num())
			{
				int32 V = Neighbors[Frame.NeighborIdx];
				Frame.NeighborIdx++;

				if (Disc[V] == -1)
				{
					Parent[V] = U;
					Disc[V] = Low[V] = Timer++;
					Stack.Push({V, 0});
				}
				else if (V != Parent[U])
				{
					Low[U] = FMath::Min(Low[U], Disc[V]);
				}
			}
			else
			{
				// All neighbors processed, pop
				Stack.Pop();
				if (Stack.Num() > 0)
				{
					int32 ParentNode = Stack.Last().Node;
					Low[ParentNode] = FMath::Min(Low[ParentNode], Low[U]);

					// If Low[U] > Disc[ParentNode], edge (ParentNode, U) is a bridge
					if (Low[U] > Disc[ParentNode])
					{
						uint64 Key = (uint64)FMath::Min(ParentNode, U) << 32 | (uint64)FMath::Max(ParentNode, U);
						BridgeEdges.Add(Key);
					}
				}
			}
		}
	}

	// Map bridge edges back to door indices
	for (int32 DoorIdx = 0; DoorIdx < Doors.Num(); ++DoorIdx)
	{
		if (AlreadyLocked.Contains(DoorIdx)) continue;

		const FDoorDef& D = Doors[DoorIdx];
		if (D.RoomA == TEXT("exterior") || D.RoomB == TEXT("exterior")) continue;

		int32 IdxA = FindRoomIndexById(Rooms, D.RoomA);
		int32 IdxB = FindRoomIndexById(Rooms, D.RoomB);

		// Handle "corridor" lookup
		if (IdxA == INDEX_NONE && D.RoomA == TEXT("corridor"))
		{
			for (int32 i = 0; i < Rooms.Num(); ++i)
			{
				if (Rooms[i].RoomType == TEXT("corridor")) { IdxA = i; break; }
			}
		}
		if (IdxB == INDEX_NONE && D.RoomB == TEXT("corridor"))
		{
			for (int32 i = 0; i < Rooms.Num(); ++i)
			{
				if (Rooms[i].RoomType == TEXT("corridor")) { IdxB = i; break; }
			}
		}

		if (IdxA == INDEX_NONE || IdxB == INDEX_NONE) continue;

		uint64 Key = (uint64)FMath::Min(IdxA, IdxB) << 32 | (uint64)FMath::Max(IdxA, IdxB);
		if (BridgeEdges.Contains(Key))
		{
			BridgeDoors.Add(DoorIdx);
		}
	}

	return BridgeDoors;
}

// ============================================================================
// WP-6: Horror Door Locking
// ============================================================================

TSet<int32> FMonolithMeshFloorPlanGenerator::ApplyDoorLocking(
	const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
	float LockRatio, FRandomStream& Rng)
{
	TSet<int32> LockedDoors;

	if (LockRatio <= 0.0f || Doors.Num() == 0)
		return LockedDoors;

	// Count lockable doors (interior only)
	TArray<int32> LockableDoorIndices;
	for (int32 i = 0; i < Doors.Num(); ++i)
	{
		if (Doors[i].RoomA != TEXT("exterior") && Doors[i].RoomB != TEXT("exterior"))
			LockableDoorIndices.Add(i);
	}

	if (LockableDoorIndices.Num() == 0)
		return LockedDoors;

	int32 TargetLocked = FMath::RoundToInt32(LockableDoorIndices.Num() * LockRatio);
	TargetLocked = FMath::Clamp(TargetLocked, 0, LockableDoorIndices.Num() - 1); // Always leave at least 1 unlocked

	// Helper: get room type from room ID (looks up in Rooms array, falls back to ID itself)
	auto GetRoomType = [&](const FString& RoomId) -> FString
	{
		int32 Idx = FindRoomIndexById(Rooms, RoomId);
		if (Idx != INDEX_NONE)
			return Rooms[Idx].RoomType;
		// Fallback: "corridor" is used as literal in some door defs
		return RoomId;
	};

	// Sort: prefer locking doors to private rooms first, with random tiebreaker
	TArray<TPair<float, int32>> Scored;
	for (int32 Idx : LockableDoorIndices)
	{
		const FDoorDef& D = Doors[Idx];
		int32 PA = static_cast<int32>(GetPrivacyZone(GetRoomType(D.RoomA)));
		int32 PB = static_cast<int32>(GetPrivacyZone(GetRoomType(D.RoomB)));
		int32 MaxPrivacy = FMath::Max(PA, PB);
		float Score = static_cast<float>(MaxPrivacy) + Rng.FRand() * 0.5f;
		Scored.Add({Score, Idx});
	}
	Scored.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B)
	{
		return A.Key > B.Key;
	});

	// Lock doors, checking bridges after each lock
	for (const auto& Pair : Scored)
	{
		if (LockedDoors.Num() >= TargetLocked)
			break;

		int32 DoorIdx = Pair.Value;

		// Check if this door is a bridge (would disconnect graph)
		TSet<int32> BridgeDoors = FindBridgeDoors(Rooms, Doors, LockedDoors);
		if (BridgeDoors.Contains(DoorIdx))
			continue; // Skip -- locking this would disconnect rooms

		LockedDoors.Add(DoorIdx);
	}

	UE_LOG(LogMonolithFloorPlan, Log, TEXT("Horror: locked %d/%d doors (target ratio %.0f%%)"),
		LockedDoors.Num(), LockableDoorIndices.Num(), LockRatio * 100.0f);

	return LockedDoors;
}

// ============================================================================
// WP-6: Dead-End Ratio Adjustment
// ============================================================================

TSet<int32> FMonolithMeshFloorPlanGenerator::AdjustDeadEndRatio(
	const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
	TSet<int32> LockedDoors, float TargetRatio, FRandomStream& Rng)
{
	if (Rooms.Num() <= 2)
		return LockedDoors;

	auto CountDeadEnds = [&]() -> float
	{
		TArray<TSet<int32>> Adj = BuildRoomAdjacencyFromDoors(Rooms, Doors, &LockedDoors);
		int32 DeadEnds = 0;
		for (int32 i = 0; i < Rooms.Num(); ++i)
		{
			if (Adj[i].Num() == 1)
				++DeadEnds;
		}
		return static_cast<float>(DeadEnds) / Rooms.Num();
	};

	float CurrentRatio = CountDeadEnds();
	int32 MaxIterations = Doors.Num(); // Safety cap

	while (CurrentRatio < TargetRatio && MaxIterations-- > 0)
	{
		// Find a room with 2+ connections and lock one of its non-bridge doors
		TArray<TSet<int32>> Adj = BuildRoomAdjacencyFromDoors(Rooms, Doors, &LockedDoors);

		// Find candidate doors to lock (connect rooms with 2+ neighbors)
		TArray<int32> Candidates;
		for (int32 DoorIdx = 0; DoorIdx < Doors.Num(); ++DoorIdx)
		{
			if (LockedDoors.Contains(DoorIdx)) continue;

			const FDoorDef& D = Doors[DoorIdx];
			if (D.RoomA == TEXT("exterior") || D.RoomB == TEXT("exterior")) continue;

			int32 IdxA = FindRoomIndexById(Rooms, D.RoomA);
			int32 IdxB = FindRoomIndexById(Rooms, D.RoomB);

			if (IdxA == INDEX_NONE && D.RoomA == TEXT("corridor"))
			{
				for (int32 i = 0; i < Rooms.Num(); ++i)
					if (Rooms[i].RoomType == TEXT("corridor")) { IdxA = i; break; }
			}
			if (IdxB == INDEX_NONE && D.RoomB == TEXT("corridor"))
			{
				for (int32 i = 0; i < Rooms.Num(); ++i)
					if (Rooms[i].RoomType == TEXT("corridor")) { IdxB = i; break; }
			}

			if (IdxA == INDEX_NONE || IdxB == INDEX_NONE) continue;

			// Both sides must have 2+ connections (so locking creates a dead end, not disconnection)
			if (Adj[IdxA].Num() >= 2 && Adj[IdxB].Num() >= 2)
				Candidates.Add(DoorIdx);
		}

		if (Candidates.Num() == 0)
			break;

		// Pick a random candidate
		int32 PickIdx = Rng.RandHelper(Candidates.Num());
		int32 DoorIdx = Candidates[PickIdx];

		// Bridge check
		TSet<int32> Bridges = FindBridgeDoors(Rooms, Doors, LockedDoors);
		if (Bridges.Contains(DoorIdx))
		{
			// Remove this candidate and try again
			Candidates.RemoveAt(PickIdx);
			continue;
		}

		LockedDoors.Add(DoorIdx);
		CurrentRatio = CountDeadEnds();
	}

	UE_LOG(LogMonolithFloorPlan, Log, TEXT("Horror: dead-end ratio adjusted to %.1f%% (target %.1f%%)"),
		CurrentRatio * 100.0f, TargetRatio * 100.0f);

	return LockedDoors;
}

// ============================================================================
// WP-6: Loop Breaking
// ============================================================================

TSet<int32> FMonolithMeshFloorPlanGenerator::ApplyLoopBreaking(
	const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
	TSet<int32> LockedDoors, FRandomStream& Rng)
{
	// Find loops: any edge that is NOT a bridge is part of a cycle.
	// Lock one such non-bridge door per cycle to force longer traversal.
	TSet<int32> Bridges = FindBridgeDoors(Rooms, Doors, LockedDoors);

	// Collect non-bridge, unlocked, interior doors
	TArray<int32> LoopDoors;
	for (int32 DoorIdx = 0; DoorIdx < Doors.Num(); ++DoorIdx)
	{
		if (LockedDoors.Contains(DoorIdx)) continue;
		if (Bridges.Contains(DoorIdx)) continue;
		if (Doors[DoorIdx].RoomA == TEXT("exterior") || Doors[DoorIdx].RoomB == TEXT("exterior")) continue;
		LoopDoors.Add(DoorIdx);
	}

	// Lock loop doors one at a time, re-checking bridges each time
	for (int32 i = 0; i < LoopDoors.Num(); ++i)
	{
		int32 DoorIdx = LoopDoors[i];
		if (LockedDoors.Contains(DoorIdx)) continue;

		// Verify still not a bridge after previous locks
		TSet<int32> CurrentBridges = FindBridgeDoors(Rooms, Doors, LockedDoors);
		if (CurrentBridges.Contains(DoorIdx))
			continue;

		LockedDoors.Add(DoorIdx);

		UE_LOG(LogMonolithFloorPlan, Verbose, TEXT("Horror: loop-break locked %s"), *Doors[DoorIdx].DoorId);

		// Only break one loop per call for controlled degradation
		break;
	}

	return LockedDoors;
}

// ============================================================================
// WP-6: Wrong Room Injection
// ============================================================================

int32 FMonolithMeshFloorPlanGenerator::ApplyWrongRoomInjection(
	TArray<FRoomDef>& Rooms, FRandomStream& Rng)
{
	static const TArray<FString> WrongTypes = {
		TEXT("exam_room"), TEXT("laboratory"), TEXT("operating_theater"),
		TEXT("shrine"), TEXT("server_room"), TEXT("holding_cell"),
		TEXT("observation"), TEXT("morgue")
	};

	// Find rooms that are NOT corridors, entries, or stairwells
	TArray<int32> CandidateRooms;
	for (int32 i = 0; i < Rooms.Num(); ++i)
	{
		const FString& Type = Rooms[i].RoomType;
		if (Type == TEXT("corridor") || Type == TEXT("entry") || Type == TEXT("entryway") ||
			Type == TEXT("foyer") || Type == TEXT("stairwell") || Type == TEXT("elevator") ||
			Type == TEXT("rest_alcove"))
			continue;
		CandidateRooms.Add(i);
	}

	if (CandidateRooms.Num() == 0)
		return 0;

	// Change 1-2 rooms
	int32 Count = FMath::Min(1 + (Rng.RandHelper(2) > 0 ? 1 : 0), CandidateRooms.Num());
	int32 Changed = 0;

	for (int32 c = 0; c < Count && CandidateRooms.Num() > 0; ++c)
	{
		int32 PickIdx = Rng.RandHelper(CandidateRooms.Num());
		int32 RoomIdx = CandidateRooms[PickIdx];

		// Pick a wrong type that differs from the current type
		FString NewType = WrongTypes[Rng.RandHelper(WrongTypes.Num())];
		if (NewType == Rooms[RoomIdx].RoomType && WrongTypes.Num() > 1)
		{
			NewType = WrongTypes[(Rng.RandHelper(WrongTypes.Num() - 1) + 1) % WrongTypes.Num()];
		}

		UE_LOG(LogMonolithFloorPlan, Log, TEXT("Horror: wrong-room injection '%s' -> '%s' (room %s)"),
			*Rooms[RoomIdx].RoomType, *NewType, *Rooms[RoomIdx].RoomId);

		Rooms[RoomIdx].RoomType = NewType;
		CandidateRooms.RemoveAt(PickIdx);
		++Changed;
	}

	return Changed;
}

// ============================================================================
// WP-6: Top-Level Horror Modifier Application
// ============================================================================

TSet<int32> FMonolithMeshFloorPlanGenerator::ApplyHorrorModifiers(
	TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
	float HorrorLevel, bool bHospiceMode, FRandomStream& Rng,
	TArray<FRoomSpaceSyntax>& OutPerRoom, FSpaceSyntaxScores& OutScores)
{
	TSet<int32> LockedDoors;

	// Door locking (horror_level > 0.3, disabled in hospice)
	if (HorrorLevel > 0.3f && !bHospiceMode)
	{
		// Lock 60-80% of doors, scaled by horror_level
		float LockRatio = 0.6f + (HorrorLevel - 0.3f) * 0.286f; // 0.6 at 0.3, 0.8 at 1.0
		LockRatio = FMath::Clamp(LockRatio, 0.6f, 0.8f);
		LockedDoors = ApplyDoorLocking(Rooms, Doors, LockRatio, Rng);
	}

	// Dead-end ratio control (horror_level > 0.2)
	if (HorrorLevel > 0.2f)
	{
		float TargetDeadEnd = 0.15f + HorrorLevel * 0.15f; // 15-30%
		// Hospice: cap at 15%
		if (bHospiceMode)
			TargetDeadEnd = FMath::Min(TargetDeadEnd, 0.15f);

		LockedDoors = AdjustDeadEndRatio(Rooms, Doors, LockedDoors, TargetDeadEnd, Rng);
	}

	// Loop breaking (horror_level > 0.5, not in hospice)
	if (HorrorLevel > 0.5f && !bHospiceMode)
	{
		LockedDoors = ApplyLoopBreaking(Rooms, Doors, LockedDoors, Rng);
	}

	// Compute Space Syntax with the final locked door set
	ComputeSpaceSyntax(Rooms, Doors, LockedDoors, OutPerRoom, OutScores);

	return LockedDoors;
}

// ============================================================================
// WP-6: Space Syntax Computation
// ============================================================================

void FMonolithMeshFloorPlanGenerator::ComputeSpaceSyntax(
	const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
	const TSet<int32>& LockedDoors,
	TArray<FRoomSpaceSyntax>& OutPerRoom, FSpaceSyntaxScores& OutScores)
{
	const int32 N = Rooms.Num();
	OutPerRoom.SetNum(N);
	OutScores = FSpaceSyntaxScores();

	if (N == 0) return;

	// Build adjacency graph (excluding locked doors)
	TArray<TSet<int32>> Adj = BuildRoomAdjacencyFromDoors(Rooms, Doors, &LockedDoors);

	// Find entry room (room with an exterior door, or first public room, or room 0)
	int32 EntryRoom = 0;
	for (int32 DoorIdx = 0; DoorIdx < Doors.Num(); ++DoorIdx)
	{
		const FDoorDef& D = Doors[DoorIdx];
		if (D.RoomA == TEXT("exterior"))
		{
			int32 Idx = FindRoomIndexById(Rooms, D.RoomB);
			if (Idx != INDEX_NONE) { EntryRoom = Idx; break; }
		}
		else if (D.RoomB == TEXT("exterior"))
		{
			int32 Idx = FindRoomIndexById(Rooms, D.RoomA);
			if (Idx != INDEX_NONE) { EntryRoom = Idx; break; }
		}
	}

	// BFS depth from entry
	{
		TArray<int32> Dist;
		Dist.SetNum(N);
		for (int32& D : Dist) D = -1;

		TQueue<int32> Queue;
		Queue.Enqueue(EntryRoom);
		Dist[EntryRoom] = 0;

		while (!Queue.IsEmpty())
		{
			int32 Curr;
			Queue.Dequeue(Curr);
			for (int32 Neighbor : Adj[Curr])
			{
				if (Dist[Neighbor] == -1)
				{
					Dist[Neighbor] = Dist[Curr] + 1;
					Queue.Enqueue(Neighbor);
				}
			}
		}

		for (int32 i = 0; i < N; ++i)
		{
			OutPerRoom[i].Depth = FMath::Max(0, Dist[i]); // -1 means unreachable, clamp to 0
		}
	}

	// Connectivity (number of direct neighbors via unlocked doors)
	for (int32 i = 0; i < N; ++i)
	{
		OutPerRoom[i].Connectivity = Adj[i].Num();
		OutPerRoom[i].bDeadEnd = (Adj[i].Num() <= 1);
	}

	// Integration (1 / mean shortest path to all other rooms)
	for (int32 i = 0; i < N; ++i)
	{
		float TotalDist = 0.0f;
		int32 ReachableCount = 0;

		for (int32 j = 0; j < N; ++j)
		{
			if (i == j) continue;
			int32 PathLen = BFSShortestPath(Adj, i, j);
			if (PathLen > 0)
			{
				TotalDist += static_cast<float>(PathLen);
				++ReachableCount;
			}
		}

		if (ReachableCount > 0)
		{
			float MeanPath = TotalDist / ReachableCount;
			OutPerRoom[i].Integration = 1.0f / MeanPath;
		}
		else
		{
			OutPerRoom[i].Integration = 0.0f;
		}
	}

	// Aggregate scores
	float SumIntegration = 0.0f;
	float SumDepth = 0.0f;
	int32 DeadEndCount = 0;
	OutScores.MaxIntegration = 0.0f;
	OutScores.MinIntegration = TNumericLimits<float>::Max();
	OutScores.MaxDepth = 0;

	for (int32 i = 0; i < N; ++i)
	{
		SumIntegration += OutPerRoom[i].Integration;
		SumDepth += OutPerRoom[i].Depth;

		OutScores.MaxIntegration = FMath::Max(OutScores.MaxIntegration, OutPerRoom[i].Integration);
		OutScores.MinIntegration = FMath::Min(OutScores.MinIntegration, OutPerRoom[i].Integration);
		OutScores.MaxDepth = FMath::Max(OutScores.MaxDepth, OutPerRoom[i].Depth);

		if (OutPerRoom[i].bDeadEnd)
			++DeadEndCount;

		if (OutPerRoom[i].Connectivity >= 3)
			++OutScores.HubCount;
	}

	OutScores.MeanIntegration = (N > 0) ? SumIntegration / N : 0.0f;
	OutScores.MeanDepth = (N > 0) ? SumDepth / N : 0.0f;
	OutScores.DeadEndRatio = (N > 0) ? static_cast<float>(DeadEndCount) / N : 0.0f;

	// Locked door ratio
	int32 InteriorDoors = 0;
	for (const FDoorDef& D : Doors)
	{
		if (D.RoomA != TEXT("exterior") && D.RoomB != TEXT("exterior"))
			++InteriorDoors;
	}
	OutScores.LockedDoorCount = LockedDoors.Num();
	OutScores.LockedDoorRatio = (InteriorDoors > 0) ? static_cast<float>(LockedDoors.Num()) / InteriorDoors : 0.0f;

	// Build per-room locked door adjacency set for tension calculation
	TSet<int32> RoomsAdjacentToLockedDoors;
	for (int32 DoorIdx : LockedDoors)
	{
		if (DoorIdx < 0 || DoorIdx >= Doors.Num()) continue;
		const FDoorDef& D = Doors[DoorIdx];
		int32 IdxA = FindRoomIndexById(Rooms, D.RoomA);
		int32 IdxB = FindRoomIndexById(Rooms, D.RoomB);
		if (IdxA == INDEX_NONE && D.RoomA == TEXT("corridor"))
			for (int32 k = 0; k < N; ++k) if (Rooms[k].RoomType == TEXT("corridor")) { IdxA = k; break; }
		if (IdxB == INDEX_NONE && D.RoomB == TEXT("corridor"))
			for (int32 k = 0; k < N; ++k) if (Rooms[k].RoomType == TEXT("corridor")) { IdxB = k; break; }
		if (IdxA != INDEX_NONE) RoomsAdjacentToLockedDoors.Add(IdxA);
		if (IdxB != INDEX_NONE) RoomsAdjacentToLockedDoors.Add(IdxB);
	}

	// Compute tension per room
	for (int32 i = 0; i < N; ++i)
	{
		OutPerRoom[i].Tension = ComputeRoomTension(OutPerRoom[i], OutScores.MeanDepth, OutPerRoom[i].Depth);

		// Adjacent to locked doors: +0.1
		if (RoomsAdjacentToLockedDoors.Contains(i))
			OutPerRoom[i].Tension = FMath::Min(OutPerRoom[i].Tension + 0.1f, 1.0f);

		// Entry room and depth 0-1: safe zone
		if (i == EntryRoom || OutPerRoom[i].Depth <= 1)
			OutPerRoom[i].Tension = 0.0f;
	}

	// Horror score: composite metric
	// horror_score = 0.3 * dead_end_ratio + 0.2 * (max_depth / room_count) + 0.3 * (1 - mean_integration) + 0.2 * locked_door_ratio
	float NormDepth = (N > 0) ? static_cast<float>(OutScores.MaxDepth) / N : 0.0f;
	float InvIntegration = 1.0f - FMath::Clamp(OutScores.MeanIntegration, 0.0f, 1.0f);

	OutScores.HorrorScore = FMath::Clamp(
		0.3f * OutScores.DeadEndRatio +
		0.2f * FMath::Clamp(NormDepth, 0.0f, 1.0f) +
		0.3f * InvIntegration +
		0.2f * OutScores.LockedDoorRatio,
		0.0f, 1.0f);
}

float FMonolithMeshFloorPlanGenerator::ComputeRoomTension(
	const FRoomSpaceSyntax& RoomSS, float MeanDepth, int32 Depth)
{
	float Tension = 0.0f;

	// Dead-end rooms: +0.3
	if (RoomSS.bDeadEnd)
		Tension += 0.3f;

	// Rooms deeper than mean: +0.2
	if (static_cast<float>(Depth) > MeanDepth)
		Tension += 0.2f;

	// Low connectivity (1 door): +0.2
	if (RoomSS.Connectivity <= 1)
		Tension += 0.2f;

	// Note: locked door adjacency bonus (+0.1) is applied in ComputeSpaceSyntax
	// where we have access to the actual locked door set

	return FMath::Clamp(Tension, 0.0f, 1.0f);
}

// ============================================================================
// WP-6: JSON Emission for Horror + Space Syntax
// ============================================================================

void FMonolithMeshFloorPlanGenerator::EmitHorrorAndSpaceSyntaxJson(
	TSharedPtr<FJsonObject>& ResultJson,
	const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
	const TSet<int32>& LockedDoors,
	const TArray<FRoomSpaceSyntax>& PerRoom,
	const FSpaceSyntaxScores& Scores,
	float HorrorLevel, int32 WrongRoomCount)
{
	// Add locked status to each door in the doors array
	if (LockedDoors.Num() > 0)
	{
		const TArray<TSharedPtr<FJsonValue>>* DoorsArr = nullptr;
		if (ResultJson->TryGetArrayField(TEXT("doors"), DoorsArr) && DoorsArr)
		{
			for (int32 i = 0; i < DoorsArr->Num() && i < Doors.Num(); ++i)
			{
				auto DoorObj = (*DoorsArr)[i]->AsObject();
				if (DoorObj.IsValid())
				{
					DoorObj->SetBoolField(TEXT("locked"), LockedDoors.Contains(i));
				}
			}
		}
	}

	// Per-room Space Syntax data
	TArray<TSharedPtr<FJsonValue>> RoomSSArr;
	for (int32 i = 0; i < Rooms.Num() && i < PerRoom.Num(); ++i)
	{
		auto RoomSSObj = MakeShared<FJsonObject>();
		RoomSSObj->SetStringField(TEXT("room_id"), Rooms[i].RoomId);
		RoomSSObj->SetStringField(TEXT("room_type"), Rooms[i].RoomType);
		RoomSSObj->SetNumberField(TEXT("integration"), PerRoom[i].Integration);
		RoomSSObj->SetNumberField(TEXT("connectivity"), PerRoom[i].Connectivity);
		RoomSSObj->SetNumberField(TEXT("depth"), PerRoom[i].Depth);
		RoomSSObj->SetBoolField(TEXT("dead_end"), PerRoom[i].bDeadEnd);
		RoomSSObj->SetNumberField(TEXT("tension"), PerRoom[i].Tension);

		// Privacy zone string
		FString PrivacyStr;
		EPrivacyZone PZ = GetPrivacyZone(Rooms[i].RoomType);
		switch (PZ)
		{
		case EPrivacyZone::PUBLIC: PrivacyStr = TEXT("public"); break;
		case EPrivacyZone::SEMI_PUBLIC: PrivacyStr = TEXT("semi_public"); break;
		case EPrivacyZone::SEMI_PRIVATE: PrivacyStr = TEXT("semi_private"); break;
		case EPrivacyZone::PRIVATE: PrivacyStr = TEXT("private"); break;
		case EPrivacyZone::SERVICE: PrivacyStr = TEXT("service"); break;
		}
		RoomSSObj->SetStringField(TEXT("privacy_zone"), PrivacyStr);

		RoomSSArr.Add(MakeShared<FJsonValueObject>(RoomSSObj));
	}
	ResultJson->SetArrayField(TEXT("room_analysis"), RoomSSArr);

	// Aggregate Space Syntax scores
	auto SSObj = MakeShared<FJsonObject>();
	SSObj->SetNumberField(TEXT("mean_integration"), Scores.MeanIntegration);
	SSObj->SetNumberField(TEXT("max_integration"), Scores.MaxIntegration);
	SSObj->SetNumberField(TEXT("min_integration"), Scores.MinIntegration);
	SSObj->SetNumberField(TEXT("max_depth"), Scores.MaxDepth);
	SSObj->SetNumberField(TEXT("mean_depth"), Scores.MeanDepth);
	SSObj->SetNumberField(TEXT("dead_end_ratio"), Scores.DeadEndRatio);
	SSObj->SetNumberField(TEXT("hub_count"), Scores.HubCount);
	ResultJson->SetObjectField(TEXT("space_syntax"), SSObj);

	// Horror stats (in the existing stats object)
	auto StatsObj = ResultJson->GetObjectField(TEXT("stats"));
	if (StatsObj.IsValid())
	{
		StatsObj->SetNumberField(TEXT("horror_level"), HorrorLevel);
		StatsObj->SetNumberField(TEXT("horror_score"), Scores.HorrorScore);
		StatsObj->SetNumberField(TEXT("locked_doors"), Scores.LockedDoorCount);
		StatsObj->SetNumberField(TEXT("locked_door_ratio"), Scores.LockedDoorRatio);
		StatsObj->SetNumberField(TEXT("dead_end_ratio"), Scores.DeadEndRatio);
		StatsObj->SetNumberField(TEXT("wrong_room_count"), WrongRoomCount);
	}
}
