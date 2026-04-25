#include "MonolithAINavigationActions.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavModifierVolume.h"
#include "Navigation/NavLinkProxy.h"
#include "NavAreas/NavArea.h"
#include "AI/Navigation/NavigationTypes.h"
#include "NavFilters/NavigationQueryFilter.h"
#include "Navigation/CrowdManager.h"

#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Builders/CubeBuilder.h"
#include "ActorFactories/ActorFactory.h"

// ============================================================
//  Helpers
// ============================================================

UWorld* FMonolithAINavigationActions::GetNavWorld()
{
	UWorld* World = MonolithAI::GetPIEWorld();
	if (!World)
	{
		World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}
	return World;
}

FVector FMonolithAINavigationActions::ParseVector(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, bool& bOutFound)
{
	bOutFound = false;
	if (!Params->HasField(FieldName)) return FVector::ZeroVector;

	// Try object: {"x": 100, "y": 200, "z": 0}
	const TSharedPtr<FJsonObject>* VecObj = nullptr;
	if (Params->TryGetObjectField(FieldName, VecObj) && VecObj && (*VecObj)->Values.Num() > 0)
	{
		bOutFound = true;
		return FVector(
			(*VecObj)->GetNumberField(TEXT("x")),
			(*VecObj)->GetNumberField(TEXT("y")),
			(*VecObj)->GetNumberField(TEXT("z")));
	}

	// Try array: [100, 200, 0]
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (Params->TryGetArrayField(FieldName, Arr) && Arr && Arr->Num() >= 3)
	{
		bOutFound = true;
		return FVector(
			(*Arr)[0]->AsNumber(),
			(*Arr)[1]->AsNumber(),
			(*Arr)[2]->AsNumber());
	}

	return FVector::ZeroVector;
}

TArray<TSharedPtr<FJsonValue>> FMonolithAINavigationActions::VectorToJsonArray(const FVector& V)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Add(MakeShared<FJsonValueNumber>(V.X));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
	return Arr;
}

// ============================================================
//  Registration
// ============================================================

void FMonolithAINavigationActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// 143. get_nav_system_config
	Registry.RegisterAction(TEXT("ai"), TEXT("get_nav_system_config"),
		TEXT("Read UNavigationSystemV1 properties: enabled, allow client-side nav, abstract nav data, dirty areas update freq"),
		FMonolithActionHandler::CreateStatic(&HandleGetNavSystemConfig),
		FParamSchemaBuilder().Build());

	// 144. get_navmesh_config
	Registry.RegisterAction(TEXT("ai"), TEXT("get_navmesh_config"),
		TEXT("Full ARecastNavMesh generation params including multi-resolution settings (Low/Default/High), cell size, tile size, agent params"),
		FMonolithActionHandler::CreateStatic(&HandleGetNavMeshConfig),
		FParamSchemaBuilder().Build());

	// 145. set_navmesh_config
	Registry.RegisterAction(TEXT("ai"), TEXT("set_navmesh_config"),
		TEXT("Modify ARecastNavMesh generation params (agent radius/height, cell size, tile size, multi-resolution)"),
		FMonolithActionHandler::CreateStatic(&HandleSetNavMeshConfig),
		FParamSchemaBuilder()
			.Optional(TEXT("agent_radius"), TEXT("number"), TEXT("Agent radius (cm)"))
			.Optional(TEXT("agent_height"), TEXT("number"), TEXT("Agent height (cm)"))
			.Optional(TEXT("cell_size"), TEXT("number"), TEXT("Cell size (cm) — smaller = more accurate but slower"))
			.Optional(TEXT("cell_height"), TEXT("number"), TEXT("Cell height (cm)"))
			.Optional(TEXT("tile_size"), TEXT("number"), TEXT("Tile size in cells"))
			.Optional(TEXT("agent_max_slope"), TEXT("number"), TEXT("Max walkable slope (degrees)"))
			.Optional(TEXT("agent_max_step_height"), TEXT("number"), TEXT("Max step height (cm)"))
			.Optional(TEXT("resolution_params"), TEXT("array"), TEXT("Array of 3 resolution param objects [{cell_size, cell_height, agent_max_step_height}] for Low/Default/High"))
			.Build());

	// 146. get_navmesh_stats
	Registry.RegisterAction(TEXT("ai"), TEXT("get_navmesh_stats"),
		TEXT("NavMesh statistics: tile count, nav data size, build status, registered nav bounds count"),
		FMonolithActionHandler::CreateStatic(&HandleGetNavMeshStats),
		FParamSchemaBuilder().Build());

	// 147. add_nav_bounds_volume
	Registry.RegisterAction(TEXT("ai"), TEXT("add_nav_bounds_volume"),
		TEXT("Spawn ANavMeshBoundsVolume at a location with given extents"),
		FMonolithActionHandler::CreateStatic(&HandleAddNavBoundsVolume),
		FParamSchemaBuilder()
			.Required(TEXT("location"), TEXT("object"), TEXT("World position {x, y, z} or [x, y, z]"))
			.Required(TEXT("extent"), TEXT("object"), TEXT("Half-extents {x, y, z} or [x, y, z]"))
			.Optional(TEXT("folder_path"), TEXT("string"), TEXT("Outliner folder path (default: AI/Navigation)"))
			.Build());

	// 148. list_nav_bounds_volumes
	Registry.RegisterAction(TEXT("ai"), TEXT("list_nav_bounds_volumes"),
		TEXT("Enumerate all ANavMeshBoundsVolume actors in the level"),
		FMonolithActionHandler::CreateStatic(&HandleListNavBoundsVolumes),
		FParamSchemaBuilder().Build());

	// 149. build_navigation
	Registry.RegisterAction(TEXT("ai"), TEXT("build_navigation"),
		TEXT("Trigger UNavigationSystemV1::Build() — kicks off navmesh generation and returns immediately"),
		FMonolithActionHandler::CreateStatic(&HandleBuildNavigation),
		FParamSchemaBuilder().Build());

	// 150. get_nav_build_status
	Registry.RegisterAction(TEXT("ai"), TEXT("get_nav_build_status"),
		TEXT("Check navigation build status: is building, tiles remaining, locked state"),
		FMonolithActionHandler::CreateStatic(&HandleGetNavBuildStatus),
		FParamSchemaBuilder().Build());

	// 151. list_nav_areas
	Registry.RegisterAction(TEXT("ai"), TEXT("list_nav_areas"),
		TEXT("List all UNavArea subclasses with their costs, colors, and flags"),
		FMonolithActionHandler::CreateStatic(&HandleListNavAreas),
		FParamSchemaBuilder().Build());

	// 152. create_nav_area
	Registry.RegisterAction(TEXT("ai"), TEXT("create_nav_area"),
		TEXT("Create a new UNavArea Blueprint with custom cost and color"),
		FMonolithActionHandler::CreateStatic(&HandleCreateNavArea),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Package path, e.g. /Game/AI/NavArea_Water"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Asset name"))
			.Optional(TEXT("default_cost"), TEXT("number"), TEXT("Default traversal cost (default: 1.0)"))
			.Optional(TEXT("fixed_area_entering_cost"), TEXT("number"), TEXT("Fixed cost applied once when entering the area (default: 0.0)"))
			.Optional(TEXT("color"), TEXT("object"), TEXT("Drawing color {r, g, b, a} in 0-255 range"))
			.Build());

	// 153. add_nav_modifier_volume
	Registry.RegisterAction(TEXT("ai"), TEXT("add_nav_modifier_volume"),
		TEXT("Spawn ANavModifierVolume that overrides the nav area class within its bounds"),
		FMonolithActionHandler::CreateStatic(&HandleAddNavModifierVolume),
		FParamSchemaBuilder()
			.Required(TEXT("location"), TEXT("object"), TEXT("World position {x, y, z} or [x, y, z]"))
			.Required(TEXT("extent"), TEXT("object"), TEXT("Half-extents {x, y, z} or [x, y, z]"))
			.Required(TEXT("area_class"), TEXT("string"), TEXT("Nav area class name (e.g. NavArea_Null, NavArea_Obstacle, or custom)"))
			.Optional(TEXT("folder_path"), TEXT("string"), TEXT("Outliner folder path (default: AI/Navigation)"))
			.Build());

	// 154. add_nav_link_proxy
	Registry.RegisterAction(TEXT("ai"), TEXT("add_nav_link_proxy"),
		TEXT("Spawn ANavLinkProxy connecting two navigation points for jumps, drops, etc."),
		FMonolithActionHandler::CreateStatic(&HandleAddNavLinkProxy),
		FParamSchemaBuilder()
			.Required(TEXT("start_location"), TEXT("object"), TEXT("Start point {x, y, z} or [x, y, z]"))
			.Required(TEXT("end_location"), TEXT("object"), TEXT("End point {x, y, z} or [x, y, z]"))
			.Optional(TEXT("link_type"), TEXT("string"), TEXT("'point' (default) or 'smart'"))
			.Optional(TEXT("area_class"), TEXT("string"), TEXT("Nav area class for the link"))
			.Optional(TEXT("folder_path"), TEXT("string"), TEXT("Outliner folder path (default: AI/Navigation)"))
			.Build());

	// 155. configure_nav_link
	Registry.RegisterAction(TEXT("ai"), TEXT("configure_nav_link"),
		TEXT("Configure an existing ANavLinkProxy: enable/disable, set area class, direction"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureNavLink),
		FParamSchemaBuilder()
			.Required(TEXT("actor_path"), TEXT("string"), TEXT("Actor name, label, or path in the level"))
			.Optional(TEXT("enabled"), TEXT("boolean"), TEXT("Enable or disable the link"))
			.Optional(TEXT("area_class"), TEXT("string"), TEXT("Nav area class for the link"))
			.Optional(TEXT("direction"), TEXT("string"), TEXT("'both' (bidirectional), 'left_to_right', or 'right_to_left'"))
			.Build());

	// 156. list_nav_links
	Registry.RegisterAction(TEXT("ai"), TEXT("list_nav_links"),
		TEXT("Enumerate all ANavLinkProxy actors in the level"),
		FMonolithActionHandler::CreateStatic(&HandleListNavLinks),
		FParamSchemaBuilder()
			.Optional(TEXT("level"), TEXT("string"), TEXT("Filter by sublevel name"))
			.Build());

	// 157. find_path
	Registry.RegisterAction(TEXT("ai"), TEXT("find_path"),
		TEXT("FindPathSync between two points — returns path points, total cost, total distance"),
		FMonolithActionHandler::CreateStatic(&HandleFindPath),
		FParamSchemaBuilder()
			.Required(TEXT("start"), TEXT("object"), TEXT("Start position {x, y, z} or [x, y, z]"))
			.Required(TEXT("end"), TEXT("object"), TEXT("End position {x, y, z} or [x, y, z]"))
			.Build());

	// 158. test_path
	Registry.RegisterAction(TEXT("ai"), TEXT("test_path"),
		TEXT("Fast reachability test between two nav points — returns bool"),
		FMonolithActionHandler::CreateStatic(&HandleTestPath),
		FParamSchemaBuilder()
			.Required(TEXT("start"), TEXT("object"), TEXT("Start position {x, y, z} or [x, y, z]"))
			.Required(TEXT("end"), TEXT("object"), TEXT("End position {x, y, z} or [x, y, z]"))
			.Build());

	// 159. project_point_to_navigation
	Registry.RegisterAction(TEXT("ai"), TEXT("project_point_to_navigation"),
		TEXT("Project a world point onto the navmesh surface — returns nearest navigable location"),
		FMonolithActionHandler::CreateStatic(&HandleProjectPointToNavigation),
		FParamSchemaBuilder()
			.Required(TEXT("point"), TEXT("object"), TEXT("World position {x, y, z} or [x, y, z]"))
			.Optional(TEXT("extent"), TEXT("object"), TEXT("Search extent {x, y, z} or [x, y, z] (default: 50,50,250)"))
			.Build());

	// 160. get_random_navigable_point
	Registry.RegisterAction(TEXT("ai"), TEXT("get_random_navigable_point"),
		TEXT("Get a random navigable point, optionally within a radius of an origin"),
		FMonolithActionHandler::CreateStatic(&HandleGetRandomNavigablePoint),
		FParamSchemaBuilder()
			.Optional(TEXT("origin"), TEXT("object"), TEXT("Center point {x, y, z} or [x, y, z]"))
			.Optional(TEXT("radius"), TEXT("number"), TEXT("Search radius (cm) — required if origin is provided"))
			.Build());

	// 161. navigation_raycast
	Registry.RegisterAction(TEXT("ai"), TEXT("navigation_raycast"),
		TEXT("NavMesh line-of-sight test — returns whether the ray hits a nav boundary"),
		FMonolithActionHandler::CreateStatic(&HandleNavigationRaycast),
		FParamSchemaBuilder()
			.Required(TEXT("start"), TEXT("object"), TEXT("Ray start {x, y, z} or [x, y, z]"))
			.Required(TEXT("end"), TEXT("object"), TEXT("Ray end {x, y, z} or [x, y, z]"))
			.Build());

	// 162. configure_nav_agent
	Registry.RegisterAction(TEXT("ai"), TEXT("configure_nav_agent"),
		TEXT("Modify a supported nav agent's properties (radius, height, step height)"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureNavAgent),
		FParamSchemaBuilder()
			.Required(TEXT("agent_index"), TEXT("number"), TEXT("Index into SupportedAgents array (0 = default agent)"))
			.Optional(TEXT("radius"), TEXT("number"), TEXT("Agent radius (cm)"))
			.Optional(TEXT("height"), TEXT("number"), TEXT("Agent height (cm)"))
			.Optional(TEXT("step_height"), TEXT("number"), TEXT("Max step height (cm)"))
			.Build());

	// 163. add_nav_invoker_component
	Registry.RegisterAction(TEXT("ai"), TEXT("add_nav_invoker_component"),
		TEXT("Add UNavigationInvokerComponent to a Blueprint for dynamic nav generation around the actor"),
		FMonolithActionHandler::CreateStatic(&HandleAddNavInvokerComponent),
		FParamSchemaBuilder()
			.Required(TEXT("blueprint_path"), TEXT("string"), TEXT("Actor Blueprint asset path"))
			.Optional(TEXT("generation_radius"), TEXT("number"), TEXT("Nav generation radius (cm, default: 3000)"))
			.Optional(TEXT("removal_radius"), TEXT("number"), TEXT("Nav removal radius (cm, default: 5000)"))
			.Build());

	// 164. get_crowd_manager_config
	Registry.RegisterAction(TEXT("ai"), TEXT("get_crowd_manager_config"),
		TEXT("Read UCrowdManager settings: max agents, max avoidance agents, avoidance config"),
		FMonolithActionHandler::CreateStatic(&HandleGetCrowdManagerConfig),
		FParamSchemaBuilder().Build());

	// 165. set_crowd_manager_config
	Registry.RegisterAction(TEXT("ai"), TEXT("set_crowd_manager_config"),
		TEXT("Modify UCrowdManager settings: max agents, max agent radius, avoidance counts, intervals, separation, collision resolution"),
		FMonolithActionHandler::CreateStatic(&HandleSetCrowdManagerConfig),
		FParamSchemaBuilder()
			.Optional(TEXT("max_agents"), TEXT("number"), TEXT("Maximum number of crowd agents"))
			.Optional(TEXT("max_avoidance_agents"), TEXT("number"), TEXT("Max agents doing avoidance"), { TEXT("max_avoided_agents") })
			.Optional(TEXT("max_agent_radius"), TEXT("number"), TEXT("Max radius (cm) of an agent that can be added to the crowd"), { TEXT("MaxAgentRadius") })
			.Optional(TEXT("max_avoided_walls"), TEXT("number"), TEXT("Max number of wall segments considered for velocity avoidance"), { TEXT("max_avoidance_walls"), TEXT("MaxAvoidedWalls") })
			.Optional(TEXT("navmesh_check_interval"), TEXT("number"), TEXT("Seconds between agents re-projecting onto the navmesh"), { TEXT("NavmeshCheckInterval") })
			.Optional(TEXT("path_optimization_interval"), TEXT("number"), TEXT("Seconds between path-optimization passes"), { TEXT("PathOptimizationInterval") })
			.Optional(TEXT("separation_dir_clamp"), TEXT("number"), TEXT("Dot threshold (-1..1) for clamping rear-neighbor separation force, -1 = disabled"), { TEXT("SeparationDirClamp") })
			.Optional(TEXT("path_offset_radius_multiplier"), TEXT("number"), TEXT("Agent-radius multiplier used to offset paths around corners"), { TEXT("PathOffsetRadiusMultiplier") })
			.Optional(TEXT("resolve_collisions"), TEXT("boolean"), TEXT("Whether the crowd simulation resolves agent-vs-agent collisions"), { TEXT("bResolveCollisions") })
			.Build());

	// 166. analyze_navigation_coverage
	Registry.RegisterAction(TEXT("ai"), TEXT("analyze_navigation_coverage"),
		TEXT("Grid-sample the level and project points to navmesh — reports coverage percentage, gaps, and stats"),
		FMonolithActionHandler::CreateStatic(&HandleAnalyzeNavigationCoverage),
		FParamSchemaBuilder()
			.Optional(TEXT("sample_spacing"), TEXT("number"), TEXT("Distance between sample points (cm, default: 200)"))
			.Optional(TEXT("bounds"), TEXT("object"), TEXT("Custom bounds {min: [x,y,z], max: [x,y,z]} — defaults to nav bounds"))
			.Build());
}

