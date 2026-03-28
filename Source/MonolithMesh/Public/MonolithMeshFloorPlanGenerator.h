#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
#include "MonolithMeshBuildingTypes.h"

/**
 * SP2: Automatic Floor Plan Generator
 *
 * Given a building archetype + footprint dimensions, generates a grid + rooms + doors
 * that feed directly into SP1's create_building_from_grid.
 *
 * Algorithm: Graph-based topology -> Squarified treemap layout -> Corridor insertion ->
 *            Adjacency validation -> Privacy gradient enforcement -> Door placement
 *
 * WP-2 additions:
 *   - Adjacency MUST_NOT enforcement (post-placement swap pass)
 *   - Public-to-private gradient (BFS depth from entry)
 *   - Wet wall clustering (plumbing chase scoring)
 *   - Circulation patterns: double_loaded, hub_spoke, racetrack, enfilade
 *
 * WP-6 additions:
 *   - Horror modifier post-processing (door locking, dead-end control, loop breaking, wrong-room injection)
 *   - Space Syntax scoring (integration, connectivity, depth)
 *   - Tension curve metadata per room
 *   - Hospice safety caps on horror features
 *
 * 3 actions:
 *   - generate_floor_plan: Full pipeline from archetype to grid/rooms/doors
 *   - list_building_archetypes: List available archetype JSON files
 *   - get_building_archetype: Return a specific archetype's JSON definition
 *
 * No GeometryScript dependency -- this is pure layout math that outputs data for SP1.
 */
class FMonolithMeshFloorPlanGenerator
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	/** Get the roof type for an archetype (public accessor for orchestrator) */
	static FString GetArchetypeRoofType(const FString& ArchetypeName);