// ============================================================
//  143. get_nav_system_config
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleGetNavSystemConfig(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return FMonolithActionResult::Error(TEXT("NavigationSystemV1 not found in world"));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("is_navigation_built"), NavSys->IsNavigationBuilt(World->GetWorldSettings()));

	// Read config properties via reflection
	UClass* NavClass = NavSys->GetClass();
	auto ReadBool = [&](const TCHAR* PropName)
	{
		if (FBoolProperty* Prop = CastField<FBoolProperty>(NavClass->FindPropertyByName(PropName)))
		{
			Result->SetBoolField(PropName, Prop->GetPropertyValue_InContainer(NavSys));
		}
	};
	auto ReadFloat = [&](const TCHAR* PropName)
	{
		if (FFloatProperty* Prop = CastField<FFloatProperty>(NavClass->FindPropertyByName(PropName)))
		{
			Result->SetNumberField(PropName, Prop->GetPropertyValue_InContainer(NavSys));
		}
		else if (FDoubleProperty* DProp = CastField<FDoubleProperty>(NavClass->FindPropertyByName(PropName)))
		{
			Result->SetNumberField(PropName, DProp->GetPropertyValue_InContainer(NavSys));
		}
	};

	ReadBool(TEXT("bAllowClientSideNavigation"));
	ReadBool(TEXT("bAutoCreateNavigationData"));
	ReadBool(TEXT("bInitialBuildingLocked"));
	ReadBool(TEXT("bSpawnNavDataInNavBoundsLevel"));
	ReadFloat(TEXT("DirtyAreasUpdateFreq"));

	// Supported agents
	const TArray<FNavDataConfig>& SupportedAgents = NavSys->GetSupportedAgents();
	TArray<TSharedPtr<FJsonValue>> Agents;
	for (int32 i = 0; i < SupportedAgents.Num(); ++i)
	{
		const FNavDataConfig& Config = SupportedAgents[i];
		auto AgentObj = MakeShared<FJsonObject>();
		AgentObj->SetNumberField(TEXT("index"), i);
		AgentObj->SetStringField(TEXT("name"), Config.Name.ToString());
		AgentObj->SetNumberField(TEXT("radius"), Config.AgentRadius);
		AgentObj->SetNumberField(TEXT("height"), Config.AgentHeight);
		AgentObj->SetNumberField(TEXT("step_height"), Config.AgentStepHeight);
		Agents.Add(MakeShared<FJsonValueObject>(AgentObj));
	}
	Result->SetArrayField(TEXT("supported_agents"), Agents);

	// Nav data instances
	TArray<TSharedPtr<FJsonValue>> NavDatas;
	for (ANavigationData* NavData : NavSys->NavDataSet)
	{
		if (!NavData) continue;
		auto NDObj = MakeShared<FJsonObject>();
		NDObj->SetStringField(TEXT("name"), NavData->GetName());
		NDObj->SetStringField(TEXT("class"), NavData->GetClass()->GetName());
		NavDatas.Add(MakeShared<FJsonValueObject>(NDObj));
	}
	Result->SetArrayField(TEXT("nav_data_instances"), NavDatas);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  144. get_navmesh_config
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleGetNavMeshConfig(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return FMonolithActionResult::Error(TEXT("NavigationSystemV1 not found"));
	}

	ARecastNavMesh* NavMesh = nullptr;
	for (ANavigationData* NavData : NavSys->NavDataSet)
	{
		NavMesh = Cast<ARecastNavMesh>(NavData);
		if (NavMesh) break;
	}

	if (!NavMesh)
	{
		return FMonolithActionResult::Error(TEXT("No ARecastNavMesh found. Ensure a NavMeshBoundsVolume exists and nav data has been created."));
	}

	auto Result = MakeShared<FJsonObject>();
	constexpr ENavigationDataResolution LegacyResolution = ENavigationDataResolution::Default;
	Result->SetStringField(TEXT("navmesh_name"), NavMesh->GetName());
	Result->SetNumberField(TEXT("agent_radius"), NavMesh->AgentRadius);
	Result->SetNumberField(TEXT("agent_height"), NavMesh->AgentHeight);
	Result->SetNumberField(TEXT("agent_max_slope"), NavMesh->AgentMaxSlope);
	Result->SetNumberField(TEXT("agent_max_step_height"), NavMesh->GetAgentMaxStepHeight(LegacyResolution));
	Result->SetNumberField(TEXT("cell_size"), NavMesh->GetCellSize(LegacyResolution));
	Result->SetNumberField(TEXT("cell_height"), NavMesh->GetCellHeight(LegacyResolution));
	Result->SetNumberField(TEXT("tile_size_uu"), NavMesh->GetTileSizeUU());
	Result->SetNumberField(TEXT("merge_region_size"), NavMesh->MergeRegionSize);
	Result->SetNumberField(TEXT("min_region_area"), NavMesh->MinRegionArea);
	Result->SetNumberField(TEXT("max_simplification_error"), NavMesh->MaxSimplificationError);

	// Multi-resolution params (Low/Default/High)
	TArray<TSharedPtr<FJsonValue>> ResParams;
	const TCHAR* ResNames[] = { TEXT("Low"), TEXT("Default"), TEXT("High") };
	for (int32 i = 0; i < 3; ++i)
	{
		const ENavigationDataResolution Resolution = static_cast<ENavigationDataResolution>(i);
		auto ResObj = MakeShared<FJsonObject>();
		ResObj->SetStringField(TEXT("resolution"), ResNames[i]);
		ResObj->SetNumberField(TEXT("cell_size"), NavMesh->GetCellSize(Resolution));
		ResObj->SetNumberField(TEXT("cell_height"), NavMesh->GetCellHeight(Resolution));
		ResObj->SetNumberField(TEXT("agent_max_step_height"), NavMesh->GetAgentMaxStepHeight(Resolution));
		ResParams.Add(MakeShared<FJsonValueObject>(ResObj));
	}
	Result->SetArrayField(TEXT("resolution_params"), ResParams);

	// Bool config
	Result->SetBoolField(TEXT("fixed_tile_pool_size"), NavMesh->bFixedTilePoolSize);
	Result->SetBoolField(TEXT("sort_navigation_areas_by_cost"), NavMesh->bSortNavigationAreasByCost);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  145. set_navmesh_config
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleSetNavMeshConfig(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return FMonolithActionResult::Error(TEXT("NavigationSystemV1 not found"));
	}

	ARecastNavMesh* NavMesh = nullptr;
	for (ANavigationData* NavData : NavSys->NavDataSet)
	{
		NavMesh = Cast<ARecastNavMesh>(NavData);
		if (NavMesh) break;
	}

	if (!NavMesh)
	{
		return FMonolithActionResult::Error(TEXT("No ARecastNavMesh found"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Set NavMesh Config")));
	NavMesh->Modify();

	int32 ChangedCount = 0;
	constexpr ENavigationDataResolution LegacyResolution = ENavigationDataResolution::Default;

	if (Params->HasField(TEXT("agent_radius")))
	{
		NavMesh->AgentRadius = Params->GetNumberField(TEXT("agent_radius"));
		ChangedCount++;
	}
	if (Params->HasField(TEXT("agent_height")))
	{
		NavMesh->AgentHeight = Params->GetNumberField(TEXT("agent_height"));
		ChangedCount++;
	}
	if (Params->HasField(TEXT("cell_size")))
	{
		NavMesh->SetCellSize(LegacyResolution, Params->GetNumberField(TEXT("cell_size")));
		ChangedCount++;
	}
	if (Params->HasField(TEXT("cell_height")))
	{
		NavMesh->SetCellHeight(LegacyResolution, Params->GetNumberField(TEXT("cell_height")));
		ChangedCount++;
	}
	if (Params->HasField(TEXT("tile_size")))
	{
		NavMesh->TileSizeUU = Params->GetNumberField(TEXT("tile_size"));
		ChangedCount++;
	}
	if (Params->HasField(TEXT("agent_max_slope")))
	{
		NavMesh->AgentMaxSlope = Params->GetNumberField(TEXT("agent_max_slope"));
		ChangedCount++;
	}
	if (Params->HasField(TEXT("agent_max_step_height")))
	{
		NavMesh->SetAgentMaxStepHeight(LegacyResolution, Params->GetNumberField(TEXT("agent_max_step_height")));
		ChangedCount++;
	}

	// Multi-resolution params
	const TArray<TSharedPtr<FJsonValue>>* ResArr = nullptr;
	if (Params->TryGetArrayField(TEXT("resolution_params"), ResArr) && ResArr)
	{
		for (int32 i = 0; i < FMath::Min(ResArr->Num(), 3); ++i)
		{
			const ENavigationDataResolution Resolution = static_cast<ENavigationDataResolution>(i);
			const TSharedPtr<FJsonObject>* ResObj = nullptr;
			if ((*ResArr)[i]->TryGetObject(ResObj) && ResObj && (*ResObj)->Values.Num() > 0)
			{
				if ((*ResObj)->HasField(TEXT("cell_size")))
				{
					NavMesh->SetCellSize(Resolution, (*ResObj)->GetNumberField(TEXT("cell_size")));
					ChangedCount++;
				}
				if ((*ResObj)->HasField(TEXT("cell_height")))
				{
					NavMesh->SetCellHeight(Resolution, (*ResObj)->GetNumberField(TEXT("cell_height")));
					ChangedCount++;
				}
				if ((*ResObj)->HasField(TEXT("agent_max_step_height")))
				{
					NavMesh->SetAgentMaxStepHeight(Resolution, (*ResObj)->GetNumberField(TEXT("agent_max_step_height")));
					ChangedCount++;
				}
			}
		}
	}

	NavMesh->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("properties_changed"), ChangedCount);
	Result->SetStringField(TEXT("message"),
		FString::Printf(TEXT("Updated %d navmesh properties. Run build_navigation to regenerate."), ChangedCount));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  146. get_navmesh_stats
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleGetNavMeshStats(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return FMonolithActionResult::Error(TEXT("NavigationSystemV1 not found"));
	}

	ARecastNavMesh* NavMesh = nullptr;
	for (ANavigationData* NavData : NavSys->NavDataSet)
	{
		NavMesh = Cast<ARecastNavMesh>(NavData);
		if (NavMesh) break;
	}

	auto Result = MakeShared<FJsonObject>();

	if (NavMesh)
	{
		Result->SetStringField(TEXT("navmesh_name"), NavMesh->GetName());
		Result->SetNumberField(TEXT("tile_count"), NavMesh->GetNavMeshTilesCount());

		// Build status
		Result->SetBoolField(TEXT("is_building"), NavSys->IsNavigationBuildInProgress());
		Result->SetBoolField(TEXT("is_built"), NavSys->IsNavigationBuilt(World->GetWorldSettings()));

		// Dirty state
		Result->SetNumberField(TEXT("dirty_areas_remaining"), NavSys->GetNumRemainingBuildTasks());
	}
	else
	{
		Result->SetStringField(TEXT("message"), TEXT("No RecastNavMesh found — navmesh may not have been built yet"));
		Result->SetBoolField(TEXT("is_building"), NavSys->IsNavigationBuildInProgress());
	}

	// Count nav bounds volumes
	int32 BoundsCount = 0;
	for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
	{
		BoundsCount++;
	}
	Result->SetNumberField(TEXT("nav_bounds_volume_count"), BoundsCount);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  147. add_nav_bounds_volume
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleAddNavBoundsVolume(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	bool bLocFound = false, bExtFound = false;
	FVector Location = ParseVector(Params, TEXT("location"), bLocFound);
	FVector Extent = ParseVector(Params, TEXT("extent"), bExtFound);

	if (!bLocFound)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: location"));
	}
	if (!bExtFound)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: extent"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add NavMeshBoundsVolume")));

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ANavMeshBoundsVolume* NavVol = Cast<ANavMeshBoundsVolume>(
		World->SpawnActor(ANavMeshBoundsVolume::StaticClass(), &Location, nullptr, SpawnParams));

	if (!NavVol)
	{
		return FMonolithActionResult::Error(TEXT("Failed to spawn NavMeshBoundsVolume"));
	}

	// Create brush geometry to define volume size
	UCubeBuilder* CubeBuilder = NewObject<UCubeBuilder>();
	CubeBuilder->X = Extent.X * 2.0;
	CubeBuilder->Y = Extent.Y * 2.0;
	CubeBuilder->Z = Extent.Z * 2.0;
	UActorFactory::CreateBrushForVolumeActor(NavVol, CubeBuilder);

	// Folder
	FString Folder = Params->GetStringField(TEXT("folder_path"));
	NavVol->SetFolderPath(FName(Folder.IsEmpty() ? TEXT("AI/Navigation") : *Folder));
	NavVol->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), NavVol->GetActorNameOrLabel());
	Result->SetArrayField(TEXT("location"), VectorToJsonArray(Location));
	Result->SetArrayField(TEXT("extent"), VectorToJsonArray(Extent));
	Result->SetStringField(TEXT("message"), TEXT("NavMeshBoundsVolume spawned. Run build_navigation to generate navmesh."));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  148. list_nav_bounds_volumes
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleListNavBoundsVolumes(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	TArray<TSharedPtr<FJsonValue>> Volumes;
	for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
	{
		ANavMeshBoundsVolume* Vol = *It;
		auto VolObj = MakeShared<FJsonObject>();
		VolObj->SetStringField(TEXT("name"), Vol->GetActorNameOrLabel());
		VolObj->SetArrayField(TEXT("location"), VectorToJsonArray(Vol->GetActorLocation()));

		FVector Origin, BoxExtent;
		Vol->GetActorBounds(false, Origin, BoxExtent);
		VolObj->SetArrayField(TEXT("extent"), VectorToJsonArray(BoxExtent));
		VolObj->SetStringField(TEXT("level"), Vol->GetLevel()->GetOuter()->GetName());

		Volumes.Add(MakeShared<FJsonValueObject>(VolObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Volumes.Num());
	Result->SetArrayField(TEXT("volumes"), Volumes);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  149. build_navigation
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleBuildNavigation(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return FMonolithActionResult::Error(TEXT("NavigationSystemV1 not found"));
	}

	NavSys->Build();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("message"), TEXT("Navigation build triggered. Use get_nav_build_status to monitor progress."));
	Result->SetBoolField(TEXT("build_started"), true);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  150. get_nav_build_status
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleGetNavBuildStatus(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return FMonolithActionResult::Error(TEXT("NavigationSystemV1 not found"));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("is_building"), NavSys->IsNavigationBuildInProgress());
	Result->SetNumberField(TEXT("remaining_build_tasks"), NavSys->GetNumRemainingBuildTasks());
	Result->SetBoolField(TEXT("is_navigation_build_locked"), NavSys->IsNavigationBuildingLocked());

	// Check each nav data
	TArray<TSharedPtr<FJsonValue>> NavDatas;
	for (ANavigationData* NavData : NavSys->NavDataSet)
	{
		if (!NavData) continue;
		auto NDObj = MakeShared<FJsonObject>();
		NDObj->SetStringField(TEXT("name"), NavData->GetName());
		NDObj->SetBoolField(TEXT("is_built"), NavSys->IsNavigationBuilt(World->GetWorldSettings()));
		NavDatas.Add(MakeShared<FJsonValueObject>(NDObj));
	}
	Result->SetArrayField(TEXT("nav_data"), NavDatas);

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  151. list_nav_areas
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleListNavAreas(const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonValue>> Areas;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (!It->IsChildOf(UNavArea::StaticClass()) || *It == UNavArea::StaticClass())
			continue;
		if (It->HasAnyClassFlags(CLASS_Abstract))
			continue;

		UNavArea* CDO = It->GetDefaultObject<UNavArea>();
		if (!CDO) continue;

		auto AreaObj = MakeShared<FJsonObject>();
		AreaObj->SetStringField(TEXT("class_name"), It->GetName());
		AreaObj->SetStringField(TEXT("class_path"), It->GetPathName());
		AreaObj->SetNumberField(TEXT("default_cost"), CDO->DefaultCost);
		AreaObj->SetNumberField(TEXT("fixed_area_entering_cost"), CDO->GetFixedAreaEnteringCost());

		// Color
		FColor DrawColor = CDO->DrawColor;
		auto ColorObj = MakeShared<FJsonObject>();
		ColorObj->SetNumberField(TEXT("r"), DrawColor.R);
		ColorObj->SetNumberField(TEXT("g"), DrawColor.G);
		ColorObj->SetNumberField(TEXT("b"), DrawColor.B);
		ColorObj->SetNumberField(TEXT("a"), DrawColor.A);
		AreaObj->SetObjectField(TEXT("color"), ColorObj);

		// Supported agents flags
		AreaObj->SetNumberField(TEXT("supported_agents_mask"), CDO->SupportedAgentsBits);

		Areas.Add(MakeShared<FJsonValueObject>(AreaObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Areas.Num());
	Result->SetArrayField(TEXT("areas"), Areas);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  152. create_nav_area
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleCreateNavArea(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath, Name;
	FMonolithActionResult Err;
	if (!MonolithAI::RequireStringParam(Params, TEXT("save_path"), SavePath, Err)) return Err;
	if (!MonolithAI::RequireStringParam(Params, TEXT("name"), Name, Err)) return Err;

	FString Error;
	if (!MonolithAI::EnsureAssetPathFree(SavePath, Name, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	UPackage* Package = MonolithAI::GetOrCreatePackage(SavePath, Error);
	if (!Package)
	{
		return FMonolithActionResult::Error(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Create NavArea")));

	UNavArea* NewArea = NewObject<UNavArea>(Package, *Name, RF_Public | RF_Standalone);
	if (!NewArea)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UNavArea"));
	}

	if (Params->HasField(TEXT("default_cost")))
	{
		NewArea->DefaultCost = Params->GetNumberField(TEXT("default_cost"));
	}

	if (Params->HasField(TEXT("fixed_area_entering_cost")))
	{
		// FixedAreaEnteringCost is protected — set via FProperty reflection
		if (FFloatProperty* CostProp = CastField<FFloatProperty>(UNavArea::StaticClass()->FindPropertyByName(TEXT("FixedAreaEnteringCost"))))
		{
			CostProp->SetPropertyValue_InContainer(NewArea, static_cast<float>(Params->GetNumberField(TEXT("fixed_area_entering_cost"))));
		}
	}

	// Color
	const TSharedPtr<FJsonObject>* ColorObj = nullptr;
	if (Params->TryGetObjectField(TEXT("color"), ColorObj) && ColorObj && (*ColorObj)->Values.Num() > 0)
	{
		NewArea->DrawColor = FColor(
			(uint8)(*ColorObj)->GetNumberField(TEXT("r")),
			(uint8)(*ColorObj)->GetNumberField(TEXT("g")),
			(uint8)(*ColorObj)->GetNumberField(TEXT("b")),
			(*ColorObj)->HasField(TEXT("a")) ? (uint8)(*ColorObj)->GetNumberField(TEXT("a")) : 255
		);
	}

	FAssetRegistryModule::AssetCreated(NewArea);
	Package->MarkPackageDirty();

	auto Result = MonolithAI::MakeAssetResult(SavePath + TEXT(".") + Name, TEXT("NavArea created"));
	Result->SetNumberField(TEXT("default_cost"), NewArea->DefaultCost);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  153. add_nav_modifier_volume
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleAddNavModifierVolume(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	bool bLocFound = false, bExtFound = false;
	FVector Location = ParseVector(Params, TEXT("location"), bLocFound);
	FVector Extent = ParseVector(Params, TEXT("extent"), bExtFound);

	if (!bLocFound) return FMonolithActionResult::Error(TEXT("Missing required parameter: location"));
	if (!bExtFound) return FMonolithActionResult::Error(TEXT("Missing required parameter: extent"));

	FString AreaClassName = Params->GetStringField(TEXT("area_class"));
	if (AreaClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: area_class"));
	}

	// Resolve area class
	UClass* AreaClass = FindFirstObject<UClass>(*AreaClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
	if (!AreaClass)
	{
		// Try with NavArea_ prefix
		AreaClass = FindFirstObject<UClass>(*(TEXT("NavArea_") + AreaClassName), EFindFirstObjectOptions::EnsureIfAmbiguous);
	}
	if (!AreaClass || !AreaClass->IsChildOf(UNavArea::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Nav area class not found: %s"), *AreaClassName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add NavModifierVolume")));

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ANavModifierVolume* ModVol = World->SpawnActor<ANavModifierVolume>(Location, FRotator::ZeroRotator, SpawnParams);
	if (!ModVol)
	{
		return FMonolithActionResult::Error(TEXT("Failed to spawn NavModifierVolume"));
	}

	// Create brush geometry
	UCubeBuilder* CubeBuilder = NewObject<UCubeBuilder>();
	CubeBuilder->X = Extent.X * 2.0;
	CubeBuilder->Y = Extent.Y * 2.0;
	CubeBuilder->Z = Extent.Z * 2.0;
	UActorFactory::CreateBrushForVolumeActor(ModVol, CubeBuilder);

	ModVol->SetAreaClass(AreaClass);

	FString Folder = Params->GetStringField(TEXT("folder_path"));
	ModVol->SetFolderPath(FName(Folder.IsEmpty() ? TEXT("AI/Navigation") : *Folder));
	ModVol->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), ModVol->GetActorNameOrLabel());
	Result->SetStringField(TEXT("area_class"), AreaClass->GetName());
	Result->SetArrayField(TEXT("location"), VectorToJsonArray(Location));
	Result->SetArrayField(TEXT("extent"), VectorToJsonArray(Extent));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  154. add_nav_link_proxy
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleAddNavLinkProxy(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	bool bStartFound = false, bEndFound = false;
	FVector StartLoc = ParseVector(Params, TEXT("start_location"), bStartFound);
	FVector EndLoc = ParseVector(Params, TEXT("end_location"), bEndFound);

	if (!bStartFound) return FMonolithActionResult::Error(TEXT("Missing required parameter: start_location"));
	if (!bEndFound) return FMonolithActionResult::Error(TEXT("Missing required parameter: end_location"));

	FString LinkType = Params->GetStringField(TEXT("link_type"));
	bool bSmartLink = LinkType.Equals(TEXT("smart"), ESearchCase::IgnoreCase);

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add NavLinkProxy")));

	FVector MidPoint = (StartLoc + EndLoc) * 0.5;
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ANavLinkProxy* LinkActor = World->SpawnActor<ANavLinkProxy>(MidPoint, FRotator::ZeroRotator, SpawnParams);
	if (!LinkActor)
	{
		return FMonolithActionResult::Error(TEXT("Failed to spawn NavLinkProxy"));
	}

	// Set point links — Left/Right are relative to actor location
	FVector LeftPoint = StartLoc - MidPoint;
	FVector RightPoint = EndLoc - MidPoint;

	if (LinkActor->PointLinks.Num() > 0)
	{
		LinkActor->PointLinks[0].Left = LeftPoint;
		LinkActor->PointLinks[0].Right = RightPoint;
	}
	else
	{
		FNavigationLink NewLink;
		NewLink.Left = LeftPoint;
		NewLink.Right = RightPoint;
		LinkActor->PointLinks.Add(NewLink);
	}

	if (bSmartLink)
	{
		LinkActor->SetSmartLinkEnabled(true);
	}

	// Area class
	FString AreaClassName = Params->GetStringField(TEXT("area_class"));
	if (!AreaClassName.IsEmpty())
	{
		UClass* AreaClass = FindFirstObject<UClass>(*AreaClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
		if (!AreaClass)
		{
			AreaClass = FindFirstObject<UClass>(*(TEXT("NavArea_") + AreaClassName), EFindFirstObjectOptions::EnsureIfAmbiguous);
		}
		if (AreaClass && AreaClass->IsChildOf(UNavArea::StaticClass()))
		{
			if (LinkActor->PointLinks.Num() > 0)
			{
				LinkActor->PointLinks[0].SetAreaClass(AreaClass);
			}
		}
	}

	FString Folder = Params->GetStringField(TEXT("folder_path"));
	LinkActor->SetFolderPath(FName(Folder.IsEmpty() ? TEXT("AI/Navigation") : *Folder));
	LinkActor->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), LinkActor->GetActorNameOrLabel());
	Result->SetArrayField(TEXT("start"), VectorToJsonArray(StartLoc));
	Result->SetArrayField(TEXT("end"), VectorToJsonArray(EndLoc));
	Result->SetStringField(TEXT("link_type"), bSmartLink ? TEXT("smart") : TEXT("point"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  155. configure_nav_link
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleConfigureNavLink(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorPath = Params->GetStringField(TEXT("actor_path"));
	if (ActorPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: actor_path"));
	}

	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	// Find the actor
	ANavLinkProxy* LinkActor = nullptr;
	for (TActorIterator<ANavLinkProxy> It(World); It; ++It)
	{
		if (It->GetActorLabel() == ActorPath || It->GetName() == ActorPath || It->GetPathName() == ActorPath)
		{
			LinkActor = *It;
			break;
		}
	}

	if (!LinkActor)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("NavLinkProxy not found: %s"), *ActorPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Configure NavLink")));
	LinkActor->Modify();

	int32 ChangedCount = 0;

	if (Params->HasField(TEXT("enabled")))
	{
		bool bEnabled = Params->GetBoolField(TEXT("enabled"));

		// Phase F #36: SetSmartLinkEnabled toggles the runtime activity flag on
		// INavLinkCustomInterface, but the link only registers as a smart link with
		// the navmesh when bSmartLinkIsRelevant=true. Toggle BOTH so 'enabled' is
		// a real on/off (not just a runtime no-op when bSmartLinkIsRelevant=false).
		LinkActor->SetSmartLinkEnabled(bEnabled);

		if (FBoolProperty* RelevantProp = CastField<FBoolProperty>(
				LinkActor->GetClass()->FindPropertyByName(TEXT("bSmartLinkIsRelevant"))))
		{
			RelevantProp->SetPropertyValue_InContainer(LinkActor, bEnabled);
		}

		ChangedCount++;
	}

	if (Params->HasField(TEXT("area_class")))
	{
		FString AreaClassName = Params->GetStringField(TEXT("area_class"));
		UClass* AreaClass = FindFirstObject<UClass>(*AreaClassName, EFindFirstObjectOptions::EnsureIfAmbiguous);
		if (!AreaClass)
		{
			AreaClass = FindFirstObject<UClass>(*(TEXT("NavArea_") + AreaClassName), EFindFirstObjectOptions::EnsureIfAmbiguous);
		}
		if (AreaClass && AreaClass->IsChildOf(UNavArea::StaticClass()))
		{
			for (FNavigationLink& Link : LinkActor->PointLinks)
			{
				Link.SetAreaClass(AreaClass);
			}
			ChangedCount++;
		}
	}

	if (Params->HasField(TEXT("direction")))
	{
		FString Dir = Params->GetStringField(TEXT("direction"));
		ENavLinkDirection::Type DirType = ENavLinkDirection::BothWays;
		if (Dir.Equals(TEXT("left_to_right"), ESearchCase::IgnoreCase))
			DirType = ENavLinkDirection::LeftToRight;
		else if (Dir.Equals(TEXT("right_to_left"), ESearchCase::IgnoreCase))
			DirType = ENavLinkDirection::RightToLeft;

		for (FNavigationLink& Link : LinkActor->PointLinks)
		{
			Link.Direction = DirType;
		}
		ChangedCount++;
	}

	LinkActor->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), LinkActor->GetActorNameOrLabel());
	Result->SetNumberField(TEXT("properties_changed"), ChangedCount);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  156. list_nav_links
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleListNavLinks(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	FString LevelFilter = Params->GetStringField(TEXT("level"));

	TArray<TSharedPtr<FJsonValue>> Links;
	for (TActorIterator<ANavLinkProxy> It(World); It; ++It)
	{
		ANavLinkProxy* Link = *It;

		if (!LevelFilter.IsEmpty())
		{
			FString LevelName = Link->GetLevel()->GetOuter()->GetName();
			if (!LevelName.Contains(LevelFilter))
				continue;
		}

		auto LinkObj = MakeShared<FJsonObject>();
		LinkObj->SetStringField(TEXT("name"), Link->GetActorNameOrLabel());
		LinkObj->SetArrayField(TEXT("location"), VectorToJsonArray(Link->GetActorLocation()));
		LinkObj->SetStringField(TEXT("level"), Link->GetLevel()->GetOuter()->GetName());
		LinkObj->SetBoolField(TEXT("smart_link_enabled"), Link->IsSmartLinkEnabled());

		// Point links info
		TArray<TSharedPtr<FJsonValue>> PointLinksArr;
		for (const FNavigationLink& PL : Link->PointLinks)
		{
			auto PLObj = MakeShared<FJsonObject>();
			PLObj->SetArrayField(TEXT("left"), VectorToJsonArray(PL.Left));
			PLObj->SetArrayField(TEXT("right"), VectorToJsonArray(PL.Right));

			FString DirStr = TEXT("both");
			if (PL.Direction == ENavLinkDirection::LeftToRight) DirStr = TEXT("left_to_right");
			else if (PL.Direction == ENavLinkDirection::RightToLeft) DirStr = TEXT("right_to_left");
			PLObj->SetStringField(TEXT("direction"), DirStr);

			PointLinksArr.Add(MakeShared<FJsonValueObject>(PLObj));
		}
		LinkObj->SetArrayField(TEXT("point_links"), PointLinksArr);

		Links.Add(MakeShared<FJsonValueObject>(LinkObj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Links.Num());
	Result->SetArrayField(TEXT("links"), Links);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  157. find_path
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleFindPath(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return FMonolithActionResult::Error(TEXT("NavigationSystemV1 not found"));
	}

	bool bStartFound = false, bEndFound = false;
	FVector Start = ParseVector(Params, TEXT("start"), bStartFound);
	FVector End = ParseVector(Params, TEXT("end"), bEndFound);

	if (!bStartFound) return FMonolithActionResult::Error(TEXT("Missing required parameter: start"));
	if (!bEndFound) return FMonolithActionResult::Error(TEXT("Missing required parameter: end"));

	ANavigationData* NavData = NavSys->GetDefaultNavDataInstance();
	if (!NavData)
	{
		return FMonolithActionResult::Error(TEXT("No navigation data available. Build navigation first."));
	}

	FPathFindingQuery Query(nullptr, *NavData, Start, End);
	FPathFindingResult PathResult = NavSys->FindPathSync(Query);

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), PathResult.IsSuccessful());
	Result->SetBoolField(TEXT("is_partial"), PathResult.IsPartial());

	if (PathResult.IsSuccessful() && PathResult.Path.IsValid())
	{
		const TArray<FNavPathPoint>& PathPoints = PathResult.Path->GetPathPoints();

		TArray<TSharedPtr<FJsonValue>> Points;
		double TotalDistance = 0.0;

		for (int32 i = 0; i < PathPoints.Num(); ++i)
		{
			Points.Add(MakeShared<FJsonValueArray>(VectorToJsonArray(PathPoints[i].Location)));

			if (i > 0)
			{
				TotalDistance += FVector::Dist(PathPoints[i - 1].Location, PathPoints[i].Location);
			}
		}

		Result->SetArrayField(TEXT("path_points"), Points);
		Result->SetNumberField(TEXT("point_count"), PathPoints.Num());
		Result->SetNumberField(TEXT("total_distance"), TotalDistance);
		Result->SetNumberField(TEXT("path_cost"), PathResult.Path->GetCost());
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  158. test_path
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleTestPath(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return FMonolithActionResult::Error(TEXT("NavigationSystemV1 not found"));
	}

	bool bStartFound = false, bEndFound = false;
	FVector Start = ParseVector(Params, TEXT("start"), bStartFound);
	FVector End = ParseVector(Params, TEXT("end"), bEndFound);

	if (!bStartFound) return FMonolithActionResult::Error(TEXT("Missing required parameter: start"));
	if (!bEndFound) return FMonolithActionResult::Error(TEXT("Missing required parameter: end"));

	ANavigationData* NavData = NavSys->GetDefaultNavDataInstance();
	if (!NavData)
	{
		return FMonolithActionResult::Error(TEXT("No navigation data available. Build navigation first."));
	}

	FPathFindingQuery Query(nullptr, *NavData, Start, End);
	bool bPathExists = NavSys->TestPathSync(Query);

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("reachable"), bPathExists);
	Result->SetStringField(TEXT("result"), bPathExists ? TEXT("Success") : TEXT("Fail"));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  159. project_point_to_navigation
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleProjectPointToNavigation(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return FMonolithActionResult::Error(TEXT("NavigationSystemV1 not found"));
	}

	bool bPointFound = false;
	FVector Point = ParseVector(Params, TEXT("point"), bPointFound);
	if (!bPointFound)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: point"));
	}

	bool bExtentFound = false;
	FVector Extent = ParseVector(Params, TEXT("extent"), bExtentFound);
	if (!bExtentFound)
	{
		Extent = FVector(50.0, 50.0, 250.0);
	}

	FNavLocation NavLoc;
	bool bProjected = NavSys->ProjectPointToNavigation(Point, NavLoc, Extent);

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bProjected);
	if (bProjected)
	{
		Result->SetArrayField(TEXT("projected_point"), VectorToJsonArray(NavLoc.Location));
		Result->SetNumberField(TEXT("distance_from_original"), FVector::Dist(Point, NavLoc.Location));
	}
	else
	{
		Result->SetStringField(TEXT("message"), TEXT("Point could not be projected to navigation — no navmesh within extent"));
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  160. get_random_navigable_point
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleGetRandomNavigablePoint(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return FMonolithActionResult::Error(TEXT("NavigationSystemV1 not found"));
	}

	FNavLocation NavLoc;
	bool bFound = false;

	bool bOriginFound = false;
	FVector Origin = ParseVector(Params, TEXT("origin"), bOriginFound);

	if (bOriginFound)
	{
		double Radius = Params->HasField(TEXT("radius")) ? Params->GetNumberField(TEXT("radius")) : 1000.0;
		bFound = NavSys->GetRandomReachablePointInRadius(Origin, Radius, NavLoc);
	}
	else
	{
		bFound = NavSys->GetRandomPoint(NavLoc);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bFound);
	if (bFound)
	{
		Result->SetArrayField(TEXT("point"), VectorToJsonArray(NavLoc.Location));
	}
	else
	{
		Result->SetStringField(TEXT("message"), TEXT("Could not find a navigable point — ensure navmesh is built"));
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  161. navigation_raycast
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleNavigationRaycast(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	bool bStartFound = false, bEndFound = false;
	FVector Start = ParseVector(Params, TEXT("start"), bStartFound);
	FVector End = ParseVector(Params, TEXT("end"), bEndFound);

	if (!bStartFound) return FMonolithActionResult::Error(TEXT("Missing required parameter: start"));
	if (!bEndFound) return FMonolithActionResult::Error(TEXT("Missing required parameter: end"));

	FVector HitLocation;
	bool bHit = UNavigationSystemV1::NavigationRaycast(World, Start, End, HitLocation);

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("hit"), bHit);
	Result->SetBoolField(TEXT("line_of_sight"), !bHit);
	if (bHit)
	{
		Result->SetArrayField(TEXT("hit_location"), VectorToJsonArray(HitLocation));
		Result->SetNumberField(TEXT("hit_distance"), FVector::Dist(Start, HitLocation));
	}
	Result->SetNumberField(TEXT("total_distance"), FVector::Dist(Start, End));

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  162. configure_nav_agent
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleConfigureNavAgent(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return FMonolithActionResult::Error(TEXT("NavigationSystemV1 not found"));
	}

	int32 AgentIndex = (int32)Params->GetNumberField(TEXT("agent_index"));

	const int32 AgentCount = NavSys->GetSupportedAgents().Num();
	if (AgentIndex < 0 || AgentIndex >= AgentCount)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Agent index %d out of range. Supported agents: 0-%d"),
			AgentIndex, AgentCount - 1));
	}

	// Nav system supported agents are configured in project settings (NavigationSystem section).
	// We need to modify the mutable config. Access via reflection since GetSupportedAgentConfig returns const.
	FArrayProperty* AgentsProp = CastField<FArrayProperty>(
		NavSys->GetClass()->FindPropertyByName(TEXT("SupportedAgents")));

	if (!AgentsProp)
	{
		return FMonolithActionResult::Error(TEXT("Could not find SupportedAgents property on NavigationSystem"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Configure Nav Agent")));
	NavSys->Modify();

	FScriptArrayHelper ArrayHelper(AgentsProp, AgentsProp->ContainerPtrToValuePtr<void>(NavSys));

	if (AgentIndex >= ArrayHelper.Num())
	{
		return FMonolithActionResult::Error(TEXT("Agent index out of bounds"));
	}

	void* ElementPtr = ArrayHelper.GetRawPtr(AgentIndex);
	UScriptStruct* StructType = CastField<FStructProperty>(AgentsProp->Inner)->Struct;
	int32 ChangedCount = 0;

	auto SetFloatProp = [&](const TCHAR* JsonField, const TCHAR* PropName)
	{
		if (Params->HasField(JsonField))
		{
			if (FFloatProperty* Prop = CastField<FFloatProperty>(StructType->FindPropertyByName(PropName)))
			{
				Prop->SetPropertyValue(Prop->ContainerPtrToValuePtr<void>(ElementPtr), (float)Params->GetNumberField(JsonField));
				ChangedCount++;
			}
		}
	};

	SetFloatProp(TEXT("radius"), TEXT("AgentRadius"));
	SetFloatProp(TEXT("height"), TEXT("AgentHeight"));
	SetFloatProp(TEXT("step_height"), TEXT("AgentStepHeight"));

	NavSys->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("agent_index"), AgentIndex);
	Result->SetNumberField(TEXT("properties_changed"), ChangedCount);
	Result->SetStringField(TEXT("message"), TEXT("Agent config updated. Rebuild navigation for changes to take effect."));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  163. add_nav_invoker_component
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleAddNavInvokerComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString BlueprintPath = Params->GetStringField(TEXT("blueprint_path"));
	if (BlueprintPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: blueprint_path"));
	}

	FString Error;
	UObject* Obj = MonolithAI::LoadAssetFromPath(BlueprintPath, Error);
	if (!Obj)
	{
		return FMonolithActionResult::Error(Error);
	}

	UBlueprint* BP = Cast<UBlueprint>(Obj);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset is not a Blueprint: %s"), *BlueprintPath));
	}

	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	// Check if already has a NavigationInvokerComponent
	static UClass* InvokerClass = FindFirstObject<UClass>(
		TEXT("UNavigationInvokerComponent"), EFindFirstObjectOptions::NativeFirst);
	if (!InvokerClass)
	{
		InvokerClass = StaticLoadClass(UActorComponent::StaticClass(), nullptr,
			TEXT("/Script/NavigationSystem.NavigationInvokerComponent"));
	}
	if (!InvokerClass)
	{
		return FMonolithActionResult::Error(TEXT("UNavigationInvokerComponent class not found. Ensure NavigationSystem module is loaded."));
	}

	for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
	{
		if (Node && Node->ComponentClass && Node->ComponentClass->IsChildOf(InvokerClass))
		{
			return FMonolithActionResult::Error(TEXT("Blueprint already has a NavigationInvokerComponent"));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Add NavInvokerComponent")));

	USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(InvokerClass, TEXT("NavInvoker"));
	if (!NewNode)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create SCS node"));
	}

	// Set generation/removal radius via reflection on the template
	UActorComponent* Template = NewNode->ComponentTemplate;
	if (Template)
	{
		if (Params->HasField(TEXT("generation_radius")))
		{
			if (FFloatProperty* Prop = CastField<FFloatProperty>(InvokerClass->FindPropertyByName(TEXT("GenerationRadius"))))
			{
				Prop->SetPropertyValue_InContainer(Template, (float)Params->GetNumberField(TEXT("generation_radius")));
			}
		}
		if (Params->HasField(TEXT("removal_radius")))
		{
			if (FFloatProperty* Prop = CastField<FFloatProperty>(InvokerClass->FindPropertyByName(TEXT("RemovalRadius"))))
			{
				Prop->SetPropertyValue_InContainer(Template, (float)Params->GetNumberField(TEXT("removal_radius")));
			}
		}
	}

	BP->SimpleConstructionScript->AddNode(NewNode);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), BlueprintPath);
	Result->SetStringField(TEXT("component_name"), TEXT("NavInvoker"));
	Result->SetStringField(TEXT("message"), TEXT("NavigationInvokerComponent added. Compile the Blueprint for changes to take effect."));
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  164. get_crowd_manager_config
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleGetCrowdManagerConfig(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	UCrowdManager* CrowdMgr = UCrowdManager::GetCurrent(World);
	auto Result = MakeShared<FJsonObject>();

	if (!CrowdMgr)
	{
		Result->SetBoolField(TEXT("active"), false);
		Result->SetStringField(TEXT("message"), TEXT("No CrowdManager active. It initializes when AI agents use crowd following."));
		return FMonolithActionResult::Success(Result);
	}

	Result->SetBoolField(TEXT("active"), true);

	// Read properties via reflection
	UClass* CrowdClass = CrowdMgr->GetClass();

	auto ReadInt = [&](const TCHAR* PropName)
	{
		if (FIntProperty* Prop = CastField<FIntProperty>(CrowdClass->FindPropertyByName(PropName)))
		{
			Result->SetNumberField(PropName, Prop->GetPropertyValue_InContainer(CrowdMgr));
		}
	};
	// Mirrors HandleGetNavSystemConfig pattern (FFloatProperty with FDoubleProperty fallback)
	auto ReadFloat = [&](const TCHAR* PropName)
	{
		if (FFloatProperty* Prop = CastField<FFloatProperty>(CrowdClass->FindPropertyByName(PropName)))
		{
			Result->SetNumberField(PropName, Prop->GetPropertyValue_InContainer(CrowdMgr));
		}
		else if (FDoubleProperty* DProp = CastField<FDoubleProperty>(CrowdClass->FindPropertyByName(PropName)))
		{
			Result->SetNumberField(PropName, DProp->GetPropertyValue_InContainer(CrowdMgr));
		}
	};
	auto ReadBool = [&](const TCHAR* PropName)
	{
		if (FBoolProperty* Prop = CastField<FBoolProperty>(CrowdClass->FindPropertyByName(PropName)))
		{
			Result->SetBoolField(PropName, Prop->GetPropertyValue_InContainer(CrowdMgr));
		}
	};

	ReadInt(TEXT("MaxAgents"));
	ReadFloat(TEXT("MaxAgentRadius"));        // bug fix: was ReadInt — silently dropped (UE 5.7 declares as float)
	ReadInt(TEXT("MaxAvoidedAgents"));
	ReadInt(TEXT("MaxAvoidedWalls"));
	ReadFloat(TEXT("NavmeshCheckInterval"));
	ReadFloat(TEXT("PathOptimizationInterval"));
	ReadFloat(TEXT("SeparationDirClamp"));
	ReadFloat(TEXT("PathOffsetRadiusMultiplier"));
	ReadBool(TEXT("bResolveCollisions"));     // uint32:1 bitfield exposed as FBoolProperty by UHT

	// Avoidance config array
	FArrayProperty* AvoidProp = CastField<FArrayProperty>(CrowdClass->FindPropertyByName(TEXT("AvoidanceConfig")));
	if (AvoidProp)
	{
		FScriptArrayHelper ArrayHelper(AvoidProp, AvoidProp->ContainerPtrToValuePtr<void>(CrowdMgr));
		Result->SetNumberField(TEXT("avoidance_config_count"), ArrayHelper.Num());
	}

	// Sampling patterns array (size only — per-element schema deferred)
	FArrayProperty* SamplingProp = CastField<FArrayProperty>(CrowdClass->FindPropertyByName(TEXT("SamplingPatterns")));
	if (SamplingProp)
	{
		FScriptArrayHelper ArrayHelper(SamplingProp, SamplingProp->ContainerPtrToValuePtr<void>(CrowdMgr));
		Result->SetNumberField(TEXT("sampling_patterns_count"), ArrayHelper.Num());
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  165. set_crowd_manager_config
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleSetCrowdManagerConfig(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	UCrowdManager* CrowdMgr = UCrowdManager::GetCurrent(World);
	if (!CrowdMgr)
	{
		return FMonolithActionResult::Error(TEXT("No CrowdManager active. Start a PIE session with crowd-following AI agents first."));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Monolith: Set CrowdManager Config")));
	CrowdMgr->Modify();

	UClass* CrowdClass = CrowdMgr->GetClass();
	int32 ChangedCount = 0;

	TSharedPtr<FJsonObject> Verified = MakeShared<FJsonObject>();

	auto SetIntPropVerified = [&](const TCHAR* JsonField, const TCHAR* PropName)
	{
		if (!Params->HasField(JsonField))
		{
			return;
		}
		const int32 Requested = (int32)Params->GetNumberField(JsonField);
		FIntProperty* Prop = CastField<FIntProperty>(CrowdClass->FindPropertyByName(PropName));
		if (!Prop)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("requested"), Requested);
			Entry->SetNumberField(TEXT("actual"), 0);
			Entry->SetBoolField(TEXT("match"), false);
			Verified->SetObjectField(JsonField, Entry);
			return;
		}
		Prop->SetPropertyValue_InContainer(CrowdMgr, Requested);
		const int32 Actual = Prop->GetPropertyValue_InContainer(CrowdMgr);
		ChangedCount++;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("requested"), Requested);
		Entry->SetNumberField(TEXT("actual"), Actual);
		Entry->SetBoolField(TEXT("match"), Requested == Actual);
		Verified->SetObjectField(JsonField, Entry);
	};

	// Float companion (mirrors get-side ReadFloat: tries FFloatProperty then FDoubleProperty)
	auto SetFloatPropVerified = [&](const TCHAR* JsonField, const TCHAR* PropName)
	{
		if (!Params->HasField(JsonField))
		{
			return;
		}
		const double Requested = Params->GetNumberField(JsonField);
		double Actual = 0.0;
		bool bWrote = false;
		if (FFloatProperty* FProp = CastField<FFloatProperty>(CrowdClass->FindPropertyByName(PropName)))
		{
			FProp->SetPropertyValue_InContainer(CrowdMgr, (float)Requested);
			Actual = FProp->GetPropertyValue_InContainer(CrowdMgr);
			bWrote = true;
		}
		else if (FDoubleProperty* DProp = CastField<FDoubleProperty>(CrowdClass->FindPropertyByName(PropName)))
		{
			DProp->SetPropertyValue_InContainer(CrowdMgr, Requested);
			Actual = DProp->GetPropertyValue_InContainer(CrowdMgr);
			bWrote = true;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("requested"), Requested);
		Entry->SetNumberField(TEXT("actual"), Actual);
		// Cast to float for compare to absorb double-rounded float storage; tolerance handles UE float quantization
		Entry->SetBoolField(TEXT("match"), bWrote && FMath::IsNearlyEqual((float)Requested, (float)Actual, 1e-3f));
		Verified->SetObjectField(JsonField, Entry);
		if (bWrote) { ChangedCount++; }
	};

	auto SetBoolPropVerified = [&](const TCHAR* JsonField, const TCHAR* PropName)
	{
		if (!Params->HasField(JsonField))
		{
			return;
		}
		const bool Requested = Params->GetBoolField(JsonField);
		FBoolProperty* Prop = CastField<FBoolProperty>(CrowdClass->FindPropertyByName(PropName));
		if (!Prop)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetBoolField(TEXT("requested"), Requested);
			Entry->SetBoolField(TEXT("actual"), false);
			Entry->SetBoolField(TEXT("match"), false);
			Verified->SetObjectField(JsonField, Entry);
			return;
		}
		Prop->SetPropertyValue_InContainer(CrowdMgr, Requested);
		const bool Actual = Prop->GetPropertyValue_InContainer(CrowdMgr);
		ChangedCount++;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetBoolField(TEXT("requested"), Requested);
		Entry->SetBoolField(TEXT("actual"), Actual);
		Entry->SetBoolField(TEXT("match"), Requested == Actual);
		Verified->SetObjectField(JsonField, Entry);
	};

	SetIntPropVerified(TEXT("max_agents"), TEXT("MaxAgents"));
	SetIntPropVerified(TEXT("max_avoidance_agents"), TEXT("MaxAvoidedAgents"));
	SetIntPropVerified(TEXT("max_avoided_walls"), TEXT("MaxAvoidedWalls"));

	SetFloatPropVerified(TEXT("max_agent_radius"), TEXT("MaxAgentRadius"));
	SetFloatPropVerified(TEXT("navmesh_check_interval"), TEXT("NavmeshCheckInterval"));
	SetFloatPropVerified(TEXT("path_optimization_interval"), TEXT("PathOptimizationInterval"));
	SetFloatPropVerified(TEXT("separation_dir_clamp"), TEXT("SeparationDirClamp"));
	SetFloatPropVerified(TEXT("path_offset_radius_multiplier"), TEXT("PathOffsetRadiusMultiplier"));

	SetBoolPropVerified(TEXT("resolve_collisions"), TEXT("bResolveCollisions"));

	CrowdMgr->MarkPackageDirty();

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("properties_changed"), ChangedCount);
	Result->SetStringField(TEXT("message"), TEXT("CrowdManager config updated"));
	if (Verified->Values.Num() > 0)
	{
		Result->SetObjectField(TEXT("verified_value"), Verified);
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  166. analyze_navigation_coverage
// ============================================================

FMonolithActionResult FMonolithAINavigationActions::HandleAnalyzeNavigationCoverage(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetNavWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No world available"));
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return FMonolithActionResult::Error(TEXT("NavigationSystemV1 not found"));
	}

	double SampleSpacing = Params->HasField(TEXT("sample_spacing")) ? Params->GetNumberField(TEXT("sample_spacing")) : 200.0;

	// Determine bounds
	FBox SampleBounds(ForceInit);

	const TSharedPtr<FJsonObject>* BoundsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("bounds"), BoundsObj) && BoundsObj && (*BoundsObj)->Values.Num() > 0)
	{
		const TArray<TSharedPtr<FJsonValue>>* MinArr = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* MaxArr = nullptr;
		if ((*BoundsObj)->TryGetArrayField(TEXT("min"), MinArr) && MinArr && MinArr->Num() >= 3 &&
			(*BoundsObj)->TryGetArrayField(TEXT("max"), MaxArr) && MaxArr && MaxArr->Num() >= 3)
		{
			SampleBounds.Min = FVector((*MinArr)[0]->AsNumber(), (*MinArr)[1]->AsNumber(), (*MinArr)[2]->AsNumber());
			SampleBounds.Max = FVector((*MaxArr)[0]->AsNumber(), (*MaxArr)[1]->AsNumber(), (*MaxArr)[2]->AsNumber());
		}
	}

	// If no custom bounds, use nav bounds volumes
	if (!SampleBounds.IsValid)
	{
		for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
		{
			FVector Origin, Ext;
			(*It)->GetActorBounds(false, Origin, Ext);
			FBox VolBounds(Origin - Ext, Origin + Ext);
			if (SampleBounds.IsValid)
			{
				SampleBounds = SampleBounds + VolBounds;
			}
			else
			{
				SampleBounds = VolBounds;
			}
		}
	}

	if (!SampleBounds.IsValid)
	{
		return FMonolithActionResult::Error(TEXT("No valid bounds found. Add NavMeshBoundsVolumes or provide custom bounds."));
	}

	// Grid sample
	int32 TotalSamples = 0;
	int32 NavigableSamples = 0;
	int32 NonNavigableSamples = 0;

	// Cap sample count to avoid performance issues
	FVector BoundsSize = SampleBounds.GetSize();
	int32 EstimatedSamples = (int32)((BoundsSize.X / SampleSpacing) * (BoundsSize.Y / SampleSpacing));
	if (EstimatedSamples > 100000)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Estimated sample count (%d) too high. Increase sample_spacing or reduce bounds. Current spacing: %.0f"),
			EstimatedSamples, SampleSpacing));
	}

	// Sample at the mid-Z of the bounds (we project down to navmesh)
	double SampleZ = SampleBounds.GetCenter().Z;

	TArray<TSharedPtr<FJsonValue>> GapRegions;
	int32 ConsecutiveGapCount = 0;
	FVector GapStart = FVector::ZeroVector;

	for (double X = SampleBounds.Min.X; X <= SampleBounds.Max.X; X += SampleSpacing)
	{
		for (double Y = SampleBounds.Min.Y; Y <= SampleBounds.Max.Y; Y += SampleSpacing)
		{
			FVector SamplePoint(X, Y, SampleZ);
			FNavLocation NavLoc;
			bool bOnNav = NavSys->ProjectPointToNavigation(SamplePoint, NavLoc, FVector(SampleSpacing * 0.5, SampleSpacing * 0.5, 500.0));

			TotalSamples++;
			if (bOnNav)
			{
				NavigableSamples++;
			}
			else
			{
				NonNavigableSamples++;
			}
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("total_samples"), TotalSamples);
	Result->SetNumberField(TEXT("navigable_samples"), NavigableSamples);
	Result->SetNumberField(TEXT("non_navigable_samples"), NonNavigableSamples);

	double CoveragePct = TotalSamples > 0 ? (double)NavigableSamples / (double)TotalSamples * 100.0 : 0.0;
	Result->SetNumberField(TEXT("coverage_percent"), FMath::RoundToDouble(CoveragePct * 100.0) / 100.0);
	Result->SetNumberField(TEXT("sample_spacing"), SampleSpacing);

	Result->SetArrayField(TEXT("bounds_min"), VectorToJsonArray(SampleBounds.Min));
	Result->SetArrayField(TEXT("bounds_max"), VectorToJsonArray(SampleBounds.Max));

	FString Quality;
	if (CoveragePct >= 90.0) Quality = TEXT("excellent");
	else if (CoveragePct >= 70.0) Quality = TEXT("good");
	else if (CoveragePct >= 50.0) Quality = TEXT("fair");
	else Quality = TEXT("poor");
	Result->SetStringField(TEXT("quality"), Quality);

	return FMonolithActionResult::Success(Result);
}