private:
	// ---- Action handlers ----
	static FMonolithActionResult GenerateFloorPlan(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListBuildingArchetypes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetBuildingArchetype(const TSharedPtr<FJsonObject>& Params);

	// ---- WP-2: Adjacency Matrix types (defined here per R1-I2: no BuildingTypes.h changes) ----

	/** Adjacency relationship between room types */
	enum class EAdjacencyRelation : uint8
	{
		MUST,       // Rooms MUST share a wall
		SHOULD,     // Strong preference for adjacency
		MAY,        // Default -- no constraint
		MAY_NOT,    // Soft avoidance
		MUST_NOT    // Rooms MUST NOT share a wall
	};

	/** Privacy zone for public-to-private gradient enforcement */
	enum class EPrivacyZone : uint8
	{
		PUBLIC,        // entry, foyer, lobby, living_room, reception, vestibule, waiting_room, nave
		SEMI_PUBLIC,   // dining_room, family_room, dining_area, bar, fellowship_hall, waiting_area
		SEMI_PRIVATE,  // hallway, corridor, kitchen, break_room, nurse_station, copy_room
		PRIVATE,       // bedroom, bathroom, office, master_bedroom, interrogation, exam_room
		SERVICE        // laundry, utility, storage, closet, garage, janitor_closet, server_room, mechanical
	};

	/** Circulation pattern for a building */
	enum class ECirculationType : uint8
	{
		DoubleLoaded,  // Standard: corridor with rooms on both sides (default)
		HubSpoke,      // Central hub room with rooms radiating off it (residential, <8 rooms)
		Racetrack,     // Loop corridor around a central core (office, commercial)
		Enfilade       // Rooms connect sequentially through aligned doors (mansion wings, horror)
	};

	/** Adjacency matrix: maps (RoomTypeA, RoomTypeB) -> EAdjacencyRelation */
	struct FAdjacencyMatrix
	{
		TMap<FString, TMap<FString, EAdjacencyRelation>> Rules;

		/** Get the rule for a pair of room types. Returns MAY if no rule defined. */
		EAdjacencyRelation GetRule(const FString& TypeA, const FString& TypeB) const;

		/** Returns true if TypeA and TypeB being adjacent violates a MUST_NOT rule */
		bool ViolatesMustNot(const FString& TypeA, const FString& TypeB) const;
	};

	// ---- Archetype types ----

	/** A room type definition from an archetype */
	struct FArchetypeRoom
	{
		FString Type;
		float MinArea = 10.0f;
		float MaxArea = 30.0f;
		int32 CountMin = 1;
		int32 CountMax = 1;
		bool bRequired = true;
		int32 Priority = 5;
		bool bAutoGenerate = false;   // For corridor type
		bool bExteriorWall = false;   // Prefers exterior placement

		// Per-floor assignment: "ground", "upper", "every", "any" (default)
		FString Floor = TEXT("any");

		// Aspect ratio constraints
		float MinAspect = 1.0f;       // Minimum width/height ratio (e.g. 1.0 = square OK)
		float MaxAspect = 3.0f;       // Maximum (prevents 1x20 rooms)
	};

	/** An adjacency constraint from an archetype (legacy array format, still parsed) */
	struct FAdjacencyRule
	{
		FString From;
		FString To;
		FString Strength;  // "required", "strong", "preferred", "weak"
	};

	/** Material hints for future material assignment */
	struct FMaterialHints
	{
		FString Exterior;
		FString Interior;
		FString FloorMaterial;   // "Floor" conflicts with floor index
	};

public:
	/** A complete loaded archetype (public for CityBlock orchestrator) */
	struct FBuildingArchetype
	{
		FString Name;
		FString Description;
		TArray<FArchetypeRoom> Rooms;
		TArray<FAdjacencyRule> Adjacency;
		FAdjacencyMatrix AdjacencyMatrix;  // WP-2: MUST/MUST_NOT matrix from JSON
		ECirculationType Circulation = ECirculationType::DoubleLoaded;  // WP-2
		int32 FloorsMin = 1;
		int32 FloorsMax = 1;
		FString RoofType;
		float FloorHeight = 270.0f;      // Default floor height in cm
		FMaterialHints MaterialHints;
	};

	/** A resolved room instance (after rolling counts from archetype ranges) */
	struct FRoomInstance
	{
		FString RoomId;       // e.g. "bedroom_1", "kitchen"
		FString RoomType;     // e.g. "bedroom", "kitchen"
		float TargetArea;     // In grid cells
		int32 Priority;
		bool bExteriorWall;
		FString Floor;        // "ground", "upper", "every", "any"
		float MinAspect = 1.0f;
		float MaxAspect = 3.0f;
	};

	/** A rectangle in grid space produced by treemap layout */
	struct FGridRect
	{
		int32 X = 0;
		int32 Y = 0;
		int32 W = 0;
		int32 H = 0;

		int32 Area() const { return W * H; }
		float AspectRatio() const { return (W > 0 && H > 0) ? FMath::Max((float)W / H, (float)H / W) : 999.0f; }
	};

	// ---- File I/O ----

	/** Get the archetype directory path */
	static FString GetArchetypeDirectory();

	/** Get the floor plan template directory path */
	static FString GetTemplateDirectory();

	/** Load an archetype from a JSON file (public for CityBlock orchestrator) */
	static bool LoadArchetype(const FString& ArchetypeName, FBuildingArchetype& OutArchetype, FString& OutError);

	// ---- Template system (WP-A) ----

	/** Load a floor plan template JSON from disk.
	 *  Reconstructs FRoomDef::GridCells by scanning the grid (R1-C1: grid is source of truth).
	 *  Validates door edge coordinates: EdgeStart.X == EdgeEnd.X || EdgeStart.Y == EdgeEnd.Y. */
	static bool LoadFloorPlanTemplate(const FString& TemplateName, const FString& Category,
		TArray<TArray<int32>>& OutGrid, int32& OutGridW, int32& OutGridH,
		TArray<FRoomDef>& OutRooms, TArray<FDoorDef>& OutDoors,
		TArray<FStairwellDef>& OutStairwells, FString& OutError);

	/** Select a template from the given category that fits the requested footprint.
	 *  Returns template name (empty on failure). Weighted random from top candidates. */
	static FString SelectTemplate(const FString& Category, float FootprintW, float FootprintH,
		FRandomStream& Rng, FString& OutError);

	/** Scale a template grid to fit a different footprint size.
	 *  Uses nearest-neighbor cell scaling. Recomputes door positions and room cells.
	 *  Post-scaling validation: corridors >= 3 cells wide, stairwells >= 4x6, entrance on exterior edge. */
	static bool ScaleTemplateGrid(
		TArray<TArray<int32>>& InOutGrid, int32& InOutGridW, int32& InOutGridH,
		TArray<FRoomDef>& InOutRooms, TArray<FDoorDef>& InOutDoors,
		TArray<FStairwellDef>& InOutStairwells,
		int32 TargetGridW, int32 TargetGridH,
		FString& OutError);

	/** Map archetype name to template category */
	static FString ArchetypeToTemplateCategory(const FString& ArchetypeName);

private:
	/** Parse a JSON object into an archetype struct */
	static bool ParseArchetypeJson(const TSharedPtr<FJsonObject>& Json, FBuildingArchetype& OutArchetype, FString& OutError);

	// ---- Room resolution ----

	/** Resolve archetype room definitions into concrete room instances using a seed.
	 *  FloorIndex controls per-floor filtering: 0 = ground, 1+ = upper, -1 = all floors (legacy behavior). */
	static TArray<FRoomInstance> ResolveRoomInstances(const FBuildingArchetype& Archetype, int32 GridW, int32 GridH, FRandomStream& Rng, int32 FloorIndex = -1);

	/** Validate that the footprint can fit all required rooms. Returns empty string on success, error message on failure. */
	static FString ValidateFootprintCapacity(const FBuildingArchetype& Archetype, int32 GridW, int32 GridH, int32 FloorIndex = -1);

	/** Validate that multi-floor archetypes have stairwell entries. Returns empty string on success, error message on failure. */
	static FString ValidateStairwellRequirement(const FBuildingArchetype& Archetype);

	/** Post-layout aspect ratio correction: tries to reshape rooms that exceed their max_aspect */
	static void CorrectAspectRatios(TArray<FGridRect>& Rects, const TArray<FRoomInstance>& Rooms, int32 GridW, int32 GridH);

	// ---- Squarified treemap ----

	/** Run squarified treemap layout to pack rooms into the footprint */
	static TArray<FGridRect> SquarifiedTreemapLayout(TArray<FRoomInstance>& Rooms, int32 GridW, int32 GridH);

	/** Internal: layout a single row of rooms within a rectangle */
	static void LayoutRow(const TArray<int32>& RowIndices, const TArray<float>& Areas, FGridRect& Rect,
		bool bHorizontal, TArray<FGridRect>& OutRects);

	/** Calculate the worst aspect ratio if we lay out the given areas in a row against the given length */
	static float WorstAspectRatio(const TArray<float>& RowAreas, float SideLength);

	// ---- WP-2: Privacy zone utilities ----

	/** Map a room type string to its default privacy zone */
	static EPrivacyZone GetPrivacyZone(const FString& RoomType);

	/** Compute BFS depth from entry room for all rooms. Returns map RoomIndex -> Depth. */
	static TMap<int32, int32> ComputeGraphDepthFromEntry(const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		const TArray<FRoomDef>& Rooms);

	/** Validate privacy gradient: PRIVATE rooms must be >= 2 steps from entry. Returns violation count. */
	static int32 ValidatePrivacyGradient(const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		const TArray<FRoomDef>& Rooms, TArray<FString>& OutViolations);

	// ---- WP-2: Adjacency validation ----

	/** Post-placement adjacency validation. Attempts room swaps to fix MUST_NOT violations.
	 *  Returns number of unresolved violations. */
	static int32 ValidateAndFixAdjacency(TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		TArray<FRoomDef>& Rooms, const FAdjacencyMatrix& Matrix, int32 MaxRetries = 3);

	/** Check all MUST_NOT violations in the current layout. Returns list of violating room pairs. */
	static TArray<TPair<int32, int32>> FindMustNotViolations(const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		const TArray<FRoomDef>& Rooms, const FAdjacencyMatrix& Matrix);

	/** Check all unmet MUST requirements. Returns list of room pairs that must be adjacent but aren't. */
	static TArray<TPair<int32, int32>> FindUnmetMustRequirements(const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		const TArray<FRoomDef>& Rooms, const FAdjacencyMatrix& Matrix);

	/** Attempt to swap two rooms in the grid to resolve an adjacency violation */
	static bool TrySwapRooms(TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		TArray<FRoomDef>& Rooms, int32 RoomIdxA, int32 RoomIdxB);

	// ---- WP-2: Wet wall clustering ----

	/** Track plumbing chase cells and compute wet-room placement bonus */
	static TSet<FIntPoint> BuildPlumbingChaseSet(const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		const TArray<FRoomDef>& Rooms);

	/** Returns true if the room type represents a "wet room" (needs plumbing) */
	static bool IsWetRoom(const FString& RoomType);

	// ---- WP-2: Circulation patterns ----

	/** Insert corridors using hub-and-spoke pattern: central hub with rooms radiating off it */
	static void InsertCorridorsHubSpoke(TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		TArray<FRoomDef>& Rooms, const TArray<FAdjacencyRule>& Adjacency,
		bool bHospiceMode, FRandomStream& Rng);

	/** Insert corridors using racetrack pattern: loop corridor around central core */
	static void InsertCorridorsRacetrack(TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		TArray<FRoomDef>& Rooms, const TArray<FAdjacencyRule>& Adjacency,
		bool bHospiceMode, FRandomStream& Rng);

	/** Insert corridors using enfilade pattern: sequential room connections, no corridor */
	static void InsertCorridorsEnfilade(TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		TArray<FRoomDef>& Rooms, const TArray<FAdjacencyRule>& Adjacency,
		bool bHospiceMode, FRandomStream& Rng);

	/** Parse circulation type string from archetype JSON */
	static ECirculationType ParseCirculationType(const FString& TypeStr);

	// ---- Corridor insertion ----

	/** Insert corridor cells where rooms need connectivity but don't share edges (double-loaded, default) */
	static void InsertCorridors(TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		TArray<FRoomDef>& Rooms, const TArray<FAdjacencyRule>& Adjacency,
		bool bHospiceMode, FRandomStream& Rng);

	/** Dispatch to the correct corridor insertion based on circulation type */
	static void InsertCorridorsForCirculation(ECirculationType Circulation,
		TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		TArray<FRoomDef>& Rooms, const TArray<FAdjacencyRule>& Adjacency,
		bool bHospiceMode, FRandomStream& Rng);

	/** Check if two rooms share at least one grid edge */
	static bool RoomsShareEdge(const FRoomDef& A, const FRoomDef& B);

	/** Find a path between two rooms through empty or existing corridor cells using BFS */
	static TArray<FIntPoint> FindCorridorPath(const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		const FRoomDef& From, const FRoomDef& To, int32 CorridorRoomIndex, int32 CorridorWidth);

	// ---- Door placement ----

	/** Place doors at room boundaries based on adjacency graph */
	static TArray<FDoorDef> PlaceDoors(const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		const TArray<FRoomDef>& Rooms, const TArray<FAdjacencyRule>& Adjacency,
		bool bHospiceMode, FRandomStream& Rng);

	/** Find shared edge cells between two rooms */
	static TArray<FIntPoint> FindSharedEdge(const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		int32 RoomIndexA, int32 RoomIndexB);

	// ---- Hospice mode ----

	/** Insert rest alcoves per hospice requirements */
	static void InsertRestAlcoves(TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		TArray<FRoomDef>& Rooms, int32 RoomsBetweenAlcoves, FRandomStream& Rng);

	// ---- Grid utilities ----

	/** Convert room rects to a populated 2D grid */
	static TArray<TArray<int32>> BuildGridFromRects(const TArray<FGridRect>& Rects, int32 GridW, int32 GridH);

	/** Build FRoomDef array from grid state */
	static TArray<FRoomDef> BuildRoomDefs(const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		const TArray<FRoomInstance>& Instances);

	/** Convert grid + rooms + doors to the JSON output format compatible with create_building_from_grid */
	static TSharedPtr<FJsonObject> BuildOutputJson(const TArray<TArray<int32>>& Grid, int32 GridW, int32 GridH,
		const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
		const FString& ArchetypeName, float FootprintWidth, float FootprintHeight,
		bool bHospiceMode, float CellSize);

	// ============================================================================
	// WP-6: Horror Subversion + Space Syntax
	// ============================================================================

	// ---- Horror modifier types ----

	/** Per-room Space Syntax metrics and tension data */
	struct FRoomSpaceSyntax
	{
		float Integration = 0.0f;     // 1 / mean_shortest_path (higher = more connected)
		int32 Connectivity = 0;       // Direct neighbor count (door count)
		int32 Depth = 0;              // BFS depth from entry room
		bool bDeadEnd = false;        // Only 1 traversable exit
		float Tension = 0.0f;         // Composite tension value 0-1
	};

	/** Aggregate Space Syntax scores for the whole floor plan */
	struct FSpaceSyntaxScores
	{
		float MeanIntegration = 0.0f;
		float MaxIntegration = 0.0f;
		float MinIntegration = 0.0f;
		int32 MaxDepth = 0;
		float MeanDepth = 0.0f;
		float DeadEndRatio = 0.0f;
		int32 HubCount = 0;          // Rooms with connectivity >= 3
		float HorrorScore = 0.0f;    // Composite horror fitness 0-1
		float LockedDoorRatio = 0.0f;
		int32 LockedDoorCount = 0;
	};

	// ---- Room adjacency graph helpers ----

	/** Build adjacency list from doors: RoomIndex -> set of connected RoomIndices.
	 *  If LockedDoors is provided, doors at those indices are excluded from the graph. */
	static TArray<TSet<int32>> BuildRoomAdjacencyFromDoors(
		const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
		const TSet<int32>* LockedDoors = nullptr);

	/** Map room ID string to room index */
	static int32 FindRoomIndexById(const TArray<FRoomDef>& Rooms, const FString& RoomId);

	/** BFS shortest path length between two rooms in adjacency graph. Returns -1 if unreachable. */
	static int32 BFSShortestPath(const TArray<TSet<int32>>& Adj, int32 From, int32 To);

	/** Check if the room graph is fully connected (all rooms reachable from room 0) */
	static bool IsGraphConnected(const TArray<TSet<int32>>& Adj, int32 NumRooms);

	/** Find bridge edges using Tarjan's algorithm.
	 *  Returns set of door indices where removing that door would disconnect the graph. */
	static TSet<int32> FindBridgeDoors(
		const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
		const TSet<int32>& AlreadyLocked);

	// ---- Horror modifiers ----

	/** Apply horror door locking. Returns set of locked door indices.
	 *  Never locks bridge doors (Tarjan check). Prefers locking private room doors. */
	static TSet<int32> ApplyDoorLocking(
		const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
		float LockRatio, FRandomStream& Rng);

	/** Adjust dead-end ratio by locking additional non-bridge doors.
	 *  Returns updated set of locked door indices. */
	static TSet<int32> AdjustDeadEndRatio(
		const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
		TSet<int32> LockedDoors, float TargetRatio, FRandomStream& Rng);

	/** Break loops by locking one non-bridge door per cycle to force longer traversal.
	 *  Returns updated set of locked door indices. */
	static TSet<int32> ApplyLoopBreaking(
		const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
		TSet<int32> LockedDoors, FRandomStream& Rng);

	/** Inject wrong-room types: change 1-2 room types to unexpected types.
	 *  Modifies Rooms in place. Returns count of rooms changed. */
	static int32 ApplyWrongRoomInjection(
		TArray<FRoomDef>& Rooms, FRandomStream& Rng);

	/** Top-level horror post-processing pass. Applies all horror modifiers based on horror_level.
	 *  Returns the set of locked door indices and fills SpaceSyntax per-room data. */
	static TSet<int32> ApplyHorrorModifiers(
		TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
		float HorrorLevel, bool bHospiceMode, FRandomStream& Rng,
		TArray<FRoomSpaceSyntax>& OutPerRoom, FSpaceSyntaxScores& OutScores);

	// ---- Space Syntax computation ----

	/** Compute Space Syntax metrics for all rooms.
	 *  LockedDoors: doors excluded from traversal graph. */
	static void ComputeSpaceSyntax(
		const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
		const TSet<int32>& LockedDoors,
		TArray<FRoomSpaceSyntax>& OutPerRoom, FSpaceSyntaxScores& OutScores);

	/** Compute tension value for a single room based on its Space Syntax metrics */
	static float ComputeRoomTension(const FRoomSpaceSyntax& RoomSS, float MeanDepth, int32 Depth);

	/** Emit Space Syntax and horror data into the output JSON */
	static void EmitHorrorAndSpaceSyntaxJson(
		TSharedPtr<FJsonObject>& ResultJson,
		const TArray<FRoomDef>& Rooms, const TArray<FDoorDef>& Doors,
		const TSet<int32>& LockedDoors,
		const TArray<FRoomSpaceSyntax>& PerRoom,
		const FSpaceSyntaxScores& Scores,
		float HorrorLevel, int32 WrongRoomCount);
};
